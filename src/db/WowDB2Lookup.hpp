#ifndef DB_WOW_DB2_LOOKUP_HPP
#define DB_WOW_DB2_LOOKUP_HPP

// WoW/Source/DB/WowDB2Lookup: the id -> record index every client table keeps. A loaded table
// hands the lookup its record blocks (the file's records, and the patched records after them),
// the id range and the counts; WowDB2LookupCreate (FUN_1405a07a0) asks each storage strategy
// what it would cost and builds the cheapest. The strategies:
//
//   Null  records only through the overflow list
//   Vec   a pointer per id in [min, max]
//   IX8/16/32   a record index per id in [min, max]; ~1 marks an empty id, ~0 one that lives in
//               the overflow list
//   PI    a sorted array of (id - min, record index) pairs, at three id and three index widths
//   Spar  sparse tables (WowClientSparseDB2), M** the multi-id maps: never asked for by the
//         loader's strategy mask, so not ported
//
// Every strategy also keeps the overflow list: (id, record) pairs sorted by id for records
// that were patched in after the load. A table whose meta names a record order (meta 0xa0)
// gets a sorted wrapper of its strategy, which iterates in that order instead of by id.

#include <cstdint>

#include "db/DBCTypes.hpp"

// What a table hands its lookup (built by WowClientDB2_Base's FinalizeLoad, FUN_1405814f0).
struct DB2LOOKUPINIT {
    uint8_t* m_records;             // 0x00 the file's records
    uint8_t* m_records2;            // 0x08 the patched records
    void* m_deleted;                // 0x10 the table's deleted-record marker
    const char* m_name;             // 0x18
    bool (*m_order)(const void* const*, const void* const*);  // 0x20 meta 0xa0
    uint32_t m_finishFlags;         // 0x28 bit 1: finish when the last finish lock goes
    int32_t m_finishLocks;          // 0x2c
    uint32_t m_stride;              // 0x30 a file record's size
    uint32_t m_stride2;             // 0x34 a patched record's size
    uint32_t m_minId;               // 0x38
    uint32_t m_maxId;               // 0x3c
    uint32_t m_recordCount;         // 0x40
    uint32_t m_indexCount;          // 0x44
    uint32_t m_copyCount;           // 0x48
    uint32_t m_recordCount2;        // 0x4c
    uint32_t m_sparseCount;         // 0x50
    uint16_t m_idOffsetBits;        // 0x54 the id field's bits, when the id is packed
    uint16_t m_idSizeBits;          // 0x56
    uint8_t m_idPacked;             // 0x58 bit 0
    uint8_t m_pad59[7];
};
static_assert(sizeof(DB2LOOKUPINIT) == 0x60, "a lookup's init block is 0x60 bytes");

// An iteration position: the index, and the record it lands on. -1 before the first,
// -2 past the end.
struct DB2LOOKUPITER {
    int32_t m_index;
    int32_t m_pad;
    void* m_record;
};

// An overflow entry.
struct DB2LOOKUPENTRY {
    uint32_t m_id;
    uint32_t m_pad;
    void* m_record;
};

// A sorted wrapper's entry: the record first, so the meta's order compares entries directly.
struct DB2LOOKUPSORTED {
    void* m_record;
    uint32_t m_id;
    uint32_t m_pad;
};

class WowDB2Lookup {
    public:
    virtual ~WowDB2Lookup();                                        // 0x00
    virtual void* Find(uint32_t id) = 0;                            // 0x08
    virtual void SetIndex(uint32_t id, uint32_t index) = 0;         // 0x10
    virtual void ApplyCopies(const uint32_t* pairs) = 0;            // 0x18
    virtual bool SetIndex2(uint32_t id, uint32_t index) = 0;        // 0x20
    virtual void Finish();                                          // 0x28
    virtual void SetRecord(uint32_t id, void* record) = 0;          // 0x30
    virtual bool ClearRecord(uint32_t id, void* record) = 0;        // 0x38
    virtual bool IsEmpty() = 0;                                     // 0x40
    virtual WowDB2Lookup* Clone() = 0;                              // 0x48
    virtual void Next(DB2LOOKUPITER* it) = 0;                       // 0x50
    virtual void* Record(const DB2LOOKUPITER* it) = 0;              // 0x58
    virtual uint32_t Id(const DB2LOOKUPITER* it) = 0;               // 0x60
    virtual uint32_t OverflowCount() = 0;                           // 0x68
    virtual uint64_t MemoryUsed() = 0;                              // 0x70
    virtual void Init(const DB2LOOKUPINIT* init) = 0;               // 0x78
    virtual void Rebuild();                                         // 0x80
    virtual uint64_t Count() = 0;                                   // 0x88

    DB2LOOKUPINIT m_init;           // 0x08
    int32_t m_lookups = 0;          // 0x68
    int32_t m_repeats = 0;          // 0x6c
    uint32_t m_lastId = 0xFFFFFFFF; // 0x70
    uint32_t m_74 = 0;
    uint8_t m_78 = 0;
    uint8_t m_dirty = 0;            // 0x79 a rebuild is owed when the last lock goes
    uint8_t m_pad7a[2];
    int32_t m_locks = 0;            // 0x7c

    WowDB2Lookup();
    // The copy every strategy's copy constructor starts with (FUN_14059f060's head).
    WowDB2Lookup(const WowDB2Lookup& other);

    // The lookup counters every Find bumps.
    void CountLookup(uint32_t id);
    // A record by its index: the file's records first, the patched ones after.
    void* RecordAt(uint32_t index);
    // After a change: rebuild now, or when the last lock goes.
    void Changed();
};

// ---- the parent lookups ------------------------------------------------------------------

// A table with a parent column keeps its records grouped by parent: (parent - min, record
// index) pairs sorted by parent, a map from a parent to its first pair, and an overflow list of
// patched (parent, record) pairs. FUN_1405a06f0 builds the cheapest of six widths.

struct DB2PARENTINIT {
    uint32_t m_count;               // 0x00
    uint32_t m_stride;              // 0x04
    int32_t m_minParent;            // 0x08
    int32_t m_maxParent;            // 0x0c
    uint8_t* m_records;             // 0x10
    bool (*m_order)(const void* left, const void* right);  // 0x18 meta 0x98
    uint8_t m_sparse;               // 0x20 indices are record pointers
    uint8_t m_pad21[7];
};

// A position among one parent's records: its first pair, its first overflow entry, and which
// of the two comes next (0 the pairs, 1 the overflow; all ~0 at the end).
struct DB2PARENTITER {
    uint32_t m_entry;
    uint32_t m_overflow;
    uint32_t m_parent;
    uint32_t m_source;
};

struct DB2PARENTOVERFLOW {
    uint32_t m_parent;
    uint32_t m_pad;
    void* m_record;
};

class WowDB2ParentLookup {
    public:
    virtual ~WowDB2ParentLookup();                                              // 0x00
    virtual void Init(const DB2PARENTINIT* init) = 0;                           // 0x08
    virtual void Add(int32_t parent, uint32_t index) = 0;                       // 0x10
    virtual void Build() = 0;                                                   // 0x18
    virtual bool Update(void* previous, void* record, int32_t parent, uint32_t index, bool clear) = 0;  // 0x20
    virtual void SortOverflow() = 0;                                            // 0x28
    virtual void* Record(const DB2PARENTITER* it) = 0;                          // 0x30
    virtual DB2PARENTITER* Begin(DB2PARENTITER* it, int32_t parent) = 0;        // 0x38
    virtual void Next(DB2PARENTITER* it) = 0;                                   // 0x40
    virtual DB2PARENTITER* End(DB2PARENTITER* it);                              // 0x48

    DB2PARENTINIT m_init;           // 0x08
    uint8_t m_flags = 0;            // 0x30 1 built, 2 the overflow needs sorting
    uint8_t m_pad31[3];
    int32_t m_locks = 0;            // 0x34
};

// FUN_1405a06f0
WowDB2ParentLookup* WowDB2ParentLookupCreate(const DB2PARENTINIT* init);

// FUN_1405a0640 / FUN_1405a0670: the first position, and the end.
DB2LOOKUPITER* WowDB2LookupBegin(WowDB2Lookup* lookup, DB2LOOKUPITER* it);
DB2LOOKUPITER* WowDB2LookupEnd(WowDB2Lookup* lookup, DB2LOOKUPITER* it);

// FUN_1405a0690 / FUN_1405a06b0: the finish locks.
void WowDB2LookupLockFinish(WowDB2Lookup* lookup);
void WowDB2LookupUnlockFinish(WowDB2Lookup* lookup);

// FUN_1405a06a0 / FUN_1405a06d0: the rebuild locks.
void WowDB2LookupLock(WowDB2Lookup* lookup);
void WowDB2LookupUnlock(WowDB2Lookup* lookup);

// FUN_1405a07a0: builds the cheapest strategy `mask` allows for `init`.
WowDB2Lookup* WowDB2LookupCreate(uint32_t mask, const DB2LOOKUPINIT* init);

#endif
