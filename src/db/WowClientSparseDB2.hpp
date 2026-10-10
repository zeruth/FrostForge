#ifndef DB_WOW_CLIENT_SPARSE_DB2_HPP
#define DB_WOW_CLIENT_SPARSE_DB2_HPP

// WoW/Source/DB/DBEngine/WowClientSparseDB2.cpp: the tables too large to keep whole
// (ConversationLine, ItemSparse, SceneScriptText, Spell). Records are read from the file on
// demand by async reads through a catalog of where each id lives, kept in a ring of recently
// used ids, and a local cache file holds the records hotfixes changed. Base vtable 0x1426b7a00;
// the per-table classes (SpellRec_C 0x1426b1768, ItemSparseRec_C 0x1426835c0) add the record
// callback map and a pool for their records.

#include <cstdint>

#include "base/ObjectAlloc.hpp"
#include "db/WowClientDB2.hpp"

// Where a record lives in the file (DB2CatalogLocation, 6 bytes): its offset and size.
#pragma pack(push, 1)
struct DB2CATALOGLOCATION {
    uint32_t m_offset;
    uint16_t m_size;
};
#pragma pack(pop)
static_assert(sizeof(DB2CATALOGLOCATION) == 6, "a catalog location is 6 bytes");

class WowClientSparseDB2 : public WowClientDB2_Base {
    public:
    // The async record reads in flight (a TSList of AsyncDBReadCallbackData, 0x1f0).
    void* m_readsNext;                              // 0x1f0
    void* m_readsPrev;                              // 0x1f8
    // The ids of the records kept, oldest first: a ring over m_ring (0x200).
    TDBCVector<int32_t> m_ring;                     // 0x200
    int32_t* m_ringHead;                            // 0x218
    int32_t* m_ringTail;                            // 0x220
    TDBCVector<DB2CATALOGLOCATION> m_catalog;       // 0x228
    WowDB2Lookup* m_catalogLookup;                  // 0x240 id -> catalog location
    CASFILE* m_file;                                // 0x248
    blz::chained_hash_header m_cacheEntries;        // 0x250 id -> DB2CatalogEntry in the cache file
    CASFILE* m_cacheReadFile;                       // 0x270
    void* m_cacheFile;                              // 0x278 the OS handle, -1 closed
    char m_cachePath[0x104];                        // 0x280
    uint8_t m_pad384[4];

    // ref: FUN_1405a2cc0
    explicit WowClientSparseDB2(const DBMeta* meta);
    // ref: FUN_1405a2e20
    ~WowClientSparseDB2() override;

    // ref: FUN_1405a2d90: frees the cache entry nodes and buckets.
    void ClearCacheEntries();

    // The table's load: the base load over the sparse slots (section bounds, the catalog,
    // records on demand), then the eviction ring and the cache file. Not ported: the base
    // loader cannot read these files (it expects their records whole), so the table reports
    // itself unloaded.
    // ref: FUN_1405a4550
    bool Load(bool errors) override;
};

static_assert(sizeof(WowClientDB2_Base) == 0x1f0, "WowClientDB2_Base is 0x1f0 bytes");
static_assert(offsetof(WowClientSparseDB2, m_catalog) == 0x228, "the catalog is at 0x228");
static_assert(offsetof(WowClientSparseDB2, m_cacheEntries) == 0x250, "the cache entries are at 0x250");
static_assert(offsetof(WowClientSparseDB2, m_cachePath) == 0x280, "the cache path is at 0x280");
static_assert(sizeof(WowClientSparseDB2) == 0x388, "WowClientSparseDB2 is 0x388 bytes");

// The typed sparse tables: the record callback map, then the pool their records come from.
class WowClientSparseDB2Table : public WowClientSparseDB2 {
    public:
    blz::chained_hash_header m_recordCallbacks;     // 0x388
    TObjectAllocHeap* m_recordPool;                 // 0x3a8 its records ("SpellRec_C", 0x200 a heap)

    // The AutoCode static initializers (FUN_1401d9950 Spell, FUN_1401d5190 ItemSparse, ...).
    WowClientSparseDB2Table(const DBMeta* meta, const char* recordName, uint32_t recordSize);
    ~WowClientSparseDB2Table() override;

    void RecordCallback() override;
};

#endif
