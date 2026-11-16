// Port-launcher glue for Ghostship (docs/planning/MENU.md): the camera baseline, save states, and what a port CVar
// change needs beyond the set. The port's own settings are applied generically by the libultraship shim (lus_shim.cpp);
// our hand-made option keys were removed on 2026-11-15 (the ports' own options replaced them).

#include <cstdlib>
#include <string>
#include <utility>

#include <android/log.h>
#include <libultraship/bridge.h>

extern "C" void audio_set_player_volume(unsigned char player, float volume);  // src/audio/external.c
constexpr unsigned char SEQ_PLAYER_LEVEL = 0, SEQ_PLAYER_ENV = 1, SEQ_PLAYER_SFX = 2;

namespace {

constexpr const char* log_tag = "plm-ghostship";

bool baseline_done = false;

// The launcher's camera baseline: Ghostship's Manual camera mode (the only mode that reads the right stick) with the
// analog camera on both axes (the port's own options, off by default; Lakitu mode by default). Applied once per
// option, only while the player hasn't chosen otherwise in Ghostship.
void apply_camera_baseline() {
    bool changed = false;
    const std::pair<const char*, int> baseline[] = {
        {"gEnhancements.Camera.DefaultMode", 2},  // 0 Lakitu, 1 Mario, 2 Manual
        {"gEnhancements.Camera.HorizontalAnalog", 1},
        {"gEnhancements.Camera.VerticalAnalog", 1},
    };
    for (const auto& [cvar, value] : baseline) {
        if (CVarGetInteger(cvar, -1) == -1) {
            CVarSetInteger(cvar, value);
            changed = true;
        }
    }
    if (changed) {
        CVarSave();
        __android_log_print(ANDROID_LOG_INFO, log_tag, "enabled the analog camera");
    }
}

} // namespace

extern "C" void plm_savestate_run_requests(void);

extern "C" void plm_module_frame(void) {
    plm_savestate_run_requests();
    if (!baseline_done) {
        baseline_done = true;
        apply_camera_baseline();
    }
}

// Save states (docs/planning/SAVE_STATES.md): the decomp's globals are in the game section (module.cmake); the main
// pool, a 32 MB static array in the port layer (src/port/Game.cpp alloc_pool), is registered when the game hands it to
// main_pool_init (wrapped at link time). Requests arrive from the UI thread and run here, on the frame thread, at the
// end of a game tick (the swap is in display_and_vsync, the tick's last step).
extern "C" void plm_savestate_add_region(void* start, size_t size);
extern "C" void __real_main_pool_init(void* start, void* end);

extern "C" void __wrap_main_pool_init(void* start, void* end) {
    static bool registered = false;
    if (!registered) {
        registered = true;
        plm_savestate_add_region(start, static_cast<char*>(end) - static_cast<char*>(start));
    }
    __real_main_pool_init(start, end);
}

extern "C" void plm_savestate_request(bool load, int slot);
extern "C" void plm_savestate_run_requests(void);

extern "C" __attribute__((visibility("default"))) void plm_save_state(const char* slot) {
    plm_savestate_request(false, std::atoi(slot));
}

extern "C" __attribute__((visibility("default"))) void plm_load_state(const char* slot) {
    plm_savestate_request(true, std::atoi(slot));
}

// The port's own settings (settings_schema.json, MENU.md design of 2026-11-14) are set generically by the libultraship
// shim (modules/common/lus/lus_shim.cpp, CVarSet* by the CVar's name). What a CVar change needs beyond the set, as the
// port's own menu callback does it, goes here; it runs on the frame thread right after each set.
extern "C" void plm_lus_cvar_changed(const char* cvar) {
    // Sequence-player volumes: the master is read every audio frame, the players only when set (external.c).
    const std::string c = cvar;
    if (c == "gSettings.Volume.MainMusic") audio_set_player_volume(SEQ_PLAYER_LEVEL, CVarGetInteger(cvar, 100) / 100.f);
    else if (c == "gSettings.Volume.Environment") audio_set_player_volume(SEQ_PLAYER_ENV, CVarGetInteger(cvar, 100) / 100.f);
    else if (c == "gSettings.Volume.SFX") audio_set_player_volume(SEQ_PLAYER_SFX, CVarGetInteger(cvar, 100) / 100.f);
}
