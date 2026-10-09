#ifndef TACT_ENCODING_TABLE_HPP
#define TACT_ENCODING_TABLE_HPP

// pkg/tact/3.6.39/src/encoding_table/encoding_table_reader.cpp: the encoding file ("EN"):
// a header, the e-spec strings, then two paged tables - c-key to e-keys, e-key to e-spec -
// each with a page index of (first key, MD5 of the page).

#include "blz/Allocator.hpp"
#include "tact/Common.hpp"
#include <cstddef>
#include <cstdint>

namespace tact {

// 0x70 bytes, at instance+0xd90.
struct ENCODINGTABLE {
    blz::allocator* m_allocator;        // 0x00
    uint8_t* m_data;                    // 0x08
    uint32_t m_size;                    // 0x10
    uint8_t m_cKeySize;                 // 0x14
    uint8_t m_eKeySize;                 // 0x15
    uint32_t m_cPageCount;              // 0x18
    uint32_t m_cPageSize;               // 0x1c
    uint32_t m_ePageCount;              // 0x20
    uint32_t m_ePageSize;               // 0x24
    Array<const char*> m_eSpecs;        // 0x28
    uint8_t m_valid;                    // 0x40
    const uint8_t* m_cIndex;            // 0x48
    const uint8_t* m_cPages;            // 0x50
    const uint8_t* m_eIndex;            // 0x58
    const uint8_t* m_ePages;            // 0x60
    uint64_t m_68;                      // 0x68

    void Init(blz::allocator* allocator);
    void Destroy();
    uint8_t* Allocate(uint32_t size);
    void Clear();
    uint8_t Parse(uint64_t a2);
    uint8_t FindESpec(const uint8_t* ekey, uint8_t keySize, const char** eSpec);
    uint8_t FindEKey(const uint8_t* ekey, uint8_t keySize, const uint8_t** entry);
};
// The page an index of (first key, page hash) entries puts a key in.
uint32_t EncodingPageSearch(const uint8_t* index, uint32_t count, uint8_t keySize, const uint8_t* key, uint8_t compare);

static_assert(offsetof(ENCODINGTABLE, m_eSpecs) == 0x28, "ENCODINGTABLE layout");
static_assert(offsetof(ENCODINGTABLE, m_cIndex) == 0x48, "ENCODINGTABLE layout");
static_assert(sizeof(ENCODINGTABLE) == 0x70, "ENCODINGTABLE is 0x70 bytes");

}

#endif
