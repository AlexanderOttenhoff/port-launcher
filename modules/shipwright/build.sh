#!/usr/bin/env bash
# Builds the Ship of Harkinian module from build/upstream/Shipwright (run prepare.sh first) and stages it for the APK.
#
# SoH needs three libraries the other libultraship ports don't: Opus and opusfile (audio, soh/soh/mixer.c) and
# SDL2_net (soh/soh/Network: remote control features, compiled in unconditionally). modules/common/lus/
# build_extra_deps.sh builds them, static and arm64, into this module's own build dir (build/modules/shipwright/deps).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/shipwright"
SRC="$ROOT/build/upstream/Shipwright"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
LUSDEPS="$ROOT/build/deps/lus-android-arm64" # tools/deps/build_lus_deps.sh
OUT="$ROOT/build/modules/shipwright"
DEPS="$OUT/deps"
STAGE="$ROOT/build/modules/apk"
TOOLCHAIN=(-G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja"
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28)

[ -d "$LUSDEPS/lib" ] || { echo "no libultraship deps (run tools/deps/build_lus_deps.sh)" >&2; exit 1; }
[ -f "$SRC/soh.o2r" ] || { echo "no soh.o2r in $SRC (run modules/shipwright/prepare.sh)" >&2; exit 1; }

# --- SoH-only dependencies (small; built once) ---
"$ROOT/modules/common/lus/build_extra_deps.sh" "$DEPS" opus opusfile sdl2_net

# --- the game ---
# The CMake project is called "Ship"; the game target is "soh" (in soh/).
"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" "${TOOLCHAIN[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_Ship_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$SDL2;$LUSDEPS;$DEPS" -DCMAKE_PREFIX_PATH="$LUSDEPS;$DEPS" \
  -DSDL2_net_DIR="$DEPS/lib/cmake/SDL2_net" \
  `# opusfile installs its header as include/opus/opusfile.h, which SoH's FindOpusFile only looks for on desktop paths.` \
  -DOPUSFILE_INCLUDE_DIR="$DEPS/include/opus" -DOPUS_INCLUDE_DIR="$DEPS/include/opus" \
  -DUSE_OPENGLES=ON -DCPU_OPTION=-march=armv8-a -DUSE_NETWORKING=OFF \
  `# The NDK fortifies memcpy as an overload; decomp code passes integers as addresses.` \
  -DCMAKE_C_FLAGS="-U_FORTIFY_SOURCE -Wno-error=format-security" -DCMAKE_CXX_FLAGS="-Wno-error=format-security"

JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target soh --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/soh/libplm_shipwright.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
PKG="$STAGE/assets/modules/shipwright"
rm -rf "$PKG" && mkdir -p "$PKG/data/assets" "$STAGE/jniLibs/arm64-v8a"
# The port's own settings for our menu (MENU.md design of 2026-11-14), generated from its ImGui menu's CVar
# widgets; desktop-only and dev items filtered (tools/schema/lus_schema.py).
python3 "$ROOT/tools/schema/lus_schema.py" shipwright "$SRC" > "$PKG/data/settings_schema.json"
python3 "$ROOT/tools/schema/add_profiles.py" "$PKG/data/settings_schema.json" shipwright  # our own profiles, if any
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
# Read-only port files (libultraship's bundle dir): the port's archive and Torch's extraction config (upstream
# installs soh/assets/yml as assets/; Extractor::CallTorch reads <bundle>/assets).
cp "$SRC/soh.o2r" "$PKG/data/"
cp -r "$SRC/soh/assets/yml/." "$PKG/data/assets/"
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
