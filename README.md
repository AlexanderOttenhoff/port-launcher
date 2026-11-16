# port-launcher (working name)

One Android app that runs native N64 decomp and recomp ports as **modules**, with one library, one in-game menu and
one settings model. "RetroArch for decompilations": tap a game and play, with modern defaults. The main target is
the Retroid Pocket 6.

The idea: one launcher that glues together your ROMs (a local folder now, your own RomM server later), native port
modules (downloaded from the module server), per-game mod sources, and a single settings interface. N64 first,
to keep the scope releasable.

## Status (2026-11-16): alpha in progress

One debug APK; every module downloads from the module server. MVP-quality code: speed over polish until the alpha,
hardening after it. Every module is marked Experimental until play-tested. This is a soft launch of a highly
unstable app: expect crashes and missing features. Planning documents (roadmap, per-game status, decisions) are kept
privately for now; code comments that mention ROADMAP.md, MENU.md and similar refer to them.

**Alpha set (decomps, in catalog.json):**

| Game | Module | Notes |
|---|---|---|
| Super Mario 64 | Ghostship | libultraship |
| Ocarina of Time | Ship of Harkinian | libultraship; Nintendo-style C buttons (L1/Y/X/R1) with the matching glyph mod pre-installed; Modern profiles |
| Majora's Mask | 2 Ship 2 Harkinian | libultraship; Modern profiles |
| Banjo-Kazooie | Lighthouse | libultraship; release candidate |
| Star Fox 64 | Starship | libultraship |
| Super Smash Bros. | BattleShip | libultraship |
| Diddy Kong Racing | Golden Balloon | own engine |
| GoldenEye 007 | GoldenEye PC port | built-in mods: steady aim, quick weapon select, instant pause, complexity/difficulty; twin-stick tank |
| Perfect Dark | Perfect Dark PC port | built-in mods: steady aim, pause on weapon wheel |

Built but pulled from the catalog: Mario Kart 64 (SpaghettiKart), Paper Mario (PaperBoat). Recomps (Banjo-Tooie,
DK64, JFG and others) build locally; recomp support (on-device builds) is post-release.

**Working:**
- **Library:** one row per module with icon, Decomp/Recomp, ROM and maturity badges; games without a ROM say exactly
  which files to provide (versions, SHA-1s, file types). Folders are scanned recursively; Settings → ROM sources has
  the folders and Rescan. Landscape only.
- **Game page and in-game menu (Back):** the same tabbed menu: Game (Launch / Resume, Reset, Quit), Input (stub),
  Mods, Achievements (stub), Settings (profile + Apply with a change count, then the port's own categories).
- **Settings:** each port's own options with upstream defaults (only input defaults are ours); profiles from the
  ports' curated presets (SoH, 2S2H); app settings (General, ROM sources, Experimental).
- **Mods:** built-in mods (our compiled-in features, on/off), downloaded mods (on/off, order, remove), Recommended
  (our curated, linked list), Add a mod from a file, Where to get more mods; Thunderstore for recomps. One download
  queue with a progress bar and a Downloads list.
- **Module server:** CI builds and publishes modules; Update / Update all; remove a downloaded module (saves stay).
- **Frontends:** ES-DE, Argosy and similar can launch games through intents.

**Experimental / known issues:** save states (session only, behind Settings → Experimental); black screen after a
long sleep (fixed 11-16, to confirm on the RP6); GoldenEye stray geometry near the reticle; SM64 hat flicker.

No ROMs, game assets, generated game code or saves belong in this repository. Recomp builds contain code generated
from your ROM: keep them private.

## Docs

| Doc | What |
|---|---|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | components, processes, settings model |
| [MODULE_SPEC.md](docs/MODULE_SPEC.md) | the contract a port implements (v0, unstable) |
| [RECOMP_MODULES.md](docs/RECOMP_MODULES.md) | how to build an N64Recomp module |

## Building

Requirements:
- JDK 17+ (21 tested);
- Android SDK with platform 36.1, NDK 26.0.10792818 and SDK CMake 3.30.3 (`ANDROID_HOME`, default
  `~/Android/Sdk`);
- the Gradle wrapper downloads Gradle 9.4.1.

```bash
tools/build_apk.sh            # debug APK -> bin/port-launcher-debug.apk (arm64-v8a only), no modules bundled
tools/build_apk.sh debug -Pplm.stage=apk   # dev APK with the modules staged in build/modules/apk
tools/test_core.sh            # core unit tests on the host
tools/test_app.sh             # the Kotlin app's JVM unit tests (android/app/src/test)
```

Modules are built separately and staged into `build/modules/apk`, which the APK build picks up:
1. `tools/deps/build_sdl2.sh` and `tools/deps/build_lus_deps.sh` build the shared dependencies once.
2. Per module, `modules/<id>/prepare.sh` makes the pinned checkout with our patches. Recomps also need your ROM at
   this step.
3. `modules/<id>/build.sh` builds and stages the module.

Builds are memory-capped (`JOBS`, `BUILD_MEMORY_MAX`); don't run an emulator during one.

**Testing:** `tools/emulator.sh` starts a headless, landscape emulator (x86_64 with ARM translation) for UI tests.
Use the real device for launching games.

## Layout

```
core/      portable C++20 launcher core (ROM scan, game DB, zip, SHA-1) + unit tests
android/   Gradle project: Kotlin/Compose app, in-game host (SDL2), JNI bridge to core/
modules/   one directory per port: manifest, pinned prepare/build scripts, patches, shim; common/ is shared
tools/     build, test and emulator scripts; deps/ builds SDL2 and libultraship dependencies
docs/      architecture, module spec, planning
survey/    research data on existing ports
notes/     private notes (gitignored)
```
