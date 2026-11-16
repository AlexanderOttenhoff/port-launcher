// Port-launcher additions for Starship (docs/planning/MENU.md). Applied on the frame thread through the libultraship
// shim's plm_module_frame hook, after Starship has loaded its config and controllers.
//
// Pad layout: the module's patch 0002 changes Starship's default bindings (B = bomb, X = boost, Y = brake), but a
// config saved before keeps its old bindings. Each revision of our layout resets the gamepad bindings to the
// defaults once (keyboard untouched), recorded in gPortLauncher.PadLayout.

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <android/log.h>

#include "libultraship/src/Context.h"
#include "libultraship/src/controller/controldeck/ControlDeck.h"
#include "libultraship/src/controller/controldevice/controller/Controller.h"
#include "libultraship/src/controller/physicaldevice/PhysicalDeviceType.h"
#include "libultraship/src/public/bridge/consolevariablebridge.h"

namespace {

constexpr int pad_layout_revision = 1;
constexpr const char* pad_layout_cvar = "gPortLauncher.PadLayout";
bool checked = false;

void apply_pad_layout() {
    if (CVarGetInteger(pad_layout_cvar, 0) >= pad_layout_revision) return;
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr || ctx->GetControlDeck() == nullptr) return;
    auto controller = ctx->GetControlDeck()->GetControllerByPort(0);
    if (controller == nullptr) return;
    controller->ClearAllMappingsForDeviceType(Ship::PhysicalDeviceType::SDLGamepad);
    controller->AddDefaultMappings(Ship::PhysicalDeviceType::SDLGamepad);
    CVarSetInteger(pad_layout_cvar, pad_layout_revision);
    CVarSave();
    __android_log_print(ANDROID_LOG_INFO, "plm-starship", "applied the launcher pad layout (revision %d)",
                        pad_layout_revision);
}

} // namespace

// Controller navigation of Starship's own menu, on by default as other libultraship ports do on consoles (owner,
// 2026-11-12). Set once while unset.
void apply_menu_navigation() {
    const char* cvar = "gControlNav";  // CVAR_IMGUI_CONTROLLER_NAV (libultraship/cmake/cvars.cmake)
    if (CVarGetInteger(cvar, -1) == -1) {
        CVarSetInteger(cvar, 1);
        CVarSave();
    }
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
    if (!checked) {
        checked = true;
        apply_pad_layout();
        apply_menu_navigation();
    }
}
