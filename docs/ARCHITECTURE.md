# Architecture (v0 draft)

Status: draft for Phase 1 (RP6 proof of concept). Background: the private planning documents.
The module contract lives in [MODULE_SPEC.md](MODULE_SPEC.md).

## Goals and constraints

- One Android app that finds the user's ROMs and runs native decomp/recomp ports as **modules**,
  with one settings model: global settings, per-game overrides and curated device defaults.
- **Build target: Android arm64 only.** Structure stays portable so that Linux handhelds
  (PortMaster), desktop Linux and Windows remain reasonable later. See ROADMAP.
- v1 **bundles** modules in the APK. No server, but modules use the same package format a
  server would deliver later.
- The patch footprint on upstream port projects stays small: one shim per engine family.

## Components

```
┌──────────────────────── APK ─────────────────────────────────────────────┐
│                                                                          │
│  Launcher process (main)                 Game process (":game")          │
│  ┌──────────────────────────┐            ┌───────────────────────────┐   │
│  │ Android shell (Kotlin)   │  Intent    │ Host activity (Java)      │   │
│  │  UI, SAF folder access,  │ ─────────▶ │  SDL2 or SDL3 glue        │   │
│  │  ROM import, launching   │ launch.json│  loads lib<module>.so     │   │
│  └───────────┬──────────────┘            │  calls plm_main()         │   │
│              │ JNI                       └────────────┬──────────────┘   │
│  ┌───────────▼──────────────┐                         │                  │
│  │ core (C++, portable)     │            ┌────────────▼──────────────┐   │
│  │  library / ROM identify  │            │ module .so                │   │
│  │  module registry         │            │  upstream port + patches  │   │
│  │  settings resolver       │            │  family shim (plm_*)      │   │
│  │  device profiles         │            │  static SDL, libc++, deps │   │
│  │  launch.json writer      │            └───────────────────────────┘   │
│  └──────────────────────────┘                                            │
│                                                                          │
│  assets/modules/<id>/…  (manifest, data, schema, profiles)               │
│  lib/arm64-v8a/libplm_<id>.so                                            │
└──────────────────────────────────────────────────────────────────────────┘
```

### core (C++20, CMake, no Android types)
Holds everything that would also run on Linux/Windows:
- **Library:** folder roots, scanning, archive reading (zip/7z), byte-order detection and
  normalisation (z64/v64/n64 → big-endian), SHA-1, matching against module manifests.
- **Module registry:** a `ModuleRepository` interface. v1 implementation: `BundledRepository`
  (reads `assets/modules/*`). Later: `InstalledRepository` (internal storage) fed by a remote
  catalog.
- **Settings resolver:** layered resolution (below), canonical keys plus per-module schemas,
  passing resolved settings to the module in launch.json.
- **Device profiles:** a capability-keyed device DB (GPU API/family, RAM, screen, model) that
  picks curated profile files.
- **Launch:** writes `launch.json` (see the spec) and returns it to the shell.
- Platform services come in through interfaces the shell implements: `FileSource`
  (paths or SAF documents), `Log`, `Paths` (app data/cache/external dirs).

### Android shell (Kotlin)
- UI: Jetpack Compose, gamepad/D-pad focus first, touch works but isn't required. Scales
  down to small screens (keeps the Linux-handheld future honest).
- Storage Access Framework (SAF) tree permissions for ROM folders. On first launch of a game, the
  ROM is imported (normalised copy) into app storage, so native code never sees SAF.
- Starts the game process and shows results (exit code, crash log) when it returns.

### Game process
- A single `android:process=":game"` holds at most one running game; the launcher kills it
  after exit. This gives a clean reset of global state and lets SDL2 and SDL3 modules
  coexist in one APK.
- Two host activities ship in the APK: `Sdl2HostActivity` and `Sdl3HostActivity`. Each
  bundles SDL's Java glue under a renamed package (`app.portlauncher.sdl2`/`.sdl3`) that
  matches the SDL build modules link (its Java package rewritten in source; see MODULE_SPEC §8). The **SDL version is pinned per host**,
  because SDL's Java and native sides must match. Modules must build against the pinned
  version (see spec).
- The host overrides SDL's library and entry-point lookup so it loads `libplm_<id>.so` and calls
  `plm_main(argc, argv)` with `argv[1]` = path to `launch.json`.

### Modules
- Built outside the APK build: per-module recipe = upstream pin + Android patch stack +
  family shim + manifest/profiles. The output is a **module package** (spec §2). The APK build
  unpacks packages into `lib/` and `assets/`.
- Family shims (written once, reused):
  - `shim-recomp`: N64ModernRuntime + RecompFrontend (B1). Small per-game adapters for
    Zelda64Recomp-style UIs (B2).
  - `shim-lus`: libultraship (family A).
  Each shim implements `plm_main`, translates launch.json's settings (MODULE_SPEC §9) into native config, and maps
  lifecycle and quit requests. Live changes from the in-game menu arrive through `plm_apply_settings`.

## Data layout (app-internal unless noted)

```
files/
  library.json                  folder roots + last scan (uri, size, mtime, ROM hashes); JSON for now
  settings/global.json          user global layer
  settings/games/<game>.json    user per-game layer (+ preset: modern | original)
  games/<game>/rom.z64          imported normalised ROM
  modules/<module>/             per-module writable roots
    config/  save/  mods/
  sessions/<ts>/launch.json     one per launch; only the newest 3 are kept (Sessions.kt)
cache/
  modules/<module>/             shader caches, extracted assets (.o2r)
external (SD card, optional, user-chosen):
  texture packs (later)
```

*Game* (a ROM identity, e.g. "Majora's Mask US") and *module* (a port, e.g. 2Ship or
Zelda64Recomp) are distinct. Saves live per module because formats differ.

## Port selection (one entry per game)

The core ships a curated **game DB**: for each game, an ordered list of candidate modules,
each with its capability requirements (e.g. `vulkan`). The launcher shows the game once and
uses the **first candidate the device can run**. Users don't pick ports. A second candidate
exists only for a concrete reason (e.g. a GLES port for devices without Vulkan), and the
ordering is ours to curate and justify in the DB. A ROM whose candidates can't run on the
device is listed as "not supported on this device" rather than hidden, so the user knows
the folder scan worked.

## Settings model

Principle: **modern by default, zero setup.** Out of the box every game runs with all
modernisations on, including game-specific options, tuned for the device.

There is one experience, the modern one: the module's curated modern defaults
(`profiles/default.json`), overlaid with the device profile (`profiles/<device>.json`). No
`original` preset (the original experience is what emulators are for), and only
settings with a valid reason to change them (preference, accessibility, device) are exposed;
fixed parts of the modern experience have no setting.

Every value the user sees is either **Default** or **Set by you**, and anything set by you has a
**Reset** button that returns it to default (per game on the game page; the global pages and the global layer were
dropped on 2026-11-15, see MENU.md "Settings model"). "Set by you" records the choice, not the value: explicitly picking a value that equals
the current default still counts as set by you (it stays put if defaults change later, and game
overrides prompt for it). Reset is the only way back to Default.

Resolution order per key. The first layer that defines the key wins:

1. **Set by you, for this game** (user per-game).
2. **Game override for this device** (curated, from the module's `profiles/<device>.json`).
   Rare and temporary: only for ports with performance or stability problems on that
   device, each with a stated reason (MODULE_SPEC §7). It beats your *global* settings, but
   never silently (see below).
3. **Set by you, globally** (user global).
4. **Default**, itself resolved as: the module's defaults (`profiles/default.json`) → the
   device's global profile (shipped in the core; e.g. RP6 controls and resolution) → the schema or canonical
   default.

Controls are part of curation. The device profile carries the N64 mapping, so no mapping
step is ever needed.

**Override warning.** When a game override would replace something you set globally, the
first launch of that game shows a short notice listing the affected settings, your value and
the recommended one, and why (e.g. "Resolution: 1× recommended on this device (you set 2×):
the port drops below 30 fps at 2× in some levels"). Choices: **Use recommended** or
**Keep mine**. The choice is remembered per game and setting, shown and changeable on the
game's page, and only asked again if the override itself changes. "Keep mine" is stored as a
decision, not as a copy of your value, so later changes to your global setting still apply
to that game. Exact UX is open (M3).

Worked example: the RP6 default resolution is 2×. A user who knows their device can take more
sets it to 4× globally (Set by you; explicitly choosing 2× would count the same way). An in-development port ships an RP6 game override of 1× for
performance. On that game's first launch the user is asked: *Use recommended (1×)* or *Keep
mine (4×)*. A user who never changed the default gets 1× for that game without a prompt (nothing
of theirs is overridden), and the game page shows "1× recommended for this device" with the
reason. Overrides belong to a device profile, so a device with its own profile isn't affected
by the RP6's overrides at all.

Because explicit values don't follow default changes, later updates may **suggest** changes to
them per device (on launch and/or as a marker in settings; accept = new explicit value, dismiss
is remembered). Planned, UX open; see the roadmap backlog.

Keys are either **canonical** (`video.*`, `audio.*`, `input.*`, shared by all modules; spec §5)
or **module options** (`<module>.<key>`, from the module's options schema). On the game page, a
value coming from a game override is labelled as such ("Recommended for this device") with its
reason.

## First run

1. Install → launcher opens to a single "Add your N64 ROM folder" action (SAF picker).
2. Scan → game list with box art and names. Unknown files are ignored silently;
   known-but-unsupported games are shown greyed with a reason.
3. Tap a game → first launch does any one-time work automatically with a progress screen
   (ROM import, libultraship asset extraction, shader warm-up where possible) → gameplay.
   (Later, with downloadable modules: a "Download modules" step, ideally one button for
   everything the scanned library needs.)

There are no setup wizards. Settings, presets and per-game options are reachable from the
game's page but never required.

## Launch sequence

1. User picks a game → core picks the module from the game DB (first candidate the device can run).
2. Core imports the ROM if needed, resolves settings, writes `launch.json` (settings included).
3. Shell starts the module's host activity in `:game` with the `launch.json` path.
4. Host loads `libplm_<id>.so`, checks `plm_abi_version()`, calls `plm_main`.
5. Module boots **straight into the game** (`"ui": "direct"`): no port launcher, ROM picker or
   first-run screens. A loading screen stays up until the first frame (RT64 shader warm-up can take
   ~45 s on first launch).
6. Back opens the launcher's own in-game menu (below); the game never sees Back.
7. "Save and quit" (or the game's own exit) → module returns → process exits → launcher shows
   any errors or crash logs. Settings are already stored (the in-game menu writes them as they change).

## In-game menu

One menu for every game, drawn by the host on top of the game's surface (a Compose overlay
above SDL's `SurfaceView` on Android), using the same schema-driven pages as the launcher.

- Back → `plm_pause()` → menu gets controller focus → on close `plm_apply_settings()` with
  the changed in-game keys → `plm_resume()`.
- Shows only **in-game** options: graphics, input, audio, gameplay hacks and QoL toggles the port
  can apply live. **Pre-game** options (randomizers, difficulty and game modes, startup-only
  settings) appear only on the game's page in the launcher.
- Ports' own menus are disabled in direct mode. Features we haven't rebuilt stay reachable
  through an advanced "Open the port's own menu" fallback (`"ui": "native"`), retired per
  family once covered.
- Portability: the contract (pause, resume, apply, menu button) is platform-neutral; the drawing
  is per platform (Compose on Android; Linux handhelds will need their own overlay renderer).

## Portability rules (enforced in review)

- No Android/JNI types or headers in `core/`, the shims or the module ABI.
- Everything platform-specific goes through the core's service interfaces or the host.
- Package format and module ABI carry a platform key (`android-arm64` now;
  `linux-aarch64`, `linux-x86_64`, `windows-x86_64` reserved).
- UI layouts must work down to 640×480.

## Repository layout (planned)

```
docs/            architecture, spec, planning
spec/            JSON Schemas for manifest, options schema, launch.json, profiles
core/            portable C++ library + tests (host-runnable on Linux for unit tests only)
android/         Gradle project: shell app + host activities
shims/           shim-recomp, shim-lus
modules/<id>/    recipe: upstream pin, patches, manifest, profiles, build script
tools/           package builder, schema extraction, APK assembly
```

Running `core/` unit tests on the Linux dev machine is not a second build target. It's just
fast tests for platform-neutral code. Only Android is shipped and supported.
