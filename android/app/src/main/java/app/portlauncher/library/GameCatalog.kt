package app.portlauncher.library

import android.content.Context
import android.util.Log
import app.portlauncher.core.CoreBridge
import org.json.JSONObject

/** A game the bundled modules can run. */
data class CatalogGame(
    val id: String,
    val title: String,
    val region: String,
    val revision: String,
    val modules: List<String>,
    /** The N64 header's 4-character game code ("NZSE"): the same for every revision and for romhacks of the game. */
    val gameCode: String = "",
    /** The ROM SHA-1s this entry accepts (manifest "sha1"); RomM matching uses them (RommSource). */
    val sha1s: List<String> = emptyList(),
)

/** A module's Thunderstore mod source (manifest "mod_sources", the recomps): its [community]. */
data class ModSource(val type: String, val community: String)

/** One mod of a preset (manifest "mod_presets"): a Thunderstore package and option values to start it with. */
data class PresetMod(val pkg: String, val options: JSONObject)


/**
 * Where a module comes from (module server, first slice 2026-11-13): inside the APK, downloaded onto the device,
 * or offered by the module server's index and not downloaded yet (library row shows Download).
 */
enum class ModuleSource { BUNDLED, INSTALLED, AVAILABLE }

/**
 * How finished a module is (ROADMAP item 9, MODULE_SPEC §11), one scale for every module so the library can mark the
 * untested ones. Manifest "maturity"; absent or unknown = EXPERIMENTAL. Why default to the lowest level: a module that
 * doesn't say anything about itself hasn't been reviewed, and over-promising in the library is worse than
 * under-promising (a player picking an "Integrated" game expects our menu and direct boot to just work).
 * - EXPERIMENTAL: boots, with known issues (or simply not checked on a device yet).
 * - INTEGRATED: direct boot, our menu, canonical settings, curated defaults.
 * - COMPLETE: every setting in our menu, no escape hatch (own launcher / native menu), save states.
 */
enum class Maturity(val label: String) {
    EXPERIMENTAL("Experimental"), INTEGRATED("Integrated"), COMPLETE("Complete");

    companion object {
        fun parse(value: String): Maturity = entries.firstOrNull { it.name.equals(value, ignoreCase = true) } ?: EXPERIMENTAL
    }
}

data class ModuleInfo(
    val id: String,
    val name: String,
    val source: ModuleSource = ModuleSource.BUNDLED,
    /** Manifest "version" of what's on the device (or offered, for AVAILABLE). */
    val version: String = "",
    /** The module server's entry when it offers this module: download source, and newer version = update. */
    val remote: app.portlauncher.modules.RemoteModule? = null,
    /** Manifest "family": decomp-based families come before "n64recomp" as a game's default (ROADMAP decisions). */
    val family: String = "",
    val modSources: List<ModSource>,
    /** The curated mods behind the game's Modern default (docs/planning/MODS.md). Original runs without them. */
    val modernMods: List<PresetMod> = emptyList(),
    /**
     * Manifest "native_launcher": the port has its own launcher screen worth offering (the recomps: RecompFrontend's
     * and JFG's), started with launch.json "ui": "native". Selecting such a port in the library asks "own launcher or
     * straight into the game" (owner request 2026-11-13). Decomps leave it out: their settings belong in our menu.
     */
    val hasLauncher: Boolean = false,
    /** Manifest "maturity" (see [Maturity]); shown as a library badge. */
    val maturity: Maturity = Maturity.EXPERIMENTAL,
    /**
     * Manifest "game_lib" (MODULE_SPEC §2): a split recomp module whose libgame.so the device builds from the ROM
     * before the first launch (LibraryController.prepareGameLib). Null for every other module.
     */
    val gameLib: app.portlauncher.recomp.GameLibSpec? = null,
    /**
     * Manifest "select_to_game": Select reaches the game instead of being swallowed (Sdl2HostActivity). Only for a
     * port whose controller layout we bind something to Select: SoH, where the Nintendo-style button-glyph mod we
     * pre-install shows N64 L on Select/Minus (owner 2026-11-16, "follow what the mod shows").
     */
    val selectToGame: Boolean = false,
    /** The Mods tab's content (manifest "mods" and "mod_sources"; ModsSpec): curated list, sources, built-in mods. */
    val mods: app.portlauncher.mods.ModsSpec = app.portlauncher.mods.ModsSpec(),
)

/** Loads the bundled module manifests into the core's game DB and answers lookups. */
class GameCatalog private constructor(private val modules: Map<String, ModuleInfo>, private val allGames: List<CatalogGame>) {
    /** Every game the bundled modules can run, by title (one entry per game ID). */
    fun games(): List<CatalogGame> = allGames

    fun find(sha1: String): CatalogGame? = CoreBridge.gameDbFind(sha1)?.let { text ->
        val json = JSONObject(text)
        val modules = json.getJSONArray("modules")
        CatalogGame(
            id = json.getString("id"),
            title = json.getString("title"),
            region = json.optString("region"),
            revision = json.optString("revision"),
            modules = (0 until modules.length()).map { modules.getString(it) }.sortedBy { preference(it) },
        )
    }

    /** Decomp ports first, recomps after: the first module is the game's default (principle 2). */
    private fun preference(moduleId: String): Int = if (modules[moduleId]?.family == "n64recomp") 1 else 0

    fun moduleName(moduleId: String): String = modules[moduleId]?.name ?: moduleId

    fun module(moduleId: String): ModuleInfo? = modules[moduleId]

    fun modules(): Collection<ModuleInfo> = modules.values

    companion object {
        @Volatile private var instance: GameCatalog? = null

        fun get(context: Context): GameCatalog = instance ?: synchronized(this) {
            instance ?: load(context).also { instance = it }
        }

        /** Rebuilds the catalogue after modules were downloaded or the module index changed. */
        fun reload(context: Context): GameCatalog = synchronized(this) { load(context).also { instance = it } }

        private fun load(context: Context): GameCatalog {
            val modules = mutableMapOf<String, ModuleInfo>()
            val games = linkedMapOf<String, CatalogGame>()
            // Sources in priority order: the APK's modules, downloaded ones, then the module server's offer for the
            // rest. A module id is taken from the first source that has it; the index entry still rides along as
            // `remote` so a downloaded module can show an update.
            val repo = app.portlauncher.modules.ModuleRepository(context)
            val remote = repo.cachedIndex().associateBy { it.id }
            val sources = mutableListOf<Triple<String, String, ModuleSource>>() // id, manifest text, source
            for (id in context.assets.list("modules").orEmpty()) {
                runCatching { context.assets.open("modules/$id/manifest.json").bufferedReader().readText() }.getOrNull()
                    ?.let { sources += Triple(id, it, ModuleSource.BUNDLED) }
            }
            for (dir in repo.installedRoot.listFiles().orEmpty().filter { it.isDirectory && !it.name.startsWith(".") }) {
                runCatching { java.io.File(dir, "manifest.json").readText() }.getOrNull()
                    ?.let { sources += Triple(dir.name, it, ModuleSource.INSTALLED) }
            }
            for (r in remote.values) sources += Triple(r.id, r.manifest.toString(), ModuleSource.AVAILABLE)
            val seen = mutableSetOf<String>()
            for ((module, text, source) in sources) {
                if (!seen.add(module)) continue
                val error = CoreBridge.gameDbAddManifest(text)
                if (error.isNotEmpty()) {
                    Log.w("plm-library", "Skipping module $module: $error")
                    continue
                }
                val json = JSONObject(text)
                json.optJSONArray("games")?.let { array ->
                    for (i in 0 until array.length()) {
                        val g = array.getJSONObject(i)
                        val id = g.getString("id")
                        val known = games[id]
                        games[id] = known?.copy(modules = known.modules + module) ?: CatalogGame(
                            id = id,
                            title = g.optString("title", id),
                            region = g.optString("region"),
                            revision = g.optString("revision"),
                            modules = listOf(module),
                            gameCode = g.optString("game_code"),
                            sha1s = g.optJSONArray("sha1")?.let { a -> (0 until a.length()).map { a.getString(it) } }.orEmpty(),
                        )
                    }
                }
                val sources = json.optJSONArray("mod_sources")
                modules[module] = ModuleInfo(
                    id = module,
                    name = json.optString("name", module),
                    source = source,
                    version = json.optString("version"),
                    remote = remote[module],
                    family = json.optString("family"),
                    hasLauncher = json.optBoolean("native_launcher", false),
                    selectToGame = json.optBoolean("select_to_game", false),
                    mods = app.portlauncher.mods.ModsSpec.fromManifest(json),
                    // For AVAILABLE modules this is the index's embedded manifest, so the badge shows before download.
                    maturity = Maturity.parse(json.optString("maturity")),
                    gameLib = json.optJSONObject("game_lib")?.let { g ->
                        app.portlauncher.recomp.GameLibSpec(g.optString("build"), g.optString("script", "build_from_rom.sh"))
                    },
                    modSources = if (sources == null) emptyList() else (0 until sources.length()).map { i ->
                        val source = sources.getJSONObject(i)
                        ModSource(source.optString("type"), source.optString("community"))
                    }.filter { it.type == "thunderstore" && it.community.isNotEmpty() },
                    modernMods = json.optJSONObject("mod_presets")?.optJSONArray("modern")?.let { array ->
                        (0 until array.length()).map { i ->
                            val mod = array.getJSONObject(i)
                            PresetMod(mod.getString("package"), mod.optJSONObject("options") ?: JSONObject())
                        }
                    }.orEmpty(),
                )
            }
            // Decomp ports first, recomps after (the first module is the game's default).
            val sorted = games.values.map { g -> g.copy(modules = g.modules.sortedBy { if (modules[it]?.family == "n64recomp") 1 else 0 }) }
            return GameCatalog(modules, sorted.sortedBy { it.title })
        }
    }
}
