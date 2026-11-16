package app.portlauncher.library

/**
 * RomM integration: the player's own RomM server as a second ROM source next to local folders (ROADMAP "glue":
 * local directory and/or their own RomM server; library badge 1's middle state, RomState.ROMM).
 *
 * UNIMPLEMENTED (stub, 2026-11-13, owner request). Nothing is connected: [isConfigured] is always false and [has]
 * never matches, so no row gets RomState.ROMM yet. The library and the ROM sources settings page already go through
 * this object, so the real thing only has to fill it in. What it will need:
 * - server URL + credentials (RomM's API token), entered on the ROM sources page; stored privately, never logged;
 * - the server's N64 platform listing, matched by SHA-1 like local files (RomM reports hashes per ROM);
 * - download-on-play into files/games/<id>/rom.z64, through the same import/verify path as local ROMs.
 */
object RommSource {
    /** Whether a RomM server is set up. Stub: always false. */
    fun isConfigured(): Boolean = false

    /** Whether the connected server has a ROM matching one of [sha1s]. Stub: never. */
    @Suppress("UNUSED_PARAMETER")
    fun has(sha1s: Collection<String>): Boolean = false
}
