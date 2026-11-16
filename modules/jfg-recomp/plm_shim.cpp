// Port-launcher module entry points for Jet Force Gemini: Recompiled (docs/MODULE_SPEC.md §4).
// Compiled into JFG's game library by module.cmake.

#include "plm/atomic_file.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>
#include <android/log.h>

#include "json/json.hpp"
#include "plm/module_abi.h"

struct recomp_context;

namespace ultramodern {
void quit(); // N64ModernRuntime (librecomp)
}

extern "C" int SDL_main(int argc, char** argv); // JFG's main(), renamed by SDL_main.h
namespace jfg::settings {
void set(const std::string& key, int value); // src/jfg/jfg_settings_api.h: applies, pushes to RT64 and saves
}
extern "C" void __real_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);

#define PLM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr const char* log_tag = "plm-jfg";

const plm_host_api* host = nullptr;
std::atomic<bool> first_frame_sent{false};

// JFG's locale ids (assets/locales/<id>.json) for the launcher's language tags.
std::string jfg_language(const std::string& locale) {
    const std::string lang = locale.substr(0, locale.find_first_of("-_"));
    if (locale.rfind("pt-BR", 0) == 0 || locale.rfind("pt_BR", 0) == 0) return "portuguese_br";
    static const std::pair<const char*, const char*> table[] = {
        {"de", "german"}, {"fr", "french"}, {"es", "spanish"}, {"it", "italian"}, {"nl", "dutch"},
        {"ja", "japanese"}, {"pl", "polish"}, {"sv", "swedish"},
    };
    for (const auto& [tag, id] : table) {
        if (lang == tag) return id;
    }
    return "english";
}

// Answers JFG's first-run language question from the launcher's locale; an existing choice is kept.
void preset_language(const std::filesystem::path& config_dir, const std::string& locale) {
    const auto file = config_dir / "jfg_language.txt";
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        return;
    }
    std::ofstream(file) << "language=" << jfg_language(locale) << "\n";
}

} // namespace

// ---- The port's own settings (settings_schema.json; docs/planning/MENU.md, design of 2026-11-14) -----------------
//
// tools/schema/jfg_schema.py generates the schema from JFG's own option tables; keys arrive as "port.<key>" with the
// value as a string. Two kinds of key, told apart by a dot, each with ONE generic setter (no per-option code):
// - "<option>": a jfg::settings option (int). At launch it goes into jfg_port_options.txt (key=value lines, which
//   JFG's load_options reads at its first tick) before SDL_main, so restart-only options like Game Frame Rate apply
//   this run. A brand-new player has no options file yet: writing one would skip JFG's "new player starts on the
//   Modern profile" (start_new_player_profile looks for the file), so then the values go through
//   jfg::settings::set at the first frame instead, after Modern applied. Live changes: jfg::settings::set at the
//   next VI swap (applies, pushes render options, saves; what the port's own menu calls).
// - "<file>.<key>": one of the recomp's own options (volume, sticks, rumble, resolution, MSAA), stored in
//   <config dir>/<file>.json; the value is the JSON value the recomp stores (an int if it is one, else a string such
//   as "Original2x"). Written into the file, at launch and live; the recomp reads it at the next start.
// The launcher sends only what the player set. A key we wrote earlier that isn't sent any more goes back to the
// port's default if the port still holds our value (plm-port-settings.json remembers what we wrote): removed from
// <file>.json, or set to the schema's default (the Modern profile's value) in the options file.
namespace {
std::mutex port_mutex;
std::vector<std::pair<std::string, std::string>> port_pending;    // live, and first-frame values for a new player
std::vector<std::pair<std::string, std::string>> port_launch;
std::filesystem::path port_config_dir;
std::filesystem::path port_data_dir;

nlohmann::json read_json_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json::object();
    return j.is_object() ? j : nlohmann::json::object();
}

void write_json_file(const std::filesystem::path& path, const nlohmann::json& j) {
    plm::write_file_atomic(path.string(), j.dump(4));
}

bool is_int(const std::string& s, int& out) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

nlohmann::json file_value(const std::string& value) {
    int n;
    return is_int(value, n) ? nlohmann::json(n) : nlohmann::json(value);
}

// The schema's defaults (data/settings_schema.json), for taking back a jfg::settings value we no longer send.
std::map<std::string, std::string> schema_defaults() {
    std::map<std::string, std::string> out;
    const nlohmann::json schema = read_json_file(port_data_dir / "settings_schema.json");
    if (schema.contains("options") && schema["options"].is_array()) {
        for (const auto& o : schema["options"]) {
            if (o.is_object() && o.contains("key") && o.contains("default") && o["default"].is_string()) {
                out[o["key"].get<std::string>()] = o["default"].get<std::string>();
            }
        }
    }
    return out;
}

// jfg_port_options.txt as ordered (key, value) lines, the way JFG's options_text writes it.
std::vector<std::pair<std::string, std::string>> read_options_file(const std::filesystem::path& path) {
    std::vector<std::pair<std::string, std::string>> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) lines.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return lines;
}

void set_option_line(std::vector<std::pair<std::string, std::string>>& lines, const std::string& key, const std::string& value) {
    for (auto& [k, v] : lines) {
        if (k == key) {
            v = value;
            return;
        }
    }
    lines.emplace_back(key, value);
}

// Before SDL_main: everything we can write into the port's files, and what to take back.
void write_launch_port_settings() {
    const std::filesystem::path options_path = port_config_dir / "jfg_port_options.txt";
    const std::filesystem::path memo_path = port_config_dir / "plm-port-settings.json";
    std::error_code ec;
    const bool new_player = !std::filesystem::exists(options_path, ec) &&
                            !std::filesystem::exists(port_config_dir / "jfg_profile.txt", ec);
    auto options = read_options_file(options_path);
    bool options_changed = false;
    std::map<std::string, nlohmann::json> files;
    std::map<std::string, bool> files_changed;
    auto file_of = [&](const std::string& name) -> nlohmann::json& {
        auto it = files.find(name);
        if (it == files.end()) it = files.emplace(name, read_json_file(port_config_dir / (name + ".json"))).first;
        return it->second;
    };
    const nlohmann::json memo = read_json_file(memo_path);
    nlohmann::json new_memo = nlohmann::json::object();
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        todo = port_launch;
    }
    for (const auto& [key, value] : todo) {
        const size_t dot = key.find('.');
        if (dot != std::string::npos) {
            nlohmann::json& file = file_of(key.substr(0, dot));
            file[key.substr(dot + 1)] = file_value(value);
            files_changed[key.substr(0, dot)] = true;
            new_memo[key] = file_value(value);
        } else {
            int n;
            if (!is_int(value, n)) continue;
            if (new_player) {
                std::lock_guard lock(port_mutex);
                port_pending.emplace_back(key, value); // after the Modern profile, at the first frame
            } else {
                set_option_line(options, key, value);
                options_changed = true;
            }
            new_memo[key] = value;
        }
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s", key.c_str(), value.c_str());
    }
    std::map<std::string, std::string> defaults;
    for (const auto& [key, written] : memo.items()) {
        if (new_memo.contains(key)) continue;
        const size_t dot = key.find('.');
        if (dot != std::string::npos) {
            nlohmann::json& file = file_of(key.substr(0, dot));
            const std::string field = key.substr(dot + 1);
            if (file.contains(field) && file[field] == written) {
                file.erase(field);
                files_changed[key.substr(0, dot)] = true;
            }
        } else {
            if (defaults.empty()) defaults = schema_defaults();
            for (auto& [k, v] : options) {
                if (k == key && written.is_string() && v == written.get<std::string>() && defaults.count(key)) {
                    v = defaults[key];
                    options_changed = true;
                }
            }
        }
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s: back to the port's default", key.c_str());
    }
    for (const auto& [name, dirty] : files_changed) {
        if (dirty) write_json_file(port_config_dir / (name + ".json"), files[name]);
    }
    if (options_changed) {
        std::string text;
        for (const auto& [k, v] : options) text += k + "=" + v + "\n";
        plm::write_file_atomic(options_path, text);
    }
    write_json_file(memo_path, new_memo);
}

// Game thread, at a VI swap: live changes (and a new player's launch values).
void apply_pending_port_settings() {
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        if (port_pending.empty()) return;
        todo.swap(port_pending);
    }
    const std::filesystem::path memo_path = port_config_dir / "plm-port-settings.json";
    nlohmann::json memo = read_json_file(memo_path);
    for (const auto& [key, value] : todo) {
        const size_t dot = key.find('.');
        if (dot != std::string::npos) {
            const std::filesystem::path path = port_config_dir / (key.substr(0, dot) + ".json");
            nlohmann::json file = read_json_file(path);
            file[key.substr(dot + 1)] = file_value(value);
            write_json_file(path, file);
            memo[key] = file_value(value);
        } else {
            int n;
            if (!is_int(value, n)) continue;
            jfg::settings::set(key, n);
            memo[key] = value;
        }
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s (in game)", key.c_str(), value.c_str());
    }
    write_json_file(memo_path, memo);
}

void queue_settings(const nlohmann::json& settings, bool live) {
    if (!settings.is_object()) return;
    std::lock_guard lock(port_mutex);
    for (const auto& [key, value] : settings.items()) {
        if (key.rfind("port.", 0) != 0 || !value.is_string()) continue;
        (live ? port_pending : port_launch).emplace_back(key.substr(5), value.get<std::string>());
    }
}
} // namespace

// The host menu changed settings (a JSON object of key -> value); applied at the next frame.
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

// The player already confirmed in the host's dialog. SDL_QUIT would make JFG ask again (its own quit prompt once
// the game runs), so stop the runtime directly, as that prompt does after its "yes". Thread-safe.
PLM_EXPORT void plm_request_quit() {
    ultramodern::quit();
}

// Every frame the game hands to the video interface passes here (-Wl,--wrap in module.cmake).
extern "C" void __wrap_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    __real_osViSwapBuffer_recomp(rdram, ctx);
    if (host != nullptr && host->first_frame != nullptr && !first_frame_sent.exchange(true)) {
        host->first_frame(host->context);
    }
    apply_pending_port_settings();
}

PLM_EXPORT int plm_main(int argc, char** argv) {
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
    const std::string data_dir = launch["dirs"].value("data", "");
    const std::string config_dir = launch["dirs"].value("config", "");
    const std::string rom = launch.value("rom", "");
    const bool direct = launch.value("ui", "direct") == "direct";

    // JFG finds its launcher files through APP_PROGRAM_PATH and keeps saves, settings and mods in APP_FOLDER_PATH.
    setenv("APP_PROGRAM_PATH", data_dir.c_str(), 1);
    setenv("APP_FOLDER_PATH", config_dir.c_str(), 1);
    setenv("HOME", config_dir.c_str(), 1);
    preset_language(config_dir, launch.value("locale", "en"));
    port_config_dir = config_dir;
    port_data_dir = data_dir;
    if (launch.contains("settings")) queue_settings(launch["settings"], false);
    write_launch_port_settings();
    __android_log_print(ANDROID_LOG_INFO, log_tag, "data=%s config=%s rom=%s ui=%s", data_dir.c_str(),
                        config_dir.c_str(), rom.c_str(), direct ? "direct" : "native");

    // --rom validates and stores the ROM, so JFG's own ROM picker is never needed. Direct boot (MODULE_SPEC §4)
    // skips JFG's launcher; new players start on JFG's Modern profile.
    std::vector<std::string> args{argv[0], "--rom", rom};
    if (direct) {
        args.emplace_back("--skip-launcher");
    } else {
        args.emplace_back("--launcher");
    }
    std::vector<char*> c_args;
    for (auto& arg : args) {
        c_args.push_back(arg.data());
    }
    c_args.push_back(nullptr);
    return SDL_main(static_cast<int>(args.size()), c_args.data());
}
