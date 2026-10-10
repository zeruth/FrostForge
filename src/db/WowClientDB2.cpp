#include "db/WowClientDB2.hpp"

#include <algorithm>
#include <cstring>
#include <new>

#include "base/CDataStore.hpp"
#include "cas/CAS.hpp"
#include "db/WowDB2Lookup.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

using namespace cas;

// Elsewhere in the client.
uint32_t LocaleGetText();
void* ClientServicesGet();
const char* GxShaderName(int32_t id, char* buffer, uint32_t size);

namespace {

const char s_db2File[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\DB\\DBEngine\\WowClientDB2.cpp";
const char s_encryptedNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBIUEncryptionStatus@WowClientDB2_Base@@@blz@@@blz@@";
const char s_encryptedBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBIUEncryptionStatus@WowClientDB2_Base@@@blz@@@blz@@";

constexpr uint32_t DB2_SIGNATURE = 0x34434457;  // 'WDC4'

// The hotfix requests object (DAT_142e8e258, vtable 0x142639fa8), made by ClientDBInitialize.
bool s_hotfixRequests;

void* DB2Alloc(uint64_t bytes, int32_t line) {
    return SMemAlloc(bytes, 0x24, s_db2File, SStrHashFnv1a(s_db2File), line, 0);
}

void DB2Free(void* memory, int32_t line) {
    SMemFree(memory, s_db2File, line, 0);
}

// A bit field of a record.
uint64_t ReadBits(const uint8_t* base, uint32_t offsetBits, uint32_t sizeBits) {
    uint64_t word;
    std::memcpy(&word, base + (offsetBits >> 3), sizeof(word));
    uint32_t top = 0x40 - sizeBits;
    return (word << ((top - (offsetBits & 7)) & 0x3f)) >> (top & 0x3f);
}

// The registry of every table by its table hash (FUN_14057bc20's map, DAT_142ccdf78): open
// addressing over (hash, table) pairs, 0 empty and 1 removed.
struct DB2REGISTRY {
    struct Entry {
        uint32_t m_key;
        uint32_t m_pad;
        WowClientDB2_Base* m_table;
    };
    uint64_t m_capacity = 8;
    Entry* m_entries = nullptr;
    uint64_t m_used = 0;
    uint64_t m_deleted = 0;
    float m_maxLoad = 0.75f;
};

uint64_t RegistryHash(uint32_t key) {
    uint64_t k = static_cast<uint64_t>(key) * 0xFF51AFD7ED558CCDull;
    k = (k >> 33 ^ k) * 0xC4CEB9FE1A85EC53ull;
    return k >> 33 ^ k;
}

// ref: FUN_14057bc20
DB2REGISTRY* Registry() {
    static DB2REGISTRY* registry = [] {
        auto map = new (SMemAlloc(sizeof(DB2REGISTRY), 0xb, s_db2File, SStrHashFnv1a(s_db2File), -6, 0)) DB2REGISTRY();
        map->m_entries = static_cast<DB2REGISTRY::Entry*>(SMemAlloc(0x80, 0xb, s_db2File, SStrHashFnv1a(s_db2File), -6, 0));
        for (uint64_t i = 0; i < map->m_capacity; i++) {
            map->m_entries[i].m_key = 0;
        }
        return map;
    }();
    return registry;
}

// FUN_1405853c0: rehash to the live count over the load limit.
void RegistryRehash(DB2REGISTRY* map) {
    float live = static_cast<float>(map->m_used - map->m_deleted);
    uint64_t needed = static_cast<uint64_t>(live / map->m_maxLoad);
    if (static_cast<float>(needed) * map->m_maxLoad < live) {
        needed++;
    }
    uint64_t capacity = 8;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (capacity == map->m_capacity && map->m_deleted == 0) {
        return;
    }
    auto entries = static_cast<DB2REGISTRY::Entry*>(SMemAlloc(capacity * sizeof(DB2REGISTRY::Entry), 0xb, s_db2File, SStrHashFnv1a(s_db2File), -6, 0));
    for (uint64_t i = 0; i < capacity; i++) {
        entries[i].m_key = 0;
    }
    for (uint64_t i = 0; i < map->m_capacity; i++) {
        DB2REGISTRY::Entry* old = &map->m_entries[i];
        if (old->m_key == 0 || (map->m_deleted != 0 && old->m_key == 1)) {
            continue;
        }
        uint64_t index = RegistryHash(old->m_key) & (capacity - 1);
        for (uint64_t step = 1; entries[index].m_key != 0; step++) {
            index = (index + step) & (capacity - 1);
        }
        entries[index] = *old;
    }
    SMemFree(map->m_entries, s_db2File, -6, 0);
    map->m_used -= map->m_deleted;
    map->m_deleted = 0;
    map->m_entries = entries;
    map->m_capacity = capacity;
}

// ref: FUN_140577f10
void RegistryInsert(uint32_t key, WowClientDB2_Base* table) {
    DB2REGISTRY* map = Registry();
    float load = static_cast<float>(map->m_used + 1) / static_cast<float>(map->m_capacity);
    if (map->m_maxLoad < load) {
        RegistryRehash(map);
    }
    uint64_t mask = map->m_capacity - 1;
    uint64_t index = RegistryHash(key) & mask;
    DB2REGISTRY::Entry* removed = nullptr;
    for (uint64_t step = 1;; step++) {
        DB2REGISTRY::Entry* slot = &map->m_entries[index];
        if (slot->m_key == 0) {
            break;
        }
        if (slot->m_key == 1) {
            if (removed == nullptr) {
                removed = slot;
            }
        } else if (slot->m_key == key) {
            return;
        }
        index = (index + step) & mask;
    }
    DB2REGISTRY::Entry* slot = removed != nullptr ? removed : &map->m_entries[index];
    map->m_used++;
    slot->m_key = key;
    slot->m_table = table;
}

// The int maps' hash: fmix64 of the sign-extended key.
uint64_t IntMapHash(int32_t key) {
    uint64_t k = static_cast<uint64_t>(static_cast<int64_t>(key));
    k = (k >> 33 ^ k) * 0xFF51AFD7ED558CCDull;
    k = (k >> 33 ^ k) * 0xC4CEB9FE1A85EC53ull;
    return k >> 33 ^ k;
}

// The indexed fields' hash: FNV-1a over the key's four bytes.
uint32_t IndexMapHash(uint32_t key) {
    uint32_t h = 0x811C9DC5;
    for (int32_t i = 0; i < 4; i++) {
        h = (h ^ ((key >> (i * 8)) & 0xff)) * 0x1000193;
    }
    return h;
}

// A table's file name for messages ("FileData ID %d").
const char* TableFileName(const WowClientDB2_Base* table, char* buffer, uint32_t size) {
    return GxShaderName(table->m_meta->m_fileDataId, buffer, size);
}

// ref: FUN_1405a5eb0: whether hotfixes can be requested from the server.
bool HotfixRequestsAvailable() {
    auto services = static_cast<uint8_t*>(ClientServicesGet());
    if (services == nullptr) {
        return false;
    }
    return services[0x771] == 1 && services[0x368] != 0;
}

}

// ---- the int maps ------------------------------------------------------------------------

template <class V, int32_t Empty, int32_t Deleted>
void TDBCIntMap<V, Empty, Deleted>::Allocate() {
    this->m_entries = static_cast<Entry*>(SMemAlloc(this->m_capacity * sizeof(Entry), 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
    for (uint64_t i = 0; i < this->m_capacity; i++) {
        this->m_entries[i].m_key = Empty;
    }
}

template <class V, int32_t Empty, int32_t Deleted>
void TDBCIntMap<V, Empty, Deleted>::Free() {
    if (this->m_entries != nullptr) {
        SMemFree(this->m_entries, s_dbcTypesFile, 0x103, 0);
        this->m_entries = nullptr;
    }
}

// ref: FUN_140584ee0
template <class V, int32_t Empty, int32_t Deleted>
void TDBCIntMap<V, Empty, Deleted>::Rehash(uint64_t count) {
    float live = static_cast<float>(this->m_used - this->m_deleted);
    uint64_t needed = static_cast<uint64_t>(live / this->m_maxLoad);
    if (static_cast<float>(needed) * this->m_maxLoad < live) {
        needed++;
    }
    if (needed < count) {
        needed = count;
    }
    uint64_t capacity = 8;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (capacity == this->m_capacity && this->m_deleted == 0 && this->m_entries != nullptr) {
        return;
    }
    auto entries = static_cast<Entry*>(SMemAlloc(capacity * sizeof(Entry), 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
    for (uint64_t i = 0; i < capacity; i++) {
        entries[i].m_key = Empty;
    }
    if (this->m_entries != nullptr) {
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            Entry* old = &this->m_entries[i];
            if (old->m_key == Empty || (this->m_deleted != 0 && old->m_key == Deleted)) {
                continue;
            }
            uint64_t index = IntMapHash(old->m_key) & (capacity - 1);
            for (uint64_t step = 1; entries[index].m_key != Empty; step++) {
                index = (index + step) & (capacity - 1);
            }
            entries[index] = *old;
        }
        SMemFree(this->m_entries, s_dbcTypesFile, 0x103, 0);
    }
    this->m_used -= this->m_deleted;
    this->m_deleted = 0;
    this->m_entries = entries;
    this->m_capacity = capacity;
}

// ref: FUN_140582d80
template <class V, int32_t Empty, int32_t Deleted>
bool TDBCIntMap<V, Empty, Deleted>::ReserveFor(uint64_t count) {
    float load = static_cast<float>(count) / static_cast<float>(this->m_capacity);
    if (load <= this->m_maxLoad) {
        return false;
    }
    this->Rehash(0);
    return true;
}

template <class V, int32_t Empty, int32_t Deleted>
V* TDBCIntMap<V, Empty, Deleted>::Insert(int32_t key, bool* inserted) {
    this->ReserveFor(this->m_used + 1);
    uint64_t mask = this->m_capacity - 1;
    uint64_t index = IntMapHash(key) & mask;
    Entry* removed = nullptr;
    for (uint64_t step = 1;; step++) {
        Entry* slot = &this->m_entries[index];
        if (slot->m_key == Empty) {
            break;
        }
        if (slot->m_key == Deleted) {
            if (removed == nullptr) {
                removed = slot;
            }
        } else if (slot->m_key == key) {
            *inserted = false;
            return &slot->m_value;
        }
        index = (index + step) & mask;
    }
    Entry* slot = removed != nullptr ? removed : &this->m_entries[index];
    slot->m_key = key;
    std::memset(&slot->m_value, 0, sizeof(V));
    this->m_used++;
    *inserted = true;
    return &slot->m_value;
}

// ref: FUN_140232ce0 (the common data's find)
template <class V, int32_t Empty, int32_t Deleted>
V* TDBCIntMap<V, Empty, Deleted>::Find(int32_t key) {
    if (this->m_entries == nullptr) {
        return nullptr;
    }
    uint64_t mask = this->m_capacity - 1;
    uint64_t index = IntMapHash(key) & mask;
    for (uint64_t step = 1; this->m_entries[index].m_key != Empty; step++) {
        Entry* slot = &this->m_entries[index];
        if ((this->m_deleted == 0 || slot->m_key != Deleted) && slot->m_key == key) {
            return &slot->m_value;
        }
        index = (index + step) & mask;
    }
    return nullptr;
}

template struct TDBCIntMap<uint32_t, -2, -3>;
template struct TDBCIntMap<void*, static_cast<int32_t>(0x80000001), static_cast<int32_t>(0x80000000)>;

// ---- field reads -------------------------------------------------------------------------

// ref: FUN_140576870
uint32_t DB2PlainFieldValue(const uint8_t* record, const DBMeta* meta, int32_t field, int32_t arrayIndex) {
    int32_t type = meta->m_fieldTypes[field];
    const uint8_t* at = record + meta->m_fieldOffsets[field];
    if ((meta->m_fieldFlags[field] & 4) == 0) {
        if (type == 0) {
            uint32_t value;
            std::memcpy(&value, at + arrayIndex * 4, 4);
            return value;
        }
        if (type == 3) {
            float value;
            std::memcpy(&value, at + arrayIndex * 4, 4);
            return static_cast<uint32_t>(static_cast<int32_t>(value));
        }
        if (type == 4) {
            return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(at[arrayIndex])));
        }
        if (type == 5) {
            int16_t value;
            std::memcpy(&value, at + arrayIndex * 2, 2);
            return static_cast<uint32_t>(static_cast<int32_t>(value));
        }
    } else {
        if (type == 0) {
            uint32_t value;
            std::memcpy(&value, at + arrayIndex * 4, 4);
            return value;
        }
        if (type == 4) {
            return at[arrayIndex];
        }
        if (type == 5) {
            uint16_t value;
            std::memcpy(&value, at + arrayIndex * 2, 2);
            return value;
        }
    }
    return 0;
}

namespace {

// ref: FUN_140575bc0: a file record's column decoded from its storage, at the file column's
// width and signedness (floats convert to int).
uint32_t DecodeField(const uint8_t* record, const uint8_t* packed, const DB2FIELDINFO* info,
    TDBCVector<DB2COMMONMAP>* common, TDBCVector<TDBCVector<uint32_t>>* pallets, const uint8_t* palletArray,
    uint32_t id, const DBMeta* meta, int32_t field, int32_t arrayIndex, bool floatBits = false) {
    int32_t type = meta->m_fileFieldTypes[field];
    bool isUnsigned = (meta->m_fileFieldFlags[field] & 4) != 0;
    uint32_t width = type == 4 ? 1 : type == 5 ? 2 : 4;

    // The raw bits of the value, before the width and sign are applied.
    uint32_t raw = 0;
    bool fromStorageValue = false;
    switch (info->m_storage) {
        case 0: {
            const uint8_t* at = record + (info->m_offsetBits >> 3) + static_cast<int64_t>(arrayIndex) * width;
            std::memcpy(&raw, at, width);
            break;
        }
        case 1:
            raw = static_cast<uint32_t>(ReadBits(packed, info->m_value1, info->m_value2));
            break;
        case 2: {
            uint32_t* value = (*common)[field].Find(static_cast<int32_t>(id));
            raw = value != nullptr ? *value : info->m_value1;
            fromStorageValue = true;
            break;
        }
        case 3: {
            uint32_t index = static_cast<uint32_t>(ReadBits(packed, info->m_value1, info->m_value2));
            std::memcpy(&raw, reinterpret_cast<const uint8_t*>((*pallets)[field].m_data) + static_cast<uint64_t>(index) * 4, width);
            break;
        }
        case 4: {
            uint32_t index = static_cast<uint32_t>(ReadBits(packed, info->m_value1, info->m_value2));
            std::memcpy(&raw, palletArray + static_cast<uint64_t>(index * info->m_value3 + arrayIndex) * 4, width);
            break;
        }
        case 5: {
            uint32_t bits = static_cast<uint32_t>(ReadBits(packed, info->m_value1, info->m_value2));
            uint32_t sign = 1u << ((info->m_value2 - 1) & 0x1f);
            if (type == 4) {
                uint8_t b = static_cast<uint8_t>(1u << ((info->m_value2 - 1) & 0x1f));
                raw = static_cast<uint8_t>((static_cast<uint8_t>(bits) ^ b) - b);
            } else if (type == 5) {
                uint16_t s = static_cast<uint16_t>(sign);
                raw = static_cast<uint16_t>((static_cast<uint16_t>(bits) ^ s) - s);
            } else {
                raw = (bits ^ sign) - sign;
            }
            break;
        }
        default:
            return 0;
    }
    (void)fromStorageValue;

    if (type == 3 && !floatBits) {
        float value;
        std::memcpy(&value, &raw, 4);
        return static_cast<uint32_t>(static_cast<int32_t>(value));
    }
    if (type == 4) {
        return isUnsigned ? static_cast<uint8_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
    }
    if (type == 5) {
        return isUnsigned ? static_cast<uint16_t>(raw) : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
    }
    return raw;
}

}

bool WowClientDB2_Base::IsFileRecord(const uint8_t* record) {
    WowDB2Lookup* lookup = this->m_lookup;
    if (lookup == nullptr) {
        return false;
    }
    uint64_t span = static_cast<uint64_t>(lookup->m_init.m_recordCount * lookup->m_init.m_stride);
    return static_cast<uint64_t>(record - lookup->m_init.m_records) < span;
}

// ref: FUN_14057b4c0
uint32_t WowClientDB2_Base::PackedId(const uint8_t* record) {
    WowDB2Lookup* lookup = this->m_lookup;
    if ((lookup->m_init.m_idPacked & 1) == 0) {
        return 0;
    }
    return static_cast<uint32_t>(ReadBits(record, lookup->m_init.m_idOffsetBits, lookup->m_init.m_idSizeBits));
}

// ref: FUN_140576930
uint32_t WowClientDB2_Base::FieldValue(const uint8_t* record, uint32_t field, int32_t arrayIndex) {
    if (!this->IsFileRecord(record) || (this->m_meta->m_fileFieldFlags[field] & 2) == 0) {
        return DB2PlainFieldValue(record, this->m_meta, static_cast<int32_t>(field), arrayIndex);
    }
    uint32_t id = 0;
    if (this->m_fieldInfo[field].m_storage == 2 && (this->m_lookup->m_init.m_idPacked & 1) != 0) {
        id = static_cast<uint32_t>(ReadBits(record, this->m_lookup->m_init.m_idOffsetBits, this->m_lookup->m_init.m_idSizeBits));
    }
    const uint8_t* palletArray = nullptr;
    if (field < this->m_palletArrays.size()) {
        palletArray = this->m_palletArrays[field];
    }
    return DecodeField(record, record + this->m_packedDataOffset, &this->m_fieldInfo[field], &this->m_commonData,
        &this->m_pallets, palletArray, id, this->m_meta, static_cast<int32_t>(field), arrayIndex);
}

// A float column (FUN_1402dc640 and the generated float reads): the value's bits, stored
// plain or decoded from the file's storage, read as a float rather than converted.
float WowClientDB2_Base::FieldFloat(const uint8_t* record, uint32_t field, int32_t arrayIndex) {
    float value;
    if (!this->IsFileRecord(record) || (this->m_meta->m_fileFieldFlags[field] & 2) == 0) {
        std::memcpy(&value, record + this->m_meta->m_fieldOffsets[field] + arrayIndex * 4, 4);
        return value;
    }
    uint32_t id = 0;
    if (this->m_fieldInfo[field].m_storage == 2 && (this->m_lookup->m_init.m_idPacked & 1) != 0) {
        id = static_cast<uint32_t>(ReadBits(record, this->m_lookup->m_init.m_idOffsetBits, this->m_lookup->m_init.m_idSizeBits));
    }
    const uint8_t* palletArray = nullptr;
    if (field < this->m_palletArrays.size()) {
        palletArray = this->m_palletArrays[field];
    }
    uint32_t bits = DecodeField(record, record + this->m_packedDataOffset, &this->m_fieldInfo[field], &this->m_commonData,
        &this->m_pallets, palletArray, id, this->m_meta, static_cast<int32_t>(field), arrayIndex, true);
    std::memcpy(&value, &bits, 4);
    return value;
}

namespace {

// FUN_14022b1b0 and its siblings: the generated records' typed reads.
template <class T>
T TypedField(const uint8_t* fieldAddress, uint32_t index, int64_t offset, WowClientDB2_Base* table) {
    const uint8_t* record = fieldAddress - offset;
    if (!table->IsFileRecord(record)) {
        T value;
        std::memcpy(&value, record + table->m_meta->m_fieldOffsets[index], sizeof(T));
        return value;
    }
    const DB2FIELDINFO* info = &table->m_fieldInfo[static_cast<int32_t>(index)];
    switch (info->m_storage) {
        case 1:
            return static_cast<T>(ReadBits(fieldAddress, info->m_value1, info->m_value2));
        case 2: {
            uint32_t* value = table->m_commonData[static_cast<int32_t>(index)].Find(static_cast<int32_t>(table->PackedId(fieldAddress)));
            return static_cast<T>(value != nullptr ? *value : info->m_value1);
        }
        case 3: {
            uint64_t slot = ReadBits(fieldAddress, info->m_value1, info->m_value2) & 0xFFFFFFFF;
            T value;
            std::memcpy(&value, reinterpret_cast<const uint8_t*>(table->m_pallets[static_cast<int32_t>(index)].m_data) + slot * 4, sizeof(T));
            return value;
        }
        case 5: {
            uint64_t bits = ReadBits(fieldAddress, info->m_value1, info->m_value2);
            T sign = static_cast<T>(1ull << ((info->m_value2 - 1) & 0x1f));
            return static_cast<T>((static_cast<T>(bits) ^ sign) - sign);
        }
        default:
            return 0;
    }
}

}

// ref: FUN_14022b1b0
uint8_t DB2FieldU8(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table) {
    return TypedField<uint8_t>(field, index, offset, table);
}

// ref: FUN_140226eb0
uint16_t DB2FieldU16(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table) {
    return TypedField<uint16_t>(field, index, offset, table);
}

// ref: FUN_1402dc640
float DB2FieldF32(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table) {
    const uint8_t* record = field - offset;
    float value = 0.0f;
    if (!table->IsFileRecord(record)) {
        std::memcpy(&value, record + table->m_meta->m_fieldOffsets[index], sizeof(value));
        return value;
    }
    const DB2FIELDINFO* info = &table->m_fieldInfo[static_cast<int32_t>(index)];
    uint32_t bits = 0;
    switch (info->m_storage) {
        case 1:
            bits = static_cast<uint32_t>(ReadBits(field, info->m_value1, info->m_value2));
            break;
        case 2: {
            uint32_t* common = table->m_commonData[static_cast<int32_t>(index)].Find(
                static_cast<int32_t>(table->PackedId(field)));
            bits = common != nullptr ? *common : info->m_value1;
            break;
        }
        case 3: {
            uint64_t slot = ReadBits(field, info->m_value1, info->m_value2) & 0xFFFFFFFF;
            std::memcpy(&bits, reinterpret_cast<const uint8_t*>(table->m_pallets[static_cast<int32_t>(index)].m_data) + slot * 4, 4);
            break;
        }
        case 5: {
            uint32_t raw = static_cast<uint32_t>(ReadBits(field, info->m_value1, info->m_value2));
            uint32_t sign = 1u << ((info->m_value2 - 1) & 0x1f);
            bits = (raw ^ sign) - sign;
            break;
        }
        default:
            return 0.0f;
    }
    std::memcpy(&value, &bits, 4);
    return value;
}

// ref: FUN_140233370
uint32_t DB2FieldU32(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table) {
    return TypedField<uint32_t>(field, index, offset, table);
}

// ref: FUN_140229de0
int32_t DB2FieldI32(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table) {
    return TypedField<int32_t>(field, index, offset, table);
}

// ref: FUN_14022b390
int16_t DB2FieldI16(const uint8_t* field, uint32_t index, int64_t offset, WowClientDB2_Base* table) {
    return TypedField<int16_t>(field, index, offset, table);
}

// ---- the table ---------------------------------------------------------------------------

// ref: FUN_1405780a0
WowClientDB2_Base::WowClientDB2_Base(const DBMeta* meta)
    : m_meta(meta), m_packedDataOffset(0), m_pad074(0), m_parentLookup(nullptr), m_header(nullptr), m_sections(nullptr),
      m_data(nullptr), m_records(nullptr), m_lookup(nullptr), m_fieldStructures(nullptr), m_asyncPending(0), m_pad19c(0),
      m_recordCount(0), m_totalCount(0), m_liveCount(0), m_maxId(-1), m_minId(0xFFFFFFF), m_stride(0), m_1d8(100),
      m_1e0(0), m_deleted(0), m_loaded(0), m_indexedValid(0), m_uniqueKeysValid(0), m_patched(0) {
    RegistryInsert(meta->m_tableHash, this);
}

// ref: FUN_140578ec0
WowClientDB2_Base::~WowClientDB2_Base() {
    blz::chained_destroy<DB2ENCRYPTEDNODE>(&this->m_encryption, s_encryptedNodeTag, s_encryptedBucketTag);
    for (DB2STRINGKEYMAP& map : this->m_stringKeys) {
        SMemFree(map.m_entries, s_dbcTypesFile, 0x103, 0);
    }
    for (DB2INTKEYMAP& map : this->m_intKeys) {
        map.Free();
    }
    for (DB2INDEXMAP& map : this->m_indexMaps) {
        SMemFree(map.m_entries, s_dbcTypesFile, 0x103, 0);
    }
    for (DB2COMMONMAP& map : this->m_commonData) {
        map.Free();
    }
}

// ref: FUN_1402112e0
void WowClientDB2_Base::Slot08() {
}

// ref: FUN_1405809d0
void WowClientDB2_Base::Slot10() {
    this->Slot40();
}

// ref: FUN_1402ca290
bool WowClientDB2_Base::Slot18() {
    return false;
}

// ref: FUN_14057c0d0: patches that arrived before the table loaded.
void WowClientDB2_Base::ApplyPendingPatches() {
    if (this->m_pendingPatches.m_size == 0) {
        return;
    }
    FROST_UNIMPLEMENTED();  // hotfix records queued before the load
}

// ref: FUN_140581b10
void WowClientDB2_Base::Unload(bool failed) {
    if (!failed) {
        if (this->m_loaded == 0) {
            return;
        }
    } else {
        char name[0x104];
        TableFileName(this, name, sizeof(name));
        ErrorDisplayFormatted(0x85100086, nullptr, 0, "Unknown error while loading %s", name);
    }
    this->Slot40();
    if (this->m_lookup != nullptr && this->m_patched != 0) {
        FROST_UNIMPLEMENTED();  // releasing patched records
    }
    for (TDBCVector<uint32_t>& pallet : this->m_pallets) {
        pallet.release();
    }
    this->m_pallets.release();
    for (uint8_t* array : this->m_palletArrays) {
        DB2Free(array, 0x83a);
    }
    this->m_palletArrays.release();
    for (DB2COMMONMAP& map : this->m_commonData) {
        map.Free();
    }
    this->m_commonData.release();
    if (this->m_header != nullptr && this->m_sections != nullptr) {
        for (uint32_t i = 0; i < this->m_header->m_sectionCount; i++) {
            (void)i;  // the encryption-key release hook (DAT_142e8e2b8) is not set
        }
    }
    this->m_records = nullptr;
    DB2Free(this->m_header, 0x84d);
    this->m_header = nullptr;
    DB2Free(this->m_sections, 0x84f);
    this->m_sections = nullptr;
    DB2Free(this->m_fieldStructures, 0x851);
    this->m_fieldStructures = nullptr;
    DB2Free(this->m_data, 0x853);
    this->m_data = nullptr;
    if (this->m_lookup != nullptr) {
        this->m_lookup->~WowDB2Lookup();
        SMemFree(this->m_lookup, s_db2File, 0, 0);
        this->m_lookup = nullptr;
    }
    if (this->m_parentLookup != nullptr) {
        this->m_parentLookup->~WowDB2ParentLookup();
        SMemFree(this->m_parentLookup, s_db2File, 0, 0);
        this->m_parentLookup = nullptr;
    }
    this->m_recordCount = 0;
    this->m_totalCount = 0;
    this->m_liveCount = 0;
    this->m_maxId = -1;
    this->m_minId = 0xFFFFFFF;
    this->m_loaded = 0;
    this->m_patched = 0;
    for (DB2INTKEYMAP& map : this->m_intKeys) {
        map.Free();
    }
    this->m_intKeys.clear();
    for (DB2STRINGKEYMAP& map : this->m_stringKeys) {
        SMemFree(map.m_entries, s_dbcTypesFile, 0x103, 0);
    }
    this->m_stringKeys.clear();
    for (TDBCVector<void*>& records : this->m_indexRecords) {
        records.release();
    }
    this->m_indexRecords.release();
    for (DB2INDEXMAP& map : this->m_indexMaps) {
        SMemFree(map.m_entries, s_dbcTypesFile, 0x103, 0);
    }
    this->m_indexMaps.release();
    this->m_uniqueKeysValid = 0;
    this->m_asyncSections.clear();
    for (auto callback : this->m_loadCallbacks) {
        if (callback != nullptr) {
            callback(1);
        }
    }
}

// ref: FUN_140581810
bool WowClientDB2_Base::Reload(bool errors) {
    this->Slot40();
    if (this->m_loaded != 0 && this->m_patched != 0) {
        this->Unload(false);
        return this->Load(errors);
    }
    return true;
}

// ref: FUN_14057a760
void WowClientDB2_Base::NotifyRecordWatchers() {
    if (this->m_pendingPatches.m_size != 0) {
        FROST_UNIMPLEMENTED();  // the pending patches' watchers
    }
}

// ref: FUN_14057a880: drops pending patches and waits out the section reads.
void WowClientDB2_Base::Slot40() {
    if (this->m_pendingPatches.m_size != 0) {
        FROST_UNIMPLEMENTED();  // pending patch release
    }
    for (DB2ASYNCSECTION& section : this->m_asyncSections) {
        if (section.m_read != nullptr) {
            FROST_UNIMPLEMENTED();  // cancelling an async section read
        }
    }
    if (this->m_asyncPending != 0) {
        FROST_UNIMPLEMENTED();  // waiting for async section reads
    }
}

// ref: FUN_14057b040
void WowClientDB2_Base::Slot48() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14057bf20
void WowClientDB2_Base::Slot50() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14057f0d0
bool WowClientDB2_Base::LoadSection(void* file, int32_t section, bool errors) {
    (void)file;
    (void)section;
    (void)errors;
    FROST_UNIMPLEMENTED();  // a late (decrypted) section's load
    return false;
}

void WowClientDB2_Base::AddRef(void*) {
}

void WowClientDB2_Base::Release(void*) {
}

// ref: FUN_14022c3f0
uint64_t WowClientDB2_Base::Slot78() {
    return 0;
}

// ref: FUN_14057b2b0
void WowClientDB2_Base::Slot80() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14057e200
bool WowClientDB2_Base::Load(bool errors) {
    if (this->m_loaded != 0) {
        ErrorDisplayFormatted(0x85100086, nullptr, 0, "DB2 already loaded");
    }
    if (!this->ValidateMeta()) {
        if (errors) {
            ErrorDisplayFormatted(0x85100086, nullptr, 0, "Invalid metadata");
        }
        this->Unload(true);
        return false;
    }
    CASFILE* file = nullptr;
    int32_t fileDataId = this->m_meta->m_fileDataId;
    char name[0x110];
    GxShaderName(fileDataId, name, 0x104);
    if (!CASFileOpenById(fileDataId, &file, 0)) {
        if (errors) {
            char error[0x100];
            CASFormatLastError(error, sizeof(error));
            ErrorDisplayFormatted(0x85100086, nullptr, 0, "Unable to open %s: %s", name, error);
        }
        this->Unload(true);
        return false;
    }
    bool loaded = this->LoadInternal(file, errors);
    this->CloseFile(file);
    return loaded;
}

// ref: FUN_14026bea0
bool WowClientDB2_Base::ValidateMeta() {
    return true;
}

// ref: FUN_14022c3f0
CASFILE** WowClientDB2_Base::FileSlot() {
    return nullptr;
}

// ref: FUN_1405817e0
void WowClientDB2_Base::CloseFile(CASFILE* file) {
    CASFileClose(&file);
}

// ref: FUN_14057dc80
bool WowClientDB2_Base::ReadExact(CASFILE* file, void* dest, uint32_t bytes, bool errors) {
    uint32_t read = 0;
    bool ok = CASFileReadEx(file, dest, bytes, &read, 0, -1);
    if (ok && read == bytes) {
        return true;
    }
    if (errors) {
        char error[0x100];
        if (!ok) {
            CASFormatLastError(error, sizeof(error));
        } else {
            SStrPrintf(error, sizeof(error), "expected %d bytes, got %d bytes", bytes, read);
        }
        char name[0x110];
        TableFileName(this, name, 0x104);
        // FUN_1402cd7b0 (the file's description) is not ported; the name stands for it.
        ErrorDisplayFormatted(0x85100086, nullptr, 0, "Failed to read %s %s: %s", name, "", error);
    }
    this->Unload(true);
    return false;
}

// ref: FUN_14057bd40
bool WowClientDB2_Base::ValidateHeader(const DB2HEADER* header, bool errors) {
    uint32_t localeBit = 1u << (LocaleGetText() & 0x1f);
    const DBMeta* meta = this->m_meta;
    if (header->m_signature != DB2_SIGNATURE) {
        if (errors) {
            ErrorDisplayFormatted(0x85100086, nullptr, 0, "Invalid signature 0x%x for Table %s", header->m_signature, meta->m_name);
        }
        return false;
    }
    if (header->m_tableHash != meta->m_tableHash) {
        if (errors) {
            ErrorDisplayFormatted(0x85100079, nullptr, 0, "Table %s has wrong table hash (found %i, expected %i)",
                meta->m_name, header->m_tableHash, meta->m_tableHash);
        }
        return false;
    }
    if (header->m_layoutHash != meta->m_layoutHash) {
        if (errors) {
            ErrorDisplayFormatted(0x85100079, nullptr, 0, "Table %s has wrong layout hash (found %i, expected %i)",
                meta->m_name, header->m_layoutHash, meta->m_layoutHash);
        }
        return false;
    }
    if (header->m_fieldCount != static_cast<uint32_t>(meta->m_fileFieldCount)) {
        if (errors) {
            ErrorDisplayFormatted(0x85100079, nullptr, 0, "Table %s has wrong number of columns (found %i, expected %i)",
                meta->m_name, header->m_fieldCount, meta->m_fileFieldCount);
        }
        return false;
    }
    if ((header->m_locale & localeBit) != localeBit) {
        if (errors) {
            ErrorDisplayFormatted(0x85100079, nullptr, 0, "Table %s has wrong locale (found %i, expected %i)",
                meta->m_name, header->m_locale, localeBit);
        }
        return false;
    }
    uint8_t flags = static_cast<uint8_t>(this->m_header->m_flags);
    if ((flags & 1) == meta->m_sparse) {
        uint8_t parent = meta->m_sparse != 0 && meta->m_hasParent != 0 ? 1 : 0;
        if ((flags >> 1 & 1) == parent && ((flags >> 2 & 1) != 0) == (meta->m_indexField == -1) && (flags & 3) != 2) {
            return true;
        }
    }
    if (errors) {
        ErrorDisplayFormatted(0x85100079, nullptr, 0, "Table '%s' has invalid metadata flags", meta->m_name);
    }
    this->Unload(true);
    return false;
}

// ref: FUN_14057d1d0
bool WowClientDB2_Base::LoadFieldData(CASFILE* file, bool errors) {
    uint64_t fields = this->m_fieldInfo.size();
    for (uint64_t i = 0; i < fields; i++) {
        if (this->m_fieldInfo[i].m_storage != 3) {
            continue;
        }
        if (this->m_pallets.size() < this->m_header->m_totalFieldCount) {
            this->m_pallets.reserve(this->m_header->m_totalFieldCount);
            while (this->m_pallets.size() < this->m_header->m_totalFieldCount) {
                new (&this->m_pallets.m_data[this->m_pallets.m_size]) TDBCVector<uint32_t>();
                this->m_pallets.m_size++;
            }
        }
        TDBCVector<uint32_t>& pallet = this->m_pallets[i];
        pallet.resize(this->m_fieldInfo[i].m_additionalSize >> 2, 0);
        if (!this->ReadExact(file, pallet.m_data, this->m_fieldInfo[i].m_additionalSize, errors)) {
            this->Unload(true);
            return false;
        }
        fields = this->m_fieldInfo.size();
    }
    for (uint64_t i = 0; i < fields; i++) {
        if (this->m_fieldInfo[i].m_storage != 4) {
            continue;
        }
        this->m_palletArrays.resize(this->m_header->m_totalFieldCount, nullptr);
        this->m_palletArrays[i] = static_cast<uint8_t*>(DB2Alloc(this->m_fieldInfo[i].m_additionalSize, 0x418));
        if (!this->ReadExact(file, this->m_palletArrays[i], this->m_fieldInfo[i].m_additionalSize, errors)) {
            this->Unload(true);
            return false;
        }
        fields = this->m_fieldInfo.size();
    }
    for (uint64_t i = 0; i < fields; i++) {
        if (this->m_fieldInfo[i].m_storage != 2) {
            continue;
        }
        if (this->m_commonData.size() < this->m_header->m_totalFieldCount) {
            this->m_commonData.reserve(this->m_header->m_totalFieldCount);
            while (this->m_commonData.size() < this->m_header->m_totalFieldCount) {
                DB2COMMONMAP* map = new (&this->m_commonData.m_data[this->m_commonData.m_size]) DB2COMMONMAP();
                map->Allocate();
                this->m_commonData.m_size++;
            }
        }
        DB2COMMONMAP& map = this->m_commonData[i];
        uint32_t count = this->m_fieldInfo[i].m_additionalSize >> 3;
        float wanted = static_cast<float>(count) / map.m_maxLoad + 0.5f;
        map.Rehash(static_cast<uint64_t>(wanted));
        TDBCVector<DB2DUPLICATE> pairs;
        pairs.resize(count, DB2DUPLICATE { 0, 0 });
        if (!this->ReadExact(file, pairs.m_data, this->m_fieldInfo[i].m_additionalSize, errors)) {
            this->Unload(true);
            return false;
        }
        for (DB2DUPLICATE& pair : pairs) {
            map.ReserveFor(map.m_used + 1);
            bool inserted;
            *map.Insert(static_cast<int32_t>(pair.m_newId), &inserted) = pair.m_sourceId;
        }
        fields = this->m_fieldInfo.size();
    }
    return true;
}

// ref: FUN_14057df40
bool WowClientDB2_Base::LoadEncryptedIds(CASFILE* file, bool errors) {
    for (uint32_t i = 0; i < this->m_header->m_sectionCount; i++) {
        uint64_t key = this->m_sections[i].m_tactKey;
        if (key == 0) {
            continue;
        }
        uint32_t count = 0;
        if (!this->ReadExact(file, &count, 4, errors)) {
            this->Unload(true);
            return false;
        }
        for (uint32_t k = 0; k < count; k++) {
            uint32_t id = 0;
            if (!this->ReadExact(file, &id, 4, errors)) {
                this->Unload(true);
                return false;
            }
            bool inserted;
            auto node = blz::chained_insert<DB2ENCRYPTEDNODE>(&this->m_encryption, id, &inserted, s_encryptedNodeTag, s_encryptedBucketTag);
            node->m_encrypted = 1;
            node->m_pending = 1;
            node->m_tactKey = key;
        }
    }
    return true;
}

// ref: FUN_14057e360
bool WowClientDB2_Base::LoadInternal(CASFILE* file, bool errors) {
    CASFILE** slot = this->FileSlot();
    if (slot != nullptr) {
        *slot = file;
    }
    CASFileSetPosition(file, 0, 0);
    this->m_header = static_cast<DB2HEADER*>(DB2Alloc(sizeof(DB2HEADER), 0x506));
    if (!this->ReadExact(file, this->m_header, sizeof(DB2HEADER), errors)) {
        return false;
    }
    if (!this->ValidateHeader(this->m_header, errors)) {
        return false;
    }
    DB2HEADER* header = this->m_header;
    this->m_sections = static_cast<DB2SECTIONHEADER*>(DB2Alloc(header->m_sectionCount * sizeof(DB2SECTIONHEADER), 0x511));
    if (!this->ReadExact(file, this->m_sections, header->m_sectionCount * sizeof(DB2SECTIONHEADER), errors)) {
        return false;
    }
    this->m_asyncSections.resize(header->m_sectionCount, DB2ASYNCSECTION {});
    uint32_t structuresSize = header->m_fieldCount << 2;
    this->m_fieldStructures = static_cast<uint32_t*>(DB2Alloc(structuresSize, 0x518));
    if (this->m_fieldStructures == nullptr) {
        return false;
    }
    if (!this->ReadExact(file, this->m_fieldStructures, structuresSize, errors)) {
        return false;
    }
    header = this->m_header;
    if (header->m_recordCount != 0 && header->m_fieldStorageInfoSize == 0) {
        return false;
    }
    if (header->m_fieldStorageInfoSize != 0) {
        this->m_fieldInfo.resize(header->m_totalFieldCount, DB2FIELDINFO {});
        if (!this->ReadExact(file, this->m_fieldInfo.m_data, header->m_fieldStorageInfoSize, errors)) {
            return false;
        }
    }
    if (!this->LoadFieldData(file, errors) || !this->LoadEncryptedIds(file, errors)) {
        return false;
    }

    header = this->m_header;
    uint32_t relationships = 0;
    uint32_t copies = 0;
    for (uint32_t i = 0; i < header->m_sectionCount; i++) {
        relationships += this->m_sections[i].m_relationshipSize;
        copies += this->m_sections[i].m_copyTableCount * 8;
    }
    uint32_t idList = this->m_meta->m_indexField == -1 ? header->m_recordCount << 2 : 0;
    int64_t position = CASFileSetPosition(file, 0, 1);
    uint64_t fileSize = CASFileGetSize(file);
    uint32_t expected = header->m_recordSize * header->m_recordCount + header->m_stringTableSize + static_cast<uint32_t>(position)
        + idList + relationships + copies;
    if (this->m_meta->m_sparse == 0 && static_cast<uint32_t>(fileSize) != expected) {
        if (errors) {
            char name[0x110];
            TableFileName(this, name, 0x104);
            ErrorDisplayFormatted(0x85100086, nullptr, 0, "File '%s' has unaccounted for bytes (fileSize %d, size from header: %d)",
                name, static_cast<int32_t>(fileSize), expected);
        }
        this->Unload(true);
        return false;
    }
    this->m_packedDataOffset = header->m_packedDataOffset;
    if (header->m_recordCount != 0 && this->m_meta->m_sparse == 0) {
        uint32_t stride = header->m_recordSize;
        this->m_data = static_cast<uint8_t*>(DB2Alloc(header->m_stringTableSize + 8 + header->m_recordCount * stride, 0x560));
        this->m_stride = stride;
        if (this->m_data == nullptr) {
            return false;
        }
    }

    TDBCVector<int32_t> ids;
    TDBCVector<DB2DUPLICATE> duplicates;
    TDBCVector<DBC2INDEXDATA> relations;
    bool loaded = true;
    for (uint32_t i = 0; i < this->m_header->m_sectionCount; i++) {
        if (this->m_sections[i].m_tactKey != 0) {
            // DAT_142e8e2a8 (whether a key is known) is not set: an encrypted section waits.
            continue;
        }
        CDataStore store;
        store.m_base = 0;
        store.m_alloc = 0;
        bool ok = this->ReadSection(&store, file, i, errors) && this->CopySection(&store, errors, i)
            && this->ReadSectionExtra(&store, errors, i);
        if (ok && this->m_meta->m_indexField == -1 && (this->m_sections[i].m_recordCount & 0x3FFFFFFF) != 0) {
            uint32_t offset = 0;
            for (uint32_t k = 0; k < this->m_header->m_sectionCount && k != i; k++) {
                offset += this->m_sections[k].m_idListSize;
            }
            ids.resize(this->m_header->m_recordCount, -1);
            store.GetData(ids.m_data + (offset >> 2), this->m_sections[i].m_idListSize);
        }
        ok = ok && this->ReadCopyTable(&store, errors, i, &duplicates) && this->ReadRelationships(&store, errors, i, &relations);
        if (!ok) {
            if (errors) {
                char name[0x110];
                TableFileName(this, name, 0x104);
                ErrorDisplayFormatted(0x85100086, nullptr, 0, "Table '%s' failed to load the table data", name);
            }
            this->Unload(true);
            loaded = false;
            break;
        }
        this->m_asyncSections[i].m_loaded = 1;
    }
    if (loaded) {
        loaded = this->FinalizeLoad(&duplicates, &ids, &relations, errors);
    }
    for (DBC2INDEXDATA& data : relations) {
        data.m_entries.release();
    }
    return loaded;
}

// ref: FUN_140580140
bool WowClientDB2_Base::ReadSection(CDataStore* store, CASFILE* file, uint32_t section, bool errors) {
    uint32_t size = this->SectionSize(section);
    uint32_t offset = this->SectionOffset(section);
    CASFileSetPosition(file, offset, 0);
    uint8_t* buffer = store->GetBufferForWrite(size);
    bool ok = this->ReadExact(file, buffer, size, errors);
    if (ok) {
        store->Finalize();
    }
    return ok;
}

// ref: FUN_14057f000
bool WowClientDB2_Base::CopySection(CDataStore* store, bool errors, uint32_t section) {
    (void)errors;
    uint32_t stride = this->m_header->m_recordSize;
    uint32_t strings = stride * this->m_header->m_recordCount;
    uint32_t records = 0;
    for (uint32_t i = 0; i < this->m_header->m_sectionCount && i != section; i++) {
        strings += this->m_sections[i].m_stringTableSize;
        records += stride * this->m_sections[i].m_recordCount;
    }
    uint32_t stringSize = this->m_sections[section].m_stringTableSize;
    store->GetData(this->m_data + records, stride * this->m_sections[section].m_recordCount);
    store->GetData(this->m_data + strings, stringSize);
    return true;
}

// ref: FUN_14026bea0
bool WowClientDB2_Base::ReadSectionExtra(CDataStore*, bool, uint32_t) {
    return true;
}

// ref: FUN_14057ddc0
bool WowClientDB2_Base::ReadCopyTable(CDataStore* store, bool errors, uint32_t section, TDBCVector<DB2DUPLICATE>* duplicates) {
    (void)errors;
    uint32_t count = this->m_sections[section].m_copyTableCount;
    if (count == 0) {
        return true;
    }
    uint64_t old = duplicates->size();
    duplicates->resize(old + count, DB2DUPLICATE { 0xFFFFFFFF, 0xFFFFFFFF });
    store->GetData(duplicates->m_data + old, count << 3);
    if (old != 0) {
        // FUN_140576da0: by source, then by the new id.
        std::sort(duplicates->begin(), duplicates->end(), [](const DB2DUPLICATE& a, const DB2DUPLICATE& b) {
            if (a.m_sourceId != b.m_sourceId) {
                return static_cast<int32_t>(a.m_sourceId) < static_cast<int32_t>(b.m_sourceId);
            }
            return static_cast<int32_t>(a.m_newId) < static_cast<int32_t>(b.m_newId);
        });
    }
    return true;
}

// ref: FUN_14057ec40
bool WowClientDB2_Base::ReadRelationships(CDataStore* store, bool errors, uint32_t section, TDBCVector<DBC2INDEXDATA>* data) {
    (void)errors;
    uint32_t columns = this->m_header->m_lookupColumnCount;
    while (data->size() < columns) {
        data->reserve(columns);
        DBC2INDEXDATA* fresh = new (&data->m_data[data->m_size]) DBC2INDEXDATA();
        fresh->m_count = 0;
        fresh->m_minId = 0x7FFFFFFF;
        fresh->m_maxId = static_cast<int32_t>(0x80000000);
        fresh->m_pad0c = 0;
        data->m_size++;
    }
    for (uint32_t c = 0; c < columns; c++) {
        DBC2INDEXDATA& column = (*data)[c];
        uint32_t start = column.m_count;
        struct {
            uint32_t m_count;
            int32_t m_minId;
            int32_t m_maxId;
        } block;
        store->GetData(&block, 0xc);
        if (block.m_count == 0) {
            continue;
        }
        column.m_count += block.m_count;
        if (block.m_minId < column.m_minId) {
            column.m_minId = block.m_minId;
        }
        if (column.m_maxId < block.m_maxId) {
            column.m_maxId = block.m_maxId;
        }
        column.m_entries.resize(column.m_count, DBC2INDEXENTRY { 0, 0 });
        store->GetData(column.m_entries.m_data + start, block.m_count << 3);
        uint32_t base = 0;
        for (uint32_t k = 0; k < this->m_header->m_sectionCount && k != section; k++) {
            base += this->m_sections[k].m_recordCount;
        }
        for (uint32_t k = 0; k < block.m_count; k++) {
            column.m_entries[start + k].m_recordIndex += base;
        }
    }
    return true;
}

// ref: FUN_14022c3f0
uint32_t WowClientDB2_Base::SparseCount() {
    return 0;
}

void WowClientDB2_Base::CallbackD0() {
    FROST_UNIMPLEMENTED();
}

void WowClientDB2_Base::CallbackD8() {
    FROST_UNIMPLEMENTED();
}

void WowClientDB2_Base::CallbackE0() {
    FROST_UNIMPLEMENTED();
}

void WowClientDB2_Base::CallbackE8() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405820c0
void* WowClientDB2_Base::DeserializeRecord(CDataStore* store) {
    (void)store;
    FROST_UNIMPLEMENTED();  // a hotfix record from the server
    return nullptr;
}

// ref: FUN_1405809e0
void WowClientDB2_Base::SetRecord(uint32_t id, void* record, void* previous, int32_t parent) {
    (void)id;
    (void)record;
    (void)previous;
    (void)parent;
    FROST_UNIMPLEMENTED();  // patching a record in
}

// ref: FUN_140580dd0
void WowClientDB2_Base::SetLookupRecord(int32_t, void*, void*) {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140580d40
void WowClientDB2_Base::UpdateParent(void*, int32_t, uint32_t, void*) {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140580b50
void WowClientDB2_Base::UpdateIndexed(void*, void*) {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140580eb0
void WowClientDB2_Base::Slot118() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14057baf0
uint32_t WowClientDB2_Base::SectionSize(uint32_t section) {
    const DB2SECTIONHEADER* s = &this->m_sections[section];
    uint32_t idList = this->m_meta->m_indexField == -1 ? s->m_idListSize : 0;
    return s->m_relationshipSize + s->m_copyTableCount * 8 + idList + s->m_stringTableSize
        + this->m_header->m_recordSize * s->m_recordCount;
}

// ref: FUN_14057bad0
uint32_t WowClientDB2_Base::SectionOffset(uint32_t section) {
    return this->m_sections[section].m_fileOffset;
}

// ref: FUN_14057ba10
CASFILE* WowClientDB2_Base::OpenFile(bool errors) {
    CASFILE* file = nullptr;
    int32_t fileDataId = this->m_meta->m_fileDataId;
    if (!CASFileOpenById(fileDataId, &file, 0)) {
        if (errors) {
            char error[0x100];
            CASFormatLastError(error, sizeof(error));
            char name[0x110];
            GxShaderName(fileDataId, name, 0x104);
            ErrorDisplayFormatted(0x85100086, nullptr, 0, "Unable to open %s: %s", name, error);
        }
        this->Unload(true);
        file = nullptr;
    }
    return file;
}

// ref: FUN_140581800
void WowClientDB2_Base::CloseFile2(CASFILE* file) {
    this->CloseFile(file);
}

// ref: FUN_1405814f0
bool WowClientDB2_Base::FinalizeLoad(TDBCVector<DB2DUPLICATE>* duplicates, TDBCVector<int32_t>* ids,
    TDBCVector<DBC2INDEXDATA>* relationships, bool errors) {
    uint32_t copies = static_cast<uint32_t>(duplicates->size());
    DB2HEADER* header = this->m_header;
    uint32_t records = header->m_recordCount;
    this->m_recordCount = records;
    this->m_totalCount = records + copies;
    this->m_minId = header->m_minId;
    this->m_maxId = header->m_maxId;
    this->m_records = records != 0 ? this->m_data : nullptr;
    this->m_liveCount = copies;
    for (uint32_t i = 0; i < header->m_sectionCount; i++) {
        if (this->m_asyncSections[i].m_loaded != 0) {
            this->m_liveCount += this->m_sections[i].m_recordCount;
        }
    }

    DB2LOOKUPINIT init;
    std::memset(&init, 0, sizeof(init));
    init.m_records = this->m_records;
    init.m_records2 = nullptr;
    init.m_deleted = &this->m_deleted;
    init.m_name = this->m_meta->m_name;
    init.m_order = this->m_meta->m_orderPtr;
    init.m_stride = this->m_stride;
    init.m_stride2 = static_cast<uint32_t>(this->m_meta->m_recordSize);
    init.m_minId = static_cast<uint32_t>(this->m_minId);
    init.m_maxId = static_cast<uint32_t>(this->m_maxId);
    init.m_recordCount = records;
    init.m_indexCount = records;
    init.m_copyCount = copies;
    init.m_recordCount2 = 0;
    init.m_sparseCount = this->SparseCount();
    if (this->m_recordCount != 0) {
        int32_t field = this->m_meta->m_indexField;
        if (field == -1) {
            init.m_idPacked &= ~1;
            init.m_idOffsetBits = 0;
            init.m_idSizeBits = 0;
        } else {
            init.m_idPacked |= 1;
            init.m_idOffsetBits = this->m_fieldInfo[field].m_offsetBits;
            init.m_idSizeBits = this->m_fieldInfo[field].m_sizeBits;
        }
    }
    this->m_lookup = WowDB2LookupCreate(this->m_meta->m_sparse != 0 ? 0x10 : 0x1f, &init);

    if (this->m_meta->m_hasParent != 0) {
        if (relationships->size() == 0) {
            DBC2INDEXDATA empty;
            empty.m_count = 0;
            empty.m_minId = 0;
            empty.m_maxId = 0;
            empty.m_pad0c = 0;
            this->BuildParentLookup(&empty);
        } else {
            this->BuildParentLookup(&(*relationships)[0]);
        }
    }
    if (this->m_recordCount != 0 && this->m_meta->m_sparse == 0) {
        this->BuildIdLookup(duplicates, ids);
    }
    if (this->m_meta->m_indexedFieldCount != 0) {
        this->BuildIndexedFields();
    }
    if (!this->BuildUniqueKeys(errors)) {
        this->Unload(true);
        return false;
    }
    this->ApplyPendingPatches();
    this->m_loaded = 1;
    for (auto callback : this->m_loadCallbacks) {
        if (callback != nullptr) {
            callback(0);
        }
    }
    return true;
}

// ref: FUN_14057a0a0
void WowClientDB2_Base::BuildParentLookup(DBC2INDEXDATA* data) {
    if (this->m_parentLookup != nullptr) {
        return;
    }
    DB2PARENTINIT init;
    std::memset(&init, 0, sizeof(init));
    init.m_count = data->m_count;
    init.m_stride = this->m_stride;
    init.m_minParent = data->m_minId;
    init.m_maxParent = data->m_maxId;
    init.m_records = this->m_records;
    init.m_order = this->m_meta->m_order;
    init.m_sparse = this->m_meta->m_sparse;
    this->m_parentLookup = WowDB2ParentLookupCreate(&init);
    for (DBC2INDEXENTRY& entry : data->m_entries) {
        this->m_parentLookup->Add(static_cast<int32_t>(entry.m_foreignId), entry.m_recordIndex);
    }
    this->m_parentLookup->Build();
}

// ref: FUN_14057a170
void WowClientDB2_Base::BuildIdLookup(TDBCVector<DB2DUPLICATE>* duplicates, TDBCVector<int32_t>* ids) {
    if (this->m_records != nullptr && this->m_recordCount != 0) {
        if (ids->size() == 0) {
            uint32_t index = 0;
            for (uint32_t section = 0; section < this->m_header->m_sectionCount; section++) {
                uint32_t count = this->m_sections[section].m_recordCount;
                if (this->m_asyncSections[section].m_loaded != 0 && count != 0) {
                    for (uint32_t k = 0; k < count; k++) {
                        uint32_t id = this->FieldValue(this->m_records + static_cast<uint64_t>(index * this->m_stride),
                            static_cast<uint32_t>(this->m_meta->m_indexField), 0);
                        this->m_lookup->SetIndex(id, index);
                        index++;
                    }
                    index -= count;
                }
                index += count;
            }
        } else {
            for (uint32_t i = 0; i < this->m_recordCount; i++) {
                int32_t id = (*ids)[i];
                if (id != -1) {
                    this->m_lookup->SetIndex(static_cast<uint32_t>(id), i);
                }
            }
        }
    }
    if (duplicates->size() != 0) {
        this->m_lookup->ApplyCopies(reinterpret_cast<const uint32_t*>(duplicates->m_data));
    }
    this->m_lookup->Finish();
}

// ref: FUN_1405798f0
void WowClientDB2_Base::BuildIndexedFields() {
    for (DB2INDEXMAP& map : this->m_indexMaps) {
        SMemFree(map.m_entries, s_dbcTypesFile, 0x103, 0);
    }
    this->m_indexMaps.clear();
    for (TDBCVector<void*>& records : this->m_indexRecords) {
        records.release();
    }
    this->m_indexRecords.clear();
    for (int32_t field = 0; field < this->m_meta->m_fileFieldCount; field++) {
        if ((this->m_meta->m_fileFieldFlags[field] & 1) == 0) {
            continue;
        }
        if (this->m_indexRecords.size() == this->m_indexRecords.capacity()) {
            this->m_indexRecords.reserve(this->m_indexRecords.capacity() + 1);
        }
        TDBCVector<void*>* records = new (&this->m_indexRecords.m_data[this->m_indexRecords.m_size]) TDBCVector<void*>();
        this->m_indexRecords.m_size++;
        if (this->m_indexMaps.size() == this->m_indexMaps.capacity()) {
            this->m_indexMaps.reserve(this->m_indexMaps.capacity() + 1);
        }
        DB2INDEXMAP* map = new (&this->m_indexMaps.m_data[this->m_indexMaps.m_size]) DB2INDEXMAP();
        map->m_entries = static_cast<DB2INDEXMAP::Entry*>(SMemAlloc(0x60, 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
        for (uint64_t i = 0; i < map->m_capacity; i++) {
            map->m_entries[i].m_key = 0x80000001;
        }
        this->m_indexMaps.m_size++;
        this->BuildIndexedField(static_cast<uint32_t>(field), map, records);
    }
    this->m_indexedValid = 1;
}

namespace {

// FUN_140582e00 / FUN_140585140: the indexed fields' map rehash (FNV-1a keys).
void IndexMapRehash(DB2INDEXMAP* map, uint64_t count) {
    float live = static_cast<float>(map->m_used - map->m_deleted);
    uint64_t needed = static_cast<uint64_t>(live / map->m_maxLoad);
    if (static_cast<float>(needed) * map->m_maxLoad < live) {
        needed++;
    }
    if (needed < count) {
        needed = count;
    }
    uint64_t capacity = 8;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (capacity == map->m_capacity && map->m_deleted == 0) {
        return;
    }
    auto entries = static_cast<DB2INDEXMAP::Entry*>(SMemAlloc(capacity * sizeof(DB2INDEXMAP::Entry), 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
    for (uint64_t i = 0; i < capacity; i++) {
        entries[i].m_key = 0x80000001;
    }
    for (uint64_t i = 0; i < map->m_capacity; i++) {
        DB2INDEXMAP::Entry* old = &map->m_entries[i];
        if (old->m_key == 0x80000001 || (map->m_deleted != 0 && old->m_key == 0x80000000)) {
            continue;
        }
        uint64_t index = IndexMapHash(old->m_key) & (capacity - 1);
        for (uint64_t step = 1; entries[index].m_key != 0x80000001; step++) {
            index = (index + step) & (capacity - 1);
        }
        entries[index] = *old;
    }
    SMemFree(map->m_entries, s_dbcTypesFile, 0x103, 0);
    map->m_used -= map->m_deleted;
    map->m_deleted = 0;
    map->m_entries = entries;
    map->m_capacity = capacity;
}

}

// ref: FUN_140579b90
void WowClientDB2_Base::BuildIndexedField(uint32_t field, DB2INDEXMAP* map, TDBCVector<void*>* records) {
    for (uint64_t i = 0; i < map->m_capacity; i++) {
        map->m_entries[i].m_key = 0x80000001;
    }
    map->m_used = 0;
    map->m_deleted = 0;
    records->m_size = 0;
    int32_t count = static_cast<int32_t>(this->m_liveCount);
    if (count == 0) {
        return;
    }
    struct ROW {
        int32_t m_value;
        uint32_t m_id;
        void* m_record;
    };
    // Sized for every live record (the reference sorts the whole array); zeroed so that a
    // short iteration sorts zeros rather than stack garbage.
    TDBCVector<ROW> rows;
    rows.resize(static_cast<uint64_t>(count), ROW { 0, 0, nullptr });
    uint32_t row = 0;
    DB2LOOKUPITER it;
    DB2LOOKUPITER end;
    WowDB2LookupBegin(this->m_lookup, &it);
    WowDB2LookupEnd(this->m_lookup, &end);
    while (it.m_index != end.m_index && row < rows.size()) {
        void* record = it.m_record;
        uint32_t id = this->m_lookup->Id(&it);
        rows[row].m_value = static_cast<int32_t>(this->FieldValue(static_cast<uint8_t*>(record), field, 0));
        rows[row].m_id = id;
        rows[row].m_record = record;
        row++;
        this->m_lookup->Next(&it);
    }
    // FUN_140577130: by value, then id.
    std::sort(rows.begin(), rows.end(), [](const ROW& a, const ROW& b) {
        if (a.m_value != b.m_value) {
            return a.m_value < b.m_value;
        }
        return static_cast<int32_t>(a.m_id) < static_cast<int32_t>(b.m_id);
    });
    uint32_t distinct = 1;
    for (int32_t i = 1; i < count; i++) {
        if (rows[i].m_value != rows[i - 1].m_value) {
            distinct++;
        }
    }
    float wanted = static_cast<float>(distinct) / map->m_maxLoad + 0.5f;
    IndexMapRehash(map, static_cast<uint64_t>(wanted));
    records->resize(static_cast<uint64_t>(count), nullptr);
    int64_t out = 0;
    int32_t start = 0;
    while (out < count) {
        int32_t next = start;
        while (out < count && rows[next].m_value == rows[start].m_value) {
            (*records)[out] = rows[next].m_record;
            next++;
            out++;
        }
        DBMETAORDER order = this->m_meta->m_order;
        if (order != nullptr) {
            // FUN_140576a60
            std::sort(records->begin() + start, records->begin() + next, [order](void* a, void* b) { return order(a, b); });
        }
        uint32_t key = static_cast<uint32_t>(rows[start].m_value);
        float load = static_cast<float>(map->m_used + 1) / static_cast<float>(map->m_capacity);
        if (map->m_maxLoad < load) {
            IndexMapRehash(map, 0);
        }
        uint64_t mask = map->m_capacity - 1;
        uint64_t index = IndexMapHash(key) & mask;
        DB2INDEXMAP::Entry* removed = nullptr;
        DB2INDEXMAP::Entry* slot = nullptr;
        for (uint64_t step = 1;; step++) {
            DB2INDEXMAP::Entry* probe = &map->m_entries[index];
            if (probe->m_key == 0x80000001) {
                break;
            }
            if (probe->m_key == 0x80000000) {
                if (removed == nullptr) {
                    removed = probe;
                }
            } else if (probe->m_key == key) {
                slot = probe;
                break;
            }
            index = (index + step) & mask;
        }
        if (slot == nullptr) {
            slot = removed != nullptr ? removed : &map->m_entries[index];
            slot->m_key = key;
            slot->m_value = DB2INDEXRANGE { 0, 0 };
            map->m_used++;
        }
        slot->m_value.m_first = static_cast<uint32_t>(start);
        slot->m_value.m_count = static_cast<uint32_t>(next - start);
        start = next;
    }
}

namespace {

uint64_t StringKeyHash(const char* key) {
    return SStrHashHT(key);
}

void StringKeyRehash(DB2STRINGKEYMAP* map) {
    float live = static_cast<float>(map->m_used - map->m_deleted);
    uint64_t needed = static_cast<uint64_t>(live / map->m_maxLoad);
    if (static_cast<float>(needed) * map->m_maxLoad < live) {
        needed++;
    }
    uint64_t capacity = 8;
    while (capacity < needed) {
        capacity *= 2;
    }
    if (capacity == map->m_capacity && map->m_deleted == 0) {
        return;
    }
    auto entries = static_cast<DB2STRINGKEYMAP::Entry*>(SMemAlloc(capacity * sizeof(DB2STRINGKEYMAP::Entry), 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
    for (uint64_t i = 0; i < capacity; i++) {
        entries[i].m_key = nullptr;
    }
    for (uint64_t i = 0; i < map->m_capacity; i++) {
        DB2STRINGKEYMAP::Entry* old = &map->m_entries[i];
        if (old->m_key == nullptr || (map->m_deleted != 0 && old->m_key == reinterpret_cast<const char*>(1))) {
            continue;
        }
        uint64_t index = StringKeyHash(old->m_key) & (capacity - 1);
        for (uint64_t step = 1; entries[index].m_key != nullptr; step++) {
            index = (index + step) & (capacity - 1);
        }
        entries[index] = *old;
    }
    SMemFree(map->m_entries, s_dbcTypesFile, 0x103, 0);
    map->m_used -= map->m_deleted;
    map->m_deleted = 0;
    map->m_entries = entries;
    map->m_capacity = capacity;
}

// FUN_1405830e0: the slot for `key`, made when missing; false when it was already there.
bool StringKeyInsert(DB2STRINGKEYMAP* map, const char* key, void* value) {
    float load = static_cast<float>(map->m_used + 1) / static_cast<float>(map->m_capacity);
    if (map->m_maxLoad < load) {
        StringKeyRehash(map);
    }
    uint64_t mask = map->m_capacity - 1;
    uint64_t index = StringKeyHash(key) & mask;
    DB2STRINGKEYMAP::Entry* removed = nullptr;
    for (uint64_t step = 1;; step++) {
        DB2STRINGKEYMAP::Entry* slot = &map->m_entries[index];
        if (slot->m_key == nullptr) {
            break;
        }
        if (slot->m_key == reinterpret_cast<const char*>(1)) {
            if (removed == nullptr) {
                removed = slot;
            }
        } else if (_stricmp(slot->m_key, key) == 0) {
            return false;
        }
        index = (index + step) & mask;
    }
    DB2STRINGKEYMAP::Entry* slot = removed != nullptr ? removed : &map->m_entries[index];
    slot->m_key = key;
    slot->m_value = value;
    map->m_used++;
    return true;
}

}

// ref: FUN_14057a2e0
bool WowClientDB2_Base::BuildUniqueKeys(bool errors) {
    this->m_uniqueKeysValid = 1;
    while (this->m_intKeys.size() < static_cast<uint64_t>(this->m_meta->m_intKeyCount)) {
        this->m_intKeys.reserve(this->m_meta->m_intKeyCount);
        DB2INTKEYMAP* map = new (&this->m_intKeys.m_data[this->m_intKeys.m_size]) DB2INTKEYMAP();
        map->Allocate();
        this->m_intKeys.m_size++;
    }
    while (this->m_stringKeys.size() < static_cast<uint64_t>(this->m_meta->m_stringKeyCount)) {
        this->m_stringKeys.reserve(this->m_meta->m_stringKeyCount);
        DB2STRINGKEYMAP* map = new (&this->m_stringKeys.m_data[this->m_stringKeys.m_size]) DB2STRINGKEYMAP();
        map->m_entries = static_cast<DB2STRINGKEYMAP::Entry*>(SMemAlloc(0x80, 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
        for (uint64_t i = 0; i < map->m_capacity; i++) {
            map->m_entries[i].m_key = nullptr;
        }
        this->m_stringKeys.m_size++;
    }
    for (int32_t k = 0; k < this->m_meta->m_intKeyCount; k++) {
        DB2INTKEYMAP& map = this->m_intKeys[k];
        for (uint64_t i = 0; i < map.m_capacity; i++) {
            map.m_entries[i].m_key = static_cast<int32_t>(0x80000001);
        }
        map.m_used = 0;
        map.m_deleted = 0;
        const DBMETAINTKEY* key = &this->m_meta->m_intKeys[k];
        DB2LOOKUPITER it;
        DB2LOOKUPITER end;
        WowDB2LookupBegin(this->m_lookup, &it);
        WowDB2LookupEnd(this->m_lookup, &end);
        while (it.m_index != end.m_index) {
            void* record = it.m_record;
            uint32_t id = this->m_lookup->Id(&it);
            int32_t value = static_cast<int32_t>(this->FieldValue(static_cast<uint8_t*>(record), static_cast<uint32_t>(key->m_field), 0));
            if (key->m_hasIgnore == 0 || value != key->m_ignore) {
                bool inserted;
                void** slot = map.Insert(value, &inserted);
                if (!inserted) {
                    if (errors) {
                        ErrorDisplayFormatted(0x85100086, nullptr, 0, "Duplicate 'unique key' %d found for ID %d in table %s",
                            value, id, this->m_meta->m_name);
                    }
                    this->m_uniqueKeysValid = 0;
                } else {
                    *slot = record;
                }
            }
            this->m_lookup->Next(&it);
        }
    }
    for (int32_t k = 0; k < this->m_meta->m_stringKeyCount; k++) {
        DB2STRINGKEYMAP& map = this->m_stringKeys[k];
        for (uint64_t i = 0; i < map.m_capacity; i++) {
            map.m_entries[i].m_key = nullptr;
        }
        map.m_used = 0;
        map.m_deleted = 0;
        const DBMETASTRINGKEY* key = &this->m_meta->m_stringKeys[k];
        uint16_t position = static_cast<uint16_t>(this->m_fieldStructures[key->m_field] >> 16);
        DB2LOOKUPITER it;
        DB2LOOKUPITER end;
        WowDB2LookupBegin(this->m_lookup, &it);
        WowDB2LookupEnd(this->m_lookup, &end);
        while (it.m_index != end.m_index) {
            auto record = static_cast<uint8_t*>(it.m_record);
            uint32_t id = this->m_lookup->Id(&it);
            int32_t offset;
            std::memcpy(&offset, record + position, 4);
            const char* text = offset != 0 ? reinterpret_cast<const char*>(record + position + offset) : "";
            if (key->m_hasIgnore == 0 || _stricmp(text, key->m_ignore) != 0) {
                if (!StringKeyInsert(&map, text, record)) {
                    if (errors) {
                        ErrorDisplayFormatted(0x85100086, nullptr, 0, "Duplicate 'unique key' %s found for ID %d in table %s",
                            text, id, this->m_meta->m_name);
                    }
                    this->m_uniqueKeysValid = 0;
                }
            }
            this->m_lookup->Next(&it);
        }
    }
    return this->m_uniqueKeysValid != 0;
}

// ref: FUN_14057b640
void* WowClientDB2_Base::GetRecord(uint32_t id, bool request, bool* local) {
    *local = true;
    bool invalid = this->m_meta->m_zeroIdValid == 0 ? id == 0 : id == 0xFFFFFFFF;
    if (invalid) {
        return nullptr;
    }
    void* record = nullptr;
    if (static_cast<int32_t>(id) >= this->m_minId && static_cast<int32_t>(id) <= this->m_maxId && this->m_lookup != nullptr) {
        record = this->m_lookup->Find(id);
    }
    if (record != nullptr) {
        return record != &this->m_deleted ? record : nullptr;
    }
    if (this->m_pendingPatches.m_size != 0) {
        FROST_UNIMPLEMENTED();  // a pending patch's request callback (DAT_142e8e2e0)
    }
    if (s_hotfixRequests && HotfixRequestsAvailable()
        && (this->m_meta->m_sparse != 0 || this->m_meta->m_a9 != 0 || request)) {
        *local = false;
        FROST_UNIMPLEMENTED();  // queueing a hotfix request for the record
    }
    return nullptr;
}

// The hotfix requests object exists and can ask the server (DAT_142e8e258, its slot 8).
bool DB2HotfixRequestsAvailable() {
    return s_hotfixRequests && HotfixRequestsAvailable();
}

// ref: FUN_140579030
void WowClientDB2_Base::AddLoadCallback(void (*callback)(int32_t event)) {
    this->m_loadCallbacks.push_back(callback);
}

// The reference stores the context as a 32-bit value beside the callback.
// ref: FUN_1405791e0
void WowClientDB2_Base::AddPatchCallback(void (*callback)(uint32_t, uint32_t, uint32_t, void*, void*), void* context) {
    DB2PATCHCALLBACK entry = { callback, context };
    this->m_patchCallbacks.push_back(entry);
}

// Every watcher with this callback goes; the last entries are moved into the gaps.
// ref: FUN_14057ae20
void WowClientDB2_Base::RemovePatchCallback(void (*callback)(uint32_t, uint32_t, uint32_t, void*, void*)) {
    uint64_t count = this->m_patchCallbacks.size();
    uint64_t i = 0;
    while (i < count) {
        if (this->m_patchCallbacks[i].m_callback != callback) {
            i++;
            continue;
        }
        while (count > i && this->m_patchCallbacks[count - 1].m_callback == callback) {
            count--;
        }
        if (count > i) {
            this->m_patchCallbacks[i] = this->m_patchCallbacks[count - 1];
            count--;
            i++;
        }
    }
    this->m_patchCallbacks.resize(count);
}

// ref: FUN_140581790
bool WowClientDB2_Base::HasRecord(int32_t id) {
    if (this->m_minId <= id && id <= this->m_maxId && this->m_lookup != nullptr) {
        void* record = this->m_lookup->Find(static_cast<uint32_t>(id));
        if (record != nullptr && record != &this->m_deleted) {
            return true;
        }
    }
    return false;
}

// ref: FUN_14057b830
void* WowClientDB2_Base::GetRecordByIntKey(int32_t value, int32_t key) {
    if (this->m_meta->m_intKeyCount == 0) {
        return nullptr;
    }
    void** record = this->m_intKeys[key].Find(value);
    return record != nullptr ? *record : nullptr;
}

// ref: FUN_14057b920
void* WowClientDB2_Base::GetRecordByStringKey(const char* value, int32_t key) {
    DB2STRINGKEYMAP* map = &this->m_stringKeys[key];
    uint64_t mask = map->m_capacity - 1;
    uint64_t index = StringKeyHash(value) & mask;
    for (uint64_t step = 1; map->m_entries[index].m_key != nullptr; step++) {
        DB2STRINGKEYMAP::Entry* slot = &map->m_entries[index];
        if ((map->m_deleted == 0 || slot->m_key != reinterpret_cast<const char*>(1)) && _stricmp(slot->m_key, value) == 0) {
            return slot->m_value;
        }
        index = (index + step) & mask;
    }
    return nullptr;
}

// ref: FUN_140442370
void WowClientDB2::RecordCallback() {
}

// ref: FUN_14057bb40
WowClientDB2_Base* WowClientDB2GetByTableHash(uint32_t tableHash) {
    DB2REGISTRY* map = Registry();
    uint64_t mask = map->m_capacity - 1;
    uint64_t index = RegistryHash(tableHash) & mask;
    for (uint64_t step = 1; map->m_entries[index].m_key != 0; step++) {
        DB2REGISTRY::Entry* slot = &map->m_entries[index];
        if ((map->m_deleted == 0 || slot->m_key != 1) && slot->m_key == tableHash) {
            return slot->m_table;
        }
        index = (index + step) & mask;
    }
    return nullptr;
}

// ref: FUN_140351bf0's hotfix requests object
void WowClientDB2SetHotfixRequests(bool enabled) {
    s_hotfixRequests = enabled;
}

// ref: FUN_14057b590
int32_t DB2ParentCount(WowClientDB2_Base* table, int32_t parent) {
    WowDB2ParentLookup* lookup = table->m_parentLookup;
    if (lookup == nullptr) {
        return 0;
    }
    DB2PARENTITER it;
    DB2PARENTITER end;
    lookup->Begin(&it, parent);
    lookup->End(&end);
    int32_t count = 0;
    while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
        || it.m_source != end.m_source) {
        count++;
        lookup->Next(&it);
    }
    return count;
}
