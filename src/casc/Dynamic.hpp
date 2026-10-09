#ifndef CASC_DYNAMIC_HPP
#define CASC_DYNAMIC_HPP

// pkg/casc/1.4.41/src/dynamic.cpp: casc's dynamic container - the local data.### archives and
// their .idx key mapping tables, shared between processes through a shmem control block.

#include "blz/AllocString.hpp"
#include "blz/Allocator.hpp"
#include "blz/IdMap.hpp"
#include "blz/ObjectPool.hpp"
#include "casc/Common.hpp"
#include "casc/ContainerIndex.hpp"
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace casc {

// casc's error codes (names from the table at 0x142d3e6c0).
enum ERRORCODE : int32_t {
    ERR_SUCCESS = 0,
    ERR_UNSPECIFIED = 1,
    ERR_NOT_RESERVED = 2,
    ERR_NOT_RESIDENT = 3,
    ERR_INVALID_SPAN = 4,
    ERR_UNSUPPORTED = 5,
    ERR_NOT_OPENED = 6,
    ERR_DISK_FULL = 7,
    ERR_IO = 8,
    ERR_NEEDS_REPAIR = 9,
    ERR_INVALID_PARAM = 10,
    ERR_LOCKED = 11,
    ERR_FILE_EXISTS = 12,
    ERR_INVALID_DRIVE_TYPE = 13,
    ERR_CASC_FULL = 14,
    ERR_CANCELED = 15,
    ERR_ARCHIVE_FULL = 16,
    ERR_ALREADY_OPENED = 17,
    ERR_NO_INDEX_FOUND = 18,
    ERR_PATH_TOO_LONG = 19,
    ERR_PERMISSION_DENIED = 20,
    ERR_WOULD_BLOCK = 21,
    ERR_FILE_TOO_BIG = 22,
    ERR_WOULD_NOT_PIPELINE = 23,
    ERR_SHARING_VIOLATION = 24,
};

const char* ErrorString(int32_t error);

// How a dynamic container is opened (0x58 bytes).
struct OPENPARAMS {
    int32_t m_archiveCacheSize;         // 0x00: open archive files kept
    int32_t m_archiveCount;             // 0x04: 1..0x3ff; the container is this many GiB
    blz::allocator* m_allocator;        // 0x08: null for the process allocator
    int32_t m_bufferCount;              // 0x10: aligned read buffers (0, or 2 and up)
    int32_t m_bufferSize;               // 0x14: a power of two
    void* m_ioContext;                  // 0x18: aio context, or null for one of its own
    uint64_t m_20;                      // 0x20
    uint8_t m_freezeArchives;           // 0x28
    void (*m_callbackA)(void* context, void* argument, void* data);  // 0x30: statistics
    void* m_callbackAContext;           // 0x38
    void (*m_callbackB)(void* context, void* argument, void* data);  // 0x40
    void* m_callbackBContext;           // 0x48
    uint8_t m_lru;                      // 0x50
    uint8_t m_reclaimFreeSpace;         // 0x51
    uint8_t m_noPidTable;               // 0x52: also turns off the buffers and the LRU
};

struct DYNAMICSTORAGE;

// A dynamic container (0xc20 bytes).
struct DYNAMICCONTAINER {
    blz::allocator* m_allocator;        // 0x000
    blz::alloc_string m_path;           // 0x008
    int32_t m_mode;                     // 0x028: 1 create, 2 read-only, 3 existing, 5 exclusive
    uint8_t m_lruActive;                // 0x02c
    uint8_t m_freezeArchives;           // 0x02d
    STATSINK m_stats;                   // 0x030
    CONTAINERINDEX m_index;             // 0x058
    DYNAMICSTORAGE* m_storage;          // 0xb00
    void* m_writer;                     // 0xb08: not ported (FUN_1421521f0 destroys it)
    void* m_lruTable;                   // 0xb10: not ported (dynamic/lru.cpp)
    SRWLOCK m_lock;                     // 0xb18
    blz::id_map<void*> m_b20;           // 0xb20
    blz::object_pool m_pool;            // 0xb50: 0x50-byte elements
    SRWLOCK m_poolLock;                 // 0xbe8
    blz::id_set m_frozen;               // 0xbf0
};
static_assert(offsetof(DYNAMICCONTAINER, m_stats) == 0x30, "DYNAMICCONTAINER layout");
static_assert(offsetof(DYNAMICCONTAINER, m_index) == 0x58, "DYNAMICCONTAINER layout");
static_assert(offsetof(DYNAMICCONTAINER, m_storage) == 0xb00, "DYNAMICCONTAINER layout");
static_assert(offsetof(DYNAMICCONTAINER, m_b20) == 0xb20, "DYNAMICCONTAINER layout");
static_assert(offsetof(DYNAMICCONTAINER, m_pool) == 0xb50, "DYNAMICCONTAINER layout");
static_assert(offsetof(DYNAMICCONTAINER, m_frozen) == 0xbf0, "DYNAMICCONTAINER layout");
static_assert(sizeof(DYNAMICCONTAINER) == 0xc20, "DYNAMICCONTAINER is 0xc20 bytes");

int32_t DynamicOpen(DYNAMICCONTAINER** container, const char* directory, int32_t mode, OPENPARAMS* params);
void DynamicClose(DYNAMICCONTAINER** container);
int32_t DynamicContains(DYNAMICCONTAINER* container, const uint8_t key[16]);
int32_t DynamicGetSpan(DYNAMICCONTAINER* container, const uint8_t key[16], uint64_t span[2]);
int32_t DynamicRemove(DYNAMICCONTAINER* container, const uint8_t key[16]);
// span: {offset into the key's data, size}.
int32_t DynamicRead(DYNAMICCONTAINER* container, const uint8_t key[16], const uint64_t span[2], void* dest,
    uint32_t* read);
int32_t DynamicReadAsync(DYNAMICCONTAINER* container, const uint8_t key[16], const uint64_t span[2], void* dest,
    uint64_t* outKey, void (*callback)(void* context, void* owner, uint64_t key), void* context, uint32_t flags);
int32_t DynamicGetResult(DYNAMICCONTAINER* container, uint64_t key, uint32_t* read);
int32_t DynamicMarkNonResident(DYNAMICCONTAINER* container, const uint8_t key[16], const uint64_t span[2]);
void WriteRepairMarker(const char* directory, int32_t* reason);

}

#endif
