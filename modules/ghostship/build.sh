#!/usr/bin/env bash
# Builds the Ghostship module from build/upstream/Ghostship (run prepare.sh first) and stages it for the APK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/ghostship"
SRC="$ROOT/build/upstream/Ghostship"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
LUSDEPS="$ROOT/build/deps/lus-android-arm64" # tools/deps/build_lus_deps.sh
OUT="$ROOT/build/modules/ghostship"
STAGE="$ROOT/build/modules/apk"

[ -f "$SRC/ghostship.o2r" ] || { echo "no ghostship.o2r in $SRC (run modules/ghostship/prepare.sh)" >&2; exit 1; }

"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_Ghostship_INCLUDE="$HERE/module.cmake" \
  `# Ghostship's libultraship fetches its own dependencies on Android (no prefix needed).` \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$SDL2" \
  -DUSE_OPENGLES=ON -DCPU_OPTION=-march=armv8-a -DUSE_NETWORKING=OFF \
  `# No runtime C compiler for mods (it ships as a second library, libtcc.so); prebuilt mod binaries still load.` \
  -DENABLE_TCC_COMPILER=OFF \
  `# The NDK fortifies memcpy as an overload; decomp code passes integers as addresses.` \
  -DCMAKE_C_FLAGS="-U_FORTIFY_SOURCE -Wno-error=format-security" -DCMAKE_CXX_FLAGS="-Wno-error=format-security"

JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target Ghostship --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_ghostship.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
PKG="$STAGE/assets/modules/ghostship"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
# The port's own settings for our menu (MENU.md design of 2026-11-14), generated from its ImGui menu's CVar
# widgets; desktop-only and dev items filtered (tools/schema/lus_schema.py).
python3 "$ROOT/tools/schema/lus_schema.py" ghostship "$SRC" > "$PKG/data/settings_schema.json"
python3 "$ROOT/tools/schema/add_profiles.py" "$PKG/data/settings_schema.json" ghostship  # our own profiles, if any
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
# Read-only port files (libultraship's bundle dir): the port's archive and Torch's extraction config.
cp "$SRC/ghostship.o2r" "$SRC/config.yml" "$PKG/data/"
cp -r "$SRC/assets" "$PKG/data/"
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
