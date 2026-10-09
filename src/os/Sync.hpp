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

#endif
