#!/usr/bin/env bash
# Builds the Jet Force Gemini: Recompiled module from a prepared JFG Android checkout and stages it for the APK.
#
#   JFG_SRC=~/src/JFG-Android modules/jfg-recomp/build.sh
#
# JFG_SRC must already contain the game code generated from your ROM (RecompiledFuncs, rsp/aspMain.cpp) and the host
# file_to_c tool, as in its docs/ANDROID.md. Needs tools/deps/build_sdl2.sh (and JFG's FreeType prefix).
# Like any recomp build, the output contains game code translated from the ROM: keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/jfg-recomp"
JFG_SRC="$(realpath "${JFG_SRC:-$HOME/src/JFG-Android}")"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
FREETYPE="${FREETYPE_PREFIX:-$HOME/Android/prefixes/freetype-2.13.3-android-arm64}"
OUT="$ROOT/build/modules/jfg-recomp"
STAGE="$ROOT/build/modules/apk"

[ -d "$JFG_SRC/RecompiledFuncs" ] || { echo "no generated game code in $JFG_SRC (run its build_linux.sh)" >&2; exit 1; }
[ -x "$JFG_SRC/build-android-host/file_to_c" ] || { echo "no $JFG_SRC/build-android-host/file_to_c" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

"$SDK_CMAKE/cmake" -S "$JFG_SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_PROJECT_JFGRecomp_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" \
  -DCMAKE_FIND_ROOT_PATH="$SDL2;$FREETYPE" \
  -DFREETYPE_LIBRARY="$FREETYPE/lib/libfreetype.a" \
  -DFREETYPE_INCLUDE_DIRS="$FREETYPE/include/freetype2" \
  -DRT64_HOST_FILE_TO_C="$JFG_SRC/build-android-host/file_to_c" \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_DICTBUILDER=OFF
# This machine has no swap: an unbounded parallel build of the recompiled game code once froze the whole desktop.
# Build with a bounded job count inside a memory-capped scope, so running out of memory kills the build, not the PC.
JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_jfg-recomp.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
extra=$(find "$OUT/cmake" -maxdepth 3 -name '*.so' ! -name 'libplm_jfg-recomp.so' | head -n 5)
[ -z "$extra" ] || { echo "module must be one self-contained .so; also found: $extra" >&2; exit 1; }

# Package layout (docs/MODULE_SPEC.md §2), unpacked the way the APK carries it.
PKG="$STAGE/assets/modules/jfg-recomp"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
cp -r "$JFG_SRC/assets" "$PKG/data/assets"
cp "$JFG_SRC/recompcontrollerdb.txt" "$PKG/data/"
# The port's own settings for our menu (docs/planning/MENU.md, design of 2026-11-14), generated from JFG's own
# option tables (tools/gen_jfg_config_rml.py in the JFG checkout); the shim applies them by key (plm_shim.cpp).
python3 "$ROOT/tools/schema/jfg_schema.py" "$JFG_SRC" > "$PKG/data/settings_schema.json"
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$JFG_SRC" rev-parse HEAD)/" \
  "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
