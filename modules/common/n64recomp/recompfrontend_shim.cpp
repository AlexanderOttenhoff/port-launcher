// Port-launcher module entry points for RecompFrontend games (docs/MODULE_SPEC.md §4): Banjo-Tooie, DK64, Blast
// Corps, Harvest Moon 64, Snowboard Kids 2 (and the disabled Banjo: Recompiled, Zelda 64: Recompiled), plus the old-UI
// (Zelda64Recomp-style) Mega Man 64 and Bomberman 64, whose UI has the same entry points. Compiled into the game library
// by each module's module.cmake, which defines PLM_LOG_TAG (and PLM_PORT_SCHEMA or PLM_OLDUI_SCHEMA for port settings).
// The game's main() must accept --rom, --skip-launcher and --game-mode (each module's patches add them).

#include "plm/atomic_file.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <android/log.h>
#include <unistd.h>

#include "json/json.hpp"
#include "plm/module_abi.h"

#include "recomp.h"
#include "librecomp/overlays.hpp"
#ifdef PLM_C_STICK_MODIFIER
#include "recompinput/input_mapping.h"
#include "recompinput/input_types.h"
#endif

namespace ultramodern {
void quit(); // N64ModernRuntime (librecomp)
}

// These games define SDL_MAIN_HANDLED, so their entry point stays main(); C++ may not name main, hence the alias.
extern "C" int game_main(int argc, char** argv) __asm__("main");
extern "C" void __real_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);
// ultramodern::error_handling::message_box(const char*), wrapped by mangled name (-Wl,--wrap in module.cmake).
extern "C" void __real__ZN11ultramodern14error_handling11message_boxEPKc(const char* msg);

#define PLM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

#ifndef PLM_LOG_TAG
#error "module.cmake must define PLM_LOG_TAG"
#endif
constexpr const char* log_tag = PLM_LOG_TAG;

const plm_host_api* host = nullptr;
std::atomic<bool> first_frame_sent{false};

// Host menu pause (plm_pause/plm_resume): the game thread waits at its next frame boundary.
std::mutex pause_mutex;
std::condition_variable pause_cv;
bool paused = false;

void wait_while_paused() {
    std::unique_lock lock(pause_mutex);
    pause_cv.wait(lock, [] { return !paused; });
}

void set_paused(bool value) {
    {
        std::lock_guard lock(pause_mutex);
        paused = value;
    }
    pause_cv.notify_all();
}

} // namespace

// Native features, read by the module's own patches through the plm_get_option host function. Ids are shared with the
// patches (e.g. Banjo's plm_native.c).
namespace {
// Native features (Start skips cutscenes = id 0, fast text = id 1) are fixed parts of the modern experience, always
// on. The launcher keys that could turn them off (game.cutscene_skip, game.text_speed) were dropped on 2026-11-15.
std::atomic<uint32_t> native_options[8]{1, 1};

void queue_port_setting(const std::string& key, const std::string& value, bool live); // below (port settings)

void apply_settings(const nlohmann::json& settings, bool live) {
    if (!settings.is_object()) return;
    for (const auto& [key, value] : settings.items()) {
        if (key.rfind("port.", 0) == 0) {
            if (value.is_string()) queue_port_setting(key.substr(5), value.get<std::string>(), live);
            continue;
        }
    }
}
} // namespace

// ---- The port's own settings (settings_schema.json; docs/planning/MENU.md, design of 2026-11-14) -----------------
//
// tools/schema/recompfrontend_schema.py generates the schema from the game's own config registrations; each key is
// "<config id>.<option id>" (the RecompFrontend config tab and the option inside it) and arrives here as
// "port.<config id>.<option id>" with the value as a string in the port's terms (an enum option's key, "true"/"false",
// a number). One generic path for every option, through RecompFrontend's config API (recompui::config::get_config +
// recomp::config::Config), so nothing here knows any specific option:
//
// - At launch the values go into the config files (<config dir>/<config id>.json) right before the game loads them:
//   recompui::config::finalize() is wrapped (-Wl,--wrap in module.cmake), and at that point every tab and option is
//   registered but nothing is loaded yet, so the option's registered type tells us how to write the value (an enum by
//   its key, a whole number as an int, a bool as a bool), and the port then loads it like any value it saved itself,
//   running its own load callbacks (apply_graphics_config etc.). Why the file and not the API after loading:
//   graphics-tab values reach the renderer only through the tab's load/save callbacks, which the port runs on load.
// - Live changes (the in-game menu) go through the API on the game thread at the next VI swap:
//   Config::update_option_value (which runs the option's change callback) and Config::save_config (which applies a
//   requires-confirmation tab's pending values, saves the file and runs the tab's save callback, e.g.
//   apply_graphics_config for resolution / MSAA / framerate).
// - The launcher sends only what the player set (untouched port options aren't sent, so the port keeps its own
//   default and anything it saved itself). A reset in our menu therefore just stops sending the key; to undo what we
//   wrote earlier, plm-port-settings.json remembers every value we wrote, and a remembered key that isn't sent any
//   more is removed from the config file (the port falls back to its default) if the file still holds our value;
//   if the player changed it since (the port's own menu), the player's value stays.
//
// Only modules that define PLM_PORT_SCHEMA (module.cmake, with the finalize wrap) get this. (The earlier
// canonical_map path for the disabled banjo/zelda modules was removed on 2026-11-15.)
#ifdef PLM_PORT_SCHEMA
#include "recompui/config.h"

extern "C" void __real__ZN8recompui6config8finalizeEv();

namespace {
std::mutex port_mutex;
std::vector<std::pair<std::string, std::string>> port_launch, port_pending; // (config.option, value)
std::atomic<bool> port_configs_loaded{false};
std::string port_config_dir;

// "<config id>.<option id>": config ids have no dots (they're file names), option ids might, so split at the first.
bool split_port_key(const std::string& key, std::string& config_id, std::string& option_id) {
    const size_t dot = key.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == key.size()) return false;
    config_id = key.substr(0, dot);
    option_id = key.substr(dot + 1);
    return true;
}

recomp::config::Config* find_config(const std::string& config_id) {
    try {
        return &recompui::config::get_config(config_id);
    } catch (const std::exception&) {
        return nullptr;
    }
}

bool parse_number(const std::string& text, double& out) {
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

// The value as the port's <config id>.json stores it (librecomp Config::get_storage_json), or null if it doesn't fit
// the option. Enum keys are matched case-insensitively like the port's loader, and written in the port's spelling.
nlohmann::json port_json_value(const recomp::config::ConfigOption& option, const std::string& value) {
    using recomp::config::ConfigOptionType;
    switch (option.type) {
    case ConfigOptionType::Enum: {
        const auto& e = std::get<recomp::config::ConfigOptionEnum>(option.variant);
        const auto it = e.find_option_from_string(value);
        return it == e.options.end() ? nlohmann::json() : nlohmann::json(it->key);
    }
    case ConfigOptionType::Number: {
        const auto& n = std::get<recomp::config::ConfigOptionNumber>(option.variant);
        double v;
        if (!parse_number(value, v)) return {};
        return n.precision == 0 ? nlohmann::json(static_cast<int>(v)) : nlohmann::json(v);
    }
    case ConfigOptionType::Bool:
        return nlohmann::json(value == "true" || value == "1" || value == "on");
    case ConfigOptionType::String:
        return nlohmann::json(value);
    default:
        return {};
    }
}

// The same value as the in-memory variant Config::update_option_value takes.
bool port_variant(const recomp::config::ConfigOption& option, const std::string& value,
                  recomp::config::ConfigValueVariant& out) {
    using recomp::config::ConfigOptionType;
    const nlohmann::json j = port_json_value(option, value);
    if (j.is_null()) return false;
    switch (option.type) {
    case ConfigOptionType::Enum: {
        const auto& e = std::get<recomp::config::ConfigOptionEnum>(option.variant);
        out = e.find_option_from_string(j.get<std::string>())->value;
        return true;
    }
    case ConfigOptionType::Number: out = j.get<double>(); return true;
    case ConfigOptionType::Bool: out = j.get<bool>(); return true;
    case ConfigOptionType::String: out = j.get<std::string>(); return true;
    default: return false;
    }
}

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream in(path);
    nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json::object();
    return j.is_object() ? j : nlohmann::json::object();
}

void write_json_file(const std::string& path, const nlohmann::json& j) {
    // Written next to the target and renamed over it, so a crash mid-write can't leave the port a half file.
    plm::write_file_atomic(path, j.dump(4));
}

std::string memo_path() {
    return port_config_dir + "/plm-port-settings.json";
}

// Launch: write the launcher's values into the config files, and take back our earlier values that aren't sent any
// more (see the comment block above). Runs inside the finalize wrapper, before the real finalize loads the files.
void write_launch_port_settings() {
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        todo = port_launch;
    }
    nlohmann::json memo = read_json_file(memo_path());
    nlohmann::json new_memo = nlohmann::json::object();
    std::map<std::string, nlohmann::json> files; // config id -> its json, loaded on first use
    auto file_of = [&](const std::string& config_id) -> nlohmann::json& {
        auto it = files.find(config_id);
        if (it == files.end()) it = files.emplace(config_id, read_json_file(port_config_dir + "/" + config_id + ".json")).first;
        return it->second;
    };
    std::map<std::string, bool> changed;
    for (const auto& [key, value] : todo) {
        std::string config_id, option_id;
        recomp::config::Config* config = split_port_key(key, config_id, option_id) ? find_config(config_id) : nullptr;
        if (config == nullptr || !config->has_option(option_id)) {
            __android_log_print(ANDROID_LOG_WARN, log_tag, "port setting %s: no such option", key.c_str());
            continue;
        }
        const nlohmann::json j = port_json_value(config->get_option(option_id), value);
        if (j.is_null()) {
            __android_log_print(ANDROID_LOG_WARN, log_tag, "port setting %s: bad value '%s'", key.c_str(), value.c_str());
            continue;
        }
        nlohmann::json& file = file_of(config_id);
        if (!file.contains(option_id) || file[option_id] != j) changed[config_id] = true;
        file[option_id] = j;
        new_memo[key] = j;
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s", key.c_str(), j.dump().c_str());
    }
    for (const auto& [key, written] : memo.items()) {
        if (new_memo.contains(key)) continue;
        std::string config_id, option_id;
        if (!split_port_key(key, config_id, option_id)) continue;
        nlohmann::json& file = file_of(config_id);
        if (file.contains(option_id) && file[option_id] == written) {
            file.erase(option_id); // back to the port's default at load
            changed[config_id] = true;
            __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s: back to the port's default", key.c_str());
        }
    }
    for (const auto& [config_id, dirty] : changed) {
        if (dirty) write_json_file(port_config_dir + "/" + config_id + ".json", files[config_id]);
    }
    write_json_file(memo_path(), new_memo);
}

// Game thread, at a VI swap: the values changed in the in-game menu since the last frame.
void apply_live_port_settings() {
    if (!port_configs_loaded.load()) return;
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        if (port_pending.empty()) return;
        todo.swap(port_pending);
    }
    nlohmann::json memo = read_json_file(memo_path());
    std::map<std::string, recomp::config::Config*> touched;
    for (const auto& [key, value] : todo) {
        std::string config_id, option_id;
        recomp::config::Config* config = split_port_key(key, config_id, option_id) ? find_config(config_id) : nullptr;
        if (config == nullptr || !config->has_option(option_id)) {
            __android_log_print(ANDROID_LOG_WARN, log_tag, "port setting %s: no such option", key.c_str());
            continue;
        }
        const recomp::config::ConfigOption& option = config->get_option(option_id);
        recomp::config::ConfigValueVariant v;
        if (!port_variant(option, value, v)) {
            __android_log_print(ANDROID_LOG_WARN, log_tag, "port setting %s: bad value '%s'", key.c_str(), value.c_str());
            continue;
        }
        config->update_option_value(option_id, v);
        touched[config_id] = config;
        memo[key] = port_json_value(option, value);
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s (live)", key.c_str(), value.c_str());
    }
    for (const auto& [config_id, config] : touched) config->save_config();
    write_json_file(memo_path(), memo);
}
} // namespace

// recompui::config::finalize(): every config tab is registered, nothing is loaded yet. Write the launcher's values
// into the files the real finalize is about to load.
extern "C" void __wrap__ZN8recompui6config8finalizeEv() {
    write_launch_port_settings();
    __real__ZN8recompui6config8finalizeEv();
    port_configs_loaded = true;
}

namespace {
void queue_port_setting(const std::string& key, const std::string& value, bool live) {
    std::lock_guard lock(port_mutex);
    (live ? port_pending : port_launch).emplace_back(key, value);
}
} // namespace
#elif defined(PLM_OLDUI_SCHEMA)
// ---- The port's own settings, old-UI recomps (Mega Man 64, Bomberman 64) ------------------------------------------
//
// These copy Zelda64Recomp's pre-RecompFrontend UI: no config API, just hand-written config code (src/game/config.cpp)
// that saves each settings file as plain JSON in the app folder (APP_FOLDER_PATH = our config dir):
// general.json, graphics.json, sound.json (+ cheats.json in BM64), and controls.json (bindings, never touched here).
// tools/schema/oldui_schema.py joins the menu (assets/config_menu/*.rml) with that storage, so each schema key is
// "<file>.<json key>" (e.g. "graphics.msaa_option") and the value is a string in the port's terms (an enum's JSON
// name like "MSAA2X", or digits). One generic path, nothing here knows an option:
//
// - At launch: plm_main writes the values into <config dir>/<file>.json before game_main runs, and the game's
//   zelda64::load_config() (called from its main()) reads them like anything it saved itself. Why the file and no
//   wrap: load_config only runs inside main(), after plm_main, so there's nothing to intercept. A partial file is
//   fine on a first start: every loader falls back per key (from_or_default / call_if_key_exists), and a file that
//   does exist is never reset by the port.
// - Live (the in-game menu): the same write, then the port's own loader for that file, load_<file>_config(path),
//   on the game thread at the next VI swap. Those loaders are plain global functions in config.cpp (not static), so we
//   can call them by name; they run the port's setters (zelda64::set_*, recomp::set_rumble_strength,
//   ultramodern::renderer::set_graphics_config, which the renderer picks up like the port's own Apply button). They
//   are declared weak: a game without a given file's loader (MM64 has no cheats) links fine, and that value then
//   applies at the next start. (The setters dirty RmlUi model variables from the game thread; the port's own menu
//   would refresh from them, and it's hidden in direct mode anyway. MVP: accepted.)
// - JSON types: what the file already holds for that key decides (number / bool / string); with nothing there yet,
//   digits are written as a number and anything else as a string - enum names are never numeric, so that's exact for
//   both games.
// - Resets work like the RecompFrontend path above: plm-port-settings.json remembers what we wrote, and a remembered
//   key the launcher stops sending is removed from its file (the port's default comes back) if the file still holds
//   our value.
bool load_general_config(const std::filesystem::path& path) __attribute__((weak));
bool load_graphics_config(const std::filesystem::path& path) __attribute__((weak));
bool load_sound_config(const std::filesystem::path& path) __attribute__((weak));
bool load_cheats_config(const std::filesystem::path& path) __attribute__((weak));

namespace {
std::mutex port_mutex;
std::vector<std::pair<std::string, std::string>> port_launch, port_pending; // (file.key, value)
std::string port_config_dir;

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream in(path);
    nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json::object();
    return j.is_object() ? j : nlohmann::json::object();
}

void write_json_file(const std::string& path, const nlohmann::json& j) {
    // Next to the target and renamed over it, so a crash mid-write can't leave the port a half file.
    plm::write_file_atomic(path, j.dump(4));
}

std::string memo_path() {
    return port_config_dir + "/plm-port-settings.json";
}

// "<file>.<key>": file stems and the port's json keys are plain identifiers; controls.json (bindings) is refused.
bool split_port_key(const std::string& key, std::string& file, std::string& option) {
    const size_t dot = key.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == key.size()) return false;
    file = key.substr(0, dot);
    option = key.substr(dot + 1);
    return file != "controls" && file.find('/') == std::string::npos && file.find("..") == std::string::npos;
}

nlohmann::json typed_value(const nlohmann::json& existing, const std::string& value) {
    char* end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    const bool numeric = end != value.c_str() && *end == '\0';
    if (existing.is_boolean()) return value == "true" || value == "1" || value == "On";
    if (existing.is_number_integer() && numeric) return static_cast<long long>(number);
    if (existing.is_number() && numeric) return number;
    if (existing.is_string()) return value;
    if (numeric) return value.find('.') == std::string::npos ? nlohmann::json(static_cast<long long>(number))
                                                             : nlohmann::json(number);
    return value;
}

// Writes the values into their files (and, at launch, takes back what we wrote before and isn't sent any more).
// Returns the files that changed.
std::vector<std::string> write_port_values(const std::vector<std::pair<std::string, std::string>>& todo, bool launch) {
    nlohmann::json memo = read_json_file(memo_path());
    nlohmann::json new_memo = launch ? nlohmann::json::object() : memo;
    std::map<std::string, nlohmann::json> files;
    auto file_of = [&](const std::string& stem) -> nlohmann::json& {
        auto it = files.find(stem);
        if (it == files.end()) it = files.emplace(stem, read_json_file(port_config_dir + "/" + stem + ".json")).first;
        return it->second;
    };
    std::map<std::string, bool> changed;
    for (const auto& [key, value] : todo) {
        std::string stem, option;
        if (!split_port_key(key, stem, option)) {
            __android_log_print(ANDROID_LOG_WARN, log_tag, "port setting %s: not a <file>.<key>", key.c_str());
            continue;
        }
        nlohmann::json& file = file_of(stem);
        const nlohmann::json j = typed_value(file.contains(option) ? file[option] : nlohmann::json(), value);
        if (!file.contains(option) || file[option] != j) changed[stem] = true;
        file[option] = j;
        new_memo[key] = j;
        __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s = %s%s", key.c_str(), j.dump().c_str(),
                            launch ? "" : " (live)");
    }
    if (launch) {
        for (const auto& [key, written] : memo.items()) {
            if (new_memo.contains(key)) continue;
            std::string stem, option;
            if (!split_port_key(key, stem, option)) continue;
            nlohmann::json& file = file_of(stem);
            if (file.contains(option) && file[option] == written) {
                file.erase(option); // the port's from_or_default / reset value comes back at load
                changed[stem] = true;
                __android_log_print(ANDROID_LOG_INFO, log_tag, "port setting %s: back to the port's default", key.c_str());
            }
        }
    }
    std::vector<std::string> out;
    for (const auto& [stem, dirty] : changed) {
        if (!dirty) continue;
        write_json_file(port_config_dir + "/" + stem + ".json", files[stem]);
        out.push_back(stem);
    }
    write_json_file(memo_path(), new_memo);
    return out;
}

void write_launch_port_settings() {
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        todo.swap(port_launch);
    }
    write_port_values(todo, true);
}

// Game thread, at a VI swap: write, then let the port load the changed files itself.
void apply_live_port_settings() {
    std::vector<std::pair<std::string, std::string>> todo;
    {
        std::lock_guard lock(port_mutex);
        if (port_pending.empty()) return;
        todo.swap(port_pending);
    }
    for (const std::string& stem : write_port_values(todo, false)) {
        bool (*loader)(const std::filesystem::path&) = stem == "general" ? load_general_config
            : stem == "graphics" ? load_graphics_config
            : stem == "sound" ? load_sound_config
            : stem == "cheats" ? load_cheats_config
            : nullptr;
        if (loader == nullptr) {
            __android_log_print(ANDROID_LOG_INFO, log_tag, "port settings in %s.json apply at the next start", stem.c_str());
            continue;
        }
        loader(std::filesystem::path(port_config_dir) / (stem + ".json"));
    }
}

void queue_port_setting(const std::string& key, const std::string& value, bool live) {
    std::lock_guard lock(port_mutex);
    (live ? port_pending : port_launch).emplace_back(key, value);
}
} // namespace
#else
namespace {
void queue_port_setting(const std::string&, const std::string&, bool) {}
void apply_live_port_settings() {}
} // namespace
#endif

// Host function for the patches: u32 plm_get_option(u32 id).
extern "C" void plm_get_option(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t id = static_cast<uint32_t>(ctx->r4);
    ctx->r2 = id < std::size(native_options) ? native_options[id].load() : 0;
}

// The host menu changed settings (a JSON object of canonical key → value); applied live.
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

// The host already asked the player; stop the runtime directly (thread-safe). A paused game thread is released
// first, so it can wind down.
PLM_EXPORT void plm_request_quit() {
    set_paused(false);
    ultramodern::quit();
}

// The game thread holds at the next VI swap; the runtime's audio queue runs dry, so it goes quiet.
PLM_EXPORT void plm_pause() {
    set_paused(true);
}

PLM_EXPORT void plm_resume() {
    set_paused(false);
}

static void send_first_frame() {
    if (host != nullptr && host->first_frame != nullptr && !first_frame_sent.exchange(true)) {
        host->first_frame(host->context);
    }
}

// The runtime reports errors such as "Error loading mods" here and then doesn't start the game. Log the message and
// take the host's loading screen down, or the port's error box stays hidden behind it.
extern "C" void __wrap__ZN11ultramodern14error_handling11message_boxEPKc(const char* msg) {
    __android_log_print(ANDROID_LOG_ERROR, log_tag, "message box: %s", msg != nullptr ? msg : "");
    send_first_frame();
    __real__ZN11ultramodern14error_handling11message_boxEPKc(msg);
}

// recompui::show_context(ContextId, std::string_view): the frontend shows one of its own screens (prompts, menus).
// Wrapped by mangled name; ContextId is one uint32_t and string_view is {data, size}, so on AArch64 the arguments
// arrive as (w0, x1, x2). A screen shown before the first game frame, such as a prompt that waits for the player,
// would stay hidden behind the host's loading screen: take it down and log which one it was.
#define PLM_SHOW_CONTEXT _ZN8recompui12show_contextENS_9ContextIdENSt6__ndk117basic_string_viewIcNS1_11char_traitsIcEEEE
#define PLM_CAT(a, b) a##b
#define PLM_WRAP(name) PLM_CAT(__wrap_, name)
#define PLM_REAL(name) PLM_CAT(__real_, name)
extern "C" void PLM_REAL(PLM_SHOW_CONTEXT)(uint32_t context, const char* name, size_t name_size);
extern "C" void PLM_WRAP(PLM_SHOW_CONTEXT)(uint32_t context, const char* name, size_t name_size) {
    __android_log_print(ANDROID_LOG_INFO, log_tag, "frontend shows context %u '%.*s'%s", context,
                        static_cast<int>(name_size), name != nullptr ? name : "",
                        first_frame_sent.load() ? "" : " before the first frame");
    send_first_frame();
    PLM_REAL(PLM_SHOW_CONTEXT)(context, name, name_size);
}

#ifdef PLM_C_STICK_MODIFIER
// C-stick modifier (docs/planning/MENU.md, owner decision): the right stick is the camera; holding R1 turns it into
// the four C buttons. The port already decides each frame whether the stick's C bindings are suppressed for the
// analog camera (and lifts that itself while the ocarina is out); the modifier lifts it too. R1 loses its default
// C-down binding so that holding it does nothing else.
extern "C" void __real__ZN11recompinput27set_right_analog_suppressedEb(bool suppressed);

static bool c_stick_modifier_held() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController* pad = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (pad != nullptr && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) return true;
    }
    return false;
}

extern "C" void __wrap__ZN11recompinput27set_right_analog_suppressedEb(bool suppressed) {
    __real__ZN11recompinput27set_right_analog_suppressedEb(suppressed && !c_stick_modifier_held());
}

// While R1 is held the stick is C buttons only: the camera reads it directly (not through the suppression), so it
// gets no stick input then.
extern "C" void __real__ZN11recompinput16get_right_analogEiPfS0_(int controller, float* x, float* y);
extern "C" void __wrap__ZN11recompinput16get_right_analogEiPfS0_(int controller, float* x, float* y) {
    __real__ZN11recompinput16get_right_analogEiPfS0_(controller, x, y);
    if (c_stick_modifier_held()) {
        if (x != nullptr) *x = 0.0f;
        if (y != nullptr) *y = 0.0f;
    }
}

// New default: C-down on the right stick only (RecompFrontend's default also binds R1). Saved non-custom controller
// profiles keep their bindings in controls.json, so drop those to let them regenerate from the defaults; custom
// profiles (the player's own) stay as they are.
static void set_c_stick_modifier_defaults(const std::string& config_dir) {
    using recompinput::GameInput;
    using recompinput::InputField;
    recompinput::set_default_mapping_for_controller(GameInput::C_DOWN, {
        InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true),
    });
    const std::string path = config_dir + "/controls.json";
    std::ifstream in(path);
    if (!in) return;
    nlohmann::json controls = nlohmann::json::parse(in, nullptr, false);
    in.close();
    if (!controls.is_object() || !controls.contains("profiles") || !controls["profiles"].is_array()) return;
    bool changed = false;
    for (auto& profile : controls["profiles"]) {
        if (profile.value("device", "") == "Controller" && !profile.value("custom", false) && profile.contains("mappings")) {
            profile.erase("mappings");
            changed = true;
        }
    }
    if (changed) {
        plm::write_file_atomic(path, controls.dump(4));
        __android_log_print(ANDROID_LOG_INFO, log_tag, "controller profiles reset to the C-stick modifier defaults");
    }
}
#endif

// The ports print diagnostics to stdout/stderr, which go nowhere on Android: forward them to logcat.
static void forward_stdio_to_logcat() {
    static int fds[2];
    if (pipe(fds) != 0) {
        return;
    }
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

// Every frame the game hands to the video interface passes here (-Wl,--wrap in module.cmake).
extern "C" void __wrap_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    __real_osViSwapBuffer_recomp(rdram, ctx);
    send_first_frame();
    apply_live_port_settings();
    wait_while_paused();
}

// The launcher installs mods into dirs.mods; the runtime reads <config>/mods. Make the latter a link to the former,
// moving over anything the port's own launcher put there.
static void link_mods_dir(const std::string& runtime_dir, const std::string& launcher_dir) {
    namespace fs = std::filesystem;
    if (launcher_dir.empty()) {
        return;
    }
    std::error_code ec;
    fs::create_directories(launcher_dir, ec);
    if (fs::is_symlink(runtime_dir, ec)) {
        if (fs::read_symlink(runtime_dir, ec) == fs::path(launcher_dir)) return;
        fs::remove(runtime_dir, ec);
    } else if (fs::is_directory(runtime_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(runtime_dir, ec)) {
            fs::rename(entry.path(), fs::path(launcher_dir) / entry.path().filename(), ec);
        }
        fs::remove_all(runtime_dir, ec);
    }
    fs::create_directory_symlink(launcher_dir, runtime_dir, ec);
    if (ec) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "can't link %s to %s: %s", runtime_dir.c_str(),
                            launcher_dir.c_str(), ec.message().c_str());
    }
}

// Option values from the launcher's preset (launch.json mods.option_defaults: {mod id: {option: value}}) count as
// defaults: they go into <config>/mod_config/<mod id>.json where the file has no value yet. The shim remembers what
// it wrote (<mod id>.plm-defaults.json), so a later preset can change or drop its own values; a value the player
// changed in the port's mod menu differs from the remembered one and stays. A dropped key falls back to the mod's
// own default.
static void apply_option_defaults(const nlohmann::json& mods, const std::string& config_dir) {
    namespace fs = std::filesystem;
    const nlohmann::json wanted_all = mods.contains("option_defaults") && mods["option_defaults"].is_object()
        ? mods["option_defaults"] : nlohmann::json::object();
    const std::string dir = config_dir + "/mod_config";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto read_json = [](const std::string& path) {
        std::ifstream in(path);
        nlohmann::json j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json::object();
        return j.is_object() ? j : nlohmann::json::object();
    };
    // Every mod we wrote defaults for before, plus every mod with defaults now.
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        const std::string suffix = ".plm-defaults.json";
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            ids.push_back(name.substr(0, name.size() - suffix.size()));
        }
    }
    for (const auto& [mod_id, options] : wanted_all.items()) {
        if (std::find(ids.begin(), ids.end(), mod_id) == ids.end()) ids.push_back(mod_id);
    }
    for (const std::string& mod_id : ids) {
        const std::string path = dir + "/" + mod_id + ".json";
        const std::string written_path = dir + "/" + mod_id + ".plm-defaults.json";
        const nlohmann::json wanted = wanted_all.contains(mod_id) && wanted_all[mod_id].is_object()
            ? wanted_all[mod_id] : nlohmann::json::object();
        const nlohmann::json written = read_json(written_path);
        nlohmann::json file = read_json(path);
        if (file.contains("mod_id") && file["mod_id"] != mod_id) continue; // not ours to touch
        file["mod_id"] = mod_id;
        if (!file.contains("storage") || !file["storage"].is_object()) file["storage"] = nlohmann::json::object();
        auto& storage = file["storage"];
        bool changed = false;
        // Our earlier values that the player didn't change: update or drop them.
        for (const auto& [option, old_value] : written.items()) {
            if (storage.contains(option) && storage[option] == old_value && !wanted.contains(option)) {
                storage.erase(option);
                changed = true;
            } else if (storage.contains(option) && storage[option] == old_value && wanted[option] != old_value) {
                storage[option] = wanted[option];
                changed = true;
            }
        }
        // New defaults where there's no value yet.
        nlohmann::json now_written = nlohmann::json::object();
        for (const auto& [option, value] : wanted.items()) {
            if (!storage.contains(option)) {
                storage[option] = value;
                changed = true;
            }
            if (storage[option] == value) now_written[option] = value;
        }
        if (changed) {
            plm::write_file_atomic(path, file.dump(4));
        }
        if (now_written.empty()) {
            fs::remove(written_path, ec);
        } else {
            plm::write_file_atomic(written_path, now_written.dump(4));
        }
    }
}

// The launcher owns which mods are on (MODULE_SPEC §4.1 "mods"). The runtime keeps that in mods.json: ids in
// enabled_mods load; an installed id missing from mod_order counts as new and turns on if it's enabled by default,
// so every installed id goes into mod_order to keep the launcher's "off" off.
static void apply_mods(const nlohmann::json& launch, const std::string& config_dir) {
    if (!launch.contains("mods") || !launch["mods"].is_object()) {
        return;
    }
    const auto& mods = launch["mods"];
    const std::string path = config_dir + "/mods.json";
    nlohmann::json config = nlohmann::json::object();
    {
        std::ifstream existing(path);
        if (existing) {
            config = nlohmann::json::parse(existing, nullptr, false);
            if (!config.is_object()) {
                config = nlohmann::json::object();
            }
        }
    }
    std::vector<std::string> order;
    if (config.contains("mod_order") && config["mod_order"].is_array()) {
        for (const auto& id : config["mod_order"]) {
            if (id.is_string()) order.push_back(id.get<std::string>());
        }
    }
    for (const auto& id : mods.value("installed", nlohmann::json::array())) {
        if (id.is_string() && std::find(order.begin(), order.end(), id.get<std::string>()) == order.end()) {
            order.push_back(id.get<std::string>());
        }
    }
    config["mod_order"] = order;
    config["enabled_mods"] = mods.value("enabled", nlohmann::json::array());
    if (!config.contains("latest_game_mode")) {
        config["latest_game_mode"] = "";
    }
    plm::write_file_atomic(path, config.dump(4));
    apply_option_defaults(mods, config_dir);
    __android_log_print(ANDROID_LOG_INFO, log_tag, "mods: %zu enabled, game mode '%s'", config["enabled_mods"].size(),
                        mods.value("game_mode", "").c_str());
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
    const std::string data_dir = launch["dirs"].value("data", "");
    const std::string config_dir = launch["dirs"].value("config", "");
    const std::string rom = launch.value("rom", "");
    const bool direct = launch.value("ui", "direct") == "direct";

    // RecompFrontend keeps saves, settings and mods in APP_FOLDER_PATH and loads assets relative to the working
    // directory (its program path is empty on Linux and Android).
    setenv("APP_FOLDER_PATH", config_dir.c_str(), 1);
    setenv("HOME", config_dir.c_str(), 1);
    // Split recomp modules (ON_DEVICE_RECOMP.md) load their game code from a separate libgame.so; the launcher says
    // where (game_loader.cpp falls back to <config dir>/libgame.so). Unsplit modules ignore it.
    if (launch.contains("game_lib")) setenv("PLM_GAME_LIB", launch.value("game_lib", "").c_str(), 1);
    if (chdir(data_dir.c_str()) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, log_tag, "plm_main: can't enter %s", data_dir.c_str());
        return 2;
    }
    __android_log_print(ANDROID_LOG_INFO, log_tag, "data=%s config=%s rom=%s ui=%s", data_dir.c_str(),
                        config_dir.c_str(), rom.c_str(), direct ? "direct" : "native");

#if defined(PLM_PORT_SCHEMA) || defined(PLM_OLDUI_SCHEMA)
    port_config_dir = config_dir;
#endif
    if (launch.contains("settings")) apply_settings(launch["settings"], false);
#ifdef PLM_OLDUI_SCHEMA
    // Old-UI recomps: into the files now; the game's main() -> zelda64::load_config() reads them (block above).
    write_launch_port_settings();
#endif
#ifdef PLM_C_STICK_MODIFIER
    set_c_stick_modifier_defaults(config_dir);
#endif
    recomp::overlays::register_base_export("plm_get_option", plm_get_option);
    link_mods_dir(config_dir + "/mods", launch["dirs"].value("mods", ""));
    apply_mods(launch, config_dir);
    const std::string game_mode = launch.contains("mods") ? launch["mods"].value("game_mode", "") : "";

    std::vector<std::string> args{argv[0], "--rom", rom};
    if (direct) {
        args.emplace_back("--skip-launcher");
        if (!game_mode.empty()) {
            args.emplace_back("--game-mode");
            args.emplace_back(game_mode);
        }
    }
    std::vector<char*> c_args;
    for (auto& arg : args) {
        c_args.push_back(arg.data());
    }
    c_args.push_back(nullptr);
    // With SDL_MAIN_HANDLED the program must declare SDL ready itself; Android's SDL refuses SDL_Init otherwise.
    SDL_SetMainReady();
    return game_main(static_cast<int>(args.size()), c_args.data());
}
