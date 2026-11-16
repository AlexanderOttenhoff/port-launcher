// Game side of the recomp split (docs/planning/ON_DEVICE_RECOMP.md, spike 2): the one export of libgame.so, handing
// the module (game_loader.cpp) the five functions it needs. libgame.so is RecompiledFuncs + the RSP microcode +
// the overlay and patch tables (register_overlays.cpp, register_patches.cpp, PatchesLib) + this file. It is linked
// -Bsymbolic against libplm_dk64-recomp.so, so its calls among its own functions never bind to the module's
// same-named trampolines, and its calls to the runtime resolve to the module.
#include <cstdint>

#include "recomp.h"
#include "librecomp/rsp.hpp"
#include "game_api.h"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr);
namespace dk64 {
void register_bk_overlays();
void register_bk_patches();
}

extern "C" __attribute__((visibility("default"))) const plm_game_api* plm_game_get_api() {
    static const plm_game_api api{
        PLM_GAME_API_VERSION,
        recomp_entrypoint,
        n_aspMain,
        dk64::register_bk_overlays,
        dk64::register_bk_patches,
    };
    return &api;
}
