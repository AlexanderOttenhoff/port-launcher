"""Extracts the CVar widgets of a libultraship port's own menu (docs/planning/MENU.md, design of 2026-11-14) into
plain Python data, for lus_schema.py. Run directly to list what a port's menu holds:

    tools/schema/lus_parse.py <port checkout> <menu file>...

Two menu styles exist in the libultraship family, and both are read here:
- data-driven (SoH's SohMenu and every port that copied it: Ghostship, Lighthouse, 2Ship, SpaghettiKart, PaperBoat,
  BattleShip): AddWidget(path, "Label", WIDGET_CVAR_CHECKBOX).CVar(CVAR_ENHANCEMENT("X")).Options(CheckboxOptions()
  .Tooltip("...").DefaultValue(true)), grouped by AddMenuEntry / AddSidebarEntry / separator text;
- immediate-mode (Starship's ImguiUI.cpp, the older style): UIWidgets::CVarCheckbox("Label", "gX", {.tooltip = ...})
  inside nested UIWidgets::BeginMenu("Settings") blocks.

This is a regex reader, not a C++ parser: it handles what these menus actually contain (string-literal labels, CVar
macros, numeric literals or enum constants for ranges/defaults, ComboMap/ComboVec tables in the same tree) and reports
anything it can't resolve on stderr instead of guessing. Why not compile and introspect the menu? It needs the whole
port (ImGui, the engine) built for the host; a regex pass over the pinned sources runs in a second inside build.sh.
"""
import glob
import os
import re
import sys
import warnings

# number() evaluates C expressions with Python's eval; things like "1 (2)" are rejected anyway, without the noise.
warnings.filterwarnings('ignore', category=SyntaxWarning)

# The target's preprocessor view: an Android build (Linux kernel, no Switch/Wii U/Windows/Apple). Anything else in an
# #if is treated as undefined (0), which matches how these ports guard optional desktop/console features.
TARGET_DEFINES = {"__ANDROID__": 1, "__linux__": 1, "ANDROID": 1, "__aarch64__": 1}

STR = r'"(?:[^"\\]|\\.)*"'


def strip_comments(text):
    """Removes // and /* */ comments outside string and char literals, keeping newlines (line numbers)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def _eval_if(expr, defines):
    expr = re.sub(r'defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)',
                  lambda m: ' 1 ' if (m.group(1) or m.group(2)) in defines else ' 0 ', expr)
    expr = expr.replace('&&', ' and ').replace('||', ' or ')
    expr = re.sub(r'!(?!=)', ' not ', expr)
    expr = re.sub(r'\b([A-Za-z_]\w*)\b',
                  lambda m: m.group(1) if m.group(1) in ('and', 'or', 'not') else str(defines.get(m.group(1), 0)), expr)
    expr = re.sub(r'(\d+)[uUlL]+', r'\1', expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}))
    except Exception:
        return False


def preprocess(text, defines=TARGET_DEFINES):
    """Blanks the lines in inactive #if branches for the Android target. Directive lines themselves are blanked too."""
    lines = text.split('\n')
    stack = []  # (parent_active, branch_active, taken)
    active = True
    out = []
    for line in lines:
        m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', line)
        if not m:
            out.append(line if active else '')
            continue
        d, rest = m.group(1), m.group(2).strip()
        if d in ('if', 'ifdef', 'ifndef'):
            cond = (rest.split()[0] in defines if d == 'ifdef' else rest.split()[0] not in defines if d == 'ifndef'
                    else _eval_if(rest, defines)) if rest else False
            stack.append((active, active and cond, cond))
            active = active and cond
        elif d == 'elif' and stack:
            parent, _, taken = stack[-1]
            cond = not taken and _eval_if(rest, defines)
            stack[-1] = (parent, parent and cond, taken or cond)
            active = parent and cond
        elif d == 'else' and stack:
            parent, _, taken = stack[-1]
            stack[-1] = (parent, parent and not taken, True)
            active = parent and not taken
        elif d == 'endif' and stack:
            active = stack.pop()[0]
        out.append('')
    return '\n'.join(out)


def balanced(text, i, open_c='(', close_c=')'):
    """Given text[i] == open_c, returns the index just past the matching close_c (skipping strings)."""
    depth, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            i = j + 1
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def split_args(s):
    """Splits a call's argument text at top-level commas."""
    args, depth, cur, i, n = [], 0, [], 0, len(s)
    while i < n:
        c = s[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == '\\' else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if ''.join(cur).strip():
        args.append(''.join(cur).strip())
    return args


def chain(text, i):
    """Reads a builder chain starting at text[i] ('.Name(args).Other(args)...'); returns ([(name, args)], end)."""
    calls = []
    n = len(text)
    while True:
        m = re.compile(r'\s*\.\s*(\w+)\s*\(').match(text, i)
        if not m:
            return calls, i
        start = m.end() - 1
        end = balanced(text, start)
        calls.append((m.group(1), text[start + 1:end - 1]))
        i = end
        if i >= n:
            return calls, i


def c_string(s):
    """The value of one or more adjacent C string literals, or None if s isn't only string literals."""
    s = s.strip()
    if not s or not re.fullmatch(r'(?:\s*' + STR + r'\s*)+', s):
        return None
    parts = re.findall(STR, s)
    raw = ''.join(p[1:-1] for p in parts)
    return (raw.replace('\\n', '\n').replace('\\t', ' ').replace('\\"', '"').replace("\\'", "'")
            .replace('\\\\', '\\'))


class Port:
    """Symbol tables for one port checkout: #defines, CMake-defined CVar names, enum constants, combo tables."""

    def __init__(self, root, extra_dirs=()):
        self.root = root
        self.obj, self.fn = {}, {}
        self.enums = {}
        self._tables = None
        headers = []
        skip = re.compile(r'/(build[^/]*|\.git|extern|vendor|_deps|tools/Torch|Torch|ZAPDTR|OTRExporter)/')
        for dirpath, dirnames, files in os.walk(root):
            # Relative to the checkout: the checkout itself lives under build/upstream, which must not match.
            if skip.search('/' + os.path.relpath(dirpath, root) + '/'):
                dirnames[:] = []
                continue
            for f in files:
                if f.endswith(('.h', '.hpp', '.inc')):
                    headers.append(os.path.join(dirpath, f))
        # Port headers before libultraship's, so a port's own definition wins over the library's default.
        headers.sort(key=lambda p: ('libultraship' in p, p))
        self.headers = headers
        self._cmake_cvars()
        for h in headers:
            try:
                text = strip_comments(open(h, errors='replace').read())
            except OSError:
                continue
            self._defines(text)
            if 'enum' in text:
                self._enums_from(text)

    def _cmake_cvars(self):
        # libultraship takes some CVar names from CMake (cmake/lus-cvars.cmake, libultraship/cmake/cvars.cmake),
        # passed as -D compile definitions: CVAR_INTERNAL_RESOLUTION, CVAR_MSAA_VALUE, CVAR_MENU_PAD_TOGGLE...
        files = sorted(glob.glob(os.path.join(self.root, 'CMakeLists.txt')) +
                       glob.glob(os.path.join(self.root, 'cmake', '*.cmake')) +
                       glob.glob(os.path.join(self.root, 'CMake', '*.cmake')) +
                       glob.glob(os.path.join(self.root, '*', 'CMakeLists.txt')) +
                       glob.glob(os.path.join(self.root, 'libultraship', 'cmake', '*.cmake')),
                       key=lambda p: ('libultraship' in p, p))
        sets = {}
        for f in files:
            for m in re.finditer(r'set\(\s*(CVAR_\w+)\s+"([^"]*)"', open(f, errors='replace').read()):
                sets.setdefault(m.group(1), m.group(2))
        for _ in range(5):
            for k, v in sets.items():
                sets[k] = re.sub(r'\$\{(\w+)\}', lambda m: sets.get(m.group(1), m.group(0)), v)
        for k, v in sets.items():
            self.obj.setdefault(k, '"%s"' % v)

    def _defines(self, text):
        text = text.replace('\\\n', ' ')
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(\w+)(\(([^)]*)\))?[ \t]*(.*)$', text, re.M):
            name, params, body = m.group(1), m.group(3), m.group(4).strip()
            # The regex only takes "(params)" when it touches the name, which is what makes a macro function-like.
            if m.group(2) is not None:
                self.fn.setdefault(name, ([p.strip() for p in params.split(',') if p.strip()], body))
            else:
                self.obj.setdefault(name, body)

    def _enums_from(self, text):
        for m in re.finditer(r'\benum\s+(?:class\s+|struct\s+)?(\w+)?\s*(?::\s*[\w:]+\s*)?\{', text):
            start = m.end() - 1
            body = text[start + 1:balanced(text, start, '{', '}') - 1]
            ename = m.group(1)
            value = -1
            for entry in split_args(body):
                em = re.match(r'(\w+)\s*(?:=\s*(.+))?$', entry.strip(), re.S)
                if not em:
                    continue
                if em.group(2) is not None:
                    v = self.number(em.group(2))
                    if v is None:
                        continue
                    value = v
                else:
                    value += 1
                self.enums.setdefault(em.group(1), value)
                if ename:
                    self.enums.setdefault(ename + '::' + em.group(1), value)

    def expand(self, expr, depth=0):
        """Macro-expands an expression (object- and function-like macros), leaving string literals alone."""
        if depth > 12:
            return expr
        out, changed, i = [], False, 0
        # Numbers are one token (with their f/u suffixes and decimals), or "120" would become "1 2 0".
        tokens = list(re.finditer(STR + r'|[A-Za-z_]\w*|\.?\d[\w.]*|::|\S', expr))
        k = 0
        while k < len(tokens):
            t = tokens[k].group(0)
            if t in self.fn and k + 1 < len(tokens) and tokens[k + 1].group(0) == '(':
                start = tokens[k + 1].start()
                end = balanced(expr, start)
                params, body = self.fn[t]
                args = split_args(expr[start + 1:end - 1])
                for p, a in zip(params, args):
                    body = re.sub(r'(?<![\w"])' + re.escape(p) + r'(?![\w"])', lambda _m, a=a: a, body)
                out.append(' ' + body + ' ')
                changed = True
                while k < len(tokens) and tokens[k].start() < end:
                    k += 1
                continue
            if t in self.obj and self.obj[t] and self.obj[t] != t:
                out.append(' ' + self.obj[t] + ' ')
                changed = True
            else:
                out.append(t if t.startswith('"') else ' ' + t + ' ')
            k += 1
        s = ''.join(out)
        return self.expand(s, depth + 1) if changed else s

    def string(self, expr, context=None, depth=0):
        """A string-valued expression (a CVar name, a label): literals and macros. None if unresolvable.
        context: the menu file's text, where a local "const char* x = ...;" can name the CVar (BattleShip)."""
        s = c_string(expr)
        if s is not None:
            return s
        e = self.expand(expr)
        e = re.sub(r'\(\s*const\s+char\s*\*\s*\)', '', e).strip()
        while e.startswith('(') and balanced(e, 0) == len(e):
            e = e[1:-1].strip()
        r = c_string(e)
        if r is None and depth < 3:
            # A getter ("enhancements::WidescreenCVarName()", BattleShip): the string its body returns.
            gm = re.fullmatch(r'\s*(?:[\w]+\s*::\s*)*(\w+)\s*\([^()]*\)\s*', expr)
            if gm:
                r = self.getter(gm.group(1))
            elif context is not None and re.fullmatch(r'\s*\w+\s*', expr):
                lm = re.search(r'\b' + re.escape(expr.strip()) + r'\s*=\s*([^;]+);', context)
                if lm:
                    r = self.string(lm.group(1), context, depth + 1)
        return r

    def getter(self, name):
        """The string a getter returns: its first return statement, which is a literal, a constant
        (constexpr const char* kX = "..."), or element 0 of a per-player array (kX[playerIndex]: player 1, the only
        player on a handheld)."""
        pat = re.compile(r'\b' + re.escape(name) + r'\s*\([^()]*\)\s*\{')
        for f in self._all_sources():
            try:
                text = open(f, errors='replace').read()
            except OSError:
                continue
            if name not in text:
                continue
            m = pat.search(text)
            if not m:
                continue
            body = text[m.end() - 1:balanced(text, m.end() - 1, '{', '}')]
            rets = re.findall(r'return\s+([^;]+);', body)
            if not rets:
                continue
            ret = rets[-1].strip()  # the last return is the normal path (earlier ones are range guards)
            if c_string(ret) is not None:
                return c_string(ret)
            cm = re.fullmatch(r'(\w+)(\s*\[[^\]]*\])?', ret)
            if not cm:
                continue
            dm = re.search(r'\b' + re.escape(cm.group(1)) + r'\s*(\[[^\]]*\])?\s*=\s*', text)
            if not dm:
                continue
            rest = text[dm.end():]
            if rest.startswith('{'):
                items = split_args(rest[1:balanced(rest, 0, '{', '}') - 1])
                return c_string(items[0]) if items else None
            sm = re.match(r'((?:' + STR + r'\s*)+)', rest)
            if sm:
                return c_string(sm.group(1))
        return None

    def number(self, expr):
        """A numeric expression: literals, macros, enum constants, casts, simple arithmetic. None if unresolvable."""
        e = self.expand(expr.strip())
        e = re.sub(r'static_cast\s*<[^>]*>', '', e)
        e = re.sub(r'\(\s*(?:u?int(?:8|16|32|64)_t|int|float|double|unsigned|uint8_t|s32|u32|s16|u16|u8|s8|f32)\s*\)',
                   '', e)
        e = re.sub(r'\btrue\b', '1', e)
        e = re.sub(r'\bfalse\b', '0', e)

        def ident(m):
            name = m
            parts = name.split('::')
            for cand in ('::'.join(parts[-2:]), parts[-1]):
                if cand in self.enums:
                    return str(self.enums[cand])
            raise KeyError(name)

        try:
            e = re.sub(r'(?<![\d.])\b[A-Za-z_][\w]*(?:\s*::\s*[A-Za-z_]\w*)*', lambda m: ident(re.sub(r'\s', '', m.group(0))), e)
        except KeyError:
            return None
        e = re.sub(r'(\d+\.?\d*(?:[eE][-+]?\d+)?)[fF]\b', r'\1', e)
        e = re.sub(r'(\d+)[uUlL]+\b', r'\1', e)
        e = re.sub(r'(\d+\.)(?!\d)', r'\g<1>0', e)
        e = e.replace('||', ' or ').replace('&&', ' and ')
        try:
            v = eval(e, {"__builtins__": {}})
        except Exception:
            return None
        return v if isinstance(v, (int, float)) and not isinstance(v, bool) else (int(v) if isinstance(v, bool) else None)

    # Combo tables: "name = { { KEY, "Label" }, ... }" (ComboMap: stored value = KEY) or "name = { "a", "b" }"
    # (ComboVec / const char* arrays: stored value = index). Looked up in the menu files first, then the whole tree.
    def table(self, name, files):
        name = name.strip().lstrip('&*').strip()
        if name.startswith('{'):
            # An inline table: .ComboMap({ { 0, "Hold R" }, { 1, "Always" } }) (Lighthouse).
            return self._table_body(name[1:balanced(name, 0, '{', '}') - 1])
        if not re.fullmatch(r'[\w:]+', name):
            return None
        name = name.split('::')[-1]
        pat = re.compile(r'\b' + re.escape(name) + r'\s*(?:\[[^\]]*\])?\s*(=\s*)?\{')
        for f in list(files) + self._all_sources():
            try:
                raw = open(f, errors='replace').read()
            except OSError:
                continue
            if name not in raw:
                continue
            text = preprocess(strip_comments(raw))
            for m in pat.finditer(text):
                start = m.end() - 1
                out = self._table_body(text[start + 1:balanced(text, start, '{', '}') - 1])
                if out:
                    return out
        return None

    def _table_body(self, body):
        pairs = re.findall(r'\{\s*([^{},]+?)\s*,\s*((?:' + STR + r'\s*)+)\s*\}', body)
        if pairs:
            out = []
            for k, label in pairs:
                v = self.number(k)
                if v is None:
                    return None
                out.append((int(v), c_string(label)))
            return out
        strs = split_args(body)
        if strs and all(c_string(s) is not None for s in strs):
            return [(i, c_string(s)) for i, s in enumerate(strs)]
        return None

    def _all_sources(self):
        if self._tables is None:
            self._tables = [p for p in self.headers if 'libultraship' not in p]
            for dirpath, dirnames, files in os.walk(self.root):
                if re.search(r'/(build[^/]*|\.git|libultraship|Torch|extern|_deps)(/|$)',
                             '/' + os.path.relpath(dirpath, self.root)):
                    dirnames[:] = []
                    continue
                self._tables += [os.path.join(dirpath, f) for f in files if f.endswith('.cpp')]
        return self._tables


DATA_TYPES = {
    'WIDGET_CVAR_CHECKBOX': 'checkbox', 'WIDGET_CVAR_COMBOBOX': 'combobox', 'WIDGET_CVAR_BTN_SELECTOR': 'combobox',
    'WIDGET_CVAR_SLIDER_INT': 'int', 'WIDGET_CVAR_SLIDER_FLOAT': 'float',
}


def _options(port, calls):
    """Flattens .Options(XxxOptions().A(x).B(y)) and designated-initializer {.a = x} options into a dict."""
    o = {}
    for name, args in calls:
        o[name] = args
        if name == 'IsPercentage':
            # FloatSliderOptions::IsPercentage() also sets the range to 0..1 and the format to a percentage, over
            # any Min/Max called before it (the chain runs in order).
            o['Min'], o['Max'], o['Format'] = '0.0f', '1.0f', '"%.0f%%"'
    return o


def parse_data_driven(port, files):
    """SoH-style menus. Returns a list of widget dicts in menu order."""
    widgets = []
    menu = sidebar = section = ''
    for f in files:
        text = preprocess(strip_comments(open(f, errors='replace').read()))
        pos = 0
        events = re.compile(r'AddMenuEntry\s*\(|AddSidebarEntry\s*\(|WidgetPath\s+\w+\s*=\s*\{|\b\w+\s*=\s*\{\s*"|'
                            r'\.\s*sidebarName\s*=|\.\s*sectionName\s*=|AddWidget\s*\(')
        for m in events.finditer(text):
            if m.start() < pos:
                continue
            tok = m.group(0)
            if tok.startswith('AddMenuEntry'):
                args = split_args(text[m.end():balanced(text, m.end() - 1) - 1])
                menu = port.string(args[0]) or menu if args else menu
                sidebar = section = ''
            elif tok.startswith('AddSidebarEntry'):
                args = split_args(text[m.end():balanced(text, m.end() - 1) - 1])
                if len(args) >= 2:
                    menu = port.string(args[0]) or menu
                    sidebar = port.string(args[1]) or sidebar
                    section = ''
            elif tok.endswith('{') or tok.endswith('"'):
                start = text.index('{', m.start())
                args = split_args(text[start + 1:balanced(text, start, '{', '}') - 1])
                if len(args) >= 2 and c_string(args[0]) is not None and c_string(args[1]) is not None and (
                        'WidgetPath' in tok or 'SECTION_COLUMN' in (args[2] if len(args) > 2 else '')):
                    menu, sidebar, section = c_string(args[0]), c_string(args[1]), ''
            elif 'sidebarName' in tok or 'sectionName' in tok:
                end = text.index(';', m.end())
                v = port.string(text[m.end():end])
                if v is not None:
                    if 'sidebarName' in tok:
                        sidebar, section = v, ''
                    else:
                        menu = v
            else:  # AddWidget(path, label, TYPE)
                end = balanced(text, m.end() - 1)
                args = split_args(text[m.end():end - 1])
                calls, pos = chain(text, end)
                if len(args) < 3:
                    continue
                wtype = args[2].strip()
                label = port.string(args[1])
                if label is None:
                    # fmt::format("Disable Tap Jump (P{})", p + 1) in a per-player loop: player 1's label, with the
                    # player tag dropped (one player on a handheld).
                    lits = re.findall(STR, args[1])
                    if lits:
                        label = re.sub(r'\s*\(P\{\}\)|\s*\{[^}]*\}', '', c_string(lits[0])).strip() or None
                if wtype == 'WIDGET_SEPARATOR_TEXT':
                    section = label or section
                    continue
                if wtype not in DATA_TYPES:
                    continue
                line = text.count('\n', 0, m.start()) + 1
                c = _options(port, calls)
                w = dict(kind=DATA_TYPES[wtype], label=label, label_expr=args[1], menu=menu, sidebar=sidebar,
                         section=section, file=os.path.relpath(f, port.root), line=line)
                if 'CVar' in c:
                    w['cvar'] = port.string(c['CVar'], text)
                    w['cvar_expr'] = c['CVar']
                if 'Options' in c:
                    ot = c['Options']
                    om = re.match(r'\s*\w+\s*\(\s*\)', ot)
                    oc, _ = chain(ot, om.end()) if om else ([], 0)
                    opts = _options(port, oc)
                    w['opts'] = opts
                widgets.append(w)
    return widgets


def parse_immediate(port, files):
    """Starship-style menus: UIWidgets::CVar*(label, cvar, ...) inside UIWidgets::BeginMenu("X") { ... } blocks."""
    widgets = []
    for f in files:
        text = preprocess(strip_comments(open(f, errors='replace').read()))
        # Menu nesting by brace depth: BeginMenu("X") opens a block whose braces we track.
        depth = 0
        stack = []  # (name, depth at which the block's body began)
        pat = re.compile(r'UIWidgets::BeginMenu\s*\(|UIWidgets::(CVarCheckbox|CVarSliderInt|CVarSliderFloat|'
                         r'CVarCombobox|PaddedEnhancementCheckbox|EnhancementCheckbox|EnhancementCombobox|'
                         r'PaddedEnhancementSliderInt|EnhancementSliderInt|PaddedEnhancementSliderFloat|'
                         r'EnhancementSliderFloat)\s*\(|UIWidgets::Tooltip\s*\(|[{}]|\bvoid\s+\w+\s*\(')
        i = 0
        pending_menu = None
        for m in pat.finditer(text):
            if m.start() < i:
                continue
            tok = m.group(0)
            if tok == '{':
                depth += 1
                if pending_menu is not None:
                    stack.append((pending_menu, depth))
                    pending_menu = None
                continue
            if tok == '}':
                while stack and stack[-1][1] >= depth:
                    stack.pop()
                depth -= 1
                continue
            if tok.startswith('void'):
                if depth == 0:
                    stack = []
                continue
            start = m.end() - 1
            end = balanced(text, start)
            args = split_args(text[start + 1:end - 1])
            i = end
            if tok.startswith('UIWidgets::BeginMenu'):
                pending_menu = port.string(args[0]) if args else None
                continue
            if tok.startswith('UIWidgets::Tooltip'):
                if widgets and widgets[-1]['file'] == os.path.relpath(f, port.root) and 'tooltip_after' not in \
                        widgets[-1] and text.count('\n', widgets[-1]['_end'], m.start()) <= 12:
                    widgets[-1]['tooltip_after'] = port.string(args[0]) if args else None
                continue
            fn = m.group(1)
            menus = [s[0] for s in stack if s[0]]
            w = dict(menu=menus[0] if menus else '', sidebar=menus[1] if len(menus) > 1 else '',
                     section=' / '.join(menus[2:]), file=os.path.relpath(f, port.root),
                     line=text.count('\n', 0, m.start()) + 1, _end=end)
            designated = {}
            if args and args[-1].startswith('{'):
                for dm in re.finditer(r'\.(\w+)\s*=\s*', args[-1]):
                    rest = args[-1][dm.end():]
                    val = split_args(rest.rstrip('}'))[0] if rest else ''
                    designated[dm.group(1)] = val
            if fn in ('CVarCheckbox',):
                w.update(kind='checkbox', label_expr=args[0], cvar_expr=args[1])
                w['opts'] = {'Tooltip': designated.get('tooltip'), 'DefaultValue': designated.get('defaultValue')}
            elif fn in ('PaddedEnhancementCheckbox', 'EnhancementCheckbox'):
                w.update(kind='checkbox', label_expr=args[0], cvar_expr=args[1])
                # (text, cvar, [padTop, padBottom,] disabled, disabledTooltip, graphic, defaultValue)
                dv_idx = 7 if fn.startswith('Padded') else 5
                w['opts'] = {'DefaultValue': args[dv_idx] if len(args) > dv_idx else None}
            elif fn in ('CVarSliderInt', 'CVarSliderFloat'):
                w.update(kind='int' if fn.endswith('Int') else 'float', label_expr=args[0], cvar_expr=args[1])
                w['opts'] = {'Min': args[2] if len(args) > 2 else None, 'Max': args[3] if len(args) > 3 else None,
                             'DefaultValue': args[4] if len(args) > 4 else None,
                             'Tooltip': designated.get('tooltip'), 'Format': designated.get('format'),
                             'Step': designated.get('step'),
                             'IsPercentage': '' if designated.get('isPercentage', '').strip() == 'true' else None}
            elif fn in ('PaddedEnhancementSliderInt', 'EnhancementSliderInt', 'PaddedEnhancementSliderFloat',
                        'EnhancementSliderFloat'):
                # (text, id, cvar, min, max, format, default[, isPercentage])
                w.update(kind='int' if 'Int' in fn else 'float', label_expr=args[0], cvar_expr=args[2])
                w['opts'] = {'Min': args[3], 'Max': args[4], 'Format': args[5],
                             'DefaultValue': args[6] if len(args) > 6 else None,
                             'IsPercentage': '' if 'Float' in fn and len(args) > 7 and args[7] == 'true' else None}
            elif fn == 'CVarCombobox':
                w.update(kind='combobox', label_expr=args[0], cvar_expr=args[1])
                w['opts'] = {'ComboVec': args[2], 'Tooltip': designated.get('tooltip'),
                             'DefaultIndex': designated.get('defaultIndex')}
            elif fn == 'EnhancementCombobox':
                w.update(kind='combobox', label_expr='', cvar_expr=args[0])
                w['opts'] = {'ComboVec': args[1], 'DefaultIndex': args[2] if len(args) > 2 else None}
            w['opts'] = {k: v for k, v in w['opts'].items() if v is not None}
            w['label'] = port.string(w['label_expr']) if w['label_expr'] else None
            if w['label'] is None and w['label_expr']:
                # A computed label ("cond ? "A" : "B""): the last literal is the general case.
                lits = re.findall(STR, w['label_expr'])
                w['label'] = c_string(lits[-1]) if lits else None
            w['cvar'] = port.string(w['cvar_expr'])
            widgets.append(w)
    for w in widgets:
        w.pop('_end', None)
        if w.get('tooltip_after') and 'Tooltip' not in w['opts']:
            w['opts']['Tooltip'] = '"%s"' % w['tooltip_after'].replace('"', '\\"').replace('\n', '\\n')
        w.pop('tooltip_after', None)
    return widgets


if __name__ == '__main__':
    root = sys.argv[1]
    port = Port(root)
    files = [os.path.join(root, f) for f in sys.argv[2:]]
    immediate = any('UIWidgets::BeginMenu' in open(f, errors='replace').read() for f in files)
    for w in (parse_immediate if immediate else parse_data_driven)(port, files):
        print(f"{w['menu']:14} {w['sidebar']:20} {w['kind']:8} {str(w.get('cvar')):55} | {w.get('label')}")
