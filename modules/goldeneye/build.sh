#!/usr/bin/env bash
# Builds the GoldenEye module from build/upstream/goldeneye-pc-port (run prepare.sh first) and stages it for the APK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/goldeneye"
SRC="$ROOT/build/upstream/goldeneye-pc-port"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
OUT="$ROOT/build/modules/goldeneye"
STAGE="$ROOT/build/modules/apk"

[ -d "$SRC/port" ] || { echo "no sources in $SRC (run modules/goldeneye/prepare.sh)" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DROMID=ntsc-final \
  -DCMAKE_PROJECT_ge007_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" \
  -DCMAKE_FIND_ROOT_PATH="$SDL2" \
  -DCMAKE_C_FLAGS="-Wno-error=format-security" -DCMAKE_CXX_FLAGS="-Wno-error=format-security"

# No swap on the dev machine: bounded jobs in a memory-capped scope (see modules/jfg-recomp/build.sh).
JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target ge007 --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_goldeneye.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }

PKG="$STAGE/assets/modules/goldeneye"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
echo "GoldenEye reads the game from your ROM; its model and level files are converted from it on first start." > "$PKG/data/README.txt"
# The native converter's non-ROM inputs (decomp file list, file table order, model header counts).
python3 -I "$HERE/tools/gen_convert_meta.py" "$SRC" "$PKG/data/ge007-convert-meta.txt" u
# The port's own settings for our menu (docs/planning/MENU.md, design of 2026-11-14), generated from its options
# menu table; the shim applies them by key through the port's configSetValue (plm_shim.cpp).
python3 "$ROOT/tools/schema/goldeneye_schema.py" "$SRC" > "$PKG/data/settings_schema.json"
python3 "$ROOT/tools/schema/add_profiles.py" "$PKG/data/settings_schema.json" goldeneye  # our own profiles, if any
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
