#ifndef BLZ_SYNC_HPP
#define BLZ_SYNC_HPP

// blz's thin wrappers over Win32 events and semaphores. Creation failures are fatal in the
// reference (they go through the assertion reporter).

#include <cstdint>
#include <windows.h>

namespace blz {

bool sync_event_init(HANDLE* event, bool manualReset, bool initialState);
void sync_event_set(HANDLE* event);
void sync_event_reset(HANDLE* event);
bool sync_semaphore_init(HANDLE* semaphore, int32_t initialCount);
void sync_semaphore_release(HANDLE* semaphore);
// Waits forever on an event or a semaphore.
void sync_wait(HANDLE* handle);
void sync_close(HANDLE* handle);

// blz::shared_mutex (0x20 bytes): an SRW lock guarding a state word and two condition variables.
// Bit 31 of the state is a writer holding or waiting; the low bits count readers. Writers
// wait at the first gate for other writers, then at the second for the readers to drain.
struct shared_mutex {
    SRWLOCK m_lock = SRWLOCK_INIT;                  // 0x00
    CONDITION_VARIABLE m_gate1 = CONDITION_VARIABLE_INIT;  // 0x08
    CONDITION_VARIABLE m_gate2 = CONDITION_VARIABLE_INIT;  // 0x10
    uint32_t m_state = 0;                           // 0x18

    void lock();
    void unlock();
    void lock_shared();
    void unlock_shared();
};
static_assert(sizeof(shared_mutex) == 0x20, "blz::shared_mutex is 0x20 bytes");

}

#endif
