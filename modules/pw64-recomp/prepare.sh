#!/usr/bin/env bash
# Prepares build/upstream/pilotwings-64-recomp: pinned sources + upstream's own library patches + our Android patches +
# game code generated from your ROM.
#   modules/pw64-recomp/prepare.sh "path/to/Pilotwings 64 (USA).z64"   (a .zip holding the .z64 works too)
# Needs git, python3 (+venv, for the decomp's splat), clang + ld.lld with the MIPS backend (the C patches), cmake,
# ninja, make, and the decomp's own build needs (IDO is built by its tools/ Makefile; binutils-mips-linux-gnu). The
# ELF and the generated code are derived from your ROM: keep them private.
#
# Same author and layout as modules/bh-recomp (danielgomesvieira2000 family, see docs/RECOMP_MODULES.md); Body
# Harvest's pipeline was copied from this one, so the differences are only in the script names:
# - Upstream never hand-edits its submodules; tools/patch_all.py rewrites them in place (idempotent). We run it and
#   commit the result per submodule on branch plm-android, so our own `git am` series has a clean base to 3-way on.
# - The recompiler does not read the ROM directly: tools/generate_game.py verifies the dump, builds the
#   decompilation (IDO, must match the ROM byte for byte, RECOMP_BUILD=1 so static symbols survive) into
#   pilotwings64.us.elf, runs N64Recomp/RSPRecomp on it (tools/recompile.sh) and builds the C patches
#   (tools/build_patches.py: host clang -target mips, ld.lld, N64Recomp again -> RecompiledPatches/).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/pw64-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches"       # shared plume/N64ModernRuntime/RecompFrontend patches
# rt64 series for this pin (stock rt64 5473732 + patch_all's two files): Snowboard Kids 2's re-anchored series, as
# bm64/mm64 use. Blast Corps' series (which bh-recomp names) does not apply here: its RGBA8 swap-chain hunk is anchored
# on Blast Corps' fork, where presentWait follows textureCount directly; stock rt64 has a FIXME comment in between.
# sk2's series also has no plume-pointer patch (we commit the patched plume into rt64 below).
RT64PATCHES="$ROOT/modules/sk2-recomp/patches/rt64"
ROM_IN="$(realpath "$1")"
SRC="$ROOT/build/upstream/pilotwings-64-recomp"
PIN=ce3634a # v0.1.1
JOBS="${JOBS:-4}"

[ -d "$SRC" ] || git clone -q https://github.com/danielgomesvieira2000/pilotwings-64-recomp.git "$SRC"
# -f / --force: a re-run starts from the pinned trees again (our `git am` series are not idempotent; the plm-android
# branches are simply re-created below). Untracked outputs (RecompiledFuncs/, the recompiler builds) survive.
git -C "$SRC" am --abort 2>/dev/null || true
git -C "$SRC" checkout -q -f "$PIN"
git -C "$SRC" submodule update -q --init --recursive --force

# 1. Upstream's own submodule patches, committed (re-running prepare.sh: patch_all.py is idempotent and the commit is
#    skipped when nothing changed). patch_rsprecomp.py edits N64Recomp, a submodule of N64ModernRuntime; that edit
#    only matters to the host RSPRecomp build below and is left uncommitted (nothing of ours applies on top of it).
python3 "$SRC/tools/patch_all.py"
commit_all() { # dir message
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" add -A
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost commit -q -m "$2" || true
}
commit_all lib/RT64/src/contrib/plume "pilotwings-64-recomp tools/patch_all.py"
commit_all lib/RT64 "pilotwings-64-recomp tools/patch_all.py"
commit_all lib/N64ModernRuntime "pilotwings-64-recomp tools/patch_all.py"
commit_all lib/RecompFrontend "pilotwings-64-recomp tools/patch_all.py"

# 2. Our Android library patches on top.
apply() { # dir patches-dir
  git -C "$SRC/$1" am --abort 2>/dev/null || true # a failed earlier run leaves its am session behind
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/RT64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/RT64 "$RT64PATCHES"
git -C "$SRC/lib/RT64" add src/contrib/plume && git -C "$SRC/lib/RT64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
apply . "$HERE/patches/pilotwings-64-recomp"

# 3. Recompilers, decomp build -> ELF -> RecompiledFuncs/ (+ rsp) -> RecompiledPatches/, all in one memory-capped
#    scope (other builds share this swapless machine).
#    - generate_game.py takes the zip or the .z64 itself (it unpacks, byte-swaps and checks the SHA-1).
#    - build_elf.sh runs `make -j$(nproc)` (32 here) and wsl_build_recompiler.sh a bare `cmake --build` (ninja's own
#      nproc+2); a PATH shim answers nproc with $JOBS and CMAKE_BUILD_PARALLEL_LEVEL caps the cmake builds, so the
#      job cap holds without editing upstream's scripts.
#    - CC_CHECK=true: the decomp's Makefile syntax-checks every C file with the host compiler (-fsyntax-only), which
#      modern clang/gcc can reject; the check writes nothing, so skipping it can't change the build. Harmless if the
#      Makefile has no such check.
SHIM="$SRC/build-host-tools/jobs-shim"
mkdir -p "$SHIM"
printf '#!/bin/sh\necho %s\n' "$JOBS" > "$SHIM/nproc" && chmod +x "$SHIM/nproc"
systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 \
  --setenv=PATH="$SHIM:$PATH" --setenv=MAKEFLAGS="CC_CHECK=true" --setenv=CMAKE_BUILD_PARALLEL_LEVEL="$JOBS" \
  bash -c 'cd "$1" && bash tools/wsl_build_recompiler.sh && python3 tools/generate_game.py "$2"' _ "$SRC" "$ROM_IN"

# 4. Host file_to_c for the Android cross build (RT64_HOST_FILE_TO_C).
c++ -std=c++17 -O2 "$SRC/lib/RT64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
