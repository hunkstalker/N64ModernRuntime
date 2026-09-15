#include <mutex>
#include <vector>

#include "ultramodern/extensions.h"
#include "ultramodern/ultramodern.hpp"

// HH: traza de eventos de displaylist (osExQueueDisplaylistEvent y su dispatch). Un evento que
// nunca casa deja el mensaje pendiente y cuelga al hilo que lo espera (cuelgue por dano).
extern "C" void hh_dl_log(const char* what, unsigned dl, unsigned type, unsigned mq, unsigned msg) {
    if (getenv("HH_MQLOG_ALL") == nullptr) return;
    static FILE* f = nullptr;
    static long total = 0;
    if (f == nullptr) { f = fopen("hh_dl.log", "w"); if (f == nullptr) return; }
    if (total > (4L * 1024 * 1024)) return;
    total += fprintf(f, "[DL] %s dl=%08X type=%u mq=%08X msg=%08X\n", what, dl, type, mq, msg);
    fflush(f);
}


struct DLEvent {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    PTR(void) displaylist;
    u32 event_type;
};

struct {
    struct {
        std::mutex dl_event_mutex;
        std::vector<DLEvent> pending_events;
    } dl_events;
} extension_state;

extern "C" void osExQueueDisplaylistEvent(PTR(OSMesgQueue) mq, OSMesg mesg, PTR(void) displaylist, u32 event_type) {
    std::lock_guard lock{ extension_state.dl_events.dl_event_mutex };

    assert(
        event_type == OS_EX_DISPLAYLIST_EVENT_SUBMITTED || 
        event_type == OS_EX_DISPLAYLIST_EVENT_PARSED || 
        event_type == OS_EX_DISPLAYLIST_EVENT_COMPLETED);
    
    extension_state.dl_events.pending_events.emplace_back(
        DLEvent{ mq, mesg, displaylist, event_type }
    );
    hh_dl_log("queue", (unsigned)displaylist, event_type, (unsigned)mq, (unsigned)mesg);
}

void dispatch_displaylist_events(PTR(void) displaylist, u32 event_type) {
    std::lock_guard lock{ extension_state.dl_events.dl_event_mutex };

    // Check every pending DL event to see if they match this displaylist and event type.
    for (auto iter = extension_state.dl_events.pending_events.begin(); iter != extension_state.dl_events.pending_events.end(); ) {
        if (iter->displaylist == displaylist && iter->event_type == event_type) {
            // Send the provided message to the corresponding message queue for this event, then remove this event from the queue.
            hh_dl_log("hit", (unsigned)iter->displaylist, (unsigned)iter->event_type, (unsigned)iter->mq, (unsigned)iter->mesg);
            ultramodern::enqueue_external_message_src(iter->mq, iter->mesg, false, ultramodern::EventMessageSource::Sp);
            iter = extension_state.dl_events.pending_events.erase(iter);
        }
        else {
            ++iter;
        }
    }
}

void ultramodern::extensions::on_displaylist_submitted(PTR(void) displaylist) {
    hh_dl_log("cb-submitted", (unsigned)displaylist, 0, 0, 0);
    dispatch_displaylist_events(displaylist, OS_EX_DISPLAYLIST_EVENT_SUBMITTED);
}

void ultramodern::extensions::on_displaylist_parsed(PTR(void) displaylist) {
    hh_dl_log("cb-parsed", (unsigned)displaylist, 0, 0, 0);
    dispatch_displaylist_events(displaylist, OS_EX_DISPLAYLIST_EVENT_PARSED);
}

void ultramodern::extensions::on_displaylist_completed(PTR(void) displaylist) {
    hh_dl_log("cb-completed", (unsigned)displaylist, 0, 0, 0);
    dispatch_displaylist_events(displaylist, OS_EX_DISPLAYLIST_EVENT_COMPLETED);
}
