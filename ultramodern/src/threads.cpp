#include <cstdio>
#include <thread>
#include <cassert>
#include <string>
#include <mutex>
#include <chrono>
#include <cstdarg>
#include <unordered_map>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "blockingconcurrentqueue.h"

#include "ultramodern/threads.hpp"

// HH: traza del scheduler cooperativo (hh_sched.log, acotado): encolar/parar/despertar/siguiente por
// hilo. Imprescindible en cuelgues donde un hilo parece despertado pero nunca vuelve a ejecutarse.
extern "C" void hh_schedlog(const char* fmt, ...) {
    static FILE* f = nullptr;
    static long total = 0;
    static std::chrono::steady_clock::time_point t0{};
    if (f == nullptr) {
        f = fopen("hh_sched.log", "w");
        if (f == nullptr) return;
        t0 = std::chrono::steady_clock::now();
    }
    if (total > (16L * 1024 * 1024)) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    va_list args;
    va_start(args, fmt);
    total += fprintf(f, "[SCH] t=%.3f ", t);
    total += vfprintf(f, fmt, args);
    va_end(args);
    fflush(f);
}

// Native APIs only used to set thread names for easier debugging
#ifdef _WIN32
#include <Windows.h>
#endif

static ultramodern::threads::callbacks_t threads_callbacks;

void ultramodern::threads::set_callbacks(const callbacks_t& callbacks) {
    threads_callbacks = callbacks;
}

std::string ultramodern::threads::get_game_thread_name(const OSThread* t) {
    if (threads_callbacks.get_game_thread_name == nullptr) {
        return "Game Thread " + std::to_string(hh_sh_get_id(const_cast<OSThread*>(t)));
    }
    return threads_callbacks.get_game_thread_name(t);
}

extern "C" void bootproc();

thread_local bool is_entrypoint_thread = false;
// Whether this thread is part of the game (i.e. the start thread or one spawned by osCreateThread)
thread_local bool is_game_thread = false;
thread_local PTR(OSThread) thread_self = NULLPTR;
// HH: copia host del contexto de ESTE hilo. No releerla del struct guest (`thread_self->context`):
// el juego puede destruir el hilo y reutilizar su struct (el caso del objeto del NPC: los floats del
// objeto pisaron `context`/`sp` y el hilo acabó llamando a un semáforo basura). Ver nota 2026-09-15.
thread_local UltraThreadContext* self_context = nullptr;

// HH: registro host OSThread* -> UltraThreadContext* (los campos guest `context`/`sp` pueden quedar
// pisados por el juego). Es la fuente de verdad para senalizar/consultar el contexto de un hilo.
static std::mutex hh_ctx_mutex;
static std::unordered_map<uintptr_t, UltraThreadContext*> hh_ctx_map;

static void hh_ctx_register(OSThread* t, UltraThreadContext* ctx) {
    std::lock_guard<std::mutex> lock{ hh_ctx_mutex };
    hh_ctx_map[(uintptr_t)t] = ctx;
}

static UltraThreadContext* hh_ctx_get(OSThread* t) {
    std::lock_guard<std::mutex> lock{ hh_ctx_mutex };
    auto it = hh_ctx_map.find((uintptr_t)t);
    return it == hh_ctx_map.end() ? nullptr : it->second;
}

static void hh_ctx_unregister(OSThread* t) {
    std::lock_guard<std::mutex> lock{ hh_ctx_mutex };
    hh_ctx_map.erase((uintptr_t)t);
}

// HH: sombra host de los campos de scheduling del OSThread. La pila del hilo 5 puede desbordar
// sobre su propio struct (nota del 2026-09-15) y el juego puede reutilizar structs destruidos;
// el scheduler debe leer/escribir SIEMPRE la copia host, no la memoria guest. Los punteros de los
// enlaces (`next`/`queue`) siguen siendo direcciones guest (los usan las colas del juego), pero se
// guardan/leen de la sombra.
struct HhThreadShadow {
    int32_t next = 0;
    int32_t priority = 0;
    int32_t queue = 0;
    uint16_t state = 0;
    int32_t id = 0;
    int32_t sp = 0;
};

static std::mutex hh_sh_mutex;
static std::unordered_map<uintptr_t, HhThreadShadow> hh_sh_map;

static HhThreadShadow* hh_sh_find(OSThread* t) {
    if (t == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock{ hh_sh_mutex };
    auto it = hh_sh_map.find((uintptr_t)t);
    return it == hh_sh_map.end() ? nullptr : &it->second;
}

extern "C" void hh_sh_init(OSThread* t, int32_t next, int32_t priority, int32_t queue,
                           uint16_t state, int32_t id, int32_t sp) {
    std::lock_guard<std::mutex> lock{ hh_sh_mutex };
    HhThreadShadow& s = hh_sh_map[(uintptr_t)t];
    s.next = next; s.priority = priority; s.queue = queue;
    s.state = state; s.id = id; s.sp = sp;
}
extern "C" int32_t hh_sh_get_next(OSThread* t)    { HhThreadShadow* s = hh_sh_find(t); return s ? s->next : 0; }
extern "C" void    hh_sh_set_next(OSThread* t, int32_t v)    { if (HhThreadShadow* s = hh_sh_find(t)) s->next = v; }
extern "C" int32_t hh_sh_get_priority(OSThread* t){ HhThreadShadow* s = hh_sh_find(t); return s ? s->priority : 0; }
extern "C" void    hh_sh_set_priority(OSThread* t, int32_t v){ if (HhThreadShadow* s = hh_sh_find(t)) s->priority = v; }
extern "C" int32_t hh_sh_get_queue(OSThread* t)   { HhThreadShadow* s = hh_sh_find(t); return s ? s->queue : 0; }
extern "C" void    hh_sh_set_queue(OSThread* t, int32_t v)   { if (HhThreadShadow* s = hh_sh_find(t)) s->queue = v; }
extern "C" int32_t hh_sh_get_state(OSThread* t)   { HhThreadShadow* s = hh_sh_find(t); return s ? (int32_t)s->state : 0; }
extern "C" void    hh_sh_set_state(OSThread* t, int32_t v)   { if (HhThreadShadow* s = hh_sh_find(t)) s->state = (uint16_t)v; }
extern "C" int32_t hh_sh_get_id(OSThread* t)      { HhThreadShadow* s = hh_sh_find(t); return s ? s->id : -1; }
extern "C" int32_t hh_sh_get_sp(OSThread* t)      { HhThreadShadow* s = hh_sh_find(t); return s ? s->sp : 0; }

// The N64 is a single-CPU machine: only one game thread may ever be executing at a time.
// The cooperative scheduler handles yields, but osStartThread from the boot thread
// (thread_self == NULL) signals a thread without parking the boot thread, so both can run
// concurrently on their host threads. A global lock serializes all game-thread execution so
// the recompiled code observes a true single-threaded machine. Each game thread holds this
// lock while it runs and releases it every time it parks (see wait_for_resumed).
static std::mutex game_mutex;
static thread_local bool holds_game_lock = false;

void ultramodern::acquire_game_lock() {
    if (!holds_game_lock) {
        game_mutex.lock();
        holds_game_lock = true;
    }
}

void ultramodern::release_game_lock() {
    if (holds_game_lock) {
        holds_game_lock = false;
        game_mutex.unlock();
    }
}

void ultramodern::set_entrypoint_thread() {
    ::is_game_thread = true;
    ::is_entrypoint_thread = true;
}

bool ultramodern::is_entrypoint_thread() {
    return ::is_entrypoint_thread;
}

bool ultramodern::is_game_thread() {
    return ::is_game_thread;
}

#if 0
int main(int argc, char** argv) {
    ultramodern::set_entrypoint_thread();

    bootproc();
}
#endif

#if 1
void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg);
#else
#define run_thread_function(func, sp, arg) func(arg)
#endif

#if defined(_WIN32)
void ultramodern::set_native_thread_name(const std::string& name) {
    std::wstring wname{name.begin(), name.end()};

    HRESULT r;
    r = SetThreadDescription(
        GetCurrentThread(),
        wname.c_str()
    );
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    int nPriority = THREAD_PRIORITY_NORMAL;

    // Convert ThreadPriority to Win32 priority
    switch (pri) {
        case ThreadPriority::Low:
            nPriority = THREAD_PRIORITY_BELOW_NORMAL;
            break;
        case ThreadPriority::Normal:
            nPriority = THREAD_PRIORITY_NORMAL;
            break;
        case ThreadPriority::High:
            nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
            break;
        case ThreadPriority::VeryHigh:
            nPriority = THREAD_PRIORITY_HIGHEST;
            break;
        case ThreadPriority::Critical:
            nPriority = THREAD_PRIORITY_TIME_CRITICAL;
            break;
        default:
            throw std::runtime_error("Invalid thread priority!");
            break;
    }
    // SetThreadPriority(GetCurrentThread(), nPriority);
}
#elif defined(__linux__)
#include <sys/prctl.h>

void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Linux only accepts up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    prctl(PR_SET_NAME, name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {
    // TODO linux thread priority
    // printf("set_native_thread_priority unimplemented\n");
    // int nPriority = THREAD_PRIORITY_NORMAL;

    // // Convert ThreadPriority to Win32 priority
    // switch (pri) {
    //     case ThreadPriority::Low:
    //         nPriority = THREAD_PRIORITY_BELOW_NORMAL;
    //         break;
    //     case ThreadPriority::Normal:
    //         nPriority = THREAD_PRIORITY_NORMAL;
    //         break;
    //     case ThreadPriority::High:
    //         nPriority = THREAD_PRIORITY_ABOVE_NORMAL;
    //         break;
    //     case ThreadPriority::VeryHigh:
    //         nPriority = THREAD_PRIORITY_HIGHEST;
    //         break;
    //     case ThreadPriority::Critical:
    //         nPriority = THREAD_PRIORITY_TIME_CRITICAL;
    //         break;
    //     default:
    //         throw std::runtime_error("Invalid thread priority!");
    //         break;
    // }
}
#elif defined(__APPLE__)
void ultramodern::set_native_thread_name(const std::string& name) {
    if (name.length() > 15) {
        // Macs seem to only accept up to 16 characters including the null terminator for a thread name.
        debug_printf("[Thread] The thread name '%s' will be truncated to 15 characters", name.c_str());
    }

    pthread_setname_np(name.c_str());
}

void ultramodern::set_native_thread_priority(ThreadPriority pri) {}
#endif

void wait_for_resumed(RDRAM_ARG UltraThreadContext* thread_context) {
    // Release the game lock so the thread being resumed can acquire it and run; then park this
    // thread. Re-acquire the lock once we're resumed.
    if (ultramodern::is_game_thread()) {
        hh_schedlog("park tid=%d\n", (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())));
    }
    ultramodern::release_game_lock();
    thread_context->running.wait();
    ultramodern::acquire_game_lock();
    if (ultramodern::is_game_thread()) {
        hh_schedlog("wake tid=%d\n", (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())));
    }
    // If this thread's context was replaced by another thread or deleted, destroy it again from its own context.
    // This will trigger thread cleanup instead.
    // HH: comprobar contra el registro host (el struct guest puede estar pisado/reutilizado).
    if (ultramodern::is_game_thread() && ultramodern::this_thread() != NULLPTR) {
        if (hh_ctx_get(TO_PTR(OSThread, ultramodern::this_thread())) != thread_context) {
            hh_schedlog("self-destroy tid=%d\n", (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())));
            osDestroyThread(PASS_RDRAM NULLPTR);
        }
    }
}

void resume_thread(OSThread* t) {
    debug_printf("[Thread] Resuming execution of thread %d\n", hh_sh_get_id(t));
    hh_schedlog("signal tid=%d\n", (int)hh_sh_get_id(t));
    // HH: contexto por el registro host (el campo guest puede estar pisado por el juego).
    UltraThreadContext* ctx = hh_ctx_get(t);
    if (ctx == nullptr) return;  // hilo ya destruido
    ctx->running.signal();
}

void run_next_thread(RDRAM_ARG1) {
    if (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        throw std::runtime_error("No threads left to run!\n");
    }

    OSThread* to_run = TO_PTR(OSThread, ultramodern::thread_queue_pop(PASS_RDRAM ultramodern::running_queue));
    debug_printf("[Scheduling] Resuming execution of thread %d\n", hh_sh_get_id(to_run));
    hh_schedlog("next tid=%d\n", (int)hh_sh_get_id(to_run));
    // HH: contexto por el registro host (el campo guest puede estar pisado por el juego).
    UltraThreadContext* ctx = hh_ctx_get(to_run);
    if (ctx == nullptr) return;
    ctx->running.signal();
}

void ultramodern::run_next_thread_and_wait(RDRAM_ARG1) {
    // HH: contexto propio del hilo (thread_local), no `thread_self->context` (struct guest, puede
    // estar destruido/reutilizado por el juego). Fallback al struct si no se inicializo (boot).
    UltraThreadContext* cur_context = self_context != nullptr ? self_context : TO_PTR(OSThread, thread_self)->context;
    hh_schedlog("rntw tid=%d\n", (int)hh_sh_get_id(TO_PTR(OSThread, thread_self)));
    // If there are no runnable game threads, idle on external (hardware/OS) messages instead of
    // aborting. Processing an external message on this game thread delivers it under the game
    // lock and may schedule a thread blocked on receive into the running queue. (racer patch:
    // "run_next_thread_and_wait idles on external messages when running queue is empty".)
    while (ultramodern::thread_queue_empty(PASS_RDRAM ultramodern::running_queue)) {
        hh_schedlog("rntw-idle tid=%d\n", (int)hh_sh_get_id(TO_PTR(OSThread, thread_self)));
        ultramodern::wait_for_external_message(PASS_RDRAM1);
    }
    run_next_thread(PASS_RDRAM1);
    wait_for_resumed(PASS_RDRAM cur_context);
}

void ultramodern::resume_thread_and_wait(RDRAM_ARG OSThread *t) {
    UltraThreadContext* cur_context = self_context != nullptr ? self_context : TO_PTR(OSThread, thread_self)->context;
    resume_thread(t);
    wait_for_resumed(PASS_RDRAM cur_context);
}

static void _thread_func(RDRAM_ARG PTR(OSThread) self_, PTR(thread_func_t) entrypoint, PTR(void) arg, UltraThreadContext* thread_context) {
    OSThread *self = TO_PTR(OSThread, self_);
    debug_printf("[Thread] Thread created: %d\n", hh_sh_get_id(self));
    thread_self = self_;
    is_game_thread = true;
    self_context = thread_context;

    // Keep the game's __osRunningThread global (0x80049940) in sync so that
    // game code that reads the current thread directly sees the right value.
    *(uint32_t*)(rdram + 0x49940) = (uint32_t)self_;

    // Set the thread name
    ultramodern::set_native_thread_name(ultramodern::threads::get_game_thread_name(self));
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);

    // Signal the initialized semaphore to indicate that this thread can be started.
    thread_context->initialized.signal();

    debug_printf("[Thread] Thread waiting to be started: %d\n", hh_sh_get_id(self));

    // Ensure the context is always cleaned up (and its host_thread joined) no matter how
    // this function exits. Otherwise a leaked/unjoined host_thread can be destroyed while
    // still joinable -> std::terminate ("terminate called without an active exception").
    struct CleanupGuard {
        OSThread* self;
        UltraThreadContext* ctx;
        ~CleanupGuard() {
            // HH: quitar del registro host antes de destruir el contexto.
            hh_ctx_unregister(self);
            ultramodern::cleanup_thread(ctx);
        }
    } guard{self, thread_context};

    // Wait until the thread is marked as running.
    try {
        wait_for_resumed(PASS_RDRAM thread_context);
    } catch (...) {
    }

    // Make sure the thread wasn't replaced or destroyed before it was started.
    if (hh_ctx_get(self) == thread_context) {
        debug_printf("[Thread] Thread started: %d\n", hh_sh_get_id(self));
        try {
            // Run the thread's function with the provided argument.
            run_thread_function(PASS_RDRAM entrypoint, hh_sh_get_sp(self), arg);
        } catch (...) {
        }
    }
    else {
        debug_printf("[Thread] Thread destroyed before being started: %d\n", hh_sh_get_id(self));
    }

    // Check if the thread hasn't been destroyed or replaced. If so, then the thread terminated or destroyed itself,
    // so mark this thread as destroyed and run the next queued thread.
    if (hh_ctx_get(self) == thread_context) {
        self->context = nullptr;
        // Release the game lock before handing execution to the next thread, otherwise the
        // thread we resume will block forever trying to acquire it.
        ultramodern::release_game_lock();
        try {
            run_next_thread(PASS_RDRAM1);
        } catch (...) {
        }
    }
    else {
        // Thread was destroyed before it was started; it still holds the game lock (acquired by
        // wait_for_resumed), so release it before exiting so other threads can run.
        ultramodern::release_game_lock();
    }
}

extern "C" void osStartThread(RDRAM_ARG PTR(OSThread) t_) {
    OSThread* t = TO_PTR(OSThread, t_);
    HH_LOG("[TH] osStartThread t=%p id=%d\n", (void*)t_, (int)hh_sh_get_id(t));
    debug_printf("[os] Start Thread %d\n", hh_sh_get_id(t));
    hh_schedlog("start tid=%d t=%08X\n", (int)hh_sh_get_id(t), (unsigned)t_);

    // If this is a game thread, insert the new thread into the running queue and then check the running queue.
    if (thread_self) {
        ultramodern::schedule_running_thread(PASS_RDRAM t_);
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
    // Otherwise, immediately start the thread and terminate this one.
    else {
        hh_sh_set_state(t, OSThreadState::QUEUED);
        resume_thread(t);
        //throw ultramodern::thread_terminated{};
    }
}

extern "C" void osCreateThread(RDRAM_ARG PTR(OSThread) t_, OSId id, PTR(thread_func_t) entrypoint, PTR(void) arg, PTR(void) sp, OSPri pri) {
    debug_printf("[os] Create Thread %d\n", id);
    HH_LOG("[TH] osCreateThread t=%p id=%d entry=0x%x arg=0x%x\n", (void*)t_, (int)id, (uint32_t)entrypoint, (uint32_t)arg);
    hh_schedlog("create tid=%d t=%08X entry=%08X pri=%d\n", (int)id, (unsigned)t_, (unsigned)entrypoint, (int)pri);
    OSThread *t = TO_PTR(OSThread, t_);
    
    t->next = NULLPTR;
    t->queue = NULLPTR;
    t->priority = pri;
    t->id = id;
    t->state = OSThreadState::STOPPED;
    t->sp = sp - 0x10; // Set up the first stack frame
    // HH: la sombra host es la fuente de verdad del scheduling (el struct guest puede pisarse).
    hh_sh_init(t, NULLPTR, pri, NULLPTR, OSThreadState::STOPPED, id, sp - 0x10);

    // Spawn a new thread, which will immediately pause itself and wait until it's been started.
    // Pass the context as an argument to the thread function to ensure that it can't get cleared before the thread captures its value.
    UltraThreadContext* context = new UltraThreadContext{};
    t->context = context;
    // HH: registrar en el mapa host antes de lanzar el hilo (fuente de verdad del contexto).
    hh_ctx_register(t, context);
    context->host_thread = std::thread{_thread_func, PASS_RDRAM t_, entrypoint, arg, t->context};

    // Wait until the thread is initialized to indicate that it's ready to be started.
    context->initialized.wait();
    debug_printf("[os] Thread %d is ready to be started\n", hh_sh_get_id(t));
}

extern "C" void osStopThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    hh_schedlog("stop tid=%d t=%08X self=%d\n", (int)hh_sh_get_id(t), (unsigned)t_, (t_ == thread_self) ? 1 : 0);
    // Check if the thread is stopping itself (arg is null or thread_self).
    if (t_ == thread_self) {
        ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
    }
    else {
        // Stop another thread: deschedule it from the run queue and mark it stopped.
        // Its host thread stays parked (it's not the active one), and it can be
        // restarted later with osStartThread. Do NOT null the context (that would
        // destroy it); stopping keeps the thread restorable.
        if (hh_sh_get_state(t) != OSThreadState::STOPPED) {
            ultramodern::thread_queue_remove(PASS_RDRAM (PTR(PTR(OSThread)))(int32_t)hh_sh_get_queue(t), t_);
            hh_sh_set_state(t, OSThreadState::STOPPED);
        }
    }
}

extern "C" void osDestroyThread(RDRAM_ARG PTR(OSThread) t_) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);
    hh_schedlog("destroy tid=%d t=%08X self=%d\n", (int)hh_sh_get_id(t), (unsigned)t_, (t_ == thread_self) ? 1 : 0);
    // Check if the thread is destroying itself (arg is null or thread_self)
    if (t_ == thread_self) {
        throw ultramodern::thread_terminated{};
    }
    // Otherwise if the thread isn't stopped, remove it from its currrent queue., 
    if (hh_sh_get_state(t) != OSThreadState::STOPPED) {
        ultramodern::thread_queue_remove(PASS_RDRAM (PTR(PTR(OSThread)))(int32_t)hh_sh_get_queue(t), t_);
    }
    // Check if the thread has already been destroyed to prevent destroying it again.
    // HH: el registro host es la fuente de verdad (el campo guest puede estar pisado).
    UltraThreadContext* cur_context = hh_ctx_get(t);
    if (cur_context != nullptr) {
        // Mark the target thread as destroyed and resume it. When it starts it'll check this and terminate itself instead of resuming.
        hh_ctx_unregister(t);
        t->context = nullptr;
        cur_context->running.signal();
    }
}

extern "C" void osSetThreadPri(RDRAM_ARG PTR(OSThread) t_, OSPri pri) {
    if (t_ == NULLPTR) {
        t_ = thread_self;
    }
    OSThread* t = TO_PTR(OSThread, t_);

    if (hh_sh_get_priority(t) != pri) {
        hh_sh_set_priority(t, pri);

        if (t_ != ultramodern::this_thread() && hh_sh_get_state(t) != OSThreadState::STOPPED) {
            ultramodern::thread_queue_remove(PASS_RDRAM (PTR(PTR(OSThread)))(int32_t)hh_sh_get_queue(t), t_);
            ultramodern::thread_queue_insert(PASS_RDRAM (PTR(PTR(OSThread)))(int32_t)hh_sh_get_queue(t), t_);
        }

        ultramodern::check_running_queue(PASS_RDRAM1);
    }
}

extern "C" OSPri osGetThreadPri(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return (OSPri)hh_sh_get_priority(TO_PTR(OSThread, t));
}

extern "C" OSId osGetThreadId(RDRAM_ARG PTR(OSThread) t) {
    if (t == NULLPTR) {
        t = thread_self;
    }
    return TO_PTR(OSThread, t)->id;
}

PTR(OSThread) ultramodern::this_thread() {
    return thread_self;
}

static std::thread thread_cleaner_thread;
static moodycamel::BlockingConcurrentQueue<UltraThreadContext*> deleted_threads{};
extern std::atomic_bool exited;

void thread_cleaner_func() {
    using namespace std::chrono_literals;
    while (!exited) {
        UltraThreadContext* to_delete;
        if (deleted_threads.wait_dequeue_timed(to_delete, 10ms)) {
            debug_printf("[Cleanup] Deleting thread context %p\n", to_delete);

            if (to_delete->host_thread.joinable()) {
                to_delete->host_thread.join();
            }
            delete to_delete;
        }
    }
}

void ultramodern::init_thread_cleanup() {
    thread_cleaner_thread = std::thread{thread_cleaner_func};
}

void ultramodern::cleanup_thread(UltraThreadContext *cur_context) {
    // Guard against enqueueing the same context more than once (e.g. if cleanup is triggered
    // by more than one path). Double-enqueueing would make the cleaner join() a thread that
    // is no longer joinable and throw std::system_error -> std::terminate.
    if (!cur_context->cleaned_up.exchange(true)) {
        deleted_threads.enqueue(cur_context);
    }
}

void ultramodern::join_thread_cleaner_thread() {
    thread_cleaner_thread.join();
}
