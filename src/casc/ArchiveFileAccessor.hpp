#ifndef CASC_ARCHIVE_FILE_ACCESSOR_HPP
#define CASC_ARCHIVE_FILE_ACCESSOR_HPP

// pkg/casc/1.4.41/src/archive_file_accessor.cpp: reads from archive files, optionally through
// a pool of aligned buffers. A read is planned as an unaligned head, whole aligned blocks and
// an unaligned tail; the head and tail go through the buffers (so a neighbouring read of the
// same block is served from memory), the middle straight into the destination.

#include "aio/Aio.hpp"
#include "blz/Allocator.hpp"
#include "blz/IdMap.hpp"
#include "blz/ObjectPool.hpp"
#include "casc/ArchiveFileCache.hpp"
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace casc {

// One aligned buffer (0x70 bytes).
struct ARCHIVEBUFFER {
    blz::allocator* m_allocator;    // 0x00
    uint64_t m_archive;             // 0x08
    uint64_t m_offset;              // 0x10: block-aligned
    uint8_t* m_data;                // 0x18: the buffer size, 0x1000-aligned
    uint64_t m_lastUse;             // 0x20: the cache clock when last chosen
    uint32_t m_valid;               // 0x28: bytes read into it
    uint8_t m_cached;               // 0x2c
    uint8_t m_invalid;              // 0x2d: written over while a read was in flight
    uint8_t m_inFlight;             // 0x2e
    uint64_t m_aioKey;              // 0x30: the asynchronous read filling it
    blz::id_set m_waiters;          // 0x38: keys of the plans waiting on it
    HANDLE m_ready;                 // 0x68: set when no read is in flight
};
static_assert(offsetof(ARCHIVEBUFFER, m_waiters) == 0x38, "ARCHIVEBUFFER layout");
static_assert(sizeof(ARCHIVEBUFFER) == 0x70, "ARCHIVEBUFFER is 0x70 bytes");

// The buffers (0x38 bytes).
struct ARCHIVEBUFFERS {
    blz::allocator* m_allocator;    // 0x00
    uint32_t m_count;               // 0x08
    uint32_t m_size;                // 0x0c: a power of two
    SRWLOCK m_lock;                 // 0x10
    HANDLE m_available;             // 0x18: semaphore, released when a read completes
    uint64_t m_clock;               // 0x20
    ARCHIVEBUFFER* m_buffers;       // 0x28
    uint8_t m_shutdown;             // 0x30
};
static_assert(sizeof(ARCHIVEBUFFERS) == 0x38, "ARCHIVEBUFFERS is 0x38 bytes");

// The part of a read that goes through one buffer (0x28 bytes).
struct READPIECE {
    uint8_t* m_dest;                // 0x00
    uint64_t m_offset;              // 0x08: the block it lies in
    uint32_t m_size;                // 0x10
    uint32_t m_done;                // 0x14
    ARCHIVEBUFFER* m_buffer;        // 0x18: the buffer it waits on
    uint32_t m_bufferOffset;        // 0x20: where in the block it starts
    uint8_t m_error;                // 0x24
};
static_assert(sizeof(READPIECE) == 0x28, "READPIECE is 0x28 bytes");

typedef void (*READCALLBACK)(void* context, void* owner, uint64_t key);

// A planned read (0xc0 bytes).
struct READPLAN {
    uint64_t m_key;                 // 0x00: its id among the pending reads, 0 until needed
    uint64_t m_middleAio;           // 0x08: the asynchronous read of the middle
    ARCHIVEFILE* m_file;            // 0x10: a reference
    struct ARCHIVEFILEACCESSOR* m_owner;  // 0x18
    READCALLBACK m_callback;        // 0x20
    void* m_callbackContext;        // 0x28
    READPIECE m_head;               // 0x30
    uint8_t m_middleDone;           // 0x58
    uint8_t m_59;                   // 0x59
    uint64_t m_middleOffset;        // 0x60
    uint8_t* m_middleDest;          // 0x68
    uint32_t m_middleSize;          // 0x70
    READPIECE m_tail;               // 0x78
    uint8_t m_ekey[16];             // 0xa0: what the read is for
    uint64_t m_spanOffset;          // 0xb0: and the span asked for ({-1, 0} for none, DAT_1428f21f0)
    uint32_t m_spanSize;            // 0xb8
    uint32_t m_spanPad;             // 0xbc
};
static_assert(offsetof(READPLAN, m_head) == 0x30, "READPLAN layout");
static_assert(offsetof(READPLAN, m_middleOffset) == 0x60, "READPLAN layout");
static_assert(offsetof(READPLAN, m_tail) == 0x78, "READPLAN layout");
static_assert(sizeof(READPLAN) == 0xc0, "READPLAN is 0xc0 bytes");

typedef void (*TRUNCATEDREAD)(void* context, const uint8_t* key, const void* span, uint32_t read);

// 0x1b8 bytes, at dynamic storage +0x08.
struct ARCHIVEFILEACCESSOR {
    blz::allocator* m_allocator;        // 0x00
    void* m_owner;                      // 0x08: handed to read callbacks
    uint8_t m_directMiddle;             // 0x10: aligned middles read straight into place
    uint8_t m_ownsAio;                  // 0x11
    uint8_t m_waiting;                  // 0x12: someone waits for the pending reads to drain
    aio::CONTEXT* m_aio;                // 0x18
    TRUNCATEDREAD m_truncated;          // 0x20
    void* m_truncatedContext;           // 0x28
    ARCHIVEBUFFERS* m_buffers;          // 0x30
    SRWLOCK m_pendingLock;              // 0x38
    HANDLE m_drained;                   // 0x40: semaphore
    blz::id_map<READPLAN*> m_pending;   // 0x48
    blz::object_pool m_planPool;        // 0x78: 0xc0-byte plans
    SRWLOCK m_planPoolLock;             // 0x110
    blz::object_pool m_requestPool;     // 0x118: 0x40-byte requests
    SRWLOCK m_requestPoolLock;          // 0x1b0

    void Construct(blz::allocator* allocator, uint32_t bufferCount, uint32_t bufferSize, uint8_t directMiddle,
        aio::CONTEXT* aio, uint64_t a7, uint8_t a8, TRUNCATEDREAD truncated, void* context);
    void Destroy();
    // The storage destroys its accessor after its archive cache, as a member.
    ~ARCHIVEFILEACCESSOR() {
        this->Destroy();
    }
    int32_t Read(ARCHIVEFILE** file, uint64_t offset, uint32_t size, void* dest, uint32_t* read);
    void Invalidate(ARCHIVEFILE** file, uint64_t offset, uint32_t size);

    bool CachedRead(ARCHIVEFILE* file, void* dest, uint64_t offset, uint32_t size, bool* fallback, uint32_t* read);
    bool FillPiece(OSFILE* file, READPLAN* plan, READPIECE* piece);
    void CompleteBuffer(ARCHIVEBUFFER* buffer, uint32_t read, uint8_t error);
    void DeliverBuffer(ARCHIVEBUFFER* buffer, uint32_t read, uint8_t error);
    void RemovePending(uint64_t key);

    void ShutdownBuffers();
    void InvalidateBuffers();
    void EnableBuffers();
    int32_t ReadAsync(const uint8_t ekey[16], const void* span, ARCHIVEFILE** file, uint64_t offset, uint32_t size,
        void* dest, uint64_t* requestKey, uint64_t* outKey, READCALLBACK callback, void* context, uint32_t flags);
    int32_t ReadBuffered(const uint8_t ekey[16], const void* span, ARCHIVEFILE* file, void* dest, uint64_t offset,
        uint32_t size, READCALLBACK callback, void* context, bool nonBlocking, bool* fallback, uint64_t* requestKey,
        uint64_t* outKey);
    bool ClaimBuffers(READPLAN* plan, bool* headNeedsRead, bool* tailNeedsRead, bool* fallback, READPLAN** stored,
        uint64_t* requestKey);
    bool ClaimFill(READPLAN* plan, READPIECE* piece, READPLAN** stored, uint64_t* requestKey);
    int32_t GetResult(uint64_t key, uint32_t* read, bool* found);
    int32_t WaitPlan(uint64_t key, uint32_t* read, bool* found);
};
static_assert(offsetof(ARCHIVEFILEACCESSOR, m_buffers) == 0x30, "ARCHIVEFILEACCESSOR layout");
static_assert(offsetof(ARCHIVEFILEACCESSOR, m_pending) == 0x48, "ARCHIVEFILEACCESSOR layout");
static_assert(offsetof(ARCHIVEFILEACCESSOR, m_planPool) == 0x78, "ARCHIVEFILEACCESSOR layout");
static_assert(offsetof(ARCHIVEFILEACCESSOR, m_requestPool) == 0x118, "ARCHIVEFILEACCESSOR layout");
static_assert(sizeof(ARCHIVEFILEACCESSOR) == 0x1b8, "ARCHIVEFILEACCESSOR is 0x1b8 bytes");

void BuildReadPlan(ARCHIVEBUFFERS* buffers, READPLAN* plan, ARCHIVEFILE* file, void* dest, uint64_t offset, uint32_t size,
    READCALLBACK callback, void* context);
ARCHIVEBUFFER* FindBuffer(ARCHIVEBUFFERS* buffers, uint64_t archive, uint64_t offset, uint32_t needed, bool* hit,
    ARCHIVEBUFFER* exclude);
uint64_t NewRequestKey(uint8_t tag);
READPLAN* PlanCopy(blz::object_pool* pool, READPLAN* plan);
void PlanFree(blz::object_pool* pool, READPLAN* plan);
int32_t ErrorFromAio(uint8_t result);

}

#endif
