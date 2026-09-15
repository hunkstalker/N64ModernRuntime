#include <bitset>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <queue>
#include <unordered_map>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

// HH: diagnostico de fuga de pila (ver recomp.cpp).
extern "C" uint32_t hh_current_sp(void);
extern "C" int hh_ring2_count(void);
extern "C" void hh_ring2_dump_last(FILE*, int);

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
    bool requeue_if_blocked;
    PTR(OSThread) target; // HH: para completaciones SP/DP; NULLPTR = reparto normal
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};
std::bitset<32> requeue_enabled;

// HH: completaciones dirigidas (SP/DP) pendientes por (hilo, mq). El juego tiene varios hilos
// (16/17/18) esperando el mismo evento SP; el reparto por lista podía entregar la completación al
// hilo equivocado (deadlock del gate, nota 2026-09-13-deadlock-sp-race.md).
static std::mutex pending_mutex;
static std::unordered_map<uint64_t, std::queue<OSMesg>> pending_completions;
static uint64_t pending_key(PTR(OSThread) t, PTR(OSMesgQueue) mq) {
    return ((uint64_t)(uint32_t)t << 32) | (uint32_t)mq;
}
// Consume una completación pendiente para (t, mq). Devuelve true si había.
static bool take_pending_completion(RDRAM_ARG PTR(OSThread) t, PTR(OSMesgQueue) mq, PTR(OSMesg) msg_) {
    if (t == NULLPTR) return false;
    std::lock_guard lock{ pending_mutex };
    auto it = pending_completions.find(pending_key(t, mq));
    if (it == pending_completions.end() || it->second.empty()) {
        return false;
    }
    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = it->second.front();
    }
    it->second.pop();
    if (it->second.empty()) {
        pending_completions.erase(it);
    }
    return true;
}


bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block, PTR(OSThread) target);

void ultramodern::set_message_queue_control(const ultramodern::MessageQueueControl& mqc) {
    requeue_enabled.reset();
    requeue_enabled.set(static_cast<int>(EventMessageSource::Timer), mqc.requeue_timer);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Sp), mqc.requeue_sp);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Si), mqc.requeue_si);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Ai), mqc.requeue_ai);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Vi), mqc.requeue_vi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Pi), mqc.requeue_pi);
    requeue_enabled.set(static_cast<int>(EventMessageSource::Dp), mqc.requeue_dp);
}

static void hh_check_mq(const char* where, PTR(OSMesgQueue) mq) {
    uint32_t v = (uint32_t)mq;
    if (v != 0 && (v < 0x80000000u || v >= 0x80800000u || (v & 3u))) {
        static int hh_n = 0;
        if (hh_n++ < 30) fprintf(stderr, "[BADMQ] %s mq=%08X\n", where, v);
    }
}


// HH: traza generica de colas de mensajes (HH_MQLOG_ALL=1) -> hh_mq_all.log.
// Objetivo: ver quien envia/recibe en cada cola (tid guest, mq, msg) durante un cuelgue.
static void hh_mqa_log(RDRAM_ARG const char* what, PTR(OSMesgQueue) mq_, OSMesg msg, int extra) {
    static const bool on = getenv("HH_MQLOG_ALL") != nullptr;
    if (!on) return;
    static FILE* f = nullptr;
    static long total = 0;
    static std::chrono::steady_clock::time_point t0{};
    if (f == nullptr) {
        f = fopen("hh_mq_all.log", "w");
        if (f == nullptr) return;
        t0 = std::chrono::steady_clock::now();
    }
    if (total > (128L * 1024 * 1024)) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int tid = -1;
    if (ultramodern::is_game_thread()) {
        tid = (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread()));
    }
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    unsigned thr = ultramodern::is_game_thread() ? (unsigned)ultramodern::this_thread() : 0;
    total += fprintf(f, "[MQA] t=%.3f %s tid=%d thr=%08X mq=%08X msg=%08X valid=%d recvHead=%08X sendHead=%08X extra=%d\n",
                     t, what, tid, thr, (unsigned)mq_, (unsigned)msg, (int)mq->validCount,
                     (unsigned)mq->blocked_on_recv, (unsigned)mq->blocked_on_send, extra);
    fflush(f);
}

// HH: traza quirurgica de la cola del helper sincrono de lectura ROM (0x8005C268, mb en 0x8005CD80):
// quien pide, quien entrega, si se duerme y si alguien lo despierta. Sirve para cazar la
// completacion perdida del cuelgue del NPC. Fichero hh_mq.log junto al exe (topes de tamano).
static void hh_mqlog(RDRAM_ARG const char* what, PTR(OSMesgQueue) mq_, PTR(OSThread) sender, int extra) {
    if ((uint32_t)mq_ != 0x8005C268u) return;
    static FILE* f = nullptr;
    static long total = 0;
    static std::chrono::steady_clock::time_point t0{};
    if (f == nullptr) {
        f = fopen("hh_mq.log", "w");
        if (f == nullptr) return;
        t0 = std::chrono::steady_clock::now();
    }
    if (total > (8L * 1024 * 1024)) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int tid = -1;
    if (ultramodern::is_game_thread()) {
        tid = (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread()));
    }
    int sid = -1;
    if (sender != NULLPTR) {
        sid = (int)hh_sh_get_id(TO_PTR(OSThread, sender));
    }
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    total += fprintf(f, "[MQ] t=%.3f %s tid=%d sender=%d valid=%d blockedHead=%08X extra=%d\n",
                     t, what, tid, sid, (int)mq->validCount, (unsigned)mq->blocked_on_recv, extra);
    fflush(f);
}

// HH: mensajes externos pendientes de repartir (diagnostico del watchdog de cuelgue).
extern "C" uint64_t hh_get_pending_ext_msgs() {
    return (uint64_t)external_messages.size_approx();
}

void ultramodern::enqueue_external_message_src(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, EventMessageSource src) {
    hh_check_mq("src", mq);
    external_messages.enqueue({mq, msg, jam, requeue_enabled[static_cast<int>(src)], NULLPTR});
}

void ultramodern::enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, bool requeue_if_blocked) {
    hh_check_mq("msg", mq);
    external_messages.enqueue({mq, msg, jam, requeue_if_blocked, NULLPTR});
}

void ultramodern::enqueue_external_message_to(PTR(OSMesgQueue) mq, OSMesg msg, bool jam, PTR(OSThread) target) {
    hh_check_mq("to", mq);
    external_messages.enqueue({mq, msg, jam, false, target});
}

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    std::vector<QueuedMessage> requeued_messages{};
    while (external_messages.try_dequeue(to_send)) {
        {
            uint32_t v = (uint32_t)to_send.mq;
            if (v != 0 && (v < 0x80000000u || v >= 0x80800000u || (v & 3u))) {
                static int hh_n = 0;
                if (hh_n++ < 30) fprintf(stderr, "[BADMQ] dequeue mq=%08X msg=%08X jam=%d req=%d target=%08X\n",
                    v, (unsigned)to_send.mesg, (int)to_send.jam, (int)to_send.requeue_if_blocked, (unsigned)to_send.target);
            }
        }
        hh_mqa_log(PASS_RDRAM "drain", to_send.mq, to_send.mesg, 0);
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false, to_send.target)) {
            if (to_send.requeue_if_blocked) {
                requeued_messages.push_back(to_send);
            }
            else {
                // HH: mensaje externo descartado por cola llena: si el juego lo espera en un
                // osRecvMesg bloqueante, ese hilo queda colgado (patron del cuelgue del NPC).
                static int hh_dropn = 0;
                if (hh_dropn++ < 100) {
                    fprintf(stderr, "[MQDROP] mq=%08X msg=%08X jam=%d (cola llena, sin requeue)\n",
                            (unsigned)to_send.mq, (unsigned)to_send.mesg, (int)to_send.jam);
                }
            }
        }
    }
    for (QueuedMessage& cur_mesg : requeued_messages) {
        external_messages.enqueue(cur_mesg);
    }
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    // Release the game lock while parked so other game threads can run.
    ultramodern::release_game_lock();
    external_messages.wait_dequeue(to_send);
    ultramodern::acquire_game_lock();
    if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false, to_send.target)) {
        if (to_send.requeue_if_blocked) {
            external_messages.enqueue(to_send);
        }
        else {
            static int hh_dropn = 0;
            if (hh_dropn++ < 100) {
                fprintf(stderr, "[MQDROP] (wait) mq=%08X msg=%08X jam=%d (cola llena, sin requeue)\n",
                        (unsigned)to_send.mq, (unsigned)to_send.mesg, (int)to_send.jam);
            }
        }
    }
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG u32 millis) {
    QueuedMessage to_send;
    ultramodern::release_game_lock();
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        ultramodern::acquire_game_lock();
        if (!do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false, to_send.target) && to_send.requeue_if_blocked) {
            external_messages.enqueue(to_send);
        }
    }
    else {
        ultramodern::acquire_game_lock();
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    HH_LOG("[MQ] osCreateMesgQueue mq=%p msg=%p count=%d\n", (void*)mq_, (void*)msg, (int)count);
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

// HH: traza quirúrgica del gate de tareas RSP (HH_GATELOG=1).
// - 0x8005C4F0: cola de tareas del hilo 17 (submit FUN_80000ed0 / recv FUN_80000bf0).
// - 0x8005C528: cola del camino de decremento del contador 0x8005CD4C.
static void hh_gatelog(RDRAM_ARG const char* what, PTR(OSMesgQueue) mq_, OSMesg msg) {
    if (getenv("HH_GATELOG") == nullptr) return;
    uint32_t mq = (uint32_t)mq_;
    if (mq != 0x8005C4F0u && mq != 0x8005C528u) return;
    uint32_t fl8 = 0;
    if (msg != 0) fl8 = *(uint32_t*)&rdram[((uint32_t)msg & 0x7FFFFF) + 8];
    fprintf(stderr, "[GATE2] %s mq=%08X msg=%08X fl8=%08X cnt=%u\n",
            what, mq, (uint32_t)msg, fl8, *(uint32_t*)&rdram[0x5CD4C]);
}

// HH: quita `t_` de una lista de bloqueados (cabeza en RDRAM). Devuelve true si estaba.
// Los enlaces `next` se leen de la sombra host (el struct guest puede estar pisado).
static bool remove_blocked_thread(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_) {
    PTR(PTR(OSThread))* head = TO_PTR(PTR(OSThread), queue_);
    int32_t prev = 0;
    int32_t cur = *head;
    while (cur != NULLPTR) {
        OSThread* c = TO_PTR(OSThread, cur);
        int32_t next = hh_sh_get_next(c);
        if (cur == (int32_t)t_) {
            if (prev == 0) {
                *head = next;
            } else {
                hh_sh_set_next(TO_PTR(OSThread, prev), next);
            }
            hh_sh_set_queue(TO_PTR(OSThread, t_), 0);
            return true;
        }
        prev = cur;
        cur = next;
    }
    return false;
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block, PTR(OSThread) target) {
    hh_gatelog(PASS_RDRAM "send", mq_, msg);
    hh_mqa_log(PASS_RDRAM "send", mq_, msg, jam ? 1 : 0);
    hh_mqlog(PASS_RDRAM "send-in", mq_, ultramodern::is_game_thread() ? ultramodern::this_thread() : NULLPTR, jam ? 1 : 0);
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (getenv("HH_QLOG") != nullptr) {
        fprintf(stderr, "[QS] tid=%d send mq=%p msg=%p jam=%d block=%d validBefore=%d\n",
                (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())), (void*)mq_, (void*)msg, (int)jam, (int)block, (int)mq->validCount);
    }
    if (!block) {
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
            return false;
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        while (MQ_IS_FULL(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())));
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }
    
    // HH: completación dirigida (SP/DP). No se inserta en el ring: se guarda para el hilo emisor
    // y se le despierta si está bloqueado en esta cola; si aún no se ha bloqueado, la consumirá
    // en su próximo osRecvMesg (take_pending_completion). Así ningún otro hilo puede robarla.
    if (target != NULLPTR) {
        PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
        bool blocked = false;
        {
            std::lock_guard lock{ pending_mutex };
            OSThread* t = TO_PTR(OSThread, target);
            blocked = ((PTR(PTR(OSThread)))(int32_t)hh_sh_get_queue(t) == blocked_queue);
            pending_completions[pending_key(target, mq_)].push(msg);
        }
        if (blocked) {
            remove_blocked_thread(PASS_RDRAM blocked_queue, target);
            ultramodern::schedule_running_thread(PASS_RDRAM target);
        }
        return true;
    }

    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        PTR(OSThread) woken = ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue);
        hh_mqlog(PASS_RDRAM "send-wake", mq_, woken, 0);
        ultramodern::schedule_running_thread(PASS_RDRAM woken);
    }
    
    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    hh_mqlog(PASS_RDRAM "recv-in", mq_, NULLPTR, block ? 1 : 0);
    if (block) hh_mqa_log(PASS_RDRAM "recv-block", mq_, 0, 0);
    // HH: completación dirigida pendiente para este hilo (la entrega el do_send dirigido).
    if (take_pending_completion(PASS_RDRAM ultramodern::this_thread(), mq_, msg_)) {
        hh_mqlog(PASS_RDRAM "recv-pend", mq_, NULLPTR, 0);
        hh_mqa_log(PASS_RDRAM "recv-pend", mq_, msg_ != NULLPTR ? *TO_PTR(OSMesg, msg_) : 0, 0);
        return true;
    }
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Otherwise, yield this thread in a loop until the queue is no longer full
        while (MQ_IS_EMPTY(mq)) {
            // Flush any externally-enqueued messages (from the VI/AI/SP/DP/etc. threads) into
            // the queue. Without this, a thread blocked on receive would never see a message
            // that arrives after it started blocking, since dequeue_external_messages is only
            // called at the start of osSendMesg/osRecvMesg.
            dequeue_external_messages(PASS_RDRAM1);
            if (!MQ_IS_EMPTY(mq)) {
                break;
            }
            {
                // Comprobar la pendiente dirigida e insertar en la lista de bloqueados bajo el
                // mismo lock con el que do_send decide si guardarla/despertar (evita la race).
                std::lock_guard lock{ pending_mutex };
                auto it = pending_completions.find(pending_key(ultramodern::this_thread(), mq_));
                if (it != pending_completions.end() && !it->second.empty()) {
                    if (msg_ != NULLPTR) {
                        *TO_PTR(OSMesg, msg_) = it->second.front();
                    }
                    it->second.pop();
                    if (it->second.empty()) {
                        pending_completions.erase(it);
                    }
                    return true;
                }
                HH_LOG("[MQ] BLOCK recv thread=%d mq=%p validCount=%d msgCount=%d\n",
                    (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())), (void*)mq_, (int)mq->validCount, (int)mq->msgCount);
                hh_mqlog(PASS_RDRAM "recv-block", mq_, NULLPTR, 0);
                debug_printf("[Message Queue] Thread %d is blocked on receive\n", hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())));
                ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            }
            // HH: al bloquear el hilo principal en su cola de comandos (0x8005C288) con la pila ya
            // hundida, volcar una vez el ring de llamadas del frame para localizar la fuga de sp.
            if ((uint32_t)mq_ == 0x8005C288u) {
                static int paradas2 = 0;
                uint32_t sp = hh_current_sp();
                if (sp != 0 && sp < 0x8005B000u) {
                    paradas2++;
                    if (paradas2 >= 3 && paradas2 <= 8) {
                        char nombre[32];
                        snprintf(nombre, sizeof(nombre), "hh_ring2_%d.log", paradas2);
                        FILE* f = fopen(nombre, "w");
                        if (f != nullptr) {
                            fprintf(f, "=== ring2 parada=%d sp=%08X n=%d ===\n", paradas2, sp, hh_ring2_count());
                            hh_ring2_dump_last(f, 60000);
                            fclose(f);
                        }
                    }
                }
            }
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    if ((uint32_t)mq_ == 0x8005C560u && getenv("HH_VERBOSE") != nullptr) {
        static uint64_t hh_n = 0;
        if (hh_n < 30 || (hh_n % 200) == 0) {
            fprintf(stderr, "[R560] n=%llu tid=%d msg=%08X\n", (unsigned long long)hh_n,
                (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())), (unsigned)TO_PTR(OSMesg, mq->msg)[mq->first]);
        }
        hh_n++;
    }
    hh_gatelog(PASS_RDRAM "recv", mq_, TO_PTR(OSMesg, mq->msg)[mq->first]);
    hh_mqlog(PASS_RDRAM "recv-ok", mq_, NULLPTR, 0);
    hh_mqa_log(PASS_RDRAM "recv-ok", mq_, TO_PTR(OSMesg, mq->msg)[mq->first], 0);
    // HH: sp del receptor + mensaje al consumir de la cola de comandos del hilo principal.
    if ((uint32_t)mq_ == 0x8005C288u) {
        static FILE* cf = nullptr;
        if (cf == nullptr) cf = fopen("hh_cmds.log", "w");
        if (cf != nullptr) {
            fprintf(cf, "sp=%08X msg=%08X\n", hh_current_sp(), (unsigned)TO_PTR(OSMesg, mq->msg)[mq->first]);
            fflush(cf);
        }
    }

    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;
    HH_LOG("[MQ] osSendMesg mq=%p msg=%p flags=%d\n", (void*)mq_, (void*)msg, flags);
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK, NULLPTR);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;
    
    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        ultramodern::enqueue_external_message(mq_, msg, jam, false);
        return 0;
    }
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK, NULLPTR);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    HH_LOG("[MQ] osRecvMesg mq=%p msg=%p flags=%d\n", (void*)mq_, (void*)msg_, flags);
    
    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");
    
    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to receive a message.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);
    
    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}
