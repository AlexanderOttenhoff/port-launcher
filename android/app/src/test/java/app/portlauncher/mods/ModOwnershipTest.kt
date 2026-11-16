package app.portlauncher.mods

import org.junit.Assert.assertEquals
import org.junit.Test

class ModOwnershipTest {
    private fun pkg(name: String, vararg files: String) =
        InstalledPackage(name, name, "1.0.0", files.toList(), emptyList(), enabled = true)

    @Test fun removingAPackageKeepsFilesAnotherPackageStillLists() {
        val a = pkg("A-Lib", "lib.nrm", "a.nrm")
        val b = pkg("B-Mod", "lib.nrm", "b.nrm")
        assertEquals(setOf("a.nrm"), ModStore.orphanedFiles(listOf(a), listOf(b)))
    }

    @Test fun removingEveryOwnerDeletesTheSharedFile() {
        val a = pkg("A-Lib", "lib.nrm")
        val b = pkg("B-Mod", "lib.nrm", "b.nrm")
        assertEquals(setOf("lib.nrm", "b.nrm"), ModStore.orphanedFiles(listOf(a, b), emptyList()))
    }

    @Test fun updateDeletesOnlyFilesTheNewVersionDropped() {
        val old = pkg("A-Mod", "a.nrm", "old_textures.rtz")
        val new = pkg("A-Mod", "a.nrm", "textures.rtz")
        assertEquals(setOf("old_textures.rtz"), ModStore.orphanedFiles(listOf(old), listOf(new)))
    }

    @Test fun clashesNameTheOtherOwnerButNotThePackagesOwnOlderVersion() {
        val installed = listOf(pkg("A-Mod", "a.nrm", "shared.nrm"), pkg("B-Mod", "b.nrm"))
        assertEquals(mapOf("shared.nrm" to "A-Mod"), ModStore.fileOwners(listOf("shared.nrm", "c.nrm"), "C-Mod", installed))
        assertEquals(emptyMap<String, String>(), ModStore.fileOwners(listOf("a.nrm", "shared.nrm"), "A-Mod", installed))
    }
}
