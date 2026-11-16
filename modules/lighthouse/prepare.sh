#!/usr/bin/env bash
# Prepares build/upstream/Lighthouse: pinned sources + Android patches, and packs the port's own archive
# (lighthouse.o2r: port assets plus libultraship's shaders, no game content) with a host build of Torch. No ROM needed:
# the game archive (bk.o2r) is extracted from the player's ROM on the device at first start.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/lighthouse"
SRC="$ROOT/build/upstream/Lighthouse"
PIN=0b59fde5

[ -d "$SRC" ] || git clone -q https://github.com/HarbourMasters/Lighthouse.git "$SRC"
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
git -C "$SRC/libultraship" checkout -q -B plm-android
git -C "$SRC/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/Lighthouse"/*.patch

# Host Torch, then pack lighthouse.o2r as the desktop build's GeneratePortO2R step does.
cmake -S "$SRC/Torch" -B "$SRC/build-host-torch" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-torch" --target torch --parallel 12
rm -rf "$SRC/build-host-torch/port_staging"
cp -r "$SRC/port" "$SRC/build-host-torch/port_staging"
cp -r "$SRC/libultraship/src/fast/shaders" "$SRC/build-host-torch/port_staging/shaders"
(cd "$SRC" && build-host-torch/torch pack build-host-torch/port_staging lighthouse.o2r o2r)
echo "prepared: $SRC"
