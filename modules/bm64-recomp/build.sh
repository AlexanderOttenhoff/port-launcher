#!/usr/bin/env bash
# Builds the Bomberman 64: Recompiled module from build/upstream/BM64Recomp (run prepare.sh first) and stages
# it for the APK. The output contains game code translated from the ROM: keep it private.
#   JOBS=4 BUILD_MEMORY_MAX=6G modules/bm64-recomp/build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/bm64-recomp"
SRC="$ROOT/build/upstream/BM64Recomp"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
FREETYPE="${FREETYPE_PREFIX:-$HOME/Android/prefixes/freetype-2.13.3-android-arm64}"
OUT="$ROOT/build/modules/bm64-recomp"
STAGE="$ROOT/build/modules/apk"

[ -d "$SRC/RecompiledFuncs" ] || { echo "no generated game code in $SRC (run modules/bm64-recomp/prepare.sh)" >&2; exit 1; }
[ -x "$SRC/build-host-tools/file_to_c" ] || { echo "no host file_to_c (run prepare.sh)" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

# PATCHES_*: the project's MIPS patches (patches/Makefile) are built with the host clang/ld.lld, not the NDK's.
"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_PROJECT_BM64Recompiled_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" \
  -DCMAKE_FIND_ROOT_PATH="$SDL2;$FREETYPE" \
  -DFREETYPE_LIBRARY="$FREETYPE/lib/libfreetype.a" \
  -DFREETYPE_INCLUDE_DIRS="$FREETYPE/include/freetype2" \
  -DRT64_HOST_FILE_TO_C="$SRC/build-host-tools/file_to_c" \
  -DPATCHES_C_COMPILER=clang -DPATCHES_LD=ld.lld \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_DICTBUILDER=OFF

# No swap on the dev machine and other builds share it: bounded jobs in a memory-capped scope, so running out of
# memory kills this build, not the desktop (see modules/jfg-recomp/build.sh).
JOBS="${JOBS:-4}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target BM64Recompiled --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_bm64-recomp.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
extra=$(find "$OUT/cmake" -maxdepth 3 -name '*.so' ! -name 'libplm_bm64-recomp.so' | head -n 5)
[ -z "$extra" ] || { echo "module must be one self-contained .so; also found: $extra" >&2; exit 1; }

# Data: the menus (assets/, loaded relative to the working directory, which the shim sets to the data dir) and the
# controller mappings main() loads from recompcontrollerdb.txt.
PKG="$STAGE/assets/modules/bm64-recomp"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
cp -r "$SRC/assets" "$PKG/data/"
cp "$SRC/recompcontrollerdb.txt" "$PKG/data/"
# The port's own settings for our menu (docs/planning/MENU.md "Port schemas"): its config menu joined with config.cpp.
python3 "$ROOT/tools/schema/oldui_schema.py" "$SRC" bm64-recomp > "$PKG/data/settings_schema.json"
# HEAD~1: prepare.sh commits our one game patch on top of the upstream pin.
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse "HEAD~1")/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
