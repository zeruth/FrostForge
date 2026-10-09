#ifndef CASC_ARCHIVE_FILE_CACHE_HPP
#define CASC_ARCHIVE_FILE_CACHE_HPP

// pkg/casc/1.4.41/src/archive_file_cache.cpp and dynamic_archive_cache.cpp: the open
// data.### files, shared by reference count and kept in a capacity-bounded LRU keyed by
// (archive, kind). Kind 0 is a buffered synchronous file, 1 unbuffered, 2 asynchronous
// (opened through aio).

#include "aio/Aio.hpp"
#include "blz/Allocator.hpp"
#include "os/Os.hpp"
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace casc {

struct LISTLINK {
    LISTLINK* m_next;
    LISTLINK* m_prev;
};

struct ARCHIVEKEY {
    uint64_t m_archive;     // 0x00
    uint8_t m_kind;         // 0x08
    uint8_t m_pad[7];
};

// An open archive file (0x50 bytes, vtable 0x1428f4f90).
class ARCHIVEFILE {
    public:
    volatile int32_t m_refs;        // 0x08
    blz::allocator* m_allocator;    // 0x10
    ARCHIVEKEY m_key;               // 0x18
    uint8_t m_28;                   // 0x28
    OSFILE m_file;                  // 0x30: kinds 0 and 1
    OSFILE m_asyncFile;             // 0x38: kind 2
    LISTLINK m_link;                // 0x40: in the cache's LRU

    virtual ~ARCHIVEFILE();
    virtual void Destroy();

    void AddRef();
    void Release();
};
static_assert(sizeof(ARCHIVEFILE) == 0x50, "ARCHIVEFILE is 0x50 bytes");

// The cache's map from key to file (0x2c bytes; entries of 0x18 hold a reference).
struct ARCHIVEMAP {
    struct ENTRY {
        ARCHIVEKEY m_key;
        ARCHIVEFILE* m_file;
    };

    uint32_t* m_buckets;            // 0x00
    ENTRY* m_entries;               // 0x08
    uint32_t* m_next;               // 0x10
    blz::allocator* m_allocator;    // 0x18
    uint32_t m_bucketCount;         // 0x20
    uint32_t m_count;               // 0x24
    uint32_t m_capacity;            // 0x28

    static uint32_t Hash(const ARCHIVEKEY* key);
    uint32_t IndexOf(const ARCHIVEKEY* key, uint32_t slot);
    ENTRY* Find(const ARCHIVEKEY* key);
    void FindLink(const ARCHIVEKEY* key, uint32_t* index, uint32_t** link);
    ENTRY* Insert(const ARCHIVEKEY* key, ARCHIVEFILE** file, bool* inserted);
    bool Remove(const ARCHIVEKEY* key);
    void Rehash(uint32_t buckets);
    void Reserve(uint32_t capacity);
    void Clear();
};

// vtable 0x1428f3588 (0x58 bytes).
class ARCHIVEFILECACHE {
    public:
    uint64_t m_capacity;            // 0x08
    LISTLINK m_lru;                 // 0x10: most recently used first
    ARCHIVEMAP m_map;               // 0x20
    uint8_t m_exclusiveRead;        // 0x50: read-only opens deny other writers

    ARCHIVEFILECACHE(blz::allocator* allocator, uint64_t capacity);
    virtual ~ARCHIVEFILECACHE();

    ARCHIVEFILE* Find(uint64_t archive, uint8_t kind);
    ARCHIVEFILE* Open(uint64_t archive, const char* path, int32_t mode, uint8_t kind, aio::CONTEXT* aio, void* a8,
        int32_t* error);
    void Clear();
};

// vtable 0x1428f3678 (0x60 bytes): the cache behind a lock, naming the files itself.
class DYNAMICARCHIVECACHE : public ARCHIVEFILECACHE {
    public:
    SRWLOCK m_lock;                 // 0x58

    DYNAMICARCHIVECACHE(blz::allocator* allocator, uint64_t capacity);
    ~DYNAMICARCHIVECACHE() override;

    ARCHIVEFILE* Open(uint64_t archive, const char* directory, int32_t mode, int32_t* error, uint8_t kind,
        aio::CONTEXT* aio, void* a9);
    void Clear();
};
static_assert(sizeof(DYNAMICARCHIVECACHE) == 0x60, "DYNAMICARCHIVECACHE is 0x60 bytes");

}

#endif
