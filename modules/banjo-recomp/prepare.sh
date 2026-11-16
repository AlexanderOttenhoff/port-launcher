#!/usr/bin/env bash
# Prepares build/upstream/BanjoRecomp: pinned sources + Android patches + game code generated from your ROM.
#   modules/banjo-recomp/prepare.sh path/to/Banjo-Kazooie-US-1.0.z64
# Needs git, cargo, clang, ld.lld, cmake, ninja. The generated code is derived from your ROM: keep it private.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/banjo-recomp"
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/BanjoRecomp"
PIN=dbd5215

[ -d "$SRC" ] || git clone -q --recurse-submodules --shallow-submodules https://github.com/BanjoRecomp/BanjoRecomp.git "$SRC"
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
apply() { # dir patches-subdir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/$2"/*.patch
}
apply lib/rt64/src/contrib/plume plume
apply lib/rt64 rt64
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime N64ModernRuntime
apply lib/RecompFrontend RecompFrontend
apply . BanjoRecomp

# ROM: decompress with the decomp's tool, then generate the C code.
(cd "$SRC/lib/bk-decomp/tools/bk_rom_compressor" && cargo build -q --release --bin bk_rom_decompress)
"$SRC/lib/bk-decomp/tools/bk_rom_compressor/target/release/bk_rom_decompress" "$ROM" "$SRC/banjo.us.v10.decompressed.z64"
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel 12
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp banjo.us.rev0.toml && ./RSPRecomp n_aspMain.us.rev0.toml)
# Host file_to_c for the Android cross build.
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
