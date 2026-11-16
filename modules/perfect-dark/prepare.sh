#!/usr/bin/env bash
# Prepares build/upstream/perfect_dark: pinned sources + Android patches. No ROM needed: the port reads the ROM at
# runtime, so the build contains no game code derived from it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/perfect-dark"
SRC="$ROOT/build/upstream/perfect_dark"
PIN=a4b58e6

[ -d "$SRC" ] || git clone -q https://github.com/fgsfdsfgs/perfect_dark.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/perfect_dark"/*.patch
echo "prepared: $SRC"
