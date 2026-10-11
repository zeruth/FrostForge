#include "framescript/EventCallbacks.hpp"

namespace {

// The table's extent (0x1430a20f0 up to its guard at 0x1430a5488). Its first 0xf8 bytes hold the
// containers FUN_14126ed10 also makes; the signaller slots follow.
constexpr uint32_t TABLE_SIZE = 0x3398;

}

// ref: FUN_141273d80
EVENTSIGNAL* EventCallbacks_Table() {
    static EVENTSIGNAL s_table[TABLE_SIZE / sizeof(EVENTSIGNAL)] = {};
    return s_table;
}

EVENTSIGNAL EventCallbacks_Get(uint32_t offset) {
    return EventCallbacks_Table()[offset / sizeof(EVENTSIGNAL)];
}

void EventCallbacks_Set(uint32_t offset, EVENTSIGNAL signal) {
    EventCallbacks_Table()[offset / sizeof(EVENTSIGNAL)] = signal;
}

// ref: FUN_141174040
void UiSignalCVarUpdate(const char* name, const char* value) {
    const char* args[2] = {name, value};
    EventCallbacks_Fire(0xec0, 0x32b2c96789f77675ull, args);
}
