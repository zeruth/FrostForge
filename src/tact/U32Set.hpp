#ifndef TACT_U32_SET_HPP
#define TACT_U32_SET_HPP

// tact's chained hash set of 32-bit values (0x2c bytes): the blz id map layout with 4-byte
// entries and murmur3's 32-bit finalizer as the hash. The decoders collect the indices of
// corrupt blocks in one.

#include "blz/Allocator.hpp"
#include <cstdint>

namespace tact {

struct U32SET {
    uint32_t* m_buckets;            // 0x00
    uint32_t* m_entries;            // 0x08
    uint32_t* m_next;               // 0x10
    blz::allocator* m_allocator;    // 0x18
    uint32_t m_bucketCount;         // 0x20
    uint32_t m_count;               // 0x24
    uint32_t m_capacity;            // 0x28

    static uint32_t Hash(uint32_t value);

    void Init();
    void Destroy();
    void Allocate(uint32_t buckets, uint32_t capacity);
    void Assign(U32SET* source);
    uint32_t* Append(uint32_t slot);
    void Rehash(uint32_t buckets);
    bool Insert(uint32_t value, uint32_t** entry);
    bool Contains(uint32_t value) const;
};

}

#endif
