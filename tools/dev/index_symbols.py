#!/usr/bin/env python3
"""Symbol index for the upstream checkouts (token saver: find a definition with one lookup instead of grepping
10,000-line files). Uses universal-ctags when installed (`sudo apt install universal-ctags`), else a small built-in
indexer for C/C++: function definitions, #defines, globals, struct/enum/typedef names.

    tools/dev/index_symbols.py [repo ...]      default: every build/upstream/* checkout; writes build/index/<repo>.tags
    tools/dev/sym.sh <name> [repo]             look a symbol up (exact name, then prefix)

Output lines: name<TAB>path:line<TAB>kind<TAB>first line of the definition. Re-run after a fresh prepare.sh.
"""
import os, re, shutil, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
UP = os.path.join(ROOT, "build", "upstream")
OUT = os.path.join(ROOT, "build", "index")
EXTS = (".c", ".cpp", ".cc", ".h", ".hpp", ".inc")
SKIP_DIRS = {".git", "build", "build-pc", "cmake-build", "assets", "docs", "node_modules", "third_party", "extern",
             "vendor", "out", "bin", "obj", "tools_pc", "reference"}
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "else", "do", "case", "defined"}

FUNC = re.compile(r"^(?!\s)(?:[A-Za-z_][\w\s\*&:<>,]*?[\s\*&])([A-Za-z_][\w:~]*)\s*\(([^;{}]*)\)?\s*(?:const\s*)?(\{)?\s*$")
DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)")
TYPE = re.compile(r"^(?:typedef\s+)?(?:struct|enum|union|class)\s+([A-Za-z_]\w*)\s*(?:\{|:|$)")
TYPEDEF_END = re.compile(r"^\}\s*([A-Za-z_]\w*)\s*;")
GLOBAL = re.compile(r"^(?!\s)(?:static\s+|extern\s+|const\s+|volatile\s+)*[A-Za-z_][\w\s\*]*?[\s\*]([A-Za-z_]\w*)\s*(?:\[[^\]]*\])*\s*(?:=[^;]*)?;\s*(?://.*|/\*.*)?$")


def builtin(repo_dir):
    out = []
    for base, dirs, files in os.walk(repo_dir):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".")]
        for name in files:
            if not name.endswith(EXTS):
                continue
            path = os.path.join(base, name)
            rel = os.path.relpath(path, ROOT)
            try:
                lines = open(path, errors="replace").read().split("\n")
            except OSError:
                continue
            for i, line in enumerate(lines):
                if len(line) > 300:
                    continue
                m = DEFINE.match(line)
                if m:
                    out.append((m.group(1), f"{rel}:{i+1}", "define", line.strip())); continue
                m = TYPE.match(line) or TYPEDEF_END.match(line)
                if m:
                    out.append((m.group(1), f"{rel}:{i+1}", "type", line.strip())); continue
                m = FUNC.match(line)
                if m and m.group(1) not in KEYWORDS and not line.rstrip().endswith(";"):
                    # A definition: body brace on this line or the next non-empty one.
                    nxt = next((l for l in lines[i+1:i+4] if l.strip()), "")
                    if m.group(3) or nxt.strip().startswith("{") or (m.group(2) is None):
                        out.append((m.group(1), f"{rel}:{i+1}", "function", line.strip())); continue
                if name.endswith((".c", ".cpp", ".cc")):
                    m = GLOBAL.match(line)
                    if m and m.group(1) not in KEYWORDS and "(" not in line.split("=")[0]:
                        out.append((m.group(1), f"{rel}:{i+1}", "global", line.strip()))
    return out


def with_ctags(repo_dir):
    exe = shutil.which("ctags")
    # Definitions only (no prototypes or struct members: they double the output of every lookup), paths relative to
    # the repo root so lookups stay short.
    res = subprocess.run([exe, "-R", "--languages=C,C++", "--kinds-C=-pm", "--kinds-C++=-pm", "--fields=+n", "-x",
                          "--_xformat=%N\t%F:%n\t%K\t%C", *[f"--exclude={d}" for d in SKIP_DIRS],
                          os.path.relpath(repo_dir, ROOT)], capture_output=True, text=True, errors="replace", cwd=ROOT)
    return [tuple(l.split("\t", 3)) for l in res.stdout.splitlines() if l.count("\t") >= 3]


def main(repos):
    os.makedirs(OUT, exist_ok=True)
    repos = repos or sorted(d for d in os.listdir(UP) if os.path.isdir(os.path.join(UP, d)))
    use_ctags = bool(shutil.which("ctags")) and "Universal" in subprocess.run(["ctags", "--version"], capture_output=True, text=True).stdout
    for repo in repos:
        d = os.path.join(UP, repo)
        if not os.path.isdir(d):
            print(f"skip {repo}: no checkout"); continue
        rows = with_ctags(d) if use_ctags else builtin(d)
        with open(os.path.join(OUT, f"{repo}.tags"), "w") as f:
            for r in sorted(rows, key=lambda r: r[0]):
                f.write("\t".join(x.replace("\t", " ") for x in r) + "\n")
        print(f"{repo}: {len(rows)} symbols ({'ctags' if use_ctags else 'built-in'})")


if __name__ == "__main__":
    main(sys.argv[1:])
