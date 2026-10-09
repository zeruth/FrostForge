#ifndef CASC_INDEX_TABLES_HPP
#define CASC_INDEX_TABLES_HPP

// pkg/casc/1.4.41/src/dynamic/index_tables.cpp: the sixteen index files of a container
// ("0<bucket><version>.idx"), which version of each is current, and its open file.

#include "blz/Allocator.hpp"
#include "casc/KeyMappingTable.hpp"
#include "os/Os.hpp"
#include <cstdint>

namespace casc {

struct SHMEMBINDING;

// 0x118 bytes, at container index +0xb8.
struct INDEXTABLES {
    blz::allocator* m_allocator;    // 0x00
    const char* m_path;             // 0x08
    int32_t m_mode;                 // 0x10
    struct TABLE {
        uint32_t m_version;
        uint32_t m_pad;
        OSFILE m_file;
    } m_tables[16];                 // 0x18

    void Init(blz::allocator* allocator);
    void Destroy();
    int32_t Bind(const char* path, int32_t mode, SHMEMBINDING* binding);
    int32_t Open(uint32_t bucket, uint32_t version, bool existing);
    int32_t Scan();
    int32_t Refresh(uint32_t bucket, uint32_t version, KEYMAPPINGTABLE* table);
    int32_t Flush(uint32_t bucket, KEYMAPPINGTABLE* table, uint32_t* version, KMTRECORD* records, uint32_t count);
    OSFILE* File(uint32_t bucket);
};
static_assert(sizeof(INDEXTABLES) == 0x118, "INDEXTABLES is 0x118 bytes");

}

#endif
