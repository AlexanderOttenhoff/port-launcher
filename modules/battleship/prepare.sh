#!/usr/bin/env bash
# Prepares build/upstream/BattleShip (Super Smash Bros.): pinned sources + Android patches. BattleShip builds for
# Android upstream, on forks of libultraship and Torch (JRickey/*, branch ssb64) and a fork of the SSB64 decomp.
# No ROM and no host tools needed: the game archive (BattleShip.o2r) is extracted from the player's ROM on the device
# at first start by the Torch linked into the module (plm_options.cpp calls upstream's torch_extract_o2r), and the
# port's own archive (f3d.o2r: libultraship's shaders) is a zip the CMake build makes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/battleship"
SRC="$ROOT/build/upstream/BattleShip"
PIN=2148fc9

[ -d "$SRC" ] || git clone -q https://github.com/JRickey/BattleShip.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" submodule update -q --init libultraship torch decomp
git -C "$SRC/libultraship" checkout -q -B plm-android
git -C "$SRC/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
echo "prepared: $SRC"
