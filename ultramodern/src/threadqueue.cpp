#include <cassert>

#include "ultramodern/ultramodern.hpp"

static PTR(OSThread) running_queue_impl = NULLPTR;

static PTR(OSThread)* queue_to_ptr(RDRAM_ARG PTR(PTR(OSThread)) queue) {
    if (queue == ultramodern::running_queue) {
        return &running_queue_impl;
    }
    return TO_PTR(PTR(OSThread), queue);
}

// HH: los enlaces (next/priority/queue) se leen/escriben de la SOMBRA host (hh_sh_*), no del struct
// guest: la pila del hilo puede desbordar sobre su propio struct. Las cabezas de lista siguen en
// memoria guest (structs de cola del juego y running_queue_impl).
void ultramodern::thread_queue_insert(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) toadd_, bool fifo_equals) {
    PTR(OSThread)* head = queue_to_ptr(PASS_RDRAM queue_);
    OSThread* toadd = TO_PTR(OSThread, toadd_);
    const int32_t pri = hh_sh_get_priority(toadd);
    debug_printf("[Thread Queue] Inserting thread %d into queue 0x%08X\n", hh_sh_get_id(toadd), (uintptr_t)queue_);
    // HH: `fifo_equals` (solo al ceder el CPU: swap_to_thread) inserta los empates de prioridad al
    // final (round-robin, como libultra). Sin el flag se mantiene el orden original de upstream
    // (`>` = LIFO entre iguales) para no alterar el orden de creacion/wake de hilos.
    int32_t prev = 0;
    int32_t cur = *head;
    while (cur != 0) {
        OSThread* c = TO_PTR(OSThread, cur);
        const int32_t cp = hh_sh_get_priority(c);
        const bool advance = fifo_equals ? (cp >= pri) : (cp > pri);
        if (!advance) break;
        prev = cur;
        cur = hh_sh_get_next(c);
    }
    hh_sh_set_next(toadd, cur);
    hh_sh_set_queue(toadd, (int32_t)queue_);
    if (prev == 0) {
        *head = toadd_;
    } else {
        hh_sh_set_next(TO_PTR(OSThread, prev), toadd_);
    }

    debug_printf("  Contains:");
    cur = *head;
    while (cur != 0) {
        OSThread* c = TO_PTR(OSThread, cur);
        debug_printf("%d (%d) ", hh_sh_get_id(c), hh_sh_get_priority(c));
        cur = hh_sh_get_next(c);
    }
    debug_printf("\n");
}

PTR(OSThread) ultramodern::thread_queue_pop(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    PTR(OSThread) ret = *queue;
    if (ret == NULLPTR) {
        return ret;
    }
    OSThread* t = TO_PTR(OSThread, ret);
    *queue = hh_sh_get_next(t);
    hh_sh_set_queue(t, 0);
    debug_printf("[Thread Queue] Popped thread %d from queue 0x%08X\n", hh_sh_get_id(t), (uintptr_t)queue_);
    return ret;
}

bool ultramodern::thread_queue_remove(RDRAM_ARG PTR(PTR(OSThread)) queue_, PTR(OSThread) t_) {
    PTR(OSThread)* head = queue_to_ptr(PASS_RDRAM queue_);
    debug_printf("[Thread Queue] Removing thread %d from queue 0x%08X\n", hh_sh_get_id(TO_PTR(OSThread, t_)), (uintptr_t)queue_);

    int32_t prev = 0;
    int32_t cur = *head;
    while (cur != 0) {
        OSThread* c = TO_PTR(OSThread, cur);
        int32_t next = hh_sh_get_next(c);
        if (cur == (int32_t)t_) {
            if (prev == 0) {
                *head = next;
            } else {
                hh_sh_set_next(TO_PTR(OSThread, prev), next);
            }
            hh_sh_set_queue(c, 0);
            return true;
        }
        prev = cur;
        cur = next;
    }
    return false;
}

bool ultramodern::thread_queue_empty(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    return *queue == NULLPTR;
}

PTR(OSThread) ultramodern::thread_queue_peek(RDRAM_ARG PTR(PTR(OSThread)) queue_) {
    PTR(OSThread)* queue = queue_to_ptr(PASS_RDRAM queue_);
    return *queue;
}
