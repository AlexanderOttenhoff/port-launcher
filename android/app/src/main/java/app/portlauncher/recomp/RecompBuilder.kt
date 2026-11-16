package app.portlauncher.recomp

import app.portlauncher.util.writeTextAtomic
import android.content.Context
import android.util.Log
import java.io.File

/**
 * Manifest "game_lib" (MODULE_SPEC §2): the module is a split recomp module whose game code (libgame.so) isn't in
 * the package and must be built on the device from the player's ROM before the first launch
 * (docs/planning/ON_DEVICE_RECOMP.md). [build] is "on-device" (the only value so far); [script] is the toolkit's
 * driver script for this module, relative to the toolkit root.
 */
data class GameLibSpec(val build: String, val script: String) {
    val onDevice: Boolean get() = build == "on-device"
}

/** Where the build is, for the Preparing screen. [fraction] 0..1 over the whole build, [phase] a player-facing line. */
data class BuildProgress(val phase: String, val fraction: Float)

/** The build failed: [message] for the player, [logTail] the last lines of the build's output for the error screen. */
open class RecompBuildException(message: String, val logTail: String) : Exception(message)

/** The player pressed Cancel: not an error, so the controller closes the screen instead of showing the error state. */
class RecompCancelledException : RecompBuildException("Cancelled.", "")

/**
 * What a build needs, whatever runs it. [workDir]/[outDir] are scratch space the backend may wipe; the result is
 * expected at [outDir]/libgame.so.
 */
data class BuildRequest(
    val toolkit: File,
    val script: String,
    val rom: File,
    val workDir: File,
    val outDir: File,
    val jobs: Int,
    val log: File,
)

/**
 * How the toolkit runs. Two planned backends (docs/planning/ON_DEVICE_RECOMP.md "The toolchain on the device"):
 *  - [ExecBackend] (now): the toolkit's programs run as processes. Works where the firmware allows exec from app
 *    storage: the RP6 does; stock Android 10+ (and the emulator) refuse it (W^X, error=13).
 *  - a dlopen backend (later): clang/lld and the generators built as shared libraries, loaded in a separate process
 *    and called through entry points, which stock Android allows. It slots in here: same request, same progress lines
 *    (it will emit the PLM_PROGRESS protocol itself), same cancel.
 * Why an interface for one implementation: the controller and the screen shouldn't change when the second arrives.
 */
interface RecompBackend {
    val name: String

    /** Runs the build, blocking (call on an IO thread). Throws [RecompBuildException] on failure. */
    fun run(request: BuildRequest, onProgress: (BuildProgress) -> Unit)

    /** Stops a running [run] from another thread; [run] then throws [RecompCancelledException]. */
    fun cancel()
}

/**
 * Runs the toolkit's POSIX sh driver (build_from_rom.sh) with the system shell. Only /system/bin/sh itself is exec'd
 * from the system partition, which is always allowed; the script then execs the toolkit's own binaries
 * (recomp/bin/, toolkit/bin/clang), and that's the step stock Android blocks.
 *
 * Why a child process and not threads in the app: it's what exec means, and it keeps clang's memory (a few hundred MB
 * per job) out of the app's heap; the app only reads stdout on an IO thread, so the UI stays responsive.
 */
class ExecBackend : RecompBackend {
    override val name = "exec"
    @Volatile private var process: Process? = null
    @Volatile private var cancelled = false
    @Volatile private var pid: Int? = null

    override fun run(request: BuildRequest, onProgress: (BuildProgress) -> Unit) {
        cancelled = false
        pid = null
        request.workDir.deleteRecursively()
        request.outDir.deleteRecursively()
        request.outDir.mkdirs()
        request.log.parentFile?.mkdirs()
        val parser = ProgressParser()
        val tail = ArrayDeque<String>()
        // `echo PLM_PID $$; exec sh script`: the wrapper prints its pid, then becomes the script under the same pid.
        // Why: cancel() needs the pid to kill the process tree, and java.lang.Process on Android has no pid() (the
        // Java 9 API isn't there) and its private field is a hidden API. Script and arguments go in as $0..$2, never
        // spliced into the command string, so paths with spaces or quotes can't break it.
        val pb = ProcessBuilder("/system/bin/sh", "-c", "echo PLM_PID \$\$; exec /system/bin/sh \"\$0\" \"\$1\" \"\$2\"",
            File(request.toolkit, request.script).path, request.rom.path, request.jobs.toString())
            .directory(request.toolkit)
            .redirectErrorStream(true) // one stream: the log tail must show errors in order with what preceded them
        pb.environment().apply {
            put("WORK_DIR", request.workDir.path)
            put("OUT_DIR", request.outDir.path)
            // clang and sh here-docs write temp files to $TMPDIR, defaulting to /tmp or /data/local/tmp, neither of
            // which an app may write. Point it at our own cache.
            put("TMPDIR", request.workDir.parentFile?.path ?: request.workDir.path)
        }
        val started = System.currentTimeMillis()
        val p = try { pb.start() } catch (e: java.io.IOException) {
            throw RecompBuildException("Couldn't start the build: ${e.message}", "")
        }
        process = p
        request.log.bufferedWriter().use { log ->
            log.write("# backend exec, jobs ${request.jobs}, rom ${request.rom}\n")
            try { p.inputStream.bufferedReader().forEachLine { line ->
                if (pid == null && line.startsWith("PLM_PID ")) { pid = line.removePrefix("PLM_PID ").trim().toIntOrNull(); return@forEachLine }
                log.write(line); log.write("\n")
                if (!line.startsWith("PLM_PROGRESS")) {
                    tail.addLast(line)
                    if (tail.size > TAIL_LINES) tail.removeFirst()
                }
                parser.feed(line)?.let(onProgress)
            } } catch (e: java.io.IOException) {
                // cancel() force-closes the pipe under this reader ("read interrupted by close()"); that's the
                // cancel, not a build error. Anything else is a real failure.
                if (!cancelled) throw RecompBuildException("Lost the build's output: ${e.message}", tail.joinToString("\n"))
            }
            val exit = p.waitFor()
            process = null
            log.write("# exit $exit after ${(System.currentTimeMillis() - started) / 1000} s\n")
            val tailText = tail.joinToString("\n")
            if (cancelled) throw RecompCancelledException()
            if (exit != 0) {
                // Exit 126 / "Permission denied" from the script's first exec of a toolkit binary: the device forbids
                // exec from app storage (stock Android). Say so plainly instead of leaving it to the log.
                val denied = exit == 126 || tail.any { it.contains("Permission denied") || it.contains("can't execute") }
                throw RecompBuildException(
                    if (denied) "This device doesn't allow running the build tools from app storage. Only some " +
                        "devices (like the Retroid Pocket 6) allow it; others need a version of the toolkit that " +
                        "isn't ready yet."
                    else "The build stopped with an error (exit $exit).",
                    tailText,
                )
            }
        }
        Log.i(TAG, "exec build finished in ${(System.currentTimeMillis() - started) / 1000} s")
    }

    override fun cancel() {
        cancelled = true
        val p = process ?: return
        // Killing sh alone leaves xargs and the clang workers running (they'd be reparented to init and keep the CPU
        // busy for minutes). Process groups don't help: the script's children share the APP's group, so a group kill
        // would take the launcher down too. So walk /proc and kill the whole subtree. Same uid, so
        // allowed. Parents before children, from a snapshot of the tree: a dead xargs can't start new clangs, and its
        // orphaned children are still in the snapshot. The pid comes from the wrapper's PLM_PID line (see run()).
        val root = pid ?: run { p.destroyForcibly(); return }
        killTree(root)
        p.destroyForcibly()
    }

    private fun killTree(root: Int) {
        val children = mutableMapOf<Int, MutableList<Int>>()
        File("/proc").listFiles()?.forEach { dir ->
            val pid = dir.name.toIntOrNull() ?: return@forEach
            // /proc/<pid>/stat: "pid (comm) state ppid ..."; comm may contain spaces, so split after the last ')'.
            val stat = runCatching { File(dir, "stat").readText() }.getOrNull() ?: return@forEach
            val ppid = stat.substringAfterLast(')').trim().split(' ').getOrNull(1)?.toIntOrNull() ?: return@forEach
            children.getOrPut(ppid) { mutableListOf() } += pid
        }
        fun kill(pid: Int) {
            runCatching { android.os.Process.sendSignal(pid, android.os.Process.SIGNAL_KILL) }
            children[pid]?.forEach(::kill)
        }
        kill(root)
    }

    companion object {
        private const val TAIL_LINES = 40
        private const val TAG = "plm-recomp"
    }
}

/**
 * Turns the scripts' PLM_PROGRESS lines (tools/toolkit/build_from_rom.sh header) into one 0..1 bar. Weights from
 * the desktop and emulator runs: everything before the compile is seconds, the compile is nearly all of it, the link
 * is about a second. So the bar is basically "files compiled / total", with small slices before and after so it
 * visibly moves at the start.
 */
class ProgressParser {
    private var total = 0
    private var done = 0
    private var phase = "check"

    fun feed(line: String): BuildProgress? {
        if (!line.startsWith("PLM_PROGRESS ")) return null
        val parts = line.removePrefix("PLM_PROGRESS ").trim().split(' ', limit = 2)
        when (parts.getOrNull(0)) {
            "phase" -> phase = parts.getOrNull(1) ?: return null
            "total" -> total = parts.getOrNull(1)?.toIntOrNull() ?: return null
            "file" -> done++
            else -> return null
        }
        return current()
    }

    private fun current(): BuildProgress {
        val compile = if (total > 0) done.toFloat() / total else 0f
        return when (phase) {
            "check" -> BuildProgress("Checking your game file", 0.00f)
            "copy" -> BuildProgress("Setting up", 0.01f)
            "decompress" -> BuildProgress("Unpacking the game data", 0.02f)
            "generate" -> BuildProgress("Translating the game code", 0.04f)
            "compile" -> BuildProgress("Compiling the game code ($done of $total)", 0.08f + 0.88f * compile)
            "link" -> BuildProgress("Putting it together", 0.97f)
            "done" -> BuildProgress("Done", 1f)
            else -> BuildProgress(phase, 0.5f)
        }
    }
}

/**
 * The cache of device-built game code: files/recomp/<module>/libgame.so plus libgame.key next to it.
 *
 * Key = ROM SHA-1 + module version + toolkit version (one line each). Why these three: the ROM is the input; the
 * module version stands for the configs and the runtime ABI the code was generated and linked against; the toolkit
 * version covers clang, the generators and the prebuilt patches. Any change means the cached library may not match,
 * so build it again (once). Without a key file (the spike-2 hand copy) the library counts as stale.
 */
class RecompCache(context: Context, moduleId: String) {
    val dir = File(context.filesDir, "recomp/$moduleId")
    val lib = File(dir, "libgame.so")
    private val keyFile = File(dir, "libgame.key")
    val logFile = File(dir, "build.log")

    fun key(romSha1: String, moduleVersion: String, toolkitVersion: String) =
        "rom $romSha1\nmodule $moduleVersion\ntoolkit $toolkitVersion\n"

    fun isFresh(key: String): Boolean = lib.isFile && keyFile.isFile && keyFile.readText() == key

    /** The key's ROM and module lines match: usable even without a toolkit (see RecompPrepare.needsBuild). */
    fun matchesIgnoringToolkit(romSha1: String, moduleVersion: String): Boolean =
        lib.isFile && keyFile.isFile && keyFile.readText().startsWith("rom $romSha1\nmodule $moduleVersion\n")

    /**
     * Moves a finished build in: copy to a temp name, make it read-only (Android 14+ requires that for loaded code
     * from app storage, spike 1; it costs nothing elsewhere), rename over the old one, then write the key. Key last,
     * so a crash in between leaves "stale", never "fresh but wrong".
     */
    fun store(built: File, key: String) {
        dir.mkdirs()
        keyFile.delete()
        val tmp = File(dir, "libgame.so.tmp")
        tmp.delete()
        built.copyTo(tmp, overwrite = true)
        tmp.setWritable(false, false)
        tmp.setReadOnly()
        if (lib.exists()) { lib.setWritable(true); lib.delete() }
        if (!tmp.renameTo(lib)) throw RecompBuildException("Couldn't store the game code in $dir.", "")
        keyFile.writeTextAtomic(key)
    }
}
