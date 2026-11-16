# port-launcher: map for agents

One Android APK running native N64 decomp/recomp ports as downloadable modules (RP6 handheld is the main device).
Decisions and owner rules live in the auto-memory and docs/planning; this file is where things are and how to run them.

## Find things cheaply
- **Symbols in upstream C/C++ checkouts:** `tools/dev/sym.sh <name> [repo]` (index in build/index, rebuild with
  `tools/dev/index_symbols.py [repo]` after a fresh prepare; universal-ctags, ~1 min for all, seconds per repo). Prefer it over grepping big files such as
  GoldenEye's `src/game/bondview2.c` (10k lines); then read ~40 lines around the hit.
- Kotlin and docs: ripgrep with `-n` and a narrow path. Read files in ranges, not whole (GameMenu.kt ~1000 lines).
- Docs: `docs/planning/ROADMAP.md` (priorities, known bugs, future ideas), `GAME_STATUS.md` (per-game state),
  `MENU.md` (menus, settings, profiles), `MODS.md` (mods: curated/linked, built-in, policy), `SAVE_STATES.md`,
  `PORT_REVIEW.md`, `DECOMP_ROLLOUT.md`; `docs/MODULE_SPEC.md` (manifest, launch.json), `docs/ARCHITECTURE.md`.

## Layout
- `android/app/src/main/java/app/portlauncher/`
  - `host/` game process: `Sdl2HostActivity.kt` (keys, menu, launch.json refresh, select passthrough),
    `GameMenu.kt` (the tabbed menu used everywhere: Game/Input/Mods/Achievements/Settings with profiles and
    category drill-down), `ModsPage.kt` (Mods tab model), `ModuleLauncher.kt` (writes launch.json, starts games).
  - `library/` launcher: `LibraryController.kt` (rows, game page, play, pre-install, app settings),
    `GameCatalog.kt` (manifests → ModuleInfo), `GameSettings.kt` (per-game settings, PortSchema, profiles),
    `LibraryScanner.kt` (recursive SAF scan).
  - `mods/` `ModsSpec` (manifest "mods"), `CuratedSource` (linked third-party mods), `BuiltinMods`, `ModStore`
    (files in SHIP_HOME/mods or recomp mods dir), `Thunderstore` (recomps).
  - `ui/LibraryScreen.kt` (Compose library), `downloads/Downloads.kt` (one queue), `util/` (AppFlags, UiState).
- `core/` C++ core (game DB, ROM scan) behind `CoreBridge`.
- `modules/<id>/`: `manifest.json`, `prepare.sh` (pinned upstream + patches), `build.sh`, shim (`plm_shim.cpp` or
  `plm_options.cpp`), `patches/<repo>/NNNN-*.patch`. `modules/common/` (lus shim, Android glue, savestate).
  `modules/downloadable.txt`: served by the module server (a trailing `# pulled` = built but not in catalog.json).
- `tools/schema/*_schema.py`: per-port settings schemas (+ profiles from upstream presets for SoH/2S2H).
- `build/upstream/<repo>`: upstream checkouts (gitignored); `build/modules/apk` staged libs; `build/modules/dist`
  packaged zips + index.json/catalog.json; `build/deps` SDL2 (patched: `tools/deps/patches/`) and lus deps.

## Commands (all print only what matters; full logs in build/logs)
- Build modules: `tools/dev/build_module.sh goldeneye [perfect-dark ...]` (memory-capped; never run uncapped, the
  32 GB/no-swap box froze once; don't run the emulator during builds).
- Bump versions (the app offers updates on change): `tools/dev/bump.sh <id> ...`
- Package for the local server: `tools/dev/pack.sh` (serve: `cd build/modules/dist && python3 -m http.server 8765
  --bind 0.0.0.0`; the device's files/module-index-url points at it).
- APK: `tools/dev/apk.sh` → bin/port-launcher-debug.apk; `--install` needs the owner's OK first (never while they play).
- Upstream patch: edit in build/upstream/<repo>, then `tools/dev/save_patch.sh <id> <repo> "Android: what and why"`.
- Link check for curated mods (also first step of CI's tools/ci/build_modules.sh): `tools/check_mod_links.py`.
- adb: `~/Android/Sdk/platform-tools/adb` (RP6 over Wi-Fi); reading logcat/files is fine, changing the device needs OK.
- SDL rebuild after editing its patches: `tools/deps/build_sdl2.sh`, then relink every module.

## Gotchas
- Shell patterns: `pkill -f` with a literal pattern can kill the agent shell; use `qem[u]`-style patterns.
- Python heredocs that write Kotlin/C: escape `\n` inside string literals (`\\n`), or they become real newlines.
- Module manifest JSON is hand-formatted; edit it as text, then validate with `python3 -m json.tool`.
- GoldenEye patches 0008-0012 are ours (quick select, instant pause, complexity/difficulty, switches, tank); PD 0004-0008.
- Commit and push master freely while the repo is private; ask before force-push, other repos, releases.
