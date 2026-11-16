#!/system/bin/sh
# Runs ON THE DEVICE: the whole on-device recomp pipeline for DK64, from the player's ROM to libgame.so
# (docs/planning/ON_DEVICE_RECOMP.md). Nothing ROM-derived exists before this runs, and nothing leaves the device.
#   sh build_from_rom.sh <rom.z64> [jobs]      (in the staged directory from tools/toolkit/stage_dk64_device.sh)
# Output: out/libgame.so. Times each phase, so a run doubles as the speed measurement.
#
# Phases:
#  1. check   the ROM's SHA-1 (DK64 USA, .z64 byte order). The decompressor would refuse other revisions too, but
#             the configs and symbols are made for exactly this file, so refuse anything else up front.
#  2. decompress  dk64_decompress inflates the compressed overlays. It always writes
#             donkeykong64.decompressed.us.z64 into the current directory, which is the name us.toml and
#             n_aspMain.toml expect next to themselves.
#  3. generate    N64Recomp us.toml -> RecompiledFuncs/ (funcs_*.c, funcs.h, recomp_overlays.inl, lookup.cpp);
#             RSPRecomp n_aspMain.toml -> rsp/n_aspMain.cpp. Both resolve paths relative to their config file and
#             create their output directories themselves.
#  4. compile + link   build_libgame.sh, pointed at the work dir.
#
# Work dir (work/): a copy of the bundle's game/ (hand-written sources and headers) plus recomp/config/, so the
# generated files land in the layout the sources include them from, and the bundle itself is never written to (on
# Android it will be the read-only toolkit/module install). The decompressed ROM (32 MB) is deleted once the
# generators are done; the generated C (~43 MB) is deleted at the end unless KEEP_WORK=1 (for
# diffing the generated code against a desktop run).
#
# Overrides for the desktop test (no device, no emulator): RECOMP_BIN (host builds of the three tools, from
# build_recompilers.sh host), TOOLKIT_BIN (NDK clang 17 in place of the device toolkit), WORK_DIR, OUT_DIR.
# POSIX sh only: the device runs it with mksh (/system/bin/sh); checked with dash and busybox sh.
#
# Progress for the app (the "Preparing Donkey Kong 64" screen, ROADMAP item 13; RecompBuilder.kt parses them):
#   PLM_PROGRESS phase <name>   a phase starts: check, copy, decompress, generate, compile, link, done
#   PLM_PROGRESS total <n>      how many compile units follow (build_libgame.sh)
#   PLM_PROGRESS file <name>    one compile unit finished (build_libgame.sh; arrives out of order with parallel jobs)
# Why a fixed prefix and not the human timing lines: those are for people and change wording; this is an interface.
# Everything else on stdout/stderr is free text that the app only keeps for the error screen's log tail.
set -e
progress() { echo "PLM_PROGRESS $*"; }
[ -n "$1" ] || { echo "usage: sh build_from_rom.sh <rom.z64> [jobs]" >&2; exit 2; }
D=$(cd "$(dirname "$0")" && pwd)
ROM=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
JOBS=${2:-4}
RB=${RECOMP_BIN:-$D/recomp/bin}
WORK=${WORK_DIR:-$D/work}
OUT=${OUT_DIR:-$D/out}
ROM_SHA1=cf806ff2603640a748fca5026ded28802f1f4a50 # Donkey Kong 64 (USA).z64
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 1; }

t0=$(date +%s)
progress phase check
if command -v sha1sum >/dev/null; then
  sum=$(sha1sum "$ROM" | cut -d' ' -f1)
  [ "$sum" = "$ROM_SHA1" ] || { echo "not the DK64 USA ROM (.z64): sha1 $sum, expected $ROM_SHA1" >&2; exit 1; }
else
  echo "no sha1sum: skipping the ROM check (the decompressor still refuses non-US ROMs)" >&2
fi

progress phase copy
rm -rf "$WORK" && mkdir -p "$WORK"
cp -R "$D/game/." "$WORK/"
cp -R "$D/recomp/config/." "$WORK/"
cd "$WORK"
t1=$(date +%s)
progress phase decompress
"$RB/dk64_decompress" "$ROM"
[ -f donkeykong64.decompressed.us.z64 ] || { echo "decompressor wrote no donkeykong64.decompressed.us.z64" >&2; exit 1; }
t2=$(date +%s)
progress phase generate
# The generators are chatty (one line per function); keep it in a log, show it only on failure.
"$RB/N64Recomp" us.toml >"$WORK/generate.log" 2>&1 || { tail -n 30 "$WORK/generate.log" >&2; exit 1; }
"$RB/RSPRecomp" n_aspMain.toml >>"$WORK/generate.log" 2>&1 || { tail -n 30 "$WORK/generate.log" >&2; exit 1; }
rm -f donkeykong64.decompressed.us.z64
t3=$(date +%s)
echo "generated: $(ls RecompiledFuncs | wc -l) files in RecompiledFuncs/, $(ls rsp | wc -l) in rsp/"

GAME_DIR="$WORK" OUT_DIR="$OUT" sh "$D/build_libgame.sh" "$JOBS"
t4=$(date +%s)
[ "${KEEP_WORK:-0}" = 1 ] || rm -rf "$WORK"
progress phase done
echo "pipeline: check+copy $((t1 - t0)) s, decompress $((t2 - t1)) s, generate $((t3 - t2)) s," \
  "compile+link $((t4 - t3)) s, total $((t4 - t0)) s (jobs $JOBS)"
