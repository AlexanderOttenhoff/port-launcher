package app.portlauncher.util

import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/**
 * Replaces this file's contents with [text] so that a reader (or the next app start after a crash, a kill from the
 * task switcher, or the battery dying) sees either the complete old file or the complete new one, never half of
 * each or an empty file.
 *
 * Why (SLOP_AUDIT §1.4, hardening pass 2026-11-14): the MVP wrote the mod preset, the Thunderstore cache, launch.json,
 * install stamps, ... with plain `writeText`, which truncates first and then writes. A process death in between left
 * an empty or cut-off JSON file, and every reader here does `runCatching { JSONObject(...) }.getOrDefault(empty)`,
 * so the player silently lost the state. The writers that already did tmp + rename (library.json, the module index
 * cache, installed-mods.json, the settings files) each used a fixed tmp name (two writers racing on one tmp file can
 * rename a half-written file into place) and ignored `renameTo`'s false return (silently keeping the old data).
 *
 * How:
 * - a unique temp file in the same directory (same filesystem, so the rename is an atomic POSIX rename(2), which
 *   also replaces an existing target in one step on Android/Linux);
 * - fsync before the rename, so after a power loss the new name never points at data still only in the page cache;
 * - a failed rename throws instead of returning quietly, and the temp file is always cleaned up.
 *
 * Not a lock: two processes writing the same file still means last writer wins (the launcher and the :game process
 * both write a module's settings, but never at the same time in practice: the launcher is in the background while a
 * game runs). Plain java.io on purpose, not android.util.AtomicFile, so it runs in JVM unit tests.
 */
fun File.writeTextAtomic(text: String) {
    val dir = absoluteFile.parentFile ?: throw IOException("no parent directory for $this")
    dir.mkdirs()
    val tmp = File.createTempFile(".$name.", ".tmp", dir)
    try {
        FileOutputStream(tmp).use { out ->
            out.write(text.toByteArray(Charsets.UTF_8))
            out.fd.sync()
        }
        if (!tmp.renameTo(this)) throw IOException("couldn't move $tmp over $this")
    } finally {
        // After a successful rename tmp no longer exists and this is a no-op.
        tmp.delete()
    }
}
