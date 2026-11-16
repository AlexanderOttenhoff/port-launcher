#!/usr/bin/env bash
# Prepares build/upstream/Shipwright (Ship of Harkinian): pinned sources + Android patches, and packs the port's own
# archive (soh.o2r: custom assets and libultraship's shaders, no game content) with a host build of upstream's
# soh-o2r-packer (its tools-only configure, SOH_TOOLS_ONLY), as the desktop build's GenerateSohOtr step does. No ROM
# needed: the game archive (oot.o2r) is extracted from the player's ROM on the device at first start.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/shipwright"
SRC="$ROOT/build/upstream/Shipwright"
PIN=d4bf6528a

[ -d "$SRC" ] || git clone -q https://github.com/HarbourMasters/Shipwright.git "$SRC"
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
git -C "$SRC/libultraship" checkout -q -B plm-android
git -C "$SRC/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/Shipwright"/*.patch

# Host soh-o2r-packer (links Torch), then pack soh.o2r exactly as upstream's GenerateSohOtr command does.
cmake -S "$SRC" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSOH_TOOLS_ONLY=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_SCAN_FOR_MODULES=OFF >/dev/null
# Memory-capped where systemd user scopes exist (the dev PC has no swap); CI runners have none.
TOOLS_BUILD=(cmake --build "$SRC/build-host-tools" --target soh-o2r-packer --parallel "${JOBS:-12}")
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
  systemd-run --user --scope -q -p MemoryMax=12G -p MemorySwapMax=0 "${TOOLS_BUILD[@]}"
else
  "${TOOLS_BUILD[@]}"
fi
VERSION=$(sed -n 's/^project(Ship VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
rm -rf "$SRC/soh/assets/custom/shaders"
cp -r "$SRC/libultraship/src/fast/shaders" "$SRC/soh/assets/custom/shaders"
"$SRC/build-host-tools/soh-o2r-packer" "$SRC/soh/assets/custom" "$SRC/soh.o2r" "$VERSION"
echo "prepared: $SRC (soh.o2r for SoH $VERSION)"
