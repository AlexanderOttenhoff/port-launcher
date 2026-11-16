package app.portlauncher.core

/** JNI entry points into the portable C++ core (core/). JSON results keep the boundary small. */
object CoreBridge {
    init {
        System.loadLibrary("plm_launcher")
    }

    external fun coreVersion(): String

    external fun sha1Hex(data: ByteArray): String

    /** Scans one candidate file; the caller keeps ownership of fd. Returns {"roms":[...],"error":"..."}. */
    external fun scanFd(fd: Int, fileName: String): String

    /** Writes the ROM (a zip entry if zipEntry isn't empty) to outPath as big-endian .z64; returns its SHA-1 or null. */
    external fun exportRomFd(fd: Int, zipEntry: String, outPath: String): String?

    /** Adds a module manifest to the game DB; returns "" or an error. */
    external fun gameDbAddManifest(manifest: String): String

    /** Returns the game for a ROM SHA-1 as JSON, or null. */
    external fun gameDbFind(sha1: String): String?

    /** Game process only: asks the loaded module to save and quit. */
    external fun requestModuleQuit(library: String): Boolean

    /** Game process only: calls an optional void() export such as plm_pause; false if the module lacks it. */
    external fun callModule(library: String, symbol: String): Boolean

    /** Game process only: calls an optional void(const char*) export such as plm_apply_settings. */
    external fun callModuleString(library: String, symbol: String, argument: String): Boolean

    /** Game process: whether the loaded module exports [symbol] (optional features, e.g. plm_save_state). */
    external fun hasModuleFunction(library: String, symbol: String): Boolean

    /** Game process only: gives the loaded module the host API; [host] provides static onModuleFirstFrame(). */
    external fun attachHost(library: String, host: Class<*>): Boolean
}
