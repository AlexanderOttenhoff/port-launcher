package app.portlauncher.host

import android.app.Activity
import android.app.ActivityManager
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.core.content.IntentCompat

/**
 * The in-game menu's Reset (owner 2026-11-15: "add a reset here for fun"): a console's reset button, done as a
 * clean relaunch. Why a relaunch and not a module call: there is no reset in the module ABI, and a module runs once
 * per ":game" process (MODULE_SPEC §4), so the only clean restart is a new process with the same launch.json.
 *
 * Why this activity: the game process can't start its own replacement (the new activity would land in the dying
 * process). So Sdl2HostActivity starts this one (main process, no UI) with its own launch intent, then kills itself;
 * this waits until the ":game" process is gone and starts the game again, so it sits on the same task as before
 * (library or frontend below it). Gives up after RELAUNCH_TIMEOUT_MS rather than starting into a half-dead process.
 */
class RelaunchActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val game = IntentCompat.getParcelableExtra(intent, EXTRA_GAME_INTENT, Intent::class.java)
        if (game == null) { finish(); return }
        val started = System.currentTimeMillis()
        fun attempt() {
            val alive = (getSystemService(ACTIVITY_SERVICE) as ActivityManager).runningAppProcesses.orEmpty()
                .any { it.processName == "$packageName:game" }
            when {
                !alive -> { startActivity(game.setClass(this, Sdl2HostActivity::class.java)); finish() }
                System.currentTimeMillis() - started > RELAUNCH_TIMEOUT_MS -> finish()
                else -> handler.postDelayed(::attempt, 100)
            }
        }
        attempt()
    }

    companion object {
        const val EXTRA_GAME_INTENT = "app.portlauncher.extra.GAME_INTENT"
        private const val RELAUNCH_TIMEOUT_MS = 5_000L
    }
}
