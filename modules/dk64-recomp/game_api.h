// The interface between a split recomp module and its libgame.so (docs/planning/ON_DEVICE_RECOMP.md, spike 2).
// game_entry.cpp (in libgame.so) fills it; game_loader.cpp (in the module) calls through it. Bump the version on any
// change: a libgame.so built for another version is refused instead of crashing.
#pragma once
#include <cstdint>

#include "recomp.h"
#include "librecomp/rsp.hpp"

#define PLM_GAME_API_VERSION 1

struct plm_game_api {
    uint32_t version;
    void (*entrypoint)(uint8_t* rdram, recomp_context* ctx);
    RspExitReason (*n_aspMain)(uint8_t* rdram, uint32_t ucode_addr);
    void (*register_overlays)();
    void (*register_patches)();
};
