#!/usr/bin/env bash
# Prepares build/upstream/PaperBoat: pinned sources + Android patches. No ROM needed: the game archive (pm64.o2r) is
# extracted from the player's ROM on the device at first start (patch 0001: silently, when the launcher passes
# PLM_ROM). Unlike the other libultraship ports, no host Torch step: PaperBoat's own archive (paperboat.o2r: port
# assets and shaders) is a plain zip that its CMake build makes with `cmake -E tar` (target GeneratePortO2R).
#
# Note: PaperBoat pins a libultraship fork (JeodC/libultraship, branch lus-converge), not Kenix3's; our two
# libultraship patches are the same as SpaghettiKart's and apply to it unchanged.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/paperboat"
SRC="$ROOT/build/upstream/PaperBoat"
PIN=9dc47846b

[ -d "$SRC" ] || git clone -q https://github.com/HarbourMasters/PaperBoat.git "$SRC"
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
git -C "$SRC/external/libultraship" checkout -q -B plm-android
git -C "$SRC/external/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/PaperBoat"/*.patch
echo "prepared: $SRC"
