#ifndef BLZ_OBJECT_POOL_HPP
#define BLZ_OBJECT_POOL_HPP

#include "blz/Allocator.hpp"
#include <cstdint>

namespace blz {

// A fixed-size object pool (0x98 bytes): pages of `m_elementsPerPage` elements, each page
// ending in a POOLPAGE header. Pages with free slots sit on the partial list, wholly free ones
// on the free list (at most `m_maxFreePages` are kept); every page is findable on free through
// the recent-page cache or the sorted page array.
struct POOLPAGE {
    POOLPAGE* m_next;      // 0x00
    POOLPAGE* m_prev;      // 0x08
    void* m_freeList;      // 0x10
    uint32_t m_bytes;      // 0x18: elements * element size
    uint32_t m_elementSize;// 0x1c
    uint32_t m_freeCount;  // 0x20
    uint8_t m_tracked;     // 0x24: in the sorted array
    void* m_base;          // 0x28
};
static_assert(sizeof(POOLPAGE) == 0x30, "POOLPAGE is 0x30 bytes");

struct object_pool {
    allocator* m_allocator;         // 0x00
    uint32_t m_elementSize;         // 0x08
    uint32_t m_elementsPerPage;     // 0x0c
    uint32_t m_alignment;           // 0x10
    POOLPAGE* m_partial[2];         // 0x18: list head (next, prev)
    POOLPAGE* m_free[2];            // 0x28: list head (next, prev)
    void* m_recent[8];              // 0x38: page bases not yet in the sorted array
    void** m_sorted;                // 0x78
    uint32_t m_sortedCount;         // 0x80
    uint32_t m_sortedCapacity;      // 0x84
    int32_t m_allocated;            // 0x88
    int32_t m_freeSlots;            // 0x8c
    uint32_t m_freePages;           // 0x90
    uint32_t m_maxFreePages;        // 0x94

    void Init(allocator* allocator, uint32_t elementSize, uint32_t alignment, uint32_t elementsPerPage,
        uint32_t maxFreePages);
    void Destroy();
    void* Allocate();
    void Free(void* object);

    POOLPAGE* NewPage();
    POOLPAGE* ReturnToPage(void* object);
    void Compact();

    POOLPAGE* PageOf(void* base) {
        return reinterpret_cast<POOLPAGE*>(static_cast<uint8_t*>(base) + this->m_elementSize * this->m_elementsPerPage);
    }
};
static_assert(sizeof(object_pool) == 0x98, "object_pool is 0x98 bytes");

}

#endif
