#include "base/RCString.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace {

// One interned string: the bucket chain link, then the text. Entries are 8-byte aligned.
struct RCStringEntry {
    RCStringEntry* m_next;
    char m_text[1];
};

// The 64 KiB (or larger) blocks entries are carved from: TSGrowableArray<void*, 8> of the
// blocks, then the free range of the newest one.
struct RCStringBlocks {
    uint32_t m_count;
    void** m_data;
    uint32_t m_capacity;
    uint32_t m_pad14;
    uint32_t m_granularity;
    uint32_t m_pad1C;
    char* m_cursor;
    char* m_end;
};

constexpr uint32_t BUCKET_COUNT = 0x209;

struct RCStringPool {
    RCStringEntry* m_buckets[BUCKET_COUNT];
    RCStringBlocks m_blocks;
};

static_assert(sizeof(RCStringBlocks) == 0x30, "RCStringBlocks layout");
static_assert(offsetof(RCStringPool, m_blocks) == 0x1048, "RCStringPool layout");
static_assert(sizeof(RCStringPool) == 0x1078, "RCStringPool layout");

// DAT_142e20850
RCStringPool* s_pool;

// Starts a new block of at least `bytes` (64 KiB minimum) and makes it current; the old
// block's unused tail is abandoned. The block array grows by TSGrowableArray's rule, with a
// granularity of 32.
// ref: FUN_1402cb0d0
void RCStringBlocksReserve(RCStringBlocks* blocks, uint64_t bytes) {
    uint32_t index = blocks->m_count;
    uint64_t size = bytes > 0x10000 ? bytes : 0x10000;

    uint32_t count = index + 1;
    if (index < count && blocks->m_capacity < count) {
        uint32_t granularity = blocks->m_granularity;
        if (granularity == 0) {
            if (count < 0x20) {
                granularity = count;
                for (uint32_t low = count & index; low != 0; low = (low - 1) & low) {
                    granularity = low;
                }
                if (granularity == 0) {
                    granularity = 1;
                }
            } else {
                blocks->m_granularity = 0x20;
                granularity = 0x20;
            }
        }
        uint32_t capacity = count;
        if (count % granularity != 0) {
            capacity = (granularity - count % granularity) + count;
        }

        void** old = blocks->m_data;
        blocks->m_data = nullptr;
        blocks->m_capacity = capacity;
        if (capacity != 0) {
            // The reference tags this allocation with its own return address.
            blocks->m_data = static_cast<void**>(SMemAlloc(uint64_t(capacity) * 8, 8,
                static_cast<const char*>(_ReturnAddress()), 0, -7, 0));
        }
        if (old) {
            uint32_t keep = blocks->m_count <= capacity ? blocks->m_count : capacity;
            for (uint32_t i = 0; i < keep; i++) {
                blocks->m_data[i] = old[i];
            }
            SMemFree(old, ".?AV?$TSGrowableArray@PEAX$07@@", -2, 0);
        }
    }
    blocks->m_count = count;

    blocks->m_data[index] = SMemAlloc(size, 1, __FILE__, SStrHashFnv1a(__FILE__), 0x26, 0);
    blocks->m_cursor = static_cast<char*>(blocks->m_data[index]);
    blocks->m_end = blocks->m_cursor + size;
}

// TSFixedArray<void*, 8> teardown: every block, then the array.
// ref: FUN_1402caec0
void RCStringBlocksFree(RCStringBlocks* blocks) {
    for (uint32_t i = 0; i < blocks->m_count; i++) {
        SMemFree(blocks->m_data[i], __FILE__, 0x1C, 0);
    }
    if (blocks->m_data) {
        SMemFree(blocks->m_data, ".?AV?$TSFixedArray@PEAX$07@@", -2, 0);
    }
}

}

// ref: FUN_1402caf30
const char* RCString::Intern(const char* str) {
    if (!s_pool) {
        auto pool = static_cast<RCStringPool*>(SMemAlloc(sizeof(RCStringPool), 1, __FILE__, SStrHashFnv1a(__FILE__), 0x41, 0));
        pool->m_blocks.m_count = 0;
        pool->m_blocks.m_data = nullptr;
        pool->m_blocks.m_capacity = 0;
        pool->m_blocks.m_granularity = 0;
        RCStringBlocksReserve(&pool->m_blocks, 0);
        std::memset(pool->m_buckets, 0, sizeof(pool->m_buckets));
        s_pool = pool;
    }

    RCStringPool* pool = s_pool;
    RCStringEntry** bucket = &pool->m_buckets[SStrHashHT(str) % BUCKET_COUNT];
    for (RCStringEntry* entry = *bucket; entry; entry = entry->m_next) {
        if (std::strcmp(str, entry->m_text) == 0) {
            return entry->m_text;
        }
    }

    uint64_t length = std::strlen(str);
    uint64_t bytes = (length + 0x10) & ~uint64_t(7);
    if (static_cast<uint64_t>(pool->m_blocks.m_end - pool->m_blocks.m_cursor) < bytes) {
        RCStringBlocksReserve(&pool->m_blocks, bytes);
    }
    auto entry = reinterpret_cast<RCStringEntry*>(pool->m_blocks.m_cursor);
    std::memcpy(entry->m_text, str, length + 1);
    pool->m_blocks.m_cursor += bytes;
    entry->m_next = *bucket;
    *bucket = entry;
    return entry->m_text;
}

// ref: FUN_1402cb270
void RCStringDestroyPool() {
    if (RCStringPool* pool = s_pool) {
        RCStringBlocksFree(&pool->m_blocks);
        ::operator delete(pool, sizeof(RCStringPool));
    }
    s_pool = nullptr;
}

// ref: FUN_1402c8f50
void RCStringShutdown() {
    RCStringDestroyPool();
}

// ref: FUN_1402cb2b0
bool RCStringPoolIsEmpty() {
    return s_pool == nullptr;
}
