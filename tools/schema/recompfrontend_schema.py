#!/usr/bin/env python3
"""Generates settings_schema.json for a RecompFrontend game (docs/planning/MENU.md, design of 2026-11-14: each
game's own settings, all of them, in one consistent launcher UI) from the game's own config registrations.

    tools/schema/recompfrontend_schema.py <game checkout> <module id> > settings_schema.json

RecompFrontend games declare their options in code, through recomp::config::Config (librecomp/config.hpp):
add_enum_option / add_number_option / add_percent_number_option / add_bool_option / add_string_option, on config
tabs made by recompui::config::create_{general,graphics,sound}_tab (the frontend's own common options, in
lib/RecompFrontend/recompui/src/config/ui_config_tab_*.cpp) or create_config_tab(name, id, ...) (a game's own tab,
e.g. DK64's "technical"). Each tab is one config (saved as <config dir>/<id>.json), so an option is identified by
"<config id>.<option id>" - that is the schema key, and the shim (modules/common/n64recomp/recompfrontend_shim.cpp)
splits it again to call recompui::config::get_config(<config id>) and set <option id> with the type the option was
registered with. No per-option code anywhere: a new option upstream shows up in our menu at the next module build.

Values in the schema are strings in the port's own terms:
- enum: the option's key (what <id>.json stores, e.g. "Auto", "InvertY"), labelled with the option's display name;
- bool: "true" / "false";
- number: the number; ranges come from the registration (percent options are 0-100 %).

What the source can't tell us is curated below, shared first and per game after:
- FILTERED: desktop-only items (MENU.md's only filter: window mode, desktop-only input such as background input and
  mouse, dev/debug switches). Options the port registers as hidden (debug_mode, developer_mode, api_option,
  hpfb_option) are skipped like the port's own menu skips them.
- LABELS / DETAILS: where the registration has no usable text (an empty name, a description built at runtime).
- CATEGORY: our tab for each option; the config tab gives the default (general -> gameplay, graphics, sound -> audio,
  anything else -> enhancements), with input-ish ids in general moved to controls.
- Per game (GAMES): extra filters, labels, categories and DEFAULT_OVERRIDES (sent at launch: "force").
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lite import (Constants, calls, clean_html, enum_tables, read_source, source_files,  # noqa: E402
                      string_value)

# ---- shared curation ------------------------------------------------------------------------------------------------

FILTERED = {
    "graphics.wm_option": "window mode: Android is always fullscreen",
    "general.background_input_mode": "reads the controller while the window is out of focus: a desktop window thing",
    "general.mouse_sensitivity": "mouse aiming: no mouse on the handheld",
    "general.mouse_invert_mode": "mouse aiming: no mouse on the handheld",
    "graphics.developer_mode": "RT64 developer tools (a dev/debug window)",
    "general.debug_mode": "debug features",
    "graphics.api_option": "graphics API picker: Android is Vulkan only",
    "graphics.hpfb_option": "renderer internals the port hides",
}

LABELS = {
    "graphics.rr_manual_value": "Manual Framerate",
}
DETAILS = {
    # The port builds this text at runtime with the detected refresh rate (get_framerate_text).
    "graphics.rr_option": "Sets the game's output framerate. This option does not affect gameplay. Original keeps "
                          "the game's own rate, Display matches the screen's refresh rate (in-between frames are "
                          "interpolated), Manual uses the Manual Framerate below.",
    "graphics.rr_manual_value": "The output framerate while Framerate is Manual (in-between frames are interpolated). "
                                "A lower target than the screen's rate saves battery.",
}

# Friendlier steps than the registration's 1: our menu steps through every value, so a 0-100 % slider in ones is
# 100 presses (PortSchema also caps a range at 41 steps). Explicit choices where a few values are what matter.
PERCENT_STEP = 5
CHOICES = {
    # 20-240 in ones: the targets players pick, including half-rate ones that save battery on a 120 Hz screen.
    "graphics.rr_manual_value": [("30", "30 fps"), ("40", "40 fps"), ("45", "45 fps"), ("60", "60 fps"),
                                 ("72", "72 fps"), ("90", "90 fps"), ("120", "120 fps"), ("144", "144 fps")],
}

CONTROLS_HINTS = ("invert", "camera", "sensitivity", "deadzone", "rumble", "gyro", "mouse", "aim", "analog")

# ---- per-game curation ----------------------------------------------------------------------------------------------

GAMES = {
    "banjotooie-recomp": {},
    "dk64-recomp": {
        "category": {"general.story_skip": "gameplay", "general.lightning_flashes": "graphics",
                     "technical.multiplayer_enabled": "gameplay"},
    },
    "blastcorps-recomp": {},
    "hm64-recomp": {},
    "sk2-recomp": {
        "category": {"general.two_player_split_screen_direction": "gameplay"},
    },
    # zelda64-recomp and banjo-recomp are disabled (modules/disabled.txt); their schemas can be added the same way.
}

# ---- extraction -----------------------------------------------------------------------------------------------------

ADD_CALL = r'\b(\w+)\s*(?:\.|->)\s*add_(enum|number|percent_number|bool|string)_option'
DEFAULT_TAB_NAMES = {"general": "General", "graphics": "Graphics", "sound": "Sound", "controls": "Controls"}
TAB_INFO = {}  # config id -> (creation order, tab label); filled by extract()
FRONTEND_TABS = {"ui_config_tab_general.cpp": "general", "ui_config_tab_graphics.cpp": "graphics",
                 "ui_config_tab_sound.cpp": "sound"}
CREATE_TAB = re.compile(r'(?:auto|recomp::config::Config)\s*&\s*(\w+)\s*=\s*recompui::config::'
                        r'(create_general_tab|create_graphics_tab|create_sound_tab|create_config_tab)\s*\(')
FUNC_WITH_CONFIG = re.compile(r'\b(\w+)\s*\(\s*recomp::config::Config\s*&\s*(\w+)\s*\)\s*\{')


def number(expr, consts=None):
    expr = re.sub(r'(?<=\d)[fF]\b', '', expr.strip())
    try:
        return float(expr)
    except ValueError:
        pass
    if expr in ("true", "false"):
        return 1.0 if expr == "true" else 0.0
    return None


def fmt(v):
    return str(int(v)) if float(v).is_integer() else ("%g" % v)


def general_tab_flags(game_texts):
    """GeneralTabOptions (recompui/config.h): which of the general tab's optional controls a game turns on."""
    flags = {"has_rumble_strength": True, "has_gyro_sensitivity": False, "has_mouse_sensitivity": False}
    for text in game_texts:
        for m in re.finditer(r'\.\s*(has_rumble_strength|has_gyro_sensitivity|has_mouse_sensitivity)\s*=\s*(true|false)', text):
            flags[m.group(1)] = m.group(2) == "true"
    return flags


def extract(checkout):
    src_dirs = [os.path.join(checkout, d) for d in ("src", "include") if os.path.isdir(os.path.join(checkout, d))]
    frontend = os.path.join(checkout, "lib", "RecompFrontend", "recompui")
    consts = Constants()
    game_files = [f for d in src_dirs for f in source_files(d)]
    for f in game_files + source_files(os.path.join(frontend, "include")):
        consts.add_file(f)
    game_texts = {f: read_source(f) for f in game_files if f.endswith((".cpp", ".c"))}

    # Which config tabs the game creates, and the variables that hold them.
    var_config, created = {}, set()
    # Tab labels and creation order (owner 2026-11-15: our tabs copy the original menu's names and order). The
    # frontend shows tabs in the order the game creates them; built-in tabs default to their tab_name.
    for text in game_texts.values():
        for m in re.finditer(r'recompui::config::create_(\w+)_tab\s*\(', text):
            kind = m.group(1)
            args = list(calls(text[m.start():], r'create_%s_tab' % kind))[0][1]
            if kind == "config":
                cid, label = string_value(args[1], consts), string_value(args[0], consts)
            else:
                cid = kind
                named = args[1] if kind == "general" and len(args) > 1 else (args[0] if kind != "general" and args else None)
                label = (string_value(named, consts) if named else None) or DEFAULT_TAB_NAMES.get(kind, kind.title())
            if cid and cid not in TAB_INFO:
                TAB_INFO[cid] = (len(TAB_INFO), label or cid.title())
    for f, text in game_texts.items():
        for m in CREATE_TAB.finditer(text):
            kind = m.group(2)
            if kind == "create_config_tab":
                args = list(calls(text[m.start():], r'create_config_tab'))[0][1]
                cid = string_value(args[1], consts)
            else:
                cid = kind[len("create_"):-len("_tab")]
            var_config[m.group(1)] = cid
            created.add(cid)
        for kind in ("general", "graphics", "sound"):
            if re.search(r'recompui::config::create_%s_tab\s*\(' % kind, text):
                created.add(kind)
    # Helper functions that receive a tab: add_general_options(general_config) -> its parameter means "general".
    func_config = {}
    for text in game_texts.values():
        for fname in {m.group(1) for m in FUNC_WITH_CONFIG.finditer(text)}:
            for m in re.finditer(r'\b%s\s*\(\s*(\w+)\s*\)\s*;' % fname, text):
                if m.group(1) in var_config:
                    func_config[fname] = var_config[m.group(1)]

    flags = general_tab_flags(game_texts.values())
    found = []
    sources = [(os.path.join(frontend, "src", "config", f), cid) for f, cid in FRONTEND_TABS.items() if cid in created]
    sources += [(f, None) for f in game_texts]
    for path, fixed_cid in sources:
        text = read_source(path) if fixed_cid else game_texts[path]
        tables = enum_tables(text, consts)
        funcs = [(m.start(), m.group(1), m.group(2)) for m in FUNC_WITH_CONFIG.finditer(text)]
        for m, args, _ in calls(text, ADD_CALL):
            var, kind = m.group(1), m.group(2)
            if fixed_cid:
                cid = fixed_cid
                # The general tab's optional controls (GeneralTabOptions): only the ones this game turns on.
                guard = re.search(r'if\s*\(\s*options\.(has_\w+)\s*\)\s*\{[^{}]*$', text[:m.start()][-400:])
                if guard and not flags.get(guard.group(1), True):
                    continue
            elif var in var_config:
                cid = var_config[var]
            else:
                enclosing = [f for f in funcs if f[0] < m.start()]
                cid = func_config.get(enclosing[-1][1]) if enclosing and enclosing[-1][2] == var else None
            if cid is None:
                print(f"{os.path.basename(path)}: no config tab for {var}.add_{kind}_option({args[0]})", file=sys.stderr)
                continue
            oid = string_value(args[0], consts)
            if oid is None:
                print(f"unresolved option id {args[0]}", file=sys.stderr)
                continue
            o = {"config": cid, "id": oid, "kind": kind,
                 "name": string_value(args[1], consts) or "", "desc": string_value(args[2], consts)}
            a = args + [None] * 10
            if kind == "enum":
                table = tables.get(a[3].strip()) if a[3] else None
                if table is None:
                    print(f"{cid}.{oid}: no enum table {a[3]}", file=sys.stderr)
                    continue
                o["values"] = [(k, l) for _, k, l in table]
                dflt = re.sub(r'\s+', '', a[4] or "")
                match = [k for v, k, _ in table if v == dflt or v.split("::")[-1] == dflt.split("::")[-1]]
                if not match:
                    n = number(dflt)
                    match = [table[int(n)][1]] if n is not None and int(n) < len(table) else []
                if not match:
                    print(f"{cid}.{oid}: default {dflt} not in its table; using the first value", file=sys.stderr)
                o["default"] = match[0] if match else table[0][1]
                o["hidden"] = (a[5] or "").strip() == "true"
            elif kind == "number":
                nums = [number(x) if x is not None else None for x in a[3:9]]
                mn, mx, step, prec, pct, dflt = nums
                o.update(min=mn or 0.0, max=mx or 0.0, step=step or 1.0, percent=bool(pct),
                         default=dflt if dflt is not None else (mn or 0.0))
                o["hidden"] = (a[9] or "").strip() == "true"
            elif kind == "percent_number":
                o.update(min=0.0, max=100.0, step=1.0, percent=True, default=number(a[3]) or 0.0)
                o["hidden"] = (a[4] or "").strip() == "true"
            elif kind == "bool":
                o["default"] = (a[3] or "false").strip() == "true"
                o["hidden"] = (a[4] or "").strip() == "true"
            elif kind == "string":
                o["default"] = string_value(a[3] or '""', consts) or ""
                o["hidden"] = (a[4] or "").strip() == "true"
            found.append(o)
    return found


def category(key, cid, oid, game):
    if key in game.get("category", {}):
        return game["category"][key]
    if cid == "graphics":
        return "graphics"
    if cid == "sound":
        return "audio"
    if cid == "general":
        return "controls" if any(h in oid for h in CONTROLS_HINTS) else "gameplay"
    return "enhancements"


def main(checkout, module):
    game = GAMES.get(module)
    if game is None:
        sys.exit(f"no curation for {module} in {__file__}")
    filtered = {**FILTERED, **game.get("filtered", {})}
    options, skipped = [], []
    for o in extract(checkout):
        key = f"{o['config']}.{o['id']}"
        if key in filtered:
            skipped.append(f"{key} ({filtered[key]})")
            continue
        if o.get("hidden"):
            skipped.append(f"{key} (hidden by the port)")
            continue
        if any(x["key"] == key for x in options):
            continue
        entry = {"key": key,
                 "label": game.get("labels", {}).get(key) or LABELS.get(key) or o["name"] or o["id"],
                 "detail": game.get("details", {}).get(key) or DETAILS.get(key) or clean_html(o["desc"] or ""),
                 "category": category(key, o["config"], o["id"], game)}
        entry["tab"] = TAB_INFO.get(o["config"], (99, o["config"].title()))[1]
        if o["kind"] == "enum":
            entry["values"] = [{"value": k, "label": l} for k, l in o["values"]]
            entry["default"] = o["default"]
        elif o["kind"] == "bool":
            entry["values"] = [{"value": "true", "label": "On"}, {"value": "false", "label": "Off"}]
            entry["default"] = "true" if o["default"] else "false"
        elif key in CHOICES:
            entry["values"] = [{"value": v, "label": l} for v, l in CHOICES[key]]
            entry["default"] = fmt(o["default"])
        elif o["kind"] in ("number", "percent_number"):
            if o["max"] <= o["min"]:
                skipped.append(f"{key} (no range)")
                continue
            step = PERCENT_STEP if o["percent"] and o["step"] == 1 and o["max"] - o["min"] >= 50 else o["step"]
            entry.update(min=o["min"], max=o["max"], step=step, default=fmt(o["default"]))
            if o["percent"]:
                entry["unit"] = "%"
        else:
            skipped.append(f"{key} (free text: no picker)")
            continue
        if key in game.get("default_overrides", {}):
            entry["default"] = game["default_overrides"][key]
            entry["force"] = True
        options.append(entry)
    order = {label: n for n, label in TAB_INFO.values()}
    options.sort(key=lambda e: order.get(e["tab"], 99))
    for s in skipped:
        print(f"filtered: {s}", file=sys.stderr)
    json.dump({"format": 1, "module": module, "options": options}, sys.stdout, indent=1, ensure_ascii=False)
    print()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1], sys.argv[2])
