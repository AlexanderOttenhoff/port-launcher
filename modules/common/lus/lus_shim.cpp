// Port-launcher module entry points for libultraship ports (docs/MODULE_SPEC.md §4): Starship first, then the other
// Harbour Masters ports on SDL2. Compiled into the game library by each module's module.cmake, which defines
// PLM_LOG_TAG and PLM_GAME_MAIN (the port's entry symbol: "main", or "SDL_main" where SDL renames it).
//
// libultraship keeps config, saves, mods and the game's .o2r archives in its app directory (SHIP_HOME, a patch on
// Android) and extracts the game archive from the ROM on first start; with PLM_ROM set the port does that without
// prompts (our patch). The port's own archive (e.g. starship.o2r) ships in the module's data dir and is linked in.

#include "plm/atomic_file.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <android/log.h>
#include <unistd.h>

#include "nlohmann/json.hpp"
#include "plm/module_abi.h"

#ifndef PLM_LOG_TAG
#error "module.cmake must define PLM_LOG_TAG"
#endif
#ifndef PLM_GAME_MAIN
#define PLM_GAME_MAIN "main"
#endif
#ifndef PLM_PORT_ARCHIVE
#define PLM_PORT_ARCHIVE ""
#endif
// Ports that extract when given a ROM path on the command line (Lighthouse) get it only while their game archive
// (PLM_ROM_ARCHIVE) is missing; otherwise they ask whether to extract again.
#ifndef PLM_ROM_ARCHIVE
#define PLM_ROM_ARCHIVE ""
#endif

extern "C" int game_main(int argc, char** argv) __asm__(PLM_GAME_MAIN);
extern "C" void __real_SDL_GL_SwapWindow(SDL_Window* window);
extern "C" SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                                        SDL_AudioSpec* obtained, int allowed_changes);

#define PLM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr const char* log_tag = PLM_LOG_TAG;
const plm_host_api* host = nullptr;
std::atomic<bool> first_frame_sent{false};
std::atomic<SDL_AudioDeviceID> audio_device{0};

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

// The host sets the module's environment before loading it (launch.json "env"); ports read some of it in global
// constructors. Under ARM translation (the emulator) variables set from Java aren't visible here, so the host also
// writes them to <app files>/plm-env; read that first, before the port's constructors (init priority 101).
__attribute__((constructor(101))) void plm_early_env() {
    if (getenv("SHIP_HOME") != nullptr) return;
    std::ifstream cmdline("/proc/self/cmdline");
    std::string process;
    std::getline(cmdline, process, '\0');
    const std::string package = process.substr(0, process.find(':'));
    if (package.empty()) return;
    std::ifstream env("/data/data/" + package + "/files/plm-env");
    std::string line;
    while (std::getline(env, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
    }
}

} // namespace

PLM_EXPORT uint32_t plm_abi_version() {
    return PLM_ABI_VERSION;
}

PLM_EXPORT void plm_set_host(const plm_host_api* api) {
    host = api;
}

// libultraship handles SDL_QUIT itself (saves config, leaves the main loop).
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

// A module's per-frame hook (optional, e.g. modules/ghostship/plm_options.cpp): runs on the thread that presents
// frames, after the port has loaded its config, so pending settings can be applied there.
extern "C" void plm_module_frame(void) __attribute__((weak));

// ---- The port's own settings (docs/planning/MENU.md, design of 2026-11-14) ----------------------------------------
//
// settings_schema.json (generated from the port's own menu by tools/schema/lus_schema.py) lists the port's CVars;
// the launcher passes the player's choices as "port.<CVar name>" -> value, at launch (launch.json "settings") and
// live (plm_apply_settings). They're applied here, generically, for every libultraship module: no per-option code.
//
// Which setter: the value's shape, as the generator writes it (lus_schema.py docstring). A '.' means a float
// (CVarSetFloat: float CVars always carry one, "1.0"), an integer means an int (CVarSetInteger), anything else is a
// string (CVarSetString). Why not carry the type in the key? The value is already unambiguous and the keys stay the
// port's own CVar names, readable in logs and in the per-game settings file.
//
// When:
// - at launch, twice. First, before the game starts, the values are written into the port's own config file
//   (PLM_LUS_CONFIG, e.g. shipofharkinian.json, "CVars" tree) if it exists, so options the port reads only at init
//   (MSAA, texture filter, "Needs reload" items) are right on this launch. A first launch has no config yet; writing
//   one from scratch would skip the port's own config-version migration, so then the second step alone covers it.
//   Second, the same values are applied on the frame thread at the first frames, after the port has loaded its config
//   (and CVarSave persists them), like every live change.
// - live: queued from the UI thread, applied on the frame thread before the module's own per-frame work.
// The CVar setters are libultraship's C bridge (consolevariablebridge.h), declared here so this file builds against
// every libultraship revision the family pins (Starship's older tree has the same signatures).
extern "C" {
void CVarSetInteger(const char* name, int32_t value);
void CVarSetFloat(const char* name, float value);
void CVarSetString(const char* name, const char* value);
void CVarSave(void);
// Optional module hooks: whether the port is ready for CVar changes (SoH and 2Ship present frames while extracting,
// before the managers their CVar callbacks need exist), and what to do after one changed (re-run SoH-style ShipInit
// callbacks, push volumes to sequence players, resize the framebuffer...). Both run on the frame thread.
bool plm_lus_port_ready(void) __attribute__((weak));
void plm_lus_cvar_changed(const char* cvar) __attribute__((weak));
}

namespace {

std::mutex port_mutex;
std::vector<std::pair<std::string, std::string>> port_pending;  // CVar name, value

enum class CVarKind { Int, Float, String };

CVarKind kind_of(const std::string& v) {
    if (v.empty()) return CVarKind::String;
    char* end = nullptr;
    std::strtod(v.c_str(), &end);
    if (end == nullptr || *end != '\0') return CVarKind::String;
    return v.find('.') != std::string::npos ? CVarKind::Float : CVarKind::Int;
}

// Splits a settings object: "port.*" keys are queued here; the rest is returned for the module's own handler.
nlohmann::json take_port_settings(const nlohmann::json& settings) {
    nlohmann::json rest = nlohmann::json::object();
    if (!settings.is_object()) return rest;
    std::lock_guard lock(port_mutex);
    for (const auto& [key, value] : settings.items()) {
        if (key.rfind("port.", 0) == 0 && value.is_string()) {
            port_pending.emplace_back(key.substr(5), value.get<std::string>());
        } else {
            rest[key] = value;
        }
    }
    return rest;
}

// Launch step 1: the values into the port's config file before the game reads it (see above).
void write_port_config(const std::string& config_dir) {
#ifdef PLM_LUS_CONFIG
    const std::string path = config_dir + "/" + PLM_LUS_CONFIG;
    std::ifstream in(path);
    if (!in) return;  // first launch: the frame-thread step covers it
    nlohmann::json config = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (!config.is_object()) return;
    std::vector<std::pair<std::string, std::string>> values;
    {
        std::lock_guard lock(port_mutex);
        values = port_pending;
    }
    if (values.empty()) return;
    for (const auto& [cvar, value] : values) {
        // "gSettings.Volume.Master" -> config["CVars"]["gSettings"]["Volume"]["Master"], as libultraship's
        // ConsoleVariable::Save nests it (and LoadFromPath reads it back, typed by the JSON value).
        nlohmann::json* node = &config["CVars"];
        size_t start = 0, dot;
        while ((dot = cvar.find('.', start)) != std::string::npos) {
            nlohmann::json& child = (*node)[cvar.substr(start, dot - start)];
            if (!child.is_object()) child = nlohmann::json::object();
            node = &child;
            start = dot + 1;
        }
        nlohmann::json& leaf = (*node)[cvar.substr(start)];
        switch (kind_of(value)) {
            case CVarKind::Int: leaf = static_cast<int32_t>(std::strtol(value.c_str(), nullptr, 10)); break;
            case CVarKind::Float: leaf = std::strtof(value.c_str(), nullptr); break;
            case CVarKind::String: leaf = value; break;
        }
    }
    plm::write_file_atomic(path, config.dump(4));
    __android_log_print(ANDROID_LOG_INFO, log_tag, "wrote %zu port settings into %s", values.size(), PLM_LUS_CONFIG);
#else
    (void)config_dir;
#endif
}

} // namespace

// Frame thread: applies the queued port settings. Also callable by a module at a better moment of its own startup
// (SpaghettiKart's PortLauncher_BeforeIntro: after config load, before the intro reads its CVars).
extern "C" void plm_lus_apply_port_settings(void) {
    if (plm_lus_port_ready != nullptr && !plm_lus_port_ready()) return;
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        todo.swap(port_pending);
    }
    if (todo.empty()) return;
    for (const auto& [cvar, value] : todo) {
        switch (kind_of(value)) {
            case CVarKind::Int: CVarSetInteger(cvar.c_str(), static_cast<int32_t>(std::strtol(value.c_str(), nullptr, 10))); break;
            case CVarKind::Float: CVarSetFloat(cvar.c_str(), std::strtof(value.c_str(), nullptr)); break;
            case CVarKind::String: CVarSetString(cvar.c_str(), value.c_str()); break;
        }
        if (plm_lus_cvar_changed != nullptr) plm_lus_cvar_changed(cvar.c_str());
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s", cvar.c_str(), value.c_str());
    }
    CVarSave();  // as the port's own menu does after a change: the choice survives a crash or a kill
}

// Live changes from the launcher's in-game menu (UI thread).
PLM_EXPORT void plm_apply_settings(const char* json) {
    if (json == nullptr) return;
    const nlohmann::json settings = nlohmann::json::parse(json, nullptr, false);
    take_port_settings(settings);  // every option is a port CVar since 2026-11-15 (no hand-made keys left)
}

// Every presented frame passes here (-Wl,--wrap in module.cmake): first frame for the host, and the menu pause.
extern "C" void __wrap_SDL_GL_SwapWindow(SDL_Window* window) {
    __real_SDL_GL_SwapWindow(window);
    if (host != nullptr && host->first_frame != nullptr && !first_frame_sent.exchange(true)) {
        host->first_frame(host->context);
    }
    {
        std::unique_lock lock(pause_mutex);
        pause_cv.wait(lock, [] { return !paused; });
    }
    plm_lus_apply_port_settings();  // before the module's own work, so its per-frame code sees the new values
    if (plm_module_frame != nullptr) plm_module_frame();
}

extern "C" SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired,
                                                        SDL_AudioSpec* obtained, int allowed_changes) {
    const SDL_AudioDeviceID id = __real_SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);
    if (!iscapture) audio_device = id;
    return id;
}

// A module's optional pre-start hook (e.g. modules/lighthouse/plm_options.cpp: the camera baseline): called before
// the game starts with whatever launch.json "settings" keys aren't port CVars (none since 2026-11-15).
extern "C" void plm_module_settings(const char* json) __attribute__((weak));

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
    namespace fs = std::filesystem;
    const std::string data_dir = launch["dirs"].value("data", "");
    const std::string config_dir = launch["dirs"].value("config", "");
    const std::string rom = launch.value("rom", "");
    std::error_code ec;
    fs::create_directories(config_dir, ec);

    // The port's own archive (built with the module) next to the game archive in the app directory.
    if (std::string archive = PLM_PORT_ARCHIVE; !archive.empty()) {
        const fs::path link = fs::path(config_dir) / archive;
        fs::remove(link, ec);
        fs::create_symlink(fs::path(data_dir) / archive, link, ec);
    }
    setenv("SHIP_HOME", config_dir.c_str(), 1);
    setenv("PLM_BUNDLE", data_dir.c_str(), 1); // read-only port files: archive, Torch config, asset yamls
    setenv("HOME", config_dir.c_str(), 1);
    setenv("PLM_ROM", rom.c_str(), 1);
    // Android apps have no writable /tmp, and std::filesystem::temp_directory_path() falls back to /data/local/tmp
    // without TMPDIR. Ship of Harkinian's and 2Ship's extractors work in a temp directory, so give them one in the
    // module's own config dir.
    const fs::path tmp = fs::path(config_dir) / "tmp";
    fs::create_directories(tmp, ec);
    setenv("TMPDIR", tmp.c_str(), 1);
    if (chdir(config_dir.c_str()) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "plm_main: can't enter %s", config_dir.c_str());
    }
    __android_log_print(ANDROID_LOG_INFO, log_tag, "data=%s config=%s rom=%s", data_dir.c_str(), config_dir.c_str(), rom.c_str());

    if (launch.contains("settings")) {
        // The port's own settings ("port.*") are ours to apply (above); the module's handler gets the rest.
        const nlohmann::json rest = take_port_settings(launch["settings"]);
        write_port_config(config_dir);
        if (plm_module_settings != nullptr) plm_module_settings(rest.dump().c_str());
    }

    std::vector<std::string> args{argv[0]};
    if (std::string archive = PLM_ROM_ARCHIVE; !archive.empty() && !fs::exists(fs::path(config_dir) / archive)) {
        args.push_back(rom);
    }
    std::vector<char*> c_args;
    for (auto& arg : args) c_args.push_back(arg.data());
    c_args.push_back(nullptr);
    SDL_SetMainReady();
    return game_main(static_cast<int>(args.size()), c_args.data());
}
