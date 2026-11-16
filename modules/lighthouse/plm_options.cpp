// Port-launcher glue for Lighthouse (docs/planning/MENU.md): the camera/control baseline and save states. The port's
// own settings are applied generically by the libultraship shim (lus_shim.cpp); our hand-made option keys were
// removed on 2026-11-15 (the ports' own options replaced them).

#include <cstdlib>
#include <string>

#include <android/log.h>
#include <unistd.h>
#include <libultraship/bridge.h>

#include "port/FrameHost.h"
#include "port/Controller/ControlSchemes.h"
#include "port/ShipInit.hpp"
#include "port/UI/cvar_prefixes.h"

namespace {

// The launcher's camera baseline (owner, 2026-11-12: "Modern + free look"): Lighthouse's Modern control scheme (the
// Xbox Live Arcade layout: right stick turns the camera, C-up/C-down on face buttons, R1 recentres) and its Free
// Look right-stick orbit camera. Applied once, only while the player hasn't chosen otherwise in Lighthouse: the
// scheme rewrites the gamepad bindings and saves them, as Lighthouse's own menu does.
void apply_camera_baseline() {
    Lighthouse::RunOnGameThread([] {
        // Layout revision of our Modern bindings (2: B stays B, C-down on X, patch 0006; 3: R3 = C-up, 0009). A player on an older
        // revision of our Modern layout gets it re-applied once; a scheme they picked themselves is left alone.
        constexpr int layout_revision = 3;
        const char* scheme = CVAR_SETTING("Controls.Scheme");
        const char* revision = "gPortLauncher.ModernLayout";
        const int current = CVarGetInteger(scheme, -1);
        if (current == -1 || (current == CONTROL_SCHEME_MODERN && CVarGetInteger(revision, 1) < layout_revision)) {
            CVarSetInteger(scheme, CONTROL_SCHEME_MODERN);
            CVarSetInteger(revision, layout_revision);
            ControlSchemes_Apply(CONTROL_SCHEME_MODERN);  // saves the bindings and CVars
            __android_log_print(ANDROID_LOG_INFO, "plm-lighthouse", "applied the Modern control scheme (layout %d)",
                                layout_revision);
        }
        const char* free_look = CVAR_ENHANCEMENT("Camera.FreeLook.Enabled");
        if (CVarGetInteger(free_look, -1) == -1) {
            CVarSetInteger(free_look, 1);
            ShipInit::Init(free_look);
            CVarSave();
            __android_log_print(ANDROID_LOG_INFO, "plm-lighthouse", "enabled Free Look");
        }
    });
}

} // namespace

// Called by the libultraship shim with the launch settings (unused now); the hook is just the moment to apply the
// baseline, before the game starts.
extern "C" __attribute__((visibility("default"))) void plm_module_settings(const char*) {
    apply_camera_baseline();
}

// Save states (docs/planning/SAVE_STATES.md, common/savestate/plm_savestate.cpp): taken and restored between ticks
// on the game thread. The host calls these from its in-game menu while the game is paused, so they run right after it
// resumes, before the next tick: the state is the paused frame.
//
// Quiescing (why, RP6 2026-11-13: load still crashed after the arena and graveyard fixes): "between ticks" is only
// quiet for the tick thread. Lighthouse runs BK's other N64 threads as real OS threads (src/port/OS/OS_Threads.cpp):
// thread5 (task lists, framebuffer flags), the audio manager (synthesizer voices, command lists), joy, viMgr and the
// motor thread, plus the window thread rendering the last display list. They read and write the same game memory
// the state copies, so a copy taken or restored under them tears what they're walking, and after a load each one
// resumes from wherever it was, against memory that says otherwise (an audio thread waiting for a reply the restored
// state never asked for, a thread5 finishing a task that no longer exists). So both save and load now:
//   1. drain the graphics pipeline first (port_pipelineSyncPoint, the game's own "RDP done" handshake Lighthouse
//      uses before map loads): no display list is in flight or being drawn by the window thread;
//   2. copy only while every decomp thread sits blocked at the top of its loop on its own (empty) queue, with the
//      message-queue mutex held so none can wake (OS_RunWithThreadsParked, Lighthouse patch 0013), and thread5 has
//      no task out. States are then always taken and restored at the same, canonical point of every thread.
// If the threads don't line up within a second (shouldn't happen: they idle most of each frame), the request is
// dropped with a log line rather than risking a torn state.
#include "port/DevTools/ThreadWatchdog.h"

extern "C" {
bool plm_savestate_save(int slot);
bool plm_savestate_load(int slot);
void port_pipelineSyncPoint(void);  // src/port/Game.cpp
// Each decomp thread's top-of-loop queue (OSMesgQueue*, passed through as void*: only compared, never touched here).
void* thread5_getTaskQueue(void);
void* audioManager_getFrameMesgQueue(void);
void* si_getEventQueue(void);
void* viMgr_getRetraceQueue(void);
void* baMotor_getRetraceQueue(void);
int OS_RunWithThreadsParked(void* const* idleQueues, int numIdle, int expected, int (*fn)(void*), void* arg);
int OS_LiveThreadCount(void);
}

namespace {

struct StateRequest {
    bool load;
    int slot;
    bool ok = false;
};

int run_parked(void* arg) {
    Thread5WatchdogState t5{};
    thread5_getWatchdogState(&t5);
    // 0x10 = idle (no gfx/audio task started or yielded), and both task rings empty.
    if (t5.unkFlag1 != 0x10 || t5.gfxActiveId != t5.gfxSelectedId || t5.audioActiveId != t5.audioSelectedId) return 0;
    auto* request = static_cast<StateRequest*>(arg);
    request->ok = request->load ? plm_savestate_load(request->slot) : plm_savestate_save(request->slot);
    return 1;
}

void run_state_request(bool load, int slot) {
    ThreadWatchdog_BeginExpectedStall("port-launcher save state");
    port_pipelineSyncPoint();
    void* const idle[] = {thread5_getTaskQueue(), audioManager_getFrameMesgQueue(), si_getEventQueue(),
                          viMgr_getRetraceQueue(), baMotor_getRetraceQueue()};
    StateRequest request{load, slot};
    bool ran = false;
    for (int attempt = 0; attempt < 1000 && !ran; attempt++) {
        ran = OS_RunWithThreadsParked(idle, 5, OS_LiveThreadCount(), run_parked, &request) != 0;
        if (!ran) usleep(1000);
    }
    ThreadWatchdog_EndExpectedStall();
    if (!ran) {
        __android_log_print(ANDROID_LOG_ERROR, "plm-lighthouse", "%s state %d dropped: decomp threads didn't park",
                            load ? "load" : "save", slot);
    }
}

} // namespace

extern "C" __attribute__((visibility("default"))) void plm_save_state(const char* slot) {
    const int n = slot != nullptr ? std::atoi(slot) : 0;
    Lighthouse::RunOnGameThread([n] { run_state_request(false, n); });
}

extern "C" __attribute__((visibility("default"))) void plm_load_state(const char* slot) {
    const int n = slot != nullptr ? std::atoi(slot) : 0;
    Lighthouse::RunOnGameThread([n] { run_state_request(true, n); });
}

// The port's own settings (settings_schema.json, MENU.md design of 2026-11-14) are set generically by the libultraship
// shim (modules/common/lus/lus_shim.cpp, CVarSet* by the CVar's name). What a CVar change needs beyond the set, as the
// port's own menu callback does it, goes here; it runs on the frame thread right after each set.
// Lighthouse arms enhancements through per-CVar ShipInit callbacks (re-armed on the game thread, as apply() does).
extern "C" void plm_lus_cvar_changed(const char* cvar) {
    const std::string name = cvar;
    Lighthouse::RunOnGameThread([name] { ShipInit::Init(name); });
}
