#!/usr/bin/env python3
"""Extracts the settings menus of the ports the launcher packages into one Markdown inventory, for designing the
launcher's own unified menu (docs/planning/MENU.md: the ports' menus get replaced).

    extract_port_menus.py <build/upstream dir> <JFG-Android checkout or ""> > docs/planning/PORT_MENUS_RAW.md

Parses each menu's source as written (no build needed): libultraship UIWidgets builders (Lighthouse, Ghostship),
Starship's ImGui menu, GoldenEye's options overlay rows, Perfect Dark's Extended menu items, the recomp frontends'
add_*_option calls (shared RecompFrontend tabs and each game's config.cpp) and Jet Force Gemini's RmlUi pages.
Static extraction: labels/tooltips built at runtime are missed, and a few rows may be dev-only.
"""
import os
import re
import sys

STR = r'"((?:[^"\\]|\\.)*)"'


def lits(text):
    """Concatenated C string literals at the start of text (adjacent literals join)."""
    out, pos = [], 0
    while True:
        m = re.compile(r'\s*' + STR).match(text, pos)
        if not m:
            break
        out.append(m.group(1))
        pos = m.end()
    return "".join(out).replace("\\n", " ").replace('\\"', '"').strip()


def clean(s, limit=220):
    s = re.sub(r"<[^>]+>", "", s)            # recomp colour markup
    s = re.sub(r"\s+", " ", s).strip()
    return s if len(s) <= limit else s[: limit - 1] + "…"


def row(label, kind, key, tip):
    bits = [f"**{clean(label, 80)}**"]
    meta = ", ".join(x for x in (kind, f"`{key}`" if key else "") if x)
    if meta:
        bits.append(f"({meta})")
    if tip:
        bits.append(f"— {clean(tip)}")
    return "- " + " ".join(bits)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


# --------------------------------------------------------------- libultraship UIWidgets (Lighthouse, Ghostship) ---
def lus_widgets(files):
    lines, section = [], None
    for path in files:
        text = read(path)
        tokens = []
        for m in re.finditer(r'(?:WidgetPath\s+)?path\s*=\s*\{\s*' + STR + r'\s*,\s*' + STR, text):
            tokens.append((m.start(), "path", f"{m.group(1)} › {m.group(2)}"))
        for m in re.finditer(r'AddWidget\(\s*path\s*,\s*' + STR + r'\s*,\s*(WIDGET_\w+)', text):
            tokens.append((m.start(), "widget", (m.group(1), m.group(2), m.end())))
        tokens.sort()
        for i, (pos, kind, val) in enumerate(tokens):
            if kind == "path":
                if val != section:
                    section = val
                    lines.append(f"\n#### {section}\n")
                continue
            label, wtype, end = val
            if wtype in ("WIDGET_SEPARATOR_TEXT",):
                lines.append(f"\n*{clean(label, 80)}*")
                continue
            if wtype in ("WIDGET_SEPARATOR", "WIDGET_SPACER"):
                continue
            nxt = tokens[i + 1][0] if i + 1 < len(tokens) else len(text)
            body = text[end:nxt]
            cvar = re.search(r'\.CVar\((.*?)\)\s*\n', body)
            tip = re.search(r'Tooltip\(', body)
            tiptext = lits(body[tip.end():]) if tip else ""
            short = wtype.replace("WIDGET_", "").replace("CVAR_", "").lower()
            lines.append(row(label, short, cvar.group(1).strip() if cvar else "", tiptext))
    return lines


# --------------------------------------------------------------- Starship (ImguiUI.cpp) ---
def starship(path):
    text = read(path)
    lines, last = [], None
    tokens = []
    for m in re.finditer(r'ImGui::BeginMenu\(\s*' + STR, text):
        tokens.append((m.start(), "menu", m.group(1)))
    for m in re.finditer(r'ImGui::SeparatorText\(\s*' + STR, text):
        tokens.append((m.start(), "sep", m.group(1)))
    for m in re.finditer(r'UIWidgets::(\w+)\(\s*' + STR + r'\s*,\s*' + STR, text):
        tokens.append((m.start(), "w", (m.group(1), m.group(2), m.group(3), m.end())))
    tokens.sort()
    for i, (pos, kind, val) in enumerate(tokens):
        if kind == "menu":
            if val != last:
                last = val
                lines.append(f"\n#### {val}\n")
        elif kind == "sep":
            lines.append(f"\n*{val}*")
        else:
            fn, label, cvar, end = val
            nxt = tokens[i + 1][0] if i + 1 < len(tokens) else len(text)
            tip = re.search(r'\.tooltip\s*=\s*', text[end:nxt])
            tiptext = lits(text[end:nxt][tip.end():]) if tip else ""
            lines.append(row(label, fn.replace("CVar", "").lower(), cvar, tiptext))
    return lines


# --------------------------------------------------------------- GoldenEye (optionsoverlay.c rows) ---
def goldeneye(path):
    text = read(path)
    helps = dict(re.findall(r'\{\s*' + STR + r'\s*,\s*' + STR + r'\s*\}', text))
    lines = []
    for m in re.finditer(r'static\s+(?:const\s+)?(?:struct\s+)?Row\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\n\};', text, re.S):
        name, body = m.group(1), m.group(2)
        rows = re.findall(r'\{\s*\.key\s*=\s*' + STR + r'\s*,\s*\.label\s*=\s*' + STR + r'\s*,\s*\.kind\s*=\s*(\w+)', body)
        if not rows:
            continue
        for key, label, kind in rows:
            if key.startswith("__Sep") or key.startswith("__Back") or not label:
                continue
            if kind == "ROW_HEADER":
                lines.append(f"\n#### {label.title()}\n")
                continue
            lines.append(row(label, kind.replace("ROW_", "").lower(), "" if key.startswith("__") else key,
                             helps.get(key, "")))
    return lines


# --------------------------------------------------------------- Perfect Dark (optionsmenu.c Extended menu) ---
def perfect_dark(path):
    text = read(path)
    titles = {}
    for m in re.finditer(r'struct\s+menudialogdef\s+\w+\s*=\s*\{\s*MENUDIALOGTYPE_\w+\s*,\s*\(uintptr_t\)\s*' + STR +
                         r'\s*,\s*(\w+)', text):
        titles[m.group(2)] = m.group(1)
    lines = []
    for m in re.finditer(r'struct\s+menuitem\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\n\};', text, re.S):
        name, body = m.group(1), m.group(2)
        items = re.findall(r'MENUITEMTYPE_(\w+)\s*,[^{}]*?\(uintptr_t\)\s*' + STR, body)
        if not items:
            continue
        lines.append(f"\n#### {titles.get(name, name)}\n")
        for kind, label in items:
            if kind in ("SEPARATOR", "END"):
                continue
            lines.append(row(label, kind.lower(), "", ""))
    return lines


# --------------------------------------------------------------- recomp frontends (add_*_option) ---
def recomp_options(path):
    text = read(path)
    lines = []
    for m in re.finditer(r'add_(bool|enum|number|string)_option\(\s*([^,]+?)\s*,\s*' + STR + r'\s*,', text):
        kind, key, label = m.group(1), m.group(2), m.group(3)
        desc = lits(text[m.end():])
        lines.append(row(label, kind, key.split("::")[-1], desc))
    return lines


# --------------------------------------------------------------- Jet Force Gemini (RmlUi pages) ---
def rml_page(path):
    text = read(path)
    lines = []
    for m in re.finditer(r'<label class="config-option__title( config-group__title)?">(.*?)</label>', text):
        if m.group(1):
            lines.append(f"\n*{m.group(2)}*")
        else:
            lines.append(f"- **{m.group(2)}**")
    return lines


def section(title, note, body):
    out = [f"\n## {title}\n", note + "\n"] if note else [f"\n## {title}\n"]
    out += body if body else ["*(nothing extracted)*"]
    return out


def main():
    up = sys.argv[1]
    jfg = sys.argv[2] if len(sys.argv) > 2 else ""
    j = lambda *p: os.path.join(up, *p)
    out = ["# Port menus: raw inventory (generated)",
           "",
           "Generated by `tools/menus/extract_port_menus.py` from the pinned port sources; regenerate after updating a "
           "port. Each entry: **label** (widget type, setting key) — tooltip. Static extraction: runtime-built entries "
           "are missed. Curated notes: [PORT_MENUS.md](PORT_MENUS.md)."]
    lh = sorted(j("Lighthouse/src/port/UI", f) for f in os.listdir(j("Lighthouse/src/port/UI"))
                if f.startswith("LighthouseMenu") and f.endswith(".cpp"))
    out += section("Banjo-Kazooie — Lighthouse (libultraship)", "", lus_widgets(lh))
    gs = sorted(j("Ghostship/src/port/ui", f) for f in os.listdir(j("Ghostship/src/port/ui"))
                if f.startswith("GhostshipMenu") and f.endswith(".cpp"))
    out += section("Super Mario 64 — Ghostship (libultraship)", "", lus_widgets(gs))
    out += section("Star Fox 64 — Starship (libultraship, older menu)", "", starship(j("Starship/src/port/ui/ImguiUI.cpp")))
    out += section("GoldenEye 007 — PC port options overlay (F10)", "", goldeneye(j("goldeneye-pc-port/port/src/optionsoverlay.c")))
    out += section("Perfect Dark — PC port Extended options (in-game menu)", "", perfect_dark(j("perfect_dark/port/src/optionsmenu.c")))
    rf = j("Zelda64Recomp/lib/RecompFrontend/recompui/src/config")
    shared = []
    for f in ("ui_config_tab_graphics.cpp", "ui_config_tab_general.cpp"):
        shared += [f"\n#### {f.replace('ui_config_tab_', '').replace('.cpp', '').title()} tab\n"] + recomp_options(os.path.join(rf, f))
    out += section("Recomps — shared RecompFrontend tabs", "Graphics, Sound, Controls and Mods tabs are common to the recomp "
                   "frontends (Majora's Mask, Banjo, Banjo-Tooie, DK64); Controls is a per-N64-button remapping page.", shared)
    for title, path in (("Majora's Mask — Zelda64Recomp", "Zelda64Recomp/src/game/config.cpp"),
                        ("Banjo-Kazooie — BanjoRecomp (recomp, secondary)", "BanjoRecomp/src/game/config.cpp"),
                        ("Donkey Kong 64 — DK64 Recompiled", "Donkey-Kong-64-Recompiled/src/game/config.cpp")):
        out += section(title, "Game-specific options (in addition to the shared tabs).", recomp_options(j(path)))
    out += section("Banjo-Tooie — BanjoTooieRecompiled", "No game-specific options found: shared tabs only.", [])
    if jfg and os.path.isdir(os.path.join(jfg, "assets/config_menu")):
        body = []
        for f in ("jfg_general.rml", "jfg_gameplay.rml", "jfg_graphics.rml", "jfg_controls.rml", "jfg_profiles.rml",
                  "jfg_textures.rml"):
            p = os.path.join(jfg, "assets/config_menu", f)
            if os.path.exists(p):
                body += [f"\n#### {f.replace('jfg_', '').replace('.rml', '').title()}\n"] + rml_page(p)
        out += section("Jet Force Gemini — JFG Recompiled (RmlUi)", "", body)
    print("\n".join(out))


if __name__ == "__main__":
    main()
