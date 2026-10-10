#include "framescript/EventCallbacks.hpp"

namespace {

// The table's extent (0x1430a20f0 up to its guard at 0x1430a5488). Its first 0xf8 bytes hold the
// containers FUN_14126ed10 also makes; the signaller slots follow.
constexpr uint32_t TABLE_SIZE = 0x3398;

EVENTSIGNAL* Table() {
    // ref: FUN_141273d80
    static EVENTSIGNAL s_table[TABLE_SIZE / sizeof(EVENTSIGNAL)] = {};
    return s_table;
}

}

EVENTSIGNAL EventCallbacks_Get(uint32_t offset) {
    return Table()[offset / sizeof(EVENTSIGNAL)];
}

void EventCallbacks_Set(uint32_t offset, EVENTSIGNAL signal) {
    Table()[offset / sizeof(EVENTSIGNAL)] = signal;
}
