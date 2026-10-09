#ifndef TACT_TASK_SCHEDULER_HPP
#define TACT_TASK_SCHEDULER_HPP

// pkg/tact/3.6.39/src/task_scheduler.cpp: the thread pool tact runs its asynchronous work on.

#include "blz/ObjectPool.hpp"
#include "os/Os.hpp"
#include "tact/Common.hpp"
#include <cstddef>
#include <windows.h>

namespace tact {

typedef void (*TASKFUNCTION)(void* argument);

// The scheduler interface (vtable 0x14273d2d0): Post queues a function for a worker.
class TaskSchedulerBase {
    public:
    virtual ~TaskSchedulerBase() = default;
    virtual void Post(TASKFUNCTION function, void* argument, int32_t priority) = 0;
};

// The built-in pool (0xe0 bytes, vtable 0x1428e6928).
class TaskScheduler : public TaskSchedulerBase {
    public:
    struct TASK {
        TASKFUNCTION m_function;
        void* m_argument;
        TASK* m_next;  // queue links (0x10)
        TASK* m_prev;
    };

    CONDITION_VARIABLE m_wake;      // 0x08
    SRWLOCK m_queueLock;            // 0x10
    Array<OSTHREAD> m_threads;      // 0x18
    blz::object_pool m_pool;        // 0x30
    SRWLOCK m_poolLock;             // 0xc8
    TASK* m_queue[2];               // 0xd0: list head (next, prev), linked through TASK::m_next

    TaskScheduler();
    ~TaskScheduler() override;
    void Post(TASKFUNCTION function, void* argument, int32_t priority) override;
    bool Start(uint32_t threads, uint64_t affinity, int32_t priority);

    static void Worker(void* argument);
    void FreeTask(TASK* task);

    TASK* QueueHead() {
        return reinterpret_cast<TASK*>(reinterpret_cast<uint8_t*>(this->m_queue) - offsetof(TASK, m_next));
    }
};

// A scheduler the client supplies as functions (0x28 bytes, vtable 0x1428e6918): Post forwards
// to post(context, function, argument, priority); destruction calls release(releaseContext).
typedef void (*POSTFUNCTION)(void* context, TASKFUNCTION function, void* argument, int32_t priority);

class ClientTaskScheduler : public TaskSchedulerBase {
    public:
    POSTFUNCTION m_post;            // 0x08
    void* m_context;                // 0x10
    void (*m_release)(void* context);  // 0x18
    void* m_releaseContext;         // 0x20

    ClientTaskScheduler(POSTFUNCTION post, void* context, void (*release)(void*), void* releaseContext);
    ~ClientTaskScheduler() override;
    void Post(TASKFUNCTION function, void* argument, int32_t priority) override;
};

}

#endif
