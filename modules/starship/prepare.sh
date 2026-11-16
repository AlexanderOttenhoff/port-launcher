#!/usr/bin/env bash
# Prepares build/upstream/Starship: pinned sources + Android patches, and packs the port's own archive
# (starship.o2r, no game content) with a host build of Torch. No ROM needed: the game archive (sf64.o2r) is extracted
# from the player's ROM on the device at first start.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/starship"
SRC="$ROOT/build/upstream/Starship"
PIN=3322643f

[ -d "$SRC" ] || git clone -q https://github.com/HarbourMasters/Starship.git "$SRC"
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
git -C "$SRC/libultraship" checkout -q -B plm-android
git -C "$SRC/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/Starship"/*.patch

# Host Torch, then pack starship.o2r (the port's assets) as the desktop build's GeneratePortO2R step does.
cmake -S "$SRC/tools/Torch" -B "$SRC/build-host-torch" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-torch" --target torch --parallel 12
(cd "$SRC" && build-host-torch/torch pack port starship.o2r o2r)
echo "prepared: $SRC"
