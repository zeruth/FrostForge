#ifndef TACT_PATCH_INDEX_HPP
#define TACT_PATCH_INDEX_HPP

// pkg/tact/3.6.39/src/patch: the patch index. A header, a list of blocks ({type, size}), then
// the blocks: 1 the name strings, 2 and 3 patch records, 5 and 6 file patch records (with
// their own strings). Records go into three maps keyed by the target e-key, and only when the
// container lacks the target but holds the source.

#include "blz/AllocString.hpp"
#include "blz/Allocator.hpp"
#include "tact/Common.hpp"
#include <cstddef>
#include <cstdint>

namespace tact {

class Instance;

// What the loader is handed (from DynamicInit::OnPatchIndexRead).
struct PATCHINDEXSOURCE {
    Instance* m_instance;
    uint8_t* m_data;
    uint32_t m_size;
    const char* m_258;
    uint64_t m_260;
    uint64_t m_268;
};

// The client's check for file patches: (context, instance, name, source key, record).
typedef bool (*PATCHFILTER)(void* context, Instance* instance, const char* name, const uint8_t* key, const void* record);

// A record map keyed by the first 16 bytes of each entry (0x2c bytes; murmur3 over 9 bytes).
template <uint32_t SIZE>
struct PATCHMAP {
    struct ENTRY {
        uint8_t m_bytes[SIZE];
    };

    uint32_t* m_buckets;            // 0x00
    ENTRY* m_entries;               // 0x08
    uint32_t* m_next;               // 0x10
    blz::allocator* m_allocator;    // 0x18
    uint32_t m_bucketCount;         // 0x20
    uint32_t m_count;               // 0x24
    uint32_t m_capacity;            // 0x28

    void Init(blz::allocator* allocator);
    ENTRY* Find(const uint8_t key[16], uint32_t slot);
    ENTRY* Append(uint32_t slot);
    void Rehash(uint32_t buckets);
    void Destroy();
};

struct PATCHCURSOR {
    const uint8_t* m_begin;
    const uint8_t* m_cur;
    uint32_t m_offset;              // the block being read
    uint32_t m_size;
};

// 0x170 bytes, at instance+0x1028.
struct PATCHINDEX {
    PATCHCURSOR* m_cursor;              // 0x000: only while loading
    Instance* m_instance;               // 0x008
    PATCHFILTER m_filter;               // 0x010
    void* m_filterContext;              // 0x018
    uint32_t m_headerSize;              // 0x020
    uint32_t m_version;                 // 0x024
    uint32_t m_28;                      // 0x028
    uint8_t m_key[16];                  // 0x02c
    blz::alloc_string m_extra;          // 0x040
    blz::alloc_string m_names;          // 0x060: block 1
    blz::alloc_string m_fileNames;      // 0x080: block 6
    blz::alloc_string m_paths;          // 0x0a0: block 5
    Array<uint64_t> m_blocks;           // 0x0c0: {type, size}
    uint8_t m_patchKeySize;             // 0x0d8: block 3
    uint8_t m_entryKeySize;             // 0x0d9: block 2
    uint8_t m_fileKeySize;              // 0x0da: blocks 5 and 6
    PATCHMAP<0x30> m_patches;           // 0x0e0: block 3
    PATCHMAP<0x50> m_entries;           // 0x110: block 2
    PATCHMAP<0x58> m_files;             // 0x140: blocks 5 and 6

    void Init();
    void Destroy();
    uint8_t Load(PATCHINDEXSOURCE* source);
    void ReadHeader();
    void ReadEntries(uint32_t offset);
    void ReadPatches(uint32_t offset);
    void ReadPaths(uint32_t offset);
    void ReadFiles(uint32_t offset);
};
static_assert(offsetof(PATCHINDEX, m_extra) == 0x40, "PATCHINDEX layout");
static_assert(offsetof(PATCHINDEX, m_blocks) == 0xc0, "PATCHINDEX layout");
static_assert(offsetof(PATCHINDEX, m_patches) == 0xe0, "PATCHINDEX layout");
static_assert(offsetof(PATCHINDEX, m_entries) == 0x110, "PATCHINDEX layout");
static_assert(offsetof(PATCHINDEX, m_files) == 0x140, "PATCHINDEX layout");
static_assert(sizeof(PATCHINDEX) == 0x170, "PATCHINDEX is 0x170 bytes");

}

#endif
