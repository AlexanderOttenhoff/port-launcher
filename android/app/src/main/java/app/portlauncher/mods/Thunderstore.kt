package app.portlauncher.mods

import app.portlauncher.util.writeTextAtomic
import org.json.JSONArray
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/** A Thunderstore package's latest version, as the browse list needs it. */
data class RemotePackage(
    val fullName: String, // Owner-Name, stable across versions
    val owner: String,
    val name: String,
    val version: String,
    val description: String,
    val downloadUrl: String,
    val fileSize: Long,
    val downloads: Long,
    val dependencies: List<String>, // Owner-Name-1.2.3
    val categories: List<String>,
) {
    val displayName: String get() = name.replace('_', ' ')
}

/**
 * Read-only client for a Thunderstore community's public package API (docs/planning/MODS.md). The package list is
 * cached for an hour; call off the UI thread.
 */
class Thunderstore(private val community: String, private val cacheDir: File) : RemoteModSource {
    override val label = "Thunderstore ($community)"

    override val sorts = listOf("downloads" to "Most downloaded", "name" to "Name")

    /** Categories come from the packages themselves (Thunderstore tags), most used first. */
    override fun categories(): List<Pair<String, String>> = runCatching {
        packages().flatMap { it.categories }.groupingBy { it }.eachCount().entries
            .sortedByDescending { it.value }.map { it.key to "${it.key} (${it.value})" }
    }.getOrDefault(emptyList())

    /** Thunderstore's API hands over the whole community at once (cached), so search, sort and filter run locally. */
    override fun browse(query: BrowseQuery): BrowsePage {
        val text = query.text.trim().lowercase()
        var list = packages()
        if (text.isNotEmpty()) list = list.filter {
            it.name.lowercase().contains(text) || it.owner.lowercase().contains(text) || it.description.lowercase().contains(text)
        }
        if (query.category.isNotEmpty()) list = list.filter { query.category in it.categories }
        list = if (query.sort == "name") list.sortedBy { it.displayName.lowercase() } else list.sortedByDescending { it.downloads }
        val per = 30
        val from = (query.page - 1) * per
        return BrowsePage(list.drop(from).take(per), list.size > from + per)
    }

    override fun webUrl(pkg: RemotePackage) = "https://thunderstore.io/c/$community/p/${pkg.owner}/${pkg.name}/"

    override fun details(pkg: RemotePackage): ModDetails = ModDetails(
        summary = pkg.description, text = "",
        files = listOf(ModFile("${pkg.fullName}-${pkg.version}.zip", pkg.fileSize, pkg.downloads, supported = true)),
        downloads = pkg.downloads, likes = 0, updated = 0, license = "",
    )
    private val cacheFile = File(cacheDir, "thunderstore-$community.json")

    override fun packages(refresh: Boolean): List<RemotePackage> {
        val fresh = cacheFile.isFile && System.currentTimeMillis() - cacheFile.lastModified() < CACHE_MS
        val text = if (fresh && !refresh) cacheFile.readText() else download().also {
            // Atomic (util/AtomicFiles.kt): a cut-off cache file inside the hour would fail to parse until it expired.
            cacheFile.writeTextAtomic(it)
        }
        return parse(text)
    }

    private fun download(): String {
        val connection = open(URL("https://thunderstore.io/c/$community/api/v1/package/"))
        return connection.inputStream.bufferedReader().use { it.readText() }
    }

    /** Downloads [pkg]'s zip to [out], reporting progress from 0 to 1. */
    override fun downloadPackage(pkg: RemotePackage, out: File, progress: (Float) -> Unit, file: String?): String {
        val connection = open(URL(pkg.downloadUrl))
        val total = connection.contentLengthLong.takeIf { it > 0 } ?: pkg.fileSize
        val type = connection.contentType.orEmpty()
        if (type.startsWith("text/html")) throw IOException("Thunderstore sent a web page instead of the mod")
        out.parentFile?.mkdirs()
        connection.inputStream.use { input ->
            out.outputStream().use { output ->
                val buffer = ByteArray(64 * 1024)
                var done = 0L
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    output.write(buffer, 0, n)
                    done += n
                    if (total > 0) progress((done.toFloat() / total).coerceAtMost(1f))
                }
            }
        }
        return out.name
    }

    private fun open(url: URL): HttpURLConnection {
        val connection = url.openConnection() as HttpURLConnection
        connection.connectTimeout = 15_000
        connection.readTimeout = 30_000
        connection.instanceFollowRedirects = true
        connection.setRequestProperty("User-Agent", "port-launcher")
        if (connection.responseCode !in 200..299) {
            throw IOException("Thunderstore answered ${connection.responseCode}")
        }
        return connection
    }

    private fun parse(text: String): List<RemotePackage> {
        val array = JSONArray(text)
        return (0 until array.length()).mapNotNull { i ->
            val p = array.getJSONObject(i)
            if (p.optBoolean("is_deprecated") || p.optBoolean("has_nsfw_content")) return@mapNotNull null
            val versions = p.optJSONArray("versions") ?: return@mapNotNull null
            if (versions.length() == 0) return@mapNotNull null
            val latest = versions.getJSONObject(0)
            val downloads = (0 until versions.length()).sumOf { versions.getJSONObject(it).optLong("downloads") }
            RemotePackage(
                fullName = p.getString("full_name"),
                owner = p.optString("owner"),
                name = p.optString("name"),
                version = latest.optString("version_number"),
                description = latest.optString("description"),
                downloadUrl = latest.getString("download_url"),
                fileSize = latest.optLong("file_size"),
                downloads = downloads,
                dependencies = latest.optJSONArray("dependencies").strings(),
                categories = p.optJSONArray("categories").strings(),
            )
        }.sortedByDescending { it.downloads }
    }

    companion object {
        private const val CACHE_MS = 60 * 60 * 1000L

        /** "Owner-Name-1.2.3" → "Owner-Name". */
        fun packageOf(dependency: String): String = dependency.substringBeforeLast('-')
    }
}

internal fun JSONArray?.strings(): List<String> =
    if (this == null) emptyList() else (0 until length()).map { getString(it) }
