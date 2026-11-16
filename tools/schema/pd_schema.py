#!/usr/bin/env python3
"""Generates Perfect Dark's settings_schema.json (docs/planning/MENU.md, design of 2026-11-14: each game's own
settings in one consistent launcher UI) from the port's own config registrations.

    tools/schema/pd_schema.py <perfect_dark checkout> > settings_schema.json

The PC port registers every option with configRegisterInt / UInt / Float / String("Section.Key", &var, min, max)
in port/src/*.c (saved in pd.ini). That gives the keys, types and ranges, but no labels or descriptions, so:
- labels come from the port's "Extended Options" menu text (port/src/optionsmenu.c), mapped by hand in LABELS (the
  menu binds variables in handler functions, not keys, so the mapping can't be read automatically); keys the menu
  doesn't show get a label derived from the key name ("FovAffectsZoom" -> "Fov affects zoom");
- defaults come from the variables' initializers (static s32 vidMSAA = 1;), with DEFAULTS for the struct-backed
  per-player / per-controller ones (PLAYER_EXT_CFG_DEFAULT, CONTROLLERCFG_DEFAULT);
- CHOICES name the values of enum-like ints, from the menu's own option texts.
Per-player and per-controller keys are registered for players 1-4 (split-screen); the schema carries player 1 only:
on a handheld one person plays, and four copies of every row would bury the rest.

The shim (modules/perfect-dark/plm_shim.cpp) applies "port.<Section.Key>" generically through the port's own
configLoad: values go into a small ini file in the port's format and configLoad reads it like pd.ini (clamped to the
registered range, by key, no per-option code).
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lite import read_source  # noqa: E402

FILTERED = {
    "Video.DefaultFullscreen": "fullscreen: Android is always fullscreen",
    "Video.DefaultMaximize": "window state",
    "Video.DefaultWidth": "window size",
    "Video.DefaultHeight": "window size",
    "Video.ExclusiveFullscreen": "fullscreen mode",
    "Video.CenterWindow": "window position",
    "Video.AllowHiDpi": "desktop HiDPI windows",
    "Video.VSync": "desktop VSync",
    "Video.DisplayFPSInterval": "the FPS counter's refresh interval (a dev knob)",
    "Audio.BufferSize": "audio driver buffer in bytes (a dev knob)",
    "Audio.QueueLimit": "audio driver queue in bytes (a dev knob)",
    "Input.MouseEnabled": "mouse: no mouse on the handheld",
    "Input.MouseLockMode": "mouse: no mouse on the handheld",
    "Input.MouseSpeedX": "mouse: no mouse on the handheld",
    "Input.MouseSpeedY": "mouse: no mouse on the handheld",
    "Input.FakeGamepads": "a dev knob",
    "Input.FirstGamepadNum": "controller picker (the launcher assigns controllers)",
    "Input.UseHIDAPI": "desktop controller driver choice",
    "Input.UseRawInput": "desktop (Windows) controller driver choice",
    "Input.Player1.ControllerIndex": "controller picker (the launcher assigns controllers)",
    "Game.MemorySize": "emulated memory size (a dev knob)",
    "Game.ExtraSleep": "desktop CPU-saving sleep (a dev knob)",
    "Game.MenuMouseControl": "mouse: no mouse on the handheld",
    "Game.Player1.MouseAimMode": "mouse: no mouse on the handheld",
    "Game.Player1.MouseAimSpeedX": "mouse: no mouse on the handheld",
    "Game.Player1.MouseAimSpeedY": "mouse: no mouse on the handheld",
    "Game.Player1.UseKeyReloads": "keyboard reload key (range 0-0 upstream: not settable)",
    "Game.Player1.ExtendedControls": "the control style is fixed to modern dual-stick on Android (patch 0002)",
    "Input.Player1.StickCButtons": "the controller layout is ours (patches 0003/0006)",
    "Input.Player1.SwapSticks": "the controller layout is ours (patches 0003/0006)",
    "Input.Player1.CancelCButtons": "the controller layout is ours (patches 0003/0006)",
}

LABELS = {  # the Extended Options menu's own text (optionsmenu.c)
    "Video.MSAA": "Anti-aliasing", "Video.FramerateLimit": "Framerate Limit", "Game.TickRateDivisor": "Uncap Tickrate",
    "Video.DisplayFPS": "Display FPS", "Video.TextureFilter": "Texture Filtering",
    "Video.TextureFilter2D": "GUI Texture Filtering", "Video.AnisotropicFilter": "Anisotropic Filtering",
    "Video.DetailTextures": "Detail Textures", "Game.CenterHUD": "HUD Centering",
    "Game.GEMuzzleFlashes": "GE64-style Muzzle Flashes", "Game.ScreenShakeIntensity": "Explosion Shake",
    "Video.GlareBrightness": "Glare Brightness", "Video.OverexposureScale": "Overexposure Scale",
    "Game.DisableMpDeathMusic": "Disable MP Death Music", "Game.Player1.CrouchMode": "Crouch Mode",
    "Game.Player1.FovY": "Vert FOV", "Game.Player1.CrosshairSway": "Crosshair Sway",
    "Game.Player1.CrosshairEdgeBoundary": "Crosshair Edge Deadzone", "Game.Player1.CrosshairSize": "Crosshair Size",
    "Game.Player1.CrosshairColour": "Crosshair Colour", "Game.Player1.CrosshairHealth": "Crosshair Colour by Health",
    "Game.Player1.RadialMenuSpeed": "Radial Menu Speed", "Input.Player1.RumbleScale": "Vibration",
    "Input.Player1.LStickDeadzoneX": "LStick Deadzone X", "Input.Player1.LStickDeadzoneY": "LStick Deadzone Y",
    "Input.Player1.RStickDeadzoneX": "RStick Deadzone X", "Input.Player1.RStickDeadzoneY": "RStick Deadzone Y",
    "Input.Player1.LStickScaleX": "LStick Scale X", "Input.Player1.LStickScaleY": "LStick Scale Y",
    "Input.Player1.RStickScaleX": "RStick Scale X", "Input.Player1.RStickScaleY": "RStick Scale Y",
    "Game.SkipIntro": "Skip Intro", "Game.MaxExplosions": "Max Explosions",
    "Game.Player1.FovAffectsZoom": "FOV Affects Zoom",
}
DETAILS = {
    "Video.MSAA": "Multisample anti-aliasing. Applies at the next start.",
    "Video.FramebufferEffects": "Framebuffer effects (blur, the camspy and night-vision screens). Applies at the "
                                "next start.",
    "Game.TickRateDivisor": "Uncapped: the game updates every frame instead of at its original rate.",
    "Video.FramerateLimit": "0: no limit.",
    "Game.MaxExplosions": "How many explosions can exist at once (the original game allows 6).",
    "Input.Player1.RumbleScale": "Rumble strength.",
}
CHOICES = {  # the menu's own option texts
    "Video.MSAA": [("1", "Off"), ("2", "2x (MSAA)"), ("4", "4x (MSAA)"), ("8", "8x (MSAA)"), ("16", "16x (MSAA)")],
    "Video.TextureFilter": [("0", "Nearest"), ("1", "Bilinear"), ("2", "Three Point")],
    "Video.MipmapFilter": [("0", "Off"), ("1", "Nearest"), ("2", "Linear")],
    "Video.AnisotropicFilter": [("1", "Off"), ("2", "2x"), ("4", "4x"), ("8", "8x"), ("16", "16x")],
    "Game.CenterHUD": [("0", "None"), ("1", "4:3"), ("2", "Wide")],
    "Game.TickRateDivisor": [("1", "Off"), ("0", "On")],
    "Game.Player1.CrouchMode": [("0", "Hold"), ("1", "Analog"), ("2", "Toggle"), ("3", "Toggle + Analog")],
    "Game.Player1.CrosshairHealth": [("0", "Off"), ("1", "On (Green)"), ("2", "On (White)")],
    "Game.Player1.CrosshairSize": [("0", "0"), ("1", "1"), ("2", "2"), ("3", "3"), ("4", "4")],
    "Game.Player1.CrosshairColour": [("0x00ff0028", "Green (original)"), ("0xff000028", "Red"),
                                     ("0x00a0ff28", "Blue"), ("0xffff0028", "Yellow"), ("0xffffff28", "White")],
    "Video.FramerateLimit": [("0", "No limit"), ("30", "30"), ("60", "60"), ("90", "90"), ("120", "120")],
}
STEPS = {
    "Game.Player1.FovY": 5, "Game.MaxExplosions": 6,
    "Input.Player1.LStickDeadzoneX": 1024, "Input.Player1.LStickDeadzoneY": 1024,
    "Input.Player1.RStickDeadzoneX": 1024, "Input.Player1.RStickDeadzoneY": 1024,
}
RANGES = {  # narrower than the registration where the full range is meaningless in a menu
    "Game.ScreenShakeIntensity": (0, 3, 0.25), "Game.Player1.FovY": (45, 120, 5),
    "Input.Player1.LStickScaleX": (0.25, 3, 0.25), "Input.Player1.LStickScaleY": (0.25, 3, 0.25),
    "Input.Player1.RStickScaleX": (0.25, 3, 0.25), "Input.Player1.RStickScaleY": (0.25, 3, 0.25),
    "Input.Player1.LStickDeadzoneX": (0, 16384, 1024), "Input.Player1.LStickDeadzoneY": (0, 16384, 1024),
    "Input.Player1.RStickDeadzoneX": (0, 16384, 1024), "Input.Player1.RStickDeadzoneY": (0, 16384, 1024),
    "Game.Player1.RadialMenuSpeed": (0.5, 10, 0.5), "Game.Player1.CrosshairSway": (0, 2, 0.25),
    "Game.Player1.CrosshairEdgeBoundary": (0, 1, 0.05), "Input.Player1.RumbleScale": (0, 1, 0.1),
    "Video.GlareBrightness": (0, 1, 0.1), "Video.OverexposureScale": (0, 1, 0.1),
}
DEFAULTS = {  # PLAYER_EXT_CFG_DEFAULT (src/game/mplayer/mplayer.c), CONTROLLERCFG_DEFAULT (port/src/input.c)
    "Game.Player1.FovY": 60, "Game.Player1.FovAffectsZoom": 1, "Game.Player1.RadialMenuSpeed": 4,
    "Game.Player1.CrosshairSway": 1, "Game.Player1.CrosshairEdgeBoundary": 0.7, "Game.Player1.CrouchMode": 3,
    "Game.Player1.CrosshairColour": "0x00ff0028", "Game.Player1.CrosshairSize": 2, "Game.Player1.CrosshairHealth": 0,
    "Input.Player1.RumbleScale": 0.5, "Input.Player1.LStickDeadzoneX": 4096, "Input.Player1.LStickDeadzoneY": 4096,
    "Input.Player1.RStickDeadzoneX": 4096, "Input.Player1.RStickDeadzoneY": 6144,
    "Input.Player1.LStickScaleX": 1, "Input.Player1.LStickScaleY": 1, "Input.Player1.RStickScaleX": 1,
    "Input.Player1.RStickScaleY": 1,
    "Game.MaxExplosions": 6, "Game.CenterHUD": 0,  # MAX_EXPLOSIONS_DEFAULT, HUDCENTER_NONE
    "Video.TextureFilter": 1, "Video.MipmapFilter": 2,  # FILTER_LINEAR, MIPMAP_LINEAR
}
CATEGORY = {"Audio": "audio", "Input": "controls", "Video": "graphics"}
GAME_CATEGORY = {"Game.CenterHUD": "graphics", "Game.ScreenShakeIntensity": "graphics", "Game.TickRateDivisor": "graphics",
                 "Game.GEMuzzleFlashes": "graphics", "Game.Player1.FovY": "graphics",
                 "Game.Player1.FovAffectsZoom": "graphics", "Game.Player1.CrouchMode": "controls",
                 "Game.Player1.RadialMenuSpeed": "controls", "Game.DisableMpDeathMusic": "audio"}

# Extended Options pages in the port's order (optionsmenu.c g_ExtendedMenuItems: Video, Audio, Mouse, Controller, Game,
# Key Bindings; Mouse and Key Bindings have nothing left after the filter).
PAGE = {"Input": "Controller"}
PAGE_ORDER = ["Video", "Audio", "Controller", "Game"]

REG = re.compile(r'configRegister(Int|UInt|Float|String)\s*\(\s*(strFmt\s*\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)|"([^"]+)")'
                 r'\s*,\s*&?([\w\[\]\.]+)\s*,\s*([^,)]+)(?:,\s*([^)]+))?\)')


def label_from_key(key):
    name = key.split(".")[-1]
    words = re.sub(r'(?<=[a-z0-9])(?=[A-Z])', ' ', name).split()
    return " ".join([words[0]] + [w if w.isupper() else w.lower() for w in words[1:]]) if words else key


def num(text):
    text = text.strip().rstrip("fF")
    try:
        return float(int(text, 0)) if re.fullmatch(r'-?(0x[0-9a-fA-F]+|\d+)', text) else float(text)
    except ValueError:
        return {"true": 1.0, "false": 0.0}.get(text)


def fmt(v):
    if isinstance(v, str):
        return v
    return str(int(v)) if float(v).is_integer() else ("%g" % v)


def main(checkout):
    src_dir = os.path.join(checkout, "port", "src")
    texts = {f: read_source(os.path.join(src_dir, f)) for f in sorted(os.listdir(src_dir)) if f.endswith(".c")}
    inits = {}
    for root in (os.path.join(checkout, "port", "src"), os.path.join(checkout, "src", "game")):
        for f in os.listdir(root):
            if f.endswith(".c"):
                for m in re.finditer(r'^(?:static\s+)?(?:s32|u32|f32|int)\s+(\w+)\s*=\s*([-\w.]+)\s*;',
                                     read_source(os.path.join(root, f)), re.M):
                    inits.setdefault(m.group(1), m.group(2))
    options, skipped = [], []
    for fname, text in texts.items():
        for m in REG.finditer(text):
            typ, fmtkey, var, plain = m.group(1), m.group(3), m.group(4), m.group(5)
            if fmtkey:
                key = fmtkey.replace("%d", "1").replace("%s", "Input.Player1")
                if var not in ("i", "secname"):
                    continue
            else:
                key = plain
            if typ == "String":
                continue  # key bindings (the layout is ours) and other free text
            lo, hi = num(m.group(7)), num(m.group(8) or "0")
            if key in FILTERED:
                skipped.append(f"{key} ({FILTERED[key]})")
                continue
            if any(o["key"] == key for o in options):
                continue
            section = key.split(".")[0]
            entry = {"key": key, "label": LABELS.get(key) or label_from_key(key), "detail": DETAILS.get(key, ""),
                     "category": GAME_CATEGORY.get(key) or CATEGORY.get(section, "gameplay")}
            # The Extended Options page it belongs to (owner 2026-11-15: our tabs copy the original's names and
            # order). The config section matches the page, except Input.* (the Controller page).
            entry["tab"] = PAGE.get(section, section)
            d = DEFAULTS.get(key)
            if d is None:
                v = m.group(6)
                d = num(inits.get(v, "")) if v in inits else None
            if key in CHOICES:
                entry["values"] = [{"value": v, "label": l} for v, l in CHOICES[key]]
            elif lo == 0 and hi == 1 and typ != "Float":
                entry["values"] = [{"value": "1", "label": "On"}, {"value": "0", "label": "Off"}]
            elif key in RANGES:
                entry["min"], entry["max"], entry["step"] = RANGES[key]
            elif lo is not None and hi is not None and hi > lo:
                entry.update(min=lo, max=hi, step=STEPS.get(key, 1 if typ != "Float" else (hi - lo) / 20))
            else:
                skipped.append(f"{key} (no usable range)")
                continue
            if d is None:
                print(f"no default for {key}; using the minimum", file=sys.stderr)
                d = entry["values"][0]["value"] if "values" in entry else entry["min"]
            entry["default"] = fmt(d)
            options.append(entry)
    # Our two additions (steady aim, pause on weapon wheel; "Plm." keys the shim catches) are built-in mods since
    # 2026-11-16 (manifest "builtin_mods", the Mods tab), not settings: the settings pages mirror the port's own menu.
    options.sort(key=lambda o: PAGE_ORDER.index(o["tab"]) if o["tab"] in PAGE_ORDER else len(PAGE_ORDER))
    for s in skipped:
        print(f"filtered: {s}", file=sys.stderr)
    json.dump({"format": 1, "module": "perfect-dark", "options": options}, sys.stdout, indent=1, ensure_ascii=False)
    print()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1])
