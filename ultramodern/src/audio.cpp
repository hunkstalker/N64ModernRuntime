#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static uint32_t sample_rate = 48000;

static ultramodern::audio_callbacks_t audio_callbacks;

// HH: emulacion del FIFO del AI (Audio Interface) del N64. En hardware el AI tiene dos registros
// (DMA actual + siguiente); el interrupt AI salta al COMPLETAR el buffer actual y osAiGetLength
// devuelve lo que queda de ESE DMA (no la cola del dispositivo). El port disparaba el evento cada
// VI y reportaba la cola SDL recortada a 1 VI: el driver de audio del juego va a destiempo
// (retraso audible) y su motor de eventos progresaba el modulo de forma no fiel -> freeze del CaC.
// Con HH_AI_FIFO=1 se emula el FIFO real (ver notes/2026-09-17-replay-mode-vi-vis-negativo.md).
namespace {
struct HhAiFifo {
    bool have_cur = false;
    bool have_pend = false;
    int16_t* cur_data = nullptr;
    uint32_t cur_samples = 0;
    double cur_start = 0.0;
    int16_t* pend_data = nullptr;
    uint32_t pend_samples = 0;
    bool irq = false;
};
HhAiFifo g_ai;

double hh_ai_now_s() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void hh_ai_start(int16_t* data, uint32_t samples) {
    // Nota: el audio al dispositivo se encola en el momento del submit (queue_audio_buffer), no
    // aqui: el driver envia en rafagas (3 x 720 frames por ciclo de tick) y el modelo FIFO de 2
    // ranuras descartaria parte de la rafaga dejando al dispositivo sin muestras (underrun).
    // El modelo (longitud DMA actual + evento al completar) es solo lo que ve el juego.
    g_ai.cur_data = data;
    g_ai.cur_samples = samples;
    g_ai.cur_start = hh_ai_now_s();
    g_ai.have_cur = true;
    if (getenv("HH_AUDIOLOG") != nullptr) {
        fprintf(stderr, "[AIF] t=%.3f start frames=%u\n", hh_ai_now_s(), samples / 2);
    }
}

void hh_ai_advance() {
    for (int guard = 0; guard < 8; guard++) {
        if (!g_ai.have_cur) {
            if (g_ai.have_pend) {
                hh_ai_start(g_ai.pend_data, g_ai.pend_samples);
                g_ai.have_pend = false;
                g_ai.irq = true;
                continue;
            }
            break;
        }
        const double dur = (g_ai.cur_samples / 2.0) / (double)sample_rate;
        if (hh_ai_now_s() - g_ai.cur_start + 1e-9 < dur) {
            break;
        }
        g_ai.irq = true;
        if (g_ai.have_pend) {
            hh_ai_start(g_ai.pend_data, g_ai.pend_samples);
            g_ai.have_pend = false;
        }
        else {
            g_ai.have_cur = false;
            break;
        }
    }
}
}  // namespace

extern "C" int hh_ai_fifo_enabled(void) {
    // Por defecto ON (emulacion fiel del AI). HH_AI_FIFO=0 restaura el comportamiento antiguo
    // (evento cada VI + cola SDL recortada) para pruebas de regresion.
    static const int on = [] {
        const char* e = getenv("HH_AI_FIFO");
        return (e != nullptr && strcmp(e, "0") == 0) ? 0 : 1;
    }();
    static bool logged = false;
    if (!logged) {
        logged = true;
        fprintf(stderr, "[AI ] FIFO del AI %s (HH_AI_FIFO=%s)\n",
                on ? "activo: longitud/DMA + evento al completar" : "desactivado (comportamiento antiguo)",
                on ? "1" : "0");
    }
    return on;
}

// Llamado por el hilo VI una vez por VI. Devuelve 1 si hay que entregar el evento AI (ha
// completado un buffer). Sin HH_AI_FIFO devuelve 1 siempre (comportamiento antiguo).
extern "C" int hh_ai_fifo_poll(void) {
    if (!hh_ai_fifo_enabled()) {
        return 1;
    }
    hh_ai_advance();
    if (g_ai.irq) {
        g_ai.irq = false;
        return 1;
    }
    return 0;
}

void ultramodern::set_audio_callbacks(const ultramodern::audio_callbacks_t& callbacks) {
    audio_callbacks = callbacks;
}

void ultramodern::init_audio() {
    // Pick an initial dummy sample rate; this will be set by the game later to the true sample rate.
    set_audio_frequency(48000);
}

void ultramodern::set_audio_frequency(uint32_t freq) {
    if (audio_callbacks.set_frequency) {
        audio_callbacks.set_frequency(freq);
    }
    sample_rate = freq;
}

void ultramodern::queue_audio_buffer(RDRAM_ARG PTR(int16_t) audio_data_, uint32_t byte_count) {
    // Ensure that the byte count is an integer multiple of samples.
    assert((byte_count & 1) == 0);

    // Calculate the number of samples from the number of bytes.
    uint32_t sample_count = byte_count / sizeof(int16_t);

    if (hh_ai_fifo_enabled()) {
        int16_t* data = TO_PTR(int16_t, audio_data_);
        if (sample_count == 0) {
            return;
        }
        if (getenv("HH_AUDIOLOG") != nullptr) {
            uint32_t rem = 0;
            if (g_ai.have_cur) {
                const double dur = (g_ai.cur_samples / 2.0) / (double)sample_rate;
                double left = dur - (hh_ai_now_s() - g_ai.cur_start);
                if (left > 0.0) rem = (uint32_t)(left * (double)sample_rate + 0.5);
            }
            fprintf(stderr, "[AIF] t=%.3f submit frames=%u (rem=%u cur=%d pend=%d)\n",
                    hh_ai_now_s(), sample_count / 2, rem, g_ai.have_cur ? 1 : 0, g_ai.have_pend ? 1 : 0);
        }
        // Al dispositivo va SIEMPRE lo enviado (el driver es quien manda el audio real).
        if (audio_callbacks.queue_samples) {
            audio_callbacks.queue_samples(data, sample_count);
        }
        if (!g_ai.have_cur) {
            hh_ai_start(data, sample_count);
            return;
        }
        if (!g_ai.have_pend) {
            g_ai.pend_data = data;
            g_ai.pend_samples = sample_count;
            return;
        }
        // FIFO lleno: en hardware el registro del "siguiente" se sobrescribe.
        static int hh_n = 0;
        if (hh_n++ < 10) {
            fprintf(stderr, "[AI ] FIFO lleno: se reemplaza el buffer pendiente\n");
        }
        g_ai.pend_data = data;
        g_ai.pend_samples = sample_count;
        return;
    }

    // Queue the swapped audio data.
    if (sample_count > 0 && audio_callbacks.queue_samples) {
        audio_callbacks.queue_samples(TO_PTR(int16_t, audio_data_), sample_count);
    }
}

// For SDL2
//uint32_t buffer_offset_frames = 1;
// For Godot
float buffer_offset_frames = 0.5f;

// If there's ever any audio popping, check here first. Some games are very sensitive to
// the remaining sample count and reporting a number that's too high here can lead to issues.
// Reporting a number that's too low can lead to audio lag in some games.
uint32_t ultramodern::get_remaining_audio_bytes() {
    if (hh_ai_fifo_enabled()) {
        // Hardware: lo que queda del DMA ACTUAL (el driver calcula el siguiente tamano con esto).
        hh_ai_advance();
        if (!g_ai.have_cur) {
            return 0;
        }
        const double dur = (g_ai.cur_samples / 2.0) / (double)sample_rate;
        double left = dur - (hh_ai_now_s() - g_ai.cur_start);
        if (left < 0.0) {
            left = 0.0;
        }
        const uint32_t frames = (uint32_t)(left * (double)sample_rate + 0.5);
        return frames * 2 * sizeof(int16_t);
    }
    // Get the number of remaining buffered audio bytes.
    uint32_t buffered_byte_count;
    if (audio_callbacks.get_frames_remaining != nullptr) {
        buffered_byte_count = audio_callbacks.get_frames_remaining() * 2 * sizeof(int16_t);
    }
    else {
        buffered_byte_count = 100;
    }
    // Adjust the reported count to be some number of refreshes in the future, which helps ensure that
    // there are enough samples even if the audio thread experiences a small amount of lag. This prevents
    // audio popping on games that use the buffered audio byte count to determine how many samples
    // to generate.
    uint32_t samples_per_vi = (sample_rate / 60);
    if (buffered_byte_count > static_cast<uint32_t>(buffer_offset_frames * sizeof(int16_t) * samples_per_vi)) {
        buffered_byte_count -= static_cast<uint32_t>(buffer_offset_frames * sizeof(int16_t) * samples_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    return buffered_byte_count;
}
