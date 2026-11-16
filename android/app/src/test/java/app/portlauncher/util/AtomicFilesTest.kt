package app.portlauncher.util

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException

class AtomicFilesTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test fun createsFileAndParentDirectories() {
        val file = File(tmp.root, "a/b/library.json")
        file.writeTextAtomic("{\"x\":1}")
        assertEquals("{\"x\":1}", file.readText())
    }

    @Test fun replacesExistingContentsAndLeavesNoTempFiles() {
        val file = File(tmp.root, "settings.json")
        file.writeText("old, and longer than the new contents")
        file.writeTextAtomic("new")
        assertEquals("new", file.readText())
        // Only the target is left: the unique temp file was renamed over it, not copied.
        assertEquals(listOf("settings.json"), tmp.root.list()!!.toList())
    }

    @Test fun failedRenameKeepsOldContentsAndCleansUp() {
        // The target is a non-empty directory, so rename(2) fails: the old contents must survive, no temp file stays.
        val target = File(tmp.root, "taken").apply { mkdirs() }
        File(target, "child").writeText("keep")
        val thrown = runCatching { target.writeTextAtomic("new") }.exceptionOrNull()
        assertTrue("expected IOException, got $thrown", thrown is IOException)
        assertEquals("keep", File(target, "child").readText())
        assertEquals(listOf("taken"), tmp.root.list()!!.toList())
    }
}
