#!/usr/bin/env bash
# Builds the SDL2 the `sdl2` host is pinned to: SDL 2.32.10, static + PIC, for Android arm64, with its JNI
# bindings moved from org.libsdl.app to app.portlauncher.sdl2 so they match the host's Java glue
# (android/app/src/main/java/app/portlauncher/sdl2). Modules link this statically (docs/MODULE_SPEC.md §8).
#
#   tools/deps/build_sdl2.sh            -> build/deps/SDL2-2.32.10-android-arm64-plm
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION=2.32.10
SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
JNI_PREFIX=app_portlauncher_sdl2
JAVA_PATH=app/portlauncher/sdl2

: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
CMAKE="$ANDROID_HOME/cmake/3.30.3/bin/cmake"
WORK="$ROOT/build/deps/work"
PREFIX="$ROOT/build/deps/SDL2-$VERSION-android-arm64-plm"
mkdir -p "$WORK"
cd "$WORK"

[ -f "SDL2-$VERSION.tar.gz" ] || curl -fsSL --retry 3 -o "SDL2-$VERSION.tar.gz" \
  "https://github.com/libsdl-org/SDL/releases/download/release-$VERSION/SDL2-$VERSION.tar.gz"
echo "$SHA256  SDL2-$VERSION.tar.gz" | sha256sum -c -
rm -rf "SDL2-$VERSION" sdl2-build && tar -xzf "SDL2-$VERSION.tar.gz"

# Vulkan windows: don't create an EGL surface on the window RT64 presents to (from DKR-R via JFG).
patch --no-backup-if-mismatch -d "SDL2-$VERSION" -p1 < "$ROOT/tools/deps/patches/sdl2-vulkan-surface.patch"
# Sleep/wake (2026-11-16): the thread that renders rebinds the GL context to a recreated window surface, so ports
# that render on another thread than the one pumping events don't stay black after screen off/on.
patch --no-backup-if-mismatch -d "SDL2-$VERSION" -p1 < "$ROOT/tools/deps/patches/sdl2-rebind-on-swap.patch"
# Sleep/wake (2026-11-16, the real cause): with two threads pumping events (GoldenEye), both blocked on the one
# resume signal and only one woke; now one thread owns the pause wait and the others follow it ("plm-pause" logs).
patch --no-backup-if-mismatch -d "SDL2-$VERSION" -p1 < "$ROOT/tools/deps/patches/sdl2-pause-multithread.patch"

# SDL2 hard-codes its Java package in a JNI-name macro and in RegisterNatives class paths.
for f in src/core/android/SDL_android.c src/hidapi/android/hid.cpp; do
  sed -i -e "s|org_libsdl_app|$JNI_PREFIX|g" -e "s|org/libsdl/app/|$JAVA_PATH/|g" "SDL2-$VERSION/$f"
done
if grep -q 'org_libsdl_app\|org/libsdl/app/' "SDL2-$VERSION/src/core/android/SDL_android.c" \
     "SDL2-$VERSION/src/hidapi/android/hid.cpp"; then
  echo "unexpected org.libsdl.app references left in SDL sources" >&2; exit 1
fi

"$CMAKE" -S "SDL2-$VERSION" -B sdl2-build -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$ANDROID_HOME/cmake/3.30.3/bin/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF
"$CMAKE" --build sdl2-build --parallel
rm -rf "$PREFIX" && "$CMAKE" --install sdl2-build >/dev/null
echo "SDL2: $PREFIX"
