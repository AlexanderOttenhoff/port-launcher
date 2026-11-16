#!/usr/bin/env bash
# Prepares build/upstream/goldenballoon (Diddy Kong Racing): pinned sources + Android patches. No ROM needed at build
# time: Golden Balloon reads the player's ROM at runtime (--rom), so the build holds no ROM-derived code.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/goldenballoon"
SRC="$ROOT/build/upstream/goldenballoon"
PIN=2b69e748

[ -d "$SRC" ] || git clone -q https://github.com/akratch/goldenballoon.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/goldenballoon"/*.patch
echo "prepared: $SRC"
