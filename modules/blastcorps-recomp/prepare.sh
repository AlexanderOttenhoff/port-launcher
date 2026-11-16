#!/usr/bin/env bash
# Prepares build/upstream/blastcorps-recomp: pinned sources + Android patches + game code generated from your ROM.
#   modules/blastcorps-recomp/prepare.sh "path/to/Blast Corps (USA) (Rev 1).z64"
# Needs git, python3, clang, cmake, ninja. The generated code is derived from your ROM: keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/blastcorps-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # shared plume/N64ModernRuntime/RecompFrontend patches
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/blastcorps-recomp"
PIN=b551045

[ -d "$SRC" ] || git clone -q https://github.com/SlaveOfIDO/blastcorps-recomp.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" submodule update -q --init --recursive --depth 1 lib/N64ModernRuntime lib/RecompFrontend lib/rt64
# ultralib and the decomp (lib/blastcorps, headers only): the patches build (patches/Makefile); gzip-hpp and SlotMap:
# the game build.
git -C "$SRC" submodule update -q --init --depth 1 lib/ultralib lib/gzip-hpp lib/SlotMap lib/blastcorps
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
# Blast Corps' rt64 fork: our rt64 patches, with the RGBA8 swap-chain hunk re-anchored (it adds L3DEX support).
apply lib/rt64 "$HERE/patches/rt64"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
apply . "$HERE/patches/blastcorps-recomp"

# ROM: the project's own decompressor (standard library only), then generate the C code.
python3 "$SRC/tools/decompress_rom.py" "$ROM" "$SRC/baserom.us.v11.decompressed.z64"
mkdir -p "$SRC/build-host-tools"
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel 12
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp us.v11.toml && ./RSPRecomp aspMain.us.v11.toml)
# Host file_to_c for the Android cross build.
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
