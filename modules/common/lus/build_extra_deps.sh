#!/usr/bin/env bash
# Builds the extra Android (arm64, static) libraries some Harbour Masters ports need beyond the shared libultraship
# set (tools/deps/build_lus_deps.sh), into a module's own prefix:
#   modules/common/lus/build_extra_deps.sh <prefix> <lib>...      libs: opus opusfile sdl2_net png
# Why per module and not in build/deps/lus-android-arm64: only Ship of Harkinian (opus, opusfile, SDL2_net) and
# 2Ship2Harkinian (opus, opusfile, libpng for its built-in ZAPD extractor) need them, and the shared prefix stays the
# set every libultraship module is built and tested against. Each library is built once (a stamp file per lib).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PREFIX="$1"; shift
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
SDL2="$ROOT/build/deps/SDL2-2.32.10-android-arm64-plm"
LUSDEPS="$ROOT/build/deps/lus-android-arm64"
LLVM="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
mkdir -p "$PREFIX/src"

cmake_dep() { # name repo tag extra-cmake-args...
  local name="$1" repo="$2" tag="$3"; shift 3
  [ -f "$PREFIX/.built-$name" ] && return
  [ -d "$PREFIX/src/$name" ] || git clone -q --depth 1 --branch "$tag" "$repo" "$PREFIX/src/$name"
  "$SDK_CMAKE/cmake" -S "$PREFIX/src/$name" -B "$PREFIX/src/$name/build-android" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
  "$SDK_CMAKE/cmake" --build "$PREFIX/src/$name/build-android" --parallel 8 >/dev/null
  "$SDK_CMAKE/cmake" --install "$PREFIX/src/$name/build-android" >/dev/null
  touch "$PREFIX/.built-$name"
  echo "built $name"
}

for lib in "$@"; do
  case "$lib" in
    opus)
      cmake_dep opus https://github.com/xiph/opus.git v1.5.2 -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF ;;
    opusfile)
      # The last release (0.12) has no CMake build (autotools only) and is four C files: compile them directly with
      # the NDK's clang, without HTTP support (needs OpenSSL; the ports don't use it). Needs opus first.
      if [ ! -f "$PREFIX/.built-opusfile" ]; then
        [ -d "$PREFIX/src/opusfile" ] || git clone -q --depth 1 --branch v0.12 https://github.com/xiph/opusfile.git "$PREFIX/src/opusfile"
        mkdir -p "$PREFIX/src/opusfile-obj" "$PREFIX/lib" "$PREFIX/include/opus"
        for f in info internal opusfile stream; do
          "$LLVM/aarch64-linux-android28-clang" -O2 -fPIC -DOP_DISABLE_HTTP -DOP_HAVE_LRINTF \
            -I"$PREFIX/src/opusfile/include" -I"$PREFIX/include/opus" -I"$LUSDEPS/include" \
            -c "$PREFIX/src/opusfile/src/$f.c" -o "$PREFIX/src/opusfile-obj/$f.o"
        done
        rm -f "$PREFIX/lib/libopusfile.a"
        "$LLVM/llvm-ar" rcs "$PREFIX/lib/libopusfile.a" "$PREFIX/src/opusfile-obj"/*.o
        cp "$PREFIX/src/opusfile/include/opusfile.h" "$PREFIX/include/opus/"
        touch "$PREFIX/.built-opusfile"
        echo "built opusfile"
      fi ;;
    sdl2_net)
      cmake_dep SDL2_net https://github.com/libsdl-org/SDL_net.git release-2.2.0 -DSDL2NET_SAMPLES=OFF \
        -DSDL2NET_INSTALL=ON -DSDL2_DIR="$SDL2/lib/cmake/SDL2" -DCMAKE_FIND_ROOT_PATH="$SDL2" ;;
    png)
      # zlib comes from the NDK.
      cmake_dep libpng https://github.com/pnggroup/libpng.git v1.6.44 -DPNG_SHARED=OFF -DPNG_STATIC=ON \
        -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_EXECUTABLES=OFF ;;
    *) echo "unknown extra dependency: $lib" >&2; exit 1 ;;
  esac
done
