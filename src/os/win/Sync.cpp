#include "os/Sync.hpp"

// ref: FUN_140262450
bool OsMutexInit(OSMUTEX* mutex) {
    InitializeSRWLock(&mutex->m_lock);
    return true;
}

// ref: FUN_140262440
void OsMutexLock(OSMUTEX* mutex) {
    AcquireSRWLockExclusive(&mutex->m_lock);
}

// ref: FUN_140262470
void OsMutexUnlock(OSMUTEX* mutex) {
    ReleaseSRWLockExclusive(&mutex->m_lock);
}

// ref: FUN_14025cc90
int32_t OsLazyCritSectEnter(OSLAZYCRITSECT* section) {
    if (section->m_state == 2) {
        if (!TryEnterCriticalSection(&section->m_section)) {
            EnterCriticalSection(&section->m_section);
            return 3;
        }
        return 2;
    }
    long state;
    while (true) {
        state = InterlockedCompareExchange(&section->m_state, 1, 0);
        if (state == 0) {
            break;
        }
        if (state != 1) {
            break;
        }
    }
    if (state != 2) {
        InitializeCriticalSection(&section->m_section);
        InterlockedExchange(&section->m_state, 2);
    }
    EnterCriticalSection(&section->m_section);
    return 1;
}

// ref: FUN_14025cd30
void OsLazyCritSectLeave(OSLAZYCRITSECT* section) {
    LeaveCriticalSection(&section->m_section);
}
