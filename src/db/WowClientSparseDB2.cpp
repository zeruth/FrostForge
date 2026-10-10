#include "db/WowClientSparseDB2.hpp"

#include <cstring>
#include <new>

#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

namespace {

const char s_catalogNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBHUDB2CatalogEntry@@@blz@@@blz@@";
const char s_catalogBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBHUDB2CatalogEntry@@@blz@@@blz@@";

// The reads list's links (TSList: a node's first word is its next, the second its previous).
struct READLINK {
    READLINK* m_next;
    READLINK* m_prev;
};

}

// ref: FUN_1405a2cc0
WowClientSparseDB2::WowClientSparseDB2(const DBMeta* meta) : WowClientDB2_Base(meta) {
    this->m_readsNext = &this->m_readsNext;
    this->m_readsPrev = &this->m_readsNext;
    this->m_ring.resize(9);
    this->m_ringTail = this->m_ring.m_data;
    this->m_ringHead = this->m_ring.m_data;
    this->m_catalogLookup = nullptr;
    this->m_file = nullptr;
    this->m_cacheEntries = blz::chained_hash_header();
    this->m_cacheReadFile = nullptr;
    this->m_cacheFile = reinterpret_cast<void*>(~uintptr_t(0));
    std::memset(this->m_cachePath, 0, sizeof(this->m_cachePath));
    std::memset(this->m_pad384, 0, sizeof(this->m_pad384));
}

// ref: FUN_1405a2e20
WowClientSparseDB2::~WowClientSparseDB2() {
    this->ClearCacheEntries();
    // The catalog and the ring go with their vectors. Reads still listed are unlinked, not freed:
    // the async layer owns them.
    auto sentinel = reinterpret_cast<READLINK*>(&this->m_readsNext);
    auto node = static_cast<READLINK*>(this->m_readsNext);
    while (node != sentinel && node != nullptr) {
        READLINK* next = node->m_next;
        node->m_prev = nullptr;
        node->m_next = nullptr;
        node = next;
    }
    this->m_readsPrev = nullptr;
    this->m_readsNext = nullptr;
}

// ref: FUN_1405a2d90
void WowClientSparseDB2::ClearCacheEntries() {
    struct NODE {
        NODE* m_next;
    };
    if (this->m_cacheEntries.m_buckets == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < this->m_cacheEntries.m_bucketCount; i++) {
        auto node = static_cast<NODE*>(this->m_cacheEntries.m_buckets[i]);
        while (node != nullptr) {
            NODE* next = node->m_next;
            SMemFree(node, s_catalogNodeTag, -6, 0);
            node = next;
        }
    }
    SMemFree(this->m_cacheEntries.m_buckets, s_catalogBucketTag, -6, 0);
    this->m_cacheEntries.m_buckets = nullptr;
}

// ref: FUN_1405a4550
bool WowClientSparseDB2::Load(bool errors) {
    (void)errors;
    // FUN_14057e200 over the sparse slots (0x58 FUN_1405a46d0, 0xb8 FUN_1405a4040 the catalog,
    // 0x120/0x128 the section bounds, 0xf0/0xf8 records), then the field offsets, the ring
    // (FUN_1405a58a0) and the cache file (FUN_1402cff40)
    FROST_UNIMPLEMENTED();
    return false;
}

// The AutoCode initializer's tail: a pool of 0x200 records a heap, Storm category 0x24.
WowClientSparseDB2Table::WowClientSparseDB2Table(const DBMeta* meta, const char* recordName, uint32_t recordSize)
    : WowClientSparseDB2(meta) {
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x24, __FILE__, SStrHashFnv1a(__FILE__), __LINE__, 0);
    this->m_recordPool = new (memory) TObjectAllocHeap(recordName, recordSize, 0x200, 0x24);
}

// The AutoCode atexit handler (FUN_1425e0d10 for Spell).
WowClientSparseDB2Table::~WowClientSparseDB2Table() {
    if (this->m_recordPool != nullptr) {
        this->m_recordPool->~TObjectAllocHeap();
        SMemFree(this->m_recordPool, __FILE__, __LINE__, 0);
        this->m_recordPool = nullptr;
    }
}

// The typed tables' record callback (FUN_140442370 for the plain ones) is empty.
void WowClientSparseDB2Table::RecordCallback() {
}
