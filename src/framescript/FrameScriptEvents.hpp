#ifndef FRAMESCRIPT_FRAME_SCRIPT_EVENTS_HPP
#define FRAMESCRIPT_FRAME_SCRIPT_EVENTS_HPP

// FrameScriptEvents: which objects listen for which events. An event is named by its id, the
// Murmur3 hash of its upper-cased name (LanguageBinding::EventID). Each event keeps a list of
// listeners, and one more list hears every event. While a list is being dispatched,
// registrations against it are deferred and applied when the dispatch ends.

#include <cstdint>

#include "blz/Vector.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"

extern "C" {
#include "lblizzard.h"
#include "lua.h"
}

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

// The list of an event, or null when nothing ever registered for it.
EventListenerList* FindEventList(uint64_t id);
// Whether a listener hears a dispatch: not removed during it, and with an OnEvent handler
// (FUN_140648490).
bool ShouldDeliver(EventListenerList* list, EventListener* listener);

// A string by pointer and length, as the event code passes its name (null for nil).
struct EVENTSTRING {
    const char* m_data;
    uint64_t m_length;
};

// Pushes the error handler, the handler at `ref`, the object and the event's name, returning
// the error handler's index (FUN_140648330); calls it with `count` arguments of its own and pops
// the error handler (FUN_140647940).
int32_t PushHandler(lua_State* L, const EVENTSTRING* name, int32_t ref, FrameScript_Object* object);
void CallHandler(lua_State* L, int32_t count, int32_t errorHandler);
// A string pushed by pointer and length, nil when there is none (FUN_14065f750).
void PushEventString(lua_State* L, const EVENTSTRING* text);

// OnEvent(self, event, ...) for every listener of a list that should hear it, as each generated
// event's fire function has it inlined (FUN_140b1e390 for GX_RESTARTED): the list deferred, each
// listener's OnEvent bindings, each run under its own taint with the event's arguments pushed
// by `push` (which returns how many it pushed), timed while profiling.
template <class Push>
void FireList(EventListenerList* list, const char* name, uint64_t length, uint64_t id, Push push) {
    DispatchScope scope(list);
    EventListener* listener = list->m_listeners.m_data;
    EventListener* last = listener + list->m_listeners.m_size;
    for (; listener != last; listener++) {
        if (!ShouldDeliver(list, listener)) {
            continue;
        }
        lua_State* L = FrameScript_GetContext();
        FrameScript_Object::Script scripts[3];
        listener->m_object->GetScripts(scripts, 0x1a);
        PROFILEHANDLERSCOPE profile;
        ProfileHandlerEnter(&profile, id, listener->m_object);
        for (FrameScript_Object::Script& script : scripts) {
            if (script.m_ref == 0) {
                continue;
            }
            const char* previous = lua_taint;
            lua_pushjmptaint(L, lua_taint, lua_taintTracking);
            lua_taintTracking++;
            if (lua_taintTracking != 0 && lua_taintSuspended == 0) {
                luaT_settaint(L, script.m_taint);
            }
            EVENTSTRING event = {name, length};
            int32_t errorHandler = PushHandler(L, &event, script.m_ref, listener->m_object);
            int32_t count = push(L);
            CallHandler(L, count, errorHandler);
            if (lua_taintTracking != 0 && lua_taintSuspended == 0) {
                luaT_settaint(L, previous);
            }
            lua_taintTracking--;
            lua_popjmptaint(L);
            if (lua_taintTracking < 1) {
                lua_taintTracking = 0;
                lua_firstTaint = nullptr;
            }
        }
        ProfileHandlerLeave(&profile);
    }
}

// An event to its own listeners, then to those of every event, inside the event's profiling
// scope (each generated dispatcher, e.g. FUN_140b3ceb0 for GX_RESTARTED, with the event's own
// fire function).
typedef void (*EVENTFIRE)(EventListenerList* list, void* args);
inline void Dispatch(uint64_t id, void* args, EVENTFIRE fire) {
    PROFILEEVENTSCOPE scope;
    ProfileEventEnter(&scope, id);
    EventListenerList* list = FindEventList(id);
    EventListenerList* all = GetAllEventsList();
    if (list != nullptr) {
        fire(list, args);
        fire(all, args);
    } else if (all->m_listeners.m_size != 0 || all->m_deferred != nullptr) {
        fire(all, args);
    }
    ProfileEventLeave(&scope);
}

}

#endif
