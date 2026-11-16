// Port-launcher additions for SpaghettiKart (Mario Kart 64; docs/planning/MENU.md). Applied on the frame thread through
// the libultraship shim's plm_module_frame hook, after SpaghettiKart has loaded its config (spaghettify.cfg.json).
//
// What's here: applying the port's settings before the intro reads them, save states, and what a port CVar change
// needs beyond the set. The port's own settings are applied generically by the libultraship shim (lus_shim.cpp); our
// hand-made option keys were removed on 2026-11-15. Pad layout: upstream's defaults are kept (A accelerate, X brake, R1/R2 hop, L2 item,
// Y/B are C-left/C-down); a racing control profile is a later Controls-tab job (MENU.md "Input").

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <android/log.h>
#include <libultraship/bridge.h>

#include "ship/Context.h"
#include "ship/window/Window.h"

extern "C" void plm_savestate_add_dynamic_region(void* start, size_t capacity, size_t (*used)(void));  // plm_savestate.cpp
extern "C" void audio_set_player_volume(unsigned char player, float volume);  // src/audio/external.c
// src/audio/external.h: sequence players 0 = level music, 1 = environment (jingles, crowd), 2 = sound effects.
constexpr unsigned char SEQ_PLAYER_LEVEL = 0, SEQ_PLAYER_ENV = 1, SEQ_PLAYER_SFX = 2;

namespace {

constexpr const char* log_tag = "plm-spaghettikart";

// Save states and MK64's memory pool (src/racing/memory.c): a 256 MB static bump allocator, kept out of the game's
// save-state section by patch 0004. Only [pool start, gNextFreeMemoryAddress) is in use; everything above it is free
// by construction (courses reset the pointer to an anchor and refill). The pool array is static, so its start comes
// from sPoolEnd (a global: start + 256 MB, set statically and again by initialize_memory_pool). A 1 MB margin covers
// code that writes just past the pointer before advancing it (gCollisionMesh is built at gNextFreeMemoryAddress).
constexpr size_t mk64_pool_size = 0x10000000;  // memory.c: sMemoryPool[0x10000000]
constexpr size_t mk64_pool_margin = 1 << 20;
extern "C" uintptr_t sPoolEnd;               // src/racing/memory.c
extern "C" uintptr_t gNextFreeMemoryAddress; // src/code_800029B0.c

size_t mk64_pool_used() {
    const uintptr_t start = sPoolEnd - mk64_pool_size;
    if (gNextFreeMemoryAddress < start) return 0;  // before initialize_memory_pool()
    return gNextFreeMemoryAddress - start + mk64_pool_margin;  // plm_savestate clamps to the capacity
}

void register_memory_pool() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    plm_savestate_add_dynamic_region(reinterpret_cast<void*>(sPoolEnd - mk64_pool_size), mk64_pool_size, mk64_pool_used);
}

} // namespace

// Patch 0002: SpaghettiKart's main() calls this after its config and window exist but before it reads gSkipIntro,
// so the launch settings (Startup Behaviour above all) apply to this launch rather than the next one.
extern "C" void plm_lus_apply_port_settings(void);  // lus_shim.cpp: the port's own CVars from our menu

extern "C" void PortLauncher_BeforeIntro(void) {
    plm_lus_apply_port_settings();  // also before the intro, e.g. Startup Behaviour-style CVars read once at boot
    register_memory_pool();
}


// Save states (docs/planning/SAVE_STATES.md): requests from the UI thread run on the frame thread, at the end of a game
// tick (the swap ends the frame).
extern "C" void plm_savestate_request(bool load, int slot);
extern "C" void plm_savestate_run_requests(void);

extern "C" __attribute__((visibility("default"))) void plm_save_state(const char* slot) {
    plm_savestate_request(false, std::atoi(slot));
}

extern "C" __attribute__((visibility("default"))) void plm_load_state(const char* slot) {
    plm_savestate_request(true, std::atoi(slot));
}

extern "C" void plm_module_frame(void) {
    plm_savestate_run_requests();
}

// The port's own settings (settings_schema.json, MENU.md design of 2026-11-14) are set generically by the libultraship
// shim (modules/common/lus/lus_shim.cpp, CVarSet* by the CVar's name). What a CVar change needs beyond the set, as the
// port's own menu callback does it, goes here; it runs on the frame thread right after each set.
extern "C" void plm_lus_cvar_changed(const char* cvar) {
    // Per-player volumes reach the sequence players only at load or from the port menu's callback (PortMenu.cpp).
    const std::string c = cvar;
    if (c == "gMainMusicVolume") audio_set_player_volume(SEQ_PLAYER_LEVEL, CVarGetFloat(cvar, 1.f));
    else if (c == "gSFXMusicVolume") audio_set_player_volume(SEQ_PLAYER_SFX, CVarGetFloat(cvar, 1.f));
    else if (c == "gEnvironmentVolume") audio_set_player_volume(SEQ_PLAYER_ENV, CVarGetFloat(cvar, 1.f));
    // The interpreter reads these two only at init; the window setters apply them live (the port menu's callbacks).
    const std::string name = cvar;
    auto ctx = Ship::Context::GetRawInstance();
    auto w = ctx != nullptr ? ctx->GetWindow() : nullptr;
    if (w != nullptr && name.size() >= 18 && name.compare(name.size() - 18, 18, "InternalResolution") == 0) {
        w->SetResolutionMultiplier(CVarGetFloat(cvar, 1.f));
    } else if (w != nullptr && name.size() >= 9 && name.compare(name.size() - 9, 9, "MSAAValue") == 0) {
        w->SetMsaaLevel(CVarGetInteger(cvar, 1));
    }
}
