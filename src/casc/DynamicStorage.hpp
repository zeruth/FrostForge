#ifndef CASC_DYNAMIC_STORAGE_HPP
#define CASC_DYNAMIC_STORAGE_HPP

// pkg/casc/1.4.41/src/dynamic_storage.cpp: the data.### archives of a dynamic container. An
// offset's top bits pick the archive (offset >> m_shift) and the rest is the position in it.

#include "blz/IdMap.hpp"
#include "casc/ArchiveFileAccessor.hpp"
#include "casc/ArchiveFileCache.hpp"
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace casc {

// 0x260 bytes, at dynamic container +0xb00.
struct DYNAMICSTORAGE {
    blz::allocator* m_allocator;        // 0x000
    ARCHIVEFILEACCESSOR m_accessor;     // 0x008
    char* m_directory;                  // 0x1c0
    uint8_t m_shift;                    // 0x1c8: 30
    uint64_t m_mask;                    // 0x1d0
    uint8_t m_readOnly;                 // 0x1d8
    DYNAMICARCHIVECACHE m_archives;     // 0x1e0
    blz::id_set* m_frozen;              // 0x240: archives written by no one (read-only opens)
    SRWLOCK m_lock;                     // 0x248
    LISTLINK m_list;                    // 0x250

    DYNAMICSTORAGE(blz::allocator* allocator, const char* directory, uint64_t cacheCapacity, uint8_t shift,
        uint8_t readOnly, aio::CONTEXT* aio, uint64_t a8, uint8_t a9, uint32_t bufferCount, uint32_t bufferSize,
        TRUNCATEDREAD truncated, void* context);
    ~DYNAMICSTORAGE();

    void SetOwner(void* owner);
    int32_t Freeze(blz::id_set* frozen);
    bool IsFrozen(uint64_t archive);
    int32_t Read(void* dest, uint64_t offset, uint32_t size, uint32_t* read);
    int32_t ReadAsync(const uint8_t ekey[16], const void* span, void* dest, uint64_t offset, uint32_t size,
        uint64_t requestKey, uint64_t* outKey, READCALLBACK callback, void* context, uint32_t flags);
    int32_t GetResult(uint64_t key, uint32_t* read, bool* found);
    int32_t Write(const void* data, uint64_t offset, uint32_t size);
};
static_assert(offsetof(DYNAMICSTORAGE, m_directory) == 0x1c0, "DYNAMICSTORAGE layout");
static_assert(offsetof(DYNAMICSTORAGE, m_archives) == 0x1e0, "DYNAMICSTORAGE layout");
static_assert(offsetof(DYNAMICSTORAGE, m_frozen) == 0x240, "DYNAMICSTORAGE layout");
static_assert(sizeof(DYNAMICSTORAGE) == 0x260, "DYNAMICSTORAGE is 0x260 bytes");

bool ScanArchives(const char* directory, blz::allocator* allocator, uint8_t present[0x3ff], int32_t* count);

}

#endif
