package app.portlauncher.mods

import org.json.JSONArray
import org.json.JSONObject

/**
 * Everything the Mods tab needs to know about a module's mods (owner 2026-11-16, docs/planning/MODS.md "Curated mods"),
 * built from its manifest (GameCatalog) and handed to the game process as JSON (Sdl2HostActivity.EXTRA_MODS_SPEC):
 * - [thunderstore]: the recomps' Thunderstore community (manifest "mod_sources"), browsed in the tab; empty = none;
 * - [curated]: our recommended list (manifest "mods.curated"), linked third-party mods;
 * - [sources]: where to get more (label, web page), opened in the browser (manifest "mods.sources");
 * - [builtin]: our compiled-in features as switchable mods (manifest "mods.builtin");
 * - [preinstall]: curated ids installed before the first launch (manifest "mods.preinstall");
 * - [files]: the port loads mod files (libultraship archives), so "Add a mod from a file" is offered.
 */
data class ModsSpec(
    val thunderstore: String = "",
    val curated: List<CuratedMod> = emptyList(),
    val sources: List<Pair<String, String>> = emptyList(),
    val builtin: List<BuiltinMod> = emptyList(),
    val preinstall: List<String> = emptyList(),
    val files: Boolean = false,
) {
    /** Whether the game has a Mods tab with content (else the tab says "coming soon"). */
    val any: Boolean get() = thunderstore.isNotEmpty() || curated.isNotEmpty() || sources.isNotEmpty() || builtin.isNotEmpty() || files

    fun toJson(): JSONObject = JSONObject().put("thunderstore", thunderstore)
        .put("curated", JSONArray().also { a -> curated.forEach { a.put(it.toJson()) } })
        .put("sources", JSONArray().also { a -> sources.forEach { (l, u) -> a.put(JSONObject().put("label", l).put("url", u)) } })
        .put("builtin", JSONArray().also { a -> builtin.forEach { a.put(it.toJson()) } })
        .put("preinstall", JSONArray(preinstall)).put("files", files)

    companion object {
        fun fromJson(o: JSONObject?): ModsSpec {
            if (o == null) return ModsSpec()
            val sources = o.optJSONArray("sources")
            val pre = o.optJSONArray("preinstall")
            return ModsSpec(
                thunderstore = o.optString("thunderstore"),
                curated = CuratedMod.listFromJson(o.optJSONArray("curated")),
                sources = (0 until (sources?.length() ?: 0)).map { sources!!.getJSONObject(it) }.map { it.optString("label") to it.optString("url") },
                builtin = BuiltinMod.listFromJson(o.optJSONArray("builtin")),
                preinstall = (0 until (pre?.length() ?: 0)).map { pre!!.getString(it) },
                files = o.optBoolean("files", false),
            )
        }

        /** From a module manifest: "mods" plus a Thunderstore "mod_sources" entry; [files] for libultraship ports. */
        fun fromManifest(manifest: JSONObject): ModsSpec {
            val mods = manifest.optJSONObject("mods") ?: JSONObject()
            val ts = manifest.optJSONArray("mod_sources")?.let { a ->
                (0 until a.length()).map { a.getJSONObject(it) }.firstOrNull { it.optString("type") == "thunderstore" }?.optString("community")
            }.orEmpty()
            return fromJson(mods).copy(thunderstore = ts, files = manifest.optString("family") == "libultraship")
        }
    }
}
