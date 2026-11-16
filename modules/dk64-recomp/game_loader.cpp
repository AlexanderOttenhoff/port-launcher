// Module side of the recomp split (docs/planning/ON_DEVICE_RECOMP.md, spike 2), built into libplm_dk64-recomp.so
// when PLM_SPLIT_GAME is on (module.cmake).
//
// Why: the game code N64Recomp generates from the ROM (RecompiledFuncs/, rsp/) may never be published, so with the
// split it lives in a separate libgame.so that is built from the player's own ROM (on the device, eventually; on
// the desktop for this spike). The module itself then holds no ROM-derived code and can go on the public module
// server like the decomps.
//
// The module needs exactly five things from the game side (measured with nm on the unsplit build, 2026-11-13); the
// rest of the traffic goes the other way (generated code calling ~180 runtime functions, which libgame.so resolves
// against this library, its DT_NEEDED). These trampolines stand in for those five: on first use they dlopen
// libgame.so and ask its plm_game_get_api() for the real functions (game_entry.cpp). No link-time dependency on the
// game exists, so this library loads without libgame.so, and a missing libgame.so is a clear error, not a linker
// failure at System.load.
//
// Where libgame.so is: launch.json "game_lib" (the shim exports it as PLM_GAME_LIB), else <config dir>/libgame.so
// (APP_FOLDER_PATH), which is what the spike's manual copy uses until the app builds and passes it.
#include <android/log.h>
#include <dlfcn.h>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "game_api.h"

namespace dk64 {
void register_bk_overlays();
void register_bk_patches();
}

namespace {
const plm_game_api* game_api() {
    static const plm_game_api* api = [] () -> const plm_game_api* {
        std::string path;
        if (const char* lib = std::getenv("PLM_GAME_LIB"); lib != nullptr && *lib != '\0') path = lib;
        else if (const char* dir = std::getenv("APP_FOLDER_PATH"); dir != nullptr) path = std::string(dir) + "/libgame.so";
        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            __android_log_print(ANDROID_LOG_FATAL, "plm-dk64", "can't load the game code %s: %s", path.c_str(), dlerror());
            std::abort();
        }
        auto get = reinterpret_cast<const plm_game_api* (*)()>(dlsym(handle, "plm_game_get_api"));
        const plm_game_api* found = get != nullptr ? get() : nullptr;
        if (found == nullptr || found->version != PLM_GAME_API_VERSION) {
            __android_log_print(ANDROID_LOG_FATAL, "plm-dk64", "%s: no plm_game_get_api or wrong version", path.c_str());
            std::abort();
        }
        __android_log_print(ANDROID_LOG_INFO, "plm-dk64", "game code loaded from %s", path.c_str());
        return found;
    }();
    return api;
}
} // namespace

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx) {
    game_api()->entrypoint(rdram, ctx);
}

RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr) {
    return game_api()->n_aspMain(rdram, ucode_addr);
}

void dk64::register_bk_overlays() {
    game_api()->register_overlays();
}

void dk64::register_bk_patches() {
    game_api()->register_patches();
}

// From the generated lookup.cpp (RecompiledFuncs/), which holds no game code: the boot entry address in the ROM
// header, the same for every DK64 revision the config supports. Kept here so the module needs nothing generated.
gpr get_entrypoint_address() { return (gpr)(int32_t)0x80000400u; }
const char* get_rom_name() { return ".z64"; }
