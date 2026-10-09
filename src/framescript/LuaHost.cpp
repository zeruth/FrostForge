// What the client's Lua core (vendor/lua-5.1.4, see BLIZZARD.md) asks of the engine: the
// clocks it profiles and times scripts with, and which script timeout applies.

#include "os/Timer.hpp"
#include "util/Unimplemented.hpp"

extern "C" {

// Profile ticks per second (OsTimerGetFrequency, FUN_141237dc0).
long long luai_timerfrequency(void) {
    return OsTimerGetFrequency();
}

// Profile ticks now (OsTimerGetTicks, FUN_141237df0).
long long luai_timerticks(void) {
    return static_cast<long long>(OsTimerGetTicks());
}

// The clock "script ran too long" measures (FUN_141237e30).
unsigned int luai_tickcount(void) {
    return OsGetTickCount();
}

// Whether the first of the two script timeouts applies: a flag on the UI root (CSimpleTop
// +0xf61). The timeouts are only armed once FrameScript is up, and the UI root is not ported
// yet, so this is reached only by a tainted script running past the check limit.
// ref: FUN_14192fa50
int luai_usefirsttimeout(void) {
    FROST_UNIMPLEMENTED();
    return 0;
}

}
