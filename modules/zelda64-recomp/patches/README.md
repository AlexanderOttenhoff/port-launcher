# Android patches for Zelda 64: Recompiled (Majora's Mask)

Base: upstream's `recomp-frontend` branch (ab0fcb5, 2026-08), the RecompFrontend version of
Zelda64Recomp. It pins newer rt64/N64ModernRuntime/RecompFrontend commits than Banjo: Recompiled, and
Banjo's library patches (`modules/banjo-recomp/patches/{plume,rt64,N64ModernRuntime,RecompFrontend}`)
apply to them unchanged, so prepare.sh uses those instead of keeping copies. The `dev` branch (1.2.x)
still uses a pre-plume RT64 that those patches don't fit.

`Zelda64Recomp/`:
1. DXC chosen by the host CPU (Banjo's 0004, applied as-is).
2. Android build: shared library, nfd stub, host `file_to_c`, SDL2 found before the subprojects.
3. `--rom`, `--skip-launcher`, `--game-mode` on every platform (direct start of MM, which is entry 1 of
   `supported_games`; the OoT entry is an upstream "coming soon" placeholder); no window icon on
   Android.

ROM decompression: `decompress_mm.py` (ours), same steps as the MM decomp's `tools/fixbaserom.py`,
checked against the decomp's MD5s. Long term these belong upstream, as for Banjo.
