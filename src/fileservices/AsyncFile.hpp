#ifndef FILESERVICES_ASYNC_FILE_HPP
#define FILESERVICES_ASYNC_FILE_HPP

// Engine/Source/FileServices/AsyncFile.cpp: the asynchronous file loader everything streams
// through (textures, models, sounds). A request (CAsyncObject) is queued by priority; the
// "Async Pending" thread opens its file and hands it to the disk queue ("Disk Queue", one
// thread), whose thread reads it when it is resident and otherwise passes it to the network
// queue ("Net Queue", three threads) to be made resident and read. Finished requests wait on a
// list until the main thread's AsyncReadPoll runs their callbacks.

#include <cstddef>
#include <cstdint>

namespace cas {
struct CASFILE;
}

struct CAsyncObject;
struct CAsyncQueue;

// The request's callbacks. The buffer callback runs on a reader thread and returns where to
// read `size` bytes; the read callback runs there after the read; the rest run on the main
// thread from AsyncReadPoll.
typedef void* (*ASYNCBUFFERPROC)(uint32_t size);
typedef void (*ASYNCREADPROC)(void* userData, void* buffer, uint32_t size);
typedef void (*ASYNCERRORPROC)(void* userData, void* buffer, uint32_t size, const char* error);

// A list link: next, previous; a list is a link that points at itself when empty.
struct ASYNCLINK {
    ASYNCLINK* m_next;
    ASYNCLINK* m_prev;
};

// The request's state bits (+0x150).
enum : uint32_t {
    ASYNC_DELIVERED = 0x1,      // taken off the done list for its callbacks
    ASYNC_COMPLETE = 0x2,
    ASYNC_READING = 0x4,        // a reader thread holds it
    ASYNC_REQUEUED = 0x8,       // not resident: passed to the network queue
    ASYNC_SUCCEEDED = 0x10,
    ASYNC_LOCKED = 0x20,        // its user data is being read under the lock
};

// One request (0x1b0 bytes, from the "CAsyncObject" object heap; its heap index is in the last
// four bytes).
struct CAsyncObject {
    char m_name[0x104];                 // 0x000: empty when it is by file data id
    int32_t m_fileDataId;               // 0x104
    cas::CASFILE* m_file;               // 0x108
    ASYNCLINK m_chunks;                 // 0x110: blz::list<AsyncFile_FileChunk>
    uint64_t m_chunkCount;              // 0x120
    void* m_buffer;                     // 0x128
    uint32_t m_offset;                  // 0x130
    uint32_t m_size;                    // 0x134: 0 with m_wholeFile is the whole file
    uint32_t m_priority;                // 0x138: 0..3
    uint8_t m_noRead;                   // 0x13c: only open (and make resident)
    uint8_t m_wholeFile;                // 0x13d: the file is this request's own to close
    volatile uint8_t m_canceled;        // 0x13e
    volatile uint8_t m_done;            // 0x13f
    int64_t m_sortKey;                  // 0x140: 0x7fffffff unsorted
    int32_t m_errorKind;                // 0x148
    uint32_t m_errorCode;               // 0x14c
    uint32_t m_flags;                   // 0x150
    void* m_userData;                   // 0x158
    bool (*m_openProc)(CAsyncObject* object, cas::CASFILE** file);  // 0x160: opens the file itself
    ASYNCBUFFERPROC m_bufferProc;       // 0x168
    ASYNCREADPROC m_readProc;           // 0x170
    ASYNCREADPROC m_doneProc;           // 0x178
    ASYNCERRORPROC m_errorProc;         // 0x180
    ASYNCREADPROC m_cancelProc;         // 0x188
    CAsyncQueue* m_queue;               // 0x190
    ASYNCLINK m_link;                   // 0x198
    uint32_t m_1a8;                     // 0x1a8
    uint32_t m_heapIndex;               // 0x1ac
};
static_assert(offsetof(CAsyncObject, m_fileDataId) == 0x104, "CAsyncObject layout");
static_assert(offsetof(CAsyncObject, m_sortKey) == 0x140, "CAsyncObject layout");
static_assert(offsetof(CAsyncObject, m_userData) == 0x158, "CAsyncObject layout");
static_assert(offsetof(CAsyncObject, m_link) == 0x198, "CAsyncObject layout");
static_assert(sizeof(CAsyncObject) == 0x1b0, "CAsyncObject is 0x1b0 bytes");

// Startup and shutdown. `pollBudget` is how long AsyncReadPoll may deliver for (ms, at least 20).
void AsyncReadConfigure(int32_t threadSleep, uint32_t pollBudget);
void AsyncReadShutdown();
// The main thread's delivery of finished requests.
void AsyncReadPoll();

// Requests. Priorities above 2 are 3.
CAsyncObject* AsyncFileReadById(int32_t fileDataId, uint32_t priority, void* userData, void* a4,
    ASYNCBUFFERPROC bufferProc, ASYNCREADPROC readProc, ASYNCREADPROC doneProc, ASYNCERRORPROC errorProc,
    ASYNCREADPROC cancelProc);
CAsyncObject* AsyncFileReadByName(const char* name, uint32_t priority, void* userData, void* a4,
    ASYNCBUFFERPROC bufferProc, ASYNCREADPROC readProc, ASYNCREADPROC doneProc, ASYNCERRORPROC errorProc,
    ASYNCREADPROC cancelProc);
CAsyncObject* AsyncFileReadFromFile(cas::CASFILE* file, void* buffer, uint32_t offset, uint32_t size,
    uint32_t priority, void* userData, ASYNCBUFFERPROC bufferProc, ASYNCREADPROC readProc, ASYNCREADPROC doneProc,
    ASYNCERRORPROC errorProc, ASYNCREADPROC cancelProc);
CAsyncObject* AsyncFileCreate(uint32_t priority, void* userData, void* a3, ASYNCBUFFERPROC bufferProc,
    ASYNCREADPROC readProc, ASYNCREADPROC doneProc, ASYNCERRORPROC errorProc, ASYNCREADPROC cancelProc,
    uint32_t offset, uint32_t size);
void AsyncFilePrefetchById(int32_t fileDataId);
void AsyncFilePrefetchSpan(int32_t fileDataId, uint32_t offset, uint32_t size);
void AsyncFilePrefetchByName(const char* name);

void AsyncFileCancel(CAsyncObject* object);
// Blocks until the request is done, then delivers it.
void AsyncFileWait(CAsyncObject* object);
bool AsyncFileIsResident(CAsyncObject* object);
bool AsyncFileSetSortKey(CAsyncObject* object, int32_t key);
bool AsyncFileLockUserData(CAsyncObject* object, void** userData);
void AsyncFileUnlockUserData(CAsyncObject* object);
const char* AsyncFileGetName(CAsyncObject* object);
uint64_t AsyncFileGetSize(CAsyncObject* object);
void AsyncFileGetResidentBytes(CAsyncObject* object, uint64_t* resident, uint64_t* total);

// Hooks and switches.
void* AsyncSetErrorHandler(void* handler);
void* AsyncSetPollHook(void* hook);
void AsyncSetHooks(void* a1, void* a2);
void AsyncSetWaitCallbacks(void* begin, void* progress, void* tick);
void AsyncSetBackgroundDownload(int32_t enabled);
void AsyncSetSortReads(bool sort);
void AsyncRequestResort();
// Applies the sort keys set since the last resort (the reader threads call it).
void AsyncResort();
void AsyncLock();
void AsyncUnlock();

// State.
bool AsyncDiskBusy();
bool AsyncNetBusy();
int32_t AsyncGetNetStatus();
uint32_t AsyncGetStalled();
void AsyncSetStalled(uint32_t stalled);
uint32_t AsyncHadCriticalFailure();
void AsyncClearCriticalFailure();
int32_t AsyncGetFailedFileDataId();
const char* AsyncGetFailureText();

// Priority helpers: a base per kind plus a distance clamped to [0, 10000].
int32_t AsyncPriorityFor(int32_t kind, void* a2, void* a3, float distance, int32_t urgent);
float AsyncPriorityMaxDistance();
int32_t AsyncPriorityForQueue(int32_t queue, float distance);

#endif
