#include "os/Timer.hpp"
#include "os/Os.hpp"
#include "storm/thread/SCritSect.hpp"
#include <windows.h>
#include <timeapi.h>
#include <cstdlib>
#include <cstddef>

// The engine's clock (0x142cfb9f8): milliseconds from GetTickCount or QueryPerformanceCounter,
// scaled and offset so the speed can change without a jump, and never going backwards.
struct OSTIMER {
    int32_t m_method;               // 0x00: 1 GetTickCount, 2 QueryPerformanceCounter
    int32_t m_testError;            // 0x04: why the test fell back
    int64_t m_frequency;            // 0x08
    double m_scale;                 // 0x10: ticks to milliseconds
    double m_offset;                // 0x18
    SCritSect m_lock;               // 0x20
    int32_t m_last;                 // 0x48: the last time handed out
    int32_t m_4c;                   // 0x4c
    int32_t m_50;                   // 0x50: 1
    float m_54;                     // 0x54: 125.0
};
static_assert(offsetof(OSTIMER, m_lock) == 0x20, "OSTIMER layout");
static_assert(offsetof(OSTIMER, m_last) == 0x48, "OSTIMER layout");

namespace {

OSTIMER s_timer = { 1, 0, 0, 1.0, 0.0, {}, 0, 0, 1, 125.0f };

uint64_t ReadTicks(int32_t method) {
    if (method == 1 || method != 2) {
        return GetTickCount();
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

void SleepMs(uint32_t ms) {
    OsSleepNanoseconds(int64_t(ms) * 1000000);  // FUN_141238570
}

}

// ref: FUN_141237770
OSTIMER* OsTimerGet() {
    static bool s_initialized = [] {
        s_timer.m_lock.Initialize();  // FUN_1402637c0
        s_timer.m_frequency = 1000;
        return true;
    }();
    (void)s_initialized;
    return &s_timer;
}

// Chooses the clock: 0 the best that tests sound, 1 GetTickCount, 2 QueryPerformanceCounter
// (after a test across every core), 3 QueryPerformanceCounter untested.
// ref: FUN_141237800
void OsTimerSetMethod(OSTIMER* timer, int32_t method) {
    timer->m_lock.Enter();
    timer->m_method = -1;
    LARGE_INTEGER frequency;
    BOOL haveFrequency = QueryPerformanceFrequency(&frequency);
    timer->m_frequency = frequency.QuadPart;
    int32_t chosen;
    if (method == 3) {
        if (!haveFrequency || timer->m_frequency == 0) {
            chosen = 1;
        } else {
            timer->m_lock.Enter();
            timer->m_method = 2;
            goto counter;
        }
    } else {
        int32_t best;
        if (!haveFrequency) {
            best = 1;
            timer->m_testError = 1;
        } else if (timer->m_frequency == 0) {
            timer->m_testError = 2;
            best = 1;
        } else {
            HANDLE process = GetCurrentProcess();
            HANDLE thread = GetCurrentThread();
            DWORD priorityClass = GetPriorityClass(process);
            int32_t priority = GetThreadPriority(thread);
            SetPriorityClass(process, REALTIME_PRIORITY_CLASS);
            SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
            SleepMs(0);
            timer->m_testError = 0;
            DWORD start = GetTickCount();
            while (GetTickCount() == start) {
            }
            LARGE_INTEGER previous;
            QueryPerformanceCounter(&previous);
            uint32_t cores = OsGetCpuInfo()->m_logical;
            if (cores > 1) {
                DWORD_PTR affinity;
                DWORD_PTR system;
                GetProcessAffinityMask(process, &affinity, &system);
                for (uint32_t i = 0; i < 0x200; i++) {
                    SetThreadAffinityMask(thread, DWORD_PTR(1) << ((i % cores) & 0x3f));
                    SleepMs(0);
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    if (now.QuadPart <= previous.QuadPart) {
                        timer->m_testError = 4;
                        break;
                    }
                    previous = now;
                }
                SetThreadAffinityMask(thread, affinity);
                SleepMs(0);
            }
            SetPriorityClass(process, priorityClass);
            SetThreadPriority(thread, priority);
            SleepMs(0);
            best = 2 - (timer->m_testError != 0 ? 1 : 0);
        }
        chosen = best;
        if (method != 0 && best != method) {
            chosen = method;
            if (method == 2 && best == 1) {
                timer->m_testError = 5;
            }
        }
    }
    timer->m_lock.Enter();
    timer->m_method = chosen;
    if (chosen == 1 || chosen != 2) {
        timer->m_offset = 0.0;
        timer->m_scale = 1.0;
        timer->m_last = static_cast<int32_t>(static_cast<int64_t>(static_cast<double>(int64_t(ReadTicks(chosen))) * timer->m_scale));
        timer->m_lock.Leave();
        timer->m_lock.Leave();
        return;
    }
counter:
    timer->m_offset = 0.0;
    timer->m_scale = 1000.0 / static_cast<double>(timer->m_frequency);  // DAT_14262f260
    {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        timer->m_last = static_cast<int32_t>(static_cast<int64_t>(static_cast<double>(counter.QuadPart) * timer->m_scale));
    }
    timer->m_lock.Leave();
    timer->m_lock.Leave();
}

// The time in milliseconds, never less than the last time handed out.
// ref: FUN_141237ae0
uint32_t OsTimerGetMs(OSTIMER* timer) {
    uint64_t ticks = ReadTicks(timer->m_method);
    uint64_t ms = static_cast<uint64_t>(static_cast<double>(static_cast<int64_t>(ticks)) * timer->m_scale + timer->m_offset);
    int32_t now = static_cast<int32_t>(ms);
    if (now - timer->m_last < 0) {
        return static_cast<uint32_t>(timer->m_last);
    }
    timer->m_last = now;
    return static_cast<uint32_t>(ms);
}

// As OsTimerGetMs, reading the scale under the lock and leaving the last time alone.
// ref: FUN_141237b40
uint32_t OsTimerPeekMs(OSTIMER* timer) {
    timer->m_lock.Enter();
    int32_t method = timer->m_method;
    double scale = timer->m_scale;
    double offset = timer->m_offset;
    timer->m_lock.Leave();
    uint64_t ticks = ReadTicks(method);
    uint64_t ms = static_cast<uint64_t>(static_cast<double>(static_cast<int64_t>(ticks)) * scale + offset);
    if (static_cast<int32_t>(ms) - timer->m_last < 0) {
        return static_cast<uint32_t>(timer->m_last);
    }
    return static_cast<uint32_t>(ms);
}

// ref: FUN_141237bf0
uint32_t OsGetAsyncTimeMs() {
    return OsTimerGetMs(OsTimerGet());
}

// ref: FUN_141237c10
uint32_t OsPeekAsyncTimeMs() {
    return OsTimerPeekMs(OsTimerGet());
}

// ref: FUN_141237c30
double OsTimerGetResolution() {
    OSTIMER* timer = OsTimerGet();
    int64_t frequency = (timer->m_method == 1 || timer->m_method != 2) ? 1000 : timer->m_frequency;
    return static_cast<double>(frequency) * timer->m_scale * 0.001;  // DAT_142724238
}

// ref: FUN_141237c70
int32_t OsTimerGetMethod() {
    return OsTimerGet()->m_method;
}

// ref: FUN_141237c80
const char* OsTimerMethodName(int32_t method) {
    if (method == -1) {
        return "[Not Set]";
    }
    if (method == 0) {
        return "[Best Available]";
    }
    if (method == 1) {
        return "GetTickCount";
    }
    if (method == 2) {
        return "QueryPerformanceCounter";
    }
    return method == 3 ? "QueryPerformanceCounter (No test)" : "[Unknown]";
}

// Changes the clock's speed without a jump in the time it reads.
// ref: FUN_141237cd0
void OsTimerSetSpeed(float speed) {
    OSTIMER* timer = OsTimerGet();
    uint64_t ticks = ReadTicks(timer->m_method);
    double scale = timer->m_scale;
    double scaled = static_cast<double>(static_cast<int64_t>(ticks)) * scale;
    double offset = timer->m_offset;
    timer->m_lock.Enter();
    timer->m_offset = (scaled + offset) - scaled * static_cast<double>(speed);
    timer->m_scale = static_cast<double>(speed) * scale;
    timer->m_lock.Leave();
}

// ref: FUN_141237d90
void OsTimerInitialize(int32_t method) {
    timeBeginPeriod(1);
    OsTimerSetMethod(OsTimerGet(), method);
}

// ref: FUN_141237dc0
int64_t OsTimerGetFrequency() {
    OSTIMER* timer = OsTimerGet();
    if (timer->m_method != 1 && timer->m_method == 2) {
        return timer->m_frequency;
    }
    return 1000;
}

// ref: FUN_141237df0
uint64_t OsTimerGetTicks() {
    return ReadTicks(OsTimerGet()->m_method);
}

// ref: FUN_141237e30
uint32_t OsGetTickCount() {
    return GetTickCount();
}
