#include "ultramodern/ultramodern.hpp"

#include <cstdio>
#include <cstdlib>

#include "helpers.hpp"

#define MAXCONTROLLERS 4

extern "C" void recomp_set_current_frame_poll_id(uint8_t* rdram, recomp_context* ctx) {
    // TODO reimplement the system for tagging polls with IDs to handle games with multithreaded input polling.
}

extern "C" void recomp_measure_latency(uint8_t* rdram, recomp_context* ctx) {
    ultramodern::measure_input_latency();
}

extern "C" void osContInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);
    PTR(u8) bitpattern = _arg<1, PTR(u8)>(rdram, ctx);
    PTR(OSContStatus) data = _arg<2, PTR(OSContStatus)>(rdram, ctx);
    u8 bitpattern_local = 0;

    s32 ret = osContInit(PASS_RDRAM mq, &bitpattern_local, data);

    MEM_B(0, bitpattern) = bitpattern_local;

    _return<s32>(ctx, ret);
}

extern "C" void osContReset_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);
    PTR(OSContStatus) data = _arg<1, PTR(OSContStatus)>(rdram, ctx);

    s32 ret = osContReset(PASS_RDRAM mq, data);

    _return<s32>(ctx, ret);
}

extern "C" void osContStartReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);

    s32 ret = osContStartReadData(PASS_RDRAM mq);

    _return<s32>(ctx, ret);
}

extern "C" void osContGetReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSContPad) data = _arg<0, PTR(OSContPad)>(rdram, ctx);

    OSContPad dummy_data[MAXCONTROLLERS];

    osContGetReadData(dummy_data);

    if (std::getenv("HH_INLOG") != nullptr) {
        static uint16_t pad_last = 0;
        if (dummy_data[0].button != pad_last) {
            std::fprintf(stderr, "[PAD] c0 button=0x%04X err=%u\n", dummy_data[0].button, (unsigned)dummy_data[0].err_no);
            pad_last = dummy_data[0].button;
        }
    }

    for (int controller = 0; controller < MAXCONTROLLERS; controller++) {
        // libultra writes an OSContPad for every controller, including absent ones (with
        // err_no = CONT_NO_RESPONSE_ERROR). Skipping absent controllers leaves stale values
        // in the game's buffer, which game logic can misread as "controller connected".
        MEM_H(6 * controller + 0, data) = dummy_data[controller].button;
        MEM_B(6 * controller + 2, data) = dummy_data[controller].stick_x;
        MEM_B(6 * controller + 3, data) = dummy_data[controller].stick_y;
        MEM_B(6 * controller + 4, data) = dummy_data[controller].err_no;
    }
}

extern "C" void osContStartQuery_recomp(uint8_t * rdram, recomp_context * ctx) {
    PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);

    s32 ret = osContStartQuery(PASS_RDRAM mq);

    _return<s32>(ctx, ret);
}

extern "C" void osContGetQuery_recomp(uint8_t * rdram, recomp_context * ctx) {
    PTR(OSContStatus) data = _arg<0, PTR(OSContStatus)>(rdram, ctx);

    osContGetQuery(PASS_RDRAM data);
}

extern "C" void osContSetCh_recomp(uint8_t* rdram, recomp_context* ctx) {
    u8 ch = _arg<0, u8>(rdram, ctx);

    s32 ret = osContSetCh(PASS_RDRAM ch);

    _return<s32>(ctx, ret);
}

extern "C" void __osMotorAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);
    s32 flag = _arg<1, s32>(rdram, ctx);

    s32 ret = __osMotorAccess(PASS_RDRAM pfs, flag);

    _return<s32>(ctx, ret);
}

extern "C" void osMotorInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSMesgQueue) mq = _arg<0, PTR(OSMesgQueue)>(rdram, ctx);
    PTR(OSPfs) pfs = _arg<1, PTR(OSPfs)>(rdram, ctx);
    int channel = _arg<2, s32>(rdram, ctx);

    s32 ret = osMotorInit(PASS_RDRAM mq, pfs, channel);

    _return<s32>(ctx, ret);
}

extern "C" void osMotorStart_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);

    s32 ret = osMotorStart(PASS_RDRAM pfs);

    _return<s32>(ctx, ret);
}

extern "C" void osMotorStop_recomp(uint8_t* rdram, recomp_context* ctx) {
    PTR(OSPfs) pfs = _arg<0, PTR(OSPfs)>(rdram, ctx);

    s32 ret = osMotorStop(PASS_RDRAM pfs);

    _return<s32>(ctx, ret);
}
