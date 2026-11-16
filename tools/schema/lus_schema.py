#!/usr/bin/env python3
"""Generates a libultraship port's settings_schema.json (docs/planning/MENU.md, design of 2026-11-14: each game's own
settings, all of them, in one consistent launcher UI) from the CVar widgets of the port's own ImGui menu.

    tools/schema/lus_schema.py <module id> <port checkout> > settings_schema.json

Each module's build.sh runs it into $PKG/data/, so labels, tooltips, ranges, choices and defaults always match the
pinned upstream. The reading itself is in lus_parse.py (both menu styles of the family). This file holds what the
menu source can't tell us, in one place:
- PORTS: which files hold each port's menu, and per-port curation;
- FILTER_*: the desktop-only and developer items (the MENU.md filter, the only one): window/fullscreen, vsync and
  refresh-rate matching, multi-viewports, the ImGui menu's own look and keys, cursor, focus handling, dev tools,
  networking (built with USE_NETWORKING=OFF), the randomizer (seed generation needs the port's own randomizer UI);
- OWN (optional "own" per port, none left since 2026-11-15): CVars a module option of our own drives, left out so
  the same switch doesn't appear twice. The skips that used to be here are now plain port options with the port's
  own defaults (owner 2026-11-15: keep the ports' defaults; only input defaults are ours);
- STANDARD_CHOICES: friendlier choices for the family's shared graphics CVars (frame rate, render scale, MSAA)
  instead of a 1..360 slider;
- DEFAULT_OVERRIDES: where the port's default is wrong for a handheld; always sent ("force").

Values are strings in the port's own CVar terms. The value's shape tells the shim which CVar setter to use
(modules/common/lus/lus_shim.cpp): a value with a '.' is a float (CVarSetFloat), an integer is an int
(CVarSetInteger), anything else a string (CVarSetString). That's why float CVars always get a '.' here ("1.0", never
"1"), even for whole numbers: libultraship's CVarGetFloat returns the default for a CVar stored as an integer.
Each option also carries "cvar_type" (int/float) for humans and future tools, and "group" (the port's menu
sidebar / section) for grouping in our menu.
"""
import glob
import pathlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lus_parse import Port, parse_data_driven, parse_immediate  # noqa: E402

# --- What the filter drops (MENU.md "Filtered out") -----------------------------------------------------------------

FILTER_MENUS = {"Dev Tools", "Developer", "Network", "Randomizer", "About", "Assets"}
FILTER_SIDEBARS = {"Console", "Stats", "Gfx Debugger", "Input Viewer", "Mod Menu", "Presets", "Crowd Control",
                   "Scene Visibility"}
FILTER_CVAR_PATTERNS = [
    r'\.Menu\.', r'^gSettings\.Menu', r'ControlNav$',  # the port's ImGui menu (disabled in direct mode, patch 0002)
    r'ImGuiScale', r'CursorVisibility', r'MenuBar',
    r'Vsync', r'MatchRefreshRate', r'FpsMatchDisplay',  # desktop refresh-rate controls
    r'WindowedFullscreen', r'Fullscreen', r'MultiViewports',  # windows
    r'RenderParallelization', r'ExtraLatencyThreshold', r'SimulatedInputLag',  # DX11 / dev-only
    r'^gOpenWindows', r'^gDeveloperTools', r'LogLevel', r'SwitchPerf', r'Debug',
    r'AllowBackgroundInputs', r'FocusControl\.', r'AlternateAssetsHotkey',  # desktop focus and keyboard hotkeys
    r'^gAdvancedResolution\.(?!Overscan)',  # the resolution editor window (its overscan crop stays)
    r'SaveImport|SaveConvertSlot',  # 2Ship's save import tool: slot pickers for a button we don't have
]
FILTER_KINDS = {"color"}

# --- The family's shared graphics CVars: friendlier choices ----------------------------------------------------------


def standard_choices(cvar, w):
    """Choices for the libultraship graphics CVars every port has, by CVar name suffix."""
    if cvar.endswith('InterpolationFPS'):
        lo = int(_num(w, 'Min', 20))
        return [(str(lo), f"Original ({lo} fps)")] + [(str(v), f"{v} fps") for v in (60, 90, 120) if v > lo]
    if cvar.endswith('MSAAValue'):
        return [("1", "Off"), ("2", "2x"), ("4", "4x"), ("8", "8x")]
    if cvar.endswith('InternalResolution'):
        return [("0.5", "50%"), ("0.75", "75%"), ("1.0", "100% (screen)"), ("1.5", "150%"), ("2.0", "200%")]
    return None


# Our handheld defaults for those. Empty since 2026-11-15 (owner: every upstream default except controller
# mapping); it used to force 60 fps interpolation.
STANDARD_DEFAULTS = {}

# --- Per-port tables ------------------------------------------------------------------------------------------------

PORTS = {
    "ghostship": dict(
        menus=["src/port/ui/GhostshipMenu*.cpp"],
    ),
    "lighthouse": dict(
        menus=["src/port/UI/LighthouseMenu*.cpp"],
        # Our modern control scheme baseline (plm_options.cpp) owns Controls.Scheme; the menu keeps the others.
        filter={"gSettings.Controls.Scheme"},
    ),
    "starship": dict(
        menus=["src/port/ui/ImguiUI.cpp"],
        # gTextureFilter: an unlabeled combobox (the label is an ImGui::Text above it); gVoiceLanguage: EU ROMs only
        # (the module plays the US 1.1 ROM).
        filter={"gTextureFilter", "gVoiceLanguage"},
        labels={"gInterpolationFPS": "Frame rate", "gMSAAValue": "Anti-aliasing (MSAA)"},
        # The FPS slider's minimum is a local variable (minFps = 30, "FPS: Original (30)"), invisible to the reader.
        choices={"gInterpolationFPS": [("30", "Original (30 fps)"), ("60", "60 fps"), ("90", "90 fps"),
                                       ("120", "120 fps")]},
    ),
    "shipwright": dict(
        menus=["soh/soh/SohGui/SohMenu*.cpp"],
    ),
    "2ship2harkinian": dict(
        menus=["mm/2s2h/BenGui/BenMenu.cpp"],
    ),
    "spaghettikart": dict(
        menus=["src/port/ui/PortMenu.cpp"],
    ),
    "paperboat": dict(
        menus=["src/port/ui/PaperboatMenu*.cpp"],
        # PaperBoat draws its own touch controls on Android, on by default; the launcher is controller-first, so
        # ours is Off (it was a hard-coded baseline in plm_options.cpp before the schema).
        defaults={"gTouchControls.Enabled": "0"},
    ),
    "battleship": dict(
        menus=["port/gui/PortMenu.cpp"],
    ),
}

SIDEBAR_CATEGORY = [
    (re.compile(r'^Audio$'), 'audio'),
    (re.compile(r'^(Controls|Controller|Control Enhancements|Camera)$'), 'controls'),
    (re.compile(r'^(Graphics|HUD|Status Bar|Overlay|Cosmetics)$'), 'graphics'),
    (re.compile(r'^(Gameplay|Difficulty|Difficulty Options|Cheats|Rulesets|Modes|Extra Modes|Time Savers|'
                r'Skips & Speed-ups|Cutscenes)$'), 'gameplay'),
    # Sidebars that are clearly the port's extras (fixes, restorations, QoL...) stay on Enhancements even when a
    # CVar name below mentions a camera or the HUD.
    (re.compile(r'^(Fixes|Restoration|Restorations|Quality of Life|Items|Items/Songs|Minigames|Saving|'
                r'Tooie Backports|Accessibility|Timers|Notifications)$'), 'enhancements'),
]
CVAR_CATEGORY = [
    (re.compile(r'Volume|Audio\.'), 'audio'),
    (re.compile(r'Camera|FreeLook|Gyro|Rumble|Invert|TouchControls|Controls\.'), 'controls'),
    (re.compile(r'InternalResolution|MSAA|InterpolationFPS|TextureFilter|Widescreen|AspectRatio'), 'graphics'),
]


def category(w, cvar):
    """Our tab for a widget: the port's own sidebar (or section, or top menu) when it says, else what the CVar is
    about; Settings › General and the like go to Enhancements."""
    for name in (w.get('sidebar') or '', w.get('section') or '', w.get('menu') or ''):
        for pat, cat in SIDEBAR_CATEGORY:
            if pat.match(name):
                return cat
    for pat, cat in CVAR_CATEGORY:
        if pat.search(cvar):
            return cat
    return 'enhancements'


def _num(w, key, default):
    v = w['_port'].number(w['opts'][key]) if key in w.get('opts', {}) else None
    return default if v is None else v


def clean_label(label):
    """The port's label without printf parts ("Master Volume: %d %%" -> "Master Volume") or ImGui ids."""
    label = label.split('##')[0].replace('\n', ' ')
    label = re.sub(r'[:\s]*%[-+ #0-9.]*[a-z%].*$', '', label)
    return re.sub(r'\s+', ' ', label).strip().rstrip(':').strip()


def unit_from(fmt):
    """The unit a slider's format prints after the number: "%d frames" -> " frames", "%d%%" -> "%"."""
    if not fmt:
        return ''
    m = re.search(r'%[-+ #0-9.]*[dif](.*)$', fmt)
    return m.group(1).replace('%%', '%').rstrip() if m else ''


def ffmt(v):
    """A float CVar value, always with a '.' (see the module docstring)."""
    s = ('%.4f' % v).rstrip('0')
    return s + '0' if s.endswith('.') else s


def float_values(w, lo, hi, step, default, pct, fmt):
    span = hi - lo
    if span <= 0:
        return [(ffmt(lo), ffmt(lo))]
    n = max(1, round(span / step))
    # At most ~20 steps: a menu row stepped with left/right, not a mouse slider.
    stride = 1
    for s in (1, 2, 4, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000):
        stride = s
        if n / s <= 20:
            break
    vals = [lo + i * step for i in range(0, n + 1, stride)]
    if abs(vals[-1] - hi) > 1e-6:
        vals.append(hi)
    # The port's default off the stride grid (Camera Speed 32 on a 1..100 slider): in place, so stepping through the
    # values stays in order (the launcher would otherwise put a missing default first).
    if lo <= default <= hi and all(abs(v - default) > 1e-6 for v in vals):
        vals = sorted(vals + [default])
    unit = unit_from(fmt)
    out = []
    for v in vals:
        if pct:
            lab = '%d%%' % round(v * 100)
        else:
            lab = ('%.2f' % v).rstrip('0').rstrip('.') + (unit if unit != '%' else '%')
        out.append((ffmt(v), lab))
    return out


def build(module, root):
    cfg = PORTS[module]
    port = Port(root)
    files = sorted(f for pat in cfg['menus'] for f in glob.glob(os.path.join(root, pat)))
    if not files:
        sys.exit(f"lus_schema: no menu files for {module} in {root}")
    immediate = any('UIWidgets::BeginMenu' in open(f, errors='replace').read() for f in files)
    widgets = (parse_immediate if immediate else parse_data_driven)(port, files)
    skip_cvars = set(cfg.get('own', ())) | set(cfg.get('filter', ()))
    seen = set()
    options, dropped = [], []
    for w in widgets:
        w['_port'] = port
        cvar = w.get('cvar')
        where = f"{w['file']}:{w['line']}"
        if not cvar:
            print(f"skipped (CVar unresolved): {where} {w.get('label')!r} {w.get('cvar_expr')!r}", file=sys.stderr)
            continue
        if cvar in seen:
            continue
        seen.add(cvar)
        if (w.get('menu') in FILTER_MENUS or w.get('sidebar') in FILTER_SIDEBARS or w['kind'] in FILTER_KINDS
                or any(re.search(p, cvar) for p in FILTER_CVAR_PATTERNS) or cvar in skip_cvars):
            dropped.append(cvar)
            continue
        opts = w.get('opts', {})
        label = cfg.get('labels', {}).get(cvar) or clean_label(w.get('label') or '') or cvar
        tip = port.string(opts['Tooltip']) if 'Tooltip' in opts else None
        o = {"key": cvar, "label": label, "detail": re.sub(r'\s+', ' ', tip or '').strip(),
             "category": category(w, cvar),
             "group": ' / '.join(x for x in (w.get('sidebar') or w.get('menu'), w.get('section')) if x)}
        # The port's own place for it (owner 2026-11-15: our tabs copy the original menu's names and order): the
        # sidebar entry is the tab, the top menu (Settings / Enhancements) groups tabs, the separator text is a
        # section header inside the tab.
        o["tab"] = w.get('sidebar') or w.get('menu') or 'Settings'
        if w.get('sidebar') and w.get('menu'):
            o["tab_group"] = w['menu']
        if w.get('section'):
            o["section"] = w['section']
        choices = cfg.get('choices', {}).get(cvar) or standard_choices(cvar, w)
        if choices:
            o["values"] = [{"value": v, "label": l} for v, l in choices]
            o["cvar_type"] = "float" if '.' in choices[0][0] else "int"
            # No DefaultValue: UIWidgets' own default (1.0 for float sliders, 1 for int sliders: full resolution,
            # MSAA off).
            d = _num(w, 'DefaultValue', 1)
            o["default"] = ffmt(d) if o["cvar_type"] == "float" else str(int(d))
            if cvar.endswith('InterpolationFPS') and o["label"] in ("Current FPS", cvar):
                o["label"] = "Frame rate"  # the port's label reads as a readout ("Current FPS: 60")
            for suffix, value in STANDARD_DEFAULTS.items():
                if cvar.endswith(suffix):
                    o["default"], o["force"] = value, True
        elif w['kind'] == 'checkbox':
            o["values"] = [{"value": "1", "label": "On"}, {"value": "0", "label": "Off"}]
            o["cvar_type"] = "int"
            o["default"] = "1" if _num(w, 'DefaultValue', 0) else "0"
        elif w['kind'] == 'combobox':
            name = opts.get('ComboMap') or opts.get('ComboVec')
            table = port.table(name, files) if name else None
            if not table:
                print(f"skipped (no choices for {name!r}): {where} {cvar}", file=sys.stderr)
                continue
            o["values"] = [{"value": str(k), "label": l} for k, l in table]
            o["cvar_type"] = "int"
            d = _num(w, 'DefaultIndex', 0)
            o["default"] = str(int(d)) if any(k == int(d) for k, _ in table) else str(table[0][0])
        elif w['kind'] == 'int':
            lo, hi = int(_num(w, 'Min', 1)), int(_num(w, 'Max', 10))
            step = int(_num(w, 'Step', 1)) or 1
            if 'Volume' in cvar and lo == 0 and hi == 100 and step == 1:
                step = 5
            o.update(min=lo, max=hi, step=step)
            unit = unit_from(port.string(opts['Format']) if 'Format' in opts else '')
            if unit:
                o["unit"] = unit if unit == '%' else ' ' + unit.strip()
            o["cvar_type"] = "int"
            o["default"] = str(int(_num(w, 'DefaultValue', 1)))
        elif w['kind'] == 'float':
            lo, hi = float(_num(w, 'Min', 0.01)), float(_num(w, 'Max', 10.0))
            step = float(_num(w, 'Step', 0.01)) or 0.01
            d = float(_num(w, 'DefaultValue', 1.0))
            pct = 'IsPercentage' in opts or '%%' in (port.string(opts.get('Format', '""')) or '')
            fmt = port.string(opts['Format']) if 'Format' in opts else ''
            o["values"] = [{"value": v, "label": l}
                           for v, l in float_values(w, lo, hi, step, d, pct and 'IsPercentage' in opts, fmt)]
            if pct and 'IsPercentage' not in opts:
                # "%.0f%%" over a 0..100 float (Starship-style volumes stored 0..1 use IsPercentage instead).
                for v in o["values"]:
                    v["label"] = v["label"].rstrip('%') + '%'
            o["cvar_type"] = "float"
            o["default"] = ffmt(d)
        else:
            continue
        if cvar in cfg.get('defaults', {}):
            o["default"], o["force"] = cfg['defaults'][cvar], True
        options.append(o)
    # Menu files are read in file-name order (SohMenuEnhancements.cpp before SohMenuSettings.cpp), but the ports'
    # menu bar always starts with Settings, then Enhancements, then the rest: restore that order (stable sort, so
    # sidebars and rows keep their source order inside a menu).
    rank = {"Settings": 0, "Enhancements": 1}
    first_seen = {}
    for o in options:
        first_seen.setdefault(o.get("tab_group", o["tab"]), len(first_seen))
    options.sort(key=lambda o: (rank.get(o.get("tab_group", o["tab"]), 2), first_seen[o.get("tab_group", o["tab"])]))
    print(f"{module}: {len(options)} options, {len(dropped)} filtered: {' '.join(dropped)}", file=sys.stderr)
    out = {"format": 1, "module": module, "source": "libultraship menu (tools/schema/lus_schema.py)",
           "options": options}
    profiles = build_profiles(module, root, options)
    if profiles:
        out["profiles"] = profiles
    return out


# Profiles (owner 2026-11-16): a named set of option values the player picks and applies in one go from our Settings
# tab ("Modern": many quality-of-life options at once). Taken from the port's own curated presets, so they follow
# upstream's choices and updates rather than ours (port-defaults-except-input: a profile is a layer on top of the
# port's defaults, never edited into them). Only CVars that are options in this schema are kept (a preset also sets
# windows, cosmetics and trackers we don't show); values use the option's own value format.
#  - SoH ships preset files (soh/assets/custom/presets): "Curated First Time Vanilla" and "Curated Returning Vanilla",
#    whose "enhancements" block is what its preset menu applies for Enhancements.
#  - 2S2H has one built-in "Curated" preset (PresetManager.cpp, curatedPresetJ), its developers' own: it also turns on
#    developer tools, a cheat and joke cosmetics. We take its gEnhancements and gFixes only. Mirroring SoH's pair (OoT
#    and MM echo each other, owner 2026-11-16), the first-playthrough profile leaves out the story skips: the intro,
#    story cutscenes and the first cycle.
MM_RETURNING_ONLY = {"gEnhancements.Cutscenes.SkipIntroSequence", "gEnhancements.Cutscenes.SkipStoryCutscenes",
                     "gEnhancements.Cutscenes.SkipFirstCycle", "gEnhancements.Cutscenes.SkipToFileSelect"}


def _flatten(obj, prefix=""):
    out = {}
    for k, v in (obj or {}).items():
        if isinstance(v, dict):
            out.update(_flatten(v, prefix + k + "."))
        elif v is not None:
            out[prefix + k] = v
    return out


def _profile(pid, label, detail, values, options):
    by_key = {o["key"]: o for o in options}
    kept = {}
    for cvar, v in sorted(values.items()):
        o = by_key.get(cvar)
        if o is None:
            continue
        text = ffmt(float(v)) if o.get("cvar_type") == "float" else str(int(v)) if isinstance(v, (int, float, bool)) else str(v)
        if "values" in o:
            ok = any(x["value"] == text for x in o["values"])
        else:  # a range (min/max/step), stepped by the app (PortSchema.rangeValues)
            ok = float(o.get("min", 0)) <= float(text) <= float(o.get("max", 0))
        if ok:
            kept[cvar] = text
    print(f"{pid}: {len(kept)} of {len(values)} preset values are menu options", file=sys.stderr)
    return {"id": pid, "label": label, "detail": detail, "values": kept}


def build_profiles(module, src, options):
    root = pathlib.Path(src)
    if module == "shipwright":
        d = root / "soh/assets/custom/presets"
        out = []
        for pid, label, name, detail in [
            ("modern", "Modern", "Enhancements - Curated First Time Vanilla.json",
             "Ship of Harkinian's curated enhancements for a first playthrough: quality-of-life options, time savers "
             "and bug fixes, without skipping the story."),
            ("modern-returning", "Modern: returning player", "Enhancements - Curated Returning Vanilla.json",
             "Ship of Harkinian's curated enhancements for players who know the game: the Modern set plus more "
             "skips."),
        ]:
            f = d / name
            if f.is_file():
                out.append(_profile(pid, label, detail, _flatten(json.loads(f.read_text())["blocks"].get("enhancements")), options))
        return out
    if module == "2ship2harkinian":
        cpp = (root / "mm/2s2h/PresetManager/PresetManager.cpp").read_text()
        m = re.search(r'curatedPresetJ = R"\((.*?)\)"_json', cpp, re.S)
        if not m:
            return []
        cvars = json.loads(m.group(1))["CVars"]
        values = _flatten({k: cvars[k] for k in ("gEnhancements", "gFixes") if k in cvars})
        first = {k: v for k, v in values.items() if k not in MM_RETURNING_ONLY}
        return [
            _profile("modern", "Modern",
                     "2 Ship 2 Harkinian's curated enhancements and fixes for a first playthrough: quality-of-life "
                     "options and time savers, without skipping the story or the first cycle.", first, options),
            _profile("modern-returning", "Modern: returning player",
                     "2 Ship 2 Harkinian's curated enhancements and fixes in full, for players who know the game: "
                     "also skips the intro, story cutscenes and the first cycle.", values, options),
        ]
    return []


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in PORTS:
        sys.exit(f"usage: {sys.argv[0]} <{'|'.join(PORTS)}> <port checkout>")
    json.dump(build(sys.argv[1], sys.argv[2]), sys.stdout, indent=1, ensure_ascii=False)
    print()
