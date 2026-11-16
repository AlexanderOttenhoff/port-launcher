package app.portlauncher.downloads

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import java.util.concurrent.Executors

/**
 * The launcher's one download queue (owner 2026-11-15): modules, module updates and mods all go through here, one at
 * a time, instead of each screen showing its own percentage. The library shows a progress bar at the bottom and a
 * Downloads list; deliberately just a list (no pause, cancel or reorder).
 *
 * Launcher process only: the in-game menu runs in the game's process and shows its own progress.
 */
object Downloads {
    enum class State { QUEUED, RUNNING, DONE, FAILED }

    data class Task(val id: Int, val title: String, val state: State, val progress: Float? = null, val error: String? = null)

    /** Every task this session, oldest first; Compose state, so the library redraws as it changes. */
    var tasks by mutableStateOf(listOf<Task>())
        private set

    private val worker = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private var nextId = 1

    val active: Task? get() = tasks.firstOrNull { it.state == State.RUNNING }
    val queued: Int get() = tasks.count { it.state == State.QUEUED }

    private fun update(id: Int, change: (Task) -> Task) = main.post { tasks = tasks.map { if (it.id == id) change(it) else it } }

    /**
     * Queues [job], which reports progress (0..1) through its argument and runs on the queue's thread. [onDone] runs on
     * the main thread with the failure, or null on success.
     */
    fun enqueue(title: String, job: (progress: (Float) -> Unit) -> Unit, onDone: (Throwable?) -> Unit = {}) {
        val id = nextId++
        tasks = tasks + Task(id, title, State.QUEUED)
        worker.execute {
            update(id) { it.copy(state = State.RUNNING, progress = 0f) }
            var last = -1
            val result = runCatching {
                job { p ->
                    val percent = (p * 100).toInt()
                    if (percent != last) { last = percent; update(id) { it.copy(progress = p) } }
                }
            }
            val error = result.exceptionOrNull()
            update(id) { it.copy(state = if (error == null) State.DONE else State.FAILED, progress = null, error = error?.message) }
            main.post { onDone(error) }
        }
    }

    /** Drops finished and failed tasks from the list. */
    fun clearFinished() {
        tasks = tasks.filter { it.state == State.QUEUED || it.state == State.RUNNING }
    }
}
