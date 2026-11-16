// Port-launcher additions for BattleShip (Super Smash Bros.; docs/planning/MENU.md): first-start extraction and
// save-state regions. The port's own settings are applied generically by the libultraship shim (lus_shim.cpp); our
// hand-made option keys were removed on 2026-11-15.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <android/log.h>
#include <unistd.h>
#include <libultraship/bridge.h>

#include "ship/Context.h"
#include "ship/window/Window.h"

extern "C" {
int torch_extract_o2r(const char* rom_path, const char* src_dir, const char* dst_dir);  // port/android_torch_bridge.cpp
extern void* gPortSceneHeap;               // decomp/src/sys/taskman.c (the scene arena, malloc'd once)
extern const size_t gPortSceneHeapSize;
void plm_savestate_add_region(void* start, size_t size);
bool plm_savestate_save(int slot);
bool plm_savestate_load(int slot);
}

namespace {

constexpr const char* log_tag = "plm-battleship";

bool arena_registered = false;

// First start: BattleShip.o2r from the launcher's ROM. Upstream's Android app does this from its own Java first-run
// screen through libtorch_runner.so; in-game, its extractor would look for a desktop Torch executable. We call the
// same torch_extract_o2r with the bundle (config.yml, yamls/) as Torch's config dir and the config dir as output.
void extract_if_needed() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists("BattleShip.o2r", ec)) return;  // relative to the config dir (the shim chdir's there)
    const char* rom = std::getenv("PLM_ROM");
    const char* bundle = std::getenv("PLM_BUNDLE");
    if (rom == nullptr || bundle == nullptr) return;
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return;
    __android_log_print(ANDROID_LOG_INFO, log_tag, "extracting BattleShip.o2r from %s", rom);
    const int rc = torch_extract_o2r(rom, bundle, cwd);
    __android_log_print(rc == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, log_tag, "extraction %s (%d)",
                        rc == 0 ? "done" : "FAILED", rc);
}

} // namespace

// Called by the libultraship shim before the game starts (its launch settings are unused now).
extern "C" __attribute__((visibility("default"))) void plm_module_settings(const char*) {
    extract_if_needed();  // before the game starts, which otherwise opens its first-run wizard
}

// Save states: the game loop runs as a coroutine on its own mmap'd stack (port/coroutine_android.cpp), and so does
// every N64 thread (osStartThread) and every GObj "thread" process (objman.c: fighters, effects, menus). At the end of
// a frame all of them are parked, each with its registers in PortCoroutine::ctx and its frames on its stack; saving
// both with the game's memory and restoring them together resumes the game where the state was taken. The layout
// below mirrors port/coroutine_android.cpp for arm64 (CoroCtx is 176 bytes there, static_assert'ed); keep it in sync.
struct alignas(16) PlmCoroCtx {
    uint64_t regs[14];  // x19..x28 = 0..9, x29 = 10, x30 = 11, sp = 12, pad = 13
    double fp[8];       // d8..d15
};
static_assert(sizeof(PlmCoroCtx) == 176, "must match CoroCtx in port/coroutine_android.cpp");
struct PlmPortCoroutine {
    PlmCoroCtx ctx;
    PlmCoroCtx caller_ctx;
    void (*entry)(void*);
    void* arg;
    int finished;
    void* stack_mem;     // mmap base; the lowest page is a PROT_NONE guard
    size_t stack_total;  // including the guard page
};

extern "C" PlmPortCoroutine* __real_port_coroutine_create(void (*entry)(void*), void* arg, size_t stack_size);
extern "C" void __real_port_coroutine_destroy(PlmPortCoroutine* co);
extern "C" void port_coroutine_trampoline_aarch64(void);  // port/coroutine_aarch64.S
extern "C" void __real_port_taskman_evict_arena_caches(const void* base, size_t size);

namespace {

// Coroutine POOL (why, RP6 2026-11-13 "crashed on save state"): the first version registered every coroutine's stack
// as a save region when it was created, but GObj thread processes are created and destroyed all the time
// (gcEndGObjProcess -> osDestroyThread -> port_coroutine_destroy munmaps the stack and frees the struct). The region
// list kept the dead ranges, so the next save memcpy'd from unmapped memory: SIGSEGV on save. Even without that, a
// load can't work if the coroutine a restored OSThread points at was freed or reused since.
//
// So coroutines are never really destroyed: destroy only marks the pool entry free, and create reuses a free entry
// with the same stack size before mapping a new one. Pool entries (struct + stack) are therefore stable memory,
// registered once as save regions, in creation order (plm_savestate restores the regions that existed at save time
// and leaves newer ones alone). Which entries are live (pool_used) is itself a save region: after a load, entries
// created after the save are free again (their flag was 0 in the state), and entries live at save time are live
// again with the exact registers and stack they had, matching the OSThread.port_coroutine pointers restored with the
// game section. Peak memory = peak number of simultaneous coroutines (service threads 256 KB, GObj threads 64 KB).
// All of this runs on the SDL thread (coroutines are created, resumed and destroyed only there): no locking.
constexpr size_t kPoolMax = 2048;
PlmPortCoroutine* pool_co[kPoolMax];  // NOT in the state: append-only list of every coroutine ever created
size_t pool_n = 0;
uint8_t pool_used[kPoolMax];  // IN the state (registered on the first create)
bool pool_overflowed = false;

size_t page_size() {
    const long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? static_cast<size_t>(p) : 4096;
}

// Same rounding as port_coroutine_create (MIN_STACK_SIZE 32 KB, whole pages, plus the guard page).
size_t mapping_size_for(size_t stack_size) {
    const size_t ps = page_size();
    if (stack_size < 32768) stack_size = 32768;
    return ((stack_size + ps - 1) & ~(ps - 1)) + ps;
}

// Puts a pooled coroutine back into the state port_coroutine_create leaves a fresh one in: on the first swap the asm
// loads sp = top of stack, x19 = the coroutine (the trampoline's argument), x30 = the trampoline, everything else 0.
void reinit(PlmPortCoroutine* co, void (*entry)(void*), void* arg) {
    std::memset(&co->ctx, 0, sizeof(co->ctx));
    std::memset(&co->caller_ctx, 0, sizeof(co->caller_ctx));
    co->entry = entry;
    co->arg = arg;
    co->finished = 0;
    co->ctx.regs[12] = reinterpret_cast<uint64_t>(static_cast<char*>(co->stack_mem) + co->stack_total);
    co->ctx.regs[0] = reinterpret_cast<uint64_t>(co);
    co->ctx.regs[11] = reinterpret_cast<uint64_t>(&port_coroutine_trampoline_aarch64);
}

// Scene epoch (why): BattleShip keeps a lot of PORT-side state about the scene arena's contents outside the game
// section: the 32-bit reloc token table (port/resource/RelocPointerTable.cpp, generational: a scene change
// invalidates every token into the arena), the struct-fixup registry, texture and display-list caches keyed by arena
// address, and the intern/extern file buffers' bookkeeping (port/bridge/lbreloc_bridge.cpp). A state restored after
// a scene change would bring back tokens the table now rejects and arena bytes the caches describe differently: a
// crash or garbage. Saving all of that is a bigger job (hardening pass); for now a load is only allowed in the scene
// it was saved in. syTaskmanStartTask calls port_taskman_evict_arena_caches exactly when it recycles the arena for a
// new scene, so wrapping it (at link time, module.cmake) counts scenes.
uint32_t scene_epoch = 0;
std::map<int, uint32_t> slot_epoch;
std::mutex state_requests_mutex;
std::vector<std::pair<bool, int>> state_requests;  // (load?, slot), queued from the UI thread

// Frame thread, between ticks (plm_module_frame): every coroutine is parked here.
void run_state_requests() {
    std::vector<std::pair<bool, int>> todo;
    {
        std::lock_guard lock(state_requests_mutex);
        todo.swap(state_requests);
    }
    for (const auto& [load, slot] : todo) {
        if (!load) {
            if (pool_overflowed) {
                __android_log_print(ANDROID_LOG_ERROR, log_tag, "save state refused: coroutine pool overflowed");
                continue;
            }
            if (plm_savestate_save(slot)) slot_epoch[slot] = scene_epoch;
            continue;
        }
        const auto it = slot_epoch.find(slot);
        if (it == slot_epoch.end()) continue;
        if (it->second != scene_epoch) {
            __android_log_print(ANDROID_LOG_WARN, log_tag,
                                "load state refused: slot %d was saved in another scene (port-side caches can't be "
                                "restored yet)", slot);
            continue;
        }
        plm_savestate_load(slot);
    }
}

} // namespace

extern "C" PlmPortCoroutine* __wrap_port_coroutine_create(void (*entry)(void*), void* arg, size_t stack_size) {
    if (pool_n == 0 && !pool_overflowed) plm_savestate_add_region(pool_used, sizeof(pool_used));
    const size_t want = mapping_size_for(stack_size);
    for (size_t i = 0; i < pool_n; i++) {
        if (!pool_used[i] && pool_co[i]->stack_total == want) {
            reinit(pool_co[i], entry, arg);
            pool_used[i] = 1;
            return pool_co[i];
        }
    }
    PlmPortCoroutine* co = __real_port_coroutine_create(entry, arg, stack_size);
    if (co == nullptr || co->stack_mem == nullptr) return co;
    if (pool_n == kPoolMax) {
        // Not in the state: a load would restore OSThreads pointing at coroutines whose stacks weren't saved.
        if (!pool_overflowed) __android_log_print(ANDROID_LOG_ERROR, log_tag, "coroutine pool full: save states off");
        pool_overflowed = true;
        return co;
    }
    pool_co[pool_n] = co;
    pool_used[pool_n] = 1;
    pool_n++;
    const size_t page = page_size();
    // The whole struct (registers, entry, finished flag; caller_ctx is rewritten by every resume anyway), and the
    // stack above the guard page.
    plm_savestate_add_region(co, sizeof(*co));
    plm_savestate_add_region(static_cast<char*>(co->stack_mem) + page, co->stack_total - page);
    __android_log_print(ANDROID_LOG_INFO, log_tag, "save states: coroutine %zu (%zu KB stack) registered", pool_n,
                        (co->stack_total - page) / 1024);
    return co;
}

extern "C" void __wrap_port_coroutine_destroy(PlmPortCoroutine* co) {
    for (size_t i = 0; i < pool_n; i++) {
        if (pool_co[i] == co) {
            pool_used[i] = 0;  // stays mapped, for reuse and for older states (see the pool comment)
            return;
        }
    }
    __real_port_coroutine_destroy(co);  // an overflow coroutine, never in a state
}

extern "C" void __wrap_port_taskman_evict_arena_caches(const void* base, size_t size) {
    scene_epoch++;
    __real_port_taskman_evict_arena_caches(base, size);
}

extern "C" __attribute__((visibility("default"))) void plm_save_state(const char* slot) {
    std::lock_guard lock(state_requests_mutex);
    state_requests.push_back({false, std::atoi(slot)});
}

extern "C" __attribute__((visibility("default"))) void plm_load_state(const char* slot) {
    std::lock_guard lock(state_requests_mutex);
    state_requests.push_back({true, std::atoi(slot)});
}

extern "C" void plm_module_frame(void) {
    // The scene arena exists from the first scene on and is recycled, never moved (taskman.c).
    if (!arena_registered && gPortSceneHeap != nullptr) {
        arena_registered = true;
        plm_savestate_add_region(gPortSceneHeap, gPortSceneHeapSize);
    }
    run_state_requests();
}

// The port's own settings (settings_schema.json, MENU.md design of 2026-11-14) are set generically by the libultraship
// shim (modules/common/lus/lus_shim.cpp, CVarSet* by the CVar's name). What a CVar change needs beyond the set, as the
// port's own menu callback does it, goes here; it runs on the frame thread right after each set.
extern "C" void plm_lus_cvar_changed(const char* cvar) {
    // The interpreter reads these two only at init; the window setters apply them live (the port menu's callbacks).
    const std::string name = cvar;
    auto ctx = Ship::Context::GetInstance();
    auto w = ctx != nullptr ? ctx->GetWindow() : nullptr;
    if (w != nullptr && name.size() >= 18 && name.compare(name.size() - 18, 18, "InternalResolution") == 0) {
        w->SetResolutionMultiplier(CVarGetFloat(cvar, 1.f));
    } else if (w != nullptr && name.size() >= 9 && name.compare(name.size() - 9, 9, "MSAAValue") == 0) {
        w->SetMsaaLevel(CVarGetInteger(cvar, 1));
    }
}
