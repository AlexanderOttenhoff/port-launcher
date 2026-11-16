// Port-launcher module entry points for the Perfect Dark port (docs/MODULE_SPEC.md §4).
// Compiled into the game library by module.cmake.
//
// The port reads the ROM at runtime from its base dir (pd.ntsc-final.z64) and keeps its config and saves in its
// save dir; both come from launch.json. It quits through SDL_QUIT (saves its config, then exit()).

#include "plm/atomic_file.h"
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <map>
#include <utility>
#include <vector>

#include <SDL.h>
#include <android/log.h>
#include <unistd.h>

#include "nlohmann/json.hpp"
#include "plm/module_abi.h"

extern "C" int pd_main(int argc, const char** argv) __asm__("main");
extern "C" void __real_SDL_GL_SwapWindow(SDL_Window* window);
extern "C" SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                                        SDL_AudioSpec* obtained, int allowed_changes);

#define PLM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr const char* log_tag = "plm-pd";

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

// Our two additions, in PD's settings schema as "port.Plm.SteadyAim" / "port.Plm.WheelPause" (they replaced the
// hand-made pd.* entries, 2026-11-15). Both are flags the game reads each time it needs them (wheel pause when the
// weapon wheel opens, steady aim every input tick), so they're safe to set from any thread.
extern "C" void amPlmSetWheelPause(int on);   // patch 0005 (activemenu.c)
extern "C" void bmovePlmSetSteadyAim(int on); // patch 0006 (bondmove.c)

// The port's own settings (settings_schema.json from tools/schema/pd_schema.py; docs/planning/MENU.md, design of
// 2026-11-14) arrive as "port.<Section.Key>", the port's own pd.ini keys. One generic setter for all of them, with no
// patch to the port: the values are written as a small ini file in the port's own format and handed to its public
// configLoad(), which sets each registered variable by key, clamped to its registered range, exactly like loading
// pd.ini. At launch that happens right after the port's configInit() (wrapped, -Wl,--wrap in module.cmake: every
// option is registered by PD_CONSTRUCTORs and pd.ini is loaded, but video/input/audio aren't initialised yet), so
// restart-only options (MSAA, framebuffer effects) apply this run too. Live changes go the same way on the frame
// thread (the SDL_GL_SwapWindow wrapper), where the game reads these variables.
// The port saves every variable into pd.ini at exit, so what we set persists. The launcher sends only what the player
// set; plm-port-settings.json remembers what we set, and a remembered key that isn't sent any more is set back to the
// schema's default (the port's), so a reset in our menu really resets.
extern "C" int32_t configLoad(const char* fname); // port/src/config.c
extern "C" void __real_configInit(void);

namespace {
std::mutex port_mutex;
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

// Hands (Section.Key, value) pairs to the port's configLoad through an ini file in its format.
void port_config_set(const std::vector<std::pair<std::string, std::string>>& values) {
    if (values.empty()) return;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> sections;
    for (const auto& [key, value] : values) {
        const size_t dot = key.rfind('.');
        if (dot == std::string::npos || value.find_first_of("\r\n") != std::string::npos) continue;
        sections[key.substr(0, dot)].emplace_back(key.substr(dot + 1), value);
    }
    const std::string path = port_config_dir + "/plm-port-settings.ini";
    {
        std::ofstream out(path, std::ios::trunc);
        for (const auto& [section, entries] : sections) {
            out << "[" << section << "]\n";
            for (const auto& [k, v] : entries) out << k << "=" << v << "\n";
        }
    }
    configLoad(path.c_str());
    std::remove(path.c_str());
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

// Applies values and keeps the memo of what we set; at launch, also takes back what isn't sent any more.
void apply_port_values(std::vector<std::pair<std::string, std::string>> values, bool launch) {
    const std::string memo_path = port_config_dir + "/plm-port-settings.json";
    nlohmann::json memo = read_json_file(memo_path);
    nlohmann::json new_memo = launch ? nlohmann::json::object() : memo;
    for (const auto& [key, value] : values) new_memo[key] = value;
    if (launch) {
        std::map<std::string, std::string> defaults;
        for (const auto& [key, old] : memo.items()) {
            if (new_memo.contains(key)) continue;
            if (defaults.empty()) defaults = schema_defaults();
            if (defaults.count(key)) {
                values.emplace_back(key, defaults[key]);
                __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s: back to the default", key.c_str());
            }
        }
    }
    for (const auto& [key, value] : values)
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s%s", key.c_str(), value.c_str(), launch ? "" : " (live)");
    port_config_set(values);
    write_json_file(memo_path, new_memo);
}

void apply_pending_port_settings() {
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        if (port_pending.empty()) return;
        todo.swap(port_pending);
    }
    apply_port_values(todo, false);
}
} // namespace

// configInit(): every option registered, pd.ini loaded, nothing initialised from them yet.
extern "C" void __wrap_configInit(void) {
    __real_configInit();
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        todo = port_launch;
    }
    apply_port_values(todo, true);
}

namespace {
void apply_settings(const nlohmann::json& settings, bool live) {
    if (!settings.is_object()) return;
    for (const auto& [key, value] : settings.items()) {
        if (!value.is_string()) continue;
        const std::string text = value.get<std::string>();
        if (key == "port.Plm.WheelPause" || key == "port.Plm.SteadyAim") {
            // Not pd.ini keys: caught here, before the generic ini setter (which would warn and remember them).
            if (key == "port.Plm.WheelPause") amPlmSetWheelPause(text == "1" ? 1 : 0);
            else bmovePlmSetSteadyAim(text == "1" ? 1 : 0);
            __android_log_print(ANDROID_LOG_INFO, log_tag, "setting %s = %s", key.c_str(), text.c_str());
            continue;
        }
        if (key.rfind("port.", 0) == 0) {
            std::lock_guard lock(port_mutex);
            (live ? port_pending : port_launch).emplace_back(key.substr(5), value.get<std::string>());
            continue;
        }
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
    std::unique_lock lock(pause_mutex);
    pause_cv.wait(lock, [] { return !paused; });
    lock.unlock();
    apply_pending_port_settings();
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
    port_config_dir = save_dir; // the port's cwd and pd.ini folder; our memo and temp ini live there too
    port_data_dir = launch["dirs"].value("data", "");
    if (launch.contains("settings")) apply_settings(launch["settings"], false);

    // The port looks for <basedir>/pd.ntsc-final.z64: point a link there at the launcher's imported ROM.
    namespace fs = std::filesystem;
    const fs::path base_dir = fs::path(config_dir) / "data";
    const fs::path rom_link = base_dir / "pd.ntsc-final.z64";
    std::error_code ec;
    fs::create_directories(base_dir, ec);
    fs::create_directories(save_dir, ec);
    fs::remove(rom_link, ec);
    fs::create_symlink(rom, rom_link, ec);
    if (ec) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "can't link the ROM: %s", ec.message().c_str());
        return 2;
    }
    if (chdir(save_dir.c_str()) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "plm_main: can't enter %s", save_dir.c_str());
    }
    __android_log_print(ANDROID_LOG_INFO, log_tag, "base=%s save=%s rom=%s", base_dir.c_str(), save_dir.c_str(), rom.c_str());

    std::vector<std::string> args{argv[0], "--basedir", base_dir.string(), "--savedir", save_dir,
                                  // Android only has OpenGL ES; the port supports ES 3.0 as a fallback profile.
                                  "--gl-version", "3.0es",
                                  // The port's Linux crash handler isn't built on Android.
                                  "--no-crash-handler"};
    std::vector<const char*> c_args;
    for (auto& arg : args) c_args.push_back(arg.c_str());
    c_args.push_back(nullptr);
    SDL_SetMainReady();
    return pd_main(static_cast<int>(args.size()), c_args.data());
}
