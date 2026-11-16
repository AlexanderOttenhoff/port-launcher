package app.portlauncher.library

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import app.portlauncher.core.CoreBridge
import org.json.JSONObject

/**
 * Walks ROM folders granted through the Storage Access Framework and identifies candidate files in the
 * core. Files whose size and modification time haven't changed keep their previous result.
 */
class LibraryScanner(private val context: Context) {
    private data class Candidate(val uri: Uri, val name: String, val size: Long, val modified: Long)

    fun scan(folders: List<String>, previous: List<ScannedFile>, onProgress: (done: Int, total: Int) -> Unit): List<ScannedFile> {
        val candidates = folders.flatMap { listCandidates(Uri.parse(it)) }.distinctBy { it.uri.toString() }
        val known = previous.associateBy { it.uri }
        return candidates.mapIndexed { index, candidate ->
            onProgress(index, candidates.size)
            val old = known[candidate.uri.toString()]
            if (old != null && old.size == candidate.size && old.modified == candidate.modified && old.error.isEmpty()) {
                old
            } else {
                scanFile(candidate)
            }
        }.also { onProgress(candidates.size, candidates.size) }
    }

    private fun listCandidates(treeUri: Uri): List<Candidate> {
        val result = mutableListOf<Candidate>()
        val pending = ArrayDeque(listOf(DocumentsContract.getTreeDocumentId(treeUri)))
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )
        while (pending.isNotEmpty()) {
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, pending.removeFirst())
            runCatching {
                context.contentResolver.query(children, columns, null, null, null)?.use { cursor ->
                    while (cursor.moveToNext()) {
                        val id = cursor.getString(0)
                        val name = cursor.getString(1) ?: continue
                        if (cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                            pending.addLast(id)
                        } else if (isCandidate(name)) {
                            result += Candidate(
                                DocumentsContract.buildDocumentUriUsingTree(treeUri, id), name,
                                cursor.getLong(3), cursor.getLong(4),
                            )
                        }
                    }
                }
            }.onFailure { Log.w(TAG, "Can't list $children", it) }
        }
        return result
    }

    private fun scanFile(candidate: Candidate): ScannedFile {
        val json = runCatching {
            context.contentResolver.openFileDescriptor(candidate.uri, "r")?.use { pfd ->
                JSONObject(CoreBridge.scanFd(pfd.fd, candidate.name))
            }
        }.getOrNull()
        val roms = json?.optJSONArray("roms")?.let { array ->
            (0 until array.length()).map { LibraryStore.parseRom(array.getJSONObject(it)) }
        }.orEmpty()
        val error = json?.optString("error").orEmpty().ifEmpty { if (json == null) "can't open file" else "" }
        return ScannedFile(candidate.uri.toString(), candidate.name, candidate.size, candidate.modified, roms, error)
    }

    companion object {
        private const val TAG = "plm-library"

        // Mirrors plm::is_candidate_file, to avoid a JNI call per directory entry.
        fun isCandidate(name: String): Boolean {
            val lower = name.lowercase()
            return listOf(".z64", ".v64", ".n64", ".zip").any { lower.endsWith(it) }
        }
    }
}
