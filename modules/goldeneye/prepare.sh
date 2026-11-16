#!/usr/bin/env bash
# Prepares build/upstream/goldeneye-pc-port: pinned sources + Android patches. No ROM needed: the port reads the ROM
# at runtime and the build compiles no ROM-derived files (the decomp's committed assets are source).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/goldeneye"
SRC="$ROOT/build/upstream/goldeneye-pc-port"
PIN=5b86a9bb   # v0.6.0

[ -d "$SRC" ] || git clone -q https://github.com/jkdansereau/goldeneye-pc-port.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/goldeneye-pc-port"/*.patch
echo "prepared: $SRC"
