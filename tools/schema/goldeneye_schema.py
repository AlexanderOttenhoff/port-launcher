#!/usr/bin/env python3
"""Generates GoldenEye 007's settings_schema.json (docs/planning/MENU.md, design of 2026-11-14: each game's own
settings in one consistent launcher UI) from the PC port's own options table.

    tools/schema/goldeneye_schema.py <goldeneye-pc-port checkout> > settings_schema.json

The port registers options with configRegisterInt/UInt/Float/String("Section.Key", &var, min, max) (ge007.ini) and
describes the ones its options menu shows (F10 overlay and the front-end PC Options, port/src/optionsoverlay.c) in a
data table, rows[]: key, label, kind (toggle / slider / enum / MSAA / frame-rate cap...), step, value names, UI range,
display unit and divisor, restart flag, grouped under section headers (VIDEO, AUDIO, MOUSE, CONTROLLER, GAME, KEY
BINDINGS). That table is the port's own UI, so it is the source here: every config-backed row becomes an option with
the row's label and value names; registration ranges and variable initializers fill in the rest. Ini-only keys the
menu doesn't show (migration flags, legacy aliases, dev knobs) stay out, like in the port's menu.

The shim (modules/goldeneye/plm_shim.cpp) applies "port.<Section.Key>" generically with the port's public
configSetValue(key, double) - right after configLoad at launch (wrapped) and on the frame thread live.

Filtered (FILTERED, plus whole kinds): window/fullscreen/VSync, the mouse section (no mouse on the handheld), the
output-device picker, key and pad binding capture (the controller layout is ours), per-save-file "Bond.*" rows (they
live in the game's EEPROM save, set in the game's own watch menu, not in the config), Input.AimMode (our own
ge.steady_aim overrides it: patch 0004), the update check (the module server updates modules).
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lite import balanced, read_source, split_args, string_value  # noqa: E402

FILTERED = {
    "Video.Fullscreen": "fullscreen: Android is always fullscreen",
    "Video.FullscreenMode": "fullscreen mode",
    "Video.VSync": "desktop VSync",
    "Input.PadPreset": "the controller layout is ours (patch 0002)",
    "Game.CheckUpdates": "the port's own update check: the module server updates modules",
}
KEEP_FROM_MOUSE = {"Input.AimRange", "Input.AimMode"}  # device-neutral (D404) although they sit on the Mouse page
# Our handheld defaults, always sent ("force"). Input.AimMode replaced our hand-made "Steady aim" entry (2026-11-15):
# Centred (1) is the modern default; the shim also stills the standing sway with it (patch 0005).
FORCED = {"Input.AimMode": 1}
SKIP_KINDS = {"ROW_ACTION", "ROW_HEADER", "ROW_RES", "ROW_AUDIODEV", "ROW_PADBIND", "ROW_BIND", "ROW_BOND_FILE",
              "ROW_PADSEAT"}
SECTION_CATEGORY = {"VIDEO": "graphics", "AUDIO": "audio", "MOUSE": None, "CONTROLLER": "controls", "GAME": "gameplay",
                    "KEY BINDINGS": None}
CATEGORY = {"Input.AimRange": "controls", "Input.AimMode": "controls", "Input.CrouchMode": "controls", "Video.FovScale": "graphics",
            "Game.HudScale": "graphics", "Game.ScreenShakeIntensity": "graphics"}
# Defaults the initializers don't show: the per-stick deadzones start "unset" and migrate from the shared one
# (input.c: STICK_DEADZONE 7500 = 25 %).
DEFAULTS = {"Input.PadDeadzoneL": 7500, "Input.PadDeadzoneR": 7500}
DETAILS = {
    "Video.MSAA": "Multisample anti-aliasing. Applies at the next start.",
    "Video.FovScale": "Vertical field of view; 60° is the original game.",
    "Video.DrawDistance": "A multiple of the original draw distance.",
    "Video.FogDistance": "A multiple of the level's own fog distance (capped at the draw distance).",
    "Video.LodDistance": "A multiple of the original model detail distance.",
    "Game.AIWideView": "Which part of the screen enemies' line-of-sight tests use: the original N64 area or "
                       "everything drawn (widescreen, raised FOV).",
    "Input.AimRange": "How far the N64-style crosshair travels.",
}


def num(text):
    text = (text or "").strip().rstrip("fF")
    if not text:
        return None
    try:
        return float(int(text, 0)) if re.fullmatch(r'-?(0x[0-9a-fA-F]+|\d+)', text) else float(text)
    except ValueError:
        m = re.fullmatch(r'([\d.]+)\s*/\s*([\d.]+)', text)
        return float(m.group(1)) / float(m.group(2)) if m else None


def fmt(v):
    v = round(v, 4)
    return str(int(v)) if float(v).is_integer() else ("%g" % v)


def frange(lo, hi, step):
    out, i = [], 0
    while lo + i * step <= hi + 1e-6:
        out.append(lo + i * step)
        i += 1
    return out


# No profiles (owner 2026-11-16): GoldenEye's modernisation comes from our bundled fixes and mods (steady aim, quick
# select, instant pause, complexity/difficulty, tank controls), and the port's only preset ("Original N64",
# port/src/video.c kVideoPresets) is graphics, which stay at the port's defaults until device benchmarking (ROADMAP).
# A profile was generated from that preset for a few hours on 2026-11-16 and taken out again; see git history.


def main(checkout):
    src = os.path.join(checkout, "port", "src")
    texts = {f: read_source(os.path.join(src, f)) for f in sorted(os.listdir(src)) if f.endswith(".c")}
    overlay = texts["optionsoverlay.c"]

    names = {}
    for m in re.finditer(r'static const char \*const (k\w+)\[\]\s*=\s*\{', overlay):
        body = overlay[m.end():balanced(overlay, m.end() - 1, '{', '}') - 1]
        names[m.group(1)] = [string_value(a) for a in split_args(body) if a != "NULL" and string_value(a) is not None]
    fps_seq = [int(x) for x in re.search(r'kFpsCapSeq\[\]\s*=\s*\{([^}]*)\}', overlay).group(1).split(",")]
    msaa_seq = [int(x) for x in re.search(r'kMsaaSeq\[\]\s*=\s*\{([^}]*)\}', overlay).group(1).split(",")]

    regs, inits = {}, {}
    reg = re.compile(r'configRegister(Int|UInt|Float|String)\s*\(\s*"([^"]+)"\s*,\s*&?([\w\[\]]+)\s*,\s*([^,)]+)(?:,\s*([^)]+))?\)')
    for text in texts.values():
        for m in reg.finditer(text):
            regs[m.group(2)] = (m.group(1), m.group(3), num(m.group(4)), num(m.group(5)))
        for d in re.finditer(r'^\s*(?:static\s+)?(?:volatile\s+)?(?:int|float|s32|f32|u32|unsigned(?:\s+int)?)\s+([^;(){}]*);', text, re.M):
            for part in d.group(1).split(","):
                mm = re.match(r'\s*(\w+)\s*=\s*(-?[\d.]+)[fF]?\s*$', part)
                if mm:
                    inits.setdefault(mm.group(1), float(mm.group(2)))

    start = overlay.index("static struct Row rows[] = {")
    body = overlay[start:balanced(overlay, overlay.index("{", start + 20), '{', '}')]
    options, skipped, section = [], [], None
    for m in re.finditer(r'\{\s*(\.key\s*=.*?)\}\s*(?:,|$)', body, re.S):
        fields = {}
        for a in split_args(m.group(1)):
            mm = re.match(r'\.(\w+)\s*=\s*(.*)$', a, re.S)
            if mm:
                fields[mm.group(1)] = mm.group(2).strip()
        key, kind = string_value(fields.get("key", '""')) or "", fields.get("kind", "")
        label = string_value(fields.get("label", '""')) or key
        if kind == "ROW_HEADER":
            section = label
            continue
        if kind in SKIP_KINDS or key.startswith("__") or fields.get("saveScoped") == "1" or key.startswith("Bond."):
            continue
        if key in FILTERED:
            skipped.append(f"{key} ({FILTERED[key]})")
            continue
        category = CATEGORY.get(key) or SECTION_CATEGORY.get(section)
        if category is None and key not in KEEP_FROM_MOUSE:
            skipped.append(f"{key} ({section.lower()} page: no mouse/keyboard on the handheld)")
            continue
        if key not in regs:
            skipped.append(f"{key} (not a config key)")
            continue
        typ, var, rlo, rhi = regs[key]
        default = FORCED.get(key, DEFAULTS.get(key, inits.get(var)))
        entry = {"key": key, "label": label, "detail": DETAILS.get(key, ""), "category": category or "controls"}
        # The overlay's own page (owner 2026-11-15: our tabs copy the original's names and order). The few rows kept
        # from the Mouse page are stick/aim settings, so they go to the Controller page, the one that remains.
        entry["tab"] = "Controller" if section.upper() in ("MOUSE", "CONTROLLER") else section.title()
        if fields.get("restart") == "1" and "next start" not in entry["detail"]:
            entry["detail"] = (entry["detail"] + " Applies at the next start.").strip()
        step = num(fields.get("step")) or 1
        lo = num(fields.get("uiMin")) if "uiMin" in fields else rlo
        hi = num(fields.get("uiMax")) if "uiMax" in fields else rhi
        unit = string_value(fields.get("unit", '""')) or ""
        div = num(fields.get("dispDiv"))
        if kind in ("ROW_TOGGLE", "ROW_ENUM"):
            vals = names.get(fields.get("names", ""), [])
            if not vals:
                skipped.append(f"{key} (no value names)")
                continue
            entry["values"] = [{"value": str(i), "label": n} for i, n in enumerate(vals)]
        elif kind == "ROW_MSAA":
            entry["values"] = [{"value": str(v), "label": "Off" if v == 1 else f"{v}x"} for v in msaa_seq]
        elif kind == "ROW_FPSCAP":
            entry["values"] = [{"value": str(v), "label": "Auto (display rate)" if v < 0 else f"{v} fps"} for v in fps_seq]
        elif kind == "ROW_SLIDER":
            if lo is None or hi is None or hi <= lo:
                skipped.append(f"{key} (no range)")
                continue
            if key == "Video.FovScale":  # stored as % of the original vertical FOV, shown as degrees (60° = 100 %)
                entry["values"] = [{"value": fmt(v), "label": f"{round(v * 0.6)}°"} for v in frange(lo, hi, step)]
            elif div:
                while (hi - lo) / step + 1 > 41:  # our menu steps through every value: 5 % steps, not 1 %
                    step *= 5
                entry["values"] = [{"value": fmt(v), "label": f"{round(v / div)}{unit}"} for v in frange(lo, hi, step)]
            elif key in ("Video.DrawDistance", "Video.FogDistance", "Video.LodDistance"):
                entry["values"] = [{"value": fmt(v), "label": f"{v / 100:g}x"} for v in frange(lo, hi, step)]
            else:
                entry.update(min=lo, max=hi, step=step)
                if unit:
                    entry["unit"] = unit
        else:
            skipped.append(f"{key} (row kind {kind})")
            continue
        if default is None:
            print(f"no default for {key} ({var}); using the first value", file=sys.stderr)
            default = num(entry["values"][0]["value"]) if "values" in entry else lo
        entry["default"] = fmt(default)
        if key in FORCED:
            entry["force"] = True
        options.append(entry)
    for s in skipped:
        print(f"filtered: {s}", file=sys.stderr)
    json.dump({"format": 1, "module": "goldeneye", "options": options}, sys.stdout, indent=1, ensure_ascii=False)
    print()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1])
