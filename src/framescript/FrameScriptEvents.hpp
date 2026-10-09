#ifndef FRAMESCRIPT_FRAME_SCRIPT_EVENTS_HPP
#define FRAMESCRIPT_FRAME_SCRIPT_EVENTS_HPP

// FrameScriptEvents: which objects listen for which events. An event is named by its id, the
// Murmur3 hash of its upper-cased name (LanguageBinding::EventID). Each event keeps a list of
// listeners, and one more list hears every event. While a list is being dispatched,
// registrations against it are deferred and applied when the dispatch ends.

#include <cstdint>

#include "blz/Vector.hpp"

class FrameScript_Object;

namespace FrameScriptEvents {

extern const char s_listenerTag[];
extern const char s_deferredTag[];
extern const char s_objectTag[];

// A listener and the units it filters on (0 for none).
struct EventListener {
    FrameScript_Object* m_object;
    uint16_t m_units[4];
};
static_assert(sizeof(EventListener) == 0x10, "EventListener is 0x10 bytes");

// A registration made while the list was dispatching: 0 add, 1 remove.
struct DeferredRegistration {
    FrameScript_Object* m_object;
    uint64_t m_units;
    int32_t m_op;
};
static_assert(sizeof(DeferredRegistration) == 0x18, "DeferredRegistration is 0x18 bytes");

typedef blz::vector<DeferredRegistration, s_deferredTag> DeferredList;

class EventListenerList {
    public:
    blz::vector<EventListener, s_listenerTag> m_listeners;  // 0x00
    DeferredList* m_deferred = nullptr;                      // 0x18 set while dispatching

    // 0 when the listener was added or its units changed, 1 when nothing changed.
    int32_t Add(FrameScript_Object* object, uint64_t units);
    // 0 when removed, 1 when it was not there.
    int32_t Remove(FrameScript_Object* object);
    // Queues an operation while dispatching; true when the queue changed.
    bool Defer(FrameScript_Object* object, uint64_t units, int32_t op);
    // The objects that will hear the next dispatch, deferred additions included.
    blz::vector<FrameScript_Object*, s_objectTag>* Collect(blz::vector<FrameScript_Object*, s_objectTag>* out);
};
static_assert(sizeof(EventListenerList) == 0x20, "EventListenerList is 0x20 bytes");

// Defers registrations against a list for its lifetime (the outermost one wins), then
// applies them.
class DispatchScope {
    public:
    EventListenerList* m_list;
    DeferredList m_deferred;

    explicit DispatchScope(EventListenerList* list);
    ~DispatchScope();
};

// The event id of a name.
uint64_t GetEventID(const char* name, uint64_t length);
// Whether the client knows the event.
bool IsKnownEvent(uint64_t id);

// 0 registered (or units updated), 1 already registered, 2 unknown event.
int32_t RegisterEvent(FrameScript_Object* object, uint64_t id);
// 0 unregistered, 1 not registered, 2 unknown event.
int32_t UnregisterEvent(FrameScript_Object* object, uint64_t id);
void RegisterAllEvents(FrameScript_Object* object);
// 0 removed from at least one list, 1 from none.
int32_t UnregisterAllEvents(FrameScript_Object* object);
bool IsEventRegistered(FrameScript_Object* object, uint64_t id);
// The units an object filters an event on; false if it is not registered.
bool GetEventUnits(FrameScript_Object* object, uint64_t id, uint64_t* units);
// The listeners of an event: those of every event first.
blz::vector<FrameScript_Object*, s_objectTag>* GetListeners(blz::vector<FrameScript_Object*, s_objectTag>* out, uint64_t id);
// The list of an event, created when missing.
EventListenerList* GetEventList(uint64_t id);
// The list every event goes to.
EventListenerList* GetAllEventsList();
// Drops every registration.
void Clear();

}

#endif
