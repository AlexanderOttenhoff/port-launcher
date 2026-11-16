#!/usr/bin/env bash
# Prepares build/upstream/BM64Recomp: pinned sources + Android patches + game code generated from your ROM.
#   modules/bm64-recomp/prepare.sh "path/to/Bomberman 64 (USA).z64"
# Needs git, a host C++ compiler, clang, ld.lld, make, cmake, ninja. The generated code is derived from your ROM: keep
# it private.
#
# Like Mega Man 64 (modules/mm64-recomp), a "B2" project (Zelda64Recomp-dev style UI copy) whose rt64 (71b5906, already
# post-plume) and N64ModernRuntime (0bb76b0, 2026-01) take our shared library patches unchanged.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/bm64-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # shared plume/N64ModernRuntime patches
RT64PATCHES="$ROOT/modules/sk2-recomp/patches/rt64" # applies cleanly to rt64 71b5906 (see mm64-recomp/prepare.sh)
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/BM64Recomp"
PIN=726adc9 # master 2026 (v1.0.0 plus Flatpak fixes)
# The Bomberman 64 decomp, only for its ROM decompressor (tools/bm64decompress.cpp, standard C++ only).
DECOMP="$ROOT/build/upstream/bm64-decomp"
DECOMP_PIN=c7c5fd5

[ -d "$SRC" ] || git clone -q https://github.com/RevoSucks/BM64Recomp.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
# Everything but freetype-windows-binaries (Android links our own FreeType prefix). lib/sf64 is only read for the
# patches' libultra headers (patches/Makefile), never built (an sf64 leftover: the project started from Starfox64Recomp).
git -C "$SRC" submodule update -q --init --recursive --depth 1 lib/N64ModernRuntime lib/rt64 lib/RmlUi lib/lunasvg lib/sse2neon
git -C "$SRC" submodule update -q --init --depth 1 lib/sf64 BM64Syms
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/rt64 "$RT64PATCHES"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply . "$HERE/patches/BM64Recomp"

# ROM: the recompiler wants the decompressed ROM (bm64.us.toml: bm64.decomp.us.z64). Upstream's BUILDING.md says to
# build the decomp for it, but that only runs the decomp's own decompressor first (its Makefile's `decompress` target):
# no IDO, no Python. Its output matches the decomp's checksum.md5 (d2f9313cccae0af85d23ac37666c0173).
[ -d "$DECOMP" ] || git clone -q https://github.com/Bomberhackers/bm64.git "$DECOMP"
git -C "$DECOMP" checkout -q "$DECOMP_PIN"
mkdir -p "$SRC/build-host-tools" "$SRC/rsp"
c++ -std=c++17 -O2 "$DECOMP/tools/bm64decompress.cpp" -o "$SRC/build-host-tools/bm64decompress"
"$SRC/build-host-tools/bm64decompress" "$ROM" "$SRC/bm64.decomp.us.z64"
echo "d2f9313cccae0af85d23ac37666c0173  $SRC/bm64.decomp.us.z64" | md5sum -c --quiet

cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel "${JOBS:-4}"
# Copied into the source dir: the game's own CMake runs ./N64Recomp there for the patches (patches.toml).
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp bm64.us.toml && ./RSPRecomp aspMain.toml)
# Host file_to_c for the Android cross build (RT64_HOST_FILE_TO_C).
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
