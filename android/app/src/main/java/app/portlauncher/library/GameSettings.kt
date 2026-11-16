package app.portlauncher.library

import app.portlauncher.util.writeTextAtomic
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * A canonical setting the launcher's menu offers (docs/planning/MENU.md). Values are canonical, module-independent.
 * Only settings with a valid reason to change them (preference, accessibility, device) belong here; fixed parts of
 * the modern experience (Start skips cutscenes, fast text) are built in without a setting.
 */
data class CanonicalOption(
    val key: String,
    val label: String,
    val detail: String,
    val values: List<Pair<String, String>>, // value to label
    val default: String,
    /** The in-game menu tab it appears on (MenuTab ids: gameplay, controls, graphics, audio). */
    val tab: String = MenuTab.GAMEPLAY,
    /**
     * Whether launch.json carries the value while it's still the default. Our own options always do (the module has
     * no other source). A port's own option (PortSchema) doesn't, unless its schema says "force": untouched, the port
     * keeps its own default and whatever it saved, instead of us overwriting it every launch.
     */
    val sendDefault: Boolean = true,
    /**
     * Where the port's own menu shows it (owner 2026-11-15: the settings pages copy the original menu's tab names and
     * order, minus PC-only items): the tab's name, the top menu that groups tabs (libultraship's Settings /
     * Enhancements; empty when the port has one level), and the header above it inside the tab. Our own options
     * leave [tabLabel] null and appear on the tab named after [tab] (MenuTab.label), merged into a port tab of the
     * same name when there is one.
     */
    val tabLabel: String? = null,
    val tabGroup: String = "",
    val section: String = "",
)

/**
 * Our own tab ids (docs/planning/MENU.md "Menu pages"). Since 2026-11-15 a port's own options bring their own tabs
 * (CanonicalOption.tabLabel); these ids remain for our own options and as the fallback for a schema without "tab".
 * "game" is the main page (Resume, Save/Load state, Reset, Quit).
 */
object MenuTab {
    /** The tab name our own options (and schema options without "tab") appear under. */
    fun label(id: String) = when (id) {
        GAMEPLAY -> "Gameplay"; CONTROLS -> "Controls"; GRAPHICS -> "Graphics"; AUDIO -> "Audio"
        ENHANCEMENTS -> "Enhancements"; else -> id.replaceFirstChar { it.uppercase() }
    }

    const val GAME = "game"
    /** The one tab holding all the port's settings (owner 2026-11-16): profiles, then the port's categories. */
    const val SETTINGS = "settings"
    const val GAMEPLAY = "gameplay"
    const val CONTROLS = "controls"
    const val GRAPHICS = "graphics"
    const val AUDIO = "audio"
    /** The port's own options that aren't gameplay, controls, graphics or audio (settings schema, MENU.md 2026-11-14). */
    const val ENHANCEMENTS = "enhancements"
    const val MODS = "mods"
    /** Game inputs to device inputs, with rebinding (planned, MENU.md / INPUT_INTENTS.md); a stub tab for now. */
    const val INPUT = "input"
    const val ACHIEVEMENTS = "achievements"
    /** App settings only: feature flags for post-alpha features (util/AppFlags). */
    const val EXPERIMENTAL = "experimental"
    /** App settings only: everyday app options (util/AppFlags). */
    const val GENERAL = "general"
    /** Home screen only: ROM folders (and RomM, later). Not a settings tab: no canonical options live here. */
    const val SOURCES = "sources"
}

/**
 * A module's own settings, from the settings_schema.json in its data folder (docs/planning/MENU.md, design of
 * 2026-11-14: each game's own settings in one consistent UI, instead of a lowest-common-denominator set).
 *
 * Format 1:
 *   {"format": 1, "options": [{"key": "<the port's own key>", "label": "...", "detail": "...",
 *     "tab": "<the port's own tab name>", "tab_group": "<its top menu, optional>", "section": "<header, optional>",
 *     "category": "gameplay|controls|graphics|audio|enhancements",   // fallback when "tab" is missing
 *     "default": "<value>",
 *     "values": [{"value": "...", "label": "..."}]            // a choice or a toggle, or
 *     "min": 0, "max": 100, "step": 5, "unit": "%"}]}          // a range
 * Values are strings in the port's own terms (a CVar's "1"/"0", a config enum's id...). Keys are stored and passed
 * prefixed with "port." so the shim can tell them from our own option keys and apply them by the port's key with one
 * generic setter.
 */
/**
 * A settings profile (owner 2026-11-16): a named set of option values applied in one go from the Settings tab
 * ("Modern": many quality-of-life options at once). From the schema's "profiles" (tools/schema/lus_schema.py takes them
 * from the port's own curated presets). [values] are keyed like options ("port." + CVar).
 */
data class SettingsProfile(val id: String, val label: String, val detail: String, val values: Map<String, String>) {
    fun toJson(): JSONObject = JSONObject().put("id", id).put("label", label).put("detail", detail)
        .put("values", JSONObject().also { o -> values.forEach { (k, v) -> o.put(k, v) } })

    companion object {
        fun fromJson(o: JSONObject): SettingsProfile {
            val v = o.optJSONObject("values") ?: JSONObject()
            return SettingsProfile(o.optString("id"), o.optString("label"), o.optString("detail"),
                v.keys().asSequence().associateWith { v.getString(it) })
        }

        fun listFromJson(array: org.json.JSONArray?): List<SettingsProfile> =
            (0 until (array?.length() ?: 0)).map { fromJson(array!!.getJSONObject(it)) }

        fun listToJson(list: List<SettingsProfile>) = org.json.JSONArray().also { a -> list.forEach { a.put(it.toJson()) } }
    }
}

object PortSchema {
    const val PREFIX = "port."
    private const val MAX_STEPS = 41

    /** The schema's "profiles" (see [SettingsProfile]); keys get the option prefix. */
    fun parseProfiles(text: String): List<SettingsProfile> {
        val root = runCatching { JSONObject(text) }.getOrNull() ?: return emptyList()
        return SettingsProfile.listFromJson(root.optJSONArray("profiles")).map { p ->
            p.copy(values = p.values.mapKeys { PREFIX + it.key })
        }
    }

    fun parse(text: String): List<CanonicalOption> {
        val root = runCatching { JSONObject(text) }.getOrNull() ?: return emptyList()
        val options = root.optJSONArray("options") ?: return emptyList()
        return (0 until options.length()).mapNotNull { i ->
            val o = options.optJSONObject(i) ?: return@mapNotNull null
            val key = o.optString("key").takeIf { it.isNotEmpty() } ?: return@mapNotNull null
            val values = o.optJSONArray("values")?.let { a ->
                (0 until a.length()).map { a.getJSONObject(it).let { v -> v.getString("value") to v.optString("label", v.getString("value")) } }
            } ?: rangeValues(o)
            if (values.isEmpty()) return@mapNotNull null
            val default = o.optString("default", values.first().first)
            CanonicalOption(
                key = PREFIX + key,
                label = o.optString("label", key),
                detail = o.optString("detail"),
                values = if (values.any { it.first == default }) values else listOf(default to default) + values,
                default = default,
                tab = when (o.optString("category")) {
                    "gameplay" -> MenuTab.GAMEPLAY
                    "controls" -> MenuTab.CONTROLS
                    "graphics" -> MenuTab.GRAPHICS
                    "audio" -> MenuTab.AUDIO
                    else -> MenuTab.ENHANCEMENTS
                },
                sendDefault = o.optBoolean("force", false),
                tabLabel = o.optString("tab").takeIf { it.isNotEmpty() },
                tabGroup = o.optString("tab_group"),
                section = o.optString("section"),
            )
        }
    }

    /** A numeric range as a list of choices (the menu steps through values); long ranges are subsampled. */
    private fun rangeValues(o: JSONObject): List<Pair<String, String>> {
        if (!o.has("min") || !o.has("max")) return emptyList()
        val min = o.getDouble("min"); val max = o.getDouble("max")
        var step = o.optDouble("step", 1.0).takeIf { it > 0 } ?: 1.0
        while ((max - min) / step + 1 > MAX_STEPS) step *= 2
        val integral = listOf(min, max, step).all { it == Math.floor(it) }
        val unit = o.optString("unit")
        val out = mutableListOf<Pair<String, String>>()
        var v = min
        while (v <= max + 1e-9) {
            val text = if (integral) v.toLong().toString() else "%.2f".format(java.util.Locale.ROOT, v).trimEnd('0').trimEnd('.')
            out += text to (text + unit)
            v += step
        }
        return out
    }
}

// Our own per-game tweaks (CanonicalOptions + manifest "native_options") were folded into the ports' settings schemas
// on 2026-11-15 (owner): the skips are the ports' own options with our defaults, PD's two extras are schema entries
// of their own. Every option the menu shows now comes from the module's settings_schema.json (PortSchema).

/**
 * Settings the player set for one module, i.e. one port (files/settings/<module id>.json: canonical key to value).
 * Per module, not per ROM/game ID (owner 2026-11-13): two revisions of a game played by the same port share their
 * settings, and a game's decomp and recomp each have their own. MVP: choices made in the older per-game files
 * (files/games/<game id>/settings.json) are not migrated; only test devices have any. Anything not in the
 * file is the default; an explicit choice stays "set by you" even when it equals the
 * default (ARCHITECTURE settings model). The game process writes it from the in-game menu, the launcher reads it.
 */
class GameSettings(private val file: File) {
    fun load(): JSONObject = runCatching { JSONObject(file.readText()) }.getOrDefault(JSONObject())

    fun set(key: String, value: String) {
        // Atomic (util/AtomicFiles.kt): the old fixed ".tmp" name raced between the launcher and the game process.
        file.writeTextAtomic(load().put(key, value).toString(2))
    }

    /**
     * Drops the player's choice for [key], so the value falls back to the default.
     * Why remove instead of writing the default: an explicit choice stays "set by you" even when it equals the
     * default (ARCHITECTURE settings model), so "reset" has to mean "no choice", not "chose the default".
     * Added 2026-11-13 for the per-setting reset button in GameMenu (owner request).
     */
    fun remove(key: String) {
        val json = load()
        if (!json.has(key)) return
        json.remove(key)
        file.writeTextAtomic(json.toString(2))
    }

    companion object {
        /** One module's own layer: files/settings/<module id>.json (launch.json "settings_file" points here). */
        fun moduleFile(filesDir: File, moduleId: String) = File(filesDir, "settings/$moduleId.json")

        fun forModule(filesDir: File, moduleId: String) = GameSettings(moduleFile(filesDir, moduleId))

        /**
         * Resolved values and the menu description of the module's options (PortSchema), for launch.json ("settings",
         * "menu_options"): set for this game, else the default ("source": "game" or "default"). No All-games layer
         * since 2026-11-15; a stale files/settings.json from before is ignored, and so are stored values of the old
         * hand-made keys (sm64.skip_* etc.): they're simply no longer asked for.
         */
        fun resolve(file: File, portOptions: List<CanonicalOption>): Pair<JSONObject, JSONArray> {
            val user = GameSettings(file).load()
            val settings = JSONObject()
            val menu = JSONArray()
            // In the schema's order: the menu orders tabs by first appearance, and the port's menu order is the
            // design goal (2026-11-15).
            for (option in portOptions) {
                val key = option.key
                val (value, source) = when {
                    user.has(key) -> user.getString(key) to "game"
                    else -> option.default to "default"
                }
                if (source != "default" || option.sendDefault) settings.put(key, value)
                menu.put(describe(option, value, source))
            }
            return settings to menu
        }

        /** The menu's description of one option (GameMenu reads these fields; a reset returns to "default"). */
        fun describe(option: CanonicalOption, value: String, source: String): JSONObject = JSONObject()
            .put("key", option.key).put("label", option.label).put("detail", option.detail).put("tab", option.tab)
            .put("tab_label", option.tabLabel ?: MenuTab.label(option.tab))
            .put("tab_group", option.tabGroup).put("section", option.section)
            .put("value", value).put("default", option.default).put("source", source)
            .put("set_by_user", source == "game")
            .put("values", JSONArray().also { array ->
                option.values.forEach { (v, label) -> array.put(JSONObject().put("value", v).put("label", label)) }
            })
    }
}
