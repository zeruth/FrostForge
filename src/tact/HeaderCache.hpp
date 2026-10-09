#ifndef TACT_HEADER_CACHE_HPP
#define TACT_HEADER_CACHE_HPP

// pkg/tact/3.6.39: the parsed encoding headers of recently read block-encoded files, by e-key
// (instance+0xa28, 0x50 bytes): a hash of entries kept in least-recently-used order, the
// oldest dropped past the capacity. Entries hand out a reference-counted holder of the header.

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace blz {
class allocator;
}

namespace tact {

struct ENCODINGHEADER;

// The holder (vtable 0x1428e6710, 0x28 bytes from the buffer allocator).
class HEADERCACHEVALUE {
    public:
    virtual ~HEADERCACHEVALUE();    // 0x00: FUN_1420db500
    virtual void Destroy();         // 0x08: FUN_1420db810

    volatile long m_refs = 0;       // 0x08
    uint8_t m_ekey[16];             // 0x10
    ENCODINGHEADER* m_header;       // 0x20

    void AddRef();
    void Release();
};

struct HEADERCACHELINK {
    HEADERCACHELINK* m_next;
    HEADERCACHELINK* m_prev;
};

struct HEADERCACHEENTRY {
    uint8_t m_ekey[16];             // 0x00
    HEADERCACHEVALUE* m_value;      // 0x10
    HEADERCACHELINK m_link;         // 0x18
};
static_assert(sizeof(HEADERCACHEENTRY) == 0x28, "a header cache entry is 0x28 bytes");

struct HEADERCACHE {
    SRWLOCK m_lock;                 // 0x00
    uint32_t m_capacity;            // 0x08
    uint32_t m_pad0c;
    HEADERCACHELINK m_recent;       // 0x10 most recent first
    uint32_t* m_buckets;            // 0x20
    HEADERCACHEENTRY* m_entries;    // 0x28
    uint32_t* m_next;               // 0x30
    blz::allocator* m_allocator;    // 0x38
    uint32_t m_bucketCount;         // 0x40
    uint32_t m_count;               // 0x44
    uint32_t m_entryCapacity;       // 0x48
    uint32_t m_pad4c;

    // FUN_1420db860: the holder for `ekey` (a new reference), or null.
    HEADERCACHEVALUE* Find(const uint8_t ekey[16]);
    // FUN_1420db550: caches `header` for `ekey` (a header already there wins; this one is
    // freed) and returns a new reference to the holder.
    HEADERCACHEVALUE* Insert(const uint8_t ekey[16], ENCODINGHEADER* header);
};
static_assert(sizeof(HEADERCACHE) == 0x50, "the header cache is 0x50 bytes");
static_assert(offsetof(HEADERCACHE, m_bucketCount) == 0x40, "HEADERCACHE layout");

}

#endif
