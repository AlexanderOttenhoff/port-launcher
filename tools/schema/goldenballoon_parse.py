"""Parses Golden Balloon's video-config tables (platform/video_config.c) into Python data. Shared by the schema
generator; run directly to list the keys."""
import re, sys

def _strings(s):
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', s))

def parse(src):
    text = open(src).read()
    body = text[text.index('s_schema[MDKR_VIDEO_KEY_COUNT] = {'):]
    body = body[:body.index('\n};')]
    entries = {}
    for m in re.finditer(r'\[(MDKR_(?!VIDEO_KEY_COUNT)[A-Z0-9_]+)\]\s*=\s*\{(.*?)\n    \}', body, re.S):
        enum, inner = m.group(1), m.group(2)
        inner = re.sub(r'/\*.*?\*/', '', inner, flags=re.S)
        parts = re.match(r'\s*"([^"]+)",\s*"([^"]*)",\s*(MDKR_VIDEO_TYPE_\w+),\s*(MDKR_VIDEO_SCOPE_\w+),\s*([-\d.]+)f,\s*([-\d.]+)f,\s*(.*),\s*(MDKR_VIDEO_CAT_\w+)\s*$', inner, re.S)
        if not parts:
            print("unparsed", enum, file=sys.stderr); continue
        name, env, typ, scope, mn, mx, rest, cat = parts.groups()
        # rest = label string(s), help string(s): split at the first string boundary that ends the label
        strs = re.findall(r'"((?:[^"\\]|\\.)*)"', rest)
        label = strs[0] if strs else name
        helptext = ''.join(strs[1:])
        entries[enum] = dict(name=name, type=typ.split('_')[-1].lower(), scope=scope.split('_')[-1].lower(),
                             min=float(mn), max=float(mx), label=label, help=helptext, cat=cat.split('CAT_')[-1].lower())
    def table(start, cast):
        t = text[text.index(start):]
        t = t[:t.index('\n};')]
        out = {}
        for m in re.finditer(r'\[(MDKR_[A-Z0-9_]+)\]\s*=\s*\{([^}]*)\}', t):
            vals = [v.strip() for v in m.group(2).split(',')]
            out[m.group(1)] = [cast(v) for v in vals[:3]]
        return out
    num = table('s_preset[MDKR_VIDEO_KEY_COUNT][3] = {', lambda v: float(v.rstrip('f')) if re.match(r'^-?[\d.]+f?$', v) else None)
    txt = table('s_preset_text[MDKR_VIDEO_KEY_COUNT][3] = {', lambda v: None if v in ('NULL', '') else v.strip('"'))
    for k, e in entries.items():
        e['preset'] = num.get(k)
        e['preset_text'] = txt.get(k)
    # Explicit defaults in mdkr_video_config_defaults() override the Restored preset (or stand in where a key has no
    # preset row): "values[KEY].number = 1.0f;" and "snprintf(config->values[KEY].text, ..., "%s", "value");".
    body = text[text.index('void mdkr_video_config_defaults('):]
    body = body[:body.index('\n}\n')]
    for m in re.finditer(r'values\[(MDKR_[A-Z0-9_]+)\]\.number\s*=\s*(-?[\d.]+)f?;', body):
        if m.group(1) in entries:
            entries[m.group(1)]['default'] = float(m.group(2))
    for m in re.finditer(r'values\[(MDKR_[A-Z0-9_]+)\]\.text\),\s*"%s",\s*"([^"]*)"\)', body):
        if m.group(1) in entries:
            entries[m.group(1)]['default'] = m.group(2)
    return entries

if __name__ == '__main__':
    for k, e in parse(sys.argv[1]).items():
        print(f"{e['cat']:12} {e['type']:6} {e['scope']:7} {e['name']:32} {e['min']}-{e['max']} R={e['preset'] and e['preset'][1]} T={e['preset_text'] and e['preset_text'][1]} | {e['label']}")
