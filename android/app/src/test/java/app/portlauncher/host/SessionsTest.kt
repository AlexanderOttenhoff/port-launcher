package app.portlauncher.host

import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

class SessionsTest {
    @get:Rule val tmp = TemporaryFolder()

    @Test fun keepsTheNewestByTimestampNotByName() {
        // "999" sorts after "1000" as a string; numerically it's older and must go first.
        assertEquals(listOf("999"), Sessions.toPrune(listOf("999", "1000", "1001", "1002"), keep = 3))
    }

    @Test fun leavesForeignNamesAlone() {
        assertEquals(listOf("1"), Sessions.toPrune(listOf("notes", "1", "2", ".tmp"), keep = 1))
    }

    @Test fun nothingToPruneUnderTheLimit() {
        assertEquals(emptyList<String>(), Sessions.toPrune(listOf("1", "2"), keep = 3))
    }

    @Test fun createPrunesOldFoldersAndKeepsTheNewOne() {
        val root = File(tmp.root, "sessions")
        for (t in 1L..5L) File(root, "$t").mkdirs().also { File(root, "$t/launch.json").writeText("{}") }
        val created = Sessions.create(tmp.root, now = 10L)
        assertEquals(File(root, "10"), created)
        assertEquals(setOf("4", "5", "10"), root.list()!!.toSet())
    }
}
