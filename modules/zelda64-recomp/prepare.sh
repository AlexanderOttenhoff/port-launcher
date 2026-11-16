#!/usr/bin/env bash
# Prepares build/upstream/Zelda64Recomp: pinned sources + Android patches + game code generated from your ROM.
#   modules/zelda64-recomp/prepare.sh "path/to/Majora's Mask (USA).z64"
# Needs git, python3, clang, ld.lld, cmake, ninja. The generated code is derived from your ROM: keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/zelda64-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # same rt64/plume/N64ModernRuntime/RecompFrontend patches as Banjo
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/Zelda64Recomp"
# Upstream's recomp-frontend branch (2026-08): the RecompFrontend version of Zelda64Recomp, on the same
# libraries as Banjo: Recompiled. The dev branch still uses the pre-plume RT64 our Android patches don't fit.
PIN=ab0fcb5

[ -d "$SRC" ] || git clone -q --recurse-submodules --shallow-submodules https://github.com/Zelda64Recomp/Zelda64Recomp.git "$SRC"
git -C "$SRC" fetch -q origin recomp-frontend
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule sync -q --recursive && git -C "$SRC" submodule update -q --init --recursive
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/rt64 "$LIBPATCHES/rt64"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
apply . "$HERE/patches/Zelda64Recomp"

# ROM: decompress (checked against the MM decomp's MD5s), then generate the C code.
python3 -I "$HERE/decompress_mm.py" "$ROM" "$SRC/mm.us.rev1.rom_uncompressed.z64"
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel 12
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp us.rev1.toml && ./RSPRecomp aspMain.us.rev1.toml && ./RSPRecomp njpgdspMain.us.rev1.toml)
# Host file_to_c for the Android cross build.
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
