#ifndef CASC_CONTAINER_INDEX_HPP
#define CASC_CONTAINER_INDEX_HPP

// pkg/casc/1.4.41/src/dynamic/container_index.cpp: the index of a dynamic container - the
// sixteen key mapping tables over their files, bound through the shmem control block under a
// global mutex shared with every other process using the container.

#include "blz/AllocString.hpp"
#include "blz/IdMap.hpp"
#include "blz/ObjectPool.hpp"
#include "casc/Common.hpp"
#include "casc/IndexTables.hpp"
#include "casc/KeyMappingTable.hpp"
#include "casc/Shmem.hpp"
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace casc {

// Writes the reconstruction headers of an archive (container, archive number, the 16 headers).
typedef int32_t (*HEADERWRITER)(void* context, uint32_t archive, const uint8_t* keys);

// The free space cache (0xd0 bytes, at container index +0x950).
struct FREESPACECACHE {
    blz::allocator* m_allocator;    // 0x00
    STATSINK* m_stats;              // 0x08
    blz::object_pool m_pool;        // 0x10: 0x30-byte spans
    void* m_free[2];                // 0xa8: list head
    void* m_used[2];                // 0xb8: list head
    uint32_t m_reallocated;         // 0xc8
    uint32_t m_unused;              // 0xcc

    void Init(blz::allocator* allocator, STATSINK* stats);
    void Destroy();
};
static_assert(sizeof(FREESPACECACHE) == 0xd0, "FREESPACECACHE is 0xd0 bytes");

// 0xaa8 bytes, at dynamic storage +0x58.
struct CONTAINERINDEX {
    blz::allocator* m_allocator;        // 0x00
    STATSINK* m_stats;                  // 0x08
    uint8_t m_bound;                    // 0x10
    uint8_t m_reclaim;                  // 0x11: free space reclamation
    blz::alloc_string m_path;           // 0x18
    int32_t m_mode;                     // 0x38
    uint64_t m_maxSize;                 // 0x40
    HEADERWRITER m_writeHeaders;        // 0x48
    void* m_writeHeadersContext;        // 0x50
    uint8_t m_noPidTable;               // 0x58
    SRWLOCK m_lock;                     // 0x60
    SHMEMBINDING m_shmem;               // 0x68
    HANDLE m_mutex;                     // 0xb0
    INDEXTABLES m_tables;               // 0xb8
    KEYMAPPINGTABLE m_kmts[16];         // 0x1d0
    FREESPACECACHE m_freeSpace;         // 0x950
    SRWLOCK m_bucketLocks[16];          // 0xa20
    blz::id_set* m_frozen;              // 0xaa0

    void Construct(blz::allocator* allocator, STATSINK* stats);
    void Destroy();
    int32_t Bind(const char* path, int32_t mode, uint64_t maxSize, uint8_t reclaim, HEADERWRITER writeHeaders,
        void* context, int32_t* processes, blz::id_set* frozen, uint8_t noPidTable);
    int32_t BindWritable(int32_t* processes, blz::id_set* frozen);
    int32_t Reconcile(blz::id_set* frozen);
    bool HasPidTable();
    int32_t Lookup(const uint8_t key[9], uint32_t salt, SPAN* span);
    int32_t LookupLocked(uint32_t bucket, const uint8_t key[9], SPAN* span);
    int32_t QuerySpan(const uint8_t key[9], uint32_t salt, const SPAN* request);
    int32_t Resolve(const uint8_t key[9], uint32_t salt, const SPAN* request, SPAN* out, SPAN* stored);
    uint8_t CheckWriters();
    int32_t RemoveKeys(const uint8_t* keys, uint32_t count, uint8_t salt);
    int32_t RemoveKey(const uint8_t key[9], uint32_t salt);
    int32_t RemoveKeyLocked(const uint8_t key[9], uint32_t salt);
    int32_t Update(uint32_t bucket, const uint8_t key[9], uint8_t type, const SPAN* span, SPAN* previous);
    int32_t UpdateResidency(const uint8_t key[16], uint32_t salt, const SPAN* span, uint8_t resident);
    int32_t FlushAll();
    int32_t FlushTable(uint32_t bucket, KMTRECORD* records, uint32_t count);
    int32_t EnumerateAll(KMTSPANVISITOR visitor, void* context);

    static uint32_t BucketOf(const uint8_t key[9], uint32_t salt);
};
static_assert(offsetof(CONTAINERINDEX, m_shmem) == 0x68, "CONTAINERINDEX layout");
static_assert(offsetof(CONTAINERINDEX, m_tables) == 0xb8, "CONTAINERINDEX layout");
static_assert(offsetof(CONTAINERINDEX, m_kmts) == 0x1d0, "CONTAINERINDEX layout");
static_assert(offsetof(CONTAINERINDEX, m_freeSpace) == 0x950, "CONTAINERINDEX layout");
static_assert(offsetof(CONTAINERINDEX, m_bucketLocks) == 0xa20, "CONTAINERINDEX layout");
static_assert(sizeof(CONTAINERINDEX) == 0xaa8, "CONTAINERINDEX is 0xaa8 bytes");

}

#endif
