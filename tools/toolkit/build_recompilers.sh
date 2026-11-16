#!/usr/bin/env bash
# Builds the tools that turn a player's DK64 ROM into the generated C (docs/planning/ON_DEVICE_RECOMP.md, pipeline
# step "Generate the C on the device"):
#   N64Recomp        the game's MIPS code -> RecompiledFuncs/*.c        (N64Recomp's N64RecompCLI target)
#   RSPRecomp        the audio microcode -> rsp/n_aspMain.cpp
#   dk64_decompress  the DK64 project's own decompressor.cpp (zlib), which inflates the compressed overlays so the
#                    recompilers can read them
#
#   tools/toolkit/build_recompilers.sh            android-arm64 -> build/toolkit/recomp-android/bin
#   tools/toolkit/build_recompilers.sh host       this PC       -> build/toolkit/recomp-host/bin
#
# Why two flavours: the android build is what the device bundle carries (stage_dk64_device.sh). The host build of
# the same sources stands in for it on the desktop, so build_from_rom.sh can be tested end to end here (no device,
# no emulator) and its output diffed against the desktop prepare.sh run.
# Sources: the N64Recomp and decompressor.cpp of the pinned DK64 checkout (build/upstream, from prepare.sh), so the
# device generates exactly the code today's desktop builds generate. Only read, never modified.
#
# All three are plain C++20/C++17 with vendored deps (fmt, tomlplusplus, rabbitizer); zlib comes from the NDK
# sysroot (a stub of the system's /system/lib64/libz.so, which every Android has). libc++ is linked statically (the
# NDK toolchain's default c++_static), so the binaries need only libc/libm/libdl/libz from the system.
# 16 KB page alignment like every library we ship (spike 1): harmless for executables, required if these ever
# become dlopen'd libraries, which is the plan for stock Android (no exec from app storage).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FLAVOUR="${1:-android}"
UP="$ROOT/build/upstream/Donkey-Kong-64-Recompiled"
NR="$UP/lib/N64ModernRuntime/N64Recomp"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
NDK_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
OUT="$ROOT/build/toolkit/recomp-$FLAVOUR"
JOBS="${JOBS:-4}"
[ -f "$NR/CMakeLists.txt" ] || { echo "no N64Recomp in $NR (run modules/dk64-recomp/prepare.sh)" >&2; exit 1; }

# No swap on the dev machine and other builds running: a memory-capped scope, few jobs.
capped() {
  if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
    systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 "$@"
  else
    "$@"
  fi
}

case "$FLAVOUR" in
  android)
    CMAKE_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384")
    # Same compiler, target and flags as the CMake build gets from the NDK toolchain file.
    CXX=("$NDK_BIN/clang++" --target=aarch64-linux-android28 -static-libstdc++ -fPIE -pie
      -Wl,-z,max-page-size=16384)
    ;;
  host)
    CMAKE_ARGS=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
    CXX=(clang++)
    ;;
  *) echo "usage: $0 [android|host]" >&2; exit 1 ;;
esac

mkdir -p "$OUT/bin"
"$SDK_CMAKE/cmake" -S "$NR" -B "$OUT/cmake" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_BUILD_TYPE=Release "${CMAKE_ARGS[@]}" >/dev/null
capped "$SDK_CMAKE/cmake" --build "$OUT/cmake" --target N64RecompCLI RSPRecomp --parallel "$JOBS"
cp "$OUT/cmake/N64Recomp" "$OUT/cmake/RSPRecomp" "$OUT/bin/"
# decompressor.cpp is one file; prepare.sh builds it the same way for the host.
capped "${CXX[@]}" -std=c++17 -O2 "$UP/decompressor.cpp" -lz -o "$OUT/bin/dk64_decompress"
if [ "$FLAVOUR" = android ]; then
  for b in "$OUT/bin"/*; do "$NDK_BIN/llvm-strip" "$b"; done
fi
ls -l "$OUT/bin"
