// Port-launcher module entry points for the GoldenEye 007 PC port (docs/MODULE_SPEC.md §4).
// Compiled into the game library by module.cmake.
//
// The port resolves everything through "data/" in its working directory: the ROM (ge007.ntsc-final.z64), the
// sidecars converted from it (pcmodels-*/, pccg-*/), ge007.ini and the ge007.eep save. The shim makes the module's
// config dir that working directory and links the launcher's imported ROM into data/. It quits through SDL_QUIT.

#include "plm/atomic_file.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <map>
#include <vector>

#include <SDL.h>
#include <android/log.h>
#include <unistd.h>

#include "nlohmann/json.hpp"
#include "plm/module_abi.h"

// main.c includes SDL.h, which renames main to SDL_main on Android.
extern "C" int ge_main(int argc, const char** argv) __asm__("SDL_main");
extern "C" void __real_SDL_GL_SwapWindow(SDL_Window* window);
extern "C" int plm_ge_convert(const char* rom_path, const char* meta_path, const char* data_dir, const char* region,
                              char* error, size_t error_size);  // ge_convert.cpp
extern "C" SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                                        SDL_AudioSpec* obtained, int allowed_changes);

#define PLM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr const char* log_tag = "plm-ge007";

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

// The launcher's per-game options (docs/planning/MENU.md). Two kinds:
// - port.Input.AimMode also goes to the override the port re-reads (patch 0004), which stills the sway (0005).
// - The port's own settings (settings_schema.json from tools/schema/goldeneye_schema.py, MENU.md design of
//   2026-11-14), keys "port.<Section.Key>" = the port's own ge007.ini keys. ONE generic setter for all of them, the
//   port's public configSetValue(key, double) (clamped to the registered range, like an ini load; the port's own
//   settings presets use it too). At launch they're applied right after the port's configLoad() (wrapped, -Wl,--wrap
//   in module.cmake: options registered and the ini loaded, video/input not initialised yet), so restart-only options
//   like MSAA apply this run; the port saves its config at exit, so they persist. Live changes are queued and applied
//   on the frame thread (the SDL_GL_SwapWindow wrapper), where the game reads these variables.
//   The launcher sends only what the player set; plm-port-settings.json remembers what we set, and a remembered key
//   that isn't sent any more is set back to the schema's default (the port's), so a reset in our menu really resets.
// The earlier canonical options (audio.master, controls.invert_y, controls.camera_speed through patch 0006's setters)
// are covered by the schema (Audio.MasterVolume, Input.PadLookInvertY, Input.PadLookSensX/Y) and gone from here.
extern "C" void inputPlmSetAimMode(int mode);           // patch 0004 (input.c)
extern "C" int configSetValue(const char* key, double v); // port/src/config.c
extern "C" void __real_configLoad(void);

namespace {
std::mutex settings_mutex;
std::vector<std::pair<std::string, std::string>> port_launch, port_pending;
std::string port_config_dir, port_data_dir;

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream in(path);
    nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json::object();
    return j.is_object() ? j : nlohmann::json::object();
}

void write_json_file(const std::string& path, const nlohmann::json& j) {
    plm::write_file_atomic(path, j.dump(4));
}

std::map<std::string, std::string> schema_defaults() {
    std::map<std::string, std::string> out;
    const nlohmann::json schema = read_json_file(port_data_dir + "/settings_schema.json");
    if (schema.contains("options") && schema["options"].is_array()) {
        for (const auto& o : schema["options"]) {
            if (o.is_object() && o.contains("key") && o.contains("default") && o["default"].is_string())
                out[o["key"].get<std::string>()] = o["default"].get<std::string>();
        }
    }
    return out;
}

void apply_port_values(std::vector<std::pair<std::string, std::string>> values, bool launch) {
    const std::string memo_path = port_config_dir + "/plm-port-settings.json";
    const nlohmann::json memo = read_json_file(memo_path);
    nlohmann::json new_memo = launch ? nlohmann::json::object() : memo;
    for (const auto& [key, value] : values) new_memo[key] = value;
    if (launch) {
        std::map<std::string, std::string> defaults;
        for (const auto& [key, old] : memo.items()) {
            if (new_memo.contains(key)) continue;
            if (defaults.empty()) defaults = schema_defaults();
            if (defaults.count(key)) values.emplace_back(key, defaults[key]);
        }
    }
    for (const auto& [key, value] : values) {
        char* end = nullptr;
        const double v = std::strtod(value.c_str(), &end);
        const bool ok = end != value.c_str() && *end == '\0' && configSetValue(key.c_str(), v) == 1;
        __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, log_tag, "port setting %s = %s%s%s", key.c_str(),
                            value.c_str(), launch ? "" : " (live)", ok ? "" : ": not applied");
    }
    write_json_file(memo_path, new_memo);
}

void queue_settings(const nlohmann::json& settings, bool live) {
    if (!settings.is_object()) return;
    std::lock_guard lock(settings_mutex);
    for (const auto& [key, value] : settings.items()) {
        if (!value.is_string()) continue;
        if (key == "port.Input.AimMode") {
            // The port's Aim mode (it replaced our hand-made "Steady aim" entry, 2026-11-15). Centred (1) also stills
            // the standing view's breathing sway, which patch 0005 hangs off this override setter, so it goes
            // through inputPlmSetAimMode (patch 0004) as well as the generic config setter below.
            inputPlmSetAimMode(value.get<std::string>() == "1" ? 1 : 0);
        }
        if (key.rfind("port.", 0) == 0) {
            (live ? port_pending : port_launch).emplace_back(key.substr(5), value.get<std::string>());
        }
    }
}

void apply_pending_settings() {
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(settings_mutex);
        if (port_pending.empty()) return;
        todo.swap(port_pending);
    }
    apply_port_values(todo, false);
}
} // namespace

// configLoad(): every option registered and ge007.ini loaded; nothing initialised from them yet.
extern "C" void __wrap_configLoad(void) {
    __real_configLoad();
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(settings_mutex);
        todo = port_launch;
    }
    apply_port_values(todo, true);
}

PLM_EXPORT void plm_apply_settings(const char* json) {
    if (json == nullptr) return;
    queue_settings(nlohmann::json::parse(json, nullptr, false), true);
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
    apply_pending_settings();
    if (host != nullptr && host->first_frame != nullptr && !first_frame_sent.exchange(true)) {
        host->first_frame(host->context);
    }
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
    const std::string rom = launch.value("rom", "");
    port_config_dir = config_dir; // the port's working directory; our memo lives next to its data/
    port_data_dir = launch["dirs"].value("data", "");
    if (launch.contains("settings")) queue_settings(launch["settings"], false);

    namespace fs = std::filesystem;
    const fs::path data_dir = fs::path(config_dir) / "data";
    const fs::path rom_link = data_dir / "ge007.ntsc-final.z64";
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    fs::remove(rom_link, ec);
    fs::create_symlink(rom, rom_link, ec);
    if (ec) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "can't link the ROM: %s", ec.message().c_str());
        return 2;
    }
    // The port reads models, stage geometry and stage setups from "sidecars" converted from the ROM. Its release
    // builds spawn a Python converter, which Android can't run; ge_convert.cpp is a native port of it. Convert on
    // the first start, and again whenever the converter changes (the stamp).
    constexpr const char* convert_stamp = "ge_convert 1";
    const fs::path stamp_path = data_dir / "plm-convert.stamp";
    std::string stamp;
    if (std::ifstream stamp_in(stamp_path); stamp_in) std::getline(stamp_in, stamp);
    if (stamp != convert_stamp || !fs::exists(data_dir / "pcmodels-ntsc-final/manifest.csv") ||
        !fs::exists(data_dir / "pccg-ntsc-final/manifest.csv")) {
        const std::string meta = launch["dirs"].value("data", "") + "/ge007-convert-meta.txt";
        char error[512] = "";
        const auto t0 = std::chrono::steady_clock::now();
        if (plm_ge_convert(rom.c_str(), meta.c_str(), data_dir.c_str(), "ntsc-final", error, sizeof error) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, log_tag, "converting the ROM's game files failed: %s", error);
            return 2;
        }
        std::ofstream(stamp_path) << convert_stamp << "\n";
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        __android_log_print(ANDROID_LOG_INFO, log_tag, "converted the ROM's game files in %lld ms",
                            static_cast<long long>(ms.count()));
    }

    if (chdir(config_dir.c_str()) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "plm_main: can't enter %s", config_dir.c_str());
        return 2;
    }
    __android_log_print(ANDROID_LOG_INFO, log_tag, "data=%s rom=%s", data_dir.c_str(), rom.c_str());

    // Android only has OpenGL ES; the port's renderer supports ES 3.0 as a profile.
    std::vector<std::string> args{argv[0], "--gl-version", "3.0es"};
    std::vector<const char*> c_args;
    for (auto& arg : args) c_args.push_back(arg.c_str());
    c_args.push_back(nullptr);
    SDL_SetMainReady();
    return ge_main(static_cast<int>(args.size()), c_args.data());
}
