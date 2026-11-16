#!/usr/bin/env bash
# Stages everything the device needs to build DK64's libgame.so itself from the player's ROM (on-device recomp
# spike 3 + pipeline step "generate the C on the device") into build/toolkit/device/, to push to the device and run
# with its build_from_rom.sh.
#
#   tools/toolkit/stage_dk64_device.sh   (after tools/toolkit/build_llvm.sh, tools/toolkit/build_recompilers.sh and
#                                         PLM_SPLIT_GAME=1 modules/dk64-recomp/build.sh)
#
# Nothing in the bundle is derived from the ROM: it's what a published "recomp toolkit" + module would carry.
#   toolkit/   clang + lld (the LLVM driver binary and its names), clang's builtin headers
#   sysroot/   NDK headers, crt objects, libc/libm/libdl stubs, libc++ static, builtins, unwinder (all from NDK r26)
#   recomp/    bin/: N64Recomp, RSPRecomp, dk64_decompress for android-arm64 (build_recompilers.sh)
#              config/: the recompiler configs (us.toml, n_aspMain.toml) and the decomp's symbol file they name
#              (DK64Syms/dump.toml): committed upstream, no ROM bytes
#   prebuilt/  non-ROM parts of libgame.so that CI builds: the patches (PatchesLib, register_patches.o), game_entry.o,
#              and the module library to link against
#   game/      the hand-written sources and headers the generated code is compiled with (register_overlays.cpp,
#              recomp.h, the librecomp/ultramodern/sse2neon headers), in the upstream layout. The generated code
#              itself (RecompiledFuncs/, rsp/) is NOT here any more: build_from_rom.sh generates it on the device.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TK="$ROOT/build/toolkit"
OUT="$TK/device"
UP="$ROOT/build/upstream/Donkey-Kong-64-Recompiled"
MB="$ROOT/build/modules/dk64-recomp/cmake"
RB="$TK/recomp-android/bin"
N="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/26.0.10792818}/toolchains/llvm/prebuilt/linux-x86_64"
LLVM="$TK/android/bin/llvm"
[ -x "$LLVM" ] || { echo "no $LLVM (run tools/toolkit/build_llvm.sh)" >&2; exit 1; }
[ -x "$RB/N64Recomp" ] || { echo "no $RB/N64Recomp (run tools/toolkit/build_recompilers.sh)" >&2; exit 1; }
[ -f "$MB/libgame.so" ] || { echo "no split DK64 build (PLM_SPLIT_GAME=1 modules/dk64-recomp/build.sh)" >&2; exit 1; }

rm -rf "$OUT" && mkdir -p "$OUT"/{toolkit/bin,sysroot/usr/lib/aarch64-linux-android/28,prebuilt,game,recomp/bin,recomp/config}

# toolkit: one stripped multi-call binary; the driver picks the tool by argv[0], so the names are symlinks.
"$N/bin/llvm-strip" -o "$OUT/toolkit/bin/llvm" "$LLVM"
for t in clang clang++ ld.lld; do ln -s llvm "$OUT/toolkit/bin/$t"; done
mkdir -p "$OUT/toolkit/lib/clang/17/lib/linux/aarch64"
# clang 17 builtin headers (stddef.h, arm_neon.h...): the NDK's copy, same clang generation as the toolkit.
cp -r "$N/lib/clang/17/include" "$OUT/toolkit/lib/clang/17/"
cp "$N/lib/clang/17/lib/linux/libclang_rt.builtins-aarch64-android.a" "$OUT/toolkit/lib/clang/17/lib/linux/"
cp "$N/lib/clang/17/lib/linux/aarch64/"{libunwind.a,libatomic.a} "$OUT/toolkit/lib/clang/17/lib/linux/aarch64/"

# sysroot
cp -r "$N/sysroot/usr/include" "$OUT/sysroot/usr/"
L="$N/sysroot/usr/lib/aarch64-linux-android"
cp "$L"/{libc++_static.a,libc++abi.a} "$OUT/sysroot/usr/lib/aarch64-linux-android/"
cp "$L/28"/{crtbegin_so.o,crtend_so.o,libc.so,libm.so,libdl.so,liblog.so,libc++.a} "$OUT/sysroot/usr/lib/aarch64-linux-android/28/"
# The driver adds -latomic for the Android target's C++ link; it's found in the resource dir.

# prebuilt (CI-buildable, no ROM-derived code)
cp "$MB/libPatchesLib.a" "$OUT/prebuilt/"
cp "$MB/CMakeFiles/plm_game.dir/src/main/register_patches.cpp.o" "$OUT/prebuilt/register_patches.o"
cp "$MB/CMakeFiles/plm_game.dir/home/alexander/src/port-launcher/modules/dk64-recomp/game_entry.cpp.o" "$OUT/prebuilt/game_entry.o"
"$N/bin/llvm-strip" --strip-debug -o "$OUT/prebuilt/libplm_dk64-recomp.so" "$MB/libplm_dk64-recomp.so"

# recomp: the generators and what they read besides the ROM. us.toml names only DK64Syms/dump.toml (data_dump.toml
# isn't used); n_aspMain.toml reads only the decompressed ROM.
cp "$RB"/{N64Recomp,RSPRecomp,dk64_decompress} "$OUT/recomp/bin/"
( cd "$UP" && cp --parents us.toml n_aspMain.toml DK64Syms/dump.toml "$OUT/recomp/config/" )

# game: the non-generated sources and the headers the generated code includes (from ninja's deps log), in the
# upstream layout so the sources' relative includes keep working once the generated files land next to them.
( cd "$UP" && cp --parents src/main/register_overlays.cpp include/ovl_patches.hpp \
    lib/N64ModernRuntime/librecomp/include/librecomp/{overlays.hpp,rsp.hpp,rsp_vu.hpp,rsp_vu_impl.hpp,sections.h} \
    lib/N64ModernRuntime/N64Recomp/include/recomp.h lib/N64ModernRuntime/thirdparty/sse2neon/sse2neon.h \
    lib/N64ModernRuntime/ultramodern/include/ultramodern/ultra64.h "$OUT/game/" )
cp "$ROOT/tools/toolkit/build_libgame.sh" "$ROOT/tools/toolkit/build_from_rom.sh" "$OUT/"

# Guard: nothing generated from the ROM may be in the bundle (it's meant to be publishable).
leak=$(find "$OUT" \( -name 'funcs_*.c' -o -name 'recomp_overlays.inl' -o -name 'n_aspMain.cpp' -o -name 'funcs.h' \
  -o -name '*.z64' -o -path '*/RecompiledFuncs*' \) | head -n 5)
[ -z "$leak" ] || { echo "ROM-derived files in the bundle: $leak" >&2; exit 1; }
du -sh "$OUT"/* | sed "s|$OUT/||"

# The tarball the app installs (debug builds: RecompToolkit.kt picks it up, docs/planning/ON_DEVICE_RECOMP.md
# "Installing the toolkit"). A tarball because adb push stops at the toolkit's symlinks, and because the app keeps
# one file's SHA-1 as the toolkit version (part of libgame.so's cache key). Paths are relative ("./toolkit/..."), so
# it unpacks straight into files/recomp-toolkit/. Not gzipped: most of it is the clang binary, and the app untars
# 140 MB in seconds but would spend far longer inflating it on the phone than the transfer saves over USB.
tar -C "$OUT" -cf "$TK/plm-recomp-toolkit.tar" .
ls -l "$TK/plm-recomp-toolkit.tar"
