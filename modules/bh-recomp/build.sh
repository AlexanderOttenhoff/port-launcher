#!/usr/bin/env bash
# Builds the Body Harvest: Recompiled module from build/upstream/body-harvest-recomp (run prepare.sh first) and stages
# it for the APK. The output contains game code translated from the ROM: keep it private.
# SRC/OUT/STAGE/ROOT can be overridden (used to test-build from a copy of the tree).
set -euo pipefail
: "${ROOT:=$(cd "$(dirname "$0")/../.." && pwd)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${SRC:=$ROOT/build/upstream/body-harvest-recomp}"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
FREETYPE="${FREETYPE_PREFIX:-$HOME/Android/prefixes/freetype-2.13.3-android-arm64}"
: "${OUT:=$ROOT/build/modules/bh-recomp}"
: "${STAGE:=$ROOT/build/modules/apk}"

[ -f "$SRC/RecompiledFuncs/funcs.h" ] || { echo "no generated game code in $SRC (run modules/bh-recomp/prepare.sh)" >&2; exit 1; }
[ -x "$SRC/build-host-tools/file_to_c" ] || { echo "no host file_to_c (run prepare.sh)" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

# BH_WITH_* are the upstream phase gates; all three on is the full game (as upstream's own release builds).
"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SHARED_LIBS=OFF \
  -DBH_WITH_RUNTIME=ON -DBH_WITH_RECOMPILED=ON -DBH_WITH_FRONTEND=ON \
  -DCMAKE_PROJECT_BodyHarvestRecomp_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" \
  -DCMAKE_FIND_ROOT_PATH="$SDL2;$FREETYPE" \
  -DFREETYPE_LIBRARY="$FREETYPE/lib/libfreetype.a" \
  -DFREETYPE_INCLUDE_DIRS="$FREETYPE/include/freetype2" \
  -DRT64_HOST_FILE_TO_C="$SRC/build-host-tools/file_to_c" \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_DICTBUILDER=OFF

# No swap on the dev machine: bounded jobs in a memory-capped scope (see modules/jfg-recomp/build.sh).
JOBS="${JOBS:-4}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target body-harvest-recomp --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_bh-recomp.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
extra=$(find "$OUT/cmake" -maxdepth 3 -name '*.so' ! -name 'libplm_bh-recomp.so' | head -n 5)
[ -z "$extra" ] || { echo "module must be one self-contained .so; also found: $extra" >&2; exit 1; }

PKG="$STAGE/assets/modules/bh-recomp"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
# The POST_BUILD step staged the frontend's stylesheet, fonts and icons next to the library (assets/); the game reads
# them relative to the working directory, which the shim sets to the data dir.
cp -r "$OUT/cmake/assets" "$PKG/data/"
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
