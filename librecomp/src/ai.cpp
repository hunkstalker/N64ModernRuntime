#include "recomp.h"
#include <cstdio>
#include <chrono>
#include <string>
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

#define VI_NTSC_CLOCK 48681812

static double hh_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

extern "C" void osAiSetFrequency_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t freq = ctx->r4;
    // This makes actual audio frequency more accurate to console, but may not be desirable
    //uint32_t dacRate = (uint32_t)(((float)VI_NTSC_CLOCK / freq) + 0.5f);
    //freq = VI_NTSC_CLOCK / dacRate;
    HH_LOG("[AI ] t=%.0f set_freq freq=%u\n", hh_ms(), (unsigned)freq);
    ctx->r2 = freq;
    ultramodern::set_audio_frequency(freq);
}

extern "C" void osAiSetNextBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t bytes = ctx->r5;
    // HH: el driver de audio de este juego programa AI_DACRATE por MMIO (no usa osAiSetFrequency).
    // El puerto ignora ese registro y se queda en 48 kHz por defecto -> desajuste de tasa
    // (audio a tirones/ralentizado). Leer el registro del area MMIO (0xA4500004 -> offset
    // 0x24500004) y adaptar la frecuencia si cambio. VI_NTSC_CLOCK ~ 48681812 Hz.
    {
        static uint32_t hh_last_freq = 0xFFFFFFFFu;
        uint32_t dacrate = *(uint32_t*)&rdram[0x24500004];
        uint32_t freq;
        if (dacrate == 0 || dacrate == 0xFFFFFFFFu) {
            // El juego no programa AI_DACRATE y produce 720 frames por VI: a 60 VI/s eso son
            // 43200 Hz exactos. (Mupen usa 44100 por defecto, que deja un deficit del 2% ->
            // huecos/petardeo.) Configurable con HH_AI_RATE=<hz>.
            const char* rate_env = getenv("HH_AI_RATE");
            freq = (rate_env != nullptr && *rate_env != '\0')
                       ? (uint32_t)strtoul(rate_env, nullptr, 10) : 43200u;
            if (freq == 0) freq = 43200u;
        }
        else {
            freq = (uint32_t)((double)VI_NTSC_CLOCK / (double)(dacrate + 1) + 0.5);
        }
        if (freq != hh_last_freq && freq >= 4000 && freq <= 192000) {
            hh_last_freq = freq;
            fprintf(stderr, "[AI ] AI rate: dacrate=%u -> %u Hz\n", dacrate, freq);
            ultramodern::set_audio_frequency(freq);
        }
    }
    // HH: el driver de audio calcula el tamano con (0x2E0 - osAiGetLength()/4 + 0x100) & 0xFFF0
    // y lo guarda en un s16. Si la cola virtual supera esa ventana (bursts de ticks), el resultado
    // hace wrap y se convierte en un byte_count negativo (~4 GiB) que envenenaria la cola de audio
    // (osAiGetLength gigante -> command lists runaway que pisan los contextos de voz).
    // Un byte_count negativo o absurdo no corresponde a ningun buffer real: se ignora.
    if ((int32_t)bytes < 0 || bytes > 0x200000u) {
        static int hh_n = 0;
        if (hh_n++ < 20) fprintf(stderr, "[AI ] set_next IGNORADO ptr=%08X bytes=%d (wrap del juego)\n", (unsigned)ctx->r4, (int32_t)bytes);
        ctx->r2 = 0;
        return;
    }
    HH_LOG("[AI ] t=%.0f set_next ptr=%08X bytes=%u rem=%u\n", hh_ms(), (unsigned)ctx->r4, bytes, ultramodern::get_remaining_audio_bytes());
    ultramodern::queue_audio_buffer(rdram, ctx->r4, bytes);
    ctx->r2 = 0;
}

extern "C" void osAiGetLength_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = ultramodern::get_remaining_audio_bytes();
    static uint64_t n = 0;
    if ((n++ & 0x3F) == 0) HH_LOG("[AI ] t=%.0f get_length -> %u (n=%llu)\n", hh_ms(), (unsigned)ctx->r2, (unsigned long long)n);
}

extern "C" void osAiGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 0x00000000; // Pretend the audio DMAs finish instantly
}
