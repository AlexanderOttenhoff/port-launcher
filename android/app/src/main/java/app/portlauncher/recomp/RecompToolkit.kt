package app.portlauncher.recomp

import app.portlauncher.util.writeTextAtomic
import android.content.Context
import android.system.Os
import android.util.Log
import java.io.BufferedInputStream
import java.io.EOFException
import java.io.File
import java.io.InputStream
import java.security.DigestInputStream
import java.security.MessageDigest
import java.util.zip.GZIPInputStream

/**
 * The on-device recomp toolkit (docs/planning/ON_DEVICE_RECOMP.md): clang + lld, the NDK sysroot pieces, the
 * generators (N64Recomp, RSPRecomp, the game's decompressor), the module's prebuilt non-ROM parts and the driver
 * scripts, in the layout tools/toolkit/stage_dk64_device.sh stages:
 *
 *   files/recomp-toolkit/{toolkit/, sysroot/, recomp/, game/, prebuilt/, build_from_rom.sh, build_libgame.sh}
 *
 * Why app-internal storage (filesDir) and not external: the exec backend runs these binaries, and on the RP6 exec is
 * allowed from the app's own data dir; external storage is mounted noexec everywhere. The dlopen backend (stock
 * Android, later) will load libraries from here too, which also has to be app-internal.
 *
 * How it gets here (MVP, debug builds only): nothing downloads it yet. A debug build picks up a tarball the developer
 * pushed (see [pickupFiles]; docs/planning/ON_DEVICE_RECOMP.md "Installing the toolkit") and unpacks it with the tiny
 * tar reader below. Later the module server will offer it as a download (same tarball, same unpack code).
 *
 * Version: the SHA-1 of the tarball it was unpacked from, kept in [VERSION_FILE]. It's part of libgame.so's cache key
 * (RecompBuilder), so a new toolkit (new clang, new prebuilt patches, new configs) rebuilds the game code once.
 */
class RecompToolkit(private val context: Context) {
    val root: File get() = File(context.filesDir, "recomp-toolkit")

    /** Null when no toolkit is installed (or the install was interrupted: the version file is written last). */
    fun version(): String? = File(root, VERSION_FILE).takeIf { it.isFile }?.readText()?.trim()?.ifEmpty { null }

    /**
     * Where a debug build looks for a pushed toolkit tarball, first match wins:
     *  1. the app's external files dir, /sdcard/Android/data/<package>/files/plm-recomp-toolkit.tar: adb can push
     *     there directly, and the app reads it without any storage permission (scoped storage would block
     *     /sdcard/Download for a file the app didn't create, which is why that suggested spot isn't used);
     *  2. files/plm-recomp-toolkit.tar, for `adb shell run-as <package> sh -c 'cat > files/...' < tar` on devices where
     *     adb can't write to Android/data.
     * Release builds never look (no side-loading of executable code into a shipped app).
     */
    fun pickupFiles(): List<File> = listOfNotNull(
        context.getExternalFilesDir(null)?.let { File(it, TARBALL) },
        File(context.filesDir, TARBALL),
    )

    fun pendingPickup(debug: Boolean): File? = if (!debug) null else pickupFiles().firstOrNull { it.isFile && it.length() > 0 }

    /**
     * Unpacks [tarball] (plain or gzipped tar) into [root], replacing what's there, and deletes the tarball afterwards
     * (it's ~140 MB, and keeping it would re-install on every launch). Slow (seconds): call off the UI thread.
     * [onProgress] gets 0..1 by bytes read.
     *
     * Atomic enough for an MVP: unpacks into a sibling temp dir, then swaps it in, and writes the version file only
     * when everything else is there, so an interrupted install reads as "no toolkit" rather than a half one.
     */
    fun install(tarball: File, onProgress: (Float) -> Unit = {}) {
        val tmp = File(context.filesDir, "recomp-toolkit.tmp").apply { deleteRecursively(); mkdirs() }
        val sha = MessageDigest.getInstance("SHA-1")
        val total = tarball.length().coerceAtLeast(1)
        val counting = CountingInputStream(tarball.inputStream()) { read -> onProgress(read.toFloat() / total) }
        DigestInputStream(BufferedInputStream(counting, 1 shl 16), sha).use { digesting ->
            // gzip magic 1f 8b: accept both, so whoever packs it can choose (stage_dk64_device.sh writes plain tar).
            val buffered = BufferedInputStream(digesting, 1 shl 16)
            buffered.mark(2)
            val gz = buffered.read() == 0x1f && buffered.read() == 0x8b
            buffered.reset()
            untar(if (gz) GZIPInputStream(buffered, 1 shl 16) else buffered, tmp)
            // Drain whatever trails the tar's end blocks, so the SHA-1 covers the whole file.
            val sink = ByteArray(1 shl 16)
            while (digesting.read(sink) >= 0) { /* hash the rest */ }
        }
        // A tarball made one level up (tar -cf x.tar device/) has everything under one folder: step into it.
        val content = if (File(tmp, SCRIPT_MARKER).isFile) tmp else
            tmp.listFiles()?.singleOrNull { File(it, SCRIPT_MARKER).isFile }
                ?: throw IllegalStateException("$tarball has no $SCRIPT_MARKER: not a recomp toolkit bundle")
        val version = sha.digest().joinToString("") { "%02x".format(it) }
        root.deleteRecursively()
        if (!content.renameTo(root)) throw IllegalStateException("couldn't move the toolkit into $root")
        tmp.deleteRecursively()
        File(root, VERSION_FILE).writeTextAtomic(version)
        tarball.delete()
        Log.i(TAG, "toolkit $version installed from $tarball")
    }

    /**
     * The smallest tar reader that handles what GNU tar and bsdtar write for the bundle: regular files, directories,
     * symlinks (the toolkit's clang/clang++/ld.lld are symlinks to the one multi-call `llvm` binary, which picks the
     * tool by argv[0]), GNU long names ('L', the sysroot's headers go past 100 characters) and pax headers ('x', for
     * path/linkpath). Why hand-written: Android has no tar API, and commons-compress would be a dependency for ~80
     * lines. Hard links, devices and sparse files aren't in the bundle and are skipped with a log line.
     *
     * Executable bits: kept from the tar's mode (anything with an x bit becomes executable for everyone, which in app
     * storage only means "for the app"). That's what makes the exec backend work at all: Java's file writes create
     * 0600 files, and the scripts exec recomp/bin/ and toolkit/bin/ directly.
     */
    private fun untar(input: InputStream, dest: File) {
        val header = ByteArray(512)
        val canonicalDest = dest.canonicalPath
        var longName: String? = null
        var longLink: String? = null
        var paxPath: String? = null
        var paxLink: String? = null
        while (true) {
            if (!readFully(input, header)) return
            if (header.all { it == 0.toByte() }) return // end-of-archive block
            val type = header[156].toInt().toChar()
            val size = octal(header, 124, 12)
            val mode = octal(header, 100, 8).toInt()
            val prefix = if (String(header, 257, 5, Charsets.US_ASCII) == "ustar") cString(header, 345, 155) else ""
            val rawName = longName ?: paxPath ?: (if (prefix.isNotEmpty()) "$prefix/" else "") + cString(header, 0, 100)
            val linkName = longLink ?: paxLink ?: cString(header, 157, 100)
            when (type) {
                'L' -> { longName = String(readBytes(input, size), Charsets.UTF_8).trimEnd('\u0000'); continue }
                'K' -> { longLink = String(readBytes(input, size), Charsets.UTF_8).trimEnd('\u0000'); continue }
                'x' -> {
                    // pax records: "<len> key=value\n"
                    String(readBytes(input, size), Charsets.UTF_8).lineSequence().forEach { line ->
                        val kv = line.substringAfter(' ', "")
                        when {
                            kv.startsWith("path=") -> paxPath = kv.removePrefix("path=")
                            kv.startsWith("linkpath=") -> paxLink = kv.removePrefix("linkpath=")
                        }
                    }
                    continue
                }
                'g' -> { skip(input, padded(size)); continue } // global pax header: nothing we use
            }
            longName = null; longLink = null; paxPath = null; paxLink = null
            val name = rawName.removePrefix("./").trimEnd('/')
            if (name.isEmpty() || name == ".") { skip(input, padded(size)); continue }
            val out = File(dest, name)
            // Never write outside the destination ("../" entries in a tarball someone handed us).
            if (!out.canonicalPath.startsWith(canonicalDest + File.separator)) {
                throw IllegalStateException("tar entry escapes the destination: $name")
            }
            when (type) {
                '5' -> out.mkdirs()
                '2' -> {
                    out.parentFile?.mkdirs()
                    out.delete()
                    Os.symlink(linkName, out.path)
                }
                '0', '\u0000', '7' -> {
                    out.parentFile?.mkdirs()
                    out.outputStream().use { o -> copyN(input, o, size) }
                    skip(input, padded(size) - size)
                    if (mode and 0b001_001_001 != 0) out.setExecutable(true, false)
                    continue
                }
                else -> Log.w(TAG, "tar: skipping $name (type '$type')")
            }
            skip(input, padded(size))
        }
    }

    private fun padded(size: Long) = (size + 511) / 512 * 512

    private fun octal(b: ByteArray, off: Int, len: Int): Long {
        // GNU base-256 for sizes over 8 GB: high bit set. Not in our bundle, but cheap to refuse clearly.
        if (b[off].toInt() and 0x80 != 0) throw IllegalStateException("tar: base-256 size not supported")
        val s = String(b, off, len, Charsets.US_ASCII).trim { it == ' ' || it == '\u0000' }
        return if (s.isEmpty()) 0 else s.toLong(8)
    }

    private fun cString(b: ByteArray, off: Int, len: Int): String {
        var end = off
        while (end < off + len && b[end] != 0.toByte()) end++
        return String(b, off, end - off, Charsets.UTF_8)
    }

    private fun readFully(input: InputStream, buf: ByteArray): Boolean {
        var n = 0
        while (n < buf.size) {
            val r = input.read(buf, n, buf.size - n)
            if (r < 0) { if (n == 0) return false else throw EOFException("truncated tar") }
            n += r
        }
        return true
    }

    private fun readBytes(input: InputStream, size: Long): ByteArray {
        val buf = ByteArray(size.toInt())
        readFully(input, buf)
        skip(input, padded(size) - size)
        return buf
    }

    private fun copyN(input: InputStream, out: java.io.OutputStream, size: Long) {
        val buf = ByteArray(1 shl 16)
        var left = size
        while (left > 0) {
            val r = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
            if (r < 0) throw EOFException("truncated tar")
            out.write(buf, 0, r)
            left -= r
        }
    }

    private fun skip(input: InputStream, n: Long) {
        var left = n
        val buf = ByteArray(4096)
        while (left > 0) {
            val r = input.read(buf, 0, minOf(buf.size.toLong(), left).toInt())
            if (r < 0) throw EOFException("truncated tar")
            left -= r
        }
    }

    private class CountingInputStream(private val inner: InputStream, private val onRead: (Long) -> Unit) : InputStream() {
        private var count = 0L
        private var lastReport = 0L
        override fun read(): Int = inner.read().also { if (it >= 0) bump(1) }
        override fun read(b: ByteArray, off: Int, len: Int): Int = inner.read(b, off, len).also { if (it > 0) bump(it.toLong()) }
        override fun close() = inner.close()
        private fun bump(n: Long) {
            count += n
            if (count - lastReport > 1 shl 20) { lastReport = count; onRead(count) } // once per MB, not per read
        }
    }

    companion object {
        const val TARBALL = "plm-recomp-toolkit.tar"
        const val VERSION_FILE = ".plm-toolkit-version"
        /** A file every toolkit bundle has at its root; used to find the root inside the tarball. */
        private const val SCRIPT_MARKER = "build_from_rom.sh"
        private const val TAG = "plm-recomp"
    }
}
