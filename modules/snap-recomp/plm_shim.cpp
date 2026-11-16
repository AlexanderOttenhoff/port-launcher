// Port-launcher module entry points for Snap64 Recomp (Pokemon Snap; docs/MODULE_SPEC.md §4). Compiled into the
// game library by module.cmake. Why not the shared RecompFrontend shim: see module.cmake.
//
// How Snap64 Recomp finds its files on Android (upstream src/paths.cpp): one game folder, named by SNAP_DATA_DIR,
// holds the ROM (pokemonsnap.z64), snapsettings.json, saves/, mods/, mods.json, the log (snap64.log) and the files the
// port ships beside its executable (menu_text/, gamecontrollerdb.txt, cache/rt64-seen-shaders.bin), which upstream's
// own activity copies in from the APK's assets when they're missing. We do the same: SNAP_DATA_DIR is the launcher's
// writable config dir, and the shipped files come from our read-only data dir.

#include "plm/atomic_file.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <SDL.h>
#include <android/log.h>

#include "json/json.hpp"
#include "plm/module_abi.h"

struct recomp_context;

namespace ultramodern {
void quit(); // N64ModernRuntime (ultramodern)
}

extern "C" int SDL_main(int argc, char** argv); // Snap64's main(), renamed by SDL_main.h on Android
extern "C" void __real_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void __real__ZN11ultramodern14error_handling11message_boxEPKc(const char* msg);

#define PLM_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

#ifndef PLM_LOG_TAG
#error "module.cmake must define PLM_LOG_TAG"
#endif
constexpr const char* log_tag = PLM_LOG_TAG;

const plm_host_api* host = nullptr;
std::atomic<bool> first_frame_sent{false};

// Host menu pause (plm_pause/plm_resume): the game thread waits at its next VI swap; the audio queue runs dry, so
// the game goes quiet too.
std::mutex pause_mutex;
std::condition_variable pause_cv;
bool paused = false;

void set_paused(bool value) {
    {
        std::lock_guard lock(pause_mutex);
        paused = value;
    }
    pause_cv.notify_all();
}

void send_first_frame() {
    if (host != nullptr && host->first_frame != nullptr && !first_frame_sent.exchange(true)) {
        host->first_frame(host->context);
    }
}

namespace fs = std::filesystem;

// Upstream's activity copies its shipped files into the game folder "when they are missing" (Snap64Activity.java);
// same rule here, so a player's edited gamecontrollerdb.txt or a grown seen-shader list isn't overwritten.
void copy_missing(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        const fs::path dest = to / fs::relative(it->path(), from, ec);
        if (it->is_directory(ec)) {
            fs::create_directories(dest, ec);
        } else if (!fs::exists(dest, ec)) {
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(it->path(), dest, ec);
        }
    }
}

// The launcher installs mods into dirs.mods; librecomp reads <game folder>/mods. Make the latter a link to the
// former, moving over anything already there (same as the shared shim).
void link_mods_dir(const fs::path& runtime_dir, const std::string& launcher_dir) {
    if (launcher_dir.empty()) return;
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

// The launcher owns which mods are on (MODULE_SPEC §4.1 "mods"): librecomp's mods.json (enabled_mods, mod_order),
// with every installed id in mod_order so a mod that's enabled by default stays off when the launcher says off.
void apply_mods(const nlohmann::json& launch, const fs::path& game_dir) {
    if (!launch.contains("mods") || !launch["mods"].is_object()) return;
    const auto& mods = launch["mods"];
    const fs::path path = game_dir / "mods.json";
    nlohmann::json config = nlohmann::json::object();
    if (std::ifstream existing(path); existing) {
        config = nlohmann::json::parse(existing, nullptr, false);
        if (!config.is_object()) config = nlohmann::json::object();
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
    if (!config.contains("latest_game_mode")) config["latest_game_mode"] = "";
    plm::write_file_atomic(path, config.dump(4));
    __android_log_print(ANDROID_LOG_INFO, log_tag, "mods: %zu enabled", config["enabled_mods"].size());
}

} // namespace

PLM_EXPORT uint32_t plm_abi_version() {
    return PLM_ABI_VERSION;
}

PLM_EXPORT void plm_set_host(const plm_host_api* api) {
    host = api;
}

// The host already asked the player; stop the runtime directly. A paused game thread is released first so it can
// wind down. main() then saves snapsettings.json if dirty and returns.
PLM_EXPORT void plm_request_quit() {
    set_paused(false);
    ultramodern::quit();
}

PLM_EXPORT void plm_pause() {
    set_paused(true);
}

PLM_EXPORT void plm_resume() {
    set_paused(false);
}

// No canonical or port settings yet: Snap64 keeps its own settings in snapsettings.json and its own Options pages
// ("Own UI" in docs/planning/GAME_STATUS.md). Logged so a test sees the call arrive.
PLM_EXPORT void plm_apply_settings(const char* json) {
    __android_log_print(ANDROID_LOG_INFO, log_tag, "settings ignored (own UI): %s", json != nullptr ? json : "");
}

// Every frame the game hands to the video interface passes here (-Wl,--wrap in module.cmake).
extern "C" void __wrap_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    __real_osViSwapBuffer_recomp(rdram, ctx);
    send_first_frame();
    std::unique_lock lock(pause_mutex);
    pause_cv.wait(lock, [] { return !paused; });
}

// A runtime error box (and Snap's own fatal boxes, which go through the same callback) would sit hidden behind the
// host's loading screen: log it and take the screen down first.
extern "C" void __wrap__ZN11ultramodern14error_handling11message_boxEPKc(const char* msg) {
    __android_log_print(ANDROID_LOG_ERROR, log_tag, "message box: %s", msg != nullptr ? msg : "");
    send_first_frame();
    __real__ZN11ultramodern14error_handling11message_boxEPKc(msg);
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

    // The one game folder (see the top of this file). Snap64 chdir()s into it itself.
    setenv("SNAP_DATA_DIR", config_dir.c_str(), 1);
    // Our menu owns Select/Back in direct mode (patch 0002 reads this; MODULE_SPEC: no port menus from game input).
    // When the player opened the port's own launcher instead (launch.json "ui": "native"), its Options stay on Select.
    if (launch.value("ui", "direct") == "direct") setenv("SNAP_NO_SELECT_MENU", "1", 1);
    copy_missing(data_dir, config_dir);
    link_mods_dir(fs::path(config_dir) / "mods", launch["dirs"].value("mods", ""));
    apply_mods(launch, config_dir);
    __android_log_print(ANDROID_LOG_INFO, log_tag, "data=%s game folder=%s rom=%s ui=%s", data_dir.c_str(),
                        config_dir.c_str(), rom.c_str(), direct ? "direct" : "native");

    // --rom: our game patch hands it to the first-run ROM chooser's hook (checked by hash, copied into the game
    // folder once), so the chooser never shows. Snap64 has no launcher screen; --skip-launcher is for symmetry.
    std::vector<std::string> args{argv[0], "--rom", rom};
    if (direct) args.emplace_back("--skip-launcher");
    std::vector<char*> c_args;
    for (auto& arg : args) c_args.push_back(arg.data());
    c_args.push_back(nullptr);
    // SDL's own activity calls SDL_SetMainReady before SDL_main; our host calls plm_main instead, so do it here or
    // Android's SDL refuses SDL_Init.
    SDL_SetMainReady();
    return SDL_main(static_cast<int>(args.size()), c_args.data());
}
