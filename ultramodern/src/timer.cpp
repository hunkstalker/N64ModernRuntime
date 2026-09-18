#include <thread>
#include <variant>
#include <set>
#include <cstdlib>
#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include "Windows.h"
#endif

// Start time for the program
static std::chrono::high_resolution_clock::time_point start_time = std::chrono::high_resolution_clock::now();
// Offset of the duration since program start used to calculate the value for osGetTime. 
static int64_t ostime_offset = 0;
// HH: reloj de juego esclavo del replay (HH_REPLAY_CLOCK=1, lo activa el port desde input.cpp).
// Con el replay activo, el tiempo de juego sigue la marca temporal de la muestra grabada en vez del
// reloj de pared, para que los waits dependientes de tiempo consuman los mismos frames que en la
// grabacion (fidelidad del diferencial port<->emulador). Ver notes/2026-09-17-cac-timeline-modulo24-periodo.md.
static bool hh_replay_clock_active = false;
static int64_t hh_replay_time = 0;      // valor fijado en la ultima muestra
static int64_t hh_replay_base = 0;
static double hh_replay_t0 = 0.0;
static uint64_t hh_replay_wall = 0;     // time_now() de la ultima muestra
static double hh_replay_t_set = 0.0;    // t de la ultima muestra
static double hh_replay_rate = 1.0;     // segundos de grabacion por segundo de pared
static constexpr int64_t hh_ticks_per_sec = 46'875'000; // counter_per_ms * 1000
// HH: interpolacion DETERMINISTA del reloj de replay: en vez de avanzar con tiempo de pared real
// (que hacia el replay no reproducible), avanza por VI (como el N64). 60 VI/s.
extern "C" uint64_t hh_get_vi_count(void);
static uint64_t hh_replay_vi0 = 0;      // VI al aplicar la ultima muestra
static constexpr int64_t hh_counter_per_vi = (int64_t)46'875 * 1000 / 60; // 781250
// Game speed multiplier (1 means no speedup)
constexpr uint32_t speed_multiplier = 1;
// N64 CPU counter ticks per millisecond
constexpr uint32_t counter_per_ms = 46'875 * speed_multiplier;

struct OSTimer {
    PTR(OSTimer) unused1;
    PTR(OSTimer) unused2;
    OSTime interval;
    OSTime timestamp;
    PTR(OSMesgQueue) mq;
    OSMesg msg;
};

struct AddTimerAction {
    PTR(OSTimer) timer;
};

struct RemoveTimerAction {
    PTR(OSTimer) timer;
};

using Action = std::variant<AddTimerAction, RemoveTimerAction>;

struct {
    std::thread thread;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
} timer_context;

uint64_t duration_to_ticks(std::chrono::high_resolution_clock::duration duration) {
    uint64_t delta_micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    // More accurate than using a floating point timer, will only overflow after running for 12.47 years
    // Units: (micros * (counts/millis)) / (micros/millis) = counts
    uint64_t total_count = (delta_micros * counter_per_ms) / 1000;

    return total_count;
}

std::chrono::microseconds ticks_to_duration(uint64_t ticks) {
    using namespace std::chrono_literals;
    return ticks * 1000us / counter_per_ms;
}

std::chrono::high_resolution_clock::time_point ticks_to_timepoint(uint64_t ticks) {
    return start_time + ticks_to_duration(ticks);
}

uint64_t time_now() {
    // HH: HH_TIMESCALE=<f> escala el reloj del juego (para el replay frame-indexado: con el port a
    // ~28,9 fps, un factor ~0,963 hace que el tiempo interno por frame sea 1/30 s, como el original,
    // y los waits por tiempo consuman el mismo numero de frames; el limitador del juego sigue
    // mandando sobre la velocidad de pared). Ver notes/2026-09-17-bizhawk-replay-freeze-con-rafaga.md.
    static const double hh_scale = [] {
        const char* ts = getenv("HH_TIMESCALE");
        if (ts == nullptr || *ts == '\0') return 1.0;
        double f = atof(ts);
        return (f > 0.1 && f < 10.0) ? f : 1.0;
    }();
    uint64_t t = duration_to_ticks(std::chrono::high_resolution_clock::now() - start_time);
    if (hh_scale == 1.0) return t;
    return (uint64_t)((double)t * hh_scale);
}

extern "C" void hh_replay_clock_set(double t_seconds) {
    uint64_t wall = time_now();
    if (!hh_replay_clock_active) {
        hh_replay_clock_active = true;
        hh_replay_base = (int64_t)wall - ostime_offset;
        hh_replay_t0 = t_seconds;
        hh_replay_rate = 1.0;
    } else {
        // Tasa local (segundos de grabacion / segundo de pared) para interpolar dentro del frame y
        // evitar busy-waits congelados (el reloj solo avanzaba al aplicar muestra).
        double wall_dt = (double)(wall - hh_replay_wall) / (double)hh_ticks_per_sec;
        double rec_dt = t_seconds - hh_replay_t_set;
        if (wall_dt > 1e-4 && rec_dt > 0.0) {
            hh_replay_rate = rec_dt / wall_dt;
            if (hh_replay_rate > 2.0) hh_replay_rate = 2.0;
            if (hh_replay_rate < 0.05) hh_replay_rate = 0.05;
        }
    }
    hh_replay_time = hh_replay_base + (int64_t)((t_seconds - hh_replay_t0) * (double)hh_ticks_per_sec);
    hh_replay_wall = wall;
    hh_replay_t_set = t_seconds;
    hh_replay_vi0 = hh_get_vi_count();
}

extern "C" bool hh_replay_clock_on() {
    return hh_replay_clock_active;
}

// HH: indice de muestra del replay aplicada (para correlacionar eventos con la grabacion).
static uint64_t hh_replay_sample = 0;
extern "C" void hh_replay_set_sample(uint64_t idx) {
    hh_replay_sample = idx;
}
extern "C" uint64_t hh_replay_get_sample() {
    return hh_replay_sample;
}

void timer_thread(RDRAM_ARG1) {
    ultramodern::set_native_thread_name("Timer Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::VeryHigh);

    // Lambda comparator function to keep the set ordered
    auto timer_sort = [PASS_RDRAM1](PTR(OSTimer) a_, PTR(OSTimer) b_) {
        OSTimer* a = TO_PTR(OSTimer, a_);
        OSTimer* b = TO_PTR(OSTimer, b_);

        // Order by timestamp if the timers have different timestamps
        if (a->timestamp != b->timestamp) {
            return a->timestamp < b->timestamp;
        }

        // If they have the exact same timestamp then order by address instead
        return a < b;
    };

    // Ordered set of timers that are currently active
    std::set<PTR(OSTimer), decltype(timer_sort)> active_timers{timer_sort};
    
    // Lambda to process a timer action to handle adding and removing timers
    auto process_timer_action = [&](const Action& action) {
        // Determine the action type and act on it
        if (const auto* add_action = std::get_if<AddTimerAction>(&action)) {
            active_timers.insert(add_action->timer);
        } else if (const auto* remove_action = std::get_if<RemoveTimerAction>(&action)) {
            active_timers.erase(remove_action->timer);
        }
    };

    while (true) {
        // Empty the action queue
        Action cur_action;
        while (timer_context.action_queue.try_dequeue(cur_action)) {
            process_timer_action(cur_action);
        }

        // If there's no timer to act on, wait for one to come in from the action queue
        while (active_timers.empty()) {
            timer_context.action_queue.wait_dequeue(cur_action);
            process_timer_action(cur_action);
        }

        // Get the timer that's closest to running out
        PTR(OSTimer) cur_timer_ = *active_timers.begin();
        OSTimer* cur_timer = TO_PTR(OSTimer, cur_timer_);

        // Remove the timer from the queue (it may get readded if waiting is interrupted)
        active_timers.erase(cur_timer_);

        // Determine how long to wait to reach the timer's timestamp
        auto wait_duration = ticks_to_timepoint(cur_timer->timestamp) - std::chrono::high_resolution_clock::now();

        // Wait for either the duration to complete or a new action to come through
        if (wait_duration.count() >= 0 && timer_context.action_queue.wait_dequeue_timed(cur_action, wait_duration)) {
            // Timer was interrupted by a new action 
            // Add the current timer back to the queue (done first in case the action is to remove this timer)
            active_timers.insert(cur_timer_);
            // Process the new action
            process_timer_action(cur_action);
        }
        else {
            // Waiting for the timer completed, so send the timer's message to its message queue
            HH_LOG("[TIM] fire t=%p interval=%lld mq=%p\n", (void*)cur_timer_, (long long)cur_timer->interval, (void*)cur_timer->mq);
            ultramodern::enqueue_external_message_src(cur_timer->mq, cur_timer->msg, false, ultramodern::EventMessageSource::Timer);
            // If the timer has a specified interval then reload it with that value
            if (cur_timer->interval != 0) {
                cur_timer->timestamp = cur_timer->interval + time_now();
                active_timers.insert(cur_timer_);
            }
        }
    }
}

void ultramodern::init_timers(RDRAM_ARG1) {
    timer_context.thread = std::thread{ timer_thread, PASS_RDRAM1 };
    timer_context.thread.detach();
}

uint32_t ultramodern::get_speed_multiplier() {
    return speed_multiplier;
}

std::chrono::high_resolution_clock::time_point ultramodern::get_start() {
    return start_time;
}

std::chrono::high_resolution_clock::duration ultramodern::time_since_start() {
    return std::chrono::high_resolution_clock::now() - start_time;
}

extern "C" u32 osGetCount() {
    if (hh_replay_clock_active) {
        // HH: interpolacion determinista por VI (no tiempo de pared).
        int64_t extra = (int64_t)(hh_get_vi_count() - hh_replay_vi0) * hh_counter_per_vi;
        return (uint32_t)(hh_replay_time + extra);
    }
    uint64_t total_count = time_now();

    // Allow for overflows, which is how osGetCount behaves
    return (uint32_t)total_count;
}

extern "C" void osSetCount(u32 count) {
    assert(false);
}

extern "C" OSTime osGetTime() {
    if (hh_replay_clock_active) {
        // HH: valor de la muestra + interpolacion DETERMINISTA por VI (no tiempo de pared real),
        // para que el replay sea reproducible (los waits por osGetTime consumen los mismos VI).
        int64_t extra = (int64_t)(hh_get_vi_count() - hh_replay_vi0) * hh_counter_per_vi;
        return (OSTime)(hh_replay_time + extra);
    }
    uint64_t total_count = time_now() - ostime_offset;

    return total_count;
}

extern "C" void osSetTime(OSTime t) {
    if (hh_replay_clock_active) {
        // HH: con el reloj de replay activo, osSetTime reajusta la base para no perder la referencia.
        hh_replay_base += ((int64_t)t - hh_replay_time);
        hh_replay_time = (int64_t)t;
        return;
    }
    ostime_offset = time_now() - t;
}

extern "C" int osSetTimer(RDRAM_ARG PTR(OSTimer) t_, OSTime countdown, OSTime interval, PTR(OSMesgQueue) mq, OSMesg msg) {
    OSTimer* t = TO_PTR(OSTimer, t_);

    // Determine the time when this timer will trigger off
    if (countdown == 0) {
        // Set the timestamp based on the interval
        t->timestamp = interval + time_now();
    } else {
        t->timestamp = countdown + time_now();
    }
    t->interval = interval;
    t->mq = mq;
    t->msg = msg;

    HH_LOG("[TIM] set t=%p countdown=%lld interval=%lld mq=%p\n", (void*)t_, (long long)countdown, (long long)interval, (void*)mq);

    timer_context.action_queue.enqueue(AddTimerAction{ t_ });

    return 0;
}

extern "C" int osStopTimer(RDRAM_ARG PTR(OSTimer) t_) {
    timer_context.action_queue.enqueue(RemoveTimerAction{ t_ });

    // TODO don't blindly return 0 here; requires some response from the timer thread to know what the returned value was
    return 0;
}

#ifdef _WIN32

// The implementations of std::chrono::sleep_until and sleep_for were affected by changing the system clock backwards in older versions
// of Microsoft's STL. This was fixed as of Visual Studio 2022 17.9, but to be safe ultramodern uses Win32 Sleep directly.
void ultramodern::sleep_milliseconds(uint32_t millis) {
    Sleep(millis);
}

void ultramodern::sleep_until(const std::chrono::high_resolution_clock::time_point& time_point) {
    auto time_now = std::chrono::high_resolution_clock::now();
    if (time_point > time_now) {
        long long delta_ms = std::chrono::ceil<std::chrono::milliseconds>(time_point - time_now).count();
        // printf("Sleeping %lld %d ms\n", delta_ms, (uint32_t)delta_ms);
        Sleep(delta_ms);
    }
}

#else

void ultramodern::sleep_milliseconds(uint32_t millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds{millis});
}

void ultramodern::sleep_until(const std::chrono::high_resolution_clock::time_point& time_point) {
    std::this_thread::sleep_until(time_point);
}

#endif
