#!/usr/bin/env bash
# Builds the Blast Corps Recompiled module from build/upstream/blastcorps-recomp (run prepare.sh first) and stages it for the APK.
# The output contains game code translated from the ROM: keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/blastcorps-recomp"
SRC="$ROOT/build/upstream/blastcorps-recomp"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
FREETYPE="${FREETYPE_PREFIX:-$HOME/Android/prefixes/freetype-2.13.3-android-arm64}"
OUT="$ROOT/build/modules/blastcorps-recomp"
STAGE="$ROOT/build/modules/apk"

[ -d "$SRC/RecompiledFuncs" ] || { echo "no generated game code in $SRC (run modules/blastcorps-recomp/prepare.sh)" >&2; exit 1; }
[ -x "$SRC/build-host-tools/file_to_c" ] || { echo "no host file_to_c (run prepare.sh)" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_PROJECT_BlastCorpsRecompiled_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" \
  -DCMAKE_FIND_ROOT_PATH="$SDL2;$FREETYPE" \
  -DFREETYPE_LIBRARY="$FREETYPE/lib/libfreetype.a" \
  -DFREETYPE_INCLUDE_DIRS="$FREETYPE/include/freetype2" \
  -DRT64_HOST_FILE_TO_C="$SRC/build-host-tools/file_to_c" -DHOST_N64RECOMP="$SRC/build-host-tools/N64Recomp" \
  -DPATCHES_C_COMPILER=clang -DPATCHES_LD=ld.lld \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_DICTBUILDER=OFF

# No swap on the dev machine: bounded jobs in a memory-capped scope (see modules/jfg-recomp/build.sh).
JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target BlastCorpsRecompiled --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_blastcorps-recomp.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
extra=$(find "$OUT/cmake" -maxdepth 3 -name '*.so' ! -name 'libplm_blastcorps-recomp.so' | head -n 5)
[ -z "$extra" ] || { echo "module must be one self-contained .so; also found: $extra" >&2; exit 1; }

PKG="$STAGE/assets/modules/blastcorps-recomp"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
cp -r "$SRC/assets" "$SRC/icons" "$PKG/data/"
cp "$SRC/recompcontrollerdb.txt" "$PKG/data/"
# The port's own settings for our menu (docs/planning/MENU.md, design of 2026-11-14), generated from the game's
# RecompFrontend config registrations; the shim applies them by key (recompfrontend_shim.cpp).
python3 "$ROOT/tools/schema/recompfrontend_schema.py" "$SRC" blastcorps-recomp > "$PKG/data/settings_schema.json"
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
