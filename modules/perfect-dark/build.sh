#!/usr/bin/env bash
# Builds the Perfect Dark module from build/upstream/perfect_dark (run prepare.sh first) and stages it for the APK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/perfect-dark"
SRC="$ROOT/build/upstream/perfect_dark"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
OUT="$ROOT/build/modules/perfect-dark"
STAGE="$ROOT/build/modules/apk"

[ -d "$SRC/port" ] || { echo "no sources in $SRC (run modules/perfect-dark/prepare.sh)" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_pd_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" \
  -DCMAKE_FIND_ROOT_PATH="$SDL2" \
  `# The NDK makes -Wformat-security an error; the decomp passes game text as format strings.` \
  -DCMAKE_C_FLAGS="-Wno-error=format-security" -DCMAKE_CXX_FLAGS="-Wno-error=format-security"

# No swap on the dev machine: bounded jobs in a memory-capped scope (see modules/jfg-recomp/build.sh).
JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target pd --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_perfect-dark.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }

PKG="$STAGE/assets/modules/perfect-dark"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
# The port needs no data files besides the ROM (linked in by the shim at launch).
echo "Perfect Dark reads everything from your ROM." > "$PKG/data/README.txt"
# The port's own settings for our menu (docs/planning/MENU.md, design of 2026-11-14), generated from its
# configRegister* calls; the shim applies them by key through the port's configLoad (plm_shim.cpp).
python3 "$ROOT/tools/schema/pd_schema.py" "$SRC" > "$PKG/data/settings_schema.json"
python3 "$ROOT/tools/schema/add_profiles.py" "$PKG/data/settings_schema.json" perfect-dark  # our own profiles, if any
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
