#!/usr/bin/env python3
"""Generates Golden Balloon's settings_schema.json (docs/planning/MENU.md, design of 2026-11-14: each game's own
settings in one consistent UI) from its own video-config table, platform/video_config.c.

    tools/schema/goldenballoon_schema.py <goldenballoon checkout>/platform/video_config.c > settings_schema.json

modules/goldenballoon/build.sh runs it into the module's data/ folder, so labels, help text, ranges and live/restart
scope always match the pinned upstream. What the table can't tell us lives below, in one place:
- FILTERED: desktop-only items (MENU.md: window mode, tearing, dev tools, updates, the port's own launcher, desktop
  text-to-speech, file-list strings we have no picker for);
- CHOICES: the values of string keys, which the port validates in code rather than listing in the table;
- STEPS / CHOICE_LABELS: friendlier steps and labels for numeric keys;
- DEFAULT_OVERRIDES: where our default differs from the port's Restored preset, always sent ("force").
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from goldenballoon_parse import parse  # noqa: E402

FILTERED = {
    "Window.Mode",                 # windowed/fullscreen: Android is always fullscreen
    "Video.AllowTearing",          # desktop presentation latency
    "Tools.Enabled",               # developer tools
    "App.UpdateCheck",             # the port's own updater; the module server updates modules
    "Launcher.SkipWhenReady",      # the port's own launcher isn't built here
    "Accessibility.Speech", "Accessibility.SpeechRate", "Accessibility.SpeechVolume", "Accessibility.SpeechRace",
                                   # desktop text-to-speech back ends (speech-dispatcher, SAPI, AVSpeech)
    "Video.TexturePack",           # a file path: texture packs come through our mods page
    "Content.PackDisabled",        # a list of content packs: no picker yet
    "Gameplay.SimulationCadence",  # values aren't enumerable from the source; Frame limit covers pacing
}

CHOICES = {
    "Video.Aspect": [("auto", "Fit the screen"), ("4:3", "4:3 (original)"), ("16:9", "16:9")],
    "Video.GameplayFOV": [("authored", "Original"), ("70", "70°"), ("80", "80°"), ("90", "90°"), ("100", "100°")],
    "Video.FrameLimit": [("original", "Original"), ("30", "30 fps"), ("60", "60 fps"), ("display", "Display rate"),
                         ("uncapped", "Uncapped")],
    "Video.MotionSmoothing": [("off", "Off"), ("interpolate", "Interpolate")],
    "Video.Mode": [("pure", "Pure"), ("restored", "Restored"), ("remastered", "Remastered")],
    "Video.WorldShadows": [("off", "Off"), ("soft", "Soft"), ("full", "Full")],
    "Enhancements.AIDifficulty": [("authored", "Original"), ("hard", "Hard"), ("brutal", "Brutal")],
    "Input.RumbleProfile": [("light", "Light"), ("medium", "Medium"), ("balanced", "Balanced"), ("strong", "Strong"),
                            ("full", "Full")],
    "Camera.Obstruction": [("legacy", "Original"), ("modern", "Modern")],
    "Camera.Comfort": [("authored", "Original"), ("reduced", "Reduced motion")],
    "Gameplay.MenuLanguages": [("all", "All in the ROM"), ("authentic", "This cartridge's")],
}

STEPS = {
    "Audio.MasterVolume": 5, "Audio.MusicVolume": 5, "Audio.EffectsVolume": 5,
    "Enhancements.DrawDistance": 100,
}
UNITS = {"Audio.MasterVolume": "%", "Audio.MusicVolume": "%", "Audio.EffectsVolume": "%"}
CHOICE_LABELS = {
    "Video.RenderScale": [("0.5", "50%"), ("0.75", "75%"), ("1", "100% (screen)"), ("1.5", "150%"), ("2", "200%")],
    "Video.MSAA": [("0", "Off"), ("2", "2x"), ("4", "4x"), ("8", "8x")],
    "Video.AnisotropicFiltering": [("1", "Off (N64 filter)"), ("2", "2x"), ("4", "4x"), ("8", "8x"), ("16", "16x")],
    "Enhancements.LodBias": [("0", "Original"), ("1", "Higher"), ("2", "Highest")],
    "Enhancements.Speedometer": [("0", "Off"), ("1", "On"), ("2", "Detailed")],
}
# Our defaults where the port's Restored preset differs. Always sent at launch. Empty since 2026-11-15 (owner: every
# upstream default except controller mapping); it used to force Video.RenderScale 1 instead of Restored's 2.0.
DEFAULT_OVERRIDES = {}

CATEGORY = {"presentation": "graphics", "fidelity": "graphics", "pacing": "graphics", "audio": "audio",
            "input": "controls", "interface": "enhancements"}
TAB_ORDER = ["Presentation", "Fidelity", "Pacing", "Audio", "Interface", "Input"]
GAMEPLAY_KEYS = {"Enhancements.AIDifficulty", "Enhancements.AdventureParty", "Content.BonusRacers",
                 "Content.PacksEnabled"}


def fmt(v):
    return str(int(v)) if float(v).is_integer() else ("%g" % v)


# Profiles (owner 2026-11-16, like SoH's and 2S2H's: from the port's own presets, a layer the player applies on top of
# the defaults). Golden Balloon's presentation modes are presets over many keys (video_config.c s_preset /
# s_preset_text: Pure, Restored, Remastered; keys a mode doesn't pin are NULL there and stay as the player set them).
# Restored is the port's default, which our menu's own "Port defaults" covers, so the profiles are Pure and Remastered.
# Forced keys (ours) are left out, and a value must be one the option offers.
PROFILES = [
    (0, "pure", "Pure",
     "Golden Balloon's Pure mode: the N64 presentation as it was, with nothing added. Only the settings this mode pins "
     "change; everything else stays as you set it."),
    (2, "remastered", "Remastered",
     "Golden Balloon's Remastered mode: the port's modern effects on top of the original look (RemasterFX, world "
     "shadows and the other enhancements this mode turns on). Only the settings this mode pins change; everything "
     "else stays as you set it."),
]


def build_profiles(entries, options):
    by_key = {o["key"]: o for o in options}
    out = []
    for index, pid, label, detail in PROFILES:
        values, pinned = {}, 0
        for e in entries.values():
            o = by_key.get(e["name"])
            num = e["preset"][index] if e.get("preset") else None
            txt = e["preset_text"][index] if e.get("preset_text") else None
            if num is None and txt is None:
                continue
            pinned += 1
            if o is None or o.get("force"):
                continue
            value = txt if txt is not None else fmt(num)
            if "values" in o:
                ok = any(v["value"] == value for v in o["values"])
            else:
                ok = float(o["min"]) <= float(value) <= float(o["max"])
            if ok:
                values[o["key"]] = value
        print(f"{pid}: {len(values)} of {pinned} pinned values are menu options", file=sys.stderr)
        out.append({"id": pid, "label": label, "detail": detail, "values": values})
    return out


def main(src):
    options = []
    for e in parse(src).values():
        name = e["name"]
        if name in FILTERED:
            continue
        o = {"key": name, "label": e["label"], "detail": e["help"].replace("\\n", " ").strip(),
             "category": "gameplay" if name in GAMEPLAY_KEYS else CATEGORY.get(e["cat"], "enhancements")}
        # The port's own category name (mdkr_video_category_name: Presentation, Fidelity, Pacing, Audio, Interface,
        # Input), in its enum order (owner 2026-11-15: our tabs copy the original's names and order).
        o["tab"] = e["cat"].title()
        if e["scope"] == "restart":
            o["detail"] = (o["detail"] + " Applies at the next start.").strip()
        restored_num = e["preset"][1] if e.get("preset") else None
        restored_txt = e["preset_text"][1] if e.get("preset_text") else None
        if isinstance(e.get("default"), float):
            restored_num = e["default"]
        elif isinstance(e.get("default"), str):
            restored_txt = e["default"]
        if e["type"] == "string":
            if name not in CHOICES:
                print(f"skipped (no choices): {name}", file=sys.stderr)
                continue
            o["values"] = [{"value": v, "label": l} for v, l in CHOICES[name]]
            o["default"] = restored_txt or CHOICES[name][0][0]
        elif name in CHOICE_LABELS:
            o["values"] = [{"value": v, "label": l} for v, l in CHOICE_LABELS[name]]
            o["default"] = fmt(restored_num) if restored_num is not None else CHOICE_LABELS[name][0][0]
        elif e["min"] == 0 and e["max"] == 1:
            o["values"] = [{"value": "1", "label": "On"}, {"value": "0", "label": "Off"}]
            o["default"] = fmt(restored_num) if restored_num is not None else "0"
        else:
            o["min"], o["max"] = e["min"], e["max"]
            o["step"] = STEPS.get(name, 1)
            if name in UNITS:
                o["unit"] = UNITS[name]
            o["default"] = fmt(restored_num) if restored_num is not None else fmt(e["min"])
        if name in DEFAULT_OVERRIDES:
            o["default"] = DEFAULT_OVERRIDES[name]
            o["force"] = True
        options.append(o)
    options.sort(key=lambda o: TAB_ORDER.index(o["tab"]) if o["tab"] in TAB_ORDER else len(TAB_ORDER))
    out = {"format": 1, "module": "goldenballoon", "options": options}
    profiles = build_profiles(parse(src), options)
    if profiles:
        out["profiles"] = profiles
    json.dump(out, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main(sys.argv[1])
