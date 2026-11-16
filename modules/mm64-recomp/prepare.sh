#!/usr/bin/env bash
# Prepares build/upstream/MegaMan64Recompiled: pinned sources + Android patches + game code generated from your ROM.
#   modules/mm64-recomp/prepare.sh "path/to/Mega Man 64 (USA).z64"
# Needs git, clang, ld.lld, make, cmake, ninja. No Python and no IDO/KMC: the recompiler reads the retail ROM directly
# (no decompression step) and the project's MIPS patches are built with clang. The generated code is derived from your
# ROM: keep it private.
#
# Why this project needs no library forward-port although it's a "B2" (own Zelda64Recomp-dev style UI): its rt64 pin
# (71b5906, 2026-02, branch rect-aspect-gbi) is already post-plume, and N64ModernRuntime (9ae9dbb, 2026-01) is close
# enough to Banjo's that the shared patch series apply unchanged. See docs/RECOMP_MODULES.md "B2 projects".
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/mm64-recomp"
LIBPATCHES="$ROOT/modules/banjo-recomp/patches" # shared plume/N64ModernRuntime patches
# rt64: Snowboard Kids 2's re-anchored series (CMake host tools, Android window/RGBA8, present fixes, background
# re-attach, the 4096 texture-set fix) applies cleanly on this rt64 too; it has no "move the plume pointer" patch,
# the patched plume is committed below instead. Reused from sk2 rather than copied, so a fix lands in one place.
RT64PATCHES="$ROOT/modules/sk2-recomp/patches/rt64"
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/MegaMan64Recompiled"
PIN=62d2e3e # v0.9.1

[ -d "$SRC" ] || git clone -q https://github.com/MegaMan64Recomp/MegaMan64Recompiled.git "$SRC"
git -C "$SRC" checkout -q "$PIN"
# Everything but freetype-windows-binaries (Android links our own FreeType prefix). The decomp is only read for the
# patches' headers (patches/Makefile), never built; its splat/permuter tool submodules aren't needed.
git -C "$SRC" submodule update -q --init --recursive --depth 1 lib/N64ModernRuntime lib/rt64 lib/RmlUi lib/lunasvg
git -C "$SRC" submodule update -q --init --depth 1 lib/MegaMan64Decomp MegaMan64RecompSyms
apply() { # dir patches-dir
  git -C "$SRC/$1" checkout -q -B plm-android
  git -C "$SRC/$1" -c user.name=plm -c user.email=plm@localhost am -q --3way "$2"/*.patch
}
apply lib/rt64/src/contrib/plume "$LIBPATCHES/plume"
apply lib/rt64 "$RT64PATCHES"
git -C "$SRC/lib/rt64" add src/contrib/plume && git -C "$SRC/lib/rt64" -c user.name=plm -c user.email=plm@localhost commit -q -m "plume: Android patches" || true
apply lib/N64ModernRuntime "$LIBPATCHES/N64ModernRuntime"
apply . "$HERE/patches/MegaMan64Recompiled"

# ROM: no decompression; us.rev1.toml and aspMain.us.rev1.toml read the retail ROM under this name.
cp "$ROM" "$SRC/MegaMan64.us.rev1.z64"
mkdir -p "$SRC/build-host-tools" "$SRC/rsp"
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$SRC/build-host-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
# Host tools are small, but other agents/builds share this 32 GB swapless machine: cap them like everything else.
systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 \
  cmake --build "$SRC/build-host-tools" --target N64Recomp RSPRecomp --parallel "${JOBS:-4}"
# Copied into the source dir: the game's own CMake runs ./N64Recomp there for the patches (patches.toml).
cp "$SRC/build-host-tools/N64Recomp" "$SRC/build-host-tools/RSPRecomp" "$SRC/"
(cd "$SRC" && ./N64Recomp us.rev1.toml && ./RSPRecomp aspMain.us.rev1.toml)
# Host file_to_c for the Android cross build (RT64_HOST_FILE_TO_C).
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$SRC/build-host-tools/file_to_c"
echo "prepared: $SRC"
