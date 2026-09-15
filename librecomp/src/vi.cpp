#include <ultramodern/ultramodern.hpp>
#include "recomp.h"
#include "helpers.hpp"

// ADR 0003: la familia VI principal (osCreateViManager, osViSetMode/Event/SwapBuffer, osViBlack,
// osViSetSpecialFeatures, osViGetCurrent/NextFramebuffer) se genera del ROM, que mantiene el
// OSViContext y los registros VI. El runtime solo conserva los stubs que sí se reimplementan y
// el helper de espera de frame.

extern "C" void osViSetYScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    osViSetYScale(ctx->f12.fl);
}

extern "C" void osViSetXScale_recomp(uint8_t* rdram, recomp_context * ctx) {
    osViSetXScale(ctx->f12.fl);
}

extern "C" void osViRepeatLine_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViRepeatLine(_arg<0, u8>(rdram, ctx));
}

extern uint64_t total_vis;

extern "C" void wait_one_frame(uint8_t* rdram, recomp_context* ctx) {
    uint64_t cur_vis = total_vis;
    while (cur_vis == total_vis) {
        std::this_thread::yield();
    }
}
