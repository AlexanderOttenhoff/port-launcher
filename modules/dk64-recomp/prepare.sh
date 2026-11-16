#!/usr/bin/env bash
# Prepares build/upstream/Donkey-Kong-64-Recompiled: pinned sources + Android patches + game code generated from
# your ROM.
#   modules/dk64-recomp/prepare.sh "path/to/Donkey Kong 64 (USA).z64"
# Needs git, a host C++ compiler with zlib, clang, ld.lld, cmake, ninja. The generated code is derived from your ROM:
# keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/dk64-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # same rt64/plume/N64ModernRuntime/RecompFrontend patches as Banjo
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/Donkey-Kong-64-Recompiled"
PIN=f77cc64

[ -d "$SRC" ] || git clone -q https://github.com/Rainchus/Donkey-Kong-64-Recompiled.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
# vcpkg isn't used on Android; skip that submodule.
git -C "$SRC" submodule update -q --init --recursive --depth 1 lib/N64ModernRuntime lib/RecompFrontend lib/rt64 lib/dk64_decomp
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/rt64 "$LIBPATCHES/rt64"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
# DK64's RecompFrontend fork adds in-game mod discovery over libcurl; stubbed on Android.
git -C "$SRC/lib/RecompFrontend" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/RecompFrontend"/*.patch
apply . "$HERE/patches/Donkey-Kong-64-Recompiled"

# ROM: decompress with the project's own decompressor, then generate the C code.
mkdir -p "$SRC/build-host-tools"
c++ -std=c++17 -O2 "$SRC/decompressor.cpp" -lz -o "$SRC/build-host-tools/dk64_decompress"
(cd "$SRC" && build-host-tools/dk64_decompress "$ROM")
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel 12
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp us.toml && ./RSPRecomp n_aspMain.toml)
# Host file_to_c for the Android cross build.
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
