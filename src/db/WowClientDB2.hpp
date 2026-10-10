#ifndef DB_WOW_CLIENT_DB2_HPP
#define DB_WOW_CLIENT_DB2_HPP

// WoW/Source/DB/DBEngine/WowClientDB2: a client table loaded from a WDC4 .db2 file. The file's
// records stay as the file has them (packed fields are decoded on read); a lookup maps ids to
// them, and secondary indices (unique keys, a parent field, indexed fields) are built at load.

#include <cstdint>

#include "blz/ChainedHash.hpp"
#include "db/DBCTypes.hpp"
#include "db/DBMeta.hpp"

class WowDB2Lookup;
class CDataStore;
namespace cas {
struct CASFILE;
}
using cas::CASFILE;

// The file's header (WDC4, 0x48 bytes).
struct DB2HEADER {
    uint32_t m_signature;           // 0x00 'WDC4'
    uint32_t m_recordCount;         // 0x04
    uint32_t m_fieldCount;          // 0x08
    uint32_t m_recordSize;          // 0x0c
    uint32_t m_stringTableSize;     // 0x10
    uint32_t m_tableHash;           // 0x14
    uint32_t m_layoutHash;          // 0x18
    int32_t m_minId;                // 0x1c
    int32_t m_maxId;                // 0x20
    uint32_t m_locale;              // 0x24
    uint16_t m_flags;               // 0x28 1 sparse, 2 a parent column, 4 an id list
    uint16_t m_idIndex;             // 0x2a
    uint32_t m_totalFieldCount;     // 0x2c
    uint32_t m_packedDataOffset;    // 0x30
    uint32_t m_lookupColumnCount;   // 0x34
    uint32_t m_fieldStorageInfoSize;  // 0x38
    uint32_t m_commonDataSize;      // 0x3c
    uint32_t m_palletDataSize;      // 0x40
    uint32_t m_sectionCount;        // 0x44
};
static_assert(sizeof(DB2HEADER) == 0x48, "a WDC4 header is 0x48 bytes");

// A section's header (0x28 bytes).
struct DB2SECTIONHEADER {
    uint64_t m_tactKey;             // 0x00 non-zero: encrypted with that key
    uint32_t m_fileOffset;          // 0x08
    uint32_t m_recordCount;         // 0x0c
    uint32_t m_stringTableSize;     // 0x10
    uint32_t m_recordsEnd;          // 0x14
    uint32_t m_idListSize;          // 0x18
    uint32_t m_relationshipSize;    // 0x1c
    uint32_t m_offsetMapCount;      // 0x20
    uint32_t m_copyTableCount;      // 0x24
};
static_assert(sizeof(DB2SECTIONHEADER) == 0x28, "a section header is 0x28 bytes");

// A column's storage (0x18 bytes): 0 none, 1 packed, 2 common data, 3 pallet, 4 pallet array,
// 5 packed signed.
struct DB2FIELDINFO {
    uint16_t m_offsetBits;          // 0x00
    uint16_t m_sizeBits;            // 0x02
    uint32_t m_additionalSize;      // 0x04
    uint32_t m_storage;             // 0x08
    uint32_t m_value1;              // 0x0c packed offset, or the common default
    uint32_t m_value2;              // 0x10 packed size
    uint32_t m_value3;              // 0x14 the array count
};
static_assert(sizeof(DB2FIELDINFO) == 0x18, "a field storage entry is 0x18 bytes");

// The client's open-addressed maps with an int key, `Empty` marking a free slot and `Deleted` a
// removed one; probing steps 1, 2, 3, ... from fmix64 of the sign-extended key. Rehashing
// (FUN_140584ee0 and its copies) sizes to the live count over the load limit, 8 at least.
template <class V, int32_t Empty, int32_t Deleted>
struct TDBCIntMap {
    struct Entry {
        int32_t m_key;
        V m_value;
    };

    uint64_t m_capacity = 8;        // 0x00
    Entry* m_entries = nullptr;     // 0x08
    uint64_t m_used = 0;            // 0x10 live and removed
    uint64_t m_deleted = 0;         // 0x18
    float m_maxLoad = 0.75f;        // 0x20

    void Allocate();
    void Free();
    void Rehash(uint64_t count);
    // FUN_140582d80: rehashes when `count` would pass the load limit.
    bool ReserveFor(uint64_t count);
    // The value for `key`, inserted zeroed when missing.
    V* Insert(int32_t key, bool* inserted);
    V* Find(int32_t key);
};

// A common-data column: record id -> value (FUN_140232ce0 finds).
using DB2COMMONMAP = TDBCIntMap<uint32_t, -2, -3>;
// A unique int key: value -> record.
using DB2INTKEYMAP = TDBCIntMap<void*, static_cast<int32_t>(0x80000001), static_cast<int32_t>(0x80000000)>;

// A unique string key's map: a string -> record (empty 0, removed 1).
struct DB2STRINGKEYMAP {
    struct Entry {
        const char* m_key;
        void* m_value;
    };
    uint64_t m_capacity = 8;
    Entry* m_entries = nullptr;
    uint64_t m_used = 0;
    uint64_t m_deleted = 0;
    float m_maxLoad = 0.75f;
};

// An indexed column: value -> (first, count) into the records sorted by value.
struct DB2INDEXRANGE {
    uint32_t m_first;
    uint32_t m_count;
};
struct DB2INDEXMAP {
    struct Entry {
        uint32_t m_key;
        DB2INDEXRANGE m_value;
    };
    uint64_t m_capacity = 8;
    Entry* m_entries = nullptr;
    uint64_t m_used = 0;
    uint64_t m_deleted = 0;
    float m_maxLoad = 0.75f;
};

// The relationship (parent) data a section carries: (foreign id, record index) pairs and their
// range (DBC2IndexData, 0x28).
struct DBC2INDEXENTRY {
    uint32_t m_foreignId;
    uint32_t m_recordIndex;
};
struct DBC2INDEXDATA {
    uint32_t m_count;               // 0x00
    int32_t m_minId;                // 0x04
    int32_t m_maxId;                // 0x08
    uint32_t m_pad0c;
    TDBCVector<DBC2INDEXENTRY> m_entries;  // 0x10
};

// A copy-table row: the new id, and the id it copies.
struct DB2DUPLICATE {
    uint32_t m_newId;
    uint32_t m_sourceId;
};

// What each section's load tracks (WowClientDB2_Base::AsyncSection, 0x28).
struct DB2ASYNCSECTION {
    class WowClientDB2_Base* m_table;  // 0x00
    void* m_read;                   // 0x08 the async read
    void* m_buffer;                 // 0x10
    uint8_t m_loaded;               // 0x18
    uint8_t m_pad19[7];
    void* m_20;                     // 0x20
};

// A watcher of patched records (0x10).
struct DB2PATCHCALLBACK {
    void (*m_callback)(uint32_t table, uint32_t id, uint32_t a3, void* record, void* previous);
    void* m_context;
};

// An encrypted section's record (FUN_14057df40's nodes).
struct DB2ENCRYPTEDNODE {
    DB2ENCRYPTEDNODE* m_next;
    uint32_t m_key;                 // the record id
    uint8_t m_encrypted;            // 0x0c
    uint8_t m_pending;              // 0x0d
    uint8_t m_pad0e[2];
    uint64_t m_tactKey;             // 0x10
    uint64_t m_pad18;
};

class WowDB2ParentLookup;

class WowClientDB2_Base {
    public:
    virtual ~WowClientDB2_Base();                                               // 0x00
    virtual void Slot08();                                                      // 0x08
    virtual void Slot10();                                                      // 0x10
    virtual bool Slot18();                                                      // 0x18
    virtual void ApplyPendingPatches();                                         // 0x20
    virtual void Unload(bool failed);                                           // 0x28
    virtual bool Reload(bool errors);                                           // 0x30
    virtual void NotifyRecordWatchers();                                        // 0x38
    virtual void Slot40();                                                      // 0x40
    virtual void Slot48();                                                      // 0x48
    virtual void Slot50();                                                      // 0x50
    virtual bool LoadSection(void* file, int32_t section, bool errors);         // 0x58
    virtual void RecordCallback() = 0;                                          // 0x60
    virtual void AddRef(void* record);                                          // 0x68
    virtual void Release(void* record);                                         // 0x70
    virtual uint64_t Slot78();                                                  // 0x78
    virtual void Slot80();                                                      // 0x80
    virtual bool Load(bool errors);                                             // 0x88
    virtual bool ValidateMeta();                                                // 0x90
    virtual CASFILE** FileSlot();                                               // 0x98
    virtual void CloseFile(CASFILE* file);                                      // 0xa0
    virtual bool ReadSection(CDataStore* store, CASFILE* file, uint32_t section, bool errors);  // 0xa8
    virtual bool CopySection(CDataStore* store, bool errors, uint32_t section);  // 0xb0
    virtual bool ReadSectionExtra(CDataStore* store, bool errors, uint32_t section);  // 0xb8
    virtual bool ReadCopyTable(CDataStore* store, bool errors, uint32_t section,
        TDBCVector<DB2DUPLICATE>* duplicates);                                  // 0xc0
    virtual uint32_t SparseCount();                                             // 0xc8
    virtual void CallbackD0();                                                  // 0xd0
    virtual void CallbackD8();                                                  // 0xd8
    virtual void CallbackE0();                                                  // 0xe0
    virtual void CallbackE8();                                                  // 0xe8
    virtual void* DeserializeRecord(CDataStore* store);                         // 0xf0
    virtual void SetRecord(uint32_t id, void* record, void* previous, int32_t parent);  // 0xf8
    virtual void SetLookupRecord(int32_t id, void* record, void* previous);     // 0x100
    virtual void UpdateParent(void* record, int32_t parent, uint32_t id, void* previous);  // 0x108
    virtual void UpdateIndexed(void* record, void* previous);                   // 0x110
    virtual void Slot118();                                                     // 0x118
    virtual uint32_t SectionSize(uint32_t section);                             // 0x120
    virtual uint32_t SectionOffset(uint32_t section);                           // 0x128
    virtual CASFILE* OpenFile(bool errors);                                     // 0x130
    virtual void CloseFile2(CASFILE* file);                                     // 0x138

    const DBMeta* m_meta;                           // 0x008
    TDBCVector<DB2FIELDINFO> m_fieldInfo;           // 0x010
    TDBCVector<TDBCVector<uint32_t>> m_pallets;     // 0x028
    TDBCVector<uint8_t*> m_palletArrays;            // 0x040
    TDBCVector<DB2COMMONMAP> m_commonData;          // 0x058
    uint32_t m_packedDataOffset;                    // 0x070
    uint32_t m_pad074;
    blz::chained_hash_header m_pendingPatches;      // 0x078
    WowDB2ParentLookup* m_parentLookup;             // 0x098
    TDBCVector<DB2PATCHCALLBACK> m_patchCallbacks;  // 0x0a0
    TDBCVector<void (*)(int32_t)> m_loadCallbacks;  // 0x0b8
    blz::chained_hash_header m_deserializers;       // 0x0d0
    TDBCVector<DB2INDEXMAP> m_indexMaps;            // 0x0f0
    TDBCVector<TDBCVector<void*>> m_indexRecords;   // 0x108
    DB2HEADER* m_header;                            // 0x120
    DB2SECTIONHEADER* m_sections;                   // 0x128
    uint8_t* m_data;                                // 0x130 records, then the strings
    uint8_t* m_records;                             // 0x138
    WowDB2Lookup* m_lookup;                         // 0x140
    TDBCVector<DB2INTKEYMAP> m_intKeys;             // 0x148
    TDBCVector<DB2STRINGKEYMAP> m_stringKeys;       // 0x160
    uint32_t* m_fieldStructures;                    // 0x178
    TDBCVector<DB2ASYNCSECTION> m_asyncSections;    // 0x180
    uint32_t m_asyncPending;                        // 0x198
    uint32_t m_pad19c;
    blz::chained_hash_header m_encryption;          // 0x1a0 encrypted record id -> its section's key
    uint32_t m_recordCount;                         // 0x1c0
    uint32_t m_totalCount;                          // 0x1c4
    uint32_t m_liveCount;                           // 0x1c8
    int32_t m_maxId;                                // 0x1cc
    int32_t m_minId;                                // 0x1d0
    uint32_t m_stride;                              // 0x1d4
    uint64_t m_1d8;                                 // 0x1d8
    uint64_t m_1e0;                                 // 0x1e0
    uint8_t m_deleted;                              // 0x1e4 its address marks a deleted record
    uint8_t m_loaded;                               // 0x1e5
    uint8_t m_indexedValid;                         // 0x1e6
    uint8_t m_uniqueKeysValid;                      // 0x1e7
    uint8_t m_patched;                              // 0x1e8
    uint8_t m_pad1e9[7];

    // FUN_1405780a0
    explicit WowClientDB2_Base(const DBMeta* meta);

    // FUN_14057b640: the record with `id`, or null. *local says whether it came from the file.
    void* GetRecord(uint32_t id, bool request, bool* local);
    // FUN_140581790
    bool HasRecord(int32_t id);
    // FUN_140579030: called with 0 after the table loads, 1 after it unloads.
    void AddLoadCallback(void (*callback)(int32_t event));
    // FUN_1405791e0 / FUN_14057ae20: a watcher of patched records.
    void AddPatchCallback(void (*callback)(uint32_t, uint32_t, uint32_t, void*, void*), void* context);
    void RemovePatchCallback(void (*callback)(uint32_t, uint32_t, uint32_t, void*, void*));
    // FUN_14057b830: the record whose unique int key `key` is `value`.
    void* GetRecordByIntKey(int32_t value, int32_t key);
    // FUN_14057b920
    void* GetRecordByStringKey(const char* value, int32_t key);

    // FUN_14057dc80: a checked read of exactly `bytes`.
    bool ReadExact(CASFILE* file, void* dest, uint32_t bytes, bool errors);
    // FUN_14057e360
    bool LoadInternal(CASFILE* file, bool errors);
    // FUN_14057bd40
    bool ValidateHeader(const DB2HEADER* header, bool errors);
    // FUN_14057d1d0: the pallets, pallet arrays and common data.
    bool LoadFieldData(CASFILE* file, bool errors);
    // FUN_14057df40: each encrypted section's record ids.
    bool LoadEncryptedIds(CASFILE* file, bool errors);
    // FUN_14057ec40: a section's relationship (parent) data.
    bool ReadRelationships(CDataStore* store, bool errors, uint32_t section, TDBCVector<DBC2INDEXDATA>* data);
    // FUN_1405814f0
    bool FinalizeLoad(TDBCVector<DB2DUPLICATE>* duplicates, TDBCVector<int32_t>* ids,
        TDBCVector<DBC2INDEXDATA>* relationships, bool errors);
    // FUN_14057a0a0
    void BuildParentLookup(DBC2INDEXDATA* data);
    // FUN_14057a170
    void BuildIdLookup(TDBCVector<DB2DUPLICATE>* duplicates, TDBCVector<int32_t>* ids);
    // FUN_1405798f0
    void BuildIndexedFields();
    // FUN_140579b90
    void BuildIndexedField(uint32_t field, DB2INDEXMAP* map, TDBCVector<void*>* records);
    // FUN_14057a2e0
    bool BuildUniqueKeys(bool errors);

    // FUN_140576930: an int field of a record, decoding packed fields of file records.
    uint32_t FieldValue(const uint8_t* record, uint32_t field, int32_t arrayIndex);
    // FUN_14057b4c0: a file record's packed id, or 0.
    uint32_t PackedId(const uint8_t* record);
    // Whether `record` is one of the file's records.
    bool IsFileRecord(const uint8_t* record);
};

// The typed tables: the record-callback map follows the base (0x1f0, 0x20 bytes).
class WowClientDB2 : public WowClientDB2_Base {
    public:
    blz::chained_hash_header m_recordCallbacks;     // 0x1f0

    explicit WowClientDB2(const DBMeta* meta) : WowClientDB2_Base(meta) {
    }

    // FUN_140442370
    void RecordCallback() override;
};

// Whether a missing record can be asked of the server as a hotfix (DAT_142e8e258).
bool DB2HotfixRequestsAvailable();

// FUN_140576870: a field as the client lays the record out (no decoding).
uint32_t DB2PlainFieldValue(const uint8_t* record, const DBMeta* meta, int32_t field, int32_t arrayIndex);

// The generated records' typed field reads: (the field's address, its index, its offset, the
// table). File records of packed columns are decoded.
uint8_t DB2FieldU8(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table);
uint16_t DB2FieldU16(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table);
uint32_t DB2FieldU32(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table);
int32_t DB2FieldI32(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table);
int16_t DB2FieldI16(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table);

// FUN_14057bc20: every table by its table hash.
WowClientDB2_Base* WowClientDB2GetByTableHash(uint32_t tableHash);

#endif
