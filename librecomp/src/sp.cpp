#include <cstdio>
#include <fstream>
#include <ultramodern/ultramodern.hpp>
#include "recomp.h"

extern "C" void osSpTaskLoad_recomp(uint8_t* rdram, recomp_context* ctx) {
    // Nothing to do here
}

bool dump_frame = false;

extern "C" void osSpTaskStartGo_recomp(uint8_t* rdram, recomp_context* ctx) {
    //printf("[sp] osSpTaskStartGo(0x%08X)\n", (uint32_t)ctx->r4);
    OSTask* task = TO_PTR(OSTask, ctx->r4);
    if (task->t.type == M_GFXTASK) {
        //printf("[sp] Gfx task: %08X\n", (uint32_t)ctx->r4);
    } else if (task->t.type == M_AUDTASK) {
        //printf("[sp] Audio task: %08X\n", (uint32_t)ctx->r4);
    }
    // For debugging
    if (dump_frame) {
        char addr_str[32];
        constexpr size_t ram_size = 0x800000;
        std::unique_ptr<char[]> ram_unswapped = std::make_unique<char[]>(ram_size);
        snprintf(addr_str, sizeof(addr_str) - 1, "%08X", task->t.data_ptr);
        addr_str[sizeof(addr_str) - 1] = '\0';
        std::ofstream dump_file{ "ramdump" + std::string{ addr_str } + ".bin", std::ios::binary};

        for (size_t i = 0; i < ram_size; i++) {
            ram_unswapped[i] = rdram[i ^ 3];
        }

        dump_file.write(ram_unswapped.get(), ram_size);
        dump_frame = false;
    }
    ultramodern::submit_rsp_task(rdram, ctx->r4);
}

// El juego usa el protocolo de yield de libultra para alternar gfx/audio en el RSP:
// t18 llama osSpTaskYield mientras t17 tiene una task en vuelo y luego espera la
// completacion SP. Con el reparto dirigido del port, esa completacion va al emisor
// (t17), asi que el que hace yield se quedaria esperando para siempre. Entregamos una
// completacion SP sintetica al hilo que hace yield (la task real completa por su lado).
extern void sp_complete(PTR(OSThread) submitter);
extern "C" void osSpTaskYield_recomp(uint8_t* rdram, recomp_context* ctx) {
    const char* dbg = getenv("HH_VERBOSE");
    static const bool shared = getenv("HH_SP_SHARED") != nullptr;
    static uint64_t n = 0;
    if (dbg && n < 200) fprintf(stderr, "[YLD] yield n=%llu shared=%d\n", (unsigned long long)n, (int)shared);
    n++;
    if (!shared) {
        sp_complete(ultramodern::this_thread());
    }
}

extern "C" void osSpTaskYielded_recomp(uint8_t* rdram, recomp_context* ctx) {
    const char* dbg = getenv("HH_VERBOSE");
    static uint64_t n = 0;
    uint32_t task = (uint32_t)ctx->r4;
    uint32_t flags = task ? *(uint32_t*)&rdram[(task + 4) & 0x7FFFFF] : 0;
    if (dbg && n < 200) fprintf(stderr, "[YLD] yielded n=%llu task=%08X flags=%08X\n", (unsigned long long)n, task, flags);
    n++;
    ctx->r2 = 0;
}

extern "C" void __osSpSetPc_recomp(uint8_t* rdram, recomp_context* ctx) {
    assert(false);
}
