package app.portlauncher.modules

import app.portlauncher.util.writeTextAtomic
import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.zip.ZipInputStream

/**
 * One module the module server offers (an index.json entry, written by tools/package_modules.py). [manifest] is the
 * module's whole manifest, so the library can list it and the game DB can identify ROMs before it's downloaded.
 */
data class RemoteModule(
    val id: String,
    val version: String,
    val url: String,
    val iconUrl: String,
    val size: Long,
    val sha256: String,
    val manifest: JSONObject,
)

/**
 * The module server (first slice, 2026-11-13): one hardcoded index of official decomp modules, published as GitHub
 * release files by CI (.github/workflows/modules.yml), plus the downloaded modules on this device.
 *
 * Layout on the device:
 * - files/module-index.json: the last index fetched (works offline; refreshed on start and on Rescan).
 * - files/module-icons/<id>.png: icons of modules not downloaded yet.
 * - files/installed-modules/<id>/: an unpacked package: manifest.json, icon.png, lib/arm64-v8a/libplm_<id>.so,
 *   data/. Separate from files/modules/<id>/ (the module's config, saves and mods, which survive reinstalls).
 *
 * Why the library is loaded from app storage: on-device spike 1 (docs/planning/ON_DEVICE_RECOMP.md) showed
 * System.load of an app-written .so works at targetSdk 36 on the emulator (API 37) and the RP6 (API 33). Exec
 * doesn't, but nothing here needs it. The .so is made read-only after install (Android 14's rule for
 * dynamically loaded code; not enforced for native libraries in the spike, but it costs nothing).
 *
 * SHORTCUTS (MVP): no signatures (SHA-256 against the index only, and the index comes over HTTPS from GitHub);
 * no update check beyond "index version differs from installed version"; one source, hardcoded. The roadmap's
 * multi-source design (official + user-added secondary repositories) replaces [indexUrl] later.
 */
class ModuleRepository(private val context: Context) {
    private val filesDir = context.filesDir
    private val indexFile = File(filesDir, "module-index.json")
    private val iconsDir = File(filesDir, "module-icons")
    val installedRoot = File(filesDir, "installed-modules")

    /**
     * The official module index. Development override: a file files/module-index-url holding another URL (e.g. a
     * local server on the PC: http://10.0.2.2:8765/index.json from the emulator). Debug builds only would be
     * cleaner; for the MVP the file simply doesn't exist on players' devices.
     */
    private fun indexUrl(): String =
        runCatching { File(filesDir, "module-index-url").readText().trim() }.getOrNull()?.takeIf { it.isNotEmpty() }
            ?: OFFICIAL_INDEX

    /** The index text: the dev override or the official public URL. */
    private fun fetchIndexText(): String = httpGetText(indexUrl())

    /**
     * The catalog text (owner, 2026-11-15: the APK bundles no modules; modules are introduced one by one by listing
     * them in catalog.json on the modules repository's default branch, so adding a module needs no new APK).
     * Dev override: catalog.json next to the overridden index URL (tools/package_modules.py writes one into dist).
     */
    private fun fetchCatalogText(): String {
        val override = runCatching { File(filesDir, "module-index-url").readText().trim() }.getOrNull()
        if (!override.isNullOrEmpty()) return httpGetText(override.substringBeforeLast('/') + "/catalog.json")
        return httpGetText("https://raw.githubusercontent.com/$OFFICIAL_REPO/master/catalog.json")
    }

    private val catalogFile = File(filesDir, "module-catalog.json")

    /**
     * Module ids the catalog lists, in display order, or null for "no catalog known" (never fetched and no cache):
     * then nothing is filtered, so a missing catalog can't hide every module.
     */
    private fun catalogIds(): List<String>? = runCatching {
        val a = JSONObject(catalogFile.readText()).getJSONArray("modules")
        (0 until a.length()).map { a.getJSONObject(it).getString("id") }
    }.getOrNull()

    /** The index restricted to the catalog's modules, in catalog order. */
    private fun applyCatalog(modules: List<RemoteModule>): List<RemoteModule> {
        val ids = catalogIds() ?: return modules
        val byId = modules.associateBy { it.id }
        return ids.mapNotNull { byId[it] }
    }

    /** Fetches catalog.json; keeps the cached copy when offline or malformed. Returns true if it changed. */
    private fun refreshCatalog(): Boolean {
        val text = runCatching { fetchCatalogText() }
            .onFailure { Log.i(TAG, "module catalog not reachable (${it.message}); using the cached copy") }
            .getOrNull() ?: return false
        if (runCatching { JSONObject(text).getJSONArray("modules") }.isFailure) {
            Log.w(TAG, "module catalog malformed; using the cached copy"); return false
        }
        val changed = !catalogFile.exists() || catalogFile.readText() != text
        if (changed) catalogFile.writeTextAtomic(text)
        return changed
    }

    /** The last fetched index (cached copy), or empty. */
    fun cachedIndex(): List<RemoteModule> =
        applyCatalog(runCatching { parse(JSONObject(indexFile.readText())) }.getOrDefault(emptyList()))

    /**
     * Fetches the index and the icons of modules not downloaded yet. Blocking: call off the UI thread. Keeps the
     * cached copy when offline. Returns true if the index changed.
     */
    fun refreshIndex(): Boolean {
        val catalogChanged = refreshCatalog()
        val text = runCatching { fetchIndexText() }
            .onFailure { Log.i(TAG, "module index not reachable (${it.message}); using the cached copy") }
            .getOrNull() ?: return catalogChanged
        val modules = runCatching { applyCatalog(parse(JSONObject(text))) }.getOrNull() ?: return catalogChanged
        iconsDir.mkdirs()
        for (m in modules) {
            val icon = File(iconsDir, "${m.id}.png")
            if (m.iconUrl.isNotEmpty() && !icon.exists()) runCatching { download(m.iconUrl, icon) { } }
                .onFailure { Log.i(TAG, "icon ${m.id}: ${it.message}") }
        }
        val changed = !indexFile.exists() || indexFile.readText() != text
        if (changed) {
            // Atomic (util/AtomicFiles.kt): the cached index is what the library shows offline.
            indexFile.writeTextAtomic(text)
        }
        return changed || catalogChanged
    }

    /** Directory of a downloaded module, or null if it isn't installed. */
    fun installedDir(id: String): File? = File(installedRoot, id).takeIf { File(it, "manifest.json").isFile }

    fun installedLibrary(id: String): File = File(installedRoot, "$id/lib/arm64-v8a/libplm_$id.so")

    fun installedVersion(id: String): String? =
        installedDir(id)?.let { runCatching { JSONObject(File(it, "manifest.json").readText()).optString("version") }.getOrNull() }

    /** Icon for a module that isn't bundled: the installed package's, else the one fetched with the index. */
    fun iconFile(id: String): File? =
        listOf(File(installedRoot, "$id/icon.png"), File(iconsDir, "$id.png")).firstOrNull { it.isFile }

    /**
     * Downloads, verifies and unpacks [module] into files/installed-modules/<id>/, replacing an older version.
     * Blocking; [progress] gets 0..1. Throws on a failed download, a checksum mismatch or a malformed package;
     * the previous installation is left untouched in every failure case.
     */
    fun install(module: RemoteModule, progress: (Float) -> Unit) {
        val downloads = File(context.cacheDir, "module-downloads").apply { mkdirs() }
        val zip = File(downloads, "${module.id}.zip")
        try {
            download(module.url, zip) { done -> if (module.size > 0) progress(done.toFloat() / module.size) }
            val actual = sha256(zip)
            if (!actual.equals(module.sha256, ignoreCase = true)) {
                throw IllegalStateException("${module.id}: checksum mismatch (got $actual)")
            }
            // Unpack next to the final place, then swap, so a crash mid-way never leaves a half-installed module.
            val staging = File(installedRoot, ".${module.id}.new").apply { deleteRecursively(); mkdirs() }
            unzip(zip, staging)
            val lib = File(staging, "lib/arm64-v8a/libplm_${module.id}.so")
            if (!File(staging, "manifest.json").isFile || !lib.isFile) {
                staging.deleteRecursively()
                throw IllegalStateException("${module.id}: package is missing manifest.json or its library")
            }
            lib.setReadOnly()
            val target = File(installedRoot, module.id)
            val old = File(installedRoot, ".${module.id}.old")
            old.deleteRecursively()
            if (target.exists()) target.renameTo(old)
            staging.renameTo(target)
            old.deleteRecursively()
            Log.i(TAG, "installed ${module.id} ${module.version}")
        } finally {
            zip.delete()
        }
    }

    /**
     * Deletes a downloaded module (files/installed-modules/<id>/), ROADMAP item 10. Leaves files/modules/<id>/ (saves,
     * config, mods) and the cached index icon alone, so the row can show the download state with its icon again.
     * Renamed out of the way first, so a crash mid-delete never leaves a half-deleted module that still looks
     * installed (installedDir only checks for manifest.json). Returns false if nothing could be removed.
     */
    fun remove(id: String): Boolean {
        val target = File(installedRoot, id)
        if (!target.exists()) return true
        val doomed = File(installedRoot, ".$id.del").apply { deleteRecursively() }
        if (!target.renameTo(doomed)) return target.deleteRecursively()
        doomed.deleteRecursively()
        Log.i(TAG, "removed $id")
        return true
    }

    /** Bytes a downloaded module takes on disk (for the remove prompt). */
    fun installedSize(id: String): Long = File(installedRoot, id).walkBottomUp().filter { it.isFile }.sumOf { it.length() }

    private fun parse(json: JSONObject): List<RemoteModule> {
        val array = json.optJSONArray("modules") ?: return emptyList()
        return (0 until array.length()).mapNotNull { i ->
            val m = array.getJSONObject(i)
            // Only modules for this platform and ABI; anything else is for another launcher version.
            if (m.optString("platform", "android-arm64") != "android-arm64" || m.optInt("abi", 0) != 0) return@mapNotNull null
            RemoteModule(
                id = m.getString("id"),
                version = m.optString("version"),
                url = m.getString("url"),
                iconUrl = m.optString("icon"),
                size = m.optLong("size"),
                sha256 = m.getString("sha256"),
                manifest = m.getJSONObject("manifest"),
            )
        }
    }

    private fun unzip(zip: File, into: File) {
        val root = into.canonicalPath + File.separator
        ZipInputStream(zip.inputStream().buffered()).use { zin ->
            while (true) {
                val entry = zin.nextEntry ?: break
                val out = File(into, entry.name)
                // Zip-slip guard: entries must stay inside the target directory.
                if (!out.canonicalPath.startsWith(root)) throw IllegalStateException("bad zip entry ${entry.name}")
                if (entry.isDirectory) out.mkdirs() else {
                    out.parentFile?.mkdirs()
                    out.outputStream().use { zin.copyTo(it) }
                }
            }
        }
    }

    private fun httpGetText(url: String): String {
        val conn = open(url)
        try {
            if (conn.responseCode != 200) throw IllegalStateException("HTTP ${conn.responseCode} for $url")
            return conn.inputStream.bufferedReader().readText()
        } finally {
            conn.disconnect()
        }
    }

    private fun download(url: String, to: File, progress: (Long) -> Unit) {
        val conn = open(url)
        try {
            if (conn.responseCode != 200) throw IllegalStateException("HTTP ${conn.responseCode} for $url")
            val tmp = File(to.parentFile, to.name + ".part")
            conn.inputStream.use { input ->
                tmp.outputStream().use { out ->
                    val buffer = ByteArray(256 * 1024)
                    var done = 0L
                    while (true) {
                        val n = input.read(buffer)
                        if (n < 0) break
                        out.write(buffer, 0, n)
                        done += n
                        progress(done)
                    }
                }
            }
            // A failed rename used to pass silently and the caller then read a missing or stale file.
            if (!tmp.renameTo(to)) throw IllegalStateException("couldn't move $tmp to $to")
        } finally {
            conn.disconnect()
        }
    }

    // GitHub release downloads redirect to its CDN; HttpURLConnection follows same-protocol redirects itself.
    private fun open(url: String): HttpURLConnection =
        (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "port-launcher")
        }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(1 shl 20)
            while (true) {
                val n = input.read(buffer)
                if (n < 0) break
                digest.update(buffer, 0, n)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    companion object {
        private const val TAG = "plm-modules"

        /** The modules repository: release files (index.json, module zips) and catalog.json. */
        const val OFFICIAL_REPO = "AlexanderOttenhoff/port-launcher-modules"

        /** The official (decomp) module index when the repository is public: its latest release's index.json. */
        const val OFFICIAL_INDEX = "https://github.com/AlexanderOttenhoff/port-launcher-modules/releases/latest/download/index.json"
    }
}
