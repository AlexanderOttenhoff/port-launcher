#!/usr/bin/env bash
# Prepares build/upstream/Snap64Recomp: pinned sources + our one game patch + game code generated from your ROM.
#   modules/snap-recomp/prepare.sh "path/to/Pokemon Snap (USA).z64"
# Needs git, python3, uv (the decomp's Python env), cargo (pigment64, the decomp's texture tool), mips-linux-gnu-ld/as
# (binutils-mips-linux-gnu), ninja, cmake, clang/clang++ and network access (github, PyPI, crates.io). The generated
# code is derived from your ROM: keep it private.
#
# Why this looks so different from the other recomps (BUILDING.md upstream, steps 1-9): Snap64 Recomp generates its
# code from an ELF, not from the ROM. The pipeline is
#   1. build the Pokemon Snap decompilation (ethteck/pokemonsnap) with SGI's IDO compiler (static recompilations the
#      decomp's own `configure.py --setup` downloads) until it matches the ROM byte for byte;
#   2. relink its ELF with --emit-relocs (N64Recomp's ELF mode needs the relocations; upstream did this by hand);
#   3. N64Recomp on that ELF, then tools/hook_funcs.py (renames the hooked functions; MSVC has no --wrap);
#   4. the port's game-side patches (patches/*.c) compiled with the decomp's IDO 7.1 against its headers, then
#      N64Recomp patches.toml, then hook_funcs.py again (finishes the patch table src/main.cpp includes);
#   5. the RSP audio microcode (aspMain.cpp) with RSPRecomp, and a host file_to_c: the cross build can't run either.
# Upstream already carries an Android build (its forks of rt64/N64ModernRuntime have the Android work: SDL-window
# Vulkan surface, RGBA swap chain, lifecycle, ARM64 icache flushes), so none of our shared library patches
# (modules/banjo-recomp/patches, modules/sk2-recomp/patches/rt64) are applied: they target the upstream libraries,
# not these forks, and the forks already solve what they solve. Only our one game patch is applied
# (patches/Snap64Recomp: our SDL2 instead of the vendored one; --rom/--skip-launcher).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/modules/snap-recomp"
ROM="$(realpath "$1")"
SRC="$ROOT/build/upstream/Snap64Recomp"
PIN=1a4e208 # main 2026-11 (1.1.7 + README)
DECOMP="$ROOT/build/upstream/pokemonsnap"
DECOMP_PIN=3a236dc # "Matched drawbitmap": the commit upstream generated against (BUILDING.md, prerequisites)
TOOLS="$ROOT/build/tools-host" # pigment64 (cargo install --root)
JOBS="${JOBS:-4}"
# No swap on the dev machine and other builds share it: every heavy step runs in a memory-capped scope.
capped() { systemd-run --user --scope -q -p MemoryMax="${BUILD_MEMORY_MAX:-6G}" -p MemorySwapMax=0 "$@"; }

echo "$(sha1sum "$ROM" | cut -d' ' -f1)" | grep -qx edc7c49cc568c045fe48be0d18011c30f393cbaf \
  || { echo "$ROM is not Pokemon Snap (USA) (sha1 edc7c49c...)" >&2; exit 1; }

# --- Sources -----------------------------------------------------------------------------------------------------
[ -d "$SRC" ] || git clone -q https://github.com/JackandBeans/Snap64Recomp.git "$SRC"
# An existing (e.g. shallow) checkout may not have the pin yet: fetch the full history once.
git -C "$SRC" cat-file -e "$PIN^{commit}" 2>/dev/null || git -C "$SRC" fetch -q --unshallow origin 2>/dev/null || git -C "$SRC" fetch -q origin
git -C "$SRC" checkout -q "$PIN"
git -C "$SRC" checkout -q -B plm-android
git -C "$SRC" -c user.name=plm -c user.email=plm@localhost am -q --3way "$HERE"/patches/Snap64Recomp/*.patch
# The 33 nested submodules at their recorded commits (one commit deep), verified; also swaps dxc-bin's dxil.dll for
# Microsoft's release file (Windows-only, harmless here). Idempotent: a second run is a no-op.
python3 "$SRC/tools/fetch_deps.py"

# --- 1. The decompilation, built with IDO until it matches ------------------------------------------------------------
[ -d "$DECOMP" ] || git clone -q https://github.com/ethteck/pokemonsnap.git "$DECOMP"
git -C "$DECOMP" checkout -q "$DECOMP_PIN"
cp "$ROM" "$DECOMP/pokemonsnap.z64"
# The patches include rt64_extended_gbi.h from the decomp's include/ (upstream keeps it there untracked).
cp "$SRC/lib/rt64/include/rt64_extended_gbi.h" "$DECOMP/include/"
[ -x "$TOOLS/bin/pigment64" ] || capped cargo install -q -j "$JOBS" --root "$TOOLS" pigment64
export PATH="$TOOLS/bin:$PATH"
(cd "$DECOMP" && uv sync -q && { [ -x tools/ido7.1/cc ] || uv run configure.py --setup; })
# Extraction runs once, from clean: a second `configure.py` with splat's cache (.splache) present skips the cached
# splits and rewrites undefined_syms_auto.txt without their symbols, and the relink below then fails with hundreds
# of undefined references (seen 2026-11-14). -c drops the cache and the old extraction first.
[ -f "$DECOMP/build.ninja" ] && [ -f "$DECOMP/build/pokemonsnap.elf" ] || (cd "$DECOMP" && uv run configure.py -c >/dev/null)
# ninja's last rule checks the rebuilt ROM's SHA-1 against checksum.sha1, so a finished build is a matching one.
(cd "$DECOMP" && capped uv run ninja -j "$JOBS")

# --- 2. Relink with relocations (upstream's derived command, BUILDING.md step 2) ---------------------------------
(cd "$DECOMP" && mips-linux-gnu-ld -T undefined_syms.txt -T undefined_syms_auto.txt \
  -Map build/pokemonsnap.relocs.map -T pokemonsnap.ld --emit-relocs -o build/pokemonsnap.relocs.elf)
# The check upstream asks for: .main (0x45270 bytes at ROM 0x1000) must match the ROM, or every address shifts.
cmp <(dd if="$DECOMP/build/pokemonsnap.relocs.elf" bs=1 skip=$((0x20400)) count=$((0x45270)) 2>/dev/null) \
    <(dd if="$ROM" bs=1 skip=$((0x1000)) count=$((0x45270)) 2>/dev/null)
# N64Recomp reads both from the port root under fixed names (its config paths are relative); the ELF's stem lands in
# the generated lookup.cpp, so keep the name.
cp "$DECOMP/build/pokemonsnap.relocs.elf" "$DECOMP/pokemonsnap.z64" "$SRC/"

# --- 3. N64Recomp (the runtime fork's vendored copy) + hooks -----------------------------------------------------
H="$SRC/build-host-tools"
cmake -S "$SRC/lib/N64ModernRuntime/N64Recomp" -B "$H" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
capped cmake --build "$H" --target N64Recomp RSPRecomp --parallel "$JOBS"
(cd "$SRC" && "$H/N64Recomp" pokemonsnap.us.toml >/dev/null && python3 -I tools/hook_funcs.py)
# The tracked overlay table must come out unchanged from this ELF (it did on 2026-11-14); a diff means the ELF
# doesn't match what upstream generated from, and the build would run with shifted sections.
(cd "$SRC" && python3 -I tools/gen_overlays.py pokemonsnap.relocs.elf && git diff --quiet -- src/recomp_overlays.inl) \
  || { echo "src/recomp_overlays.inl changed: the relocatable ELF differs from upstream's" >&2; exit 1; }

# --- 4. Game-side patches (IDO 7.1 from the decomp) --------------------------------------------------------------
# The second, resolved link prints "relocation truncated to fit" for calls into overlays and links anyway
# (--noinhibit-exec in patches/Makefile); those bytes only matter when a mod hooks a patched function.
capped make -s -C "$SRC/patches" -j "$JOBS" DECOMP="$DECOMP" 2>&1 | grep -v 'relocation truncated\|additional relocation overflows\|in function' || true
[ -f "$SRC/patches/build/patches.elf" ] && [ -f "$SRC/patches/build/patches.bin" ] || { echo "patch build failed" >&2; exit 1; }
(cd "$SRC" && "$H/N64Recomp" patches.toml >/dev/null && python3 -I tools/hook_funcs.py)

# --- 5. Host-side outputs the cross build can't make itself ------------------------------------------------------
# The audio microcode: rsp/aspMain.us.toml.in with the two paths filled in, as upstream's CMake does on a native build.
sed -e "s|@SNAP_ROM_CMAKE@|$SRC/pokemonsnap.z64|" -e "s|@SNAP_ASPMAIN_CPP@|$H/aspMain.cpp|" \
  "$SRC/rsp/aspMain.us.toml.in" > "$H/aspMain.us.toml"
"$H/RSPRecomp" "$H/aspMain.us.toml" >/dev/null
c++ -std=c++17 -O2 "$SRC/lib/rt64/src/tools/file_to_c/file_to_c.cpp" -o "$H/file_to_c"
echo "prepared: $SRC"
