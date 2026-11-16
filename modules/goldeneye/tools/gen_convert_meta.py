#!/usr/bin/env python3
"""Extracts the non-ROM inputs of the GoldenEye port's sidecar converters (tools_pc/d43_emit.py, d69_emit.py,
d88_emit.py) into one metadata file for the module's native converter (ge_convert.cpp).

    gen_convert_meta.py <goldeneye-pc-port checkout> <out.txt> [u|e|j]

Everything here comes from files committed to the decompilation (the ROM file list, the file resource table order,
the model file headers); nothing is read from a ROM. The parsing below mirrors the converters line for line, so the
native converter sees exactly what the Python does:

  F <base> <rom offset> <size>   scripts/filelist.<r>.csv rows by basename (.bin stripped; later rows win)
  T <name>                       assets/obseg/file_resource_table.inc.c names, in table order
  H <header key> <NS> <NT>       MODELFILEHEADER(...) switch/texture counts (args 0, 4, 8; later files win)
"""
import csv
import os
import re
import sys


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    region = sys.argv[3] if len(sys.argv) == 4 else "u"
    lines = []

    fl_by_base = {}
    for r in csv.reader(open(os.path.join(src, f"scripts/filelist.{region}.csv"))):
        if len(r) < 3 or not r[2]:
            continue
        base = r[2].rsplit("/", 1)[-1]
        if base.endswith(".bin"):
            base = base[:-4]
        fl_by_base[base] = (int(r[0]), int(r[1]))
    for base, (addr, size) in fl_by_base.items():
        lines.append(f"F\t{base}\t{addr}\t{size}")

    for line in open(os.path.join(src, "assets/obseg/file_resource_table.inc.c")):
        m = re.search(r'\{\s*\w+\s*,\s*"([^"]+)"', line)
        if m:
            lines.append(f"T\t{m.group(1)}")

    nsnt = {}
    for root, _dirs, files in os.walk(os.path.join(src, "assets")):
        for fn in files:
            if fn.lower().endswith("modelfileheader.inc.c"):
                for line in open(os.path.join(root, fn)):
                    m = re.search(r"MODELFILEHEADER\((.*)\)\s*;?\s*$", line)
                    if not m:
                        continue
                    a = [x.strip() for x in m.group(1).split(",")]
                    if len(a) < 9:
                        continue
                    try:
                        nsnt[a[0]] = (int(a[4], 0), int(a[8], 0))
                    except ValueError:
                        pass
    for key, (ns, nt) in nsnt.items():
        lines.append(f"H\t{key}\t{ns}\t{nt}")

    with open(out, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{out}: {len(fl_by_base)} files, {sum(l[0] == 'T' for l in lines)} table entries, {len(nsnt)} model headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
