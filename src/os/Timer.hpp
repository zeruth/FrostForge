#ifndef OS_TIMER_HPP
#define OS_TIMER_HPP

// The engine clock (os/win/Timer.cpp): milliseconds that never run backwards, from
// GetTickCount or a tested QueryPerformanceCounter (the timingMethod CVar picks).

#include <cstdint>

struct OSTIMER;

OSTIMER* OsTimerGet();
void OsTimerSetMethod(OSTIMER* timer, int32_t method);
uint32_t OsTimerGetMs(OSTIMER* timer);
uint32_t OsTimerPeekMs(OSTIMER* timer);
uint32_t OsGetAsyncTimeMs();
uint32_t OsPeekAsyncTimeMs();
double OsTimerGetResolution();
int32_t OsTimerGetMethod();
const char* OsTimerMethodName(int32_t method);
void OsTimerSetSpeed(float speed);
// timeBeginPeriod(1), then the method (the timingMethod CVar's value).
void OsTimerInitialize(int32_t method);
int64_t OsTimerGetFrequency();
uint64_t OsTimerGetTicks();
// The raw GetTickCount milliseconds, whatever the timing method.
uint32_t OsGetTickCount();

#endif
