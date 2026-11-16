#!/usr/bin/env bash
# Builds the 2 Ship 2 Harkinian module from build/upstream/2ship2harkinian (run prepare.sh first) and stages it.
#
# 2Ship needs libraries the shared libultraship set doesn't have: Opus and opusfile (audio) and libpng (its linked-in
# ZAPD extractor). modules/common/lus/build_extra_deps.sh builds them into this module's own build dir.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/2ship2harkinian"
SRC="$ROOT/build/upstream/2ship2harkinian"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
LUSDEPS="$ROOT/build/deps/lus-android-arm64" # tools/deps/build_lus_deps.sh
OUT="$ROOT/build/modules/2ship2harkinian"
DEPS="$OUT/deps"
STAGE="$ROOT/build/modules/apk"

[ -d "$LUSDEPS/lib" ] || { echo "no libultraship deps (run tools/deps/build_lus_deps.sh)" >&2; exit 1; }
[ -d "$SRC/ZAPDTR/ZAPD" ] || { echo "no sources in $SRC (run modules/2ship2harkinian/prepare.sh)" >&2; exit 1; }

"$ROOT/modules/common/lus/build_extra_deps.sh" "$DEPS" opus opusfile png

# The CMake project is called "2s2h"; the game target is "2ship" (in mm/).
"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_2s2h_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$SDL2;$LUSDEPS;$DEPS" -DCMAKE_PREFIX_PATH="$LUSDEPS;$DEPS" \
  `# opusfile installs its header as include/opus/opusfile.h, which 2Ship's FindOpusFile only looks for on desktop paths.` \
  -DOPUSFILE_INCLUDE_DIR="$DEPS/include/opus" -DOPUS_INCLUDE_DIR="$DEPS/include/opus" \
  -DUSE_OPENGLES=ON -DCPU_OPTION=-march=armv8-a -DUSE_NETWORKING=OFF \
  `# The NDK fortifies memcpy as an overload; decomp code passes integers as addresses.` \
  -DCMAKE_C_FLAGS="-U_FORTIFY_SOURCE -Wno-error=format-security" -DCMAKE_CXX_FLAGS="-Wno-error=format-security"

JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target 2ship --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/mm/libplm_2ship2harkinian.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
PKG="$STAGE/assets/modules/2ship2harkinian"
rm -rf "$PKG" && mkdir -p "$PKG/data/assets" "$STAGE/jniLibs/arm64-v8a"
# The port's own settings for our menu (MENU.md design of 2026-11-14), generated from its ImGui menu's CVar
# widgets; desktop-only and dev items filtered (tools/schema/lus_schema.py).
python3 "$ROOT/tools/schema/lus_schema.py" 2ship2harkinian "$SRC" > "$PKG/data/settings_schema.json"
python3 "$ROOT/tools/schema/add_profiles.py" "$PKG/data/settings_schema.json" 2ship2harkinian  # our own profiles, if any
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
# Read-only port files (libultraship's bundle dir), laid out as upstream's Linux install does: assets/ holds the
# extractor config (Config_*.xml, filelists) and the asset XMLs; custom/ is what the module packs into 2ship.o2r.
cp -r "$SRC/mm/assets/extractor/." "$PKG/data/assets/"
cp -r "$SRC/mm/assets/xml" "$PKG/data/assets/xml"
cp -r "$SRC/mm/assets/custom" "$PKG/data/custom"
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
