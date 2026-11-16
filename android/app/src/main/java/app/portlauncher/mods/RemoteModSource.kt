package app.portlauncher.mods

import java.io.File

/** One browse request: free text (empty = no search), a sort id from [RemoteModSource.sorts], a category id (empty = all). */
data class BrowseQuery(val text: String = "", val sort: String = "", val category: String = "", val page: Int = 1)

data class BrowsePage(val items: List<RemotePackage>, val hasMore: Boolean)

/** A downloadable file of a mod; [supported] = we can install it (.o2r/.otr/.zip for libultraship, .zip for recomps). */
data class ModFile(val name: String, val size: Long, val downloads: Long, val supported: Boolean)

/** A mod's detail page (owner 2026-11-15: more than a title). [text] is plain text, paragraphs split by blank lines. */
data class ModDetails(
    val summary: String,
    val text: String,
    val files: List<ModFile>,
    val downloads: Long,
    val likes: Long,
    /** Seconds since 1970, 0 = unknown. */
    val updated: Long,
    val license: String,
)

/**
 * Where a module's downloadable mods come from: Thunderstore for the recomps (manifest "mod_sources"), our curated list
 * for the libultraship ports (manifest "mods.curated", CuratedSource; GameBanana browsing was dropped 2026-11-16). The Mods tab and the store only talk to this. Blocking network I/O: call off the UI thread.
 */
interface RemoteModSource {
    /** Shown under the list: "Mods come from <label> and are made by the community." */
    val label: String

    /** False hides the search entry (a short fixed list, CuratedSource). */
    val searchable: Boolean get() = true

    /** Sort choices for the browse list: id to label; the first is the default. */
    val sorts: List<Pair<String, String>>

    /** Category choices (id to label, with counts where known); empty = no category filter. */
    fun categories(): List<Pair<String, String>> = emptyList()

    /** One page of the browse list for [query] (search, sort, category). */
    fun browse(query: BrowseQuery): BrowsePage

    /**
     * The mod's own web page, opened in the system browser (owner 2026-11-15: images, changelogs and comments are the
     * site's job, not ours to re-render). Null when there is none.
     */
    fun webUrl(pkg: RemotePackage): String? = null

    /** The mod's detail page, or null when the source has nothing beyond the list entry. */
    fun details(pkg: RemotePackage): ModDetails? = null

    /** The browsable packages as one list (Thunderstore's dependency resolution needs the whole catalogue). */
    fun packages(refresh: Boolean = false): List<RemotePackage>

    /**
     * Downloads [pkg]'s file to [out] (progress 0..1) and returns the file name it had on the server (its extension
     * decides how it installs). [file] picks one of [ModDetails.files] by name; null = the first supported one.
     */
    fun downloadPackage(pkg: RemotePackage, out: File, progress: (Float) -> Unit, file: String? = null): String
}
