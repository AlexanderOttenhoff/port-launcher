package app.portlauncher.library

import app.portlauncher.util.writeTextAtomic
import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** One N64 ROM found in a scanned file (the file itself, or one zip entry). */
data class ScannedRom(val zipEntry: String, val sha1: String, val title: String, val gameCode: String)

/** A candidate file seen in a ROM folder; size + modified decide whether a rescan must reopen it. */
data class ScannedFile(
    val uri: String,
    val name: String,
    val size: Long,
    val modified: Long,
    val roms: List<ScannedRom>,
    val error: String,
)

data class LibraryData(val folders: List<String>, val files: List<ScannedFile>)

/** Persists the ROM folders and the last scan as files/library.json. */
class LibraryStore(context: Context) {
    private val file = File(context.filesDir, "library.json")

    fun load(): LibraryData {
        if (!file.isFile) {
            return LibraryData(emptyList(), emptyList())
        }
        return runCatching {
            val json = JSONObject(file.readText())
            val folders = json.optJSONArray("folders").strings()
            val files = json.optJSONArray("files")?.let { array ->
                (0 until array.length()).map { i -> array.getJSONObject(i).toScannedFile() }
            }.orEmpty()
            LibraryData(folders, files)
        }.getOrElse { LibraryData(emptyList(), emptyList()) }
    }

    fun save(data: LibraryData) {
        val json = JSONObject()
            .put("folders", JSONArray(data.folders))
            .put("files", JSONArray(data.files.map { it.toJson() }))
        // Atomic (util/AtomicFiles.kt): losing library.json means losing the player's ROM folder grants.
        file.writeTextAtomic(json.toString())
    }

    private fun JSONArray?.strings(): List<String> =
        if (this == null) emptyList() else (0 until length()).map { getString(it) }

    private fun JSONObject.toScannedFile() = ScannedFile(
        uri = getString("uri"),
        name = getString("name"),
        size = getLong("size"),
        modified = getLong("modified"),
        roms = optJSONArray("roms")?.let { array ->
            (0 until array.length()).map { i -> parseRom(array.getJSONObject(i)) }
        }.orEmpty(),
        error = optString("error"),
    )

    private fun ScannedFile.toJson() = JSONObject()
        .put("uri", uri)
        .put("name", name)
        .put("size", size)
        .put("modified", modified)
        .put("error", error)
        .put("roms", JSONArray(roms.map {
            JSONObject().put("zipEntry", it.zipEntry).put("sha1", it.sha1).put("title", it.title).put("gameCode", it.gameCode)
        }))

    companion object {
        fun parseRom(json: JSONObject) = ScannedRom(
            zipEntry = json.optString("zipEntry"),
            sha1 = json.getString("sha1"),
            title = json.optString("title"),
            gameCode = json.optString("gameCode"),
        )
    }
}
