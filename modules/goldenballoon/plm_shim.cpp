// Port-launcher module entry points for Golden Balloon, the Diddy Kong Racing port (docs/MODULE_SPEC.md §4).
// Compiled into the game library by module.cmake. Started from modules/perfect-dark/plm_shim.cpp (same host model).
//
// Golden Balloon reads the ROM at runtime (--rom) and keeps its saves and settings relative to the working
// directory, which is the launcher's save dir. It quits through SDL_QUIT.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <android/log.h>
#include <unistd.h>

#include "nlohmann/json.hpp"
#include "plm/module_abi.h"
#include "video_config.h"  // Golden Balloon's settings registry (platform/)

extern "C" int game_main(int argc, const char** argv) __asm__("main");
extern "C" void __real_SDL_GL_SwapWindow(SDL_Window* window);
extern "C" SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                                        SDL_AudioSpec* obtained, int allowed_changes);

#define PLM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr const char* log_tag = "plm-dkr";

const plm_host_api* host = nullptr;
std::atomic<bool> first_frame_sent{false};
std::atomic<SDL_AudioDeviceID> audio_device{0};

// Host menu pause: the main loop waits after presenting a frame; audio is paused on the port's device.
std::mutex pause_mutex;
std::condition_variable pause_cv;
bool paused = false;

void set_paused(bool value) {
    {
        std::lock_guard lock(pause_mutex);
        paused = value;
    }
    if (audio_device != 0) SDL_PauseAudioDevice(audio_device, value ? 1 : 0);
    pause_cv.notify_all();
}

// The port logs to stdout/stderr, which go nowhere on Android: forward them to logcat.
void forward_stdio_to_logcat() {
    static int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    std::thread([] {
        char buffer[1024];
        std::string line;
        ssize_t n;
        while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) {
            line.append(buffer, static_cast<size_t>(n));
            size_t pos;
            while ((pos = line.find('\n')) != std::string::npos) {
                __android_log_write(ANDROID_LOG_INFO, log_tag, line.substr(0, pos).c_str());
                line.erase(0, pos + 1);
            }
        }
    }).detach();
}

} // namespace

namespace {
// The port's own settings (settings_schema.json, generated from its video-config table by
// tools/schema/goldenballoon_schema.py; MENU.md design of 2026-11-14): keys arrive as "port.<Golden Balloon key>"
// and go to Golden Balloon by its own name, so no per-option code. At launch they become --video-launch-set
// arguments (the registry's LAUNCHER rank: above the ini and presets, below runtime changes); live changes go
// through mdkr_video_config_runtime_set on the frame thread, which validates them and applies live-scope keys at the
// next frame boundary (restart-scope keys take effect at the next launch, where the launch argument carries them).
std::mutex port_mutex;
std::vector<std::pair<std::string, std::string>> port_launch, port_pending;

void apply_settings(const nlohmann::json& settings, bool live) {
    if (!settings.is_object()) return;
    for (const auto& [key, value] : settings.items()) {
        if (!value.is_string()) continue;
        if (key.rfind("port.", 0) == 0) {
            std::lock_guard lock(port_mutex);
            (live ? port_pending : port_launch).emplace_back(key.substr(5), value.get<std::string>());
            __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s%s", key.c_str() + 5,
                                value.get<std::string>().c_str(), live ? " (live)" : "");
            continue;
        }
    }
}

// The launch arguments for the port settings (see above). Our old cross-game graphics/volume keys and their
// mapping were removed on 2026-11-15 (the port's own options replaced them).
std::vector<std::string> graphics_launch_args() {
    std::vector<std::string> args;
    std::lock_guard lock(port_mutex);
    for (const auto& [name, value] : port_launch) {
        args.emplace_back("--video-launch-set");
        args.emplace_back(name + "=" + value);
    }
    return args;
}

// Frame thread: the port settings changed in the in-game menu since the last frame.
void publish_port_settings() {
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        todo.swap(port_pending);
    }
    for (const auto& [name, value] : todo) {
        const MdkrVideoKey key = mdkr_video_key_from_name(name.c_str());
        if (key == MDKR_VIDEO_KEY_COUNT) {
            __android_log_print(ANDROID_LOG_WARN, log_tag, "port setting %s: unknown key", name.c_str());
            continue;
        }
        const MdkrVideoRuntimeResult result = mdkr_video_config_runtime_set(key, value.c_str());
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s -> %d", name.c_str(), value.c_str(),
                            static_cast<int>(result));
    }
}

} // namespace

PLM_EXPORT void plm_apply_settings(const char* json) {
    if (json == nullptr) return;
    apply_settings(nlohmann::json::parse(json, nullptr, false), true);
}

PLM_EXPORT uint32_t plm_abi_version() {
    return PLM_ABI_VERSION;
}

PLM_EXPORT void plm_set_host(const plm_host_api* api) {
    host = api;
}

// The host already asked the player. The port handles SDL_QUIT itself: it saves its config and exits.
PLM_EXPORT void plm_request_quit() {
    set_paused(false);
    SDL_Event event{};
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

PLM_EXPORT void plm_pause() {
    set_paused(true);
}

PLM_EXPORT void plm_resume() {
    set_paused(false);
}

// Every frame the port presents passes here (-Wl,--wrap in module.cmake).
extern "C" void __wrap_SDL_GL_SwapWindow(SDL_Window* window) {
    __real_SDL_GL_SwapWindow(window);
    if (host != nullptr && host->first_frame != nullptr && !first_frame_sent.exchange(true)) {
        host->first_frame(host->context);
    }
    publish_port_settings();
    std::unique_lock lock(pause_mutex);
    pause_cv.wait(lock, [] { return !paused; });
}

// Remember the port's audio device, so a pause can silence it.
extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                                        SDL_AudioSpec* obtained, int allowed_changes) {
    const SDL_AudioDeviceID id = __real_SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
    if (!iscapture) audio_device = id;
    return id;
}

PLM_EXPORT int plm_main(int argc, char** argv) {
    forward_stdio_to_logcat();
    if (argc < 2) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "plm_main: no launch.json argument");
        return 2;
    }
    std::ifstream in(argv[1]);
    const nlohmann::json launch = nlohmann::json::parse(in, nullptr, false);
    if (launch.is_discarded() || !launch.contains("dirs") || !launch.contains("rom")) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "plm_main: unreadable launch.json %s", argv[1]);
        return 2;
    }
    const std::string config_dir = launch["dirs"].value("config", "");
    const std::string save_dir = launch["dirs"].value("save", "");
    const std::string rom = launch.value("rom", "");
    if (launch.contains("settings")) apply_settings(launch["settings"], false);

    std::error_code ec;
    std::filesystem::create_directories(save_dir, ec);
    if (chdir(save_dir.c_str()) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "plm_main: can't enter %s", save_dir.c_str());
    }
    __android_log_print(ANDROID_LOG_INFO, log_tag, "save=%s rom=%s", save_dir.c_str(), rom.c_str());
    (void)config_dir;

    std::vector<std::string> args{argv[0], "--rom", rom};
    for (auto& arg : graphics_launch_args()) args.push_back(arg);
    std::vector<const char*> c_args;
    for (auto& arg : args) c_args.push_back(arg.c_str());
    c_args.push_back(nullptr);
    SDL_SetMainReady();
    return game_main(static_cast<int>(args.size()), c_args.data());
}
