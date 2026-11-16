#!/system/bin/sh
# Runs ON THE DEVICE (on-device recomp spike 3; build_from_rom.sh calls it): compiles DK64's generated code with the toolkit's clang and links
# libgame.so, the same build as the plm_game target in modules/dk64-recomp/module.cmake, minus debug info.
#   sh build_libgame.sh [jobs]     (in the staged directory from tools/toolkit/stage_dk64_device.sh)
#   LINK_ONLY=1 sh build_libgame.sh   relinks the objects from the last run
# Times each phase, so the run doubles as the speed measurement.
# build_from_rom.sh runs this after generating the C, through these overrides:
#   GAME_DIR     the sources in the upstream layout (default: the bundle's game/). build_from_rom.sh points it at its
#                work dir, where the freshly generated RecompiledFuncs/ and rsp/ sit next to a copy of game/, so the
#                sources' relative includes (register_overlays.cpp -> ../../RecompiledFuncs/recomp_overlays.inl) work.
#   OUT_DIR      objects and libgame.so (default: out/)
#   TOOLKIT_BIN  where clang and clang++ are (default: the bundle's toolkit/bin). The desktop test points it at NDK
#                clang 17, which takes the same flags, sysroot and resource dir.
set -e
D=$(cd "$(dirname "$0")" && pwd)
JOBS=${1:-4}
OUT=${OUT_DIR:-$D/out}
[ "${LINK_ONLY:-0}" = 1 ] || { rm -rf "$OUT" && mkdir -p "$OUT"; }
G=${GAME_DIR:-$D/game}
TB=${TOOLKIT_BIN:-$D/toolkit/bin}
export CC="$TB/clang" CXX="$TB/clang++" OUT
# The desktop build's flags (compile_commands.json) without -g; -w because the generated code is warning-heavy.
export COMMON="--target=aarch64-linux-android28 --sysroot=$D/sysroot -resource-dir=$D/toolkit/lib/clang/17 -DANDROID -fdata-sections -ffunction-sections -funwind-tables -fstack-protector-strong -D_FORTIFY_SOURCE=2 -O2 -DNDEBUG -fPIC -fno-strict-aliasing -falign-functions=16 -Wno-implicit-function-declaration -w"
export INC="-I$G/include -I$G/lib/N64ModernRuntime/ultramodern/include -I$G/lib/N64ModernRuntime/librecomp/include -I$G/lib/N64ModernRuntime/N64Recomp/include -I$G/lib/N64ModernRuntime/thirdparty/sse2neon"

t0=$(date +%s)
# PLM_PROGRESS lines: see build_from_rom.sh. total = the C files + the two C++ files; one "file" line per finished
# compile, printed by the worker itself so the app's bar moves as files finish, whatever order -P runs them in.
if [ "${LINK_ONLY:-0}" != 1 ]; then
  echo "PLM_PROGRESS phase compile"
  echo "PLM_PROGRESS total $(($(ls "$G"/RecompiledFuncs/funcs_*.c | wc -l) + 2))"
  ls "$G"/RecompiledFuncs/funcs_*.c | xargs -P "$JOBS" -n 1 sh -c '$CC $COMMON -std=gnu17 $INC -c "$0" -o "$OUT/$(basename "$0").o" && echo "PLM_PROGRESS file $(basename "$0")"'
fi
t1=$(date +%s)
if [ "${LINK_ONLY:-0}" != 1 ]; then
  # Two background compiles, each waited for by pid: a bare `wait` returns 0 even when a job failed, which used to
  # let a broken C++ compile through to a confusing link error.
  pids=""
  for f in rsp/n_aspMain.cpp src/main/register_overlays.cpp; do
    { $CXX $COMMON -std=c++20 $INC -c "$G/$f" -o "$OUT/$(basename "$f").o" && echo "PLM_PROGRESS file $(basename "$f")"; } &
    pids="$pids $!"
  done
  for p in $pids; do wait "$p"; done
fi
t2=$(date +%s)
echo "PLM_PROGRESS phase link"
# The patches go before the generated code: a patched function is defined (weak) in both, and the linker keeps the
# first weak definition it sees (module.cmake has the full story).
$CXX $COMMON -shared -static-libstdc++ -fuse-ld=lld -Wl,--build-id=sha1 -Wl,--gc-sections -Wl,-Bsymbolic \
  -Wl,-z,max-page-size=16384 -Wl,--wrap=osViSwapBuffer_recomp -Wl,--no-undefined -Wl,-soname,libgame.so \
  -o "$OUT/libgame.so" -Wl,--whole-archive "$D/prebuilt/libPatchesLib.a" -Wl,--no-whole-archive "$OUT"/*.o \
  "$D/prebuilt/register_patches.o" "$D/prebuilt/game_entry.o" "$D/prebuilt/libplm_dk64-recomp.so" -latomic -lm
t3=$(date +%s)
echo "C files: $((t1 - t0)) s, C++ files: $((t2 - t1)) s, link: $((t3 - t2)) s, total: $((t3 - t0)) s (jobs $JOBS)"
ls -l "$OUT/libgame.so"
