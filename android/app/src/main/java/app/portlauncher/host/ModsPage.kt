package app.portlauncher.host

import android.content.Context
import android.os.Handler
import android.os.Looper
import app.portlauncher.mods.BrowseQuery
import app.portlauncher.mods.BuiltinMod
import app.portlauncher.mods.BuiltinModState
import app.portlauncher.mods.CuratedSource
import app.portlauncher.mods.InstalledPackage
import app.portlauncher.mods.ModDetails
import app.portlauncher.mods.ModStore
import app.portlauncher.mods.ModsSpec
import app.portlauncher.mods.RemoteModSource
import app.portlauncher.mods.RemotePackage
import app.portlauncher.mods.Thunderstore

/**
 * The Mods tab's model (owner 2026-11-15: mods live on the tab itself, not a separate screen, can be configured, and
 * are reachable in game too). Shared by the library's game page and the in-game menu; GameMenu draws it. Installed
 * mods: on/off, load order, remove. "Get more mods" switches the tab to the source's browse list. Network and disk
 * work runs on a background thread; [onUpdate] is called on the main thread when the tab should be redrawn.
 *
 * Mods load when the game starts, so a change made in game marks [pendingRestart]; the tab then offers [onRestart]
 * (the menu's Reset: a relaunch in a fresh process). Built-in mods ([builtin]) are the exception: their switch is a
 * port config key, applied live through [applySetting].
 *
 * Since 2026-11-16 (MODS.md "Curated mods"): the downloadable list is our curated one (CuratedSource) for the
 * libultraship ports, or Thunderstore for the recomps; anything else comes from a file ([addFromFile]) after
 * "Where to get more mods" ([spec] sources) sent the player to a site.
 */
class ModsPage(
    private val context: Context,
    moduleId: String,
    val spec: ModsSpec,
    /** In game: changes need a restart, offered through this. Null on the game page (the next launch applies them). */
    val onRestart: (() -> Unit)? = null,
    /** The launcher refreshes its library (total conversions are library rows). */
    private val onChanged: () -> Unit = {},
    /** In game: applies a port setting live ("port.<key>", value), for built-in mods. Null on the game page. */
    private val applySetting: ((String, String) -> Unit)? = null,
    /** Opens the system file picker; the callback gets the chosen file (null = cancelled). Null hides "from a file". */
    private val pickFile: ((((android.net.Uri?) -> Unit)) -> Unit)? = null,
) {
    /** The browsable list: Thunderstore for the recomps, else our curated list. */
    val source: RemoteModSource =
        if (spec.thunderstore.isNotEmpty()) Thunderstore(spec.thunderstore, context.cacheDir) else CuratedSource(spec.curated)
    private val store = ModStore(context, moduleId, spec.files)
    private val builtinState = BuiltinModState(context.filesDir, moduleId)

    val builtin: List<BuiltinMod> get() = spec.builtin
    /** Whether the browse list has anything (Thunderstore, or a non-empty curated list). */
    val hasBrowse: Boolean get() = spec.thunderstore.isNotEmpty() || spec.curated.isNotEmpty()
    val canAddFiles: Boolean get() = spec.files && pickFile != null

    fun builtinOn(m: BuiltinMod) = builtinState.isOn(m)

    /** Switches a built-in mod: saved for the game, and applied live in game (no restart). */
    fun toggleBuiltin(m: BuiltinMod) {
        val on = !builtinState.isOn(m)
        builtinState.set(m, on)
        if (m.setting.isNotEmpty()) applySetting?.invoke("port.${m.setting}", if (on) "1" else "0")
        status = null
        onUpdate()
    }

    /** "Add a mod from a file": the system picker, then installed like a download (an archive the port loads). */
    fun addFromFile() {
        val pick = pickFile ?: return
        pick { uri ->
            if (uri == null) return@pick
            val name = runCatching {
                context.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)
                    ?.use { c -> if (c.moveToFirst()) c.getString(0) else null }
            }.getOrNull() ?: uri.lastPathSegment?.substringAfterLast('/') ?: "mod.zip"
            work("Installing $name…") {
                val copy = java.io.File(context.cacheDir, "mod-picked/$name").apply { parentFile?.mkdirs() }
                context.contentResolver.openInputStream(uri)?.use { input -> copy.outputStream().use { input.copyTo(it) } }
                    ?: throw app.portlauncher.mods.ModInstallException("Couldn't read $name")
                try { store.installLocalFile(copy) } finally { copy.delete() }
                changed()
            }
        }
    }
    private val main = Handler(Looper.getMainLooper())

    /** Set by GameMenu: redraws the tab (keeping the focused row where it can). */
    var onUpdate: () -> Unit = {}

    var browsing = false
    var installed: List<InstalledPackage> = store.installed()
        private set
    /** The browse list so far (pages appended by [loadMore]); null until the first page arrives. */
    var catalogue: List<RemotePackage>? = null
        private set
    var hasMore = false
        private set
    /** Search text (empty = browse), sort id, category id (empty = all). Changing any reloads page 1. */
    var query = BrowseQuery()
        private set
    var categories: List<Pair<String, String>> = emptyList()
        private set
    /** The mod whose detail view is open (null = the list), and its details once loaded. */
    var detailOf: RemotePackage? = null
        private set
    var details: ModDetails? = null
        private set
    /** Thunderstore needs its whole catalogue for dependencies; loaded once on the first install. */
    private var fullCatalogue: List<RemotePackage>? = null
    /** A line for the detail panel: progress or the last problem; null when idle. */
    var status: String? = null
        private set
    var busy = false
        private set
    var pendingRestart = false
        private set

    private fun work(label: String, job: () -> Unit) {
        if (busy) return
        busy = true
        status = label
        onUpdate()
        Thread {
            val error = runCatching(job).exceptionOrNull()
            val now = store.installed()
            main.post {
                installed = now
                busy = false
                status = error?.let { it.message ?: "Something went wrong" }
                onChanged()
                onUpdate()
            }
        }.start()
    }

    private fun changed() {
        if (onRestart != null) pendingRestart = true
    }

    fun toggle(p: InstalledPackage) = work(if (p.enabled) "Switching ${p.displayName} off…" else "Switching ${p.displayName} on…") {
        store.setEnabled(p.fullName, !p.enabled); changed()
    }

    fun move(p: InstalledPackage, dir: Int) = work("Reordering…") { store.move(p.fullName, dir); changed() }

    fun remove(p: InstalledPackage) = work("Removing ${p.displayName}…") { store.remove(p.fullName); changed() }

    /** Loads page 1 of the browse list for the current [query] (and the category list, once). */
    fun loadCatalogue(refresh: Boolean = false) = fetch(page = 1)

    /** Appends the next page ("Load more"). */
    fun loadMore() { if (hasMore) fetch(page = query.page + 1) }

    fun search(text: String) { query = query.copy(text = text.trim(), page = 1); fetch(1) }

    fun cycleSort(dir: Int) {
        val sorts = source.sorts
        if (sorts.isEmpty()) return
        val i = sorts.indexOfFirst { it.first == query.sort }.coerceAtLeast(0)
        query = query.copy(sort = sorts[((i + dir) % sorts.size + sorts.size) % sorts.size].first, page = 1)
        fetch(1)
    }

    fun cycleCategory(dir: Int) {
        val ids = listOf("") + categories.map { it.first }
        val i = ids.indexOf(query.category).coerceAtLeast(0)
        query = query.copy(category = ids[((i + dir) % ids.size + ids.size) % ids.size], page = 1)
        fetch(1)
    }

    fun sortLabel() = source.sorts.firstOrNull { it.first == query.sort }?.second ?: source.sorts.firstOrNull()?.second.orEmpty()
    fun categoryLabel() = categories.firstOrNull { it.first == query.category }?.second ?: "All"

    private fun fetch(page: Int) {
        if (busy) return
        busy = true
        status = if (page == 1) "Loading mods from ${source.label}…" else "Loading more…"
        onUpdate()
        val q = query.copy(page = page)
        Thread {
            val result = runCatching { source.browse(q) }
            val cats = if (categories.isEmpty()) runCatching { source.categories() }.getOrDefault(emptyList()) else categories
            main.post {
                busy = false
                categories = cats
                result.onSuccess { pageResult ->
                    query = q
                    catalogue = if (page == 1) pageResult.items else catalogue.orEmpty() + pageResult.items
                    hasMore = pageResult.hasMore
                    status = null
                }.onFailure {
                    status = "Couldn't reach ${source.label}. Check the connection and try again."
                }
                onUpdate()
            }
        }.start()
    }

    /** Opens [pkg]'s detail view and loads its details (description, files with sizes). */
    fun openDetails(pkg: RemotePackage) {
        detailOf = pkg
        details = null
        busy = true
        status = "Loading ${pkg.displayName}…"
        onUpdate()
        Thread {
            val d = runCatching { source.details(pkg) }.getOrNull()
            main.post {
                if (detailOf == pkg) details = d
                busy = false
                status = if (d == null) "Couldn't load the details. You can still install it." else null
                onUpdate()
            }
        }.start()
    }

    fun closeDetails() { detailOf = null; details = null; status = null; onUpdate() }

    /**
     * The installed item for one of [pkg]'s files. A mod with several files (a full pack and a lighter one, separate
     * models...) installs each file as its own item (owner 2026-11-15: installing one asset marked them all installed),
     * keyed "<mod>~<file>"; a mod with a single file keeps the plain mod key.
     */
    fun installedFile(pkg: RemotePackage, file: String, multi: Boolean): InstalledPackage? =
        installed.firstOrNull { it.fullName == if (multi) "${pkg.fullName}~$file" else pkg.fullName }

    /** Every installed item of [pkg] (any of its files), for the list's Installed / Update badge. */
    fun installedOf(pkg: RemotePackage): List<InstalledPackage> =
        installed.filter { it.fullName == pkg.fullName || it.fullName.startsWith(pkg.fullName + "~") }

    /** Installs [pkg], [file] being one of its details' files (null = the first supported one). */
    fun install(pkg: RemotePackage, file: String? = null, multi: Boolean = false) {
        val item = if (multi && file != null)
            pkg.copy(fullName = "${pkg.fullName}~$file", name = "${pkg.name} · ${file.substringBeforeLast('.')}")
        else pkg
        // The job itself, reporting progress 0..1 (Thunderstore reports per package; the bar shows the latest).
        val job = { report: (Float) -> Unit ->
            if (source is Thunderstore) {
                val all = fullCatalogue ?: source.packages().also { fullCatalogue = it }
                store.install(source, pkg, all) { _, p -> report(p) }
            } else {
                // A curated entry may keep only some of a zip's archives (one variant of several).
                val only = (source as? CuratedSource)?.entry(pkg)?.entries.orEmpty()
                store.installArchive(source, item, file, only) { p -> report(p) }
            }
            changed()
        }
        if (onRestart == null) {
            // The game page (launcher process): into the launcher's one download queue (owner 2026-11-15), shown at
            // the bottom of the library and in its Downloads list.
            status = "Added to Downloads: ${item.displayName}"
            onUpdate()
            app.portlauncher.downloads.Downloads.enqueue(item.displayName, { report -> job(report) }) { error ->
                installed = store.installed()
                status = error?.let { "Couldn't install ${item.displayName}: ${it.message}" }
                onChanged()
                onUpdate()
            }
        } else {
            // In game (the game's own process): progress in the menu's panel.
            work("Downloading ${item.displayName}…") { job { p -> progress("Downloading ${item.displayName}…", p) } }
        }
    }

    private var lastPercent = -1
    private fun progress(label: String, fraction: Float) {
        val percent = (fraction * 100).toInt()
        if (percent == lastPercent) return
        lastPercent = percent
        main.post { status = "$label $percent%"; onUpdate() }
    }

    companion object {
        /** The page for a module's mods (ModsSpec from its manifest), or null when it has none ("coming soon"). */
        fun forSpec(
            context: Context, moduleId: String, spec: ModsSpec,
            onRestart: (() -> Unit)? = null, onChanged: () -> Unit = {},
            applySetting: ((String, String) -> Unit)? = null,
            pickFile: ((((android.net.Uri?) -> Unit)) -> Unit)? = null,
        ): ModsPage? = if (!spec.any) null else ModsPage(context, moduleId, spec, onRestart, onChanged, applySetting, pickFile)
    }
}
