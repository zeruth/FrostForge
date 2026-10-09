#include "tact/TaskScheduler.hpp"
#include <cstddef>

namespace tact {

namespace {

const char s_schedulerFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\task_scheduler.cpp";

void Unlink(TaskScheduler::TASK* task) {
    task->m_prev->m_next = task->m_next;
    task->m_next->m_prev = task->m_prev;
    task->m_prev = task;
    task->m_next = task;
}

}

// ref: FUN_1420ddb50
TaskScheduler::TaskScheduler() {
    this->m_threads.Init(GetAllocator());
    this->m_pool.Init(GetAllocator(), sizeof(TASK), 8, 0x20, 1);
    InitializeSRWLock(&this->m_poolLock);
    TASK* head = this->QueueHead();
    this->m_queue[0] = head;
    this->m_queue[1] = head;
    InitializeConditionVariable(&this->m_wake);
    InitializeSRWLock(&this->m_queueLock);
}

// Stops the workers (one empty task each) and waits for them.
// ref: FUN_1420ddc10
TaskScheduler::~TaskScheduler() {
    AcquireSRWLockExclusive(&this->m_queueLock);
    for (uint32_t i = 0; i < this->m_threads.m_count; i++) {
        AcquireSRWLockExclusive(&this->m_poolLock);
        auto task = static_cast<TASK*>(this->m_pool.Allocate());
        ReleaseSRWLockExclusive(&this->m_poolLock);
        task->m_function = nullptr;
        task->m_argument = nullptr;
        TASK* head = this->QueueHead();
        task->m_prev = head->m_prev;
        task->m_next = head;
        head->m_prev = task;
        task->m_prev->m_next = task;
    }
    ReleaseSRWLockExclusive(&this->m_queueLock);
    WakeAllConditionVariable(&this->m_wake);
    for (uint32_t i = 0; i < this->m_threads.m_count; i++) {
        OsThreadJoin(&this->m_threads.m_data[i]);
    }
    for (uint32_t i = 0; i < this->m_threads.m_count; i++) {
        OsThreadWaitStarted(&this->m_threads.m_data[i]);
    }
    this->m_threads.m_count = 0;

    TASK* head = this->QueueHead();
    for (TASK* task = head->m_next; task != head;) {
        TASK* next = task->m_next;
        task->m_next = task;
        task->m_prev = task;
        task = next;
    }
    head->m_prev = head;
    head->m_next = head;
    this->m_pool.Destroy();
    this->m_threads.Free();
}

// ref: FUN_1420ddea0
void TaskScheduler::Post(TASKFUNCTION function, void* argument, int32_t priority) {
    AcquireSRWLockExclusive(&this->m_poolLock);
    auto task = static_cast<TASK*>(this->m_pool.Allocate());
    ReleaseSRWLockExclusive(&this->m_poolLock);
    if (task) {
        task->m_next = task;
        task->m_prev = task;
    }
    task->m_function = function;
    task->m_argument = argument;

    AcquireSRWLockExclusive(&this->m_queueLock);
    Unlink(task);
    TASK* head = this->QueueHead();
    if (priority == 1) {
        TASK* first = head->m_next;
        task->m_prev = first->m_prev;
        task->m_next = first;
        first->m_prev = task;
    } else {
        task->m_prev = head->m_prev;
        task->m_next = head;
        head->m_prev = task;
    }
    task->m_prev->m_next = task;
    ReleaseSRWLockExclusive(&this->m_queueLock);
    WakeAllConditionVariable(&this->m_wake);
}

// ref: FUN_1420ddfb0
void TaskScheduler::FreeTask(TASK* task) {
    Unlink(task);
    AcquireSRWLockExclusive(&this->m_poolLock);
    this->m_pool.Free(task);
    ReleaseSRWLockExclusive(&this->m_poolLock);
}

// ref: FUN_1420de180
void TaskScheduler::Worker(void* argument) {
    auto scheduler = static_cast<TaskScheduler*>(argument);
    OsThreadSetName("TACT Task Thread");
    TASK* head = scheduler->QueueHead();
    for (;;) {
        AcquireSRWLockExclusive(&scheduler->m_queueLock);
        while (head->m_next == head) {
            SleepConditionVariableSRW(&scheduler->m_wake, &scheduler->m_queueLock, INFINITE, 0);
        }
        TASK* task = head->m_next;
        Unlink(task);
        ReleaseSRWLockExclusive(&scheduler->m_queueLock);
        if (!task->m_function) {
            scheduler->FreeTask(task);
            return;
        }
        task->m_function(task->m_argument);
        scheduler->FreeTask(task);
    }
}

// Starts `threads` workers (0: one per logical processor) with an affinity and priority.
// ref: FUN_1420de020
bool TaskScheduler::Start(uint32_t threads, uint64_t affinity, int32_t priority) {
    if (threads == 0) {
        threads = OsGetCpuInfo()->m_logical;
    }
    this->m_threads.Reserve(threads);
    for (uint32_t i = 0; i < threads; i++) {
        if (this->m_threads.m_count == this->m_threads.m_capacity) {
            this->m_threads.Reserve(this->m_threads.m_count + 1);
        }
        OSTHREAD* thread = &this->m_threads.m_data[this->m_threads.m_count];
        thread->m_handle = nullptr;
        thread->m_function = nullptr;
        thread->m_argument = nullptr;
        this->m_threads.m_count++;
        uint32_t error = OsThreadCreate(thread, &TaskScheduler::Worker, this, 0);
        if (error != 0) {
            char message[0x100];
            OsErrorString(message, sizeof(message), error);
            blz::log(g_logCategory, 4, s_schedulerFile, 0x3F, "Failed to create thread pool thread. Got error (%u): %s",
                error, message);
            return false;
        }
        OsThreadSetAffinity(thread, affinity);
        OsThreadSetPriority(thread, priority);
    }
    return true;
}

}

namespace tact {

// ref: FUN_1420ddbe0
ClientTaskScheduler::ClientTaskScheduler(POSTFUNCTION post, void* context, void (*release)(void*), void* releaseContext)
    : m_post(post), m_context(context), m_release(release), m_releaseContext(releaseContext) {
}

// ref: FUN_1420dde40
ClientTaskScheduler::~ClientTaskScheduler() {
    if (this->m_release) {
        this->m_release(this->m_releaseContext);
    }
}

// ref: FUN_1420ddfa0
void ClientTaskScheduler::Post(TASKFUNCTION function, void* argument, int32_t priority) {
    this->m_post(this->m_context, function, argument, priority);
}

}
