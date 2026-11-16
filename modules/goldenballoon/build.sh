#!/usr/bin/env bash
# Builds the Golden Balloon (Diddy Kong Racing) module from build/upstream/goldenballoon (run prepare.sh first) and
# stages it for the APK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/goldenballoon"
SRC="$ROOT/build/upstream/goldenballoon"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
OUT="$ROOT/build/modules/goldenballoon"
STAGE="$ROOT/build/modules/apk"

[ -f "$SRC/CMakeLists.txt" ] || { echo "no sources in $SRC (run modules/goldenballoon/prepare.sh)" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

# The leanest configuration: the GL renderer only (no WebGPU), no ImGui app shell, no online play.
"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_mdkr64_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$SDL2" \
  -DMDKR_WEBGPU_BACKEND=OFF -DMDKR_APP=OFF -DMDKR_NATIVE_PHONE_PARTY=OFF -DMDKR_ENABLE_ONLINE_BETA=OFF \
  -DMDKR_ENABLE_ONLINE_ROOM_PREVIEW=OFF -DMDKR_BUILD_FUZZERS=OFF -DMDKR_ENABLE_GPU_TESTS=OFF -DBUILD_TESTING=OFF

JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target mdkr64 --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_goldenballoon.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
PKG="$STAGE/assets/modules/goldenballoon"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
# The port's own settings for our menu (MENU.md design of 2026-11-14), generated from its video-config table.
python3 "$ROOT/tools/schema/goldenballoon_schema.py" "$SRC/platform/video_config.c" > "$PKG/data/settings_schema.json"
python3 "$ROOT/tools/schema/add_profiles.py" "$PKG/data/settings_schema.json" goldenballoon  # our own profiles, if any
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
