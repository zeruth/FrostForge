#ifndef FRAMESCRIPT_EVENT_CALLBACKS_HPP
#define FRAMESCRIPT_EVENT_CALLBACKS_HPP

// The client's event signallers (the table FUN_141273d80 makes at 0x1430a20f0): one slot per
// script event. A script API registration installs the signaller of each event it owns
// (FUN_141163f90 and its neighbours, one setter per slot); the code an event comes from calls
// the slot, when one is installed, with the event's id and its arguments.

#include <cstdint>

typedef void (*EVENTSIGNAL)(uint64_t id, void* args);

// The slot at a byte offset into the table.
EVENTSIGNAL EventCallbacks_Get(uint32_t offset);
void EventCallbacks_Set(uint32_t offset, EVENTSIGNAL signal);

// Calls a slot's signaller, when one is installed.
inline void EventCallbacks_Fire(uint32_t offset, uint64_t id, void* args) {
    if (EVENTSIGNAL signal = EventCallbacks_Get(offset)) {
        signal(id, args);
    }
}

// CVAR_UPDATE to the scripts: a script CVar's name and its new value (FUN_141174040).
void UiSignalCVarUpdate(const char* name, const char* value);

#endif
