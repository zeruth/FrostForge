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

    // The base slots the sparse class replaces (vtable 0x1426b7a00): records come from the file
    // on demand through a catalog, so the sections, the record store and the parent links are
    // its own. Each runs the reference's code, translated, over this object (the layout is
    // the reference's). Until 2026-10-21 only Load was overridden, and the base loader read a
    // sparse file as a dense one (17 MB in one read for Spell, refused).
    void Slot08() override;                                                             // 0x08 FUN_1405a48c0
    void Slot10() override;                                                             // 0x10 FUN_1405a4980
    bool Slot18() override;                                                             // 0x18 FUN_1405a3470
    void Unload(bool failed) override;                                                  // 0x28 FUN_1405a4e30
    void Slot40() override;                                                             // 0x40 FUN_1405a3550
    bool LoadSection(void* file, int32_t section, bool errors) override;                // 0x58 FUN_1405a46d0
    bool ValidateMeta() override;                                                       // 0x90 FUN_1405a48b0
    CASFILE** FileSlot() override;                                                      // 0x98 FUN_1405a37f0
    bool ReadSectionExtra(CDataStore* store, bool errors, uint32_t section) override;   // 0xb8 FUN_1405a4040
    uint32_t SparseCount() override;                                                    // 0xc8 FUN_1405a3cd0
    void* DeserializeRecord(CDataStore* store) override;                                // 0xf0 FUN_1405a4f50
    void SetRecord(uint32_t id, void* record, void* previous, int32_t parent) override; // 0xf8 FUN_1405a49c0
    void UpdateParent(void* record, int32_t parent, uint32_t id, void* previous) override;  // 0x108 FUN_1405a4a40
    uint32_t SectionSize(uint32_t section) override;                                    // 0x120 FUN_1405a3c80
    uint32_t SectionOffset(uint32_t section) override;                                  // 0x128 FUN_1405a3c60
    // and five the reference fills with trivial bodies: the file stays open (records are read
    // from it on demand), so closing does nothing and opening returns the open one; there is no
    // record block or copy table to take
    void CloseFile(CASFILE* file) override;                                             // 0xa0 (nothing)
    bool CopySection(CDataStore* store, bool errors, uint32_t section) override;        // 0xb0 (true)
    bool ReadCopyTable(CDataStore* store, bool errors, uint32_t section,
        TDBCVector<DB2DUPLICATE>* duplicates) override;                                 // 0xc0 (true)
    CASFILE* OpenFile(bool errors) override;                                            // 0x130 FUN_14036b130
    void CloseFile2(CASFILE* file) override;                                            // 0x138 (nothing)

    // The two slots each table's class adds (pure in the base vtable 0x1426b7a00): a record
    // made from the table's pool (0x140), and how many records the ring keeps (0x148, asked by
    // the load with the record count and size). Missing until 2026-10-21, so the load called
    // past the end of this vtable.
    virtual void* NewRecord() = 0;                                          // 0x140
    virtual uint32_t MaxCachedRecords(uint32_t count, uint32_t size) = 0;   // 0x148
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

    // Each table's own pair (its class's vtable: Spell 0x1426b1768, ItemSparse 0x1426835c0,
    // ConversationLine 0x1426846f8, SceneScriptText 0x1426aacd8), by which table this is.
    void* NewRecord() override;
    uint32_t MaxCachedRecords(uint32_t count, uint32_t size) override;
    uint64_t RecordId(void* record) override;     // 0x78
    void FreeRecord(void* record) override;       // 0x80

    void RecordCallback() override;
    // The record callbacks on the map at 0x388: 0xd8 and 0xe8 shared (FUN_140384c40,
    // FUN_140384df0), 0xd0 and 0xe0 each table's own, translated.
    bool RunRecordCallbacks(int32_t id, const void* record, uint32_t event) override;
    bool HasRecordCallback(int32_t id, uint64_t key) override;
    bool RemoveRecordCallback(int32_t id, uint64_t key, bool all) override;
    void* RemoveRecordCallbacks(int32_t id, bool all) override;
};

#endif
