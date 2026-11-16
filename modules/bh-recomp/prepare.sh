#!/usr/bin/env bash
# Prepares build/upstream/body-harvest-recomp: pinned sources + upstream's own library patches + our Android patches +
# game code generated from your ROM.
#   modules/bh-recomp/prepare.sh "path/to/Body Harvest (USA).z64"   (a .zip holding the .z64 works too)
# Needs git, python3 (+venv, for the decomp's tools), clang, cmake, ninja, make, rsync, and the decomp's own build
# needs (IDO is fetched by its Makefile; binutils-mips-linux-gnu). The ELF and the generated code are derived from your
# ROM: keep them private.
#
# Why this looks different from the other recomp modules (danielgomesvieira2000 family, see docs/RECOMP_MODULES.md):
# - Upstream never hand-edits its submodules; tools/patch_all.py rewrites them in place (idempotent). We run it and
#   commit the result per submodule on branch plm-android, so our own `git am` series has a clean base to 3-way on.
# - The recompiler does not read the ROM directly: tools/regenerate.sh builds the decompilation (IDO, must match the
#   ROM byte for byte), fixes up its ELF and runs N64Recomp/RSPRecomp on that.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/bh-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches"       # shared plume/N64ModernRuntime/RecompFrontend patches
RT64PATCHES="$ROOT/modules/blastcorps-recomp/patches/rt64" # rt64 series that applies on this rt64 pin (5473732)
ROM_IN="$(realpath "$1")"
SRC="$ROOT/build/upstream/body-harvest-recomp"
PIN=711a2b6 # 0.1.0

[ -d "$SRC" ] || git clone -q https://github.com/danielgomesvieira2000/body-harvest-recomp.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" submodule update -q --init --recursive lib/N64ModernRuntime lib/RT64 lib/RecompFrontend lib/bh-decomp

# 1. Upstream's own submodule patches, committed (re-running prepare.sh: patch_all.py is idempotent and the commit is
#    skipped when nothing changed).
python3 "$SRC/tools/patch_all.py"
commit_all() { # dir message
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" add -A
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost commit -q -m "$2" || true
}
commit_all lib/RT64/src/contrib/plume "body-harvest-recomp tools/patch_all.py"
commit_all lib/RT64 "body-harvest-recomp tools/patch_all.py"
commit_all lib/N64ModernRuntime "body-harvest-recomp tools/patch_all.py"
commit_all lib/RecompFrontend "body-harvest-recomp tools/patch_all.py"

# 2. Our Android library patches on top.
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/RT64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/RT64 "$RT64PATCHES"
git -C "$SRC/lib/RT64" add src/contrib/plume && git -C "$SRC/lib/RT64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply lib/RecompFrontend "$LIBPATCHES/RecompFrontend"
apply . "$HERE/patches/body-harvest-recomp"

# 3. ROM (zip or z64) -> rom.z64, which regenerate.sh reads by default.
case "$ROM_IN" in
  *.zip) unzip -p "$ROM_IN" '*.z64' > "$SRC/rom.z64" ;;
  *) cp "$ROM_IN" "$SRC/rom.z64" ;;
esac

# 4. Decomp build -> ELF -> RecompiledFuncs/ (+ rsp). CC_CHECK=true: the decomp's Makefile syntax-checks every C file
#    with the host clang (-fsyntax-only), which modern clang rejects; the check produces no output, so skipping it
#    can't change the build. Memory-capped: other builds share this swapless machine.
systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" \
  -p MemorySwapMax=0 --setenv=MAKEFLAGS="CC_CHECK=true" --setenv=BH_JOBS="${JOBS:-4}" \
  bash "$SRC/tools/regenerate.sh" "$SRC/rom.z64"

# 5. Host file_to_c for the Android cross build (RT64_HOST_FILE_TO_C).
mkdir -p "$SRC/build-host-tools"
c++ -std=c++17 -O2 "$SRC/lib/RT64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
