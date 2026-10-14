#ifndef OS_SYNC_HPP
#define OS_SYNC_HPP

// The engine's two small locks: a slim reader/writer lock taken exclusively, and a critical
// section that statics can use before anything initialized them (the first entrant builds it).

#include <windows.h>

#include <cstdint>

struct OSMUTEX {
    SRWLOCK m_lock;
};

bool OsMutexInit(OSMUTEX* mutex);
void OsMutexLock(OSMUTEX* mutex);
void OsMutexUnlock(OSMUTEX* mutex);

// state: 0 untouched, 1 being built, 2 ready.
struct OSLAZYCRITSECT {
    volatile long m_state;
    CRITICAL_SECTION m_section;
};

// 1 when this call built the section, 2 when it was free, 3 when it had to wait.
int32_t OsLazyCritSectEnter(OSLAZYCRITSECT* section);
void OsLazyCritSectLeave(OSLAZYCRITSECT* section);

// A once-only call: the lock, then whether it has run.
struct OSONCE {
    OSMUTEX m_mutex;    // 0x00
    uint8_t m_done;     // 0x08
};

// Runs `function` (a blz::function<void()>: its invoker, manager and storage) the first time
// only, under the lock.
// ref: FUN_1423631d0
void OsCallOnce(OSONCE* once, const void* function);

#endif
