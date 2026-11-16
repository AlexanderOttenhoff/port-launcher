#!/usr/bin/env bash
# Prepares build/upstream/BanjoTooieRecompiled: pinned sources + our Android patches + the port's own required
# library patches. No ROM needed: upstream commits its generated code (RecompiledFuncs/, rsp/n_aspMain.cpp) because
# it can't be fully regenerated (see its README). Owner's call (2026-11-11): use that code for local builds; it is
# never committed here or published (build/ is gitignored); revisit before any public release.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/banjotooie-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # same rt64/plume/N64ModernRuntime/RecompFrontend patches as Banjo
SRC="$ROOT/build/upstream/BanjoTooieRecompiled"
PIN=9e58525

[ -d "$SRC" ] || git clone -q https://github.com/Vidanox/BanjoTooieRecompiled.git "$SRC"
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/rt64 "$LIBPATCHES/rt64"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
# The port's own required patches (tools/setup_deps.py does the same), on top of ours.
for dep in N64ModernRuntime rt64 N64Recomp; do
  git -C "$SRC/lib/$dep" checkout -q -B plm-android
  git -C "$SRC/lib/$dep" apply --3way "$SRC/patches/$dep.patch"
  git -C "$SRC/lib/$dep" -c user.name=plm -c user.email=plm@localhost commit -qam "Banjo-Tooie: required patch"
done
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/BanjoTooieRecompiled"/*.patch
# Host file_to_c for the Android cross build.
mkdir -p "$SRC/build-host-tools"
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
