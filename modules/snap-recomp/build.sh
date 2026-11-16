#!/usr/bin/env bash
# Builds the Snap64 Recomp (Pokemon Snap) module from build/upstream/Snap64Recomp (run prepare.sh first) and stages it
# for the APK. The output contains game code translated from the ROM: keep it private.
#   JOBS=4 BUILD_MEMORY_MAX=6G modules/snap-recomp/build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/snap-recomp"
SRC="$ROOT/build/upstream/Snap64Recomp"
H="$SRC/build-host-tools"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
OUT="$ROOT/build/modules/snap-recomp"
STAGE="$ROOT/build/modules/apk"

[ -f "$SRC/RecompiledFuncs/funcs.h" ] && [ -f "$SRC/RecompiledPatches/patches.c" ] \
  || { echo "no generated game code in $SRC (run modules/snap-recomp/prepare.sh)" >&2; exit 1; }
[ -x "$H/file_to_c" ] && [ -f "$H/aspMain.cpp" ] || { echo "no host file_to_c / aspMain.cpp (run prepare.sh)" >&2; exit 1; }
[ -f "$SDL2/lib/libSDL2.a" ] || { echo "no static SDL2 (run tools/deps/build_sdl2.sh)" >&2; exit 1; }

# Upstream's own Android cross build (its BUILDING.md step 16), with our changes:
# - NDK 26 / API 28 instead of r30 / API 29 (the app's floor; nothing in the port needs 29: zstd's qsort_r user is
#   already left out on Android by upstream's rt64 fork);
# - SNAP_EXTERNAL_SDL2 (our game patch): our static SDL2, matching the host app's SDL Java side, not lib/SDL shared;
# - SNAP_ASPMAIN_PREBUILT / RT64_FILE_TO_C_HOST: the two host outputs prepare.sh made (a cross build can't run them);
# - SNAP_ROM: upstream's configure insists on a ROM to recompile the microcode from even when it's prebuilt.
# No FreeType: this port has no RmlUi (its menus are drawn in the game's own font).
"$SDK_CMAKE/cmake" -S "$SRC" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_PROJECT_Snap64Recomp_INCLUDE="$HERE/module.cmake" \
  -DSNAP_EXTERNAL_SDL2=ON -DSDL2_DIR="$SDL2/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$SDL2" \
  -DSNAP_ROM="$SRC/pokemonsnap.z64" -DSNAP_ASPMAIN_PREBUILT="$H/aspMain.cpp" -DRT64_FILE_TO_C_HOST="$H/file_to_c" \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_DICTBUILDER=OFF

# No swap on the dev machine and other builds share it: bounded jobs in a memory-capped scope, so running out of
# memory kills this build, not the desktop (see modules/jfg-recomp/build.sh).
JOBS="${JOBS:-4}"
BUILD=("$SDK_CMAKE/cmake" --build "$OUT/cmake" --target Snap64Recomp --parallel "$JOBS")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 "${BUILD[@]}"
else
  "${BUILD[@]}"
fi

LIB="$OUT/cmake/libplm_snap-recomp.so"
[ -f "$LIB" ] || { echo "build produced no $LIB" >&2; exit 1; }
extra=$(find "$OUT/cmake" -maxdepth 3 -name '*.so' ! -name 'libplm_snap-recomp.so' | head -n 5)
[ -z "$extra" ] || { echo "module must be one self-contained .so; also found: $extra" >&2; exit 1; }

# Data: what upstream's Gradle build stages as the app's assets (android/app/build.gradle, stageAssets), which the
# shim copies into the game folder when missing: menu_text/ (title badge, pointer), the seen-shader list (RT64
# pre-warms those pipelines), the controller mappings, the mods/texture_packs READMEs and the licences.
PKG="$STAGE/assets/modules/snap-recomp"
rm -rf "$PKG" && mkdir -p "$PKG/data/menu_text" "$PKG/data/cache" "$PKG/data/mods" "$PKG/data/texture_packs" \
  "$STAGE/jniLibs/arm64-v8a"
cp "$LIB" "$STAGE/jniLibs/arm64-v8a/"
cp "$SRC"/menu_text/*.png "$PKG/data/menu_text/"
cp "$SRC/shaders/rt64-seen-shaders.bin" "$PKG/data/cache/"
cp "$SRC/assets/gamecontrollerdb.txt" "$PKG/data/"
cp "$SRC/mods/README.md" "$PKG/data/mods/"
cp "$SRC/texture_packs/README.txt" "$PKG/data/texture_packs/"
cp -r "$SRC/LICENSE" "$SRC/NOTICE.md" "$SRC/licenses" "$PKG/data/"
# HEAD~1: prepare.sh commits our one game patch on top of the upstream pin.
sed -e "s/@UPSTREAM_COMMIT@/$(git -C "$SRC" rev-parse "HEAD~1")/" "$HERE/manifest.json" > "$PKG/manifest.json"
echo "staged: $STAGE"
