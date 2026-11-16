#!/usr/bin/env bash
# Builds the on-device recomp toolkit's compiler: clang + lld from LLVM 17.0.6 for android-arm64, AArch64 target
# only (docs/planning/ON_DEVICE_RECOMP.md, spike 3).
#
#   tools/toolkit/build_llvm.sh        (sources: build/toolkit/src, see below; output: build/toolkit/)
#
# Why LLVM 17.0.6: the NDK we build modules with (r26) ships clang 17, so the device compiles generated code with
# the same compiler generation, flags and builtin headers as today's desktop builds.
# Sources: the llvm-project-17.0.6.src release tarball, only llvm/ clang/ lld/ cmake/ third-party/ extracted.
#
# Two stages, both memory-capped (32 GB, no swap):
#  1. host: llvm-tblgen and clang-tblgen for this PC (the cross build runs them to generate sources);
#  2. android: the LLVM driver build (one multi-call binary for clang and lld, LLVM_TOOL_LLVM_DRIVER_BUILD), plus
#     the static libraries, which package_toolkit links into the dlopen-able libplmcc.so.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TK="$ROOT/build/toolkit"
SRC="$TK/src"
: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
SDK_CMAKE="$ANDROID_HOME/cmake/3.30.3/bin"
JOBS="${JOBS:-12}"
[ -d "$SRC/llvm" ] || { echo "no LLVM sources in $SRC" >&2; exit 1; }

capped() {
  if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
    systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-20G}" -p MemorySwapMax=0 "$@"
  else
    "$@"
  fi
}

# Stage 1: host tablegen.
if [ ! -x "$TK/host/bin/clang-tblgen" ]; then
  "$SDK_CMAKE/cmake" -S "$SRC/llvm" -B "$TK/host" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
    -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
  capped "$SDK_CMAKE/cmake" --build "$TK/host" --target llvm-tblgen clang-tblgen --parallel "$JOBS"
fi

# Stage 2: android-arm64.
"$SDK_CMAKE/cmake" -S "$SRC/llvm" -B "$TK/android" -G Ninja -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64 \
  -DLLVM_HOST_TRIPLE=aarch64-linux-android -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-android28 \
  -DLLVM_TABLEGEN="$TK/host/bin/llvm-tblgen" -DCLANG_TABLEGEN="$TK/host/bin/clang-tblgen" \
  -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON -DLLVM_BUILD_TOOLS=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_UTILS=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
  -DCLANG_BUILD_TOOLS=ON -DCLANG_INCLUDE_TESTS=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_PIC=ON \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384"
capped "$SDK_CMAKE/cmake" --build "$TK/android" --target llvm-driver --parallel "$JOBS"
ls -la "$TK/android/bin/llvm"
