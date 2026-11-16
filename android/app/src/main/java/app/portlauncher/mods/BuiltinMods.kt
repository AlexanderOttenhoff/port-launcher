package app.portlauncher.mods

import app.portlauncher.util.writeTextAtomic
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * A built-in mod (owner 2026-11-16): one of our gameplay features compiled into the module (GoldenEye's quick weapon
 * select, PD's steady aim...), off = exactly the stock game. Shown on the Mods tab with the downloaded mods, switched
 * on or off but never removed. The switch is the port's own config key [setting] (sent as "port.<setting>" with the
 * settings, so the shims apply it like any port option, live in game too). From the manifest's "mods.builtin".
 */
data class BuiltinMod(val id: String, val name: String, val description: String, val setting: String, val default: Boolean) {
    fun toJson(): JSONObject = JSONObject().put("id", id).put("name", name).put("description", description)
        .put("setting", setting).put("default", default)

    companion object {
        fun fromJson(o: JSONObject) = BuiltinMod(o.optString("id"), o.optString("name"), o.optString("description"),
            o.optString("setting"), o.optBoolean("default", true))

        fun listFromJson(a: JSONArray?): List<BuiltinMod> = (0 until (a?.length() ?: 0)).map { fromJson(a!!.getJSONObject(it)) }
    }
}

/**
 * Which built-in mods are on, per module: files/modules/<id>/builtin-mods.json ({"<id>": true}), read by the launcher
 * and the game process alike. An id without an entry has its default.
 */
class BuiltinModState(filesDir: File, moduleId: String) {
    private val file = File(filesDir, "modules/$moduleId/builtin-mods.json")

    private fun load(): JSONObject = runCatching { JSONObject(file.readText()) }.getOrDefault(JSONObject())

    fun isOn(mod: BuiltinMod): Boolean = load().optBoolean(mod.id, mod.default)

    @Synchronized
    fun set(mod: BuiltinMod, on: Boolean) {
        val state = load().put(mod.id, on)
        file.parentFile?.mkdirs()
        file.writeTextAtomic(state.toString(2))
    }

    /** "port.<setting>" = "1"/"0" for every built-in mod: merged into the launch settings (always sent). */
    fun settings(mods: List<BuiltinMod>): Map<String, String> =
        mods.filter { it.setting.isNotEmpty() }.associate { "port.${it.setting}" to if (isOn(it)) "1" else "0" }
}
