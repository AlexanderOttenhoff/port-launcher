// Port-launcher additions for PaperBoat (Paper Mario; docs/planning/MENU.md): save states, and what a port CVar change
// needs beyond the set. The port's own settings are applied generically by the libultraship shim (lus_shim.cpp); our
// hand-made option keys were removed on 2026-11-15 (the ports' own options replaced them). Upstream's pad defaults
// are kept.
//
// PaperBoat's own touch controls (TouchControls.cpp, on by default on Android) used to be switched off here every
// launch. They're now the port's own "Enable Touch Controls" option in our menu, with our default Off forced by the
// schema (tools/schema/lus_schema.py, paperboat "defaults"; an input default, which stays ours), so a player who wants
// them can turn them on.

#include <cstdlib>
#include <string>

#include <libultraship/bridge.h>

#include "ship/Context.h"
#include "ship/window/Window.h"

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
