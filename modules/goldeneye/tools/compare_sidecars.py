#!/usr/bin/env python3
"""Compares two sets of GoldenEye sidecars entry by entry (decompressing RZ entries), e.g. the native converter's
output against the upstream Python converters'. Exit 0 when every entry's content is identical.

    compare_sidecars.py <data dir A> <data dir B> [region]
"""
import csv
import os
import sys
import zlib


def load(d, sub, binname):
    rows = list(csv.reader(open(os.path.join(d, sub, "manifest.csv"), newline="")))[1:]
    blob = open(os.path.join(d, sub, binname), "rb").read()
    return [(r[0], blob[int(r[1]):int(r[1]) + int(r[2])]) for r in rows], blob


def content(b):
    return zlib.decompress(b[2:], -15) if b[:2] == b"\x11\x72" else b


def main() -> int:
    a, b = sys.argv[1], sys.argv[2]
    region = sys.argv[3] if len(sys.argv) > 3 else "ntsc-final"
    bad = 0
    for sub, binname in ((f"pcmodels-{region}", "pcmodels.bin"), (f"pccg-{region}", "pccg.bin")):
        ea, ba = load(a, sub, binname)
        eb, bb = load(b, sub, binname)
        if [n for n, _ in ea] != [n for n, _ in eb]:
            print(f"{sub}: entry names/order differ ({len(ea)} vs {len(eb)})")
            bad += 1
            continue
        diff = [n for (n, x), (_, y) in zip(ea, eb) if content(x) != content(y)]
        same_bytes = ba == bb
        print(f"{sub}: {len(ea)} entries, {len(ea) - len(diff)} identical in content"
              f"{'' if not diff else ', DIFFERENT: ' + ', '.join(diff[:8])}; files byte-identical: {same_bytes}")
        bad += len(diff)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
