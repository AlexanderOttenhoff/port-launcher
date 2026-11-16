// Port-launcher additions for 2 Ship 2 Harkinian (Majora's Mask; docs/planning/MENU.md). Applied on the frame thread
// (2Ship's game thread: it runs the game loop and presents on the same thread) through the libultraship shim's
// plm_module_frame hook, once 2Ship's managers exist. The port's own settings are applied generically by the
// libultraship shim (lus_shim.cpp); our hand-made option keys were removed on 2026-11-15.
//
// Also here: making 2ship.o2r on the device (see make_port_archive) and registering the malloc'd game heaps for save
// states (__wrap_Heaps_Alloc).

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <android/log.h>
#include <libultraship/bridge.h>

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/controldevice/controller/mapping/sdl/SDLButtonToButtonMapping.h"
#include "ship/controller/physicaldevice/PhysicalDeviceType.h"
#include <SDL2/SDL.h>
#include <libultraship/libultra/controller.h>
#include "2s2h/ShipInit.hpp"
#include "2s2h/GameInteractor/GameInteractor.h"

extern "C" {
int zapd_main(int argc, char* argv[]);                       // ZAPDTR/ZAPD/Main.cpp (ZAPDLib, linked into the game)
void AudioSeq_SetPortVolumeScale(uint8_t seqPlayerIndex, float volume);  // src/audio/sequence.c
extern uint16_t gBuildVersionMajor, gBuildVersionMinor, gBuildVersionPatch;  // include/build.h
extern uint8_t* gSystemHeap;                                  // src/buffers/heaps.c
extern uint8_t* gAudioHeap;
void __real_Heaps_Alloc(void);
void plm_savestate_add_region(void* start, size_t size);     // plm_savestate.cpp
void plm_savestate_request(bool load, int slot);
void plm_savestate_run_requests(void);
}

namespace {

constexpr const char* log_tag = "plm-2ship";
// include/sequence.h
constexpr uint8_t PLM_SEQ_PLAYER_BGM_MAIN = 0, PLM_SEQ_PLAYER_FANFARE = 1, PLM_SEQ_PLAYER_SFX = 2, PLM_SEQ_PLAYER_BGM_SUB = 3,
                  PLM_SEQ_PLAYER_AMBIENCE = 4;
// AUDIO_HEAP_SIZE and SYSTEM_HEAP_SIZE come from include/z64.h (pulled in by GameInteractor.h).

bool baseline_done = false;

// 2Ship applies many CVars through per-CVar "ship init" callbacks (2s2h/ShipInit.hpp), run by its menu after a
// change; we do the same.
void set_int(const char* cvar, int value) {
    CVarSetInteger(cvar, value);
    ShipInit::Init(cvar);
}

// The controller layout (owner 2026-11-16, same as Ocarina of Time's in modules/shipwright/plm_options.cpp): the
// right stick is camera only and the C buttons move to shoulder and face buttons, exactly as the pre-installed Switch
// Pro Button Icons mod (GameBanana 517884: its HUD, text and ocarina icons show these buttons in place of the C
// arrows) lays them out: C-up = L1, C-left = Y, C-right = X, C-down = R1, N64 L (the minimap toggle) = Select. Z and
// R keep 2Ship's defaults (left / right trigger); A, B, Start and the D-pad too.
// Why the right stick loses its C bindings: 2Ship binds C to the right stick by default, so with free look on, every
// camera move also fired a C item. Why SDL Y for C-left and SDL X for C-right: Android reports the RP6's face buttons
// by their printed label. Gamepad (SDL) mappings only. Versioned: applied once per layout revision, so a player's own
// remapping in 2Ship's controller menu is kept afterwards. Select reaches the game through the manifest's
// "select_to_game".
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

// Once, while unset: the modern camera (ROADMAP principle 4). 2Ship's free look puts the camera on the right stick;
// off by default upstream. A later choice in 2Ship's own config is kept.
void apply_baseline() {
    if (CVarGetInteger("gEnhancements.Camera.FreeLook.Enable", -1) == -1) {
        set_int("gEnhancements.Camera.FreeLook.Enable", 1);
        CVarSave();
        __android_log_print(ANDROID_LOG_INFO, log_tag, "enabled free look");
    }
    apply_control_layout();
}

// 2ship.o2r (the port's own assets: fonts, textures, the version file; no game content) is made at build time by a
// host ZAPD upstream ("botr" mode of extract_assets.py). The game links the same ZAPD for ROM extraction, so the module
// runs that mode here, before the game starts, from the upstream custom assets it ships (data/custom), into the config
// dir. Redone when the stamp (2Ship version) changes, since 2Ship refuses an archive from another version.
void make_port_archive() {
    namespace fs = std::filesystem;
    const char* bundle = std::getenv("PLM_BUNDLE");
    if (bundle == nullptr || !*bundle) return;
    char version[24];
    snprintf(version, sizeof(version), "%u.%u.%u", gBuildVersionMajor, gBuildVersionMinor, gBuildVersionPatch);
    const fs::path stamp = "2ship.o2r.version";  // relative to the config dir (the shim chdir's there)
    std::error_code ec;
    if (fs::exists("2ship.o2r", ec) && fs::exists(stamp, ec)) {
        std::FILE* f = std::fopen(stamp.c_str(), "r");
        char old[24] = {};
        if (f != nullptr) {
            std::fgets(old, sizeof(old), f);
            std::fclose(f);
        }
        if (std::string(old) == version) return;
    }
    const std::string custom = std::string(bundle) + "/custom";
    std::vector<std::string> args{"ZAPD", "botr", "-se", "OTR", "--norom", "--customAssetsPath", custom,
                                  "--customOtrFile", "2ship.o2r", "--portVer", version};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    __android_log_print(ANDROID_LOG_INFO, log_tag, "making 2ship.o2r (%s) from %s", version, custom.c_str());
    const int rc = zapd_main(static_cast<int>(args.size()), argv.data());
    if (rc != 0 || !fs::exists("2ship.o2r", ec)) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "making 2ship.o2r failed (%d)", rc);
        return;
    }
    if (std::FILE* f = std::fopen(stamp.c_str(), "w")) {
        std::fputs(version, f);
        std::fclose(f);
    }
}

} // namespace

// Called by the libultraship shim before the game starts (its launch settings are unused now).
extern "C" __attribute__((visibility("default"))) void plm_module_settings(const char*) {
    make_port_archive();  // before the game starts: it checks for 2ship.o2r first thing
}

// Save states (generic game-section snapshot): the system heap (game actors, scenes) and the audio heap are allocated
// by Heaps_Alloc outside the section; register them once they exist. Both stay at the same address for the session.
// RISK (untested on a device): the audio thread mixes from the audio heap while a state is copied or restored between
// frames; SoH's own save states take an audio lock for this, our generic path doesn't.
extern "C" void __wrap_Heaps_Alloc(void) {
    __real_Heaps_Alloc();
    if (gSystemHeap != nullptr) plm_savestate_add_region(gSystemHeap, SYSTEM_HEAP_SIZE);
    if (gAudioHeap != nullptr) plm_savestate_add_region(gAudioHeap, AUDIO_HEAP_SIZE);
}

extern "C" __attribute__((visibility("default"))) void plm_save_state(const char* slot) {
    plm_savestate_request(false, std::atoi(slot));
}

extern "C" __attribute__((visibility("default"))) void plm_load_state(const char* slot) {
    plm_savestate_request(true, std::atoi(slot));
}

extern "C" void plm_module_frame(void) {
    // Frames are presented during extraction too, before 2Ship's managers exist; its ship-init callbacks need them
    // (as in Ship of Harkinian). GameInteractor is created right after extraction.
    if (GameInteractor::Instance == nullptr) return;
    plm_savestate_run_requests();
    if (!baseline_done) {
        baseline_done = true;
        apply_baseline();
    }
}

// The port's own settings (settings_schema.json, MENU.md design of 2026-11-14) are set generically by the libultraship
// shim (modules/common/lus/lus_shim.cpp, CVarSet* by the CVar's name). What a CVar change needs beyond the set, as the
// port's own menu callback does it, goes here; it runs on the frame thread right after each set.
extern "C" bool plm_lus_port_ready(void) {
    return GameInteractor::Instance != nullptr;  // as plm_module_frame: 2Ship presents frames while extracting
}

extern "C" void plm_lus_cvar_changed(const char* cvar) {
    ShipInit::Init(cvar);
    // Sequence-player volumes: 2Ship's menu pushes them to the players in its callback (BenMenu.cpp).
    const std::pair<const char*, uint8_t> players[] = {
        {"gSettings.Audio.MainMusicVolume", PLM_SEQ_PLAYER_BGM_MAIN}, {"gSettings.Audio.SubMusicVolume", PLM_SEQ_PLAYER_BGM_SUB},
        {"gSettings.Audio.FanfareVolume", PLM_SEQ_PLAYER_FANFARE}, {"gSettings.Audio.SoundEffectsVolume", PLM_SEQ_PLAYER_SFX},
        {"gSettings.Audio.AmbienceVolume", PLM_SEQ_PLAYER_AMBIENCE},
    };
    for (const auto& [name, player] : players) {
        if (std::string(cvar) == name) AudioSeq_SetPortVolumeScale(player, CVarGetFloat(cvar, 1.f));
    }
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
