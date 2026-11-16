#!/usr/bin/env bash
# Builds libultraship's Android dependencies (static, arm64) into build/deps/lus-android-arm64:
# libzip (zlib from the NDK; no bzip2/lzma/zstd/crypto), nlohmann_json, tinyxml2, spdlog, and the audio codecs
# the ports use (libogg, libvorbis). On desktop Linux these
# come from system packages; libultraship's android.cmake only fetches SDL.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
CMAKE="$ANDROID_HOME/cmake/3.30.3/bin/cmake"
NINJA="$ANDROID_HOME/cmake/3.30.3/bin/ninja"
PREFIX="$ROOT/build/deps/lus-android-arm64"
WORK="$ROOT/build/deps/lus-src"
mkdir -p "$WORK" "$PREFIX"

fetch() { # name repo tag
  [ -d "$WORK/$1" ] || git clone -q --depth 1 --branch "$3" "$2" "$WORK/$1"
}
build() { # name extra-cmake-args...
  local name="$1"; shift
  "$CMAKE" -S "$WORK/$name" -B "$WORK/$name/build-android" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
  "$CMAKE" --build "$WORK/$name/build-android" --parallel 8 >/dev/null
  "$CMAKE" --install "$WORK/$name/build-android" >/dev/null
  echo "built $name"
}

fetch libzip https://github.com/nih-at/libzip.git v1.10.1
fetch json https://github.com/nlohmann/json.git v3.11.3
fetch tinyxml2 https://github.com/leethomason/tinyxml2.git 10.0.0
fetch spdlog https://github.com/gabime/spdlog.git v1.14.1
fetch ogg https://github.com/xiph/ogg.git v1.3.5
fetch vorbis https://github.com/xiph/vorbis.git v1.3.7

build libzip -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF \
  -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF \
  -DBUILD_OSSFUZZ=OFF -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF
build json -DJSON_BuildTests=OFF -DJSON_Install=ON
build tinyxml2 -Dtinyxml2_BUILD_TESTING=OFF
build spdlog -DSPDLOG_BUILD_EXAMPLE=OFF -DSPDLOG_BUILD_TESTS=OFF
build ogg -DINSTALL_DOCS=OFF -DBUILD_TESTING=OFF
build vorbis -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" -DBUILD_TESTING=OFF
echo "prefix: $PREFIX"
