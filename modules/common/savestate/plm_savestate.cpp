// Save states for decomp ports whose game state is the game's own static data (docs/planning/SAVE_STATES.md).
//
// module.cmake force-includes plm_game_section.h into the game's sources (not the port layer or libultraship), so the
// linker gathers every game global and static into plm_game_data / plm_game_bss. A port whose heap lives elsewhere
// registers it with plm_savestate_add_region(). A save copies those ranges; a load copies them back. Both must run on
// the game thread between ticks (the module calls them from there). States live in memory for the session: pointers
// inside them are only valid while the library stays loaded at the same address.

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include <android/log.h>

extern "C" {
extern char __start_plm_game_data[] __attribute__((weak));
extern char __stop_plm_game_data[] __attribute__((weak));
extern char __start_plm_game_bss[] __attribute__((weak));
extern char __stop_plm_game_bss[] __attribute__((weak));
// plm_arena.c, linked only by modules whose game heap goes through it (Lighthouse): logs its use on every save.
void plm_arena_log_usage(void) __attribute__((weak));
// ... and holds its lock around the copies, so no allocation is half done in a state.
void plm_arena_snapshot_begin(void) __attribute__((weak));
void plm_arena_snapshot_end(void) __attribute__((weak));
}

namespace {

struct Region {
    char* start;
    size_t size;  // capacity: the whole range, and what a fixed region always copies
    // Dynamic regions (plm_savestate_add_dynamic_region): how much of the range is in use right now, from its start.
    // Only that much is copied, so a huge mostly-empty pool doesn't cost its full size per state.
    size_t (*used)(void) = nullptr;
};

struct Slot {
    std::vector<char> data;     // concatenated copy of every region's saved part
    std::vector<size_t> sizes;  // bytes saved per region, in regions() order
};

std::mutex regions_mutex;
std::vector<Region> extra_regions;
std::map<int, Slot> slots;

std::vector<Region> regions() {
    std::vector<Region> list;
    if (__start_plm_game_data != nullptr && __stop_plm_game_data > __start_plm_game_data)
        list.push_back({__start_plm_game_data, static_cast<size_t>(__stop_plm_game_data - __start_plm_game_data)});
    if (__start_plm_game_bss != nullptr && __stop_plm_game_bss > __start_plm_game_bss)
        list.push_back({__start_plm_game_bss, static_cast<size_t>(__stop_plm_game_bss - __start_plm_game_bss)});
    std::lock_guard lock(regions_mutex);
    list.insert(list.end(), extra_regions.begin(), extra_regions.end());
    return list;
}

size_t saved_size(const Region& r) {
    if (r.used == nullptr) return r.size;
    const size_t used = r.used();
    return used < r.size ? used : r.size;
}

} // namespace

// A game heap outside the game's sources (e.g. a pool array in the port layer): included in every state.
extern "C" void plm_savestate_add_region(void* start, size_t size) {
    std::lock_guard lock(regions_mutex);
    extra_regions.push_back({static_cast<char*>(start), size});
}

// A game heap that is filled from its start (a bump allocator) and mostly empty: only its first used() bytes are
// saved, and a load restores exactly what was saved (the rest is unused by definition). First user: SpaghettiKart's
// 256 MB memory pool.
extern "C" void plm_savestate_add_dynamic_region(void* start, size_t capacity, size_t (*used)(void)) {
    std::lock_guard lock(regions_mutex);
    extra_regions.push_back({static_cast<char*>(start), capacity, used});
}

// Game thread, between ticks. Returns false when the module has no game section (nothing to save).
extern "C" bool plm_savestate_save(int slot) {
    const std::vector<Region> list = regions();
    std::vector<size_t> sizes;
    size_t total = 0;
    for (const Region& r : list) {
        sizes.push_back(saved_size(r));
        total += sizes.back();
    }
    if (total == 0) return false;
    Slot& state = slots[slot];
    state.data.resize(total);
    state.sizes = sizes;
    size_t offset = 0;
    if (plm_arena_snapshot_begin != nullptr) plm_arena_snapshot_begin();
    for (size_t i = 0; i < list.size(); i++) {
        std::memcpy(state.data.data() + offset, list[i].start, sizes[i]);
        offset += sizes[i];
    }
    if (plm_arena_snapshot_end != nullptr) plm_arena_snapshot_end();
    __android_log_print(ANDROID_LOG_INFO, "plm-savestate", "saved slot %d (%zu bytes, %zu regions)", slot, total,
                        list.size());
    if (plm_arena_log_usage != nullptr) plm_arena_log_usage();
    return true;
}

// Game thread, between ticks. Returns false when the slot is empty.
extern "C" bool plm_savestate_load(int slot) {
    const auto it = slots.find(slot);
    if (it == slots.end()) return false;
    const Slot& state = it->second;
    const std::vector<Region> list = regions();
    // Regions are append-only. Ones registered after this save (e.g. BattleShip's pooled coroutine stacks, created
    // as the game spawns more GObj threads) weren't in the state and are left as they are: the module keeps them
    // consistent itself (BattleShip saves its pool's in-use flags, so they come back free).
    bool matches = list.size() >= state.sizes.size();
    for (size_t i = 0; matches && i < state.sizes.size(); i++) {
        // Fixed regions must be the same size; a dynamic one only has to fit.
        matches = list[i].used == nullptr ? state.sizes[i] == list[i].size : state.sizes[i] <= list[i].size;
    }
    if (!matches) {
        __android_log_print(ANDROID_LOG_ERROR, "plm-savestate", "slot %d doesn't match the game's memory", slot);
        return false;
    }
    size_t offset = 0;
    if (plm_arena_snapshot_begin != nullptr) plm_arena_snapshot_begin();
    for (size_t i = 0; i < state.sizes.size(); i++) {
        std::memcpy(list[i].start, state.data.data() + offset, state.sizes[i]);
        offset += state.sizes[i];
    }
    if (plm_arena_snapshot_end != nullptr) plm_arena_snapshot_end();
    __android_log_print(ANDROID_LOG_INFO, "plm-savestate", "loaded slot %d", slot);
    return true;
}

// For ports that run module work on the frame thread (lus_shim's plm_module_frame): plm_save_state / plm_load_state
// queue a request from the UI thread, and the frame hook runs the queue at the end of a game tick.
namespace {
std::mutex requests_mutex;
std::vector<std::pair<bool, int>> requests;  // (load?, slot)
}

extern "C" void plm_savestate_request(bool load, int slot) {
    std::lock_guard lock(requests_mutex);
    requests.push_back({load, slot});
}

extern "C" void plm_savestate_run_requests(void) {
    std::vector<std::pair<bool, int>> todo;
    {
        std::lock_guard lock(requests_mutex);
        todo.swap(requests);
    }
    for (const auto& [load, slot] : todo) {
        if (load) plm_savestate_load(slot);
        else plm_savestate_save(slot);
    }
}
