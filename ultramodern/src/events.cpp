#include <thread>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <variant>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <queue>
#include <cstring>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/extensions.h"

#include "ultramodern/rsp.hpp"
#include "ultramodern/renderer_context.hpp"

static ultramodern::events::callbacks_t events_callbacks{};

void ultramodern::events::set_callbacks(const ultramodern::events::callbacks_t& callbacks) {
    events_callbacks = callbacks;
}

struct SpTaskAction {
    OSTask task;
    PTR(OSThread) submitter; // HH: hilo que envió la task (para la completación dirigida)
};

// HH: task RSP en vuelo (no-gfx) -> hilo emisor, para entregarle su completación SP.
static std::mutex sp_submitter_mutex;
static std::unordered_map<uint32_t, PTR(OSThread)> sp_task_submitters;

struct ScreenUpdateAction {
    ultramodern::renderer::ViRegs regs;
};

struct UpdateConfigAction {
};

struct DummyWorkloadAction {
    int32_t fb_address;
};

using Action = std::variant<SpTaskAction, ScreenUpdateAction, UpdateConfigAction, DummyWorkloadAction>;

struct ViState {
    const OSViMode* mode = nullptr;
    PTR(void) framebuffer;
    PTR(OSMesg) mq;
    OSMesg msg;
    uint32_t state;
    uint32_t control;
    int retrace_count = 1;
};

#define VI_STATE_BLACK 0x20
#define VI_STATE_REPEATLINE 0x40

static struct {
    struct {
        std::thread thread;
        int cur_state;
        int field;
        ViState states[2];
        ultramodern::renderer::ViRegs regs;
        ultramodern::renderer::ViRegs update_screen_regs;

        ViState* get_next_state() {
            return &states[cur_state ^ 1];
        }
        ViState* get_cur_state() {
            return &states[cur_state];
        }
        void update_vi() {
            ViState* next_state = get_next_state();
            const OSViMode* next_mode = next_state->mode;
            if (next_mode == nullptr) {
                // Skip the update if the next mode hasn't been set yet.
                return;
            }

            const OSViCommonRegs* common_regs = &next_mode->comRegs;
            const OSViFieldRegs* field_regs = &next_mode->fldRegs[field];
            PTR(void) framebuffer = osVirtualToPhysical(next_state->framebuffer);
            PTR(void) origin = framebuffer + field_regs->origin;

            // Process the VI state flags.
            uint32_t hStart = common_regs->hStart;
            if (next_state->state & VI_STATE_BLACK) {
                hStart = 0;
            }

            uint32_t yScale = field_regs->yScale;
            if (next_state->state & VI_STATE_REPEATLINE) {
                yScale = 0;
                origin = framebuffer;
            }

            // TODO implement osViFade

            // Update VI registers.
            regs.VI_ORIGIN_REG = origin;
            regs.VI_WIDTH_REG = common_regs->width;
            regs.VI_TIMING_REG = common_regs->burst;
            regs.VI_V_SYNC_REG = common_regs->vSync;
            regs.VI_H_SYNC_REG = common_regs->hSync;
            regs.VI_LEAP_REG = common_regs->leap;
            regs.VI_H_START_REG = hStart;
            regs.VI_V_START_REG = field_regs->vStart; // TODO implement osViExtendVStart
            regs.VI_V_BURST_REG = field_regs->vBurst;
            regs.VI_INTR_REG = field_regs->vIntr;
            regs.VI_X_SCALE_REG = common_regs->xScale; // TODO implement osViSetXScale
            regs.VI_Y_SCALE_REG = yScale; // TODO implement osViSetYScale
            regs.VI_STATUS_REG = next_state->control;
            
            // Swap VI states.
            cur_state ^= 1;
            *get_next_state() = *get_cur_state();
        }
    } vi;
    struct {
        std::thread gfx_thread;
        std::thread task_thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } sp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } dp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } ai;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } si;
    // The same message queue may be used for multiple events, so share a mutex for all of them
    std::mutex message_mutex;
    uint8_t* rdram;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
    moodycamel::BlockingConcurrentQueue<OSTask*> sp_task_queue{};
    moodycamel::ConcurrentQueue<OSThread*> deleted_threads{};
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } vi_event;
} events_context{};

ultramodern::renderer::ViRegs* ultramodern::renderer::get_vi_regs() {
    return &events_context.vi.update_screen_regs;
}


// HH: traza de eventos (osSetEventMesg y entregas VI/AI) en fichero PROPIO (hh_evt.log) para que no
// lo trunque el logger de colas. Sirve para cazar el cuelgue por dano (el hilo VI deja de entregar).
extern uint64_t total_vis;

// HH: base de RDRAM para diagnosticos fuera de este TU (p. ej. el dispatch del bucle principal).
extern "C" uint8_t* hh_get_rdram_base(void) { return events_context.rdram; }

extern "C" void hh_evt_log(const char* what, int event_id, PTR(OSMesgQueue) mq_, OSMesg msg, unsigned long long vi, int tid) {
    if (getenv("HH_MQLOG_ALL") == nullptr) return;
    static FILE* f = nullptr;
    static long total = 0;
    if (f == nullptr) {
        f = fopen("hh_evt.log", "w");
        if (f == nullptr) return;
    }
    if (total > (4L * 1024 * 1024)) return;
    total += fprintf(f, "[EVT] t_vis=%llu %s tid=%d event=%d mq=%08X msg=%08X\n",
                     vi, what, tid, event_id, (unsigned)mq_, (unsigned)msg);
    fflush(f);
}

extern "C" void osSetEventMesg(RDRAM_ARG OSEvent event_id, PTR(OSMesgQueue) mq_, OSMesg msg) {
    hh_evt_log("set-enter", (int)event_id, mq_, msg, (unsigned long long)total_vis,
               ultramodern::is_game_thread() ? (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())) : -1);
    HH_LOG("[EV] osSetEventMesg event=%d mq=%p msg=%p\n", (int)event_id, (void*)mq_, (void*)msg);
    std::unique_lock<std::mutex> lock{ events_context.message_mutex };
    hh_evt_log("set-acquired", (int)event_id, mq_, msg, (unsigned long long)total_vis,
               ultramodern::is_game_thread() ? (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())) : -1);

    switch (event_id) {
        case OS_EVENT_SP:
            events_context.sp.msg = msg;
            events_context.sp.mq = mq_;
            break;
        case OS_EVENT_DP:
            events_context.dp.msg = msg;
            events_context.dp.mq = mq_;
            break;
        case OS_EVENT_AI:
            events_context.ai.msg = msg;
            events_context.ai.mq = mq_;
            break;
        case OS_EVENT_SI:
            events_context.si.msg = msg;
            events_context.si.mq = mq_;
            break;
        case OS_EVENT_VI:
            events_context.vi_event.msg = msg;
            events_context.vi_event.mq = mq_;
            break;
    }

    lock.unlock();
    hh_evt_log("set-exit", (int)event_id, mq_, msg, (unsigned long long)total_vis,
               ultramodern::is_game_thread() ? (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())) : -1);

    // Mirror the ROM libultra bookkeeping: __osEventStateTab[event_id] = { mq, msg }.
    // The game reads this table directly at 0x800CD5F0 + event_id * 8; the runtime replaces
    // osSetEventMesg with this C++ version, so the table has to be written here too.
    if (event_id < 24) {
        uint32_t addr = 0xCD5F0 + (uint32_t)event_id * 8;
        *reinterpret_cast<uint32_t*>(&rdram[addr]) = static_cast<uint32_t>(mq_);
        *reinterpret_cast<uint32_t*>(&rdram[addr + 4]) = static_cast<uint32_t>(msg);
    }
}

uint64_t total_vis = 0;

// HH: latido del hilo de VI del runtime (para el watchdog de cuelgue del port).
static std::atomic<uint64_t> hh_vi_ticks{0};
extern "C" uint64_t hh_get_vi_ticks() { return hh_vi_ticks.load(); }

// HH: contador de VI (frames de juego) expuesto para el replay determinista de input.
extern "C" uint64_t hh_get_vi_count(void) { return total_vis; }

// ADR 0003: el OSViContext y los registros VI los mantiene libultra del ROM
// (viMgrMain -> __osViSwapContext). El runtime solo lee el bloque de registros MMIO
// (KSEG1 0x04400000) para alimentar ViRegs/RT64. Devuelve false mientras el ROM no los
// haya inicializado (VI_CTRL == 0), en cuyo caso se usa el modo dummy.
static bool load_vi_regs(uint8_t* rdram, ultramodern::renderer::ViRegs& regs) {
    constexpr uint32_t VI_BASE = 0x20000000u + 0x04400000u;
    auto r = [&](uint32_t off) { return *(uint32_t*)&rdram[VI_BASE + off]; };
    uint32_t status = r(0x00);
    if (status == 0) {
        return false;
    }
    regs.VI_STATUS_REG = status;
    regs.VI_ORIGIN_REG = r(0x04);
    regs.VI_WIDTH_REG = r(0x08);
    regs.VI_INTR_REG = r(0x0C);
    regs.VI_V_CURRENT_LINE_REG = r(0x10);
    regs.VI_TIMING_REG = r(0x14);
    regs.VI_V_SYNC_REG = r(0x18);
    regs.VI_H_SYNC_REG = r(0x1C);
    regs.VI_LEAP_REG = r(0x20);
    regs.VI_H_START_REG = r(0x24);
    regs.VI_V_START_REG = r(0x28);
    regs.VI_V_BURST_REG = r(0x2C);
    regs.VI_X_SCALE_REG = r(0x30);
    regs.VI_Y_SCALE_REG = r(0x34);
    return true;
}


extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;

void set_dummy_vi(bool odd);

void vi_thread_func() {
    ultramodern::set_native_thread_name("VI Thread");
    // This thread should be prioritized over every other thread in the application, as it's what allows
    // the game to generate new audio and gfx lists.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;

    while (!exited) {
        hh_vi_ticks.fetch_add(1, std::memory_order_relaxed);
        // Determine the next VI time (more accurate than adding 16ms each VI interrupt)
        auto next = ultramodern::get_start() + (total_vis * 1000000us) / (60 * ultramodern::get_speed_multiplier());
        //if (next > std::chrono::high_resolution_clock::now()) {
        //    printf("Sleeping for %" PRIu64 " us to get from %" PRIu64 " us to %" PRIu64 " us \n",
        //        (next - std::chrono::high_resolution_clock::now()) / 1us,
        //        (std::chrono::high_resolution_clock::now() - events_context.start) / 1us,
        //        (next - events_context.start) / 1us);
        //} else {
        //    printf("No need to sleep\n");
        //}
        // Detect if there's more than a second to wait and wait a fixed amount instead for the next VI if so, as that usually means the system clock went back in time.
        if (std::chrono::floor<std::chrono::seconds>(next - std::chrono::high_resolution_clock::now()) > 1s) {
            // printf("Skipping the next VI wait\n");
            next = std::chrono::high_resolution_clock::now();
        }
        ultramodern::sleep_until(next);
        auto time_now = ultramodern::time_since_start();
        // Calculate how many VIs have passed
        uint64_t new_total_vis = (time_now * (60 * ultramodern::get_speed_multiplier()) / 1000ms) + 1;
        if (new_total_vis > total_vis + 1) {
            //printf("Skipped % " PRId64 " frames in VI interupt thread!\n", new_total_vis - total_vis - 1);
        }
        total_vis = new_total_vis;

        // If the game hasn't started yet, set a dummy VI mode and origin.
        if (!ultramodern::is_game_started()) {
            static bool odd = false;
            set_dummy_vi(odd);
            odd = !odd;

            events_context.action_queue.enqueue(DummyWorkloadAction{events_context.vi.get_next_state()->framebuffer});
        }

        // Queue a screen update for the graphics thread with the current VI register state.
        // Doing this before the VI update is equivalent to updating the screen after the previous frame's scanout finished.
        events_context.action_queue.enqueue(ScreenUpdateAction{ events_context.vi.regs });

        // ADR 0003: con el subsistema VI del ROM activo, los registros los escribe
        // __osViSwapContext; el runtime solo los lee. Hasta que el ROM los inicialice,
        // se mantiene el modo dummy (set_dummy_vi + update_vi).
        static bool rom_vi_active = false;
        if (!rom_vi_active && ultramodern::is_game_started()) {
            rom_vi_active = load_vi_regs(events_context.rdram, events_context.vi.regs);
        }
        if (rom_vi_active) {
            load_vi_regs(events_context.rdram, events_context.vi.regs);
        } else {
            events_context.vi.update_vi();
        }

        // If the game has started, handle sending VI and AI events.
        if (ultramodern::is_game_started()) {
            {
                hh_evt_log("vi-deliver-try", -1, NULLPTR, 0, (unsigned long long)total_vis, -1);
                std::lock_guard lock{ events_context.message_mutex };
                // Interrupt VI de hardware: lo consume viMgrMain (osCreateViManager lo registró
                // con osSetEventMesg(OS_EVENT_VI)). La entrega al juego la hace el ROM.
                if (events_context.vi_event.mq != NULLPTR) {
                    ultramodern::enqueue_external_message_src(events_context.vi_event.mq, events_context.vi_event.msg, false, ultramodern::EventMessageSource::Vi);
                }
                hh_evt_log("vi-deliver-ok", -1, events_context.vi_event.mq, (OSMesg)events_context.vi_event.msg, (unsigned long long)total_vis, -1);
                // HH diag: vigilancia de los contextos de audio (voice+0x60/+0x64/+0x5C) y de la
                // tabla de voces (0x80091BE0..) para localizar la corrupcion del descriptor.
                if (getenv("HH_CTXWATCH") != nullptr) {
                    uint8_t* g = events_context.rdram;
                    struct { const char* name; uint32_t addr; } fields[] = {
                        { "v1+60", 0x800C7A50 }, { "v1+64", 0x800C7A54 }, { "v1+5C", 0x800C7A5C },
                        { "v2+60", 0x800C8A40 }, { "v2+64", 0x800C8A44 }, { "v2+5C", 0x800C8A4C },
                        { "v3+60", 0x800C9A30 }, { "v3+64", 0x800C9A34 }, { "v3+5C", 0x800C9A3C },
                        { "tbl0",  0x80091BE0 }, { "tbl1",  0x80091BE4 }, { "tbl2",  0x80091BE8 },
                    };
                    static uint32_t prev[12];
                    static bool init = false;
                    for (unsigned i = 0; i < 12; i++) {
                        uint32_t v = *(uint32_t*)&g[fields[i].addr & 0x1FFFFFFF];
                        if (!init || v != prev[i]) {
                            fprintf(stderr, "[CTXW] vis=%llu %s=%08X (antes %08X)\n",
                                    (unsigned long long)total_vis, fields[i].name, v, init ? prev[i] : 0);
                            prev[i] = v;
                        }
                    }
                    init = true;
                }
                // HH diag: vigilancia del objeto RSP (0x8005C4B0) para el crash del driver.
                if (getenv("HH_WATCH59") != nullptr) {
                    uint8_t* g = events_context.rdram;
                    auto rw = [&](uint32_t a){ return *(uint32_t*)&g[a]; };
                    static uint32_t p0 = 0, p4 = 0, p8 = 0, pc = 0;
                    uint32_t v0 = rw(0x5C4B0), v4 = rw(0x5C4B4), v8 = rw(0x5C4B8), vc = rw(0x5C4BC);
                    if (v0 != p0) { fprintf(stderr, "[W59] vis=%llu +0=%08X (antes %08X)\n", (unsigned long long)total_vis, v0, p0); p0 = v0; }
                    if (v4 != p4) { fprintf(stderr, "[W59] vis=%llu +4=%08X (antes %08X)\n", (unsigned long long)total_vis, v4, p4); p4 = v4; }
                    if (v8 != p8) { fprintf(stderr, "[W59] vis=%llu +8=%08X (antes %08X)\n", (unsigned long long)total_vis, v8, p8); p8 = v8; }
                    if (vc != pc) { fprintf(stderr, "[W59] vis=%llu +C=%08X (antes %08X)\n", (unsigned long long)total_vis, vc, pc); pc = vc; }
                }
                // GATE: traza de cambios del contador de tareas RSP pendientes (0x8005CD4C).
                if (ultramodern::debug::verbose()) {
                    uint8_t* g = events_context.rdram;
                    uint32_t c = *(uint32_t*)&g[0x5CD4C];
                    static uint32_t prev_cd4c = 0;
                    static bool cd4c_init = false;
                    if (!cd4c_init || c != prev_cd4c) {
                        fprintf(stderr, "[GATE] vis=%llu cd4c=%u\n", (unsigned long long)total_vis, c);
                        prev_cd4c = c;
                        cd4c_init = true;
                    }
                }
                // DUMP: render-mode state of the game (DAT_80037750 / DAT_80037730 / DAT_80037738)
                if ((total_vis % 60) == 0) {
                    uint8_t* g = events_context.rdram;
                    auto rd = [&](uint32_t a){ return *(uint32_t*)&g[a & 0x1FFFFFFF]; };
                    auto rdu16 = [&](uint32_t a){ return *(uint16_t*)&g[(a ^ 2) & 0x1FFFFFFF]; };
                    auto rdu8 = [&](uint32_t a){ return g[(a ^ 3) & 0x1FFFFFFF]; };
                    HH_LOG("[RND] vis=%llu 3750=0x%08X 3730=0x%08X 3738=0x%08X 3734=0x%08X 373c=0x%08X fe00=0x%04X fe02=0x%02X node=%08X n0=%08X n14=%08X n18=%08X n1C=%08X v478=%08X f545=%02X f544=%02X d550=%02X ld14=%08X ld18=%08X ld1C=%08X ld20=%08X cd4c=%u t17q=%08X t17n=%08X t16q=%08X mqr=%08X mqs=%08X mqv=%u req=%08X q158r=%08X q158v=%u t19q=%08X\n",
                        (unsigned long long)total_vis, rd(0x80037750), rd(0x80037730), rd(0x80037738), rd(0x80037734), rd(0x8003773c),
                        rdu16(0x801CFE00), rdu8(0x801CFE02),
                        rd(0x801D03C0), rd(0x801D03C0), rd(0x801D03D4), rd(0x801D03D8), rd(0x801D03DC), rd(0x80089478),
                        rdu8(0x8008D545), rdu8(0x8008D544), rdu8(0x8008D550),
                        rd(0x8005D014), rd(0x8005D018), rd(0x8005D01C), rd(0x8005D020),
                        rd(0x8005CD4C), rd(0x8005C9D8 + 8), rd(0x8005C9D8), rd(0x8005CB88 + 8),
                        rd(0x8005C4F0), rd(0x8005C4F4), rd(0x8005C4F8),
                        rd(0x8005C4B0 + 0x888), rd(0x8005C608), rd(0x8005C608 + 8), rd(0x8005C678 + 8));
                }
                {
                    // HH_DUMP_VI admite lista separada por comas (p. ej. "3000,4500,5400").
                    static bool dumpvi_parsed = false;
                    static unsigned long long dumpvi_list[32];
                    static int dumpvi_count = 0;
                    if (!dumpvi_parsed) {
                        dumpvi_parsed = true;
                        const char* dumpvi = getenv("HH_DUMP_VI");
                        if (dumpvi != nullptr) {
                            char buf[512];
                            strncpy(buf, dumpvi, sizeof(buf) - 1);
                            buf[sizeof(buf) - 1] = 0;
                            for (char* tok = strtok(buf, ","); tok != nullptr && dumpvi_count < 32; tok = strtok(nullptr, ",")) {
                                dumpvi_list[dumpvi_count++] = strtoull(tok, nullptr, 0);
                            }
                        }
                    }
                    for (int i = 0; i < dumpvi_count; i++) {
                        if (dumpvi_list[i] != total_vis) continue;
                        uint8_t* g = events_context.rdram;
                        char path[512];
                        snprintf(path, sizeof(path), "work/debug/port_vi%llu.bin", (unsigned long long)total_vis);
                        FILE* f = fopen(path, "wb");
                        if (f) { fwrite(g, 1, 0x800000, f); fclose(f); fprintf(stderr, "[DUMP] VI %llu -> %s\n", (unsigned long long)total_vis, path); }
                        break;
                    }
                }
                // Per-frame evolution of the boot object (HH debug): +0x00..+0x2C.
                if (ultramodern::debug::verbose()) {
                    uint8_t* g = events_context.rdram;
                    auto rdw = [&](uint32_t a){ return *(uint32_t*)&g[a & 0x1FFFFFFF]; };
                    HH_LOG("[OBJ] vis=%llu %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X\n",
                        (unsigned long long)total_vis,
                        rdw(0x801D03C0), rdw(0x801D03C4), rdw(0x801D03C8), rdw(0x801D03CC),
                        rdw(0x801D03D0), rdw(0x801D03D4), rdw(0x801D03D8), rdw(0x801D03DC),
                        rdw(0x801D03E0), rdw(0x801D03E4), rdw(0x801D03E8), rdw(0x801D03EC));
                }
                if (ultramodern::debug::verbose()) {
                    uint8_t* g = events_context.rdram;
                    auto rd16 = [&](uint32_t a){ return *(uint16_t*)&g[(a ^ 2) & 0x1FFFFFFF]; };
                    auto rd32 = [&](uint32_t a){ return *(uint32_t*)&g[a & 0x1FFFFFFF]; };
                    HH_LOG("[TBL] vis=%llu %04X:%08X %04X:%08X %04X:%08X %04X:%08X\n",
                        (unsigned long long)total_vis,
                        rd16(0x8008DFC0), rd32(0x8008DFC4),
                        rd16(0x8008DFC8), rd32(0x8008DFCC),
                        rd16(0x8008DFD0), rd32(0x8008DFD4),
                        rd16(0x8008DFD8), rd32(0x8008DFDC));
                }
            }
            if (events_context.ai.mq != NULLPTR) {
                // Send a message to the VI queue, and do not set it to be requeued if the queue was full for the same reason as the VI message above.
                static uint64_t ai_fires = 0;
                if ((ai_fires++ % 60) == 0) {
                    HH_LOG("[AIEV] fire n=%llu vis=%llu mq=%p\n", (unsigned long long)ai_fires, (unsigned long long)total_vis, (void*)events_context.ai.mq);
                }
                ultramodern::enqueue_external_message_src(events_context.ai.mq, events_context.ai.msg, false, ultramodern::EventMessageSource::Ai);
            }
        }

        if (events_callbacks.vi_callback != nullptr) {
            events_callbacks.vi_callback();
        }
    }
}

// HH: completación dirigida al hilo que envió la task (si sigue bloqueado en el evento);
// el juego tiene varios hilos (16/17/18) esperando el mismo mq SP/DP y el reparto por lista
// podía entregar la completación al hilo equivocado (deadlock del gate, ver nota 2026-09-13).
void sp_complete(PTR(OSThread) submitter = NULLPTR) {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    // HH_SP_SHARED: semantica libultra (cola de evento compartida): la completacion la consume
    // cualquier waiter (el protocolo de yield del juego lo requiere). Sin el knob se mantiene el
    // reparto dirigido al emisor (fix del deadlock del gate).
    static const bool shared = getenv("HH_SP_SHARED") != nullptr;
    if (shared) {
        submitter = NULLPTR;
    }
    HH_LOG("[SPC] fire mq=%p msg=%p target=%p\n", (void*)events_context.sp.mq, (void*)events_context.sp.msg, (void*)submitter);
    ultramodern::enqueue_external_message_to(events_context.sp.mq, events_context.sp.msg, false, submitter);
}

void dp_complete(PTR(OSThread) submitter = NULLPTR) {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    static const bool shared = getenv("HH_SP_SHARED") != nullptr;
    if (shared) {
        submitter = NULLPTR;
    }
    HH_LOG("[DP] dp_complete -> mq=%p msg=%p target=%p\n", (void*)events_context.dp.mq, (void*)events_context.dp.msg, (void*)submitter);
    ultramodern::enqueue_external_message_to(events_context.dp.mq, events_context.dp.msg, false, submitter);
}

void task_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready) {
    ultramodern::set_native_thread_name("SP Task Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (true) {
        // Wait until an RSP task has been sent
        OSTask* task;
        events_context.sp_task_queue.wait_dequeue(task);

        if (task == nullptr) {
            return;
        }
        // HH: recuperar el hilo emisor de esta task (si lo registró submit_rsp_task).
        PTR(OSThread) submitter = NULLPTR;
        {
            std::lock_guard lock{ sp_submitter_mutex };
            // Clave por offset de rdram: (uint8_t*)task - rdram == PTR - 0xFFFFFFFF80000000.
            uint32_t task_key = (uint32_t)((uint8_t*)task - rdram);
            auto it = sp_task_submitters.find(task_key);
            if (it != sp_task_submitters.end()) {
                submitter = it->second;
                sp_task_submitters.erase(it);
            }
        }
        HH_LOG("[SPTHR] deq task=%p type=%u target=%p\n", (void*)task, (unsigned)task->t.type, (void*)submitter);

        // Execute the RSP task. Tasks without a registered ucode (p. ej. audio, M_AUDTASK=2) se
        // tratan como no-op: se completa igualmente para que el juego avance (audio dummy).
        if (!ultramodern::rsp::run_task(PASS_RDRAM task)) {
            static bool warned[64] = {};
            uint32_t t = static_cast<uint32_t>(task->t.type);
            if (t < 64 && !warned[t]) {
                warned[t] = true;
                HH_LOG("[RSP] sin ucode para task type %u -> no-op (dummy)\n", t);
            }
        }
        HH_LOG("[SPTHR] done task=%p\n", (void*)task);

        // Tell the game that the RSP has completed (al hilo emisor, si está bloqueado)
        sp_complete(submitter);
    }
}

std::atomic_uint32_t display_refresh_rate = 60;
std::atomic<float> resolution_scale = 1.0f;

uint32_t ultramodern::get_target_framerate(uint32_t original) {
    auto& config = ultramodern::renderer::get_graphics_config();

    switch (config.rr_option) {
        case ultramodern::renderer::RefreshRate::Original:
        default:
            return original;
        case ultramodern::renderer::RefreshRate::Manual:
            return config.rr_manual_value;
        case ultramodern::renderer::RefreshRate::Display:
            return display_refresh_rate.load();
    }
}

uint32_t ultramodern::get_display_refresh_rate() {
    return display_refresh_rate.load();
}

float ultramodern::get_resolution_scale() {
    return resolution_scale.load();
}

void ultramodern::trigger_config_action() {
    events_context.action_queue.enqueue(UpdateConfigAction{});
}

std::atomic<ultramodern::renderer::SetupResult> renderer_setup_result = ultramodern::renderer::SetupResult::Success;
std::atomic<ultramodern::renderer::GraphicsApi> renderer_chosen_api = ultramodern::renderer::GraphicsApi::Auto;

void gfx_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready, ultramodern::renderer::WindowHandle window_handle) {
    bool enabled_instant_present = false;
    using namespace std::chrono_literals;

    ultramodern::set_native_thread_name("Gfx Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    auto old_config = ultramodern::renderer::get_graphics_config();

    auto renderer_context = ultramodern::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::get_graphics_config().developer_mode);

    renderer_chosen_api.store(renderer_context->get_chosen_api());
    if (!renderer_context->valid()) {
        renderer_setup_result.store(renderer_context->get_setup_result());
        // Notify the caller thread that this thread is ready.
        thread_ready->signal();
        return;
    }

    if (events_callbacks.gfx_init_callback != nullptr) {
        events_callbacks.gfx_init_callback();
    }

    ultramodern::rsp::init();

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (!exited) {
        // Try to pull an action from the queue
        Action action;
        if (events_context.action_queue.wait_dequeue_timed(action, 1ms)) {
            // Determine the action type and act on it
            if (const auto* task_action = std::get_if<SpTaskAction>(&action)) {
                // Tell the game that the RSP completed instantly. This will allow it to queue other task types, but it won't
                // start another graphics task until the RDP is also complete. Games usually preserve the RSP inputs until the RDP
                // is finished as well, so sending this early shouldn't be an issue in most cases.
                // If this causes issues then the logic can be replaced with responding to yield requests.
                ultramodern::measure_input_latency();

                PTR(u64) displaylist = task_action->task.t.data_ptr;
                ultramodern::extensions::on_displaylist_submitted(displaylist);

                [[maybe_unused]] auto renderer_start = std::chrono::high_resolution_clock::now();
                renderer_context->send_dl(&task_action->task);
                [[maybe_unused]] auto renderer_end = std::chrono::high_resolution_clock::now();

                dp_complete(task_action->submitter);
                // TODO hook the parsed event up to the actual parsing point when a callback is added to RT64.
                ultramodern::extensions::on_displaylist_parsed(displaylist);
                ultramodern::extensions::on_displaylist_completed(displaylist);
                // printf("Renderer ProcessDList time: %d us\n", static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count()));
            }
            else if (const auto* screen_update_action = std::get_if<ScreenUpdateAction>(&action)) {
                events_context.vi.update_screen_regs = screen_update_action->regs;
                renderer_context->update_screen();
                display_refresh_rate = renderer_context->get_display_framerate();
                resolution_scale = renderer_context->get_resolution_scale();
            }
            else if (const auto* config_action = std::get_if<UpdateConfigAction>(&action)) {
                (void)config_action;
                auto new_config = ultramodern::renderer::get_graphics_config();
                if (renderer_context->update_config(old_config, new_config)) {
                    old_config = new_config;
                }
            }
            else if (const auto* dummy_workload_action = std::get_if<DummyWorkloadAction>(&action)) {
                renderer_context->send_dummy_workload(dummy_workload_action->fb_address);
            }
        }
    }

    graphics_shutdown_ready.wait();
    renderer_context->shutdown();
}

#define VI_CTRL_TYPE_16             0x00002
#define VI_CTRL_TYPE_32             0x00003
#define VI_CTRL_GAMMA_DITHER_ON     0x00004
#define VI_CTRL_GAMMA_ON            0x00008
#define VI_CTRL_DIVOT_ON            0x00010
#define VI_CTRL_SERRATE_ON          0x00040
#define VI_CTRL_ANTIALIAS_MASK      0x00300
#define VI_CTRL_ANTIALIAS_MODE_1    0x00100
#define VI_CTRL_ANTIALIAS_MODE_2    0x00200
#define VI_CTRL_ANTIALIAS_MODE_3    0x00300
#define VI_CTRL_PIXEL_ADV_MASK      0x01000
#define VI_CTRL_PIXEL_ADV_1         0x01000
#define VI_CTRL_PIXEL_ADV_2         0x02000
#define VI_CTRL_PIXEL_ADV_3         0x03000
#define VI_CTRL_DITHER_FILTER_ON    0x10000

static const OSViMode dummy_mode = []() {
    OSViMode ret{};

    ret.type = 2;
    ret.comRegs.ctrl = VI_CTRL_TYPE_16 | VI_CTRL_GAMMA_DITHER_ON | VI_CTRL_GAMMA_ON | VI_CTRL_DIVOT_ON | VI_CTRL_ANTIALIAS_MODE_1 | VI_CTRL_PIXEL_ADV_3;
    ret.comRegs.width = 0x140;
    ret.comRegs.burst = 0x03E52239;
    ret.comRegs.vSync = 0x20D;
    ret.comRegs.hSync = 0xC15;
    ret.comRegs.leap = 0x0C150C15;
    ret.comRegs.hStart = 0x006C02EC;
    ret.comRegs.xScale = 0x200;
    ret.comRegs.vCurrent = 0x0;

    for (int field = 0; field < 2; field++) {
        ret.fldRegs[field].origin = 0x280;
        ret.fldRegs[field].yScale = 0x400;
        ret.fldRegs[field].vStart = 0x2501FF;
        ret.fldRegs[field].vBurst = 0xE0204;
        ret.fldRegs[field].vIntr = 0x2;
    }

    return ret;
}();

void set_dummy_vi(bool odd) {
    ViState* next_state = events_context.vi.get_next_state();
    next_state->mode = &dummy_mode;
    next_state->control = next_state->mode->comRegs.ctrl;
    // Set up a dummy framebuffer.
    next_state->framebuffer = 0x80700000;
    if (odd) {
        next_state->framebuffer += 0x25800;
    }
}

// osViSwapBuffer/osViSetMode/osViSetSpecialFeatures/osViBlack/osViGet*Framebuffer se generan
// del ROM (ADR 0003): viMgrMain mantiene el OSViContext y __osViSwapContext escribe los registros.
// El runtime solo conserva la emulación de hardware (temporización + ViRegs vía MMIO).

extern "C" void osViRepeatLine(uint8_t active) {
    std::lock_guard lock{ events_context.message_mutex };
    ViState* next_state = events_context.vi.get_next_state();
    uint32_t* state_out = &next_state->state;
    if (active) {
        *state_out |= VI_STATE_REPEATLINE;
    } else {
        *state_out &= ~VI_STATE_REPEATLINE;
    }
}

extern "C" void osViSetXScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" void osViSetYScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

void ultramodern::submit_rsp_task(RDRAM_ARG PTR(OSTask) task_) {
    OSTask* task = TO_PTR(OSTask, task_);

    HH_LOG("[SPT] submit_rsp_task task=%p type=%d ucode=0x%08llX data=0x%08llX datasize=0x%llX out=0x%08llX outsize=0x%llX\n",
        (void*)task, (int)task->t.type,
        (unsigned long long)task->t.ucode, (unsigned long long)task->t.data_ptr,
        (unsigned long long)task->t.data_size, (unsigned long long)task->t.output_buff,
        (unsigned long long)task->t.output_buff_size);

    // HH: registrar el hilo emisor para entregarle su completación (SP/DP) a él y no al primero
    // que esté esperando en el mq del evento. En gfx la task se procesa en el hilo de gráficos
    // (copia), así que el registro se consume aquí mismo.
    PTR(OSThread) submitter = ultramodern::this_thread();
    if (task->t.type == M_GFXTASK) {
        // El RSP del hardware completa al terminar de parsear la DL, independiente del RDP. Si
        // esperamos al render de RT64 (lento en software) para el sp_complete, la task gfx queda
        // "en vuelo" (+0x88C) ~80 ms y el driver de audio entra en yield constantemente (en el
        // emulador no hay yields). Completamos el SP ya; el gfx thread hace send_dl + dp_complete.
        sp_complete(submitter);
        events_context.action_queue.enqueue(SpTaskAction{ *task, submitter });
    }
    // Set all other tasks as the RSP task
    else {
        {
            std::lock_guard lock{ sp_submitter_mutex };
            sp_task_submitters[(uint32_t)((uint64_t)task_ - 0xFFFFFFFF80000000ULL)] = submitter;
        }
        events_context.sp_task_queue.enqueue(task);
    }
}

void ultramodern::send_si_message() {
    ultramodern::enqueue_external_message_src(events_context.si.mq, events_context.si.msg, false, ultramodern::EventMessageSource::Si);
}

void ultramodern::init_events(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    moodycamel::LightweightSemaphore gfx_thread_ready;
    moodycamel::LightweightSemaphore task_thread_ready;
    events_context.rdram = rdram;
    events_context.sp.gfx_thread = std::thread{ gfx_thread_func, rdram, &gfx_thread_ready, window_handle };
    events_context.sp.task_thread = std::thread{ task_thread_func, rdram, &task_thread_ready };

    // Wait for the two sp threads to be ready before continuing to prevent the game from
    // running before we're able to handle RSP tasks.
    gfx_thread_ready.wait();
    task_thread_ready.wait();

    ultramodern::renderer::SetupResult setup_result = renderer_setup_result.load();
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        auto show_renderer_error = [](const std::string& msg) {
            std::string error_msg = "An error has been encountered on startup: " + msg;

            ultramodern::error_handling::message_box(error_msg.c_str());
        };

        const std::string driver_os_suffix = "\nPlease make sure your GPU drivers and your OS are up to date.";
        switch (setup_result) {
            case ultramodern::renderer::SetupResult::Success:
                break;
            case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
                show_renderer_error("Failed to load dynamic libraries. Make sure the DLLs are next to the recomp executable.");
                break;
            case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
                show_renderer_error(ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + " is not supported on this platform. Please select a different graphics API.");
                break;
            case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
                show_renderer_error("Unable to initialize " + ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + "." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
                show_renderer_error("Unable to find compatible graphics device." + driver_os_suffix);
                break;
        }
        throw std::runtime_error("Failed to initialize the renderer");
    }

    events_context.vi.thread = std::thread{ vi_thread_func };
}

void ultramodern::join_event_threads() {
    events_context.sp.gfx_thread.join();
    events_context.vi.thread.join();

    // Send a null RSP task to indicate that the RSP task thread should exit.
    events_context.sp_task_queue.enqueue(nullptr);
    events_context.sp.task_thread.join();
}
