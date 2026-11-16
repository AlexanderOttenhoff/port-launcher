"""A tiny, forgiving C/C++ source reader for the settings-schema generators (docs/planning/MENU.md, design of
2026-11-14). It is NOT a C++ parser: it knows just enough to pull option registrations out of the ports' sources
(strip comments and `#if 0` blocks, find calls and their top-level arguments, join string literals, resolve
`ns::name` string constants and enum-option tables). Why not libclang: the generators run in every module build, on
machines that only have the Android NDK and python3; a regex reader that fails loudly (unparsed calls go to stderr)
is good enough for code that registers options in a handful of regular shapes.
"""
import os
import re
import sys

STRING = r'(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"'


def strip_comments(text):
    """Removes // and /* */ comments, keeping string literals (and line numbers, roughly) intact."""
    out = []
    i, n = 0, len(text)
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
            out.append('\n' * text.count('\n', i, n if j < 0 else j))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def strip_if0(text):
    """Drops `#if 0 ... #endif` (and the #else branch of `#if 1`); every other preprocessor line is dropped but its
    contents kept (platform #ifdefs: an option behind ALLOW_VULKAN is still an option of the port)."""
    lines = text.split('\n')
    out = []
    stack = []  # per open #if: [kind ('0', '1' or 'other'), parent dead, this branch dead]
    for line in lines:
        s = line.strip()
        m = re.match(r'#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)', s)
        if m:
            kind, rest = m.group(1), m.group(2).strip()
            parent_dead = bool(stack) and stack[-1][2]
            if kind in ('if', 'ifdef', 'ifndef'):
                k = rest if kind == 'if' and rest in ('0', '1') else 'other'
                stack.append([k, parent_dead, parent_dead or k == '0'])
            elif kind in ('elif', 'else') and stack:
                k, pdead, _ = stack[-1]
                # #if 0 ... #else: the else branch lives; #if 1 ... #else: it's dead. Anything else: keep both.
                stack[-1][2] = pdead or k == '1'
            elif kind == 'endif' and stack:
                stack.pop()
            out.append('')
            continue
        if s.startswith('#'):
            out.append('')
            continue
        out.append('' if stack and stack[-1][2] else line)
    return '\n'.join(out)


def read_source(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return strip_if0(strip_comments(f.read()))


def balanced(text, start, open_ch='(', close_ch=')'):
    """Index just past the bracket matching text[start] (which must be open_ch)."""
    depth = 0
    i, n = start, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            i = j + 1
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError('unbalanced')


def split_args(inner):
    """Splits a call's argument text at top-level commas."""
    args, depth, cur = [], 0, []
    i, n = 0, len(inner)
    while i < n:
        c = inner[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and inner[j] != c:
                j += 2 if inner[j] == '\\' else 1
            cur.append(inner[i:j + 1])
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


def calls(text, name_regex):
    """Yields (match, args) for every call whose callee matches name_regex (which must end right before '(')."""
    for m in re.finditer(name_regex + r'\s*\(', text):
        start = m.end() - 1
        try:
            end = balanced(text, start)
        except ValueError:
            continue
        yield m, split_args(text[start + 1:end - 1]), end


def unescape(s):
    return (s.replace('\\"', '"').replace("\\'", "'").replace('\\n', '\n').replace('\\t', '\t')
            .replace('\\\\', '\\'))


def string_value(expr, consts=None):
    """The value of a string expression: adjacent literals joined; a known constant name; None otherwise."""
    expr = expr.strip()
    lits = re.findall(STRING, expr)
    if lits and re.sub(STRING, '', expr).strip() == '':
        return ''.join(unescape(re.sub(r'^(?:u8|u|U|L)?"', '', l)[:-1]) for l in lits)
    if consts is not None:
        v = consts.resolve(expr)
        if v is not None:
            return v
    return None


class Constants:
    """String constants (`const std::string x = "..."`, `constexpr const char* x = "..."`, `std::string_view`...) with
    the namespaces they're declared in, so `game::configkeys::general::x` resolves to the right one when several
    namespaces declare an `x` (or an `id`)."""

    DEF = re.compile(r'\b(?:const|constexpr)\b[^;={}()]*?\b(\w+)\s*(?:\[\s*\])?\s*=\s*(' + STRING + r'(?:\s*' + STRING + r')*)\s*;')

    def __init__(self):
        self.defs = {}  # name -> [(namespace path tuple, value)]

    def add_file(self, path):
        self.add_text(read_source(path))

    def add_text(self, text):
        spans = namespace_spans(text)
        for m in self.DEF.finditer(text):
            value = string_value(m.group(2))
            if value is None:
                continue
            self.defs.setdefault(m.group(1), []).append((namespace_at(spans, m.start()), value))

    def resolve(self, expr):
        expr = re.sub(r'\s+', '', expr)
        if not re.fullmatch(r'(?:\w+::)*\w+', expr):
            return None
        parts = expr.split('::')
        cands = self.defs.get(parts[-1], [])
        if not cands:
            return None
        qual = tuple(parts[:-1])
        if qual:
            best = [v for ns, v in cands if ns[-len(qual):] == qual]
            if not best:  # a using-declaration or a partially qualified name: match the innermost qualifiers
                best = [v for ns, v in cands if qual[-1] in ns]
            if best:
                cands = [(None, v) for v in best]
        values = {v for _, v in cands}
        if len(values) == 1:
            return values.pop()
        print(f'ambiguous constant {expr}: {sorted(values)}', file=sys.stderr)
        return cands[0][1]


def namespace_spans(text):
    """[(start, end, name)] for every `namespace a::b {` block."""
    spans = []
    for m in re.finditer(r'\bnamespace\s+([\w:]+)\s*\{', text):
        try:
            end = balanced(text, m.end() - 1, '{', '}')
        except ValueError:
            end = len(text)
        spans.append((m.end(), end, m.group(1)))
    return spans


def namespace_at(spans, pos):
    path = []
    for s, e, name in spans:  # spans are in source order, so outer blocks come first
        if s <= pos < e:
            path.extend(name.split('::'))
    return tuple(path)


def enum_tables(text, consts=None):
    """`static EnumOptionVector name = {{Enum::A, "key", "Label"}, ...};` (or a std::vector of
    ConfigOptionEnumOption) -> {name: [(value expr, key, label)]}."""
    tables = {}
    pat = re.compile(r'(?:EnumOptionVector|std::vector\s*<\s*recomp::config::ConfigOptionEnumOption\s*>)\s*&?\s*(\w+)\s*=\s*\{')
    for m in pat.finditer(text):
        try:
            end = balanced(text, m.end() - 1, '{', '}')
        except ValueError:
            continue
        body = text[m.end():end - 1]
        entries = []
        for e in re.finditer(r'\{([^{}]*)\}', body):
            parts = split_args(e.group(1))
            if len(parts) < 2:
                continue
            key = string_value(parts[1], consts)
            label = string_value(parts[2], consts) if len(parts) > 2 else key
            if key is None:
                continue
            entries.append((re.sub(r'\s+', '', parts[0]), key, label if label is not None else key))
        tables[m.group(1)] = entries
    return tables


def source_files(root, exts=('.c', '.cpp', '.h', '.hpp', '.inc')):
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith('.')]
        for f in filenames:
            if f.endswith(exts):
                out.append(os.path.join(dirpath, f))
    return sorted(out)


def clean_html(s):
    """RecompFrontend descriptions are RmlUi markup: <br />, <b>, <recomp-color ...>. Our menu shows plain text."""
    s = re.sub(r'<br\s*/?>', '\n', s)
    s = re.sub(r'<[^>]+>', '', s)
    s = re.sub(r'[ \t]+', ' ', s)
    s = re.sub(r' *\n *', '\n', s)
    s = re.sub(r'\n{3,}', '\n\n', s)
    return s.strip()
