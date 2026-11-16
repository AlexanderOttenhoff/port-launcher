#include <jni.h>

#include <dlfcn.h>
#include <mutex>
#include <string>

#include <android/log.h>

#include "nlohmann/json.hpp"
#include "plm/byte_source.h"
#include "plm/game_db.h"
#include "plm/module_abi.h"
#include "plm/rom_scan.h"
#include "plm/sha1.h"
#include "plm/version.h"

namespace {

constexpr const char* log_tag = "plm-launcher";

JavaVM* java_vm = nullptr;

// Host services for the module running in this (game) process; calls go to static methods of the host activity.
struct HostBridge {
    jclass activity_class = nullptr;
    jmethodID on_first_frame = nullptr;
};
HostBridge host_bridge;

void host_first_frame(void*) {
    JNIEnv* env = nullptr;
    bool attached = false;
    if (java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        java_vm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    env->CallStaticVoidMethod(host_bridge.activity_class, host_bridge.on_first_frame);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (attached) {
        java_vm->DetachCurrentThread();
    }
}

plm_host_api host_api{PLM_HOST_API_VERSION, nullptr, host_first_frame};

std::mutex game_db_mutex;
plm::GameDb game_db;

std::string to_string(JNIEnv* env, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

jstring to_jstring(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

const char* order_name(plm::RomByteOrder order) {
    switch (order) {
    case plm::RomByteOrder::BigEndian: return "z64";
    case plm::RomByteOrder::ByteSwapped: return "v64";
    case plm::RomByteOrder::LittleEndian: return "n64";
    default: return "unknown";
    }
}

nlohmann::json game_json(const plm::GameRecord& game) {
    return {{"id", game.id}, {"title", game.title}, {"region", game.region},
            {"revision", game.revision}, {"modules", game.modules}};
}

} // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    java_vm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL Java_app_portlauncher_core_CoreBridge_coreVersion(JNIEnv* env, jobject) {
    return to_jstring(env, std::string(plm::core_version()));
}

extern "C" JNIEXPORT jstring JNICALL Java_app_portlauncher_core_CoreBridge_sha1Hex(JNIEnv* env, jobject,
                                                                                    jbyteArray data) {
    const jsize length = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    const std::string hex = plm::sha1_hex({reinterpret_cast<const std::uint8_t*>(bytes),
                                           static_cast<std::size_t>(length)});
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return to_jstring(env, hex);
}

// Scans one file (fd stays owned by the caller). Returns {"roms":[...],"error":"..."} as JSON.
extern "C" JNIEXPORT jstring JNICALL Java_app_portlauncher_core_CoreBridge_scanFd(JNIEnv* env, jobject, jint fd,
                                                                                   jstring file_name) {
    plm::FdByteSource source(fd, false);
    nlohmann::json out = {{"roms", nlohmann::json::array()}, {"error", ""}};
    if (!source.valid()) {
        out["error"] = "file can't be read randomly (not a regular file)";
        return to_jstring(env, out.dump());
    }
    const auto result = plm::scan_file(source, to_string(env, file_name));
    out["error"] = result.error;
    for (const auto& rom : result.roms) {
        out["roms"].push_back({{"zipEntry", rom.zip_entry}, {"sha1", rom.sha1}, {"size", rom.size},
                               {"byteOrder", order_name(rom.original_order)}, {"title", rom.header.title},
                               {"gameCode", rom.header.game_code}});
    }
    return to_jstring(env, out.dump());
}

// Exports a ROM as big-endian .z64. Returns its SHA-1, or null on failure.
extern "C" JNIEXPORT jstring JNICALL Java_app_portlauncher_core_CoreBridge_exportRomFd(JNIEnv* env, jobject, jint fd,
                                                                                        jstring zip_entry,
                                                                                        jstring out_path) {
    plm::FdByteSource source(fd, false);
    if (!source.valid()) {
        return nullptr;
    }
    const auto sha = plm::export_rom(source, to_string(env, zip_entry), to_string(env, out_path));
    return sha ? to_jstring(env, *sha) : nullptr;
}

// Adds one module manifest to the game DB. Returns "" or an error message.
extern "C" JNIEXPORT jstring JNICALL Java_app_portlauncher_core_CoreBridge_gameDbAddManifest(JNIEnv* env, jobject,
                                                                                              jstring manifest) {
    std::lock_guard lock(game_db_mutex);
    std::string error;
    game_db.add_manifest(to_string(env, manifest), error);
    return to_jstring(env, error);
}

// Looks a ROM up by SHA-1. Returns the game as JSON, or null if unknown.
extern "C" JNIEXPORT jstring JNICALL Java_app_portlauncher_core_CoreBridge_gameDbFind(JNIEnv* env, jobject,
                                                                                       jstring sha1) {
    std::lock_guard lock(game_db_mutex);
    const plm::GameRecord* game = game_db.find_by_sha1(to_string(env, sha1));
    return game ? to_jstring(env, game_json(*game).dump()) : nullptr;
}

// Asks a running module to save and quit (game process only). Returns false if it isn't loaded.
extern "C" JNIEXPORT jboolean JNICALL Java_app_portlauncher_core_CoreBridge_requestModuleQuit(JNIEnv* env, jobject,
                                                                                               jstring library) {
    void* handle = dlopen(to_string(env, library).c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, log_tag, "requestModuleQuit: module not loaded");
        return JNI_FALSE;
    }
    using QuitFn = void (*)();
    auto quit = reinterpret_cast<QuitFn>(dlsym(handle, "plm_request_quit"));
    if (quit != nullptr) {
        quit();
    }
    dlclose(handle);
    return quit != nullptr ? JNI_TRUE : JNI_FALSE;
}

// Game process only: calls an optional void() export of the loaded module (plm_pause, plm_resume). Returns false if
// the module doesn't export it.
extern "C" JNIEXPORT jboolean JNICALL Java_app_portlauncher_core_CoreBridge_callModule(JNIEnv* env, jobject,
                                                                                        jstring library,
                                                                                        jstring symbol) {
    void* handle = dlopen(to_string(env, library).c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return JNI_FALSE;
    }
    using VoidFn = void (*)();
    auto fn = reinterpret_cast<VoidFn>(dlsym(handle, to_string(env, symbol).c_str()));
    if (fn != nullptr) {
        fn();
    }
    dlclose(handle);
    return fn != nullptr ? JNI_TRUE : JNI_FALSE;
}

// Game process only: calls an optional void(const char*) export of the loaded module (plm_apply_settings).
extern "C" JNIEXPORT jboolean JNICALL Java_app_portlauncher_core_CoreBridge_callModuleString(JNIEnv* env, jobject,
                                                                                              jstring library,
                                                                                              jstring symbol,
                                                                                              jstring argument) {
    void* handle = dlopen(to_string(env, library).c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return JNI_FALSE;
    }
    using StringFn = void (*)(const char*);
    auto fn = reinterpret_cast<StringFn>(dlsym(handle, to_string(env, symbol).c_str()));
    if (fn != nullptr) {
        fn(to_string(env, argument).c_str());
    }
    dlclose(handle);
    return fn != nullptr ? JNI_TRUE : JNI_FALSE;
}

// Game process only: whether the loaded module exports [symbol] (optional features such as plm_save_state).
extern "C" JNIEXPORT jboolean JNICALL Java_app_portlauncher_core_CoreBridge_hasModuleFunction(JNIEnv* env, jobject,
                                                                                               jstring library,
                                                                                               jstring symbol) {
    void* handle = dlopen(to_string(env, library).c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return JNI_FALSE;
    }
    const bool found = dlsym(handle, to_string(env, symbol).c_str()) != nullptr;
    dlclose(handle);
    return found ? JNI_TRUE : JNI_FALSE;
}

// Game process only: hands the loaded module the host API (MODULE_SPEC §4) before plm_main runs. host is the
// activity class whose static onModuleFirstFrame() is called. Returns false if the module has no plm_set_host.
extern "C" JNIEXPORT jboolean JNICALL Java_app_portlauncher_core_CoreBridge_attachHost(JNIEnv* env, jobject,
                                                                                        jstring library,
                                                                                        jclass host) {
    void* handle = dlopen(to_string(env, library).c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return JNI_FALSE;
    }
    using SetHostFn = void (*)(const plm_host_api*);
    auto set_host = reinterpret_cast<SetHostFn>(dlsym(handle, "plm_set_host"));
    dlclose(handle);
    if (set_host == nullptr) {
        return JNI_FALSE;
    }
    host_bridge.activity_class = static_cast<jclass>(env->NewGlobalRef(host));
    host_bridge.on_first_frame = env->GetStaticMethodID(host, "onModuleFirstFrame", "()V");
    if (host_bridge.on_first_frame == nullptr) {
        env->ExceptionClear();
        return JNI_FALSE;
    }
    set_host(&host_api);
    return JNI_TRUE;
}
