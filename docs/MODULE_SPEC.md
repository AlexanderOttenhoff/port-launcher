# Port Module Spec: v0 (unstable)

Defines what a native game port must provide to run under the launcher. v0 changes freely
until modules from at least two engine families (libultraship and N64Recomp) pass through it.
Then it freezes as v1 and is offered to upstream port developers.

Prefix `plm_` ("port launcher module") and the extension are placeholders until the project
is named. Key words MUST/SHOULD/MAY as in RFC 2119.

## 1. Concepts

- **Game:** a ROM identity (e.g. *Majora's Mask, US 1.0*), identified by hash.
- **Module:** one native port that can run one or more games (e.g. 2Ship, Zelda64Recomp).
- **Host:** the launcher-provided process environment that loads and runs a module
  (`sdl2` or `sdl3` on Android).
- **Platform key:** `android-arm64` (only supported value in v0). Reserved: `linux-aarch64`,
  `linux-x86_64`, `windows-x86_64`.

## 2. Package layout

A module package is a zip archive:

```
manifest.json                  §3, required
lib/<platform>/libplm_<id>.so  §4, one per supported platform
data/                          read-only data the module needs (shaders, UI documents, fonts…)
options.schema.json            §6, game-specific options (may be empty)
profiles/default.json          §7, curated *modern* defaults, all devices (required)
profiles/<profile-id>.json     §7, device-specific overrides (optional)
icon.png, banner.png           launcher art (optional; art must not be game assets)
LICENSE*, NOTICE*              licences of the module and everything it bundles (required)
```

Packages MUST NOT contain game assets or ROM data. `data/` is only what the upstream project
itself distributes.

## 3. manifest.json

```json
{
  "spec": 0,
  "id": "banjo-recomp",
  "name": "Banjo: Recompiled",
  "version": "1.0.2+plm.1",
  "family": "n64recomp",
  "maturity": "experimental",
  "upstream": { "repo": "https://github.com/BanjoRecomp/BanjoRecomp", "commit": "<sha>" },
  "license": "GPL-3.0-only",
  "host": { "type": "sdl2", "sdl": "2.32" },
  "abi": 0,
  "platforms": ["android-arm64"],
  "graphics": { "api": "vulkan", "min_version": "1.1" },
  "min_android_api": 28,
  "games": [
    {
      "id": "n64.banjo-kazooie.us-1.0",
      "title": "Banjo-Kazooie",
      "region": "US", "revision": "1.0",
      "sha1": ["<sha1 of big-endian .z64>"]
    }
  ],
  "capabilities": {
    "mods": ["nrm"],
    "texture_packs": ["rtz"],
    "achievements": false
  },
  "mod_sources": [{ "type": "thunderstore", "community": "banjo-recompiled" }]
}
```

- `id`: lowercase `[a-z0-9-]+`, stable forever.
- `games[].id`: `<system>.<game-slug>.<region>-<revision>`. Game IDs are shared across modules
  (the launcher's game DB is the union), so two modules for the same ROM use the same game ID.
- `sha1`: of the ROM normalised to big-endian (`.z64`) byte order, uncompressed.
- `host.sdl`: the SDL major.minor the module was built against. It MUST equal the launcher's
  pinned version for that host type.
- `mod_sources` (optional): where the launcher finds mods for this module. Only `thunderstore`
  (a community slug on thunderstore.io) so far; the launcher reads its public package API.
- `maturity` (optional): `"experimental"`, `"integrated"` or `"complete"` (§11); absent or unknown means
  `"experimental"`. The library shows it as a badge under the game's title. Set it from the port's review state
  (docs/planning/PORT_REVIEW.md): a module not yet checked on a device stays experimental.
- `game_lib` (optional, recomp modules only): `{ "build": "on-device", "script": "build_from_rom.sh" }`. The
  module is a **split** recomp build (docs/planning/ON_DEVICE_RECOMP.md): it holds no ROM-derived code, and its
  game code (`libgame.so`) is built on the player's device from their ROM before the first launch. `build` is
  `"on-device"` (the only value so far); `script` is the recomp toolkit's driver script for this module, relative to
  the toolkit root (default `build_from_rom.sh`). The launcher keeps the result in `files/recomp/<module>/libgame.so`
  with a cache key (ROM SHA-1 + module `version` + toolkit version), rebuilds it when the key changes, and passes it
  as launch.json `game_lib`. The script is called as `sh <script> <rom.z64> <jobs>` with `WORK_DIR`/`OUT_DIR` set, must
  leave `$OUT_DIR/libgame.so`, and reports progress with `PLM_PROGRESS` lines (`phase <name>`, `total <n>`,
  `file <name>`; see `tools/toolkit/build_from_rom.sh`). Leave the key out of unsplit builds:
  `modules/dk64-recomp/build.sh` strips it unless `PLM_SPLIT_GAME=1`. (Not to be confused with `games[].game_code`, the
  ROM header's game code.)

## 4. Native ABI

The library MUST export these C symbols. Apart from them it exports only what its statically
linked SDL needs on Android (`JNI_OnLoad` and SDL's `Java_app_portlauncher_sdl*` methods) and
SHOULD hide everything else:

```c
// ABI revision the module implements. v0 modules return 0.
uint32_t plm_abi_version(void);

// Entry point. argv[1] is the absolute path to launch.json (§4.1).
// Returns 0 on normal exit, non-zero on error (written to the log first).
int plm_main(int argc, char** argv);

// Thread-safe request to save what's needed and return from plm_main soon.
// May be called from any thread; MUST NOT block.
void plm_request_quit(void);

// Freeze / unfreeze gameplay, audio and game input while the host's in-game menu is open (§4.2).
// Called from the host's UI thread; MUST NOT block. Rendering the last frame (or nothing) is fine.
void plm_pause(void);
void plm_resume(void);

// Applies changed in-game settings while running (§6 "phase"). json is a flat object of
// key -> value, like launch.json's "settings". Called while paused. Returns 0 if all keys applied.
int plm_apply_settings(const char* json);
```

Requirements:
- **No other process-global assumptions.** Don't read argv beyond `argv[1]`, don't depend on the
  working directory, don't write outside the directories in `launch.json`.
- **Static dependencies:** SDL, libc++ (`c++_static`) and all other dependencies are linked
  statically into the one `.so` (no extra shared libraries, e.g. build with
  `BUILD_SHARED_LIBS=OFF`). SDL MUST be the host's pinned build with its Java package moved to
  the host's (§8).
- **One run per process:** the host never calls `plm_main` twice in one process. Modules don't
  need to clean up global state for re-entry.
- **Lifecycle:** the module MUST handle SDL's application lifecycle events (will/did enter
  background/foreground, low memory). It MUST stop rendering and audio while in the background
  and survive losing and regaining the window surface.
- **Quit:** on `plm_request_quit` or SDL_QUIT, the module flushes saves and settings, then returns.
- **Direct boot:** with `"ui": "direct"` (the default) the module MUST go straight into the game:
  no launcher, ROM picker, language prompt or first-run screens of its own. Anything those screens
  configure comes from launch.json (§9). With `"ui": "native"` the module MAY show its own
  launcher; the host offers that only as an advanced fallback (§4.2).
- **No native menu in direct mode:** the module MUST NOT open its own menus or overlays from
  game input; the host owns the menu button (§4.2). Debug overlays may stay behind explicit
  developer settings.

### 4.1 launch.json

```json
{
  "abi": 0,
  "platform": "android-arm64",
  "module": "banjo-recomp",
  "game": "n64.banjo-kazooie.us-1.0",
  "rom": "/data/user/0/<app>/files/games/n64.banjo-kazooie.us-1.0/rom.z64",
  "dirs": {
    "data":     "<read-only: unpacked data/ from the package>",
    "config":   "<writable>",
    "save":     "<writable>",
    "cache":    "<writable, may be cleared by the launcher>",
    "mods":     "<writable>",
    "textures": "<readable, may be on external storage>",
    "logs":     "<writable>"
  },
  "locale": "en-GB",
  "device_profile": "retroid-pocket-6",
  "ui": "direct",
  "mods": {
    "installed": ["bk_nostalgia_64", "bk_recomp_asset_expansion_pak"],
    "enabled":   ["bk_nostalgia_64", "bk_recomp_asset_expansion_pak"],
    "game_mode": "bk_nostalgia_64"
  }
}
```

Unknown fields MUST be ignored, so later minor additions need no ABI bump.

- `mods` (optional; only for modules with `capabilities.mods`): the launcher installs mods and texture
  packs into `dirs.mods` and is the only writer of which ones are on. `installed` lists every mod ID
  present, `enabled` the ones that should load (IDs are the port's own, e.g. a recomp mod's
  `mod.json` `id`). `game_mode` is empty for the base game, or the ID of an enabled total-conversion
  mod (recomp `custom_gamemode`) to boot straight into; the launcher lists those as their own library
  entries. The module turns this into the port's own mod configuration before the game starts (the
  N64Recomp shim points the runtime's mods folder at `dirs.mods` and writes `mods.json`), and
  overwrites whatever the port's own menus changed there.

### 4.2 Menus: one standard in-game menu, owned by the host

- **Back opens the menu.** The host intercepts the platform's Back button (Android `KEYCODE_BACK`;
  a device profile may map another button, e.g. Select on stick-less handhelds) before the game
  sees it, calls `plm_pause()` and draws the launcher's own in-game menu on top of the game.
  The game never receives Back.
- **Menu contents:** Resume, the module's **in-game** options (§6) and the canonical in-game keys,
  controls, and "Save and quit to library" (`plm_request_quit`). Later: achievements,
  suspend/resume.
- **Closing:** changed in-game options go to `plm_apply_settings()`, then `plm_resume()`. Every change is
  also stored in the launcher's per-game layer, so the launcher stays the only owner of settings.
- **Pre-game options are never offered in-game.** They live on the game's page in the launcher.
- **Lightweight devices: pre-game-only mode.** A device profile may set `"menu": "pre_game_only"`
  (default `"full"`). Then every option, in-game ones included, is set on the game's page and
  applied through launch.json at launch; the in-game menu is reduced to Resume and
  "Save and quit" (or omitted), so the host needs no full overlay renderer. Modules need nothing
  extra: in-game options already work at startup.
- **Native fallback (temporary):** while a module is new, the game page MAY offer "Open the port's
  own menu", which launches with `"ui": "native"`. It is advanced, hidden by default and a gap
  to close, not a feature: the goal for every module is **100% of the port's settings** in the
  launcher's schema with the same UX quality, at which point the fallback is removed (§11).

## 5. Canonical settings

> **Superseded (2026-11-15).** There are no canonical or global settings any more: each module ships
> `data/settings_schema.json` with its port's own options (docs/planning/MENU.md), and the manifest fields
> `canonical_settings`, `canonical_live`, `canonical_map` and `native_options` are gone. Controller mapping (§5.1)
> gets a new design (docs/planning/INPUT_INTENTS.md). The section below is kept as the original v0 idea.

Flat keys that every module SHOULD honour where it makes sense, so the launcher's global pages
can work across modules. v0 set:

| Key | Type | Values / range | Meaning |
|---|---|---|---|
| `video.scale` | enum | `auto`, `1x`, `2x`, `3x`, `4x` | internal resolution relative to native 240p |
| `video.aspect` | enum | `original`, `expand`, `manual` | 4:3, widen to screen, manual |
| `video.fps_cap` | enum | `original`, `30`, `60`, `display` | frame-rate target / interpolation |
| `video.msaa` | enum | `off`, `2x`, `4x` | anti-aliasing |
| `video.hud_ratio` | enum | `original`, `expand` | HUD placement in widescreen |
| `audio.volume` | int | 0–100 | master volume |
| `audio.latency` | enum | `low`, `normal`, `high` | buffer size hint |
| `input.n64.map` | object | §5.1 | controller mapping |
| `input.deadzone` | int | 0–50 | stick deadzone, percent |
| `ui.menu_button` | enum | `back`, `select`, `guide` | button that opens the host's in-game menu (§4.2); `back` by default, device profiles may override |

Modules map these to their nearest native option and MUST ignore values they can't express,
rather than fail. Canonical video, audio and input keys are in-game options for every module
that lists them in `canonical_live` in its manifest (applied live by `plm_apply_settings()`);
canonical keys it supports but can't apply live are pre-game for that module.

There is no `original` preset (removed 2026-11-11): values named `original` above are ordinary
choices a player may pick where the key is exposed, not a mode.

### 5.1 `input.n64.map`

N64 controls → SDL GameController buttons/axes. Example (RP6 default):

```json
{ "a": "a", "b": "x", "z": "lefttrigger", "start": "start",
  "l": "leftshoulder", "r": "rightshoulder",
  "c_up": "-righty", "c_down": "+righty", "c_left": "-rightx", "c_right": "+rightx",
  "dpad": "dpad", "stick": "leftstick" }
```

`-righty` means the up direction of the right stick, following SDL's axis sign convention.

## 6. options.schema.json

Game-specific options shown on the launcher's per-game page. Keys get the module ID as a
namespace in settings files (`banjo-recomp.note_saving`).

```json
{
  "spec": 0,
  "categories": [ { "id": "gameplay", "label": "Gameplay" } ],
  "options": [
    { "key": "note_saving", "type": "enum", "category": "gameplay",
      "label": "Note & Jinjo Saving",
      "description": "Keeps collected notes and jinjos when revisiting a level.",
      "values": [ { "value": "off", "label": "Off" }, { "value": "notes", "label": "Only Notes" },
                  { "value": "jinjos", "label": "Only Jinjos" }, { "value": "both", "label": "Both" } ],
      "default": "both", "original": "off", "phase": "in_game" },
    { "key": "analog_cam_sensitivity", "type": "int", "category": "gameplay",
      "label": "Analog Camera Sensitivity", "min": 1, "max": 10, "step": 1, "default": 3, "phase": "in_game",
      "visible_if": { "key": "analog_cam", "not": "off" } }
  ]
}
```

`default` MUST be the **modern** value: the launcher's product rule is that every
modernisation is on out of the box. Only options with a valid reason to change them are listed
(preference, accessibility, device); fixed parts of the modern experience aren't options at all.
`original` (optional, informational) is the value that matches the N64 game; there is no
`original` preset.

Every option has a **phase**. **In-game is the norm; pre-game is the exception and must be
justified.** Anything that can logically be changed during play belongs in-game, even if the
shim has to do extra work (or the port needs a small change) to apply it live.
- `"phase": "in_game"`: set on the game's page *and* changeable from the in-game menu; the module
  MUST apply it through `plm_apply_settings()` without restarting. This is the target for
  graphics, input, audio and switching gameplay hacks/QoL tweaks on and off (camera, invert look,
  HUD, frame-rate, note saving, cutscene skip…), wherever the port can apply them live.
- `"phase": "pre_game"`: only on the game's page before starting; never shown in-game. Only for
  what defines the run (randomizer seeds and logic, new-game rules, game modes that can't change
  mid-save) or what genuinely can't be applied live. A pre-game option MUST carry a
  `"pre_game_reason"` (one sentence), so reviews can challenge it; "the port only reads it at
  startup" is a temporary reason and a candidate for fixing.

There is no default: every option declares its phase, so nothing lands in the wrong menu by
accident.

Types: `bool`, `enum`, `int`, `float`, `string`. `visible_if` supports `equals` / `not` against
one other key.
Descriptions are plain text in v0, with no markup.

Schemas SHOULD be generated from the upstream project's existing option declarations
(RecompFrontend `add_*_option`, libultraship `AddWidget(...).CVar(...)`), not hand-written.

## 7. Curated defaults and device profiles

`profiles/default.json` (required) holds the module's curated **modern** settings for all
devices: every modernisation on, chosen so the game feels right without anyone opening a
settings page. Example for JFG: camera tweaks on, look not inverted (key names are
illustrative until the JFG schema is generated).

```json
{ "spec": 0, "profile": "default",
  "settings": { "video.aspect": "expand", "video.fps_cap": "60",
                "jfg-recomp.camera_mode": "modern", "jfg-recomp.invert_look": false } }
```

`profiles/<profile-id>.json` (optional) holds **game overrides** for one device class. They
take precedence over the user's global settings (with a first-launch notice, ARCHITECTURE
"Settings model"), so they are kept to a minimum:

- Only for performance or stability problems of the port on that device, never for taste
  (taste belongs in `default.json`).
- Every override carries a `reason` shown to the user, and SHOULD say when to revisit it.
- Temporary by intent: removed as soon as the port runs well without it. A module with no
  device overrides is the normal case.

```json
{ "spec": 0, "profile": "retroid-pocket-6",
  "overrides": {
    "video.scale": { "value": "1x", "reason": "Drops below 30 fps at 2x in Spiral Mountain.",
                     "revisit": "when the port's RT64 update lands" }
  } }
```

The launcher's device DB maps hardware to a profile ID by capabilities (GPU API and family,
RAM, screen, model). Device-wide defaults (controls, a sensible resolution) live in the core's own
device profiles, not in module packages.

## 8. Hosts (Android)

| Host | SDL pin | Java glue package | Native SDL for modules |
|---|---|---|---|
| `sdl2` | 2.32.10 (pinned by the M2 spike) | `app.portlauncher.sdl2` | `tools/deps/build_sdl2.sh`: static + PIC, package rewritten |
| `sdl3` | 3.2.x (to pin with the first libultraship module) | `app.portlauncher.sdl3` | to do |

SDL2 has **no build option for its Java package**: it is hard-coded in a JNI-name macro
(`SDL_JAVA_PREFIX` in `SDL_android.c` and `hid.cpp`) and in the class paths `JNI_OnLoad`
registers natives against (`"org/libsdl/app/SDLActivity"`…). The host's build script rewrites
both; modules MUST link that build rather than their own SDL. (SDL3 still needs checking.)

Exact pins are fixed once the first module of each type builds. Changing a pin is a launcher
release that requires rebuilding every module of that host type.

## 9. Settings exchange

Settings travel in launch.json (§4.1); there are no separate settings files (the earlier `settings_in.json` /
`settings_out.json` were never implemented and were removed from launch.json on 2026-11-14,
SLOP_AUDIT.md §1.2, a private planning document).

- At launch: `settings` (the port's own keys as `port.<key>`, from the module's settings schema) and, for the in-game menu, `menu_options` plus `settings_file` (the module's settings layer, which the host's
  menu writes). The module translates them into its native config before starting the game.
- While running: changed in-game keys reach the module through `plm_apply_settings` (§3).
- Native settings with no schema equivalent stay in the module's own config directory and are not
  reported back.

## 10. Versioning

- `spec` (manifest, schema, profiles) and `abi` (native) are separate integers.
- The launcher refuses modules with a newer `spec`/`abi` than it supports, or an SDL pin that
  doesn't match.
- Additive fields need no bump; consumers ignore unknown fields.

## 11. Module maturity

One scale for every module, declared in the manifest's `maturity` field and shown as a library badge.

| Level | Requirements |
|---|---|
| **Experimental** (default) | Boots, with known issues, or not yet checked on a device. |
| **Integrated** | Direct boot, the host menu works (pause/resume/quit), canonical settings honoured, curated defaults and RP6 profile. May still rely on the native fallback. |
| **Complete** | Every setting the port offers is in `options.schema.json` with labels, descriptions and the right phase; pre-game options all have a justified `pre_game_reason`; mods and texture packs are managed by the launcher; the native fallback is removed. |

New modules may ship as Integrated. Complete is the target for every **official-repository** module
(decomp ports), and the launcher tracks which modules still have open gaps. **Secondary-repository modules**
(recomps and other third-party projects, ROADMAP "Two tiers") are only required to reach Integrated, and may reach
Complete on the same criteria (for recomps, save states depend on SAVE_STATES.md route 3). Until then they may keep the native
fallback permanently and honour only the canonical settings that map cheaply; the menu should then offer
"Open the game's own settings" as a regular entry, not a hidden fallback.

## Open items

- **Device profile provenance and suggestions.** Profiles (module overrides and core device
  defaults) should record who tested them, on which device, when and how (manual or automated
  benchmark), and may carry *suggestions* for users who set a value explicitly (key, suggested
  value, reason, since which version). Format to define when the second device profile arrives.

- **Host services.** Ported games call into their own Android activity today: JFG looks up static
  Java methods on whatever activity SDL runs (`openFileDialog`, `httpGetText`, `httpDownload`,
  `httpRequest`) and gets results through JNI callbacks named after *its* activity class. Under a
  generic host those break. v0 needs a portable host-services interface (file picker, HTTP,
  open URL, keyboard) offered as C function pointers rather than JNI names, so modules don't
  depend on Android classes and Linux hosts can implement the same table. M2 avoids the
  picker by passing the ROM (`--rom`), which JFG already supports.

- Settings precedence when global user settings conflict with curated per-game device
  defaults (see ARCHITECTURE).
- How a module reports "unsupported on this device" at runtime (e.g. missing Vulkan
  features), beyond the manifest's `graphics` field.
- Device capability vocabulary: align manifest requirements and profile matching with
  PortMaster/RHH `port.json` tags (`hires`, `power`, `vulkan`, `wide`, `analog_#`) so Phase 4
  packaging maps cleanly.
- Mods and texture-pack metadata (versions, compatibility ranges), deferred to Phase 3.
- Achievements and RetroAchievements hooks (JFG already has a softcore implementation).
