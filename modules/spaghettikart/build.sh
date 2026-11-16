#!/usr/bin/env bash
# Builds the SpaghettiKart module from build/upstream/SpaghettiKart (run prepare.sh first) and stages it for the APK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/spaghettikart"
SRC="$ROOT/build/upstream/SpaghettiKart"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
LUSDEPS="$ROOT/build/deps/lus-android-arm64" # tools/deps/build_lus_deps.sh
OUT="$ROOT/build/modules/spaghettikart"
STAGE="$ROOT/build/modules/apk"

[ -d "$LUSDEPS/lib" ] || { echo "no libultraship deps (run tools/deps/build_lus_deps.sh)" >&2; exit 1; }
[ -f "$SRC/spaghetti.o2r" ] || { echo "no spaghetti.o2r in $SRC (run modules/spaghettikart/prepare.sh)" >&2; exit 1; }

# The CMake project is called "Spaghettify" (upstream's executable name), hence the include variable's name.
"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_Spaghettify_INCLUDE="$HERE/module.cmake" \
  -DSDL2_DIR="$SDL2/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$SDL2;$LUSDEPS" -DCMAKE_PREFIX_PATH="$LUSDEPS" \
  -DUSE_OPENGLES=ON -DCPU_OPTION=-march=armv8-a -DUSE_NETWORKING=OFF \
  `# The NDK fortifies memcpy as an overload; decomp code passes integers as addresses.` \
  -DCMAKE_C_FLAGS="-U_FORTIFY_SOURCE -Wno-error=format-security" -DCMAKE_CXX_FLAGS="-Wno-error=format-security"

JOBS="${JOBS:-12}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target Spaghettify --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_spaghettikart.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
PKG="$STAGE/assets/modules/spaghettikart"
rm -rf "$PKG" && mkdir -p "$PKG/data" "$STAGE/jniLibs/arm64-v8a"
# The port's own settings for our menu (MENU.md design of 2026-11-14), generated from its ImGui menu's CVar
# widgets; desktop-only and dev items filtered (tools/schema/lus_schema.py).
python3 "$ROOT/tools/schema/lus_schema.py" spaghettikart "$SRC" > "$PKG/data/settings_schema.json"
python3 "$ROOT/tools/schema/add_profiles.py" "$PKG/data/settings_schema.json" spaghettikart  # our own profiles, if any
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
# Read-only port files (libultraship's bundle dir): the port's archive and Torch's extraction config (config.yml,
# yamls/, and meta/mods.toml, which the extractor adds to mk64.o2r).
cp "$SRC/spaghetti.o2r" "$SRC/config.yml" "$PKG/data/"
cp -r "$SRC/yamls" "$SRC/meta" "$PKG/data/"
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse HEAD)/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
