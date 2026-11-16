// Port-launcher additions for Ship of Harkinian (Ocarina of Time; docs/planning/MENU.md). Applied on the frame thread
// (SoH's game thread: it runs the game loop and presents on the same thread) through the libultraship shim's
// plm_module_frame hook, after SoH has loaded its config.
//
// What's here: the free-look camera baseline, save states (through SoH's own SaveStateMgr), and what a port CVar
// change needs beyond the set. The port's own settings are applied generically by the libultraship shim
// (lus_shim.cpp); our hand-made option keys were removed on 2026-11-15 (the ports' own options replaced them).

#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <SDL2/SDL.h>
#include <android/log.h>
#include <libultraship/bridge.h>
#include <libultraship/libultra/controller.h>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "ship/controller/physicaldevice/PhysicalDeviceType.h"
#include "soh/OTRGlobals.h"
#include "soh/ShipInit.hpp"
#include "soh/Enhancements/savestates.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

namespace {

constexpr const char* log_tag = "plm-shipwright";

std::mutex pending_mutex;
std::vector<std::pair<bool, unsigned int>> state_requests;  // (load?, slot), guarded by pending_mutex
bool baseline_done = false;

// SoH applies many CVars through "ship init" callbacks registered per CVar (ShipInit.hpp); its menu runs them after a
// change, so we do the same after setting one.
void set_int(const char* cvar, int value) {
    CVarSetInteger(cvar, value);
    ShipInit::Init(cvar);
}

// The controller layout (owner 2026-11-16): the right stick is camera only, and the C buttons move to face and
// shoulder buttons, exactly as the Nintendo Style Controls glyph mod (GameBanana 465012, pre-installed: its HUD shows
// these buttons in place of the C arrows) says to map them: "A=A B=B CUP=L C-LEFT=Y C-RIGHT=X C-DOWN=R L=MINUS/SELECT".
// So: C-up = L1, C-left = Y, C-right = X, C-down = R1, N64 L (the dungeon minimap toggle) = Select. Z and R keep
// SoH's defaults (left / right trigger); A, B, Start and the D-pad too.
// Why the right stick loses its C bindings: SoH binds C to the right stick by default, so with free look on, every
// camera move also fired a C item. With C on buttons, the stick does one thing.
// Why SDL Y for C-left and SDL X for C-right: Android reports the RP6's face buttons by their printed label
// (KEYCODE_BUTTON_Y on the button labelled Y), as it does for A and B, which SoH's defaults already bind by name.
// Gamepad (SDL) mappings only; keyboard bindings are untouched. Versioned like BK's ControlSchemes baseline: applied
// once per layout revision, so a player's own remapping in SoH's controller menu is kept afterwards, and a later
// revision of ours can still reach existing installs. Select must reach the game for N64 L: the manifest's
// "select_to_game" tells our host not to swallow it.
constexpr int layout_revision = 1;

void rebind(const std::shared_ptr<Ship::Controller>& controller, CONTROLLERBUTTONS_T bitmask, int32_t sdl_button) {
    auto button = controller->GetButton(bitmask);
    if (button == nullptr) return;
    button->ClearAllButtonMappingsForDeviceType(Ship::SDLGamepad);
    auto mapping = std::make_shared<Ship::SDLButtonToButtonMapping>(0, bitmask, sdl_button);
    button->AddButtonMapping(mapping);
    mapping->SaveToConfig();
    button->SaveButtonMappingIdsToConfig();
}

void apply_control_layout() {
    if (CVarGetInteger("gPortLauncher.ControlLayout", 0) >= layout_revision) return;
    auto ctx = Ship::Context::GetRawInstance();
    auto deck = ctx != nullptr ? ctx->GetControlDeck() : nullptr;
    auto controller = deck != nullptr ? deck->GetControllerByPort(0) : nullptr;
    if (controller == nullptr) return;
    rebind(controller, BTN_CUP, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    rebind(controller, BTN_CLEFT, SDL_CONTROLLER_BUTTON_Y);
    rebind(controller, BTN_CRIGHT, SDL_CONTROLLER_BUTTON_X);
    rebind(controller, BTN_CDOWN, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    rebind(controller, BTN_L, SDL_CONTROLLER_BUTTON_BACK);
    CVarSetInteger("gPortLauncher.ControlLayout", layout_revision);
    CVarSave();
    __android_log_print(ANDROID_LOG_INFO, log_tag, "applied control layout %d", layout_revision);
}

// Once, while unset: the modern camera (ROADMAP principle 4: a right-stick camera). SoH's free look moves the camera
// with the right stick; off by default upstream. A choice made in SoH's own config later is kept.
void apply_baseline() {
    if (CVarGetInteger("gSettings.FreeLook.Enabled", -1) == -1) {
        set_int("gSettings.FreeLook.Enabled", 1);
        CVarSave();
        __android_log_print(ANDROID_LOG_INFO, log_tag, "enabled free look");
    }
    apply_control_layout();
}

} // namespace

// Save states through SoH's SaveStateMgr. AddRequest reads gPlayState and pushes to an unguarded queue that the game
// thread drains (graph.c, ProcessSaveStateRequests), so it is called from the frame hook (the game thread), not from
// the UI thread that calls these. SoH refuses (with its own on-screen notice) outside gameplay or for an empty slot.
extern "C" __attribute__((visibility("default"))) void plm_save_state(const char* slot) {
    std::lock_guard lock(pending_mutex);
    state_requests.emplace_back(false, static_cast<unsigned int>(std::atoi(slot)));
}

extern "C" __attribute__((visibility("default"))) void plm_load_state(const char* slot) {
    std::lock_guard lock(pending_mutex);
    state_requests.emplace_back(true, static_cast<unsigned int>(std::atoi(slot)));
}

extern "C" void plm_module_frame(void) {
    // Frames are also presented while SoH's extractor runs (RunExtract, inside InitOTR), before the game's managers
    // exist; SoH's ship-init callbacks (run by set_int) dereference them (a crash in RegisterFreeLookFixes without
    // this). GameInteractor is created right after extraction, so wait for it.
    if (GameInteractor::Instance == nullptr) return;
    if (!baseline_done) {
        baseline_done = true;
        apply_baseline();
    }
    std::vector<std::pair<bool, unsigned int>> states;
    {
        std::lock_guard lock(pending_mutex);
        states.swap(state_requests);
    }
    if (OTRGlobals::Instance == nullptr || OTRGlobals::Instance->gSaveStateMgr == nullptr) return;
    for (const auto& [load, slot] : states) {
        const SaveStateReturn result =
            OTRGlobals::Instance->gSaveStateMgr->AddRequest({ slot, load ? RequestType::LOAD : RequestType::SAVE });
        __android_log_print(ANDROID_LOG_INFO, log_tag, "%s state %u: %s", load ? "load" : "save", slot,
                            result == SaveStateReturn::SUCCESS ? "queued" : "refused");
    }
}

// The port's own settings (settings_schema.json, MENU.md design of 2026-11-14) are set generically by the libultraship
// shim (modules/common/lus/lus_shim.cpp, CVarSet* by the CVar's name). What a CVar change needs beyond the set, as the
// port's own menu callback does it, goes here; it runs on the frame thread right after each set.
// SoH arms most enhancements through per-CVar ShipInit callbacks, which its menu runs after a change.
extern "C" bool plm_lus_port_ready(void) {
    return GameInteractor::Instance != nullptr;  // as plm_module_frame: SoH presents frames while extracting
}

extern "C" void plm_lus_cvar_changed(const char* cvar) {
    ShipInit::Init(cvar);
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
