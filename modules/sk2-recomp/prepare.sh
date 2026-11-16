#!/usr/bin/env bash
# Prepares build/upstream/snowboardkids2-recomp: pinned sources + Android patches + game code generated from your ROM.
#   modules/sk2-recomp/prepare.sh "path/to/Snowboard Kids 2 (USA).z64"
# Needs git, clang, cmake, ninja. The generated code is derived from your ROM: keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/sk2-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # shared plume/N64ModernRuntime/RecompFrontend patches
RT64PATCHES="$HERE/patches/rt64" # our rt64 series re-anchored for this fork (no plume-pointer patch: committed below)
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/snowboardkids2-recomp"
PIN=59d1621

[ -d "$SRC" ] || git clone -q https://github.com/cdlewis/snowboardkids2-recomp.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" submodule update -q --init --recursive --depth 1 lib/N64ModernRuntime lib/RecompFrontend lib/rt64
# Symbols for the recompiler; the decomp for the patches' headers (not built); sse2neon and the launcher theme for the
# game build.
git -C "$SRC" submodule update -q --init --depth 1 snowboardkids2-recomp-syms lib/sk2-decomp lib/sse2neon lib/recomp-theme
# The decomp's libultra headers (patches/Makefile includes lib/sk2-decomp/lib/ultralib/include).
git -C "$SRC/lib/sk2-decomp" submodule update -q --init --depth 1 lib/ultralib
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/rt64 "$RT64PATCHES"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
apply . "$HERE/patches/snowboardkids2-recomp"

# ROM: no decompression step; the recompiler reads the retail ROM (us.toml: snowboardkids2.z64).
cp "$ROM" "$SRC/snowboardkids2.z64"
mkdir -p "$SRC/build-host-tools"
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel 12
# Copied into the source dir: the game's own CMake runs ./N64Recomp there for the patches.
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp us.toml && ./RSPRecomp aspMain.us.toml)
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
