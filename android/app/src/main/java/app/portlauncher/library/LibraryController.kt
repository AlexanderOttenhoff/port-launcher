package app.portlauncher.library

import app.portlauncher.util.showTracked
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import app.portlauncher.core.CoreBridge
import app.portlauncher.host.GameMenu
import app.portlauncher.host.MenuAction
import app.portlauncher.host.MenuScope
import app.portlauncher.host.ModsPage
import app.portlauncher.host.FlagToggle
import app.portlauncher.util.AppFlags
import app.portlauncher.downloads.Downloads
import app.portlauncher.util.UiState
import app.portlauncher.host.ModuleLauncher
import app.portlauncher.host.SourcesPage
import app.portlauncher.mods.ModEntry
import app.portlauncher.mods.ModStore
import app.portlauncher.mods.Thunderstore
import app.portlauncher.modules.ModuleRepository
import app.portlauncher.recomp.BuildRequest
import app.portlauncher.recomp.ExecBackend
import app.portlauncher.recomp.GameLibSpec
import app.portlauncher.recomp.RecompBackend
import app.portlauncher.recomp.RecompBuildException
import app.portlauncher.recomp.RecompCache
import app.portlauncher.recomp.RecompCancelledException
import app.portlauncher.recomp.RecompToolkit
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * Where the ROM a port needs is (ROADMAP "Library rebuild", badge 1; owner 2026-11-12/13). Three states even though
 * ROMM can't happen yet: the RomM integration doesn't exist, so nothing produces it today. It's here so the badge, the
 * sorting and the row UI are built for all three now and RomM only has to set it (owner request 2026-11-13).
 */
enum class RomState { LOCAL, ROMM, NONE }

/**
 * One row of the library: one **module** (port), whatever ROM revisions it accepts (owner 2026-11-13: "one entry per
 * module, not per ROM"). Lighthouse is one row whether the player has BK US 1.0, 1.1 or both; a game with a decomp
 * and a recomp (Banjo-Kazooie: Lighthouse and BanjoRecomp) gets one row per module. [game] is the module's game as
 * the player has it (the playable revision), else its first catalogue entry for the title.
 *
 * [playable] is set when the ROM is in a scanned folder ([rom] == LOCAL); then A starts it. [isRecomp] drives the
 * Decomp/Recomp badge (manifest "family" == "n64recomp" is the secondary repository's kind) and the recomp-only
 * escape hatch (a popup on select: the port's own launcher or straight into the game; MENU.md scope note: recomps
 * may keep their own settings screens).
 * [detail] says what's missing for rows without a ROM (which revisions, or "your copy is another version").
 */
data class PortEntry(
    val game: CatalogGame,
    val moduleId: String,
    val moduleName: String,
    val isRecomp: Boolean,
    /** The port has its own launcher (manifest "native_launcher"): selecting the row asks which to open. */
    val hasLauncher: Boolean,
    val rom: RomState,
    val playable: PlayableGame?,
    val detail: String = "",
    /**
     * Where the module is (module server, 2026-11-13). AVAILABLE = offered by the module server, not downloaded:
     * selecting the row offers the download first. [downloadSize] is the package size in bytes for the prompt.
     */
    val source: ModuleSource = ModuleSource.BUNDLED,
    val downloadSize: Long = 0,
    /**
     * A downloaded module whose server version differs from the installed one. Selecting the row then asks "Update"
     * or "Play this version" (ROADMAP item 10) instead of silently launching the old one: the player decides when a
     * download happens (they may be offline or about to play on a train), and nothing updates behind their back.
     */
    val updateAvailable: Boolean = false,
    /** Manifest "maturity" (ROADMAP item 9): drawn as a badge so untested modules are marked. */
    val maturity: Maturity = Maturity.EXPERIMENTAL,
    /** Installed version and the module server's version, for the update prompt's text (empty when unknown). */
    val version: String = "",
    val remoteVersion: String = "",
    /**
     * Rows without a ROM: what the player must provide, in full (owner 2026-11-16: "clearly display what ROM files
     * they need"): the game and accepted revisions, file types, SHA-1s to check a copy with, why theirs doesn't fit,
     * and where to put it. Shown on the game page's disabled "Launch game".
     */
    val romHelp: String = "",
) {
    val needsDownload: Boolean get() = source == ModuleSource.AVAILABLE
    /** Stable LazyColumn key: the module and (for total conversions) the mod that plays as its own entry. */
    val key: String get() = moduleId + (playable?.gameMode?.let { "#${it.id}" } ?: "")
    val title: String get() = playable?.title ?: game.title
}

/**
 * A game the user can start: the best bundled module for it and the file it was found in. With [gameMode] set it's
 * a total-conversion mod that plays as its own entry (docs/planning/MODS.md), on the base game's ROM.
 */
data class PlayableGame(
    val game: CatalogGame,
    val moduleId: String,
    val moduleName: String,
    val file: ScannedFile,
    val rom: ScannedRom,
    val gameMode: ModEntry? = null,
    val hasMods: Boolean = false,
) {
    val key: String get() = game.id + (gameMode?.let { "#${it.id}" } ?: "")
    val title: String get() = gameMode?.displayName ?: game.title
}



sealed interface LibraryStatus {
    data object Idle : LibraryStatus
    data class Scanning(val done: Int, val total: Int) : LibraryStatus
    data class Preparing(val title: String) : LibraryStatus
    data class Problem(val message: String) : LibraryStatus
    /** A module download in progress (module server); [progress] 0..1. */
}

/**
 * The full-screen first-launch flow for split recomp modules (PreparingScreen; ROADMAP item 13). Null when nothing is
 * being prepared. Kept apart from [LibraryStatus] because it replaces the whole screen, and because the error state
 * has to stay up (with its log) until the player dismisses it, while the status line is transient.
 */
sealed interface PrepareState {
    data class Installing(val fraction: Float) : PrepareState
    data class Building(val title: String, val phase: String, val fraction: Float) : PrepareState
    data class Failed(val title: String, val message: String, val logTail: String, val logPath: String = "") : PrepareState
}

data class LibraryUi(
    val folders: List<String> = emptyList(),
    val playable: List<PlayableGame> = emptyList(),
    /** The library list (rows per port, see PortEntry). [playable] stays for settings scopes and the frontend path. */
    val ports: List<PortEntry> = emptyList(),
    val status: LibraryStatus = LibraryStatus.Idle,
)

/** Canonical values of the Modern preset (ROADMAP principle 4: right-stick camera, look not inverted). */

/** The library's state and actions. Long work runs on Dispatchers.IO; callers launch from a UI scope. */
class LibraryController(private val context: Context) {
    private val store = LibraryStore(context)
    // A getter, not a field: the catalogue is rebuilt after module downloads or index changes (GameCatalog.reload).
    val catalog: GameCatalog get() = GameCatalog.get(context)
    private val moduleRepo = ModuleRepository(context)
    private var data = store.load()

    var ui by mutableStateOf(build(data, LibraryStatus.Idle))
        private set

    /** See [PrepareState]. MainActivity/LaunchActivity show PreparingScreen instead of their content while set. */
    var prepare by mutableStateOf<PrepareState?>(null)
        private set

    /** The backend of the build in progress, for [cancelPrepare]. One build at a time (one game process at a time). */
    @Volatile private var backend: RecompBackend? = null

    suspend fun addFolder(treeUri: Uri) {
        context.contentResolver.takePersistableUriPermission(treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        if (treeUri.toString() !in data.folders) {
            data = data.copy(folders = data.folders + treeUri.toString())
        }
        ui = build(data, LibraryStatus.Scanning(0, 0)) // leave first-run right away
        settingsMenu?.refresh() // the ROM sources page lists the new folder while the scan runs
        rescan()
    }

    /**
     * Drops a ROM folder (settings → ROM sources, owner 2026-11-13) and its read permission, then rescans so its games
     * leave the library. Nothing on disk is touched.
     */
    suspend fun removeFolder(treeUri: String) {
        runCatching {
            context.contentResolver.releasePersistableUriPermission(Uri.parse(treeUri), Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        data = data.copy(folders = data.folders - treeUri)
        settingsMenu?.refresh()
        rescan()
    }

    /** The open home-screen settings page, so folder changes can refresh its ROM sources tab. */
    private var settingsMenu: GameMenu? = null

    /** A game page or the app settings is showing (the library doesn't reopen a remembered page over it). */
    val menuOpen: Boolean get() = settingsMenu != null

    /**
     * Back to the bare library (owner 2026-11-16): opening the app from its home-screen icon closes every menu (game
     * page, app Settings, their popups) and forgets the remembered open page, so it doesn't reopen either. Switching
     * back through Recents doesn't come here and keeps what was open. [homeRequests] tells the Compose screen to
     * close its own popups (downloads, download/update prompts). The library's row position stays: it isn't a menu.
     */
    var homeRequests by mutableStateOf(0)
        private set

    fun closeMenus() {
        uiState.putString("open_page", null)
        app.portlauncher.util.OpenDialogs.dismissAll()
        settingsMenu = null
        homeRequests++
    }

    /** Where the player was (Settings → General → "Remember position"). */
    val uiState by lazy { UiState(context.filesDir) }

    /**
     * Fetches the module server's index (on start and on Rescan). New or changed modules rebuild the catalogue and
     * the list; offline keeps the cached index, so known modules stay listed.
     */
    suspend fun refreshModules() {
        val changed = withContext(Dispatchers.IO) { moduleRepo.refreshIndex() }
        if (changed) {
            withContext(Dispatchers.IO) { GameCatalog.reload(context) }
            ui = build(data, ui.status)
        }
    }

    /**
     * Downloads and installs [port]'s module from the module server, with progress in the status line, then rebuilds
     * the catalogue so the row becomes playable. Errors go to the status line; nothing half-installed remains.
     */
    /**
     * The library's "Update all" button (owner 2026-11-15): every downloaded module with a newer version on the module
     * server, one after another (one module per download, as a single select would do). Stops at the first failure,
     * whose message stays on the status line; the rest keep their Update badge.
     */
    fun updateAll() {
        ui.ports.filter { it.updateAvailable }.distinctBy { it.moduleId }.forEach { download(it) }
    }

    /** Modules queued or downloading, so a second press doesn't queue the same module twice. */
    private val downloading = mutableSetOf<String>()

    /**
     * Queues the module's download or update in the launcher's download queue (Downloads, owner 2026-11-15: one queue
     * for everything, shown at the bottom of the library). The library rebuilds when it finishes.
     */
    fun download(port: PortEntry) {
        val remote = catalog.module(port.moduleId)?.remote ?: return
        if (!downloading.add(port.moduleId)) return
        val title = if (port.updateAvailable) "${port.moduleName} (update)" else port.moduleName
        Downloads.enqueue(title, { progress ->
            moduleRepo.install(remote) { p -> progress(p) }
            GameCatalog.reload(context)
        }) { error ->
            downloading.remove(port.moduleId)
            ui = build(data, error?.let {
                Log.w("plm-library", "download ${port.moduleId}: ${it.message}")
                LibraryStatus.Problem("Couldn't download ${port.moduleName}. Check the connection and try again.")
            } ?: LibraryStatus.Idle)
        }
    }

    /**
     * Removes a downloaded module (ROADMAP item 10): deletes files/installed-modules/<id>/ only. The module's saves,
     * config and mods (files/modules/<id>/) and its settings stay, so downloading it again later picks up where the
     * player left off; that's also why the confirmation text says so. The catalogue is rebuilt afterwards: if the
     * module server's (cached) index still offers the module, its row goes back to the download state; otherwise it
     * leaves the library. Bundled modules can't be removed (they're part of the APK), so callers only offer this for
     * ModuleSource.INSTALLED.
     */
    suspend fun removeModule(moduleId: String) {
        val name = catalog.moduleName(moduleId)
        val ok = withContext(Dispatchers.IO) {
            moduleRepo.remove(moduleId).also { GameCatalog.reload(context) }
        }
        ui = build(data, if (ok) LibraryStatus.Idle else LibraryStatus.Problem("Couldn't remove $name."))
    }

    suspend fun rescan() {
        refreshModules()
        ui = ui.copy(status = LibraryStatus.Scanning(0, 0))
        val scanner = LibraryScanner(context)
        val files = withContext(Dispatchers.IO) {
            scanner.scan(data.folders, data.files) { done, total ->
                ui = ui.copy(status = LibraryStatus.Scanning(done, total))
            }
        }
        data = data.copy(files = files)
        withContext(Dispatchers.IO) { store.save(data) }
        ui = build(data, LibraryStatus.Idle)
    }

    /**
     * Process-lifetime scope for [playAsync]. Why: the library screen used to launch play() from its
     * rememberCoroutineScope, but the Preparing screen replaces the library screen, which leaves the composition and
     * cancels that scope: the first-launch build was silently killed the moment its screen appeared (found in the
     * emulator test, 2026-11-14). The controller lives as long as MainActivity's lazy, so its scope does too.
     */
    private val scope = kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.SupervisorJob() + Dispatchers.Main)

    /** [play] in the controller's own scope, so it survives the screen that started it (see [scope]). */
    fun playAsync(activity: Context, item: PlayableGame, direct: Boolean = true) {
        scope.launch { play(activity, item, direct) }
    }

    /**
     * Imports the ROM on first play (normalised .z64 in app storage), then starts the module from [activity], so
     * the game opens in the launcher's task.
     */
    suspend fun play(activity: Context, item: PlayableGame, direct: Boolean = true) {
        ui = ui.copy(status = LibraryStatus.Preparing(item.title))
        val rom = File(context.filesDir, "games/${item.game.id}/rom.z64")
        val imported = withContext(Dispatchers.IO) { rom.isFile || importRom(item, rom) }
        if (!imported) {
            ui = ui.copy(status = LibraryStatus.Problem("Couldn't read ${item.file.name}. Rescan the library and try again."))
            return
        }
        val mods = withContext(Dispatchers.IO) {
            ModuleLauncher.installData(context, item.moduleId)
            if (!item.hasMods) return@withContext null
            // libultraship ports keep their archives in SHIP_HOME/mods (ModStore lus mode; spec.files). This store was
            // built in recomp mode before 2026-11-16, so the pre-install's archive install was refused (silently).
            val store = ModStore(context, item.moduleId, catalog.module(item.moduleId)?.mods?.files == true)
            fetchModernMods(item, store)
            fetchPreinstall(item, store)
            store.launchState(curated(item.moduleId), item.gameMode?.id).toJson(item.gameMode?.id)
        }
        // Split recomp modules: build the game code from this ROM first, once (ON_DEVICE_RECOMP.md). Before the
        // settings work below only because a failed or cancelled build should change nothing else.
        val gameLib = catalog.module(item.moduleId)?.gameLib
        if (gameLib?.onDevice == true && !prepareGameLib(item, gameLib)) {
            ui = ui.copy(status = LibraryStatus.Idle)
            return
        }
        val portOptions = withContext(Dispatchers.IO) { portSchema(item.moduleId) }
        val (resolved, menuOptions) = if (portOptions.isEmpty()) null to null else withContext(Dispatchers.IO) {
            GameSettings.resolve(GameSettings.moduleFile(context.filesDir, item.moduleId), portOptions)
        }
        // Built-in mods (ModsSpec.builtin) are port config keys, always sent with the settings (on or off).
        val modsSpec = catalog.module(item.moduleId)?.mods ?: app.portlauncher.mods.ModsSpec()
        val builtin = app.portlauncher.mods.BuiltinModState(context.filesDir, item.moduleId).settings(modsSpec.builtin)
        val settings = if (builtin.isEmpty()) resolved else (resolved ?: org.json.JSONObject()).also { o -> builtin.forEach { (k, v) -> o.put(k, v) } }
        ModuleLauncher.launch(activity, item.moduleId, item.game.id, item.title, rom, direct, mods, settings, menuOptions,
            modsSpec,
            selectToGame = catalog.module(item.moduleId)?.selectToGame == true,
            profiles = withContext(Dispatchers.IO) { portProfiles(item.moduleId) })
        ui = ui.copy(status = LibraryStatus.Idle)
    }

    /**
     * The module's own settings schema (PortSchema, MENU.md design of 2026-11-14): data/settings_schema.json in the
     * downloaded module's package, else in the APK's bundled module assets. Read straight from there (not the
     * unpacked data copy), so the home-screen settings page works before the game has ever been launched.
     */
    private fun portSchema(moduleId: String): List<CanonicalOption> = schemaText(moduleId)?.let(PortSchema::parse).orEmpty()

    /** The module's settings profiles (schema "profiles", SettingsProfile). */
    private fun portProfiles(moduleId: String): List<app.portlauncher.library.SettingsProfile> =
        schemaText(moduleId)?.let(PortSchema::parseProfiles).orEmpty()

    private fun schemaText(moduleId: String): String? {
        val installed = ModuleRepository(context).installedDir(moduleId)
            ?.let { File(it, "data/settings_schema.json") }?.takeIf { it.isFile }
        return installed?.readText()
            ?: runCatching { context.assets.open("modules/$moduleId/data/settings_schema.json").bufferedReader().use { it.readText() } }.getOrNull()
    }

    /**
     * Makes sure files/recomp/<module>/libgame.so is built from this ROM for this module and toolkit version
     * (RecompCache), building it if not, with the Preparing screen up. Returns true when the game can launch.
     * False = failed (PrepareState.Failed stays up until the player closes it) or cancelled (prepare cleared).
     *
     * Order of checks, and why:
     *  1. a side-loaded toolkit tarball waiting (debug builds) is installed first, even when the library is fresh:
     *     the developer pushed it to test it, and its new version then invalidates the cache below;
     *  2. fresh cache -> launch; this is every launch after the first, and costs two small file reads;
     *  3. no toolkit at all but a library built for this ROM and module -> launch anyway: the toolkit may have been
     *     deleted to free space, and the code it built is still right;
     *  4. otherwise build.
     */
    private suspend fun prepareGameLib(item: PlayableGame, spec: GameLibSpec): Boolean {
        val toolkit = RecompToolkit(context)
        val cache = RecompCache(context, item.moduleId)
        val title = item.game.title
        val moduleVersion = catalog.module(item.moduleId)?.version.orEmpty()
        toolkit.pendingPickup(app.portlauncher.BuildConfig.DEBUG)?.let { tarball ->
            prepare = PrepareState.Installing(0f)
            val installed = withContext(Dispatchers.IO) {
                runCatching { toolkit.install(tarball) { f -> prepare = PrepareState.Installing(f) } }
            }
            installed.exceptionOrNull()?.let { e ->
                Log.w("plm-recomp", "toolkit install from $tarball failed", e)
                prepare = PrepareState.Failed(title, "Couldn't install the build tools from ${tarball.path}: ${e.message}", "")
                return false
            }
        }
        val toolkitVersion = toolkit.version()
        if (toolkitVersion == null) {
            if (cache.matchesIgnoringToolkit(item.rom.sha1, moduleVersion)) { prepare = null; return true }
            prepare = PrepareState.Failed(title,
                "$title is built on this device from your copy of the game the first time you play it, and the " +
                    "build tools aren't installed. They aren't downloadable yet; in a debug build, push " +
                    "${RecompToolkit.TARBALL} to " +
                    "${toolkit.pickupFiles().first().parent} and start the game again " +
                    "(docs/planning/ON_DEVICE_RECOMP.md, \"Installing the toolkit\").", "")
            return false
        }
        val key = cache.key(item.rom.sha1, moduleVersion, toolkitVersion)
        if (cache.isFresh(key)) { prepare = null; return true }

        val rom = File(context.filesDir, "games/${item.game.id}/rom.z64")
        val scratch = File(context.cacheDir, "recomp/${item.moduleId}")
        val request = BuildRequest(
            toolkit = toolkit.root, script = spec.script, rom = rom,
            workDir = File(scratch, "work"), outDir = File(scratch, "out"), jobs = buildJobs(), log = cache.logFile,
        )
        // Exec only, for now (the RP6). The dlopen backend for stock Android picks itself here later, e.g. by a probe
        // exec of a toolkit binary or by what the installed toolkit contains.
        val b = ExecBackend()
        backend = b
        prepare = PrepareState.Building(title, "Starting", 0f)
        val result = withContext(Dispatchers.IO) {
            runCatching {
                b.run(request) { p -> prepare = PrepareState.Building(title, p.phase, p.fraction) }
                val built = File(request.outDir, "libgame.so")
                if (!built.isFile) throw RecompBuildException("The build finished without a libgame.so.", "")
                cache.store(built, key)
                scratch.deleteRecursively() // the objects (~40 MB); the generated C is gone already (build_from_rom.sh)
            }
        }
        backend = null
        val error = result.exceptionOrNull() ?: run { prepare = null; return true }
        withContext(Dispatchers.IO) { scratch.deleteRecursively() }
        if (error is RecompCancelledException) {
            prepare = null
            return false
        }
        Log.w("plm-recomp", "building the game code for ${item.moduleId} failed", error)
        prepare = PrepareState.Failed(title, error.message ?: error.toString(),
            (error as? RecompBuildException)?.logTail.orEmpty(), cache.logFile.path)
        return false
    }

    /** Cancel on the Preparing screen: stops the build; [prepareGameLib] then returns false and clears the screen. */
    fun cancelPrepare() {
        backend?.cancel()
    }

    /** Closes the Preparing screen's error state. */
    fun dismissPrepare() {
        if (prepare is PrepareState.Failed) prepare = null
    }

    /**
     * Parallel compile jobs: one per core, but at most one per ~768 MB of RAM (a cc1 on one of DK64's big generated
     * files peaks at a few hundred MB, ON_DEVICE_RECOMP.md "Memory") and at most 8 (the RP6 has 8 cores and plenty of
     * RAM; more jobs than big+mid cores mostly adds heat).
     */
    private fun buildJobs(): Int {
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
        val mem = android.app.ActivityManager.MemoryInfo().also { am.getMemoryInfo(it) }.totalMem
        val byMem = (mem / (768L shl 20)).toInt().coerceAtLeast(1)
        return minOf(Runtime.getRuntime().availableProcessors(), byMem, 8).coerceAtLeast(1)
    }

    /**
     * A game's page (owner 2026-11-15): selecting a game in the library opens this instead of launching straight away.
     * The same tabbed page as the in-game menu: the main page leads with "Launch game" (focused, so A then A plays),
     * then mods, achievements, the port's own launcher and removing a downloaded module; then the port's own settings
     * tabs. Nothing runs here, so changes are only saved; they take effect when the game launches.
     *
     * [port] can be a downloaded module without a ROM: the page still opens (its settings can be pre-set, and Remove
     * is reachable), with "Launch game" disabled and saying why.
     */
    fun openGame(
        activity: android.app.Activity,
        port: PortEntry,
        onLaunch: (PlayableGame) -> Unit,
        onOwnLauncher: (PlayableGame) -> Unit = {},
        onRemoveModule: (String) -> Unit = {},
    ) {
        val game = port.playable
        val module = catalog.module(port.moduleId)
        val scope = MenuScope(port.moduleId, port.title)
        val settings = GameSettings.forModule(context.filesDir, port.moduleId)
        // Remembered: this page's tab and row, and that the page is open (reopened when the launcher comes back,
        // e.g. after quitting the game started from it). Closing the page with B forgets the latter.
        val position = uiState.position("page:${port.moduleId}")
        uiState.putString("open_page", port.key)
        var launched = false
        GameMenu(
            context = activity,
            title = port.title,
            initialTab = position.first,
            initialRow = position.second,
            onPosition = { tab, row -> uiState.putPosition("page:${port.moduleId}", tab, row) },
            scope = scope,
            loadOptions = {
                GameSettings.resolve(GameSettings.moduleFile(context.filesDir, port.moduleId), portSchema(port.moduleId)).second
            },
            storeSetting = { _, key, value -> settings.set(key, value) },
            profiles = portProfiles(port.moduleId),
            // Nothing runs here, so a reset only drops the stored choice; the next launch sends the default.
            resetSetting = { _, key, _ -> settings.remove(key) },
            onLaunch = game?.let { g -> { launched = true; onLaunch(g) } },
            launchBlocked = if (game == null) port.romHelp.ifEmpty { "${port.title} needs your copy of the game: add the folder with its ROM in Settings → ROM sources." } else null,
            scopeActions = {
                listOfNotNull(
                    // The recomps' escape hatch (manifest native_launcher; MENU.md scope note): this entry replaces
                    // the library's old "Play now / Open the launcher" popup. The decomp shims ignore "ui": "native",
                    // so only ports that really have a launcher get it (2026-11-13 slop audit).
                    // Not for total conversions (gameMode): the port's launcher would offer the base game again.
                    if (game != null && game.gameMode == null && module?.hasLauncher == true) MenuAction(
                        MenuTab.GAME, "Open ${game.moduleName}'s own launcher",
                        "Starts ${game.moduleName} with its own menus, for settings that aren't in this launcher yet.") {
                        onOwnLauncher(game)
                    } else null,
                    // ROADMAP item 10. Only downloaded modules: a bundled one is part of the APK and can't be
                    // deleted. Confirmed first (the menu has closed by then), and the text says saves stay, since
                    // "remove" otherwise reads like "lose my progress".
                    if (module?.source == ModuleSource.INSTALLED) MenuAction(
                        MenuTab.GAME, "Remove downloaded module",
                        "Deletes ${module.name} from this device to free space. Saves and settings stay; download it again any time.") {
                        val mb = moduleRepo.installedSize(port.moduleId) / 1e6
                        android.app.AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog)
                            .setTitle("Remove ${module.name}?")
                            .setMessage("This deletes the downloaded module (%.0f MB). Your saves, settings and mods for it stay on this device, so downloading it again picks up where you left off.".format(mb))
                            .setPositiveButton("Remove") { _, _ -> onRemoveModule(port.moduleId) }
                            .setNegativeButton("Cancel", null)
                            .showTracked()
                    } else null,
                )
            },
            // The Mods tab itself (owner 2026-11-15: no separate screen). Changes apply at the next launch.
            mods = module?.mods?.let { spec ->
                ModsPage.forSpec(activity, port.moduleId, spec, onChanged = { refreshMods() },
                    pickFile = (activity as? app.portlauncher.MainActivity)?.let { a -> { callback -> a.pickFile(callback) } })
            },
            onClose = {
                settingsMenu = null
                if (!launched) uiState.putString("open_page", null)
            },
        ).also { settingsMenu = it }.show()
    }

    /**
     * The app's own settings (the library's Settings button). Since 2026-11-15 there are no settings shared by all
     * games, so this is only what belongs to the app: ROM sources for now.
     */
    fun openAppSettings(
        activity: android.app.Activity,
        onAddFolder: () -> Unit = {},
        onRemoveFolder: (String) -> Unit = {},
    ) {
        val position = uiState.position("settings")
        GameMenu(
            context = activity,
            title = "Settings",
            initialTab = position.first,
            initialRow = position.second,
            onPosition = { tab, row -> uiState.putPosition("settings", tab, row) },
            general = listOf(
                FlagToggle("Remember position",
                    "Reopen the launcher, game pages and the in-game menu where you left them: the game you were on, " +
                        "the page you had open, the tab and the row.",
                    { AppFlags.isOn(context.filesDir, AppFlags.REMEMBER_POSITION, default = true) },
                    { AppFlags.set(context.filesDir, AppFlags.REMEMBER_POSITION, it) }),
            ),
            scope = null,
            loadOptions = { org.json.JSONArray() },
            storeSetting = { _, _, _ -> },
            sources = SourcesPage(
                folders = { ui.folders.map { it to folderLabel(it) } },
                onAdd = onAddFolder,
                onRemove = onRemoveFolder,
                onRescan = { scope.launch { rescan() } },
                rommConfigured = { RommSource.isConfigured() },
            ),
            experimental = listOf(
                FlagToggle("Save states (session only)",
                    "Save and load state in the in-game menu, and the quick keys: hold L4, Y saves, X loads. Unfinished: states are lost when you quit the game.",
                    { AppFlags.isOn(context.filesDir, AppFlags.SAVE_STATES) },
                    { AppFlags.set(context.filesDir, AppFlags.SAVE_STATES, it) }),
            ),
            onClose = { settingsMenu = null },
        ).also { settingsMenu = it }.show()
    }

    /** Why a ROM handed to us by another app (ES-DE, Argosy…) can't be started. */
    data class NotPlayable(val fileName: String, val reason: String)

    /**
     * Frontend launch (ACTION_VIEW with a content:// URI): identifies the ROM by hash and starts its game like a tap
     * in the library, Modern preset included. Returns null when the game started.
     */
    suspend fun playUri(activity: Context, uri: Uri, fileName: String): NotPlayable? {
        val scanned = withContext(Dispatchers.IO) {
            runCatching {
                context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd -> JSONObject(CoreBridge.scanFd(pfd.fd, fileName)) }
            }.getOrNull()
        } ?: return NotPlayable(fileName, "The launcher couldn't read this file.")
        val romsJson = scanned.optJSONArray("roms")
        val roms = (0 until (romsJson?.length() ?: 0)).map { LibraryStore.parseRom(romsJson!!.getJSONObject(it)) }
        if (roms.isEmpty()) {
            return NotPlayable(fileName, scanned.optString("error").ifEmpty { "There's no N64 ROM in this file." })
        }
        for (rom in roms) {
            val game = catalog.find(rom.sha1) ?: continue
            val module = game.modules.firstOrNull() ?: continue
            val file = ScannedFile(uri.toString(), fileName, 0, 0, roms, "")
            val hasMods = catalog.module(module)?.mods?.any == true
            play(activity, PlayableGame(game, module, catalog.moduleName(module), file, rom, hasMods = hasMods))
            return if (ui.status is LibraryStatus.Problem) NotPlayable(fileName, (ui.status as LibraryStatus.Problem).message) else null
        }
        // Same game (header game code), other revision or a modified ROM: say which one we need.
        val codes = roms.map { it.gameCode }.toSet()
        val sameGame = catalog.games().filter { it.gameCode.isNotEmpty() && it.gameCode in codes }
        val reason = if (sameGame.isNotEmpty()) {
            "This copy isn't the one the port needs: " + sameGame.joinToString { "${it.title} (${it.region} ${it.revision})" } +
                ". Other revisions and modified ROMs (romhacks) don't work with it."
        } else {
            "No port for this game in the launcher yet."
        }
        return NotPlayable(fileName, reason)
    }

    private fun importRom(item: PlayableGame, out: File): Boolean {
        out.parentFile?.mkdirs()
        val sha = runCatching {
            context.contentResolver.openFileDescriptor(Uri.parse(item.file.uri), "r")?.use { pfd ->
                CoreBridge.exportRomFd(pfd.fd, item.rom.zipEntry, out.absolutePath)
            }
        }.getOrNull()
        if (sha != item.rom.sha1) {
            out.delete() // changed or unreadable since the scan
            return false
        }
        return true
    }

    private fun curated(moduleId: String): Map<String, JSONObject> =
        catalog.module(moduleId)?.modernMods.orEmpty().associate { it.pkg to it.options }

    /**
     * With the Modern preset, installs the curated mods not offered yet (MODS.md), before the game starts. Each is
     * offered once: one the player removed isn't reinstalled. Without a connection the game starts without them and
     * the rest is tried next time.
     */
    private fun fetchModernMods(item: PlayableGame, store: ModStore) {
        val module = catalog.module(item.moduleId) ?: return
        // Curated Modern presets exist only for Thunderstore (recomp) modules.
        val source = module.modSources.firstOrNull() ?: return
        var state = store.presetState()
        // Pre-installed curated mods share the "applied" record (fetchPreinstall); they're not dropped here.
        val preset = module.modernMods.map { it.pkg }.toSet() + module.mods.preinstall.map { "curated-$it" }
        // A mod the preset installed and a newer preset dropped goes away (only the named package: libraries it
        // pulled in stay, they do nothing on their own). Mods the player installed are never in `applied`.
        val dropped = state.applied - preset
        if (dropped.isNotEmpty()) {
            val installedNow = store.installed().map { it.fullName }.toSet()
            dropped.filter { it in installedNow }.forEach { store.remove(it) }
            state = state.copy(applied = state.applied - dropped)
            store.savePreset(state)
        }
        if (!state.modern || module.modernMods.isEmpty()) return
        val installed = store.installed().map { it.fullName }.toSet()
        val pending = module.modernMods.map { it.pkg }.filter { it !in state.applied }
        if (pending.isEmpty()) return
        val applied = state.applied.toMutableSet()
        applied += pending.filter { it in installed }
        val missing = pending.filter { it !in installed }
        if (missing.isNotEmpty()) {
            val thunderstore = Thunderstore(source.community, context.cacheDir)
            val catalogue = runCatching { thunderstore.packages() }.getOrNull()
            if (catalogue != null) {
                missing.forEachIndexed { index, name ->
                    val pkg = catalogue.find { it.fullName == name } ?: return@forEachIndexed
                    ui = ui.copy(status = LibraryStatus.Preparing("${item.title}: getting modern mods (${index + 1} of ${missing.size})"))
                    runCatching { store.install(thunderstore, pkg, catalogue) { _, _ -> } }
                        .onSuccess { applied += name }
                        .onFailure { Log.w("plm-library", "modern mod $name: ${it.message}") }
                }
            }
        }
        store.savePreset(state.copy(applied = applied))
        ui = ui.copy(status = LibraryStatus.Preparing(item.title))
    }

    /**
     * The module's pre-installed curated mods (manifest "mods.preinstall": curated ids), installed before the game
     * starts, each once: marked applied after a successful install, so one the player removes isn't reinstalled.
     * Without a connection the game starts without it and the install is tried next time. Linked, never bundled: the
     * download comes from the author's own release (CuratedSource; owner 2026-11-16). An earlier install of the same
     * mod under another key (CuratedMod.replaces, e.g. the GameBanana-era "gamebanana-465012~…") counts as installed.
     */
    private fun fetchPreinstall(item: PlayableGame, store: ModStore) {
        val spec = catalog.module(item.moduleId)?.mods ?: return
        val wanted = spec.preinstall.mapNotNull { id -> spec.curated.firstOrNull { it.id == id } }
        if (wanted.isEmpty()) return
        val state = store.presetState()
        val installed = store.installed().map { it.fullName }.toSet()
        val applied = state.applied.toMutableSet()
        val pending = wanted.filter { it.fullName !in state.applied }
        if (pending.isEmpty()) return
        val source = app.portlauncher.mods.CuratedSource(spec.curated)
        pending.forEachIndexed { index, mod ->
            if (mod.fullName in installed || mod.replaces.any { it in installed }) { applied += mod.fullName; return@forEachIndexed }
            ui = ui.copy(status = LibraryStatus.Preparing("${item.title}: getting ${mod.name} (${index + 1} of ${pending.size})"))
            val pkg = source.packages().first { it.fullName == mod.fullName }
            runCatching { store.installArchive(source, pkg, null, mod.entries) { } }
                .onSuccess { applied += mod.fullName }
                .onFailure { Log.w("plm-library", "pre-installed mod ${mod.fullName}: ${it.message}") }
        }
        store.savePreset(state.copy(applied = applied))
        ui = ui.copy(status = LibraryStatus.Preparing(item.title))
    }

    /** Rebuilds the list after mods changed (total conversions come and go as entries). */
    fun refreshMods() {
        ui = build(data, ui.status)
    }

    private fun build(data: LibraryData, status: LibraryStatus): LibraryUi {
        val playable = mutableMapOf<String, PlayableGame>()
        // Game code -> file name of a copy we can't use (another revision or a romhack), so a module row without a
        // ROM can say "your copy is another version". Files with no supported game are otherwise ignored: the
        // library lists modules, not the ROM folder (owner 2026-11-13).
        val wrongCopies = mutableMapOf<String, String>()
        for (file in data.files) {
            for (rom in file.roms) {
                val game = catalog.find(rom.sha1)
                val module = game?.modules?.firstOrNull()
                if (game != null && module != null) {
                    val hasMods = catalog.module(module)?.mods?.any == true
                    playable.putIfAbsent(game.id, PlayableGame(game, module, catalog.moduleName(module), file, rom, hasMods = hasMods))
                } else {
                    val sameGame = catalog.games().any { it.gameCode.isNotEmpty() && it.gameCode == rom.gameCode }
                    if (sameGame) wrongCopies.putIfAbsent(rom.gameCode, displayName(file.name))
                }
            }
        }
        // Installed total conversions play on the base game's ROM, listed right after it.
        val withModes = playable.values.sortedBy { it.game.title }.flatMap { base ->
            if (!base.hasMods) return@flatMap listOf(base)
            val modes = ModStore(context, base.moduleId).installed().filter { it.enabled }.flatMap { it.gameModes }
            listOf(base) + modes.map { base.copy(gameMode = it) }
        }
        return LibraryUi(
            folders = data.folders,
            playable = withModes,
            ports = ports(withModes, wrongCopies),
            status = status,
        )
    }

    /**
     * The library rows (PortEntry): one per bundled module (owner 2026-11-13). Order (hardcoded for now; custom
     * sorting and filtering may come later, owner): **decomps before recomps** (official repository first, and the
     * decomp is a game's default port, principle 2), then rows with a ROM before rows without, then by title. A total
     * conversion (a mod that plays as its own entry, MODS.md) follows its module's row.
     * Unknown ROMs aren't listed at all (owner 2026-11-13): the library is the launcher's modules, not the ROM folder.
     * SHORTCUT (MVP): game modes are looked up with a fresh ModStore per module on every build; fine for ~10 modules.
     */
    private fun ports(withModes: List<PlayableGame>, wrongCopies: Map<String, String>): List<PortEntry> {
        val byGame = withModes.filter { it.gameMode == null }.associateBy { it.game.id }
        // Module -> the catalogue games (revisions) it runs, in catalogue order.
        val gamesByModule = linkedMapOf<String, MutableList<CatalogGame>>()
        for (game in catalog.games()) for (moduleId in game.modules) gamesByModule.getOrPut(moduleId) { mutableListOf() } += game
        val rows = mutableListOf<PortEntry>()
        for ((moduleId, games) in gamesByModule) {
            val module = catalog.module(moduleId)
            val isRecomp = module?.family == "n64recomp"
            val hasLauncher = module?.hasLauncher == true
            val name = catalog.moduleName(moduleId)
            val source = module?.source ?: ModuleSource.BUNDLED
            val size = module?.remote?.size ?: 0
            val update = source == ModuleSource.INSTALLED && module?.remote != null && module.remote.version != module.version
            // Any revision the player has will do; the first in catalogue order wins when they have several.
            val base = games.firstNotNullOfOrNull { byGame[it.id] }
            if (base == null && RommSource.has(games.flatMap { it.sha1s })) {
                // On the player's RomM server (stub: never yet). Not playable until download-on-play exists.
                rows += PortEntry(games.first(), moduleId, name, isRecomp, hasLauncher, RomState.ROMM, null, "On your RomM server", source, size, update)
                continue
            }
            if (base == null) {
                // "Needs a US v1.0 or US v1.1 ROM" when the module accepts several revisions.
                val needs = games.joinToString(" or ") { g ->
                    listOf(g.region, g.revision.let { if (it.isEmpty()) "" else "v$it" }).filter { it.isNotEmpty() }.joinToString(" ")
                }
                val other = games.firstNotNullOfOrNull { wrongCopies[it.gameCode] }
                rows += PortEntry(games.first(), moduleId, name, isRecomp, hasLauncher, RomState.NONE, null,
                    if (other != null) "Needs $needs · your copy ($other) is another version" else "Needs a $needs ROM",
                    source, size, update).copy(romHelp = romHelp(games, other))
                continue
            }
            // The scanned ROM is the same file for every module of its game, so re-point the PlayableGame at this
            // module; hasMods is per module (only some have a mod source).
            val hasMods = module?.mods?.any == true
            val forModule = base.copy(moduleId = moduleId, moduleName = name, hasMods = hasMods)
            rows += PortEntry(base.game, moduleId, name, isRecomp, hasLauncher, RomState.LOCAL, forModule, "", source, size, update)
            if (hasMods) {
                ModStore(context, moduleId).installed().filter { it.enabled }.flatMap { it.gameModes }.forEach { mode ->
                    rows += PortEntry(base.game, moduleId, name, isRecomp, hasLauncher, RomState.LOCAL, forModule.copy(gameMode = mode), "", source, size, update)
                }
            }
        }
        // Maturity and versions for every row in one place, rather than threading three more positional arguments
        // through each PortEntry(...) call above.
        val withInfo = rows.map { row ->
            val m = catalog.module(row.moduleId)
            row.copy(maturity = m?.maturity ?: Maturity.EXPERIMENTAL, version = m?.version.orEmpty(),
                remoteVersion = m?.remote?.version.orEmpty())
        }
        // Stable sort: a total conversion keeps its place right after its module's row.
        return withInfo.sortedWith(compareBy<PortEntry> { it.isRecomp }.thenBy { it.rom.ordinal }.thenBy { it.game.title })
    }

    /** The full "which ROM to provide" text for a module's games (PortEntry.romHelp). */
    private fun romHelp(games: List<CatalogGame>, other: String?): String = buildString {
        val title = games.first().title
        append("$title needs your own copy of the game: a ROM file made from your cartridge.\n\n")
        append("Accepted versions:\n")
        games.forEach { g ->
            val version = listOf(g.region, g.revision.let { if (it.isEmpty()) "" else "v$it" }).filter { it.isNotEmpty() }.joinToString(" ")
            append("• ${g.title}${if (version.isNotEmpty()) ", $version" else ""}")
            if (g.sha1s.isNotEmpty()) append("\n   SHA-1 ${g.sha1s.joinToString(" or ")}")
            append("\n")
        }
        append("\nFile types: .z64, .v64 or .n64, or a .zip with one inside. The file name doesn't matter.\n")
        if (other != null) {
            append("\nYour copy ($other) is this game, but a version this port doesn't run (another region or revision, or a modified ROM).\n")
        }
        append("\nPut it in a folder listed in Settings → ROM sources (subfolders count), then use Rescan the library there.")
    }

    /**
     * A readable name for a folder's tree URI: "Internal storage / Roms/N64", or "SD card (1A2B-3C4D) / Roms" for
     * other volumes. Falls back to the raw URI for providers that aren't the external-storage one.
     */
    private fun folderLabel(treeUri: String): String = runCatching {
        val id = android.provider.DocumentsContract.getTreeDocumentId(Uri.parse(treeUri)) // "primary:Roms/N64"
        val volume = id.substringBefore(':')
        val path = id.substringAfter(':', "")
        val where = if (volume == "primary") "Internal storage" else "SD card ($volume)"
        if (path.isEmpty()) where else "$where / $path"
    }.getOrDefault(treeUri)

    private fun displayName(fileName: String): String = fileName.substringBeforeLast('.')
}
