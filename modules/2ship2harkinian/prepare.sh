#!/usr/bin/env bash
# Prepares build/upstream/2ship2harkinian (2 Ship 2 Harkinian): pinned sources + Android patches. No ROM and no host
# tools needed:
# - the game archive (mm.o2r) is extracted from the player's ROM on the device at first start, by the ZAPD that 2Ship
#   links into the game (ZAPDLib);
# - the port's own archive (2ship.o2r: custom assets, no game content) is normally made at build time by a host ZAPD
#   ("botr" mode); the module makes it on the device instead, with the same linked-in ZAPD, from the upstream custom
#   assets it ships (plm_options.cpp), which avoids a host build of ZAPD and libultraship.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/2ship2harkinian"
SRC="$ROOT/build/upstream/2ship2harkinian"
PIN=fd9fb03bd

[ -d "$SRC" ] || git clone -q https://github.com/HarbourMasters/2ship2harkinian.git "$SRC"
git -C "$SRC" checkout -q "$PIN" && git -C "$SRC" submodule update -q --init --recursive
git -C "$SRC/libultraship" checkout -q -B plm-android
git -C "$SRC/libultraship" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/libultraship"/*.patch
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE/patches/2ship2harkinian"/*.patch
echo "prepared: $SRC"
