#!/usr/bin/env python3
"""Generates Jet Force Gemini: Recompiled's settings_schema.json (docs/planning/MENU.md, design of 2026-11-14: each
game's own settings in one consistent launcher UI) from the port's own option tables.

    tools/schema/jfg_schema.py <JFG checkout> > settings_schema.json

JFG is not a RecompFrontend game: its options live in jfg::settings (src/jfg/jfg_settings_api.h: int values by key,
saved as key=value lines in jfg_port_options.txt), and its "Select" menu is generated from data tables in
tools/gen_jfg_config_rml.py (GENERAL, GAMEPLAY, CONTROLS, GRAPHICS, TEXTURES: kind, key, title, range or choices,
help text). Those tables are the port's own UI text, so this generator reads them (running that file as a module;
its main() is guarded) instead of copying anything. Defaults: a new player starts on the Modern profile, i.e. State's
initializers (src/jfg/jfg_options.h) overridden by the "Modern" profile text (src/jfg/jfg_profiles.cpp).

Two kinds of key, told apart by a dot (the shim, modules/jfg-recomp/plm_shim.cpp, does the same):
- "<option>" (no dot): a jfg::settings option. At launch it is written into jfg_port_options.txt before the game
  reads it; live it goes through jfg::settings::set (applied, pushed to the renderer and saved, like the port's menu).
- "<file>.<key>": one of the recomp's own options the port's tabs also show (UPSTREAM_KEYS in the table file:
  volume, sticks, rumble, resolution, MSAA), stored in <config dir>/<file>.json. The value is exactly the JSON value
  the port stores (an enum by its name, e.g. "Original2x"), so the shim writes it without knowing the option. These
  apply at the next start (the port reads those files once, at start-up).

Curated here: FILTERED (desktop-only, the port's own launcher/updater, mouse, dev tools), FILE_KEYS (which file an
upstream key lives in, and its stored values), CATEGORY overrides, and nicer slider steps.
"""
import json
import os
import re
import runpy
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lite import clean_html, read_source  # noqa: E402

FILTERED = {
    "wm_option": "window mode: Android is always fullscreen",
    "api_option": "graphics API picker: Android is Vulkan only",
    "background_input": "reads the controller while the window is out of focus: a desktop window thing",
    "skip_launcher": "the port's own launcher: the launcher's direct boot replaces it",
    "check_for_updates": "the port's own updater: the module server updates modules",
    "update_channel": "the port's own updater",
    "show_official_pack": "the port's own updater's texture pack download",
    "mouse_keyboard": "mouse and keyboard controls: no mouse on the handheld",
    "mouse_camera_speed": "mouse: no mouse on the handheld",
    "mouse_aim_speed": "mouse: no mouse on the handheld",
    "invert_mouse_y": "mouse: no mouse on the handheld",
    "texture_dump": "texture dumping (a dev tool, off again at every start)",
    "dump_by_level": "texture dumping (a dev tool)",
    "fly_speed": "the F7 fly camera (a screenshot/dev tool on a keyboard key)",
    "fly_freeze": "the F7 fly camera (a screenshot/dev tool on a keyboard key)",
    "fly_hide_hud": "the F7 fly camera (a screenshot/dev tool on a keyboard key)",
    "show_advanced": "only shows more rows in the port's own menu (ours shows them all)",
    "rr_choice": "the recomp's own framerate row: Display Frame Rate (smooth_motion) covers it on Android",
}

# The recomp's own options the port's tabs show (ui_config.cpp upstream_options), by the file the recomp saves them
# in (src/config/config.cpp) and the JSON values it stores. Defaults are the recomp's (config.cpp from_or_default).
FILE_KEYS = {
    "main_volume": ("sound", None, 100),
    "joystick_deadzone": ("general", None, 5),
    "joystick_range": ("general", None, 66),
    "rumble_strength": ("general", None, 25),
    "res_option": ("graphics", [("Original", "Original"), ("Original2x", "Original 2x"), ("Auto", "Auto")], "Auto"),
    "ds_option": ("graphics", [("1", "Off"), ("2", "2x"), ("4", "4x")], "1"),
    "msaa_option": ("graphics", [("None", "None"), ("MSAA2X", "2x"), ("MSAA4X", "4x")], "MSAA2X"),
}

TAB_CATEGORY = {"GENERAL": "enhancements", "GAMEPLAY": "gameplay", "CONTROLS": "controls", "GRAPHICS": "graphics",
                "TEXTURES": "graphics"}
CATEGORY = {"main_volume": "audio", "start_skips_cutscenes": "gameplay", "fast_pause": "gameplay"}
RESTART = {"frame_rate"}  # jfg::settings::frame_rate_needs_restart


def nice_step(lo, hi):
    span = hi - lo
    for step in (1, 2, 5, 10, 25, 50, 100):
        if span / step <= 40:
            return step
    return 100


def defaults(checkout):
    """State's initializers, then the Modern profile's lines (what a new player starts with)."""
    out = {}
    header = read_source(os.path.join(checkout, "src/jfg/jfg_options.h"))
    body = header[header.index("struct State"):]
    body = body[:body.index("\n};")]
    for m in re.finditer(r'\b(bool|int)\s+([^;]+);', body):
        for part in m.group(2).split(","):
            mm = re.match(r'\s*(\w+)\s*=\s*(true|false|-?\d+)\s*$', part)
            if mm:
                v = mm.group(2)
                out[mm.group(1)] = 1 if v == "true" else 0 if v == "false" else int(v)
    profiles = read_source(os.path.join(checkout, "src/jfg/jfg_profiles.cpp"))
    m = re.search(r'\{\s*"Modern"\s*,\s*(?:"(?:[^"\\]|\\.)*"\s*)+,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}', profiles)
    if m:
        text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))).replace("\\n", "\n")
        for line in text.split("\n"):
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = int(v)
    else:
        print("no Modern profile found: State defaults only", file=sys.stderr)
    return out


def main(checkout):
    tables = runpy.run_path(os.path.join(checkout, "tools/gen_jfg_config_rml.py"), run_name="jfg_tables")
    dflt = defaults(checkout)
    options, skipped = [], []
    for tab in ("GENERAL", "GAMEPLAY", "CONTROLS", "GRAPHICS", "TEXTURES"):
        section = None
        for kind, key, title, extra, help_text in tables[tab]:
            if kind == "group":
                section = title  # a header row inside the tab
                continue
            if key in FILTERED:
                skipped.append(f"{key} ({FILTERED[key]})")
                continue
            detail = clean_html(help_text or "").replace("&lt;", "<").replace("&gt;", ">")
            entry = {"key": key, "label": title, "detail": detail,
                     "category": CATEGORY.get(key, TAB_CATEGORY[tab])}
            # The original menu's tab and header (owner 2026-11-15: our tabs copy the original's names and order).
            entry["tab"] = tab.title()
            if section:
                entry["section"] = section
            if key in FILE_KEYS:
                file, values, d = FILE_KEYS[key]
                entry["key"] = f"{file}.{key}"
                entry["detail"] = (detail + " Applies at the next start.").strip()
                if values:
                    entry["values"] = [{"value": v, "label": l} for v, l in values]
                else:
                    lo, hi, suffix = extra
                    entry.update(min=lo, max=hi, step=nice_step(lo, hi), unit=suffix)
                entry["default"] = str(d)
            elif kind == "bool":
                entry["values"] = [{"value": "1", "label": "On"}, {"value": "0", "label": "Off"}]
                entry["default"] = str(dflt.get(key, 0))
            elif kind == "choice":
                entry["values"] = [{"value": str(v), "label": l} for v, l in extra]
                entry["default"] = str(dflt.get(key, extra[0][0]))
            elif kind == "range":
                lo, hi, suffix = extra
                entry.update(min=lo, max=hi, step=nice_step(lo, hi))
                if suffix.strip():
                    entry["unit"] = suffix
                entry["default"] = str(dflt.get(key, lo))
            else:
                skipped.append(f"{key} (unknown kind {kind})")
                continue
            if key in RESTART:
                entry["detail"] = (entry["detail"] + " Applies at the next start.").strip()
            options.append(entry)
    for s in skipped:
        print(f"filtered: {s}", file=sys.stderr)
    json.dump({"format": 1, "module": "jfg-recomp", "options": options}, sys.stdout, indent=1, ensure_ascii=False)
    print()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1])
