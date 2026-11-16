package app.portlauncher.mods

import app.portlauncher.util.writeTextAtomic
import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.zip.ZipFile

/** One recomp mod file (.nrm) inside an installed package, read from its mod.json. */
data class ModEntry(val id: String, val displayName: String, val gameMode: Boolean)

/** A package installed for one module. Total conversions ([gameModes]) play as their own library entries. */
data class InstalledPackage(
    val fullName: String,
    val displayName: String,
    val version: String,
    val files: List<String>,
    val mods: List<ModEntry>,
    val enabled: Boolean,
    /** Installed only because another package needed it; removed again when nothing needs it any more. */
    val dependency: Boolean = false,
    /** Packages this one needs (Owner-Name). */
    val requires: List<String> = emptyList(),
) {
    val gameModes: List<ModEntry> get() = mods.filter { it.gameMode }
}

/** What the module needs at launch (launch.json "mods", MODULE_SPEC §4.1). */
data class ModLaunchState(val installed: List<String>, val enabled: List<String>, val optionDefaults: JSONObject) {
    fun toJson(gameMode: String?): JSONObject = JSONObject()
        .put("installed", JSONArray(installed))
        .put("enabled", JSONArray(enabled))
        .put("game_mode", gameMode.orEmpty())
        .put("option_defaults", optionDefaults)
}

/** The game's mod preset: Modern (curated mods on) or Original; [applied] = curated packages already offered once. */
data class PresetState(val modern: Boolean, val applied: Set<String>)

class ModInstallException(message: String) : IOException(message)

/**
 * The launcher's mods for one module: files in the module's dirs.mods folder plus a record of which package put
 * them there. The launcher is the only writer; the module's shim turns the launch state into the port's own config.
 * Blocking I/O: call off the UI thread.
 */
class ModStore(context: Context, private val moduleId: String, private val lus: Boolean = false) {
    private val root = File(context.filesDir, "modules/$moduleId")
    /**
     * Where the port loads mods from. Recomps: the launcher's dirs.mods (the shim points the runtime there).
     * libultraship ports ([lus], owner 2026-11-15): SHIP_HOME/mods = config/mods, which libultraship scans (top level
     * only) for .o2r/.otr/.zip archives at start; switched-off mods wait in config/mods-disabled, outside the scan.
     */
    val modsDir = if (lus) File(root, "config/mods") else File(root, "mods")
    private val disabledDir = File(root, "config/mods-disabled")
    private val recordFile = File(root, "installed-mods.json")
    private val presetFile = File(root, "mod-preset.json")
    private val downloads = File(context.cacheDir, "mod-downloads")

    @Synchronized
    fun installed(): List<InstalledPackage> = runCatching {
        val array = JSONArray(recordFile.readText())
        (0 until array.length()).map { array.getJSONObject(it).toPackage() }
    }.getOrDefault(emptyList())

    /**
     * [curated]: the module's Modern preset (package → option values). Curated mods load only with the Modern
     * preset and not in total conversions ([gameMode]), which bring their own changes.
     */
    fun launchState(curated: Map<String, JSONObject>, gameMode: String?): ModLaunchState {
        val packages = installed()
        val modern = presetState().modern
        val on = packages.filter { p -> p.enabled && (p.fullName !in curated || (modern && gameMode == null)) }
        val defaults = JSONObject()
        for (p in on) {
            val options = curated[p.fullName] ?: continue
            if (options.length() == 0) continue
            p.mods.forEach { defaults.put(it.id, options) }
        }
        return ModLaunchState(
            installed = packages.flatMap { p -> p.mods.map { it.id } },
            enabled = on.flatMap { p -> p.mods.map { it.id } },
            optionDefaults = defaults,
        )
    }

    @Synchronized
    fun presetState(): PresetState = runCatching {
        val json = JSONObject(presetFile.readText())
        PresetState(json.optBoolean("modern", true), json.optJSONArray("applied").strings().toSet())
    }.getOrDefault(PresetState(modern = true, applied = emptySet()))

    @Synchronized
    fun savePreset(state: PresetState) {
        // Atomic (util/AtomicFiles.kt): a cut-off preset file read back as "Modern, nothing applied yet" and
        // re-offered (re-installed) the curated mods the player had removed.
        presetFile.writeTextAtomic(JSONObject().put("modern", state.modern).put("applied", JSONArray(state.applied.toList())).toString(2))
    }

    /**
     * Downloads and installs [pkg] and, first, any missing dependency (latest version), from [source]. Returns
     * the packages installed.
     */
    fun install(
        source: Thunderstore,
        pkg: RemotePackage,
        catalogue: List<RemotePackage>,
        progress: (String, Float) -> Unit,
    ): List<InstalledPackage> {
        val have = installed().map { it.fullName }.toMutableSet()
        val byName = catalogue.associateBy { it.fullName }
        val order = mutableListOf<RemotePackage>()
        fun visit(p: RemotePackage, path: Set<String>) {
            if (p.fullName in have || order.any { it.fullName == p.fullName } || p.fullName in path) return
            for (dep in p.dependencies) {
                val depPkg = byName[Thunderstore.packageOf(dep)]
                    ?: throw ModInstallException("${p.displayName} needs ${Thunderstore.packageOf(dep)}, which isn't available")
                visit(depPkg, path + p.fullName)
            }
            order += p
        }
        visit(pkg, emptySet())
        return order.map { p ->
            val zip = File(downloads, "${p.fullName}-${p.version}.zip")
            try {
                source.downloadPackage(p, zip, { progress(p.displayName, it) })
                installZip(p, zip, asDependency = p.fullName != pkg.fullName).also { have += p.fullName }
            } finally {
                zip.delete()
            }
        }
    }

    /**
     * Mod file ownership (SLOP_AUDIT §1.3, hardening pass 2026-11-14). The recomp runtime loads every mod from one
     * flat dirs.mods folder, so files from all packages share one namespace by file name. The MVP extracted straight
     * into that folder: a package shipping a file name another package already had silently overwrote it, removing
     * either package deleted the file the other still needed, and a failed validation deleted whatever it had
     * already overwritten. Now:
     * - the zip is extracted into a staging folder and validated there, so a bad download never touches modsDir;
     * - a file name another package owns may only be shipped again byte-for-byte (shared ownership, e.g. a
     *   dependency bundled inside a dependent's zip); different contents under the same name refuse the install
     *   with a message naming both packages, rather than one package breaking the other;
     * - removal and updates delete only files no remaining package lists ([orphanedFiles]).
     */
    private fun installZip(pkg: RemotePackage, zip: File, asDependency: Boolean): InstalledPackage {
        modsDir.mkdirs()
        // Inside the module root (not cacheDir), so the final moves are same-filesystem renames.
        val staging = File(root, ".mod-staging").apply { deleteRecursively(); mkdirs() }
        try {
            val written = linkedSetOf<String>()
            try {
                ZipFile(zip).use { archive ->
                    for (entry in archive.entries()) {
                        if (entry.isDirectory) continue
                        val name = entry.name.substringAfterLast('/')
                        if (!(name.endsWith(".nrm") || name.endsWith(".rtz")) || name.startsWith(".")) continue
                        val out = File(staging, name)
                        archive.getInputStream(entry).use { input -> out.outputStream().use { input.copyTo(it) } }
                        written += name
                    }
                }
            } catch (e: java.util.zip.ZipException) {
                throw ModInstallException("${pkg.displayName} isn't a valid mod download")
            }
            if (written.isEmpty()) {
                throw ModInstallException("${pkg.displayName} has no mod files this game can load")
            }
            val mods = mutableListOf<ModEntry>()
            for (name in written.filter { it.endsWith(".nrm") }) {
                val manifest = readModJson(File(staging, name))
                    ?: throw ModInstallException("${pkg.displayName} has an unreadable mod file")
                if (manifest.has("native_libraries")) {
                    // Native code is built for Windows/Linux PCs; the runtime can't load it on Android.
                    throw ModInstallException("${pkg.displayName} only works on a PC: it includes native PC code")
                }
                mods += ModEntry(
                    id = manifest.getString("id"),
                    displayName = manifest.optString("display_name", pkg.displayName),
                    gameMode = manifest.optBoolean("custom_gamemode"),
                )
            }
            val packages = installed()
            for ((file, owner) in fileOwners(written, pkg.fullName, packages)) {
                if (!File(staging, file).readBytes().contentEquals(File(modsDir, file).takeIf { it.isFile }?.readBytes() ?: ByteArray(0))) {
                    val ownerName = packages.find { it.fullName == owner }?.displayName ?: owner
                    throw ModInstallException("${pkg.displayName} and $ownerName both include $file; remove $ownerName first")
                }
            }
            // Validated: move in. A crash part-way leaves files the record doesn't list yet; the next install of the
            // package overwrites them (same names) and nothing else ever looks at unlisted files.
            for (name in written) {
                val target = File(modsDir, name)
                if (!File(staging, name).renameTo(target)) throw ModInstallException("Couldn't install $name")
            }
            val previous = packages.find { it.fullName == pkg.fullName }
            val installedPkg = InstalledPackage(
                pkg.fullName, pkg.displayName, pkg.version, written.toList(), mods, enabled = true,
                // An update keeps how the package came in; a package the player installs directly is theirs.
                dependency = previous?.dependency ?: asDependency,
                requires = pkg.dependencies.map { Thunderstore.packageOf(it) },
            )
            val remaining = packages.filter { it.fullName != pkg.fullName } + installedPkg
            save(remaining)
            // An update that dropped or renamed a file: the old version's leftovers would otherwise load forever.
            if (previous != null) orphanedFiles(listOf(previous), remaining).forEach { File(modsDir, it).delete() }
            return installedPkg
        } finally {
            staging.deleteRecursively()
        }
    }

    @Synchronized
    fun setEnabled(fullName: String, enabled: Boolean) {
        if (lus) {
            // libultraship loads whatever sits in mods/: switching a mod off moves its files out of the scan.
            val pkg = installed().find { it.fullName == fullName } ?: return
            val (from, to) = if (enabled) disabledDir to modsDir else modsDir to disabledDir
            to.mkdirs()
            pkg.files.forEach { File(from, it).takeIf { f -> f.isFile }?.renameTo(File(to, it)) }
        }
        save(installed().map { if (it.fullName == fullName) it.copy(enabled = enabled) else it })
    }

    /**
     * libultraship ports: installs a GameBanana (or other) download as archives in mods/. A bare .o2r/.otr goes in as
     * it is; a .zip holding .o2r/.otr files has those extracted (flattened); a .zip without any is taken as an archive
     * itself (an .o2r is a zip) if it looks like one. Every file is prefixed with the package id so files never collide
     * and removal knows what's ours. An update replaces the package's files.
     */
    @Synchronized
    /**
     * "Add a mod from a file" (owner 2026-11-16): a file the player downloaded themselves, installed like a download.
     * Keyed by its file name ("local-<name>"), so adding the same file again replaces it.
     */
    fun installLocalFile(file: File): InstalledPackage {
        val name = file.name
        val pkg = RemotePackage(
            fullName = "local-" + name.substringBeforeLast('.').replace(Regex("[^A-Za-z0-9._-]"), "_"), owner = "", name = name.substringBeforeLast('.'),
            version = file.length().toString(), description = "", downloadUrl = "", fileSize = file.length(), downloads = 0,
            dependencies = emptyList(), categories = emptyList(),
        )
        val picked = file
        // A source whose "download" copies the picked file, so the archive handling is the one downloads use.
        val local = object : RemoteModSource {
            override val label = "a file"
            override val sorts: List<Pair<String, String>> = emptyList()
            override fun browse(query: BrowseQuery) = BrowsePage(emptyList(), false)
            override fun packages(refresh: Boolean) = emptyList<RemotePackage>()
            override fun downloadPackage(pkg: RemotePackage, out: File, progress: (Float) -> Unit, file: String?): String {
                out.parentFile?.mkdirs()
                picked.copyTo(out, overwrite = true)
                return name
            }
        }
        return installArchive(local, pkg) { }
    }

    fun installArchive(source: RemoteModSource, pkg: RemotePackage, file: String? = null,
                       /** Archive entries to keep (CuratedMod.entries); empty keeps every .o2r/.otr in the zip. */
                       only: Set<String> = emptySet(), progress: (Float) -> Unit): InstalledPackage {
        check(lus) { "installArchive is for libultraship modules" }
        modsDir.mkdirs()
        val download = File(downloads, "${pkg.fullName}.download")
        try {
            val serverName = source.downloadPackage(pkg, download, progress, file)
            // "<id>-" for a mod, "<id><tag>-" for one file of a several-file mod ("<mod>~<file>"), so two files of the same
            // mod never share a prefix (each is its own item: removing one leaves the other).
            val key = pkg.fullName.substringAfterLast("gamebanana-").ifEmpty { pkg.fullName.substringAfterLast('-') }
            val prefix = if ('~' in key) key.substringBefore('~') + Integer.toHexString(key.substringAfter('~').hashCode()).take(4) + "-"
                else "$key-"
            val staged = mutableMapOf<String, File>()   // final file name -> staged file
            val staging = File(root, ".mod-staging").apply { deleteRecursively(); mkdirs() }
            try {
                val lower = serverName.lowercase()
                fun clean(name: String) = prefix + name.substringAfterLast('/').replace(Regex("[^A-Za-z0-9._ -]"), "_")
                if (lower.endsWith(".o2r") || lower.endsWith(".otr")) {
                    val target = File(staging, clean(serverName))
                    download.copyTo(target, overwrite = true)
                    staged[target.name] = target
                } else {
                    ZipFile(download).use { zip ->
                        val archives = zip.entries().toList().filter { e ->
                            !e.isDirectory && (e.name.lowercase().endsWith(".o2r") || e.name.lowercase().endsWith(".otr")) &&
                                (only.isEmpty() || e.name in only)
                        }
                        if (archives.isNotEmpty()) {
                            archives.forEach { e ->
                                val target = File(staging, clean(e.name))
                                zip.getInputStream(e).use { input -> target.outputStream().use { input.copyTo(it) } }
                                staged[target.name] = target
                            }
                        } else {
                            // No inner archives: an .o2r-style zip holds resource paths (folders of assets), not a
                            // single top-level readme or an installer. Accept it when most entries sit in folders.
                            val entries = zip.entries().toList().filter { !it.isDirectory }
                            if (entries.isEmpty() || entries.count { '/' in it.name } * 2 < entries.size) {
                                throw ModInstallException("${pkg.displayName} has no .o2r/.otr archive, so it can't be installed here")
                            }
                            val target = File(staging, clean(serverName.substringBeforeLast('.') + ".zip"))
                            download.copyTo(target, overwrite = true)
                            staged[target.name] = target
                        }
                    }
                }
                // Replace: drop this package's old files (enabled or not), move the new ones in.
                val old = installed().find { it.fullName == pkg.fullName }
                old?.files?.forEach { File(modsDir, it).delete(); File(disabledDir, it).delete() }
                staged.forEach { (name, file) -> file.renameTo(File(modsDir, name)) }
                val record = InstalledPackage(pkg.fullName, pkg.displayName, pkg.version, staged.keys.sorted(), emptyList(), enabled = true)
                // An update keeps its place in the load order; a new mod goes last (loads last, wins conflicts).
                val list = installed().toMutableList()
                val at = list.indexOfFirst { it.fullName == pkg.fullName }
                if (at >= 0) list[at] = record else list += record
                val saved = renumber(list)
                save(saved)
                return saved.first { it.fullName == pkg.fullName }
            } finally {
                staging.deleteRecursively()
            }
        } finally {
            download.delete()
        }
    }

    /**
     * Load order (owner 2026-11-15: mods should be configurable): moves [fullName] one place up ([dir] -1) or down (+1)
     * in the list. libultraship loads mods/ in file-name order and a later archive wins where two replace the same
     * asset, so in lus mode every package's files get a two-digit order prefix ("03_<id>-name.o2r") matching its place;
     * the recomp runtime takes the order from the launch state's "installed" list, which follows the record.
     */
    @Synchronized
    fun move(fullName: String, dir: Int) {
        val list = installed().toMutableList()
        val i = list.indexOfFirst { it.fullName == fullName }
        val j = i + dir
        if (i < 0 || j !in list.indices) return
        list.add(j, list.removeAt(i))
        save(if (lus) renumber(list) else list)
    }

    /** lus mode: gives each package's files the order prefix of its place in [list] (renaming on disk). */
    private fun renumber(list: List<InstalledPackage>): List<InstalledPackage> = list.mapIndexed { index, p ->
        val dir = if (p.enabled) modsDir else disabledDir
        val files = p.files.map { name ->
            val renamed = "%02d_".format(index + 1) + name.replace(Regex("^\\d\\d_"), "")
            if (renamed != name) File(dir, name).renameTo(File(dir, renamed))
            renamed
        }
        p.copy(files = files)
    }

    /** Removes [fullName], then any package installed only as a dependency that nothing still needs. */
    @Synchronized
    fun remove(fullName: String) {
        val before = installed()
        var packages = before
        var toRemove = listOf(fullName)
        while (toRemove.isNotEmpty()) {
            packages = packages.filter { it.fullName !in toRemove }
            val needed = packages.flatMap { it.requires }.toSet()
            toRemove = packages.filter { it.dependency && it.fullName !in needed }.map { it.fullName }
        }
        // Record first, files second: a crash in between leaves unlisted files (harmless), never a record pointing
        // at deleted ones. Only files no remaining package lists go (shared ownership, see installZip).
        save(packages)
        orphanedFiles(before.filter { p -> packages.none { it.fullName == p.fullName } }, packages)
            .forEach { File(modsDir, it).delete(); if (lus) File(disabledDir, it).delete() }
    }

    companion object {
        /** Pure: files of [removed] packages that no package in [remaining] still lists, i.e. safe to delete. */
        fun orphanedFiles(removed: List<InstalledPackage>, remaining: List<InstalledPackage>): Set<String> {
            val stillOwned = remaining.flatMap { it.files }.toSet()
            return removed.flatMap { it.files }.filter { it !in stillOwned }.toSet()
        }

        /**
         * Pure: for each of [files] that a package other than [pkg] (its own older version doesn't count) already
         * owns, that owner. Empty means no clash.
         */
        fun fileOwners(files: Collection<String>, pkg: String, installed: List<InstalledPackage>): Map<String, String> =
            files.mapNotNull { f -> installed.find { it.fullName != pkg && f in it.files }?.let { f to it.fullName } }.toMap()
    }

    @Synchronized
    private fun save(packages: List<InstalledPackage>) {
        root.mkdirs()
        val array = JSONArray()
        packages.forEach { array.put(it.toJson()) }
        // Atomic (util/AtomicFiles.kt): this record is the only thing that knows which package owns which mod file.
        recordFile.writeTextAtomic(array.toString(2))
    }

    private fun readModJson(nrm: File): JSONObject? = runCatching {
        ZipFile(nrm).use { archive ->
            val entry = archive.getEntry("mod.json") ?: return null
            JSONObject(archive.getInputStream(entry).bufferedReader().readText())
        }
    }.getOrNull()

    private fun InstalledPackage.toJson(): JSONObject = JSONObject()
        .put("package", fullName)
        .put("name", displayName)
        .put("version", version)
        .put("files", JSONArray(files))
        .put("enabled", enabled)
        .put("dependency", dependency)
        .put("requires", JSONArray(requires))
        .put("mods", JSONArray().also { array ->
            mods.forEach { array.put(JSONObject().put("id", it.id).put("name", it.displayName).put("game_mode", it.gameMode)) }
        })

    private fun JSONObject.toPackage(): InstalledPackage {
        val mods = optJSONArray("mods")
        return InstalledPackage(
            fullName = getString("package"),
            displayName = optString("name"),
            version = optString("version"),
            files = optJSONArray("files").strings(),
            mods = if (mods == null) emptyList() else (0 until mods.length()).map { i ->
                val m = mods.getJSONObject(i)
                ModEntry(m.getString("id"), m.optString("name"), m.optBoolean("game_mode"))
            },
            enabled = optBoolean("enabled", true),
            dependency = optBoolean("dependency", false),
            requires = optJSONArray("requires").strings(),
        )
    }
}
