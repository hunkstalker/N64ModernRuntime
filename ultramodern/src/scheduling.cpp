#include "ultramodern/ultramodern.hpp"

extern "C" void hh_schedlog(const char* fmt, ...);

void ultramodern::schedule_running_thread(RDRAM_ARG PTR(OSThread) t_) {
    debug_printf("[Scheduling] Adding thread %d to the running queue\n", hh_sh_get_id(TO_PTR(OSThread, t_)));
    hh_schedlog("queue tid=%d\n", (int)hh_sh_get_id(TO_PTR(OSThread, t_)));
    thread_queue_insert(PASS_RDRAM running_queue, t_);
    hh_sh_set_state(TO_PTR(OSThread, t_), OSThreadState::QUEUED);
}

void swap_to_thread(RDRAM_ARG OSThread *to) {
    debug_printf("[Scheduling] Thread %d giving execution to thread %d\n", hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())), hh_sh_get_id(to));
    hh_schedlog("swap from=%d to=%d\n", (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())), (int)hh_sh_get_id(to));
    // Insert this thread in the running queue. HH: al ceder usamos FIFO entre igual prioridad
    // (round-robin) para que el hilo que cede no quede por detrás de sus iguales para siempre.
    ultramodern::thread_queue_insert(PASS_RDRAM ultramodern::running_queue, ultramodern::this_thread(), /*fifo_equals=*/true);
    hh_sh_set_state(TO_PTR(OSThread, ultramodern::this_thread()), OSThreadState::QUEUED);
    // Unpause the target thread and wait for this one to be unpaused.
    ultramodern::resume_thread_and_wait(PASS_RDRAM to);
}

void ultramodern::check_running_queue(RDRAM_ARG1) {
    // Check if there are any threads in the running queue.
    if (!thread_queue_empty(PASS_RDRAM running_queue)) {
        // Check if the highest priority thread in the queue is higher priority than the current thread.
        OSThread* next_thread = TO_PTR(OSThread, ultramodern::thread_queue_peek(PASS_RDRAM running_queue));
        OSThread* self = TO_PTR(OSThread, ultramodern::this_thread());
        if (hh_sh_get_priority(next_thread) >= hh_sh_get_priority(self)) {
            ultramodern::thread_queue_pop(PASS_RDRAM running_queue);
            // Swap to the higher (or equal) priority thread.
            swap_to_thread(PASS_RDRAM next_thread);
        }
    }
}

extern "C" void pause_self(RDRAM_ARG1) {
    while (true) {
        // Wait until an external message arrives, then allow the next thread to run.
        ultramodern::wait_for_external_message(PASS_RDRAM1);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" void yield_self(RDRAM_ARG1) {
    ultramodern::wait_for_external_message(PASS_RDRAM1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}

extern "C" void yield_self_1ms(RDRAM_ARG1) {
    ultramodern::wait_for_external_message_timed(PASS_RDRAM1, 1);
    ultramodern::check_running_queue(PASS_RDRAM1);
}
