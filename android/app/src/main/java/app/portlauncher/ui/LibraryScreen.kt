package app.portlauncher.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.portlauncher.core.CoreBridge
import app.portlauncher.library.LibraryController
import app.portlauncher.library.LibraryStatus
import app.portlauncher.library.Maturity
import app.portlauncher.library.PlayableGame
import app.portlauncher.library.PortEntry
import app.portlauncher.library.RomState
import kotlinx.coroutines.launch

/** The library: first-run folder pick, scan progress, playable games, then games we can't run yet. */
@Composable
fun LibraryScreen(controller: LibraryController, resumes: Int = 0) {
    val ui = controller.ui
    val scope = rememberCoroutineScope()
    val activity = LocalContext.current
    val pickFolder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        if (uri != null) scope.launch { controller.addFolder(uri) }
    }
    val busy = ui.status is LibraryStatus.Scanning || ui.status is LibraryStatus.Preparing
    var showDownloads by remember { mutableStateOf(false) }
    // Opened from the home-screen icon (LibraryController.closeMenus): the downloads list closes too.
    LaunchedEffect(controller.homeRequests) { showDownloads = false }
    // Module server: fetch the index once per screen start (cached copy keeps known modules listed offline).
    LaunchedEffect(Unit) { controller.refreshModules() }

    // Selecting a game opens its page (owner 2026-11-15); "Launch game" there starts it.
    // playAsync, not scope.launch: a first-launch build replaces this screen, which would cancel this scope.
    fun openPage(port: PortEntry) {
        (activity as? android.app.Activity)?.let {
            controller.openGame(it, port,
                onLaunch = { game -> controller.playAsync(activity, game, true) },
                onOwnLauncher = { game -> controller.playAsync(activity, game, false) },
                onRemoveModule = { id -> scope.launch { controller.removeModule(id) } })
        }
    }

    // Remember position (Settings → General): a game page left open (closed by launching its game, or the app
    // closing) opens again when the library comes back, e.g. after quitting that game.
    LaunchedEffect(resumes, ui.ports.size) {
        if (controller.menuOpen || ui.ports.isEmpty()) return@LaunchedEffect
        val key = controller.uiState.getString("open_page") ?: return@LaunchedEffect
        ui.ports.firstOrNull { it.key == key && !it.needsDownload }?.let { openPage(it) }
    }

    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Box(modifier = Modifier.fillMaxSize().safeDrawingPadding().padding(horizontal = 24.dp, vertical = 16.dp)) {
            if (ui.folders.isEmpty()) {
                FirstRun(onAddFolder = { pickFolder.launch(null) })
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Header(
                        busy = busy,
                        updates = ui.ports.filter { it.updateAvailable }.distinctBy { it.moduleId }.size,
                        onUpdateAll = { scope.launch { controller.updateAll() } },
                        downloads = app.portlauncher.downloads.Downloads.tasks.size,
                        onDownloads = { showDownloads = true },
                        onSettings = {
                            (activity as? android.app.Activity)?.let {
                                controller.openAppSettings(it,
                                    onAddFolder = { pickFolder.launch(null) },
                                    onRemoveFolder = { uri -> scope.launch { controller.removeFolder(uri) } })
                            }
                        },
                    )
                    StatusLine(ui.status)
                    GameList(
                        resumes = resumes,
                        homeRequests = controller.homeRequests,
                        ports = ui.ports,
                        enabled = !busy,
                        onOpen = ::openPage,
                        focusKey = controller.uiState.getString("library_row"),
                        onFocusRow = { key -> controller.uiState.putString("library_row", key) },
                        onDownload = { port -> scope.launch { controller.download(port) } },
                    )
                }
            }
            DownloadBar(modifier = Modifier.align(Alignment.BottomCenter).padding(bottom = 28.dp))
            if (showDownloads) DownloadsDialog(onDismiss = { showDownloads = false })
            Text(
                "core ${remember { CoreBridge.coreVersion() }}",
                modifier = Modifier.align(Alignment.BottomEnd),
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun FirstRun(onAddFolder: () -> Unit) {
    val focus = remember { FocusRequester() }
    Column(
        modifier = Modifier.fillMaxSize(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
    ) {
        Text("Library", style = MaterialTheme.typography.headlineMedium)
        Text(
            "Point the launcher at the folder with your N64 games (.z64, .v64, .n64 or .zip).",
            style = MaterialTheme.typography.bodyLarge,
            textAlign = TextAlign.Center,
        )
        FocusButton(text = "Add your N64 ROM folder", modifier = Modifier.focusRequester(focus), onClick = onAddFolder)
    }
    LaunchedEffect(Unit) { focus.requestFocus() }
}

@Composable
private fun Header(busy: Boolean, updates: Int, onUpdateAll: () -> Unit, onSettings: () -> Unit,
                   downloads: Int = 0, onDownloads: () -> Unit = {}) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Text("Library", style = MaterialTheme.typography.headlineMedium, modifier = Modifier.weight(1f))
        // Only while something has an update (owner 2026-11-15): one press updates every downloaded module.
        if (updates > 0) FocusButton(text = "Update all ($updates)", enabled = !busy, onClick = onUpdateAll)
        // The download queue's list (Downloads), once anything has been queued this session.
        if (downloads > 0) FocusButton(text = "Downloads", outlined = true, onClick = onDownloads)
        FocusButton(text = "Settings", enabled = !busy, outlined = true, onClick = onSettings)
        // No "Add folder" or "Rescan" here any more (owner 2026-11-13 / 2026-11-16): both live in Settings → ROM
        // sources. First run still asks for a folder (FirstRun), since an empty library has nothing else to offer.
    }
}

@Composable
private fun StatusLine(status: LibraryStatus) {
    when (status) {
        is LibraryStatus.Scanning -> Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(
                if (status.total == 0) "Looking for games…" else "Scanning ${status.done} of ${status.total}",
                style = MaterialTheme.typography.bodyMedium,
            )
            if (status.total > 0) {
                LinearProgressIndicator(progress = { status.done / status.total.toFloat() }, modifier = Modifier.fillMaxWidth())
            } else {
                LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            }
        }
        is LibraryStatus.Preparing -> Text("Starting ${status.title}…", style = MaterialTheme.typography.bodyMedium)
        is LibraryStatus.Problem -> Text(status.message, color = MaterialTheme.colorScheme.error)
        LibraryStatus.Idle -> Unit
    }
}

/**
 * The library list (owner request 2026-11-13): one row per game × port (PortEntry), with a ROM badge (three states,
 * RomState) and a Decomp/Recomp badge. Replaces the old three sections ("Ready to play" / "Also playable with the
 * right ROM" / per-game Options button). The Options button is gone: its mods and own-launcher actions moved to the
 * game page (GameMenu scope actions). Rows show just the game: the port's own icon on the left says which port
 * (owner 2026-11-13), the badges say decomp/recomp and where the ROM is. Selecting a game opens its page (owner
 * 2026-11-15: settings first, "Launch game" the focused default), which also holds the recomps' own-launcher entry
 * that the old "Play now / Open the launcher" popup offered.
 * Only the launcher's modules are listed: unknown ROM files aren't (owner 2026-11-13).
 */
@Composable
private fun GameList(
    resumes: Int,
    /** LibraryController.homeRequests: bumped when the app is opened from the home-screen icon. */
    homeRequests: Int = 0,
    ports: List<PortEntry>,
    enabled: Boolean,
    onOpen: (PortEntry) -> Unit,
    onDownload: (PortEntry) -> Unit,
    /** The row to focus (Remember position: the game the player was on), else the first. */
    focusKey: String? = null,
    onFocusRow: (String) -> Unit = {},
) {
    val first = remember { FocusRequester() }
    val listState = androidx.compose.foundation.lazy.rememberLazyListState()
    val target = ports.firstOrNull { it.key == focusKey } ?: ports.firstOrNull()
    var downloadFor by remember { mutableStateOf<PortEntry?>(null) }
    var updateFor by remember { mutableStateOf<PortEntry?>(null) }
    // Opened from the home-screen icon (LibraryController.closeMenus): the download/update prompts close too.
    LaunchedEffect(homeRequests) { downloadFor = null; updateFor = null }
    val readyCount = ports.count { it.playable != null }
    LazyColumn(state = listState, verticalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxSize()) {
        item { SectionTitle(if (readyCount == 0) "No playable games found yet" else "Games ($readyCount ready)") }
        items(ports, key = { it.key }) { port ->
            val modifier = (if (port == target) Modifier.focusRequester(first) else Modifier)
                .onFocusChanged { if (it.hasFocus) onFocusRow(port.key) }
            PortRow(port, enabled, modifier) {
                // Not downloaded yet (module server): offer the download first, ROM or not, so the module is ready.
                if (port.needsDownload) {
                    downloadFor = port
                    return@PortRow
                }
                // A newer version on the module server (ROADMAP item 10): ask instead of silently launching the old
                // one. Asked on every select until updated: there's no "skip this version" yet (MVP); "Play this
                // version" is one press, so it doesn't get in the way. Rows without a ROM get the prompt too (with
                // only Update / Not now), since that's the one thing selecting them can do.
                if (port.updateAvailable) {
                    updateFor = port
                    return@PortRow
                }
                // The game's page, ROM or not: without one it says so on "Launch game", and settings and Remove
                // still work there.
                onOpen(port)
            }
        }
    }
    downloadFor?.let { port ->
        DownloadDialog(
            port = port,
            onDownload = { downloadFor = null; onDownload(port) },
            onDismiss = { downloadFor = null },
        )
    }
    updateFor?.let { port ->
        UpdateDialog(
            port = port,
            onUpdate = { updateFor = null; onDownload(port) },
            onPlay = { updateFor = null; onOpen(port) },
            onDismiss = { updateFor = null },
        )
    }
    // Refocus the first row on resume and whenever a scan changes the list (e.g. after adding a folder).
    // Keyed on the download state too: a finished download rebuilds the rows with the same keys, which would
    // otherwise leave nothing focused for the controller.
    // updateAvailable too: an update keeps the source INSTALLED, and the rebuilt rows lost focus (emulator 2026-11-13).
    LaunchedEffect(ports.map { it.key + it.source + it.updateAvailable }, resumes) {
        if (ports.isEmpty()) return@LaunchedEffect
        // Bring the remembered row into view first (a LazyColumn only composes visible rows), then focus it.
        val index = ports.indexOf(target)
        if (index > 0) runCatching { listState.scrollToItem(index + 1) }   // +1: the section title item
        runCatching { first.requestFocus() }
    }
}

/** Download prompt for a module the module server offers (size from the index). */
@Composable
private fun DownloadDialog(port: PortEntry, onDownload: () -> Unit, onDismiss: () -> Unit) {
    val ok = remember { FocusRequester() }
    val mb = if (port.downloadSize > 0) " (%.1f MB)".format(port.downloadSize / 1e6) else ""
    androidx.compose.material3.AlertDialog(
        onDismissRequest = onDismiss,
        icon = { PortIcon(port.moduleId, 48) },
        title = { Text(port.title) },
        text = {
            Text("${port.moduleName} isn't on this device yet. Download it$mb?" +
                if (port.playable == null) " To play it you'll also need your own ROM. ${port.detail}; the game's page lists the exact files." else "")
        },
        confirmButton = { FocusButton(text = "Download", modifier = Modifier.focusRequester(ok), onClick = onDownload) },
        dismissButton = { FocusButton(text = "Not now", outlined = true, onClick = onDismiss) },
    )
    LaunchedEffect(Unit) { runCatching { ok.requestFocus() } }
}

/**
 * Update prompt (ROADMAP item 10): a downloaded module has a newer version on the module server. Update is focused
 * (the reason we ask at all); "Play this version" keeps the installed one for now. Update downloads and replaces the
 * module (ModuleRepository.install swaps atomically; saves in files/modules/<id>/ aren't touched); the player then
 * selects the game again. SHORTCUT (MVP): no changelog, no "skip this version".
 */
@Composable
private fun UpdateDialog(port: PortEntry, onUpdate: () -> Unit, onPlay: () -> Unit, onDismiss: () -> Unit) {
    val ok = remember { FocusRequester() }
    val mb = if (port.downloadSize > 0) " (%.0f MB)".format(port.downloadSize / 1e6) else ""
    // Kept short: in landscape the dialog stacks its two buttons, and a longer text got clipped (emulator 2026-11-13).
    val versions = if (port.version.isNotEmpty() && port.remoteVersion.isNotEmpty())
        " (${port.version} → ${port.remoteVersion})" else ""
    androidx.compose.material3.AlertDialog(
        onDismissRequest = onDismiss,
        icon = { PortIcon(port.moduleId, 48) },
        title = { Text(port.title) },
        text = { Text("Update available$versions. Saves and settings stay.") },
        confirmButton = { FocusButton(text = "Update$mb", modifier = Modifier.focusRequester(ok), onClick = onUpdate) },
        dismissButton = {
            if (port.playable != null) FocusButton(text = "Use this version", outlined = true, onClick = onPlay)
            else FocusButton(text = "Not now", outlined = true, onClick = onDismiss)
        },
    )
    LaunchedEffect(Unit) { runCatching { ok.requestFocus() } }
}

/**
 * The port's own icon (assets/modules/<id>/icon.png, staged at build time from its upstream source by
 * tools/stage_icons.py). Empty space of the same size when a module has none, so titles stay aligned.
 * SHORTCUT (MVP): decoded on the UI thread, cached per composition with remember; ~10 small PNGs, fine for now.
 */
@Composable
private fun PortIcon(moduleId: String, sizeDp: Int) {
    val context = LocalContext.current
    val bitmap = remember(moduleId) {
        runCatching {
            context.assets.open("modules/$moduleId/icon.png").use { android.graphics.BitmapFactory.decodeStream(it) }
        }.getOrNull()
            // Downloaded or not-yet-downloaded modules (module server): the package's icon, or the index's.
            ?: app.portlauncher.modules.ModuleRepository(context).iconFile(moduleId)
                ?.let { android.graphics.BitmapFactory.decodeFile(it.absolutePath) }
    }?.asImageBitmap()
    if (bitmap != null) {
        androidx.compose.foundation.Image(bitmap, contentDescription = null, modifier = Modifier.size(sizeDp.dp))
    } else {
        Box(modifier = Modifier.size(sizeDp.dp))
    }
}

/**
 * One library row. Rows without a ROM are still focusable (a controller must be able to scroll past them) but A does
 * nothing and they're drawn dimmer. SHORTCUT (MVP): badge icons are Unicode glyphs, not vector icons: the app has no
 * material-icons dependency and adding the extended set (~MBs) just for three glyphs wasn't worth it yet. Swap for
 * drawn icons in the polish pass.
 */
@Composable
private fun PortRow(port: PortEntry, enabled: Boolean, modifier: Modifier, onClick: () -> Unit) {
    var focused by remember { mutableStateOf(false) }
    val playable = port.playable != null
    Card(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier
            .widthIn(max = 720.dp)
            .fillMaxWidth()
            .focusProperties { canFocus = true }
            .onFocusChanged { focused = it.isFocused },
        colors = if (playable) CardDefaults.cardColors()
        else CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
        border = if (focused) BorderStroke(3.dp, MaterialTheme.colorScheme.primary) else null,
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            PortIcon(port.moduleId, 44)
            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    port.title,
                    style = MaterialTheme.typography.titleLarge,
                    color = if (playable) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant,
                )
                // Just the game (owner 2026-11-13): no port name or region on playable rows, the icon says the port.
                // A second line only where it tells the player something: which mod this is, or which ROM is missing.
                val sub = when {
                    port.playable?.gameMode != null -> "Mod for ${port.game.title}"
                    !playable -> port.detail
                    else -> ""
                }
                // The maturity chip sits on this second line, under the title, not with the badges on the right:
                // tested in the emulator in portrait (2026-11-13), a fourth badge on the right squeezed the title to
                // one letter per line. Here it only costs a line on rows that had none.
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    MaturityBadge(port.maturity)
                    if (sub.isNotEmpty()) {
                        Text(sub, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
            Badge(
                text = if (port.isRecomp) "Recomp" else "Decomp",
                // Decomp = official repository (primary colour), recomp = secondary repository (tertiary colour);
                // ROADMAP "Two tiers, two repositories".
                container = if (port.isRecomp) MaterialTheme.colorScheme.tertiaryContainer else MaterialTheme.colorScheme.primaryContainer,
                content = if (port.isRecomp) MaterialTheme.colorScheme.onTertiaryContainer else MaterialTheme.colorScheme.onPrimaryContainer,
            )
            if (port.needsDownload || port.updateAvailable) {
                // Module server state: not on the device yet (with its size), or a newer version is offered.
                Badge(
                    if (port.updateAvailable) "⟳ Update" else if (port.downloadSize > 0) "⤓ %.0f MB".format(port.downloadSize / 1e6) else "⤓ Download",
                    MaterialTheme.colorScheme.surfaceVariant, MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            when (port.rom) {
                RomState.LOCAL -> Badge("● ROM", MaterialTheme.colorScheme.secondaryContainer, MaterialTheme.colorScheme.onSecondaryContainer)
                // Nothing sets ROMM yet (no RomM integration); built so the third state is ready (owner 2026-11-13).
                RomState.ROMM -> Badge("☁ RomM", MaterialTheme.colorScheme.surfaceVariant, MaterialTheme.colorScheme.onSurfaceVariant)
                RomState.NONE -> Badge("○ No ROM", MaterialTheme.colorScheme.surface, MaterialTheme.colorScheme.outline, outlined = true)
            }
        }
    }
}

/**
 * Maturity badge (ROADMAP item 9, MODULE_SPEC §11). Smaller type than the other badges, and under the title rather
 * than with them (PortRow): a row already carries Decomp/Recomp, maybe a download badge, and the ROM badge on the
 * right, and the title must keep room on narrow screens.
 * Colours: Experimental in the error palette so untested modules stand out (the point of the badge); Integrated
 * neutral; Complete in the primary colour (the goal state).
 */
@Composable
private fun MaturityBadge(maturity: Maturity) {
    val (container, content) = when (maturity) {
        Maturity.EXPERIMENTAL -> MaterialTheme.colorScheme.errorContainer to MaterialTheme.colorScheme.onErrorContainer
        Maturity.INTEGRATED -> MaterialTheme.colorScheme.surfaceVariant to MaterialTheme.colorScheme.onSurfaceVariant
        Maturity.COMPLETE -> MaterialTheme.colorScheme.primary to MaterialTheme.colorScheme.onPrimary
    }
    Surface(color = container, contentColor = content, shape = MaterialTheme.shapes.small) {
        Text(maturity.label, style = MaterialTheme.typography.labelSmall, maxLines = 1, softWrap = false, modifier = Modifier.padding(horizontal = 6.dp, vertical = 3.dp))
    }
}

@Composable
private fun Badge(text: String, container: androidx.compose.ui.graphics.Color, content: androidx.compose.ui.graphics.Color, outlined: Boolean = false) {
    Surface(
        color = container,
        contentColor = content,
        shape = MaterialTheme.shapes.small,
        border = if (outlined) BorderStroke(1.dp, MaterialTheme.colorScheme.outline) else null,
    ) {
        Text(text, style = MaterialTheme.typography.labelLarge, modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp))
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = 8.dp, bottom = 4.dp))
}

/**
 * Button with a clearly visible focus ring for D-pad/gamepad navigation. Focusable even in touch
 * mode, so the first controller press activates it instead of only leaving touch mode.
 */
@Composable
fun FocusButton(
    text: String,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    outlined: Boolean = false,
    onClick: () -> Unit,
) {
    var focused by remember { mutableStateOf(false) }
    val focusModifier = modifier
        .focusProperties { canFocus = true }
        .onFocusChanged { focused = it.isFocused }
    val border = if (focused) BorderStroke(3.dp, MaterialTheme.colorScheme.primary) else null
    if (outlined) {
        OutlinedButton(onClick = onClick, enabled = enabled, modifier = focusModifier,
            border = border ?: BorderStroke(1.dp, MaterialTheme.colorScheme.outline)) { Text(text) }
    } else {
        Button(onClick = onClick, enabled = enabled, modifier = focusModifier, border = border) { Text(text) }
    }
}


/**
 * The download queue's progress at the bottom of the library (owner 2026-11-15): the running download, its bar and how
 * many are queued behind it. Hidden when nothing is queued or running.
 */
@Composable
private fun DownloadBar(modifier: Modifier) {
    val downloads = app.portlauncher.downloads.Downloads
    val active = downloads.active ?: return
    val queued = downloads.queued
    Card(modifier = modifier.widthIn(max = 900.dp).fillMaxWidth()) {
        Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            val percent = active.progress?.let { " ${(it * 100).toInt()}%" }.orEmpty()
            Text("Downloading ${active.title}$percent" + if (queued > 0) " · $queued more queued" else "",
                style = MaterialTheme.typography.bodyMedium)
            val p = active.progress
            if (p != null) LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth())
            else LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        }
    }
}

/** The download queue as a plain list: what's downloading, queued, done or failed this session. */
@Composable
private fun DownloadsDialog(onDismiss: () -> Unit) {
    val downloads = app.portlauncher.downloads.Downloads
    val ok = remember { FocusRequester() }
    androidx.compose.material3.AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Downloads") },
        text = {
            LazyColumn(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                items(downloads.tasks.reversed(), key = { it.id }) { t ->
                    val state = when (t.state) {
                        app.portlauncher.downloads.Downloads.State.QUEUED -> "Queued"
                        app.portlauncher.downloads.Downloads.State.RUNNING -> "Downloading" + (t.progress?.let { " ${(it * 100).toInt()}%" } ?: "")
                        app.portlauncher.downloads.Downloads.State.DONE -> "Done"
                        app.portlauncher.downloads.Downloads.State.FAILED -> "Failed" + (t.error?.let { ": $it" } ?: "")
                    }
                    Text("${t.title} · $state", style = MaterialTheme.typography.bodyMedium)
                }
            }
        },
        confirmButton = { FocusButton(text = "Close", modifier = Modifier.focusRequester(ok), onClick = onDismiss) },
        dismissButton = { FocusButton(text = "Clear finished", outlined = true) { downloads.clearFinished() } },
    )
    LaunchedEffect(Unit) { runCatching { ok.requestFocus() } }
}
