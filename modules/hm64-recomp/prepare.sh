#!/usr/bin/env bash
# Prepares build/upstream/HarvestMoon64Recomp: pinned sources + Android patches + game code generated from your ROM.
#   modules/hm64-recomp/prepare.sh "path/to/Harvest Moon 64 (USA).z64"
# Needs git, clang, cmake, ninja. The generated code is derived from your ROM: keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/hm64-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # shared plume/N64ModernRuntime/RecompFrontend patches
RT64PATCHES="$ROOT/modules/blastcorps-recomp/patches/rt64" # rt64 series re-anchored for newer rt64 (applies here too)
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/HarvestMoon64Recomp"
PIN=0470d52

[ -d "$SRC" ] || git clone -q https://github.com/HarvestMoon64Recomp/HarvestMoon64Recomp.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" submodule update -q --init --recursive --depth 1 lib/N64ModernRuntime lib/RecompFrontend lib/rt64
# Symbols for the recompiler; the decomp for the patches' headers (patches/Makefile), not built.
git -C "$SRC" submodule update -q --init --depth 1 HarvestMoon64RecompSyms lib/hm64-decomp
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/rt64 "$RT64PATCHES"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
apply . "$HERE/patches/HarvestMoon64Recomp"

# ROM: no decompression step; the recompiler reads the retail ROM (hm64.us.toml: hm64.us.z64).
cp "$ROM" "$SRC/hm64.us.z64"
mkdir -p "$SRC/build-host-tools"
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel 12
# Copied into the source dir: the game's own CMake runs ./N64Recomp there for the patches.
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp hm64.us.toml && ./RSPRecomp n_aspMain.toml)
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
