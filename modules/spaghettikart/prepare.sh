#!/usr/bin/env bash
# Prepares build/upstream/SpaghettiKart: pinned sources + Android patches, and packs the port's own archive
# (spaghetti.o2r: port assets and shaders, no game content) with a host build of Torch, as the desktop build's
# GenerateO2R step does. No ROM needed: the game archive (mk64.o2r) is extracted from the player's ROM on the device
# at first start (patch 0001: silently, when the launcher passes PLM_ROM).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/spaghettikart"
SRC="$ROOT/build/upstream/SpaghettiKart"
PIN=ce0f9358f

[ -d "$SRC" ] || git clone -q https://github.com/HarbourMasters/SpaghettiKart.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
# Only the submodules the build needs: lib/wasm-micro-runtime (unused: load_wasm() is commented out in Game.cpp),
# tools/blender/fast64 (a Blender add-on) and the doxygen theme stay out.
git -C "$SRC" submodule update -q --init --recursive libultraship torch
git -C "$SRC/libultraship" checkout -q -B plm-android
git -C "$SRC/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/SpaghettiKart"/*.patch

# Host Torch, then pack spaghetti.o2r from assets/ (upstream's cmake/GenerateO2R.cmake does the same on desktop).
# MK64 support only: the other games' Torch code isn't needed for packing and only costs build time.
cmake -S "$SRC/torch" -B "$SRC/build-host-torch" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
  -DUSE_STANDALONE=ON -DBUILD_MK64=ON -DBUILD_SM64=OFF -DBUILD_SF64=OFF -DBUILD_FZERO=OFF >/dev/null
# Memory-capped where systemd user scopes exist (the dev PC has no swap); CI runners have none, so build plainly there.
TORCH_BUILD=(cmake --build "$SRC/build-host-torch" --target torch --parallel "${JOBS:-12}")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 "${TORCH_BUILD[@]}"
else
  "${TORCH_BUILD[@]}"
fi
(cd "$SRC" && build-host-torch/torch pack assets spaghetti.o2r o2r)
echo "prepared: $SRC"
