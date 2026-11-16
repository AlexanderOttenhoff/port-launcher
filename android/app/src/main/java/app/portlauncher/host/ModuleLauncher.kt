package app.portlauncher.host

import android.content.Context
import android.content.Intent
import app.portlauncher.library.GameSettings
import app.portlauncher.modules.ModuleRepository
import app.portlauncher.util.writeTextAtomic
import java.io.File
import org.json.JSONObject

/**
 * Prepares a bundled module's directories and launch.json, then starts its host activity (MODULE_SPEC §4.1).
 * Settings travel in launch.json itself (`settings`, `menu_options` + `settings_file`).
 */
object ModuleLauncher {
    /** Copies the module's read-only data out of the APK once per installed app version. Slow: call off the UI thread. */
    fun installData(context: Context, moduleId: String): File {
        // A downloaded module (module server) already has its read-only data unpacked in its package directory.
        // Golden Balloon's zip has no data/ entries at all (it reads everything from the ROM), so make sure the
        // folder exists, as the bundled path below does, rather than hand the module a missing directory.
        ModuleRepository(context).installedDir(moduleId)?.let { return File(it, "data").apply { mkdirs() } }
        val dataDir = File(context.filesDir, "modules/$moduleId/data")
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        val stamp = "${info.longVersionCode}-${info.lastUpdateTime}"
        val stampFile = File(dataDir, ".installed")
        if (stampFile.isFile && stampFile.readText() == stamp) {
            return dataDir
        }
        dataDir.deleteRecursively()
        // A module with no read-only data (Golden Balloon reads everything from the ROM) has no data/ folder in the
        // APK at all: an empty directory isn't packaged, and opening it as a file throws.
        if (context.assets.list("modules/$moduleId/data").orEmpty().isNotEmpty()) {
            copyAssetTree(context, "modules/$moduleId/data", dataDir)
        }
        dataDir.mkdirs()
        // Atomic (util/AtomicFiles.kt); written last, so a crash mid-copy re-copies on the next launch.
        stampFile.writeTextAtomic(stamp)
        return dataDir
    }

    /** [direct]: boot straight into the game (MODULE_SPEC §4); false opens the port's own launcher (fallback). */
    fun launch(
        context: Context,
        moduleId: String,
        gameId: String,
        title: String,
        rom: File,
        direct: Boolean = true,
        mods: JSONObject? = null,
        settings: JSONObject? = null,
        menuOptions: org.json.JSONArray? = null,
        /** The module's mods (ModsSpec: curated list, sources, built-in mods), for the in-game Mods tab (ModsPage). */
        modsSpec: app.portlauncher.mods.ModsSpec = app.portlauncher.mods.ModsSpec(),
        /** Manifest "select_to_game" (ModuleInfo.selectToGame): Select passes through to the port. */
        selectToGame: Boolean = false,
        /** The module's settings profiles, for the in-game Settings tab. */
        profiles: List<app.portlauncher.library.SettingsProfile> = emptyList(),
    ) {
        val dataDir = installData(context, moduleId)
        val root = File(context.filesDir, "modules/$moduleId")
        val dirs = listOf("config", "save", "mods", "logs").associateWith { File(root, it).apply { mkdirs() } }
        val cacheDir = File(context.cacheDir, "modules/$moduleId").apply { mkdirs() }
        val texturesDir = File(context.getExternalFilesDir(null), "textures/$moduleId").apply { mkdirs() }
        // One folder per launch, old ones pruned (Sessions.kt; they used to pile up forever, SLOP_AUDIT §1.2).
        val session = Sessions.create(context.filesDir)

        val launch = JSONObject()
            .put("abi", 0)
            .put("platform", "android-arm64")
            .put("module", moduleId)
            .put("game", gameId)
            .put("rom", rom.absolutePath)
            .put(
                "dirs",
                JSONObject()
                    .put("data", dataDir.absolutePath)
                    .put("config", dirs.getValue("config").absolutePath)
                    .put("save", dirs.getValue("save").absolutePath)
                    .put("cache", cacheDir.absolutePath)
                    .put("mods", dirs.getValue("mods").absolutePath)
                    .put("textures", texturesDir.absolutePath)
                    .put("logs", dirs.getValue("logs").absolutePath),
            )
            // No settings_in / settings_out (SLOP_AUDIT §1.2, removed 2026-11-14): a spec leftover. We wrote an empty
            // settings_in.json every launch and no shim ever read either path; settings are in this JSON instead.
            .put("locale", context.resources.configuration.locales[0].toLanguageTag())
            .put("device_profile", "unknown")
            .put("ui", if (direct) "direct" else "native")
            // Set by the host before the module's library loads: some ports read them in global constructors
            // (libultraship ports compute their app directory at load time).
            .put(
                "env",
                JSONObject()
                    .put("SHIP_HOME", dirs.getValue("config").absolutePath)
                    .put("PLM_BUNDLE", dataDir.absolutePath)
                    .put("PLM_ROM", rom.absolutePath),
            )
        // Split recomp modules (docs/planning/ON_DEVICE_RECOMP.md): the game code built from the player's ROM lives
        // apart from the module, in files/recomp/<module>/libgame.so. Spike 2 copies it there by hand; the on-device
        // build will write it there. Unsplit modules ignore the key.
        val gameLib = File(context.filesDir, "recomp/$moduleId/libgame.so")
        if (gameLib.isFile) launch.put("game_lib", gameLib.absolutePath)
        if (mods != null) launch.put("mods", mods)
        if (selectToGame) launch.put("select_to_game", true)
        if (profiles.isNotEmpty()) launch.put("profiles", app.portlauncher.library.SettingsProfile.listToJson(profiles))
        if (settings != null) launch.put("settings", settings)
        if (menuOptions != null) {
            launch.put("menu_options", menuOptions)
            launch.put("settings_file", GameSettings.moduleFile(context.filesDir, moduleId).absolutePath)
        }
        val launchFile = File(session, "launch.json").apply { writeTextAtomic(launch.toString(2)) }

        context.startActivity(
            Intent(context, Sdl2HostActivity::class.java)
                .putExtra(Sdl2HostActivity.EXTRA_MODULE_ID, moduleId)
                .putExtra(Sdl2HostActivity.EXTRA_LAUNCH_JSON, launchFile.absolutePath)
                .putExtra(Sdl2HostActivity.EXTRA_TITLE, title)
                .putExtra(Sdl2HostActivity.EXTRA_DIRECT, direct)
                .putExtra(Sdl2HostActivity.EXTRA_MODS_SPEC, modsSpec.toJson().toString())
                // Downloaded modules load from app storage by full path; bundled ones by name from the APK.
                .putExtra(Sdl2HostActivity.EXTRA_MODULE_LIB,
                    ModuleRepository(context).let { repo ->
                        if (repo.installedDir(moduleId) != null) repo.installedLibrary(moduleId).absolutePath else null
                    }),
        )
    }

    private fun copyAssetTree(context: Context, path: String, destination: File) {
        val children = context.assets.list(path).orEmpty()
        if (children.isEmpty()) {
            destination.parentFile?.mkdirs()
            context.assets.open(path).use { input -> destination.outputStream().use { input.copyTo(it) } }
            return
        }
        destination.mkdirs()
        for (child in children) {
            copyAssetTree(context, "$path/$child", File(destination, child))
        }
    }
}
