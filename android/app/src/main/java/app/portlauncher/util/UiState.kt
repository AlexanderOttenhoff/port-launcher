package app.portlauncher.util

import org.json.JSONObject
import java.io.File

/**
 * Where the player was (owner 2026-11-15: reopening the launcher or a menu returns to the same place; an app option,
 * Settings → General → "Remember position", on by default). Small JSON files in the files dir: the launcher writes
 * ui-state.json (library row, the open game page, each game page's tab and row), the game process writes
 * ui-state-game.json (each game's in-game menu tab and row), so the two processes never write the same file.
 */
class UiState(filesDir: File, game: Boolean = false) {
    private val file = File(filesDir, if (game) "ui-state-game.json" else "ui-state.json")
    private val flagsDir = filesDir

    val enabled: Boolean get() = AppFlags.isOn(flagsDir, AppFlags.REMEMBER_POSITION, default = true)

    private fun load(): JSONObject = runCatching { JSONObject(file.readText()) }.getOrDefault(JSONObject())
    private fun save(json: JSONObject) { runCatching { file.writeTextAtomic(json.toString(2)) } }

    fun getString(key: String): String? = if (!enabled) null else load().optString(key).takeIf { it.isNotEmpty() }

    fun putString(key: String, value: String?) {
        val json = load()
        if (value == null) json.remove(key) else json.put(key, value)
        save(json)
    }

    /** A menu position (tab index, row index) under [key]; (0, 0) when unknown or the option is off. */
    fun position(key: String): Pair<Int, Int> {
        if (!enabled) return 0 to 0
        val p = load().optJSONObject("positions")?.optJSONObject(key) ?: return 0 to 0
        return p.optInt("tab") to p.optInt("row")
    }

    fun putPosition(key: String, tab: Int, row: Int) {
        val json = load()
        val positions = json.optJSONObject("positions") ?: JSONObject().also { json.put("positions", it) }
        positions.put(key, JSONObject().put("tab", tab).put("row", row))
        save(json)
    }
}
