# Building an N64Recomp module (contributor guide)

Recomp modules contain machine-translated game code, so they are **not** distributed from this project's official
module repository (ROADMAP, "Decomp first"). We build proof-of-concept modules to prove the process and document it
here, so third parties can build and host recomp modules in a separate repository. The worked examples in
`modules/` are Banjo (`banjo-recomp`), Majora's Mask (`zelda64-recomp`), Banjo-Tooie (`banjotooie-recomp`) and
Donkey Kong 64 (`dk64-recomp`).

## 1. Check the project

- **RecompFrontend-based** (Banjo, MM's `recomp-frontend` branch, Banjo-Tooie, DK64): easiest. Older projects with
  their own UI copy and a pre-plume RT64 (Zelda64Recomp `dev`, many smaller recomps) need their libraries moved
  forward first.
- Library pins (`lib/rt64`, `lib/N64ModernRuntime`, `lib/RecompFrontend`): if they're on or after Banjo's, our
  library patches (`modules/banjo-recomp/patches/{plume,rt64,N64ModernRuntime,RecompFrontend}`) usually apply
  unchanged, forks included (they did for MM, Banjo-Tooie and DK64's forks).
- ROM: the exact revision and SHA-1 the project needs (usually in its README/BUILDING). Some projects decompress the
  ROM before code generation (Banjo, MM, DK64 each have their own decompressor); some commit their generated code
  (Banjo-Tooie: for local builds only, to revisit before any release).

### B2 projects (own Zelda64Recomp-dev style UI): Mega Man 64, Bomberman 64

Many smaller recomps copied Zelda64Recomp's `dev` frontend (`src/main`, `src/ui`, `src/game`) instead of using
RecompFrontend. Check their library pins first: Mega Man 64 (`mm64-recomp`) and Bomberman 64 (`bm64-recomp`) both pin
rt64 71b5906 (2026-02, already post-plume) and an N64ModernRuntime from 2026-01, so nothing had to be moved forward:
Banjo's plume and N64ModernRuntime patches and Snowboard Kids 2's re-anchored rt64 series (`modules/sk2-recomp/patches/rt64`,
incl. the 4096 texture-set fix; the patched plume is committed into rt64 instead of a pointer patch) apply unchanged.
Their prepare.sh files point at those directories rather than copying them. A project on a pre-plume rt64 would need
its rt64/N64ModernRuntime moved forward first (as Zelda64Recomp's `dev` branch would).

The game patch is the same for both (one commit each, `modules/<id>/patches/<Repo>/0001-*`): the CMake changes of
section 2, plus in the copied frontend
- `src/main/rt64_render_context.cpp`: RT64 gets the `ANativeWindow` from SDL's WM info (as our RecompFrontend patch);
- `src/ui/ui_renderer.cpp`: the menus' `SwapChainFormat` is RGBA8 on Android, matching our rt64's RGBA8 presentation;
- `src/ui/ui_state.cpp`: `<sstream>` (libc++); `src/main/main.cpp`: no window icon on Android;
- direct start in `main()` as DK64's patch (`select_rom`, `start_game`, `hide_all_contexts` before `recomp::start`).
  This runtime has no custom game modes, so `--game-mode` is accepted and ignored. Mega Man 64's `main()` also turned
  on every installed mod at start (an upstream TODO); with `--rom` it now leaves that to `mods.json`, which the shim
  writes from the launcher's choice.

The shared `recompfrontend_shim.cpp` works unchanged: everything it calls or wraps exists in the copied frontend with
the same signatures (`main` with `SDL_MAIN_HANDLED`, `recompui::show_context(ContextId, std::string_view)`,
`message_box`, `osViSwapBuffer_recomp`, `register_base_export`). Only `PLM_C_STICK_MODIFIER` needs RecompFrontend's
input code. JFG's own `plm_shim.cpp` exists because its fork's entry point is `SDL_main` and it has its own launcher
and language files, not because B2 projects need one.

ROMs: Mega Man 64 recompiles the retail ROM directly. Bomberman 64 wants the decompressed ROM; its BUILDING.md says
to build the BM64 decomp for that, but only the decomp's own decompressor (`tools/bm64decompress.cpp`, standard C++,
the Makefile's `decompress` step) is needed: no IDO and no Python. Its output matches the decomp's `checksum.md5`.

### Own-launcher projects with a decomp-built ELF: Pilotwings 64, Body Harvest

danielgomesvieira2000's recomps (`pw64-recomp`, `bh-recomp`; Body Harvest's tree was copied from Pilotwings 64's)
have their own small launcher on RecompFrontend's recompui and a different pipeline:
- **Submodule patches by script:** upstream never hand-edits its submodules; `tools/patch_all.py` rewrites them in
  place (idempotent). prepare.sh runs it and commits the result per submodule on branch `plm-android`, so our
  `git am` series has a clean base to 3-way on.
- **Library patches:** Banjo's plume, N64ModernRuntime and RecompFrontend series apply unchanged. Pilotwings 64's
  rt64 is stock 5473732 and takes Snowboard Kids 2's rt64 series (Blast Corps' doesn't apply there: its RGBA8
  swap-chain hunk is anchored on Blast Corps' fork).
- **Code generation from the decomp:** N64Recomp reads an ELF, not the ROM. Pilotwings 64's
  `tools/generate_game.py <rom>` builds `lib/Pilotwings64Decomp` with IDO (`RECOMP_BUILD=1`, must match the ROM's
  SHA-1; splat in a venv), runs N64Recomp/RSPRecomp (`tools/recompile.sh`) and compiles the C patches with the host
  clang `-target mips` + ld.lld (`tools/build_patches.py` → `RecompiledPatches/`). Body Harvest's equivalent is
  `tools/regenerate.sh`. Upstream's scripts use `nproc` jobs; prepare.sh puts an `nproc` shim on `PATH` and sets
  `CMAKE_BUILD_PARALLEL_LEVEL` to keep the 4-job cap, and passes `MAKEFLAGS=CC_CHECK=true` (skips the decomp's
  host-compiler syntax check, which writes nothing).
- **Game patch** (one commit): main accepts `--rom <path>` (`--skip-launcher`/`--game-mode` are implied/ignored),
  the settings dir follows `APP_FOLDER_PATH`, no chdir to the executable's dir on Android, `SDL_MAIN_HANDLED`,
  RT64's window is the `ANativeWindow` from `SDL_GetWindowWMInfo` (`src/renderer.cpp`), and the CMake changes of
  section 2 (shared library, nfd stub, DXC by host CPU, `PLUME_SDL_VULKAN_ENABLED`/`RT64_SDL_WINDOW_VULKAN`
  Linux-only, SDL includes for subprojects). Pilotwings 64's crash handler is already a stub off Windows; Body
  Harvest's needed an Android branch.
- The shared `recompfrontend_shim.cpp` works unchanged (`PLM_LOG_TAG`, the usual link wraps); the target is
  `Pilotwings64Recomp`, injected with `CMAKE_PROJECT_Pilotwings64Recomp_INCLUDE`.

### Projects with their own frontend and Android build: Pokémon Snap

Snap64 Recomp (`snap-recomp`) is a third kind: its own frontend (menus drawn in the game's font, `snapsettings.json`,
no RmlUi/recompui), its own forks of rt64, plume and N64ModernRuntime (branch `snap64`) that already carry an Android
build (SDL-window Vulkan surface, RGBA swap chain, app lifecycle, ARM64 icache flush after mod patches), and an
`android/` app of its own. So none of our shared library patches are applied, and the game patch is tiny: an
`SNAP_EXTERNAL_SDL2` switch (our static SDL2 instead of its shared `lib/SDL`) and `--rom` handed to its ROM chooser's
`SNAP_ROM_PICK` hook. The shared RecompFrontend shim doesn't fit (it wraps recompui symbols, enters through `main`);
`modules/snap-recomp/plm_shim.cpp` is a small own one: `SNAP_DATA_DIR` = the launcher's config dir, the shipped files
(what upstream's Gradle build stages as assets) copied in when missing, mods link + `mods.json`, `SDL_main`.

Its code comes from an ELF, not the ROM (prepare.sh follows upstream BUILDING.md steps 1-9): build the
ethteck/pokemonsnap decomp at `3a236dc` with IDO (its `configure.py --setup` downloads the static recompilations;
`pigment64` comes from `cargo install`), relink with `--emit-relocs`, N64Recomp + `tools/hook_funcs.py`, then the
port's game-side patches with the decomp's IDO 7.1 and N64Recomp again. Pitfalls: run the decomp's `configure.py`
only once per extraction (`-c`), or splat's cache drops symbols from `undefined_syms_auto.txt` and the relink fails;
copy `rt64_extended_gbi.h` into the decomp's `include/`. Checks: `.main` of the ELF matches the ROM, and
`tools/gen_overlays.py` reproduces the tracked `src/recomp_overlays.inl` exactly.

## 2. Patch the game repository (keep it small)

Per game, in `modules/<id>/patches/<Repo>/` (format-patch files applied with `git am`):
- **Android build:** the game target is a `SHARED` library on Android; find SDL2 before the subprojects; a stub
  `nfd` target (`src/android/nfd_android_stub.cpp`, no native file dialogs); `file_to_c` →
  `${RT64_HOST_FILE_TO_C}` when cross-compiling; link `SDL2::SDL2`, FreeType, `android`, `log`; define
  `PLUME_SDL_VULKAN_ENABLED` and `RT64_SDL_WINDOW_VULKAN`; DXC picked by `CMAKE_HOST_SYSTEM_PROCESSOR`.
- **Direct start:** parse `--rom <path>`, `--skip-launcher`, `--game-mode <id>` on every platform; after the game is
  registered, `select_rom()` then `start_game(game_id, game_mode)` and `hide_all_contexts()`.
- **Android gaps seen so far:** `execinfo.h`/`backtrace()` need API 33 (we target 28); window icons are Linux-only
  (`__gnu_linux__`, not `__linux__`); vcpkg toolchains and curl (DK64's fork) are skipped or stubbed.

## 3. Module files (`modules/<id>/`)

- `module.cmake`: injected with `CMAKE_PROJECT_<Project>_INCLUDE`; adds the shared shim
  (`modules/common/n64recomp/recompfrontend_shim.cpp`) with a `PLM_LOG_TAG`, sets `OUTPUT_NAME plm_<id>`, 16 KB page
  alignment and the link wraps (first frame, error boxes, frontend screens; `PLM_C_STICK_MODIFIER` for Zelda-style
  C buttons).
- `manifest.json`: id, game id, title, header `game_code`, region/revision, ROM SHA-1, upstream repo/commit,
  licence, capabilities, `mod_sources` (Thunderstore community). The port's own settings come from
  `data/settings_schema.json`, which `build.sh` generates with `tools/schema/recompfrontend_schema.py`
  (docs/planning/MENU.md "Port schemas").
- `prepare.sh`: clone and pin, submodules, our library patches, the game patches (and the project's own required
  patches if it has any), decompress, run N64Recomp/RSPRecomp, build a host `file_to_c`.
- `build.sh`: NDK cross-build in a memory-capped scope (no swap on the dev machine), stage `libplm_<id>.so`, data
  files and the manifest for the APK.

## 4. Test

1. Emulator: the game appears as playable, ROM accepted (`--rom … accepted` in logcat), no crash. Recomps usually
   draw no frames there (ARM translation), so the real test is a device.
2. Device: first frame (host log), Back → our menu → Resume / Quit, controls, a few minutes of play.

## 5. Native features

Small game patches of our own (e.g. Banjo's `patches/plm_native.c`: Start skips cutscenes, fast text) read their
setting through `plm_get_option`, a host function in the shared shim; see MENU.md.
