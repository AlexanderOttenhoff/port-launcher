#!/usr/bin/env bash
# Prepares build/upstream/Ghostship: pinned sources + Android patches, and packs the port's own archive
# (ghostship.o2r: port assets, no game content) with a host build of Torch. No ROM needed:
# the game archive (sm64.o2r: patch 0001 runs GenerateOTR(..., "sm64")) is extracted from the player's ROM on the
# device at first start.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/ghostship"
SRC="$ROOT/build/upstream/Ghostship"
PIN=aefb89b

[ -d "$SRC" ] || git clone -q https://github.com/HarbourMasters/Ghostship.git "$SRC"
git -C "$SRC" fetch -q --unshallow 2>/dev/null || true
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
git -C "$SRC/libultraship" checkout -q -B plm-android
git -C "$SRC/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/Ghostship"/*.patch

# Host Torch, then pack ghostship.o2r as the desktop build's GeneratePortO2R step does.
cmake -S "$SRC/Torch" -B "$SRC/build-host-torch" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF >/dev/null
systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-torch" --target torch --parallel 12
(cd "$SRC" && build-host-torch/torch pack port ghostship.o2r o2r -u 3.0.0)
echo "prepared: $SRC"
