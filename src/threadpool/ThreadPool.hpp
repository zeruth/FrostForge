#ifndef THREADPOOL_THREAD_POOL_HPP
#define THREADPOOL_THREAD_POOL_HPP

// The engine's thread pool (Engine/Source/ThreadPool/ThreadPool.cpp): three priorities of
// workers, each with a ring of queued tasks, fed from one static pool of task nodes. Tasks
// are blz::functions and may belong to a group, which can be waited on.

#include "blz/Function.hpp"
#include <cstdint>
#include <windows.h>

// A set of tasks that can be waited on together (0x30 bytes). Released groups go to a free
// ring and are handed out again once their tasks are done.
struct THREADPOOLGROUP {
    volatile LONG m_count;          // 0x00: tasks still to run
    SRWLOCK m_lock;                 // 0x08
    CONDITION_VARIABLE m_done;      // 0x10
    uint8_t m_idle;                 // 0x18: the count reached zero
    int32_t m_priority;             // 0x1c: the queue its tasks go to
    THREADPOOLGROUP** m_next;       // 0x20: the free ring (links point at links)
    THREADPOOLGROUP** m_prev;       // 0x28
};
static_assert(sizeof(THREADPOOLGROUP) == 0x30, "THREADPOOLGROUP is 0x30 bytes");

// Starts the workers: as many per priority as there are logical processors beyond
// `reserved`, at least two and at most six (FUN_142359e10).
void ThreadPoolInitialize(int32_t reserved);

// Stops and joins the workers and frees the released groups (FUN_14235a350).
void ThreadPoolShutdown();

// Whether the caller is the thread that started the pool (FUN_14235a050).
bool ThreadPoolIsMainThread();

// The number of workers, 0 before the pool is up (FUN_142359dd0).
uint32_t ThreadPoolThreadCount();

// A group for tasks of `priority` (FUN_14235a070), and its release (FUN_142359d10).
THREADPOOLGROUP* ThreadPoolGroupCreate(int32_t priority);
void ThreadPoolGroupRelease(THREADPOOLGROUP* group);

// Sets how many tasks the group waits for (FUN_14235a330), or adds to it (FUN_142359e00).
void ThreadPoolGroupSetCount(THREADPOOLGROUP* group, int32_t count);
void ThreadPoolGroupAddCount(THREADPOOLGROUP* group, int32_t count);

// Runs the group's queued tasks on this thread while they are at the head of the queue,
// then waits for the rest (FUN_14235a5e0).
void ThreadPoolGroupWait(THREADPOOLGROUP* group);

// Queues `count` tasks at `priority`; the functions are moved out of `tasks`
// (FUN_142359cf0 over FUN_1423599f0).
void ThreadPoolSubmit(uint64_t count, blz::function_storage* tasks, int32_t priority, THREADPOOLGROUP* group);

// Waits briefly for the priority's queue to drain and its workers to go idle (FUN_14235a710).
void ThreadPoolFlush(int32_t priority);

#endif
