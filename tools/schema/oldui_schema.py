#!/usr/bin/env python3
"""Generates settings_schema.json for an "old UI" recomp (docs/planning/MENU.md, design of 2026-11-14: each game's own
settings, all of them, in one consistent launcher UI): Mega Man 64 (mm64-recomp) and Bomberman 64 (bm64-recomp).

    tools/schema/oldui_schema.py <game checkout> <module id> > settings_schema.json

Why a generator of its own: these projects copy Zelda64Recomp's pre-RecompFrontend UI. There is no config API with
add_*_option registrations to read (recompfrontend_schema.py's source); the options exist in two hand-written places
that have to be joined:

1. The menu: assets/config_menu/<tab>.rml, one <div class="config-option"> per row with its title, the RmlUi inputs
   (radio buttons bound with data-checked="<binding>" and value="<port value>", range sliders bound with
   data-value="<binding>"), and the help text as <p data-if="cur_config_index == N"> paragraphs (N = the index the
   row sets on hover). This gives labels, choices, ranges and descriptions - what the player sees.
2. The storage: src/game/config.cpp, save_<file>_config() writing config_json["<key>"] = zelda64::get_<binding>()
   into <app folder>/<file>.json (graphics: the GraphicsConfig to_json table), and the loaders reading them back
   with from_or_default(json, "<key>", <default>). This gives where a value lives and its default.

An option is the join of both: a menu binding whose value the port saves. The schema key is "<file>.<json key>"
(e.g. "graphics.msaa_option", "cheats.infinite_lives"), and the shim (modules/common/n64recomp/recompfrontend_shim.cpp,
PLM_OLDUI_SCHEMA) writes the value into <config dir>/<file>.json before the game's zelda64::load_config() reads it,
and live by re-running the port's own load_<file>_config() - no per-option code anywhere.

Left out, besides the curated FILTERED list:
- menu rows the port never saves (MM64's Invert Y Axis, Voice and Sound Effects volume: bound in the UI but missing
  from config.cpp, so they reset every start and there's no file to apply them through); stderr lists them;
- whole menu files: the controls tab (the controller/keyboard binding menu: owner decision 2026-11-14, all input goes
  through our intents), debug (dev tools) and mods (the launcher owns mods).

Values are strings in the port's own terms: an enum's JSON name ("Auto", "MSAA2X", "On"), a number as digits.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpp_lite import balanced, calls, clean_html, read_source, source_files  # noqa: E402

# ---- shared curation ------------------------------------------------------------------------------------------------

SKIP_FILES = {
    "controls.rml": "the port's controller/keyboard binding menu (owner 2026-11-14: input goes through our intents)",
    "debug.rml": "debug tools",
    "mods.rml": "the mods tab (the launcher owns mods)",
}

FILTERED = {
    "graphics.wm_option": "window mode: Android is always fullscreen",
    "general.background_input_mode": "reads the controller while the window is out of focus: a desktop window thing",
    "general.mouse_sensitivity": "mouse aiming: no mouse on the handheld (owner 2026-11-14: no mouse/keyboard items)",
    "general.debug_mode": "debug features",
    "graphics.developer_mode": "RT64 developer tools (a dev/debug window)",
    "graphics.api_option": "graphics API picker: Android is Vulkan only",
    "graphics.hpfb_option": "renderer internals the port hides",
}

LABELS = {
    # Shares the Framerate row with rr_option in the port's menu (a slider shown while Framerate is Manual).
    "graphics.rr_manual_value": "Manual Framerate",
}
DETAILS = {
    # The port's own text ends with the detected refresh rate ({{display_refresh_rate}}), built at runtime.
    "graphics.rr_option": "Sets the game's output framerate. This option does not affect gameplay. Original keeps "
                          "the game's own rate, Display matches the screen's refresh rate (in-between frames are "
                          "interpolated), Manual uses the Manual Framerate below.",
    "graphics.rr_manual_value": "The output framerate while Framerate is Manual (in-between frames are interpolated). "
                                "A lower target than the screen's rate saves battery.",
}

# The menu's 0-100 sliders in ones are 100 presses in ours (PortSchema caps a range at 41 steps anyway).
PERCENT_STEP = 5
CHOICES = {
    # 20-360 in ones upstream: the targets players pick, including half-rate ones that save battery on 120 Hz.
    "graphics.rr_manual_value": [("30", "30 fps"), ("40", "40 fps"), ("45", "45 fps"), ("60", "60 fps"),
                                 ("72", "72 fps"), ("90", "90 fps"), ("120", "120 fps"), ("144", "144 fps")],
}

CONTROLS_HINTS = ("invert", "camera", "sensitivity", "deadzone", "rumble", "gyro", "aim", "analog", "targeting")
FILE_CATEGORY = {"graphics": "graphics", "sound": "audio", "cheats": "gameplay"}

# ---- per-game curation ----------------------------------------------------------------------------------------------

GAMES = {
    "mm64-recomp": {},
    "bm64-recomp": {},
}

# ---- storage side (config.cpp) --------------------------------------------------------------------------------------


def function_bodies(text, name_regex):
    """[(function name, body text)] for definitions whose name matches name_regex (a list: several structs each
    have their own reset())."""
    out = []
    for m in re.finditer(r'\b(%s)\s*\([^;{)]*\)\s*\{' % name_regex, text):
        try:
            end = balanced(text, m.end() - 1, '{', '}')
        except ValueError:
            continue
        out.append((m.group(1), text[m.end():end - 1]))
    return out


def enum_names(checkout):
    """{'Enum::Value' and 'ns::Enum::Value': JSON name} from every NLOHMANN_JSON_SERIALIZE_ENUM the game can see."""
    names = {}
    roots = [os.path.join(checkout, d) for d in ("include", "src", "lib/N64ModernRuntime/ultramodern/include",
                                                 "lib/N64ModernRuntime/librecomp/include")]
    for root in roots:
        if not os.path.isdir(root):
            continue
        for f in source_files(root, ('.h', '.hpp', '.cpp')):
            for m in re.finditer(r'\{\s*([\w:]+)\s*,\s*"([^"]*)"\s*\}', read_source(f)):
                parts = m.group(1).split("::")
                if len(parts) >= 2:
                    names["::".join(parts[-2:])] = m.group(2)
    return names


def resolve_default(expr, consts, enums):
    """A from_or_default default (literal, constexpr name, enum value, wm_default(), `deck ? a : b`) as a string."""
    expr = expr.strip()
    if '?' in expr and ':' in expr.split('?', 1)[1]:
        expr = expr.split('?', 1)[1].split(':', 1)[1].strip()  # the not-a-Steam-Deck branch
    expr = re.sub(r'\(\s*\)$', '', expr)
    if expr in consts:
        expr = consts[expr]
    if re.fullmatch(r'-?\d+', expr):
        return expr
    if expr in ("true", "false"):
        return expr
    parts = expr.split("::")
    if len(parts) >= 2:
        return enums.get("::".join(parts[-2:]), parts[-1])
    return None


def storage(checkout):
    """{binding or json key: (file, json key, default or None)} from src/game/config.cpp."""
    text = read_source(os.path.join(checkout, "src", "game", "config.cpp"))
    files = dict((m.group(1), m.group(2)) for m in
                 re.finditer(r'constexpr\s+std::u8string_view\s+(\w+)_filename\s*=\s*u8"(\w+)\.json"', text))
    consts = dict((m.group(1), m.group(2).strip()) for m in
                  re.finditer(r'constexpr\s+(?:auto|int|bool)\s+(\w+_default)\s*=\s*([^;]+);', text))
    # wm_default() & co: a function returning a ternary; take the desktop branch like resolve_default does.
    for name, body in function_bodies(text, r'\w+_default'):
        r = re.search(r'return\s+([^;]+);', body)
        if r:
            consts[name] = r.group(1)
    enums = enum_names(checkout)

    defaults = {}
    for m, args, _ in calls(text, r'\bfrom_or_default'):
        if len(args) == 3:
            key = args[1].strip().strip('"')
            defaults.setdefault(key, resolve_default(args[2], consts, enums))

    graphics_keys = re.findall(r'\{\s*"(\w+)"\s*,\s*config\.\w+\s*\}', text)  # ultramodern::to_json table
    out = {}
    for fname, body in function_bodies(text, r'save_\w+_config'):
        stem = files.get(fname[len("save_"):-len("_config")])
        if stem is None:
            continue
        entries = []  # (json key, getter name)
        for m in re.finditer(r'config_json\["(\w+)"\]\s*=\s*([\w:]+)\s*\(\s*\)', body):
            entries.append((m.group(1), m.group(2).split("::")[-1]))
        for m in re.finditer(r'to_json\s*\(\s*config_json\["(\w+)"\]\s*,\s*([\w:]+)\s*\(\s*\)\s*\)', body):
            entries.append((m.group(1), m.group(2).split("::")[-1]))
        if re.search(r'to_json\s*\(\s*config_json\s*,', body):
            entries += [(k, "get_" + k) for k in graphics_keys]
        for key, getter in entries:
            item = (stem, key, defaults.get(key))
            out.setdefault(key, item)
            if getter.startswith("get_"):
                out.setdefault(getter[4:], item)  # binding low_health_beeps_enabled -> key low_health_beeps
    return out, text


def reset_default(ui_text, binding):
    """Defaults the port sets in a context's reset() rather than in config.cpp (the sound sliders)."""
    for _, body in function_bodies(ui_text, r'reset'):
        m = re.search(r'\b%s\s*=\s*(?:\(int\))?\s*(-?\d+|true|false)\s*;' % re.escape(binding), body)
        if m:
            return {"true": "1", "false": "0"}.get(m.group(1), m.group(1))
    return None

# ---- menu side (config_menu/*.rml) ----------------------------------------------------------------------------------


def attrs(tag):
    return dict(re.findall(r'([\w-]+)="([^"]*)"', tag))


def menu_rows(path):
    """[(binding, kind, row title, values [(value, label)] / (min, max, unit), help text)] from one config tab."""
    text = re.sub(r'<!--.*?-->', '', open(path, encoding='utf-8').read(), flags=re.S)
    helps = {int(m.group(1)): m.group(2) for m in
             re.finditer(r'<p\s+data-if="cur_config_index\s*==\s*(\d+)"\s*>(.*?)</p>', text, re.S)}
    labels = {m.group(1): m.group(2).strip() for m in
              re.finditer(r'<label[^>]*\bfor="([^"]+)"[^>]*>(.*?)</label>', text, re.S)}
    rows = []
    chunks = re.split(r'<div\s+class="config-option"', text)[1:]
    for chunk in chunks:
        title = re.search(r'config-option__title"\s*>(.*?)</label>', chunk, re.S)
        idx = re.search(r'set_cur_config_index\((\d+)\)', chunk)
        help_text = helps.get(int(idx.group(1)), "") if idx else ""
        help_text = "\n".join(line for line in re.split(r'<br\s*/?>', help_text) if "{{" not in line)
        bindings = {}
        for tag in re.findall(r'<input\b.*?/?>', chunk, re.S):
            a = attrs(tag)
            if a.get("type") == "radio" and "data-checked" in a:
                b = bindings.setdefault(a["data-checked"], ["radio", []])
                b[1].append((a.get("value", ""), clean_html(labels.get(a.get("id", ""), a.get("value", "")))))
            elif a.get("type") == "range" and "data-value" in a:
                unit = re.search(r'\{\{\s*%s\s*\}\}([^<]*)<' % re.escape(a["data-value"]), chunk)
                bindings[a["data-value"]] = ["range", (float(a.get("min", 0)), float(a.get("max", 100)),
                                                       unit.group(1).strip() if unit else "")]
        for binding, (kind, spec) in bindings.items():
            rows.append((binding, kind, clean_html(title.group(1)) if title else binding, spec, clean_html(help_text)))
    return rows

# ---- schema ---------------------------------------------------------------------------------------------------------


def fmt(v):
    return str(int(v)) if float(v).is_integer() else ("%g" % v)


def category(key, stem, game):
    if key in game.get("category", {}):
        return game["category"][key]
    if stem in FILE_CATEGORY:
        return FILE_CATEGORY[stem]
    if stem == "general":
        return "controls" if any(h in key for h in CONTROLS_HINTS) else "gameplay"
    return "enhancements"


def main(checkout, module):
    game = GAMES.get(module)
    if game is None:
        sys.exit(f"no curation for {module} in {__file__}")
    filtered = {**FILTERED, **game.get("filtered", {})}
    stored, _ = storage(checkout)
    ui_text = read_source(os.path.join(checkout, "src", "ui", "ui_config.cpp"))
    menu_dir = os.path.join(checkout, "assets", "config_menu")
    # The menu's tab bar (assets/config_menu.rml: <tab id="tab_<file>"><div>Label</div>), in its order (owner
    # 2026-11-15: our tabs copy the original menu's names and order).
    bar = read_source(os.path.join(checkout, "assets", "config_menu.rml"))
    tabs = {m.group(1): (n, m.group(2).strip()) for n, m in
            enumerate(re.finditer(r'<tab\b[^>]*\bid="tab_(\w+)"[^>]*>\s*<div>([^<]*)</div>', bar))}
    options, skipped = [], []
    for rml in sorted(os.listdir(menu_dir)):
        if not rml.endswith(".rml"):
            continue
        if rml in SKIP_FILES:
            skipped.append(f"{rml} ({SKIP_FILES[rml]})")
            continue
        for binding, kind, title, spec, help_text in menu_rows(os.path.join(menu_dir, rml)):
            if binding not in stored:
                skipped.append(f"{rml}:{binding} (in the menu but never saved by the port: nothing to apply through)")
                continue
            stem, jkey, dflt = stored[binding]
            key = f"{stem}.{jkey}"
            if key in filtered:
                skipped.append(f"{key} ({filtered[key]})")
                continue
            if any(o["key"] == key for o in options):
                continue
            if dflt is None:
                dflt = reset_default(ui_text, binding)
            entry = {"key": key, "label": game.get("labels", {}).get(key) or LABELS.get(key) or title,
                     "detail": game.get("details", {}).get(key) or DETAILS.get(key) or help_text,
                     "category": category(key, stem, game)}
            entry["tab"] = tabs.get(rml[:-4], (99, rml[:-4].title()))[1]
            if key in CHOICES:
                entry["values"] = [{"value": v, "label": l} for v, l in CHOICES[key]]
                entry["default"] = dflt or CHOICES[key][0][0]
            elif kind == "radio":
                entry["values"] = [{"value": v, "label": l} for v, l in spec]
                values = [v for v, _ in spec]
                if dflt not in values:
                    print(f"{key}: default {dflt} not among {values}; using the first", file=sys.stderr)
                    dflt = values[0]
                entry["default"] = dflt
            else:
                lo, hi, unit = spec
                if hi <= lo:
                    skipped.append(f"{key} (no range)")
                    continue
                step = PERCENT_STEP if unit == "%" and hi - lo >= 50 else 1
                entry.update(min=lo, max=hi, step=step, default=dflt if dflt is not None else fmt(lo))
                if unit:
                    entry["unit"] = unit
            if key in game.get("default_overrides", {}):
                entry["default"] = game["default_overrides"][key]
                entry["force"] = True
            options.append(entry)
    for s in skipped:
        print(f"filtered: {s}", file=sys.stderr)
    order = {label: n for n, label in tabs.values()}
    options.sort(key=lambda e: order.get(e["tab"], 99))
    json.dump({"format": 1, "module": module, "options": options}, sys.stdout, indent=1, ensure_ascii=False)
    print()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1])
    main(sys.argv[1], sys.argv[2])
