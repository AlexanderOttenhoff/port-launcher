package app.portlauncher.util

import org.json.JSONObject
import java.io.File

/**
 * Feature flags for unfinished, post-alpha features (owner 2026-11-15), switched in Settings → Experimental. Stored in
 * files/app-flags.json, read by both the launcher and the game process (same app, same files dir), so a change
 * applies at the next game launch. Each flag has its own default (experimental ones are off).
 */
object AppFlags {
    /**
     * Save states: the in-game menu's Save/Load state and the L4 + Y/X quick keys. Post-alpha: states are session-only
     * so far, and the feature isn't complete until they survive quitting (docs/planning/SAVE_STATES.md).
     */
    const val SAVE_STATES = "save_states"

    /** Reopen the launcher and menus where the player left them (UiState). An app option, on by default. */
    const val REMEMBER_POSITION = "remember_position"

    private fun file(filesDir: File) = File(filesDir, "app-flags.json")

    fun isOn(filesDir: File, flag: String, default: Boolean = false): Boolean =
        runCatching { JSONObject(file(filesDir).readText()).optBoolean(flag, default) }.getOrDefault(default)

    fun set(filesDir: File, flag: String, on: Boolean) {
        val json = runCatching { JSONObject(file(filesDir).readText()) }.getOrDefault(JSONObject())
        file(filesDir).writeTextAtomic(json.put(flag, on).toString(2))
    }
}
