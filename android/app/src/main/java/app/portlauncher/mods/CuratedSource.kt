package app.portlauncher.mods

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * One mod on our curated list (owner 2026-11-16, docs/planning/MODS.md "Curated mods"): a third-party mod, always
 * linked (downloaded from the author's own release at install, never bundled or re-hosted), credited. From the module
 * manifest's "mods.curated"; tools/check_mod_links.py checks every [url] before builds (link rot).
 */
data class CuratedMod(
    val id: String,
    val name: String,
    val author: String,
    val license: String,
    val description: String,
    /** The mod's own page (shown and opened in the browser for pictures, changelog, comments). */
    val homepage: String,
    /** The download: a release asset or a site's stable file link. */
    val url: String,
    /** The download's file name (its extension decides how it installs); default: the URL's last segment. */
    val file: String,
    /** Archive entries to keep from a zip (one variant of several); empty = every .o2r/.otr in it. */
    val entries: Set<String>,
    /** Installed keys this entry takes over (an earlier install of the same mod under another source's key). */
    val replaces: Set<String>,
) {
    /** The installed item's key. */
    val fullName: String get() = "curated-$id"

    fun toJson(): JSONObject = JSONObject().put("id", id).put("name", name).put("author", author).put("license", license)
        .put("description", description).put("homepage", homepage).put("url", url).put("file", file)
        .put("entries", JSONArray(entries.toList())).put("replaces", JSONArray(replaces.toList()))

    companion object {
        fun fromJson(o: JSONObject): CuratedMod {
            fun set(key: String) = o.optJSONArray(key)?.let { a -> (0 until a.length()).map { a.getString(it) }.toSet() }.orEmpty()
            val url = o.optString("url")
            return CuratedMod(o.optString("id"), o.optString("name"), o.optString("author"), o.optString("license"),
                o.optString("description"), o.optString("homepage"), url,
                o.optString("file").ifEmpty { url.substringAfterLast('/').substringBefore('?') }, set("entries"), set("replaces"))
        }

        fun listFromJson(a: JSONArray?): List<CuratedMod> = (0 until (a?.length() ?: 0)).map { fromJson(a!!.getJSONObject(it)) }
    }
}

/**
 * The curated list as a mod source, so the Mods tab's list, detail view and install work as for any source. No
 * search, sort or categories: it's a short list we chose. Blocking network I/O: call off the UI thread.
 */
class CuratedSource(val mods: List<CuratedMod>) : RemoteModSource {
    override val label = "our recommended list"
    override val sorts: List<Pair<String, String>> = emptyList()
    override val searchable = false

    private fun pkg(m: CuratedMod) = RemotePackage(
        fullName = m.fullName, owner = m.author, name = m.name,
        // The download link stands in for a version: a new release is a new link, which shows as an update.
        version = m.url, description = m.description, downloadUrl = m.url, fileSize = 0, downloads = 0,
        dependencies = emptyList(), categories = emptyList(),
    )

    fun entry(pkg: RemotePackage): CuratedMod? = mods.firstOrNull { it.fullName == pkg.fullName }

    override fun browse(query: BrowseQuery) = BrowsePage(mods.map(::pkg), hasMore = false)

    override fun packages(refresh: Boolean) = mods.map(::pkg)

    override fun webUrl(pkg: RemotePackage) = entry(pkg)?.homepage?.takeIf { it.isNotEmpty() }

    override fun details(pkg: RemotePackage): ModDetails? = entry(pkg)?.let { m ->
        ModDetails(
            summary = m.description,
            // The credit (owner 2026-11-16: always credit authors), and that it comes from the author, not from us.
            text = "By ${m.author.ifEmpty { "its author" }}. Downloaded from the author's own release when you install it." +
                (if (m.homepage.isNotEmpty()) "\n\n${m.homepage}" else ""),
            files = listOf(ModFile(m.file, 0, 0, supported = true)),
            downloads = 0, likes = 0, updated = 0, license = m.license,
        )
    }

    override fun downloadPackage(pkg: RemotePackage, out: File, progress: (Float) -> Unit, file: String?): String {
        val m = entry(pkg) ?: throw ModInstallException("${pkg.displayName} isn't on the list any more")
        var url = URL(m.url)
        var connection: HttpURLConnection
        var hops = 0
        while (true) {  // follow redirects across hosts (GitHub release assets redirect to their CDN)
            connection = (url.openConnection() as HttpURLConnection).apply {
                instanceFollowRedirects = false
                connectTimeout = 15_000; readTimeout = 30_000
                setRequestProperty("User-Agent", "port-launcher")
            }
            val code = connection.responseCode
            if (code in 300..399 && hops++ < 5) { url = URL(url, connection.getHeaderField("Location")); connection.disconnect(); continue }
            if (code !in 200..299) throw ModInstallException("${m.name}: the download answered HTTP $code. The link may have moved.")
            break
        }
        val total = connection.contentLengthLong
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
                    if (total > 0) progress((done.toFloat() / total).coerceIn(0f, 1f))
                }
            }
        }
        return m.file
    }
}
