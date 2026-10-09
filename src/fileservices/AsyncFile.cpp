#include "fileservices/AsyncFile.hpp"

#include <windows.h>
#include <malloc.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <new>

#include "base/ObjectAlloc.hpp"
#include "blz/Function.hpp"
#include "cas/CAS.hpp"
#include "cas/CASManifest.hpp"
#include "gx/GxShader.hpp"
#include "os/Os.hpp"
#include "os/Sync.hpp"
#include "os/Timer.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/thread/SCritSect.hpp"
#include "storm/thread/SThread.hpp"

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\FileServices\\AsyncFile.cpp";
const char s_chunkTag[] = ".?AU?$list_node@UAsyncFile_FileChunk@@@blz@@";

// A reader thread (0x28 bytes): its queue and its index in it.
struct ASYNCTHREAD {
    ASYNCLINK m_link;                   // 0x00
    SThread m_thread;                   // 0x10
    CAsyncQueue* m_queue;               // 0x18
    uint32_t m_index;                   // 0x20
};
static_assert(sizeof(ASYNCTHREAD) == 0x28, "ASYNCTHREAD is 0x28 bytes");

}

// A read queue (0x98 bytes): its threads' wake events and what each is reading, and its
// requests by priority.
struct CAsyncQueue {
    int32_t m_state;                    // 0x00
    uint32_t m_index;                   // 0x04: 0 disk, 1 network
    uint32_t m_threadCount;             // 0x08
    SEvent m_wake[3];                   // 0x10
    volatile uint8_t m_stop;            // 0x28
    CAsyncObject* m_current[3];         // 0x30
    ASYNCLINK m_lists[4];               // 0x48
    ASYNCLINK m_link;                   // 0x88
};
static_assert(offsetof(CAsyncQueue, m_wake) == 0x10, "CAsyncQueue layout");
static_assert(offsetof(CAsyncQueue, m_current) == 0x30, "CAsyncQueue layout");
static_assert(offsetof(CAsyncQueue, m_lists) == 0x48, "CAsyncQueue layout");
static_assert(sizeof(CAsyncQueue) == 0x98, "CAsyncQueue is 0x98 bytes");

namespace {

// The lock every request's state is changed under (DAT_142e96e08), constructed statically
// (0x1401df370).
struct ASYNCLOCK {
    SCritSect m_section;
    ASYNCLOCK() {
        this->m_section.Initialize();
    }
};
ASYNCLOCK s_lock;
int32_t s_lockDepth;                    // DAT_142e96e68

// New requests by priority, before the pending thread has opened them (DAT_142cce280).
ASYNCLINK s_pending[4] = {
    { &s_pending[0], &s_pending[0] },
    { &s_pending[1], &s_pending[1] },
    { &s_pending[2], &s_pending[2] },
    { &s_pending[3], &s_pending[3] },
};
OSMUTEX s_pendingLock = { SRWLOCK_INIT };   // DAT_142e96d78
SEvent s_pendingEvent(false, false);        // DAT_142e96d88
SThread s_pendingThread;                    // DAT_142e96e30
ASYNCLINK s_done = { &s_done, &s_done };    // DAT_142cce2c8
ASYNCLINK s_queueList = { &s_queueList, &s_queueList };     // DAT_142cce2d8
ASYNCLINK s_threadList = { &s_threadList, &s_threadList };  // DAT_142cce2e8

CAsyncQueue* s_queues[2];               // DAT_142e96dd8: disk, network
const uint32_t s_threadCounts[2] = { 1, 3 };                // DAT_1426c6430
const char* const s_queueNames[2] = { "Disk Queue", "Net Queue" };  // PTR_s_Disk_Queue_1426c6420

TObjectAllocHeap* s_objectAlloc;        // DAT_142e96e40

// The request the main thread is blocked on: readers take it first (DAT_142e96d90).
CAsyncObject* volatile s_waiting;

volatile uint8_t s_shutdown;            // DAT_142e96d80
volatile uint8_t s_paused;              // DAT_142e96d81: a manifest swap is under way
volatile int32_t s_bgdlActive;          // DAT_142e96d84
volatile int32_t* s_netInFlight;        // DAT_142e96d98: shared between instances
int32_t s_localInFlight;                // DAT_142e96da0
uint8_t s_bgdl[3];                      // DAT_142e96da4: background download, per network thread
volatile uint8_t s_bgdlReading[3];      // DAT_142e96da8
uint32_t s_reads[2];                    // DAT_142e96db0
uint64_t s_bytes[2];                    // DAT_142e96db8
OSSHAREDMEMORY s_sharedMemory;          // DAT_142e96d60

volatile uint8_t s_resortRequested;     // DAT_142e96e01
uint8_t s_sortReads;                    // DAT_142e96e02: sortDiskReads
uint8_t s_resortPending;                // DAT_142e96e03
uint8_t s_bgdlWasOn;                    // DAT_142e96e04

int32_t s_failedFileDataId;             // DAT_142e96dd0
uint32_t s_criticalFailure;             // DAT_142e96e70
char s_failureText[0x80];               // DAT_142e96e80
uint32_t s_stalled;                     // DAT_142e96e6c

int32_t s_holdThreshold = 0x7fffffff;   // DAT_142cce2c0
uint32_t s_pollBudget = 100;            // DAT_142cce2c4
const int32_t s_priorityBase[2] = { 0, 10000 };             // DAT_142cce2f8
const float s_maxDistance = 10000.0f;   // DAT_1426c6510

typedef bool (*ASYNCERRORHANDLER)(int32_t kind, uint32_t code, const char* text, CAsyncObject* object);
typedef void (*ASYNCREADHOOK)(uint32_t size, int32_t network, uint32_t thread);
typedef bool (*ASYNCHOLDHOOK)(CAsyncObject* object);
ASYNCERRORHANDLER s_errorHandler;       // DAT_142e96e60
void (*s_pollHook)();                   // DAT_142e96e38
void (*s_waitBegin)();                  // DAT_142e96e50
void (*s_waitProgress)(float done);     // DAT_142e96e48
void (*s_waitTick)();                   // DAT_142e96e58
void* s_hook0;                          // DAT_142e96d38
ASYNCREADHOOK s_readHook;               // DAT_142e96dc8

bool HoldNone(CAsyncObject* object);
// Requests it returns true for are held back from the queues; its owner hands them back
// through AsyncRequeue (DAT_142cce270).
ASYNCHOLDHOOK volatile s_holdHook = &HoldNone;

// --- lists ---------------------------------------------------------------------------------

CAsyncObject* ObjectOf(ASYNCLINK* link) {
    return reinterpret_cast<CAsyncObject*>(reinterpret_cast<uint8_t*>(link) - offsetof(CAsyncObject, m_link));
}

CAsyncQueue* QueueOf(ASYNCLINK* link) {
    return reinterpret_cast<CAsyncQueue*>(reinterpret_cast<uint8_t*>(link) - offsetof(CAsyncQueue, m_link));
}

bool ListEmpty(const ASYNCLINK* list) {
    return list->m_next == list;
}

void Unlink(ASYNCLINK* link) {
    if (link->m_next != nullptr) {
        link->m_prev->m_next = link->m_next;
        link->m_next->m_prev = link->m_prev;
    }
    link->m_prev = nullptr;
    link->m_next = nullptr;
}

void LinkTail(ASYNCLINK* list, ASYNCLINK* link) {
    link->m_prev = list->m_prev;
    link->m_next = list;
    list->m_prev = link;
    link->m_prev->m_next = link;
}

void LinkBefore(ASYNCLINK* before, ASYNCLINK* link) {
    link->m_prev = before->m_prev;
    link->m_next = before;
    before->m_prev = link;
    link->m_prev->m_next = link;
}

// Moves all of `source` onto the end of `dest`.
void Splice(ASYNCLINK* dest, ASYNCLINK* source) {
    if (ListEmpty(source)) {
        return;
    }
    ASYNCLINK* first = source->m_next;
    ASYNCLINK* last = source->m_prev;
    first->m_prev = dest->m_prev;
    dest->m_prev->m_next = first;
    last->m_next = dest;
    dest->m_prev = last;
    source->m_next = source;
    source->m_prev = source;
}

// Unlinks whatever a local list still holds, as its destructor does.
void Clear(ASYNCLINK* list) {
    for (ASYNCLINK* link = list->m_next; link != list;) {
        ASYNCLINK* next = link->m_next;
        link->m_prev = nullptr;
        link->m_next = nullptr;
        link = next;
    }
    list->m_prev = nullptr;
    list->m_next = nullptr;
}

void ClearWaiting(CAsyncObject* object) {
    InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&s_waiting), nullptr, object);
}

void SignalThreads(CAsyncQueue* queue) {
    for (uint32_t i = 0; i < queue->m_threadCount; i++) {
        queue->m_wake[i].Set();
    }
}

void CloseOwnedFile(CAsyncObject* object) {
    if ((object->m_fileDataId != 0 || object->m_wholeFile) && object->m_file != nullptr) {
        cas::CASFileClose(&object->m_file);
        object->m_file = nullptr;
    }
}

// The request moves to the done list for AsyncReadPoll (the tail every ending shares).
void MoveToDone(CAsyncObject* object) {
    ClearWaiting(object);
    object->m_queue = nullptr;
    Unlink(&object->m_link);
    LinkTail(&s_done, &object->m_link);
    object->m_done = 1;
}

// ref: FUN_1402ca290
bool HoldNone(CAsyncObject* object) {
    (void)object;
    return false;
}

// --- sorting (std::sort instantiations) ----------------------------------------------------

// Descending by sort key.
// ref: FUN_1405d9970 (with FUN_1405d9bf0, FUN_1405d9d90)
void SortDescending(CAsyncObject** first, CAsyncObject** last) {
    std::sort(first, last, [](const CAsyncObject* a, const CAsyncObject* b) { return a->m_sortKey > b->m_sortKey; });
}

// Ascending by sort key.
// ref: FUN_1405d7130 (with FUN_1405d73b0, FUN_1405d7550)
void SortAscending(CAsyncObject** first, CAsyncObject** last) {
    std::sort(first, last, [](const CAsyncObject* a, const CAsyncObject* b) { return a->m_sortKey < b->m_sortKey; });
}

// --- the request's ending ------------------------------------------------------------------

// ref: FUN_1405d8b40
void ResetStats(uint32_t index) {
    s_reads[index] = 0;
    s_bytes[index] = 0;
}

// Done: the file closed, the result, out of the queue, onto the done list. `stat` 0 or 1
// counts the read against the disk or the network.
// ref: FUN_1405d7cd0
void ObjectFinish(CAsyncQueue* queue, CAsyncObject* object, uint32_t success, uint32_t stat) {
    s_lock.m_section.Enter();
    if (stat < 2) {
        s_reads[stat]++;
        s_bytes[stat] += object->m_size;
    }
    CloseOwnedFile(object);
    object->m_flags = (((success & 1) << 4) ^ ((object->m_flags & ~ASYNC_SUCCEEDED) | ASYNC_COMPLETE)) & ~ASYNC_READING;
    if (queue != nullptr) {
        for (uint32_t i = 0; i < queue->m_threadCount; i++) {
            if (queue->m_current[i] == object) {
                queue->m_current[i] = nullptr;
            }
        }
    }
    MoveToDone(object);
    s_lock.m_section.Leave();
}

// A canceled request leaves without its file being touched.
// ref: FUN_1405d8130
void FinishCanceled(CAsyncObject* object) {
    MoveToDone(object);
}

// Runs the request's callbacks and frees it.
// ref: FUN_1405da3b0
void ObjectDeliver(CAsyncObject* object) {
    ASYNCREADPROC proc;
    if (!object->m_canceled) {
        if ((object->m_flags & ASYNC_SUCCEEDED) == 0) {
            char text[0x80];
            cas::CASFormatError(object->m_errorKind, object->m_errorCode, text, sizeof(text));
            if (s_errorHandler != nullptr && s_errorHandler(object->m_errorKind, object->m_errorCode, text, object)) {
                return;
            }
            if (object->m_errorProc != nullptr) {
                object->m_errorProc(object->m_userData, object->m_buffer, object->m_size, text);
            }
            goto release;
        }
        proc = object->m_doneProc;
    } else {
        proc = object->m_cancelProc;
    }
    if (proc != nullptr) {
        proc(object->m_userData, object->m_buffer, object->m_size);
    }
release:
    if (object->m_file != nullptr && (object->m_fileDataId != 0 || object->m_wholeFile)) {
        cas::CASFileClose(&object->m_file);
        object->m_file = nullptr;
    }
    TObjectAllocHeap* alloc = s_objectAlloc;
    Unlink(&object->m_link);
    ASYNCLINK* chunk = object->m_chunks.m_next;
    object->m_chunkCount = 0;
    object->m_chunks.m_next = &object->m_chunks;
    object->m_chunks.m_prev = &object->m_chunks;
    while (chunk != &object->m_chunks) {
        ASYNCLINK* next = chunk->m_next;
        SMemFree(chunk, s_chunkTag, -6, 0);
        chunk = next;
    }
    alloc->Free(object);
}

// ref: FUN_1405d9fb0
CAsyncObject* ObjectAlloc() {
    auto object = static_cast<CAsyncObject*>(s_objectAlloc->Alloc());
    std::memset(object->m_name + 1, 0, 0x103);
    object->m_chunks.m_next = &object->m_chunks;
    object->m_chunks.m_prev = &object->m_chunks;
    object->m_chunkCount = 0;
    object->m_name[0] = '\0';
    object->m_fileDataId = 0;
    object->m_file = nullptr;
    object->m_buffer = nullptr;
    object->m_offset = 0;
    object->m_size = 0;
    object->m_flags = 0;
    object->m_userData = nullptr;
    object->m_openProc = nullptr;
    object->m_bufferProc = nullptr;
    object->m_readProc = nullptr;
    object->m_doneProc = nullptr;
    object->m_errorProc = nullptr;
    object->m_cancelProc = nullptr;
    object->m_queue = nullptr;
    object->m_link.m_next = nullptr;
    object->m_link.m_prev = nullptr;
    object->m_priority = 1;
    object->m_noRead = 0;
    object->m_wholeFile = 1;
    object->m_canceled = 0;
    object->m_done = 0;
    object->m_sortKey = 0x7fffffff;
    object->m_errorKind = 1;
    object->m_errorCode = 0;
    return object;
}

// Queues a new request by its priority and wakes the pending thread.
// ref: FUN_1405d9900
void Enqueue(CAsyncObject* object) {
    OsMutexLock(&s_pendingLock);
    LinkTail(&s_pending[object->m_priority], &object->m_link);
    OsMutexUnlock(&s_pendingLock);
    s_pendingEvent.Set();
}

// Hands back a request the hold hook kept.
// ref: FUN_1405d8b60
bool Requeue(CAsyncObject* object) {
    if (!object->m_canceled) {
        Enqueue(object);
        return true;
    }
    FinishCanceled(object);
    return false;
}

// --- the readers ---------------------------------------------------------------------------

// Which network threads may be cancelled: those making a background download resident,
// those on a canceled request, and those on a request no longer among the first the queue
// would hand out.
// ref: FUN_1405d8e20
int32_t CancelableSlots(int32_t* slots) {
    CAsyncQueue* net = s_queues[1];
    int32_t count = 0;
    if (net->m_stop) {
        return 0;
    }
    s_lock.m_section.Enter();
    bool busy = s_waiting != nullptr && s_waiting->m_queue == net;
    if (!busy) {
        for (int32_t i = 0; i < 4; i++) {
            if (!ListEmpty(&net->m_lists[i])) {
                busy = true;
                break;
            }
        }
    }
    if (busy) {
        for (uint32_t t = 0; t < net->m_threadCount; t++) {
            CAsyncObject* current = net->m_current[t];
            bool cancel = false;
            if (s_bgdlReading[t]) {
                cancel = true;
            } else if (current != nullptr && (current->m_flags & ASYNC_COMPLETE) == 0) {
                if (current->m_canceled) {
                    cancel = true;
                } else {
                    uint32_t position = 0;
                    CAsyncObject* waiting = s_waiting;
                    if (waiting == nullptr || waiting->m_queue != net || (position = 1, waiting != current)) {
                        bool found = false;
                        for (int32_t i = 0; i < 4 && !found && !cancel; i++) {
                            ASYNCLINK* list = &net->m_lists[i];
                            for (ASYNCLINK* link = list->m_next; link != list; link = link->m_next) {
                                CAsyncObject* object = ObjectOf(link);
                                if (object != s_waiting) {
                                    position++;
                                }
                                if (net->m_threadCount < position) {
                                    cancel = true;
                                    break;
                                }
                                if (object == current) {
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (cancel) {
                count++;
                *slots++ = static_cast<int32_t>(t);
            }
        }
    }
    s_lock.m_section.Leave();
    return count;
}

// ref: FUN_1405db910
void CancelNetReads() {
    int32_t slots[4];
    int32_t count = CancelableSlots(slots);
    for (int32_t i = 0; i < count; i++) {
        cas::CASCancelSlot(slots[i]);
    }
}

// Back onto its queue's list for its priority, and the queue's threads woken.
// ref: FUN_1405daf50
void RelinkToQueue(CAsyncObject* object) {
    CAsyncQueue* queue = object->m_queue;
    Unlink(&object->m_link);
    LinkTail(&queue->m_lists[object->m_priority], &object->m_link);
    SignalThreads(queue);
}

// Not resident: the network queue makes it resident and reads it.
// ref: FUN_1405d8090
void MoveToNet(CAsyncObject* object) {
    AsyncSetStalled(1);
    CAsyncQueue* net = s_queues[1];
    s_lock.m_section.Enter();
    CAsyncQueue* queue = object->m_queue;
    object->m_flags = (object->m_flags & ~ASYNC_READING) | ASYNC_REQUEUED;
    if (queue != nullptr) {
        for (uint32_t i = 0; i < object->m_queue->m_threadCount; i++) {
            if (object->m_queue->m_current[i] == object) {
                object->m_queue->m_current[i] = nullptr;
            }
        }
    }
    object->m_queue = net;
    RelinkToQueue(object);
    CancelNetReads();
    s_lock.m_section.Leave();
}

// Resident: a request that only opens is done here; one that reads goes on to its read.
// ref: FUN_1405d8ad0
bool CheckResident(CAsyncObject* object, bool resident) {
    if (!resident) {
        MoveToNet(object);
        return true;
    }
    if (!object->m_noRead
        && (object->m_buffer != nullptr
            || (object->m_bufferProc != nullptr && (object->m_size != 0 || object->m_wholeFile)))) {
        return false;
    }
    ObjectFinish(object->m_queue, object, 1, 2);
    return true;
}

// The request a reader thread takes next: the one the main thread waits on, else the first
// free one by priority. Canceled ones met on the way are finished.
// ref: FUN_1405d7e40
CAsyncObject* QueueNext(CAsyncQueue* queue, uint32_t thread) {
    s_lock.m_section.Enter();
    for (;;) {
        CAsyncObject* object = nullptr;
        CAsyncObject* waiting = s_waiting;
        if (waiting != nullptr && waiting->m_queue == queue && (waiting->m_flags & ASYNC_READING) == 0) {
            object = waiting;
        } else {
            for (int32_t i = 0; i < 4 && object == nullptr; i++) {
                ASYNCLINK* list = &queue->m_lists[i];
                for (ASYNCLINK* link = list->m_next; link != list; link = link->m_next) {
                    if ((ObjectOf(link)->m_flags & ASYNC_READING) == 0) {
                        object = ObjectOf(link);
                        break;
                    }
                }
            }
            if (object == nullptr) {
                s_lock.m_section.Leave();
                return nullptr;
            }
        }
        if (!object->m_canceled) {
            object->m_flags |= ASYNC_READING;
            queue->m_current[thread] = object;
            s_lock.m_section.Leave();
            return object;
        }
        FinishCanceled(object);
    }
}

// The request's span, into its buffer (asked of its buffer callback when it has none).
// ref: FUN_1405d8990
bool ObjectRead(CAsyncObject* object, uint32_t thread) {
    struct READCONTEXT {
        ERRORCONTEXT m_base;
        const char* m_format;
        const char* m_name;
    } context;
    const char* name = AsyncFileGetName(object);
    ErrorContextPush(&context.m_base, [](ERRORCONTEXT* base, uint64_t size, char* buffer) {
        auto read = reinterpret_cast<READCONTEXT*>(base);
        SStrPrintf(buffer, size, read->m_format, read->m_name);  // 0x1405d7110
    });
    context.m_format = "Async Reading: %s";
    context.m_name = name;
    if (object->m_bufferProc != nullptr && object->m_size != 0 && object->m_buffer == nullptr) {
        object->m_buffer = object->m_bufferProc(object->m_size);
    }
    uint32_t size = object->m_size;
    bool ok = false;
    uint32_t read = 0;
    if (size != 0
        && cas::CASFileReadAtOffset(object->m_file, object->m_buffer, object->m_offset, size, &read,
            ((object->m_flags >> 3) & 1) * 8 + 8, static_cast<int32_t>(thread))
        && (ok = read == size)) {
    } else {
        object->m_errorCode = cas::CASGetErrorCode(&object->m_errorKind);
    }
    if (object->m_readProc != nullptr && ok) {
        object->m_readProc(object->m_userData, object->m_buffer, object->m_size);
    }
    ErrorContextPop(&context.m_base);
    return ok;
}

// The disk queue's thread.
// ref: FUN_1405d7710
uint32_t DiskThread(void* param) {
    auto record = static_cast<ASYNCTHREAD*>(param);
    uint32_t thread = record->m_index;
    CAsyncQueue* queue = record->m_queue;
    for (;;) {
        queue->m_wake[thread].Wait(INFINITE);
        for (;;) {
            if (queue->m_stop) {
                return 0;
            }
            AsyncResort();
            CAsyncObject* object = QueueNext(queue, thread);
            if (object == nullptr) {
                break;
            }
            uint8_t residency = cas::CASFileGetSpanResidency(object->m_file, object->m_offset, object->m_size);
            if (CheckResident(object, residency == 2)) {
                continue;
            }
            if (object->m_wholeFile && object->m_size == 0) {
                object->m_size = static_cast<uint32_t>(cas::CASFileGetSize(object->m_file));
            }
            uint32_t success;
            if (!object->m_noRead) {
                if (ObjectRead(object, thread)) {
                    success = 1;
                } else {
                    success = 0;
                    if (cas::CASLastErrorIsNotResident()) {
                        MoveToNet(object);
                        continue;
                    }
                }
            } else {
                success = 1;
            }
            if (s_readHook != nullptr) {
                s_readHook(object->m_size, 0, thread);
            }
            ObjectFinish(queue, object, success, 0);
        }
    }
}

// One step of the background download: the next file the build has that is not resident,
// made resident. Gives up a pass after 100 resident ones; at the manifest's end it is off.
void BackgroundDownloadStep(uint32_t thread) {
    InterlockedIncrement(reinterpret_cast<volatile long*>(&s_bgdlActive));
    cas::CASMANIFEST* manifest = cas::g_casManifest;   // FUN_1402cd790
    if (manifest == nullptr) {
        InterlockedDecrement(reinterpret_cast<volatile long*>(&s_bgdlActive));
        Sleep(0);
        return;
    }
    manifest->BeginWalk();
    cas::BuildManifestEntry entry = {};
    uint32_t tries = 0;
    do {
        manifest->NextEntry(&entry);
        if (entry.m_fileDataId == 0) {
            InterlockedDecrement(reinterpret_cast<volatile long*>(&s_bgdlActive));
            s_bgdl[thread] = 0;
            Sleep(0);
            return;
        }
        if ((entry.m_contentFlags & 0x20000000) == 0
            && cas::CASGetResidencyByKey(entry.m_ckey, entry.m_fileDataId, entry.m_localeFlags, entry.m_contentFlags) == 1) {
            cas::CASFILE* file;
            if (cas::CASFileOpenByCKey(entry.m_ckey, entry.m_fileDataId, entry.m_localeFlags, entry.m_contentFlags,
                    &file)) {
                uint32_t size = static_cast<uint32_t>(cas::CASFileGetSize(file));
                uint32_t read;
                s_bgdlReading[thread] = 1;
                bool ok = cas::CASFileReadAtOffset(file, nullptr, 0, size, &read, 0x10, static_cast<int32_t>(thread));
                s_bgdlReading[thread] = 0;
                cas::CASFileClose(&file);
                if (!ok) {
                    if (cas::CASLastErrorWasCanceled()) {
                        cas::CASClearSlot(static_cast<int32_t>(thread));
                    }
                } else if (s_readHook != nullptr) {
                    s_readHook(size, 1, thread);
                }
            }
            InterlockedDecrement(reinterpret_cast<volatile long*>(&s_bgdlActive));
            Sleep(0);
            return;
        }
        if (++tries > 99) {
            InterlockedDecrement(reinterpret_cast<volatile long*>(&s_bgdlActive));
            Sleep(0);
            return;
        }
    } while (!s_paused);
    InterlockedDecrement(reinterpret_cast<volatile long*>(&s_bgdlActive));
    Sleep(0);
}

// A network queue thread: requests first, the background download when there are none.
// ref: FUN_1405d7850
uint32_t NetThread(void* param) {
    auto record = static_cast<ASYNCTHREAD*>(param);
    CAsyncQueue* queue = record->m_queue;
    uint32_t thread = record->m_index;
    for (;;) {
        if (!s_bgdl[thread]) {
            queue->m_wake[thread].Wait(INFINITE);
        }
        bool idle = false;
        for (;;) {
            if (queue->m_stop) {
                return 0;
            }
            if (s_paused) {
                break;
            }
            AsyncResort();
            CAsyncObject* object = QueueNext(queue, thread);
            if (object == nullptr) {
                idle = true;
                break;
            }
            InterlockedIncrement(reinterpret_cast<volatile long*>(s_netInFlight));
            if (object->m_wholeFile && object->m_size == 0) {
                object->m_size = static_cast<uint32_t>(cas::CASFileGetSize(object->m_file));
            }
            bool ok = ObjectRead(object, thread);
            if (!ok && cas::CASLastErrorWasCanceled()) {
                s_lock.m_section.Enter();
                object->m_flags &= ~ASYNC_READING;
                queue->m_current[thread] = nullptr;
                s_lock.m_section.Leave();
                cas::CASClearSlot(static_cast<int32_t>(thread));
                InterlockedDecrement(reinterpret_cast<volatile long*>(s_netInFlight));
                continue;
            }
            if (s_readHook != nullptr) {
                s_readHook(object->m_size, 1, thread);
            }
            ObjectFinish(queue, object, ok, 1);
            InterlockedDecrement(reinterpret_cast<volatile long*>(s_netInFlight));
        }
        if (!idle || !s_bgdl[thread] || *s_netInFlight != 0) {
            Sleep(0);
            continue;
        }
        BackgroundDownloadStep(thread);
    }
}

// Opens the files of a list of new requests; those that fail are done (failed), the rest go
// to the disk queue. Returns how many opened.
// ref: FUN_1405d81a0
int32_t OpenList(ASYNCLINK* list) {
    int32_t opened = 0;
    for (ASYNCLINK* link = list->m_next; link != list;) {
        ASYNCLINK* next = link->m_next;
        CAsyncObject* object = ObjectOf(link);
        bool ok = true;
        if (object->m_file == nullptr) {
            if (object->m_openProc != nullptr) {
                ok = object->m_openProc(object) != 0;
            } else {
                if (object->m_fileDataId == 0) {
                    ok = cas::CASFileOpenByName(object->m_name, &object->m_file, (object->m_flags >> 2) & 0x20);
                } else {
                    ok = cas::CASFileOpenById(object->m_fileDataId, &object->m_file, 0);
                }
                if (!ok) {
                    object->m_errorCode = cas::CASGetErrorCode(&object->m_errorKind);
                }
            }
        }
        if (ok) {
            opened++;
            object->m_queue = s_queues[0];
        } else {
            ObjectFinish(nullptr, object, 0, 2);
        }
        link = next;
    }
    return opened;
}

// The pending requests, past the hold hook, opened and handed to the disk queue by priority.
// ref: FUN_1405d8290
bool Dispatch() {
    ASYNCLINK local[4];
    for (int32_t i = 0; i < 4; i++) {
        local[i].m_next = &local[i];
        local[i].m_prev = &local[i];
    }
    OsMutexLock(&s_pendingLock);
    for (int32_t i = 0; i < 4; i++) {
        Splice(&local[i], &s_pending[i]);
    }
    OsMutexUnlock(&s_pendingLock);
    bool any = false;
    for (int32_t i = 0; i < 4; i++) {
        for (ASYNCLINK* link = local[i].m_next; link != &local[i];) {
            ASYNCLINK* next = link->m_next;
            Unlink(link);
            if (!s_holdHook(ObjectOf(link))) {
                LinkBefore(next, link);
            }
            link = next;
        }
        any |= !ListEmpty(&local[i]);
    }
    bool result = false;
    if (any) {
        for (int32_t i = 0; i < 4; i++) {
            OpenList(&local[i]);
        }
        s_lock.m_section.Enter();
        CAsyncQueue* disk = s_queues[0];
        for (int32_t i = 0; i < 4; i++) {
            Splice(&disk->m_lists[i], &local[i]);
        }
        s_lock.m_section.Leave();
        SignalThreads(s_queues[0]);
        result = true;
    }
    for (int32_t i = 3; i >= 0; i--) {
        Clear(&local[i]);
    }
    return result;
}

// Orders a list ascending by sort key.
// ref: FUN_1405d8880
void SortList(uint32_t count, ASYNCLINK* list) {
    auto objects = static_cast<CAsyncObject**>(_alloca(static_cast<size_t>(count) * sizeof(CAsyncObject*)));
    CAsyncObject** write = objects;
    for (ASYNCLINK* link = list->m_next; link != list; link = link->m_next) {
        *write++ = ObjectOf(link);
    }
    SortAscending(objects, objects + count);
    for (uint32_t i = 0; i < count; i++) {
        Unlink(&objects[i]->m_link);
        LinkTail(list, &objects[i]->m_link);
    }
}

// Places freshly opened requests by where their data lies on disk: what is not resident goes
// to the network, what only opens is done, the rest is sorted by container and offset.
// ref: FUN_1405d8be0
bool PlaceList(uint32_t count, ASYNCLINK* list) {
    auto files = static_cast<cas::CASFILE**>(_alloca(count * sizeof(cas::CASFILE*)));
    auto residency = static_cast<uint32_t*>(_alloca(count * sizeof(uint32_t)));
    auto containers = static_cast<uint64_t*>(_alloca(count * sizeof(uint64_t)));
    auto offsets = static_cast<uint64_t*>(_alloca(count * sizeof(uint64_t)));
    uint32_t index = 0;
    for (ASYNCLINK* link = list->m_next; link != list; link = link->m_next) {
        files[index++] = ObjectOf(link)->m_file;
    }
    uint32_t placed = 0;
    if (!cas::CASFileGetMetaData(files, residency, containers, offsets, count)) {
        for (ASYNCLINK* link = list->m_next; link != list;) {
            ASYNCLINK* next = link->m_next;
            MoveToNet(ObjectOf(link));
            link = next;
        }
        return false;
    }
    index = 0;
    for (ASYNCLINK* link = list->m_next; link != list;) {
        ASYNCLINK* next = link->m_next;
        CAsyncObject* object = ObjectOf(link);
        if (residency[index] < 2) {
            MoveToNet(object);
        } else if (!object->m_noRead
            && (object->m_buffer != nullptr
                || (object->m_bufferProc != nullptr && (object->m_size != 0 || object->m_wholeFile)))) {
            object->m_sortKey = static_cast<int64_t>((containers[index] << 32) | static_cast<uint32_t>(offsets[index]));
            placed++;
        } else {
            ObjectFinish(object->m_queue, object, 1, 2);
        }
        index++;
        link = next;
    }
    if (placed > 1) {
        SortList(placed, list);
    }
    return placed != 0;
}

// Dispatch with sortDiskReads: everything pending in one list, opened, placed by disk
// position and merged into the disk queue's first list in that order.
// ref: FUN_1405d9440
bool DispatchSorted() {
    ASYNCLINK local = { &local, &local };
    OsMutexLock(&s_pendingLock);
    for (int32_t i = 0; i < 4; i++) {
        Splice(&local, &s_pending[i]);
    }
    OsMutexUnlock(&s_pendingLock);
    for (ASYNCLINK* link = local.m_next; link != &local;) {
        ASYNCLINK* next = link->m_next;
        if (s_holdHook(ObjectOf(link))) {
            Unlink(link);
        }
        link = next;
    }
    uint32_t opened = 0;
    for (ASYNCLINK* link = local.m_next; link != &local;) {
        ASYNCLINK* next = link->m_next;
        CAsyncObject* object = ObjectOf(link);
        bool ok = true;
        if (object->m_file == nullptr) {
            if (object->m_openProc != nullptr) {
                ok = object->m_openProc(object) != 0;
            } else {
                if (object->m_fileDataId == 0) {
                    ok = cas::CASFileOpenByName(object->m_name, &object->m_file, (object->m_flags >> 2) & 0x20);
                } else {
                    ok = cas::CASFileOpenById(object->m_fileDataId, &object->m_file, 0);
                }
                if (!ok) {
                    object->m_errorCode = cas::CASGetErrorCode(&object->m_errorKind);
                }
            }
        }
        if (ok) {
            opened++;
            object->m_queue = s_queues[0];
        } else {
            s_lock.m_section.Enter();
            CloseOwnedFile(object);
            object->m_flags = (object->m_flags & ~(ASYNC_READING | ASYNC_SUCCEEDED)) | ASYNC_COMPLETE;
            MoveToDone(object);
            s_lock.m_section.Leave();
        }
        link = next;
    }
    bool result = false;
    if (opened != 0 && PlaceList(opened, &local)) {
        s_lock.m_section.Enter();
        CAsyncQueue* disk = s_queues[0];
        ASYNCLINK* target = &disk->m_lists[0];
        // Merge: each placed request goes before the first queued one with a larger key.
        ASYNCLINK* queued = target->m_next;
        ASYNCLINK* incoming = local.m_next;
        while (queued != target && incoming != &local) {
            if (ObjectOf(incoming)->m_sortKey < ObjectOf(queued)->m_sortKey) {
                ASYNCLINK* next = incoming->m_next;
                Unlink(incoming);
                LinkBefore(queued, incoming);
                incoming = next;
            } else {
                queued = queued->m_next;
            }
        }
        Splice(target, &local);
        SignalThreads(s_queues[0]);
        s_lock.m_section.Leave();
        result = true;
    }
    Clear(&local);
    return result;
}

// At shutdown: whatever is still pending is done, unread.
// ref: FUN_1405d9060
void FlushPending() {
    OsMutexLock(&s_pendingLock);
    for (int32_t i = 0; i < 4; i++) {
        while (!ListEmpty(&s_pending[i])) {
            CAsyncObject* object = ObjectOf(s_pending[i].m_next);
            s_lock.m_section.Enter();
            CloseOwnedFile(object);
            object->m_flags = (object->m_flags & ~(ASYNC_READING | ASYNC_SUCCEEDED)) | ASYNC_COMPLETE;
            MoveToDone(object);
            s_lock.m_section.Leave();
        }
    }
    OsMutexUnlock(&s_pendingLock);
}

// The "Async Pending" thread.
// ref: FUN_1405d7b70
uint32_t PendingThread(void* param) {
    (void)param;
    for (;;) {
        s_pendingEvent.Wait(INFINITE);
        for (;;) {
            if (s_shutdown) {
                FlushPending();
                return 0;
            }
            if (s_paused) {
                Sleep(0);
                break;
            }
            if (!Dispatch()) {
                break;
            }
        }
    }
}

// ref: FUN_1405d7bd0
uint32_t PendingThreadSorted(void* param) {
    (void)param;
    for (;;) {
        s_pendingEvent.Wait(INFINITE);
        for (;;) {
            if (s_shutdown) {
                FlushPending();
                return 0;
            }
            if (s_paused) {
                Sleep(0);
                break;
            }
            if (!DispatchSorted()) {
                break;
            }
        }
    }
}

// Finishes whatever a queue still holds, at shutdown.
// ref: FUN_1405d8fc0
void DrainQueue(CAsyncQueue* queue) {
    for (;;) {
        s_lock.m_section.Enter();
        CAsyncObject* object = nullptr;
        if (s_waiting != nullptr && s_waiting->m_queue == queue) {
            object = s_waiting;
        } else {
            for (int32_t i = 0; i < 4; i++) {
                if (!ListEmpty(&queue->m_lists[i])) {
                    object = ObjectOf(queue->m_lists[i].m_next);
                    break;
                }
            }
        }
        s_lock.m_section.Leave();
        if (object == nullptr) {
            return;
        }
        ObjectFinish(queue, object, 0, 2);
    }
}

// The network queue counts its reads in memory every instance shares, so that only one
// instance downloads in the background.
// ref: FUN_1405d7fa0
void OpenSharedMemory() {
    if (!OsIsFirstInstance()) {
        s_bgdl[0] = 0;
        s_bgdl[1] = 0;
        s_bgdl[2] = 0;
    }
    const char name[] = "WoW ASYNCTHREADSHAREDMEM";
    if (!OsSharedMemoryOpenNamed(&s_sharedMemory, name, 1, 4, 0)) {
        if (s_sharedMemory.m_data != nullptr) {
            *static_cast<int32_t*>(s_sharedMemory.m_data) = 0;
        }
        s_netInFlight = static_cast<int32_t*>(s_sharedMemory.m_data);
        return;
    }
    bool failed = OsSharedMemoryOpenNamed(&s_sharedMemory, name, 1, 4, 1);
    s_bgdl[0] = 0;
    s_bgdl[1] = 0;
    s_bgdl[2] = 0;
    s_netInFlight = &s_localInFlight;
    if (!failed) {
        s_netInFlight = static_cast<int32_t*>(s_sharedMemory.m_data);
    }
}

// ref: FUN_1405d7c30
bool BackgroundDownloadEnabled() {
    return (s_bgdl[0] | s_bgdl[2] | s_bgdl[1]) != 0;
}

// ref: FUN_1405d7e20
int32_t InFlight() {
    return s_bgdlActive + *s_netInFlight;
}

// Before a manifest swap: background download off and every read let finish.
// ref: FUN_1405dbc00
void OnPreSwap(void* storage) {
    (void)storage;
    s_bgdlWasOn = BackgroundDownloadEnabled();
    if (s_bgdlWasOn) {
        AsyncSetBackgroundDownload(0);
    }
    InterlockedExchange8(reinterpret_cast<volatile char*>(&s_paused), 1);
    while (InFlight() > 0) {
        OsSleepNanoseconds(1000000);    // FUN_141238570(1)
    }
}

// ref: FUN_1405dbc50
void OnPostSwap(void* storage) {
    (void)storage;
    InterlockedExchange8(reinterpret_cast<volatile char*>(&s_paused), 0);
    s_pendingEvent.Set();
    if (s_bgdlWasOn) {
        AsyncSetBackgroundDownload(1);
    }
}

// Takes the first finished request off the done list and delivers it; true while more wait.
// ref: FUN_1405db550
bool DeliverOne() {
    s_lock.m_section.Enter();
    if (ListEmpty(&s_done)) {
        s_lock.m_section.Leave();
        return false;
    }
    CAsyncObject* object = ObjectOf(s_done.m_next);
    Unlink(&object->m_link);
    bool more = !ListEmpty(&s_done);
    object->m_flags |= ASYNC_DELIVERED;
    s_lock.m_section.Leave();
    ObjectDeliver(object);
    return more;
}

// ref: FUN_1405d8e00
void SetWaiting(CAsyncObject* object) {
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&s_waiting), object);
}

// The text for a critical failure (formatted and discarded).
// ref: FUN_1405d9f50
void FormatFailure(char* buffer, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SStrVPrintfChecked(buffer, 0x100, format, args);
    va_end(args);
}

}

// --- requests --------------------------------------------------------------------------------

// ref: FUN_1405da880
CAsyncObject* AsyncFileCreate(uint32_t priority, void* userData, void* a3, ASYNCBUFFERPROC bufferProc,
    ASYNCREADPROC readProc, ASYNCREADPROC doneProc, ASYNCERRORPROC errorProc, ASYNCREADPROC cancelProc,
    uint32_t offset, uint32_t size) {
    CAsyncObject* object = ObjectAlloc();
    if (object != nullptr) {
        object->m_offset = offset;
        object->m_size = size;
        object->m_noRead = 0;
        object->m_wholeFile = size == 0;
        object->m_userData = userData;
        object->m_openProc = reinterpret_cast<int32_t (*)(CAsyncObject*)>(a3);
        if (priority > 2) {
            priority = 3;
        }
        object->m_bufferProc = bufferProc;
        object->m_readProc = readProc;
        object->m_doneProc = doneProc;
        object->m_errorProc = errorProc;
        object->m_cancelProc = cancelProc;
        object->m_priority = priority;
    }
    return object;
}

// ref: FUN_1405db600
CAsyncObject* AsyncFileReadById(int32_t fileDataId, uint32_t priority, void* userData, void* a4,
    ASYNCBUFFERPROC bufferProc, ASYNCREADPROC readProc, ASYNCREADPROC doneProc, ASYNCERRORPROC errorProc,
    ASYNCREADPROC cancelProc) {
    CAsyncObject* object = ObjectAlloc();
    if (object != nullptr) {
        object->m_noRead = 0;
        object->m_wholeFile = 1;
        object->m_offset = 0;
        object->m_size = 0;
        object->m_userData = userData;
        object->m_openProc = reinterpret_cast<int32_t (*)(CAsyncObject*)>(a4);
        object->m_fileDataId = fileDataId;
        if (priority > 2) {
            priority = 3;
        }
        object->m_bufferProc = bufferProc;
        object->m_readProc = readProc;
        object->m_doneProc = doneProc;
        object->m_errorProc = errorProc;
        object->m_cancelProc = cancelProc;
        object->m_priority = priority;
        Enqueue(object);
    }
    return object;
}

// ref: FUN_1405db6d0
CAsyncObject* AsyncFileReadFromFile(cas::CASFILE* file, void* buffer, uint32_t offset, uint32_t size,
    uint32_t priority, void* userData, ASYNCBUFFERPROC bufferProc, ASYNCREADPROC readProc, ASYNCREADPROC doneProc,
    ASYNCERRORPROC errorProc, ASYNCREADPROC cancelProc) {
    CAsyncObject* object = AsyncFileCreate(priority, userData, nullptr, bufferProc, readProc, doneProc, errorProc,
        cancelProc, offset, size);
    if (object != nullptr) {
        object->m_file = file;
        object->m_buffer = buffer;
        Enqueue(object);
    }
    return object;
}

// ref: FUN_1405db770
CAsyncObject* AsyncFileReadByName(const char* name, uint32_t priority, void* userData, void* a4,
    ASYNCBUFFERPROC bufferProc, ASYNCREADPROC readProc, ASYNCREADPROC doneProc, ASYNCERRORPROC errorProc,
    ASYNCREADPROC cancelProc) {
    CAsyncObject* object = ObjectAlloc();
    if (object != nullptr) {
        object->m_noRead = 0;
        object->m_wholeFile = 1;
        object->m_offset = 0;
        object->m_size = 0;
        object->m_userData = userData;
        object->m_openProc = reinterpret_cast<int32_t (*)(CAsyncObject*)>(a4);
        if (priority > 2) {
            priority = 3;
        }
        object->m_bufferProc = bufferProc;
        object->m_readProc = readProc;
        object->m_doneProc = doneProc;
        object->m_errorProc = errorProc;
        object->m_cancelProc = cancelProc;
        object->m_priority = priority;
        char* dest = object->m_name;
        for (int64_t left = 0x103;;) {
            char c = *name++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *dest = '\0';
                break;
            }
        }
        Enqueue(object);
    }
    return object;
}

// Makes a file resident without reading it, at the lowest priority.
// ref: FUN_1405db1e0
void AsyncFilePrefetchById(int32_t fileDataId) {
    CAsyncObject* object = ObjectAlloc();
    if (object != nullptr) {
        object->m_noRead = 1;
        object->m_wholeFile = 1;
        object->m_offset = 0;
        object->m_size = 0;
        object->m_priority = 3;
        object->m_fileDataId = fileDataId;
        Enqueue(object);
    }
}

// ref: FUN_1405db230
void AsyncFilePrefetchSpan(int32_t fileDataId, uint32_t offset, uint32_t size) {
    CAsyncObject* object = ObjectAlloc();
    if (object != nullptr) {
        object->m_offset = offset;
        object->m_size = size;
        object->m_noRead = 1;
        object->m_wholeFile = size == 0;
        object->m_priority = 3;
        object->m_fileDataId = fileDataId;
        Enqueue(object);
    }
}

// ref: FUN_1405db2a0
void AsyncFilePrefetchByName(const char* name) {
    CAsyncObject* object = ObjectAlloc();
    if (object == nullptr) {
        return;
    }
    object->m_noRead = 1;
    object->m_wholeFile = 1;
    object->m_offset = 0;
    object->m_size = 0;
    object->m_priority = 3;
    char* dest = object->m_name;
    for (int64_t left = 0x103;;) {
        char c = *name++;
        *dest++ = c;
        if (c == '\0') {
            break;
        }
        if (--left == 0) {
            *dest = '\0';
            break;
        }
    }
    Enqueue(object);
}

// ref: FUN_1405d9f80
void AsyncFileCancel(CAsyncObject* object) {
    s_lock.m_section.Enter();
    object->m_canceled = 1;
    s_lock.m_section.Leave();
}

// ref: FUN_1405dba50
void AsyncFileWait(CAsyncObject* object) {
    if ((object->m_flags & ASYNC_DELIVERED) != 0) {
        return;
    }
    if ((object->m_flags & (ASYNC_COMPLETE | ASYNC_READING)) == 0) {
        SetWaiting(object);
        if ((object->m_flags & ASYNC_REQUEUED) != 0) {
            CancelNetReads();
        }
    }
    if (s_waitBegin != nullptr) {
        s_waitBegin();
    }
    while (!object->m_done) {
        if (s_paused) {
            cas::CASPollUpdate();
        }
        if (s_waitProgress != nullptr) {
            uint64_t resident = 0;
            uint64_t total = 0;
            s_lock.m_section.Enter();
            if (object->m_file != nullptr) {
                cas::CASFileGetResidentBytes(object->m_file, &resident, &total);
            }
            s_lock.m_section.Leave();
            if (total != 0) {
                s_waitProgress(static_cast<float>(resident) / static_cast<float>(total));
            }
        }
        if (s_waitTick != nullptr) {
            s_waitTick();
        }
        if (s_pollHook != nullptr) {
            s_pollHook();
        }
        Sleep(0);
    }
    s_lock.m_section.Enter();
    SetWaiting(nullptr);
    Unlink(&object->m_link);
    object->m_flags |= ASYNC_DELIVERED;
    s_lock.m_section.Leave();
    ObjectDeliver(object);
}

// ref: FUN_1405db050
bool AsyncFileIsResident(CAsyncObject* object) {
    s_lock.m_section.Enter();
    uint32_t flags = object->m_flags;
    bool resident;
    if ((flags & ASYNC_COMPLETE) != 0) {
        resident = ((flags >> 4) & 1) != 0;
    } else if ((flags & ASYNC_REQUEUED) != 0) {
        resident = false;
    } else {
        uint8_t residency;
        if (object->m_file != nullptr) {
            residency = cas::CASFileGetSpanResidency(object->m_file, object->m_offset, object->m_size);
        } else if (object->m_fileDataId != 0) {
            residency = cas::CASFileGetResidency(object->m_fileDataId, 0, 0);    // FUN_1402cd230
        } else {
            residency = cas::CASGetResidencyByName(object->m_name, object->m_offset, object->m_size, 0);
        }
        resident = residency == 2;
    }
    s_lock.m_section.Leave();
    return resident;
}

// ref: FUN_1405db980
bool AsyncFileSetSortKey(CAsyncObject* object, int32_t key) {
    if (!s_sortReads) {
        if (s_resortRequested) {
            return false;
        }
        if (object != nullptr) {
            object->m_sortKey = key;
            s_resortPending = 1;
        }
    }
    return true;
}

// Takes the lock and hands out the user data; false once the request is canceled.
// ref: FUN_1405daff0
bool AsyncFileLockUserData(CAsyncObject* object, void** userData) {
    if (object->m_canceled) {
        return false;
    }
    s_lock.m_section.Enter();
    *userData = object->m_userData;
    object->m_flags |= ASYNC_LOCKED;
    return true;
}

// ref: FUN_1405db950
void AsyncFileUnlockUserData(CAsyncObject* object) {
    object->m_flags &= ~ASYNC_LOCKED;
    s_lock.m_section.Leave();
}

// The request's name, or for one by id the name the shader table gives the id.
// ref: FUN_1405da580
const char* AsyncFileGetName(CAsyncObject* object) {
    if (object->m_name[0] != '\0') {
        return object->m_name;
    }
    return GxShaderName(object->m_fileDataId, object->m_name, 0x104);
}

// ref: FUN_1405da5a0
uint64_t AsyncFileGetSize(CAsyncObject* object) {
    if (object->m_file == nullptr) {
        return 0;
    }
    return cas::CASFileGetSize(object->m_file);
}

// ref: FUN_1405da650
void AsyncFileGetResidentBytes(CAsyncObject* object, uint64_t* resident, uint64_t* total) {
    if (object->m_file == nullptr) {
        *resident = 0;
        *total = 0;
        return;
    }
    cas::CASFileGetResidentBytes(object->m_file, resident, total);
}

// --- the main thread -----------------------------------------------------------------------

// ref: FUN_1405db0f0
void AsyncReadPoll() {
    if (s_pollHook != nullptr) {
        s_pollHook();
    }
    uint32_t start = OsGetAsyncTimeMs();
    for (;;) {
        s_lock.m_section.Enter();
        if (ListEmpty(&s_done)) {
            s_lock.m_section.Leave();
            return;
        }
        CAsyncObject* object = ObjectOf(s_done.m_next);
        Unlink(&object->m_link);
        bool empty = ListEmpty(&s_done);
        object->m_flags |= ASYNC_DELIVERED;
        s_lock.m_section.Leave();
        ObjectDeliver(object);
        if (empty) {
            return;
        }
        if (s_pollBudget < OsGetAsyncTimeMs() - start) {
            return;
        }
    }
}

// Reorders the queues by the sort keys set since the last time (without sortDiskReads): each
// list is led by its keyed requests, lowest key first.
// ref: FUN_1405db320
void AsyncResort() {
    if (s_sortReads || !s_resortRequested) {
        return;
    }
    bool net = false;
    s_lock.m_section.Enter();
    for (int32_t q = 0; q < 2; q++) {
        CAsyncQueue* queue = s_queues[q];
        for (int32_t i = 0; i < 4; i++) {
            ASYNCLINK* list = &queue->m_lists[i];
            CAsyncObject* keyed[0x400];
            CAsyncObject** end = keyed;
            for (ASYNCLINK* link = list->m_next; link != list; link = link->m_next) {
                CAsyncObject* object = ObjectOf(link);
                if (object->m_sortKey != 0x7fffffff) {
                    if (end - keyed < 0x400) {
                        *end++ = object;
                    } else {
                        object->m_sortKey = 0x7fffffff;
                    }
                }
            }
            SortDescending(keyed, end);
            net |= q == 1 && end != keyed;
            for (CAsyncObject** at = keyed; at < end; at++) {
                CAsyncObject* object = *at;
                CAsyncQueue* owner = object->m_queue;
                object->m_sortKey = 0x7fffffff;
                Unlink(&object->m_link);
                ASYNCLINK* target = &owner->m_lists[object->m_priority];
                LinkBefore(target->m_next, &object->m_link);
                SignalThreads(owner);
            }
        }
    }
    s_lock.m_section.Leave();
    if (net) {
        int32_t slots[4];
        int32_t count = CancelableSlots(slots);
        for (int32_t i = 0; i < count; i++) {
            cas::CASCancelSlot(slots[i]);
        }
    }
    InterlockedExchange8(reinterpret_cast<volatile char*>(&s_resortRequested), 0);
}

// ref: FUN_1405dba20
void AsyncRequestResort() {
    if (!s_sortReads && s_resortPending) {
        InterlockedExchange8(reinterpret_cast<volatile char*>(&s_resortRequested), 1);
        s_resortPending = 0;
    }
}

// ref: FUN_1405da370
void AsyncLock() {
    if (s_lockDepth == 0) {
        s_lock.m_section.Enter();
    }
    s_lockDepth++;
}

// The last unlock also cancels the network reads that lost their place.
// ref: FUN_1405daef0
void AsyncUnlock() {
    if (--s_lockDepth == 0) {
        int32_t slots[4];
        int32_t count = CancelableSlots(slots);
        for (int32_t i = 0; i < count; i++) {
            cas::CASCancelSlot(slots[i]);
        }
        s_lock.m_section.Leave();
    }
}

// --- configuration -------------------------------------------------------------------------

// ref: FUN_1405da950
void AsyncReadConfigure(int32_t threadSleep, uint32_t pollBudget) {
    (void)threadSleep;
    s_pollBudget = pollBudget < 0x14 ? 0x14 : pollBudget;
    s_waitProgress = nullptr;
    s_waitBegin = nullptr;
    s_waitTick = nullptr;
    s_errorHandler = nullptr;
    for (uint32_t q = 0; q < 2; q++) {
        ResetStats(q);
        auto queue = static_cast<CAsyncQueue*>(SMemAlloc(sizeof(CAsyncQueue), 1, s_file, SStrHashFnv1a(s_file), 0x132, 0));
        for (int32_t i = 0; i < 3; i++) {
            new (&queue->m_wake[i]) SEvent(false, false);
        }
        queue->m_stop = 0;
        for (int32_t i = 0; i < 4; i++) {
            queue->m_lists[i].m_next = &queue->m_lists[i];
            queue->m_lists[i].m_prev = &queue->m_lists[i];
        }
        queue->m_link.m_next = nullptr;
        queue->m_link.m_prev = nullptr;
        queue->m_index = 2;
        queue->m_threadCount = 0;
        queue->m_state = 0;
        for (int32_t i = 0; i < 3; i++) {
            queue->m_current[i] = nullptr;
            queue->m_wake[i].Reset();
        }
        InterlockedExchange8(reinterpret_cast<volatile char*>(&queue->m_stop), 0);
        LinkTail(&s_queueList, &queue->m_link);
        queue->m_index = q;
        queue->m_threadCount = s_threadCounts[q];
        s_queues[q] = queue;
        if (q == 1) {
            OpenSharedMemory();
        }
        for (uint32_t t = 0; t < s_queues[q]->m_threadCount; t++) {
            char name[0x40];
            OsSnprintf(name, sizeof(name), "%s %d", s_queueNames[q], t);  // FUN_140216f20
            CAsyncQueue* owner = s_queues[q];
            auto record = static_cast<ASYNCTHREAD*>(SMemAlloc(sizeof(ASYNCTHREAD), 1, s_file, SStrHashFnv1a(s_file), 0x141, 0));
            record->m_link.m_prev = nullptr;
            record->m_link.m_next = nullptr;
            new (&record->m_thread) SThread();
            LinkTail(&s_threadList, &record->m_link);
            record->m_queue = owner;
            record->m_index = t;
            SThread::Create(owner->m_index != 1 ? &DiskThread : &NetThread, record, &record->m_thread, name, 0);
        }
    }
    SThread::Create(s_sortReads ? &PendingThreadSorted : &PendingThread, nullptr, &s_pendingThread, "Async Pending", 0);
    s_lockDepth = 0;
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 10, s_file, SStrHashFnv1a(s_file), 0x1fc, 0);
    s_objectAlloc = new (memory) TObjectAllocHeap("CAsyncObject", sizeof(CAsyncObject), 0x100, 10);
    blz::function_storage handler = {};
    handler.m_invoke = reinterpret_cast<void*>(&OnPreSwap);
    handler.m_manager = &blz::function_trivial_manager;
    cas::CASAddPreSwapHandler(&handler, reinterpret_cast<uint64_t>(&s_queues));
    if (handler.m_manager != &blz::function_trivial_manager) {
        handler.m_manager(handler.m_storage, nullptr, 0);
    }
    handler.m_manager = &blz::function_trivial_manager;
    handler.m_invoke = reinterpret_cast<void*>(&OnPostSwap);
    cas::CASAddManifestHandler(&handler, reinterpret_cast<uint64_t>(&s_queues));
    if (handler.m_manager != &blz::function_trivial_manager) {
        handler.m_manager(handler.m_storage, nullptr, 0);
    }
}

// ref: FUN_1405da0e0
void AsyncReadShutdown() {
    cas::CASRemovePreSwapHandlers(reinterpret_cast<uint64_t>(&s_queues));
    cas::CASRemoveManifestHandlers(reinterpret_cast<uint64_t>(&s_queues));
    InterlockedExchange8(reinterpret_cast<volatile char*>(&s_shutdown), 1);
    s_pendingEvent.Set();
    s_pendingThread.Wait(INFINITE);
    for (ASYNCLINK* link = s_threadList.m_next; link != &s_threadList; link = link->m_next) {
        auto record = reinterpret_cast<ASYNCTHREAD*>(link);
        InterlockedExchange8(reinterpret_cast<volatile char*>(&record->m_queue->m_stop), 1);
        record->m_queue->m_wake[record->m_index].Set();
        if (record->m_queue->m_index == 1) {
            cas::CASCancelSlot(static_cast<int32_t>(record->m_index));
        }
        record->m_thread.Wait(INFINITE);
    }
    for (ASYNCLINK* link = s_queueList.m_next; link != &s_queueList; link = link->m_next) {
        // Each queue's own shutdown hook (disk and network both a no-op).
        DrainQueue(QueueOf(link));
    }
    while (DeliverOne()) {
    }
    while (s_queueList.m_next != &s_queueList) {
        CAsyncQueue* queue = QueueOf(s_queueList.m_next);
        Unlink(&queue->m_link);
        for (int32_t i = 3; i >= 0; i--) {
            Clear(&queue->m_lists[i]);
        }
        queue->m_wake[2].Close();
        queue->m_wake[1].Close();
        queue->m_wake[0].Close();
        operator delete(queue, sizeof(CAsyncQueue));
    }
    s_queues[0] = nullptr;
    s_queues[1] = nullptr;
    if (s_objectAlloc != nullptr) {
        s_objectAlloc->~TObjectAllocHeap();
        SMemFree(s_objectAlloc, s_file, 0, 0);
    }
    s_objectAlloc = nullptr;
    while (s_threadList.m_next != &s_threadList) {
        auto record = reinterpret_cast<ASYNCTHREAD*>(s_threadList.m_next);
        record->m_thread.Close();
        Unlink(&record->m_link);
        operator delete(record, sizeof(ASYNCTHREAD));
    }
    s_waitProgress = nullptr;
    s_waitBegin = nullptr;
    s_waitTick = nullptr;
    s_errorHandler = nullptr;
    s_holdThreshold = 0x7fffffff;
    InterlockedExchange8(reinterpret_cast<volatile char*>(&s_shutdown), 0);
}

// ref: FUN_1405db9c0
void* AsyncSetErrorHandler(void* handler) {
    void* previous = reinterpret_cast<void*>(s_errorHandler);
    s_errorHandler = reinterpret_cast<ASYNCERRORHANDLER>(handler);
    return previous;
}

// ref: FUN_1405db9d0
void* AsyncSetPollHook(void* hook) {
    void* previous = reinterpret_cast<void*>(s_pollHook);
    s_pollHook = reinterpret_cast<void (*)()>(hook);
    return previous;
}

// ref: FUN_1405d8df0
void AsyncSetHooks(void* a1, void* a2) {
    s_hook0 = a1;
    s_readHook = reinterpret_cast<ASYNCREADHOOK>(a2);
}

// ref: FUN_1405da860
void AsyncSetWaitCallbacks(void* begin, void* progress, void* tick) {
    s_waitBegin = reinterpret_cast<void (*)()>(begin);
    s_waitProgress = reinterpret_cast<void (*)(float)>(progress);
    s_waitTick = reinterpret_cast<void (*)()>(tick);
}

// ref: FUN_1405d8e10
void* AsyncSetHoldHook(void* hook) {
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&s_holdHook), hook);
    return reinterpret_cast<void*>(&Requeue);
}

// ref: FUN_1405d7c50
void* AsyncResetHoldHook() {
    return InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&s_holdHook), reinterpret_cast<void*>(&HoldNone));
}

// enableBGDL
// ref: FUN_1405d7c60
void AsyncSetBackgroundDownload(int32_t enabled) {
    s_bgdl[0] = enabled != 0;
    s_bgdl[1] = s_bgdl[0];
    s_bgdl[2] = s_bgdl[0];
    if (enabled != 0 && s_queues[1] != nullptr) {
        for (uint32_t i = 0; i < s_queues[1]->m_threadCount; i++) {
            s_queues[1]->m_wake[i].Set();
        }
    }
}

// sortDiskReads
// ref: FUN_1405db9f0
void AsyncSetSortReads(bool sort) {
    s_sortReads = sort;
}

// --- state ---------------------------------------------------------------------------------

namespace {

bool QueueBusy(CAsyncQueue* queue) {
    if (queue == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < queue->m_threadCount; i++) {
        if (queue->m_current[i] != nullptr) {
            return true;
        }
    }
    for (int32_t i = 0; i < 4; i++) {
        if (!ListEmpty(&queue->m_lists[i])) {
            return true;
        }
    }
    return false;
}

}

// ref: FUN_1405dade0
bool AsyncDiskBusy() {
    s_lock.m_section.Enter();
    bool busy = QueueBusy(s_queues[0]);
    s_lock.m_section.Leave();
    return busy;
}

// ref: FUN_1405dae60
bool AsyncNetBusy() {
    s_lock.m_section.Enter();
    bool busy = QueueBusy(s_queues[1]);
    s_lock.m_section.Leave();
    return busy;
}

// What the network queue is busy with: 3 nothing, 2 urgent work, 1 work held back, 0 the
// most urgent kind.
// ref: FUN_1405da670
int32_t AsyncGetNetStatus() {
    uint32_t status = 3;
    s_lock.m_section.Enter();
    CAsyncQueue* net = s_queues[1];
    if (net != nullptr) {
        for (uint32_t t = 0; t < net->m_threadCount; t++) {
            CAsyncObject* object = net->m_current[t];
            if (object == nullptr) {
                for (int32_t i = 0; i < 4; i++) {
                    if (!ListEmpty(&net->m_lists[i])) {
                        object = ObjectOf(net->m_lists[i].m_next);
                        break;
                    }
                }
                if (object == nullptr) {
                    continue;
                }
            }
            uint32_t current = status;
            if (object->m_priority == 0) {
                status = current == 0 ? 2 : 0;
            } else if (object->m_priority != 1 || current < 2) {
                status = 2;
            } else {
                status = s_holdThreshold >= 0 ? 1 : 0;
            }
        }
    }
    s_stalled = status != 3;
    s_lock.m_section.Leave();
    return static_cast<int32_t>(status);
}

// ref: FUN_1405daee0
uint32_t AsyncGetStalled() {
    return s_stalled;
}

// ref: FUN_1405db9e0
void AsyncSetStalled(uint32_t stalled) {
    s_stalled = stalled;
}

// ref: FUN_1405da850
uint32_t AsyncHadCriticalFailure() {
    return s_criticalFailure;
}

// ref: FUN_1405db970
void AsyncClearCriticalFailure() {
    s_criticalFailure = 0;
}

// ref: FUN_1405dba00
int32_t AsyncGetFailedFileDataId() {
    return s_failedFileDataId;
}

// ref: FUN_1405dba10
const char* AsyncGetFailureText() {
    return s_failureText;
}

// ref: FUN_1405db870
void AsyncSetCriticalFailure(CAsyncObject* object, const char* text) {
    s_failedFileDataId = object->m_fileDataId;
    s_criticalFailure = 1;
    const char* name = object->m_name;
    if (*name == '\0') {
        name = GxShaderName(object->m_fileDataId, object->m_name, 0x104);
    }
    char* dest = s_failureText;
    const char* source = text;
    for (int64_t left = 0x7f;;) {
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            break;
        }
        if (--left == 0) {
            *dest = '\0';
            break;
        }
    }
    char buffer[0x100];
    FormatFailure(buffer, "CRITICAL async streaming failure: %s - Async fail result: %s\n", name, text);
}

// ref: FUN_1405da830
int32_t AsyncGetDiskState() {
    return s_queues[0] != nullptr ? s_queues[0]->m_state : 0;
}

// The read counters, and optionally reset.
// ref: FUN_1405da740
void AsyncGetStats(uint32_t* stats, int32_t reset) {
    s_lock.m_section.Enter();
    stats[0] = QueueBusy(s_queues[0]) ? 1 : 0;
    stats[1] = s_reads[0];
    *reinterpret_cast<uint64_t*>(stats + 4) = s_bytes[0];
    if (reset != 0) {
        ResetStats(0);
    }
    stats[2] = s_reads[1];
    *reinterpret_cast<uint64_t*>(stats + 6) = s_bytes[1];
    if (reset != 0) {
        ResetStats(1);
    }
    s_lock.m_section.Leave();
}

// ref: FUN_1405d7f90
uint32_t AsyncGetReads(uint32_t index) {
    return s_reads[index];
}

// ref: FUN_1405d7f80
uint64_t AsyncGetBytes(uint32_t index) {
    return s_bytes[index];
}

// How far the network queue's first busy request is: {resident, total, valid}.
// ref: FUN_1405da5c0
void AsyncGetNetProgress(uint64_t* progress) {
    reinterpret_cast<uint8_t*>(progress)[0x10] = 0;
    s_lock.m_section.Enter();
    CAsyncQueue* net = s_queues[1];
    if (net != nullptr) {
        for (uint32_t t = 0; t < net->m_threadCount; t++) {
            CAsyncObject* object = net->m_current[t];
            if (object != nullptr && object->m_file != nullptr) {
                reinterpret_cast<uint8_t*>(progress)[0x10] = 1;
                progress[1] = 0;
                progress[0] = 0;
                cas::CASFileGetResidentBytes(object->m_file, &progress[0], &progress[1]);
                break;
            }
        }
    }
    s_lock.m_section.Leave();
}

// ref: FUN_1405dbc90
int32_t AsyncPriorityFor(int32_t kind, void* a2, void* a3, float distance, int32_t urgent) {
    (void)a2;
    (void)a3;
    if (s_maxDistance <= distance) {
        distance = s_maxDistance;
    }
    if (distance <= 0.0f) {
        distance = 0.0f;
    }
    int32_t priority = s_priorityBase[kind] + static_cast<int32_t>(std::lrintf(distance)) - 30000;
    if (urgent == 0) {
        priority = s_priorityBase[kind] + static_cast<int32_t>(std::lrintf(distance));
    }
    return priority;
}

// ref: FUN_1405dbcd0
float AsyncPriorityMaxDistance() {
    return s_maxDistance;
}

// ref: FUN_1405dbce0
int32_t AsyncPriorityForQueue(int32_t queue, float distance) {
    if (s_maxDistance <= distance) {
        distance = s_maxDistance;
    }
    int32_t base = queue == 0 ? s_priorityBase[1] : s_priorityBase[0];
    if (distance <= 0.0f) {
        distance = 0.0f;
    }
    return static_cast<int32_t>(std::lrintf(distance)) + base;
}
