#include "threadpool/ThreadPool.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <cstdlib>
#include <cstring>

namespace {

const char s_threadPoolFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\ThreadPool\\ThreadPool.cpp";

constexpr uint64_t POINTER_MASK = 0x0000ffffffffffffull;
constexpr uint64_t TAG_MASK = 0xffff000000000000ull;
constexpr uint32_t QUEUE_SIZE = 0x19000;
constexpr uint32_t NODE_COUNT = 307200;

// A queued task (0x38 bytes). Node links are tagged: the low 48 bits point at the next node,
// the high 16 count how often the node went back to a free list, against ABA.
struct THREADPOOLNODE {
    uint64_t m_next;                // 0x00
    blz::function_storage m_task;   // 0x08
    THREADPOOLGROUP* m_group;       // 0x28
    int32_t m_priority;             // 0x30
};
static_assert(sizeof(THREADPOOLNODE) == 0x38, "THREADPOOLNODE is 0x38 bytes");

// One priority's ring of tagged node pointers (0xc8040 bytes, DAT_1444f94c0).
struct THREADPOOLQUEUE {
    CONDITION_VARIABLE m_work;      // 0x00: workers wait here for tasks
    CONDITION_VARIABLE m_done;      // 0x08: a task finished
    SRWLOCK m_lock;                 // 0x10
    uint32_t m_head;                // 0x18: where the next task goes
    uint32_t m_tail;                // 0x1c: the next task to take
    uint64_t m_entries[QUEUE_SIZE]; // 0x20
    uint8_t m_pad[0x20];
};
static_assert(sizeof(THREADPOOLQUEUE) == 0xc8040, "THREADPOOLQUEUE is 0xc8040 bytes");

// A worker (0x40 bytes, DAT_144751600).
struct THREADPOOLTHREAD {
    OSTHREAD m_thread;              // 0x00
    int32_t m_priority;             // 0x18
    volatile int32_t m_state;       // 0x1c: 1 created, 2 waiting, 3 running a task, 4 exited
    uint8_t m_pad[0x20];
};
static_assert(sizeof(THREADPOOLTHREAD) == 0x40, "THREADPOOLTHREAD is 0x40 bytes");

THREADPOOLNODE s_nodes[NODE_COUNT];     // DAT_143491480
SRWLOCK s_poolLock = SRWLOCK_INIT;      // DAT_1444f9480
uint8_t s_shutdown;                     // DAT_1444f9488
uint8_t s_initialized;                  // DAT_1444f9489
THREADPOOLQUEUE s_queues[3];            // DAT_1444f94c0
volatile uint64_t s_freeNodes;          // DAT_144751580
volatile uint64_t s_returnedNodes;      // DAT_1447515c0: finished nodes, pushed by any thread
THREADPOOLTHREAD s_threads[18];         // DAT_144751600
uint32_t s_threadsPerPriority[3];       // DAT_1445c14e0, DAT_144689520, DAT_144751560
uint32_t s_threadCount;                 // DAT_144751a80
uint32_t s_mainThread;                  // DAT_144751a88

// The workers' OS priority by pool priority (DAT_142928780).
const int32_t s_osPriorities[3] = { OS_THREAD_PRIORITY_BELOW_NORMAL, OS_THREAD_PRIORITY_NORMAL, OS_THREAD_PRIORITY_ABOVE_NORMAL };

// The free ring of released groups (PTR_LOOP_142d4bad8): a head whose links point at the
// groups' own link fields.
THREADPOOLGROUP** s_groupRing[2] = { reinterpret_cast<THREADPOOLGROUP**>(&s_groupRing),
    reinterpret_cast<THREADPOOLGROUP**>(&s_groupRing) };

THREADPOOLNODE* Untag(uint64_t link) {
    return reinterpret_cast<THREADPOOLNODE*>(static_cast<int64_t>(link << 16) >> 16);
}

THREADPOOLGROUP* GroupFromLink(void* link) {
    return reinterpret_cast<THREADPOOLGROUP*>(static_cast<uint8_t*>(link) - 0x20);
}

// Moves the returned nodes back onto the free list (FUN_14235a180).
void ReclaimReturnedNodes() {
    uint64_t chain = s_returnedNodes;
    for (;;) {
        if (Untag(chain) == nullptr) {
            return;
        }
        uint64_t seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&s_returnedNodes), 0, chain);
        if (seen == chain) {
            break;
        }
        chain = seen;
    }
    uint64_t last = chain;
    while ((Untag(last)->m_next & POINTER_MASK) != 0) {
        last = Untag(last)->m_next;
    }
    uint64_t head = s_freeNodes;
    for (;;) {
        Untag(last)->m_next = head;
        uint64_t seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&s_freeNodes), chain, head);
        if (seen == head) {
            return;
        }
        head = seen;
    }
}

// Runs a node's task, settles its group and returns the node (FUN_14235a210).
void RunNode(uint64_t link) {
    THREADPOOLNODE* node = Untag(link);
    reinterpret_cast<void (*)(void*)>(node->m_task.m_invoke)(node->m_task.m_storage);
    node->m_task.m_manager(node->m_task.m_storage, nullptr, 0);
    node->m_task.m_invoke = nullptr;
    node->m_task.m_manager = blz::function_empty_manager;
    THREADPOOLGROUP* group = node->m_group;
    if (group != nullptr && InterlockedDecrement(&group->m_count) == 0) {
        WakeAllConditionVariable(&group->m_done);
        AcquireSRWLockExclusive(&group->m_lock);
        WakeAllConditionVariable(&group->m_done);
        group->m_idle = 1;
        ReleaseSRWLockExclusive(&group->m_lock);
    }
    uint64_t retagged = (link & POINTER_MASK) | ((link + 0x1000000000000ull) & TAG_MASK);
    uint64_t head = s_returnedNodes;
    for (;;) {
        node->m_next = head;
        uint64_t seen = InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&s_returnedNodes), retagged, head);
        if (seen == head) {
            return;
        }
        head = seen;
    }
}

// Takes `count` nodes off the free list as one chain, reclaiming returned nodes when it runs
// short. The reference interleaves the reclaim with the walk; the result is the same chain.
uint64_t TakeNodes(uint64_t count) {
    for (;;) {
        uint64_t head = s_freeNodes;
        if ((head & POINTER_MASK) == 0) {
            ReclaimReturnedNodes();
            continue;
        }
        uint64_t last = head;
        uint64_t walked = 0;
        while (walked + 1 < count) {
            uint64_t next = Untag(last)->m_next;
            if ((next & POINTER_MASK) == 0) {
                break;
            }
            last = next;
            walked++;
        }
        if (walked + 1 < count) {
            ReclaimReturnedNodes();
            continue;
        }
        uint64_t rest = Untag(last)->m_next;
        if (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&s_freeNodes), rest, head) == head) {
            Untag(last)->m_next &= TAG_MASK;
            return head;
        }
    }
}

// A worker: takes tasks off its priority's queue until the pool shuts down (FUN_14235a480).
void WorkerMain(void* argument) {
    auto self = static_cast<THREADPOOLTHREAD*>(argument);
    OsSetCurrentThreadPriority(s_osPriorities[self->m_priority]);
    THREADPOOLQUEUE* queue = &s_queues[self->m_priority];
    for (;;) {
        AcquireSRWLockExclusive(&queue->m_lock);
        self->m_state = 2;
        while (queue->m_head == queue->m_tail && s_shutdown == 0) {
            SleepConditionVariableSRW(&queue->m_work, &queue->m_lock, INFINITE, 0);
        }
        if (queue->m_head == queue->m_tail) {
            self->m_state = 4;
            ReleaseSRWLockExclusive(&queue->m_lock);
            WakeConditionVariable(&queue->m_done);
            return;
        }
        self->m_state = 3;
        uint32_t tail = queue->m_tail;
        uint64_t link = queue->m_entries[tail];
        queue->m_entries[tail] = link & TAG_MASK;
        queue->m_tail = (tail + 1) % QUEUE_SIZE;
        ReleaseSRWLockExclusive(&queue->m_lock);
        if ((link & POINTER_MASK) != 0) {
            RunNode(link);
            WakeConditionVariable(&queue->m_done);
        }
    }
}

// Waits on a condition variable for `nanoseconds` (FUN_142363b00).
bool WaitFor(CONDITION_VARIABLE* condition, SRWLOCK* lock, int64_t nanoseconds) {
    uint64_t milliseconds = static_cast<uint64_t>(nanoseconds / 1000000);
    DWORD timeout = (milliseconds & 0xffffffffull) == milliseconds ? static_cast<DWORD>(milliseconds) : INFINITE;
    return SleepConditionVariableSRW(condition, lock, timeout, 0) != 0;
}

}

// ref: FUN_142359e10
void ThreadPoolInitialize(int32_t reserved) {
    s_shutdown = 0;
    uint32_t threads = OsGetCpuInfo()->m_logical - reserved;
    if (static_cast<int32_t>(threads) < 3) {
        threads = 2;
    }
    s_threadsPerPriority[0] = threads < 6 ? threads : 6;
    s_threadCount = s_threadsPerPriority[0] * 3;
    s_threadsPerPriority[1] = s_threadsPerPriority[0];
    s_threadsPerPriority[2] = s_threadsPerPriority[0];
    s_mainThread = OsGetCurrentThreadId();
    for (uint32_t i = 0; i < s_threadCount; i++) {
        THREADPOOLTHREAD* thread = &s_threads[i];
        if (i < s_threadsPerPriority[0]) {
            thread->m_priority = 0;
        } else {
            thread->m_priority = 2 - (i < s_threadsPerPriority[0] + s_threadsPerPriority[1] ? 1 : 0);
        }
        thread->m_state = 1;
        // The reference launches through blz::thread (a 0x48-byte launch object, vtable
        // 0x14261bff8, run by FUN_140245ac0); its priority field is 4, "leave as is", so the
        // launch reduces to the thread function and its argument.
        if (thread->m_thread.m_handle != nullptr) {
            abort();
        }
        OsThreadCreate(&thread->m_thread, WorkerMain, thread, 0x100000);
    }
    uint64_t previous = 0;
    for (uint32_t i = 0; i < NODE_COUNT; i++) {
        THREADPOOLNODE* node = &s_nodes[i];
        node->m_next = previous;
        node->m_task.m_manager = blz::function_empty_manager;
        previous = (previous & TAG_MASK) | (reinterpret_cast<uint64_t>(node) & POINTER_MASK);
    }
    s_freeNodes = previous;
    s_initialized = 1;
}

// ref: FUN_14235a350
void ThreadPoolShutdown() {
    if (s_initialized == 0) {
        return;
    }
    for (THREADPOOLQUEUE& queue : s_queues) {
        AcquireSRWLockExclusive(&queue.m_lock);
        s_shutdown = 1;
        ReleaseSRWLockExclusive(&queue.m_lock);
        WakeAllConditionVariable(&queue.m_work);
    }
    for (uint32_t i = 0; i < s_threadCount; i++) {
        OsThreadJoin(&s_threads[i].m_thread);
    }
    AcquireSRWLockExclusive(&s_poolLock);
    auto head = reinterpret_cast<THREADPOOLGROUP**>(&s_groupRing);
    while (s_groupRing[0] != head) {
        auto link = reinterpret_cast<THREADPOOLGROUP***>(s_groupRing[0]);
        if (link[0] != nullptr) {
            *reinterpret_cast<THREADPOOLGROUP***>(link[1]) = link[0];
            reinterpret_cast<THREADPOOLGROUP***>(link[0])[1] = link[1];
        }
        link[0] = nullptr;
        link[1] = nullptr;
        THREADPOOLGROUP* group = GroupFromLink(link);
        operator delete(group, sizeof(THREADPOOLGROUP));
    }
    s_initialized = 0;
    ReleaseSRWLockExclusive(&s_poolLock);
}

// ref: FUN_14235a050
bool ThreadPoolIsMainThread() {
    return OsGetCurrentThreadId() == s_mainThread;
}

// ref: FUN_142359dd0
uint32_t ThreadPoolThreadCount() {
    return s_initialized != 0 ? s_threadCount : 0;
}

// ref: FUN_14235a070
THREADPOOLGROUP* ThreadPoolGroupCreate(int32_t priority) {
    AcquireSRWLockExclusive(&s_poolLock);
    auto head = reinterpret_cast<THREADPOOLGROUP**>(&s_groupRing);
    for (auto link = s_groupRing[0]; link != head; link = *reinterpret_cast<THREADPOOLGROUP***>(link)) {
        THREADPOOLGROUP* group = GroupFromLink(link);
        if (group->m_idle != 0) {
            if (group->m_next != nullptr) {
                *reinterpret_cast<THREADPOOLGROUP***>(group->m_prev) = group->m_next;
                reinterpret_cast<THREADPOOLGROUP***>(group->m_next)[1] = group->m_prev;
            }
            group->m_prev = nullptr;
            group->m_next = nullptr;
            group->m_priority = priority;
            ReleaseSRWLockExclusive(&s_poolLock);
            return group;
        }
    }
    auto group = static_cast<THREADPOOLGROUP*>(
        SMemAlloc(sizeof(THREADPOOLGROUP), 1, s_threadPoolFile, SStrHashFnv1a(s_threadPoolFile), 0x22a, 0));
    group->m_count = 0;
    InitializeSRWLock(&group->m_lock);
    InitializeConditionVariable(&group->m_done);
    group->m_idle = 1;
    group->m_priority = priority;
    group->m_prev = nullptr;
    group->m_next = nullptr;
    ReleaseSRWLockExclusive(&s_poolLock);
    return group;
}

// ref: FUN_142359d10
void ThreadPoolGroupRelease(THREADPOOLGROUP* group) {
    if (group == nullptr) {
        return;
    }
    AcquireSRWLockExclusive(&s_poolLock);
    auto link = reinterpret_cast<THREADPOOLGROUP**>(&group->m_next);
    if (s_initialized != 0) {
        group->m_prev = s_groupRing[1];
        group->m_next = reinterpret_cast<THREADPOOLGROUP**>(&s_groupRing);
        s_groupRing[1] = link;
        *reinterpret_cast<THREADPOOLGROUP***>(group->m_prev) = link;
        ReleaseSRWLockExclusive(&s_poolLock);
        return;
    }
    if (group->m_next != nullptr) {
        *reinterpret_cast<THREADPOOLGROUP***>(group->m_prev) = group->m_next;
        reinterpret_cast<THREADPOOLGROUP***>(group->m_next)[1] = group->m_prev;
    }
    group->m_prev = nullptr;
    group->m_next = nullptr;
    operator delete(group, sizeof(THREADPOOLGROUP));
    ReleaseSRWLockExclusive(&s_poolLock);
}

// ref: FUN_14235a330
void ThreadPoolGroupSetCount(THREADPOOLGROUP* group, int32_t count) {
    InterlockedExchange(&group->m_count, count);
    group->m_idle = 0;
}

// ref: FUN_142359e00
void ThreadPoolGroupAddCount(THREADPOOLGROUP* group, int32_t count) {
    InterlockedAdd(&group->m_count, count);
    group->m_idle = 0;
}

// ref: FUN_14235a5e0
void ThreadPoolGroupWait(THREADPOOLGROUP* group) {
    if (group->m_count <= 0) {
        return;
    }
    THREADPOOLQUEUE* queue = &s_queues[group->m_priority];
    AcquireSRWLockExclusive(&queue->m_lock);
    while (queue->m_head != queue->m_tail) {
        uint32_t tail = queue->m_tail;
        uint64_t link = queue->m_entries[tail];
        if (Untag(link)->m_group != group) {
            break;
        }
        queue->m_entries[tail] = link & TAG_MASK;
        queue->m_tail = (tail + 1) % QUEUE_SIZE;
        ReleaseSRWLockExclusive(&queue->m_lock);
        RunNode(link);
        WakeConditionVariable(&queue->m_done);
        AcquireSRWLockExclusive(&queue->m_lock);
    }
    ReleaseSRWLockExclusive(&queue->m_lock);
    AcquireSRWLockExclusive(&group->m_lock);
    while (group->m_count != 0) {
        SleepConditionVariableSRW(&group->m_done, &group->m_lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&group->m_lock);
}

// ref: FUN_142359cf0
// ref: FUN_1423599f0
void ThreadPoolSubmit(uint64_t count, blz::function_storage* tasks, int32_t priority, THREADPOOLGROUP* group) {
    if (count == 0) {
        return;
    }
    uint64_t first = TakeNodes(count);
    uint64_t link = first;
    for (uint64_t i = 0; i < count; i++) {
        THREADPOOLNODE* node = Untag(link);
        blz::function_storage* task = &tasks[i];
        node->m_task.m_manager(node->m_task.m_storage, nullptr, 0);
        task->m_manager(node->m_task.m_storage, task->m_storage, 2);
        node->m_task.m_invoke = task->m_invoke;
        node->m_task.m_manager = task->m_manager;
        task->m_invoke = nullptr;
        task->m_manager = blz::function_empty_manager;
        node->m_priority = priority;
        node->m_group = group;
        link = node->m_next;
    }
    THREADPOOLQUEUE* queue = &s_queues[priority];
    AcquireSRWLockExclusive(&queue->m_lock);
    link = first;
    for (uint64_t i = count; i != 0; i--) {
        // The next link is read before a full queue runs the task here: running it returns the
        // node and reuses its link (the reference reads it afterwards).
        uint64_t next = Untag(link)->m_next;
        uint32_t head = (queue->m_head + 1) % QUEUE_SIZE;
        if (head == queue->m_tail) {
            ReleaseSRWLockExclusive(&queue->m_lock);
            RunNode(link);
            AcquireSRWLockExclusive(&queue->m_lock);
        } else {
            queue->m_entries[queue->m_head] = link;
            queue->m_head = head;
        }
        link = next;
    }
    ReleaseSRWLockExclusive(&queue->m_lock);
    if (priority == 0 && count < 2) {
        WakeConditionVariable(&s_queues[0].m_work);
        return;
    }
    WakeAllConditionVariable(&queue->m_work);
}

// ref: FUN_14235a5d0
void ThreadPoolReclaim() {
    ReclaimReturnedNodes();
}

// ref: FUN_14235a710
void ThreadPoolFlush(int32_t priority) {
    THREADPOOLQUEUE* queue = &s_queues[priority];
    AcquireSRWLockExclusive(&queue->m_lock);
    int64_t start = OsGetNanoseconds();
    while (queue->m_head != queue->m_tail) {
        int64_t now = OsGetNanoseconds();
        if (start + 1000000 <= now || !WaitFor(&queue->m_done, &queue->m_lock, start + 1000000 - now)) {
            break;
        }
    }
    uint32_t first = 0;
    uint32_t end = 0;
    if (priority == 0) {
        end = s_threadsPerPriority[0];
    } else if (priority == 1) {
        first = s_threadsPerPriority[0];
        end = s_threadsPerPriority[0] + s_threadsPerPriority[1];
    } else if (priority == 2) {
        first = s_threadsPerPriority[0] + s_threadsPerPriority[1];
        end = s_threadCount;
    }
    while (first < end) {
        if (s_threads[first].m_state == 3) {
            WaitFor(&queue->m_done, &queue->m_lock, 1000000);
        } else {
            first++;
        }
    }
    ReleaseSRWLockExclusive(&queue->m_lock);
}
