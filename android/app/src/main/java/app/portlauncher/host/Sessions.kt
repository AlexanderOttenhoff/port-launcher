package app.portlauncher.host

import java.io.File

/**
 * files/sessions/<launch time in ms>/: one folder per game launch, holding that launch's launch.json.
 *
 * Why this exists (SLOP_AUDIT §1.2, hardening pass 2026-11-14): every launch created a new folder and nothing ever
 * deleted one, so they piled up forever (small, but unbounded, and noise when reading files/ over adb). Now each
 * launch prunes all but the newest [KEEP]. Why keep a few instead of reusing one folder: the running game reads its
 * launch.json by path (the host activity, and the shims at plm_main), and the last couple of launches' JSON is
 * the first thing to look at when a game misbehaves. Why not delete at game exit: the :game process is killed
 * rather than shut down cleanly, so there's no reliable "after" hook; pruning at the next launch is.
 */
object Sessions {
    /** How many recent session folders survive a prune; the one just created is always among them. */
    const val KEEP = 3

    /**
     * Pure: which of the folder [names] to delete so that the newest [keep] remain. Only names that are launch
     * timestamps count; anything else in the folder isn't ours to judge and is left alone.
     */
    fun toPrune(names: Collection<String>, keep: Int = KEEP): List<String> =
        names.filter { n -> n.toLongOrNull() != null }
            .sortedByDescending { it.toLong() }
            .drop(keep)

    /** Creates this launch's folder, then deletes the old ones. Best effort: a failed delete is retried next launch. */
    fun create(filesDir: File, now: Long = System.currentTimeMillis()): File {
        val root = File(filesDir, "sessions")
        val session = File(root, now.toString()).apply { mkdirs() }
        toPrune(root.list().orEmpty().toList()).forEach { File(root, it).deleteRecursively() }
        return session
    }
}
