#include "db/WowDB2Lookup.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

const char s_dbcTypesFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\DB\\DBCTypes.h";

namespace {

const char s_lookupHeader[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\DB\\WowDB2Lookup.h";
const char s_lookupFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\DB\\WowDB2Lookup.cpp";

// The engine's running totals of what the lookups cost (DAT_142e8e2ec, DAT_142e8e2f0).
uint32_t s_lookupBytes;
int32_t s_vectorBytes;

template <class T>
T* LookupAlloc(const char* file, int32_t line) {
    void* memory = SMemAlloc(sizeof(T), 0x24, file, SStrHashFnv1a(file), line, 0);
    return static_cast<T*>(memory);
}

// The overflow list's operations: a lower bound on the id.
DB2LOOKUPENTRY* OverflowLowerBound(TDBCVector<DB2LOOKUPENTRY>* list, uint32_t id) {
    DB2LOOKUPENTRY* first = list->begin();
    DB2LOOKUPENTRY* last = list->end();
    while (first != last) {
        DB2LOOKUPENTRY* middle = first + (last - first) / 2;
        if (middle->m_id < id) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    return first;
}

// ref: FUN_14059fc70
void* OverflowFind(TDBCVector<DB2LOOKUPENTRY>* list, uint32_t id) {
    DB2LOOKUPENTRY* entry = OverflowLowerBound(list, id);
    if (entry != list->end() && entry->m_id == id) {
        return entry->m_record;
    }
    return nullptr;
}

// ref: FUN_14059fbb0
void OverflowSet(TDBCVector<DB2LOOKUPENTRY>* list, uint32_t id, void* record) {
    DB2LOOKUPENTRY* entry = OverflowLowerBound(list, id);
    if (entry != list->end() && entry->m_id == id) {
        entry->m_record = record;
        return;
    }
    DB2LOOKUPENTRY* slot = list->insert_slot(static_cast<uint64_t>(entry - list->begin()));
    slot->m_id = id;
    slot->m_record = record;
}

// ref: FUN_14059fe30: clears the entry (it stays in the list) when it holds `record`.
bool OverflowClear(TDBCVector<DB2LOOKUPENTRY>* list, uint32_t id, void* record) {
    DB2LOOKUPENTRY* entry = OverflowLowerBound(list, id);
    if (entry != list->end() && entry->m_id == id && entry->m_record == record) {
        entry->m_record = nullptr;
        return true;
    }
    return false;
}

bool IsLive(WowDB2Lookup* lookup, void* record) {
    return record != nullptr && record != lookup->m_init.m_deleted;
}

}

WowDB2Lookup::WowDB2Lookup() {
    std::memset(&this->m_init, 0, sizeof(this->m_init));
}

// ref: FUN_14059f060 (its head; every strategy's copy constructor starts the same way)
WowDB2Lookup::WowDB2Lookup(const WowDB2Lookup& other) {
    this->m_init = other.m_init;
    this->m_lookups = other.m_lookups;
    this->m_repeats = other.m_repeats;
    this->m_lastId = other.m_lastId;
    this->m_74 = other.m_74;
    this->m_78 = other.m_78;
    this->m_dirty = 1;
    this->m_locks = other.m_locks;
}

WowDB2Lookup::~WowDB2Lookup() = default;

// ref: FUN_1405885d0
void WowDB2Lookup::Finish() {
    this->Rebuild();
}

// ref: FUN_140588760
void WowDB2Lookup::Rebuild() {
}

void WowDB2Lookup::CountLookup(uint32_t id) {
    this->m_lookups++;
    if (id == this->m_lastId) {
        this->m_repeats++;
    }
    this->m_lastId = id;
}

void* WowDB2Lookup::RecordAt(uint32_t index) {
    if (index < this->m_init.m_recordCount) {
        return this->m_init.m_records + static_cast<uint64_t>(index * this->m_init.m_stride);
    }
    uint32_t index2 = index - this->m_init.m_recordCount;
    if (index2 < this->m_init.m_recordCount2) {
        return this->m_init.m_records2 + static_cast<uint64_t>(index2 * this->m_init.m_stride2);
    }
    return nullptr;
}

void WowDB2Lookup::Changed() {
    if (this->m_locks == 0) {
        this->Rebuild();
    } else {
        this->m_dirty = 1;
    }
}

namespace {

// ---- Null --------------------------------------------------------------------------------

class WowDB2Lookup_Null : public WowDB2Lookup {
    public:
    TDBCVector<DB2LOOKUPENTRY> m_overflow;  // 0x80

    WowDB2Lookup_Null() = default;

    // ref: FUN_14059f060
    WowDB2Lookup_Null(const WowDB2Lookup_Null& other) : WowDB2Lookup(other) {
        DBCVectorCopy(&this->m_overflow, other.m_overflow);
    }

    // ref: FUN_14059fa40
    ~WowDB2Lookup_Null() override = default;

    // ref: FUN_140588550
    void* Find(uint32_t id) override {
        this->CountLookup(id);
        return OverflowFind(&this->m_overflow, id);
    }

    void SetIndex(uint32_t, uint32_t) override {
    }

    void ApplyCopies(const uint32_t*) override {
    }

    // ref: FUN_14026bea0
    bool SetIndex2(uint32_t, uint32_t) override {
        return true;
    }

    // ref: FUN_1405885e0
    void SetRecord(uint32_t id, void* record) override {
        OverflowSet(&this->m_overflow, id, record);
        if (this->m_locks == 0) {
            this->Rebuild();
        }
    }

    // ref: FUN_140588620
    bool ClearRecord(uint32_t id, void* record) override {
        bool cleared = OverflowClear(&this->m_overflow, id, record);
        if (cleared && this->m_locks == 0) {
            this->Rebuild();
        }
        return cleared;
    }

    // ref: FUN_140588730
    bool IsEmpty() override {
        return this->m_overflow.size() == 0;
    }

    // ref: FUN_140588460
    WowDB2Lookup* Clone() override;

    // ref: FUN_140588670
    void Next(DB2LOOKUPITER* it) override {
        int32_t index = it->m_index + 1;
        while (index < static_cast<int32_t>(this->m_overflow.size())) {
            void* record = this->m_overflow[index].m_record;
            if (IsLive(this, record)) {
                it->m_index = index;
                it->m_record = record;
                return;
            }
            index++;
        }
        it->m_index = -2;
        it->m_pad = 0;
        it->m_record = nullptr;
    }

    // ref: FUN_1405886f0
    void* Record(const DB2LOOKUPITER* it) override {
        return this->m_overflow[it->m_index].m_record;
    }

    // ref: FUN_140588710
    uint32_t Id(const DB2LOOKUPITER* it) override {
        return this->m_overflow[it->m_index].m_id;
    }

    // ref: FUN_140588740
    uint32_t OverflowCount() override {
        return static_cast<uint32_t>(this->m_overflow.size());
    }

    // ref: FUN_140588750
    uint64_t MemoryUsed() override {
        return this->m_overflow.size() * 0x10 + 0x18;
    }

    // ref: FUN_140588520
    void Init(const DB2LOOKUPINIT* init) override {
        this->m_init = *init;
    }

    // ref: FUN_140588770
    uint64_t Count() override {
        return this->m_overflow.size();
    }

    // ref: FUN_1405884f0
    static uint32_t Estimate(const DB2LOOKUPINIT* init) {
        if (init->m_sparseCount != 0) {
            return 0xFFFFFFFF;
        }
        return init->m_recordCount2 + init->m_copyCount + init->m_indexCount != 0 ? 0xFFFFFFFF : 0;
    }
};

WowDB2Lookup* WowDB2Lookup_Null::Clone() {
    auto copy = new (LookupAlloc<WowDB2Lookup_Null>(s_lookupHeader, 0x95)) WowDB2Lookup_Null(*this);
    copy->m_dirty = 1;
    if (copy->m_locks == 0) {
        copy->Rebuild();
    }
    return copy;
}

// ---- Vec ---------------------------------------------------------------------------------

class WowDB2Lookup_Vec : public WowDB2Lookup {
    public:
    TDBCVector<void*> m_slots;              // 0x80 a record per id in [min, max]
    TDBCVector<DB2LOOKUPENTRY> m_overflow;  // 0x98

    WowDB2Lookup_Vec() = default;

    // ref: FUN_14059f260
    WowDB2Lookup_Vec(const WowDB2Lookup_Vec& other) : WowDB2Lookup(other) {
        DBCVectorCopy(&this->m_slots, other.m_slots);
        DBCVectorCopy(&this->m_overflow, other.m_overflow);
    }

    // ref: FUN_14059fb20
    ~WowDB2Lookup_Vec() override = default;

    // ref: FUN_1405889b0
    void* Find(uint32_t id) override {
        this->CountLookup(id);
        if (this->m_init.m_minId <= id && id <= this->m_init.m_maxId) {
            return this->m_slots[id - this->m_init.m_minId];
        }
        return OverflowFind(&this->m_overflow, id);
    }

    // ref: FUN_140588a40
    void SetIndex(uint32_t id, uint32_t index) override {
        this->m_slots[id - this->m_init.m_minId] = this->m_init.m_records + static_cast<uint64_t>(index * this->m_init.m_stride);
    }

    // ref: FUN_140588a60
    void ApplyCopies(const uint32_t* pairs) override {
        if (pairs == nullptr) {
            return;
        }
        for (uint32_t i = 0; i < this->m_init.m_copyCount; i++) {
            this->m_slots[pairs[i * 2] - this->m_init.m_minId] = this->m_slots[pairs[i * 2 + 1] - this->m_init.m_minId];
        }
    }

    // ref: FUN_140588ac0
    bool SetIndex2(uint32_t id, uint32_t index) override {
        void** slot = &this->m_slots[id - this->m_init.m_minId];
        void* previous = *slot;
        *slot = this->m_init.m_records2 + static_cast<uint64_t>(index * this->m_init.m_stride2);
        return previous == nullptr;
    }

    // ref: FUN_140588af0
    void SetRecord(uint32_t id, void* record) override {
        if (id < this->m_init.m_minId || this->m_init.m_maxId < id) {
            OverflowSet(&this->m_overflow, id, record);
        } else {
            this->m_slots[id - this->m_init.m_minId] = record;
        }
        this->Changed();
    }

    // ref: FUN_140588b50: in range, a slot that already holds `record` is left alone and
    // reports nothing cleared, and any other slot is cleared -- the reverse of what every
    // other strategy does. Kept as the reference has it.
    bool ClearRecord(uint32_t id, void* record) override {
        if (id < this->m_init.m_minId || this->m_init.m_maxId < id) {
            if (!OverflowClear(&this->m_overflow, id, record)) {
                return false;
            }
        } else {
            void** slot = &this->m_slots[id - this->m_init.m_minId];
            if (*slot == record) {
                return false;
            }
            *slot = nullptr;
        }
        this->Changed();
        return true;
    }

    // ref: FUN_140588f80
    bool IsEmpty() override {
        return this->m_slots.size() == 0 && this->m_overflow.size() == 0;
    }

    // ref: FUN_1405887c0
    WowDB2Lookup* Clone() override;

    // ref: FUN_140588e60
    void Next(DB2LOOKUPITER* it) override {
        int32_t index = it->m_index + 1;
        int32_t slots = static_cast<int32_t>(this->m_slots.size());
        for (; index < slots; index++) {
            void* record = this->m_slots[index];
            if (IsLive(this, record)) {
                it->m_index = index;
                it->m_record = record;
                return;
            }
        }
        for (; index < static_cast<int32_t>(this->m_overflow.size()) + slots; index++) {
            void* record = this->m_overflow[index - slots].m_record;
            if (IsLive(this, record)) {
                it->m_index = index;
                it->m_record = record;
                return;
            }
        }
        it->m_index = -2;
        it->m_pad = 0;
        it->m_record = nullptr;
    }

    // ref: FUN_140588f10
    void* Record(const DB2LOOKUPITER* it) override {
        int32_t slots = static_cast<int32_t>(this->m_slots.size());
        if (it->m_index < slots) {
            return this->m_slots[it->m_index];
        }
        return this->m_overflow[it->m_index - slots].m_record;
    }

    // ref: FUN_140588f50
    uint32_t Id(const DB2LOOKUPITER* it) override {
        int32_t slots = static_cast<int32_t>(this->m_slots.size());
        if (it->m_index < slots) {
            return this->m_init.m_minId + it->m_index;
        }
        return this->m_overflow[it->m_index - slots].m_id;
    }

    // ref: FUN_1405891c0
    uint32_t OverflowCount() override {
        return static_cast<uint32_t>(this->m_overflow.size());
    }

    // ref: FUN_1405891d0
    uint64_t MemoryUsed() override {
        return (this->m_slots.size() + this->m_overflow.size() * 2) * 8 + 200;
    }

    // ref: FUN_140588880
    void Init(const DB2LOOKUPINIT* init) override {
        this->m_init = *init;
        if (init->m_recordCount2 + init->m_copyCount + init->m_recordCount != 0) {
            this->m_slots.resize(init->m_maxId - init->m_minId + 1, nullptr);
        }
    }

    // ref: FUN_140589240
    uint64_t Count() override {
        return this->m_overflow.size() + this->m_slots.size();
    }

    // ref: FUN_140588850
    static uint32_t Estimate(const DB2LOOKUPINIT* init) {
        if (init->m_sparseCount != 0) {
            return 0xFFFFFFFF;
        }
        if (init->m_recordCount2 + init->m_copyCount + init->m_recordCount == 0) {
            return 0;
        }
        return (init->m_maxId - init->m_minId) * 8 + 8;
    }
};

WowDB2Lookup* WowDB2Lookup_Vec::Clone() {
    auto copy = new (LookupAlloc<WowDB2Lookup_Vec>(s_lookupHeader, 0x95)) WowDB2Lookup_Vec(*this);
    if (copy->m_locks == 0) {
        copy->Rebuild();
    }
    return copy;
}

// ---- IX8 / IX16 / IX32 -------------------------------------------------------------------

// A record index per id in [min, max]: ~1 empty, ~0 in the overflow list.
template <class IndexT>
class WowDB2Lookup_IX : public WowDB2Lookup {
    public:
    static constexpr IndexT EMPTY = static_cast<IndexT>(~IndexT(1));
    static constexpr IndexT OVERFLOW_INDEX = static_cast<IndexT>(~IndexT(0));

    TDBCVector<IndexT> m_slots;              // 0x80
    TDBCVector<DB2LOOKUPENTRY> m_overflow;  // 0x98

    WowDB2Lookup_IX() = default;

    // ref: FUN_14059d9f0 (IX8)
    WowDB2Lookup_IX(const WowDB2Lookup_IX& other) : WowDB2Lookup(other) {
        DBCVectorCopy(&this->m_slots, other.m_slots);
        DBCVectorCopy(&this->m_overflow, other.m_overflow);
    }

    // ref: FUN_14059f3e0 (IX8)
    ~WowDB2Lookup_IX() override = default;

    // The record a slot names: the file's records, then the patched ones (the reference
    // compares the index to the record count at the slot's width).
    void* Resolve(IndexT slot) {
        if (slot < static_cast<IndexT>(this->m_init.m_recordCount)) {
            return this->m_init.m_records + static_cast<uint64_t>(static_cast<uint32_t>(slot) * this->m_init.m_stride);
        }
        uint32_t index2 = static_cast<uint32_t>(static_cast<IndexT>(slot - static_cast<IndexT>(this->m_init.m_recordCount)));
        if (index2 < this->m_init.m_recordCount2) {
            return this->m_init.m_records2 + static_cast<uint64_t>(index2 * this->m_init.m_stride2);
        }
        return nullptr;
    }

    // ref: FUN_14058a2e0 (IX8)
    void* Find(uint32_t id) override {
        this->CountLookup(id);
        if (this->m_init.m_minId <= id && id <= this->m_init.m_maxId) {
            IndexT slot = this->m_slots[id - this->m_init.m_minId];
            if (slot == EMPTY) {
                return nullptr;
            }
            if (slot != OVERFLOW_INDEX) {
                return this->Resolve(slot);
            }
        }
        return OverflowFind(&this->m_overflow, id);
    }

    // ref: FUN_14058a3b0 (IX8)
    void SetIndex(uint32_t id, uint32_t index) override {
        this->m_slots[id - this->m_init.m_minId] = static_cast<IndexT>(index);
    }

    // ref: FUN_14058a3c0 (IX8)
    void ApplyCopies(const uint32_t* pairs) override {
        if (pairs == nullptr) {
            return;
        }
        for (uint32_t i = 0; i < this->m_init.m_copyCount; i++) {
            this->m_slots[pairs[i * 2] - this->m_init.m_minId] = this->m_slots[pairs[i * 2 + 1] - this->m_init.m_minId];
        }
    }

    // ref: FUN_14058a420 (IX8)
    bool SetIndex2(uint32_t id, uint32_t index) override {
        IndexT* slot = &this->m_slots[id - this->m_init.m_minId];
        IndexT previous = *slot;
        *slot = static_cast<IndexT>(index + this->m_init.m_recordCount);
        return previous == EMPTY;
    }

    // ref: FUN_14058a440 (IX8)
    void SetRecord(uint32_t id, void* record) override {
        if (this->m_init.m_minId <= id && id <= this->m_init.m_maxId) {
            this->m_slots[id - this->m_init.m_minId] = OVERFLOW_INDEX;
        }
        OverflowSet(&this->m_overflow, id, record);
        this->Changed();
    }

    // ref: FUN_14058a4a0 (IX8)
    bool ClearRecord(uint32_t id, void* record) override {
        bool cleared = false;
        if (this->m_init.m_minId <= id && id <= this->m_init.m_maxId) {
            IndexT* slot = &this->m_slots[id - this->m_init.m_minId];
            if (*slot == EMPTY) {
                return false;
            }
            if (*slot != OVERFLOW_INDEX) {
                if (this->Resolve(*slot) != record) {
                    return false;
                }
                *slot = EMPTY;
                cleared = true;
            }
        }
        if (!cleared && !OverflowClear(&this->m_overflow, id, record)) {
            return false;
        }
        this->Changed();
        return true;
    }

    // ref: FUN_140588f80
    bool IsEmpty() override {
        return this->m_slots.size() == 0 && this->m_overflow.size() == 0;
    }

    // ref: FUN_14058a110 (IX8)
    WowDB2Lookup* Clone() override {
        auto copy = new (LookupAlloc<WowDB2Lookup_IX>(s_lookupHeader, 0x95)) WowDB2Lookup_IX(*this);
        copy->m_dirty = 1;
        if (copy->m_locks == 0) {
            copy->Rebuild();
        }
        return copy;
    }

    // ref: FUN_14058a540 (IX8)
    void Next(DB2LOOKUPITER* it) override {
        int32_t index = it->m_index + 1;
        it->m_index = index;
        int32_t slots = static_cast<int32_t>(this->m_slots.size());
        for (;;) {
            if (slots <= index) {
                for (; index < static_cast<int32_t>(this->m_overflow.size()) + slots; index++) {
                    void* record = this->m_overflow[index - slots].m_record;
                    if (IsLive(this, record)) {
                        it->m_index = index;
                        it->m_record = record;
                        return;
                    }
                    it->m_index = index + 1;
                }
                it->m_index = -2;
                it->m_pad = 0;
                it->m_record = nullptr;
                return;
            }
            IndexT slot = this->m_slots[index];
            if (slot < EMPTY) {
                void* record = this->Resolve(slot);
                if (IsLive(this, record)) {
                    it->m_record = record;
                    return;
                }
            }
            index++;
            it->m_index = index;
        }
    }

    // ref: FUN_14058a640 (IX8)
    void* Record(const DB2LOOKUPITER* it) override {
        int32_t slots = static_cast<int32_t>(this->m_slots.size());
        if (slots <= it->m_index) {
            return this->m_overflow[it->m_index - slots].m_record;
        }
        IndexT slot = this->m_slots[it->m_index];
        if (slot < EMPTY) {
            return this->Resolve(slot);
        }
        return nullptr;
    }

    // ref: FUN_140588f50
    uint32_t Id(const DB2LOOKUPITER* it) override {
        int32_t slots = static_cast<int32_t>(this->m_slots.size());
        if (it->m_index < slots) {
            return this->m_init.m_minId + it->m_index;
        }
        return this->m_overflow[it->m_index - slots].m_id;
    }

    // ref: FUN_1405891c0
    uint32_t OverflowCount() override {
        return static_cast<uint32_t>(this->m_overflow.size());
    }

    // ref: FUN_14058a6b0 (IX8)
    uint64_t MemoryUsed() override {
        return this->m_slots.size() * sizeof(IndexT) + 200 + this->m_overflow.size() * 0x10;
    }

    // ref: FUN_14058a1d0 (IX8)
    void Init(const DB2LOOKUPINIT* init) override {
        this->m_init = *init;
        if (init->m_recordCount2 + init->m_recordCount != 0) {
            this->m_slots.resize(init->m_maxId - init->m_minId + 1, EMPTY);
        }
    }

    // ref: FUN_140589240
    uint64_t Count() override {
        return this->m_overflow.size() + this->m_slots.size();
    }

    // ref: FUN_14058a1a0 (IX8), FUN_14058ae40 (IX16), FUN_14058b4a0 (IX32)
    static uint32_t Estimate(const DB2LOOKUPINIT* init) {
        if (init->m_sparseCount != 0) {
            return 0xFFFFFFFF;
        }
        uint32_t records = init->m_recordCount2 + init->m_recordCount;
        if (records == 0) {
            return 0;
        }
        if (records > static_cast<uint32_t>(EMPTY) - 1) {
            return 0xFFFFFFFF;
        }
        return (init->m_maxId - init->m_minId) * sizeof(IndexT) + sizeof(IndexT);
    }
};

// ---- PI: packed (id, index) pairs --------------------------------------------------------

template <class KeyT, class IndexT>
struct PIENTRY {
    KeyT m_id;
    IndexT m_index;
};

template <class KeyT, class IndexT>
class WowDB2Lookup_PI : public WowDB2Lookup {
    public:
    using Entry = PIENTRY<KeyT, IndexT>;
    static constexpr IndexT EMPTY = static_cast<IndexT>(~IndexT(1));
    static constexpr IndexT OVERFLOW_INDEX = static_cast<IndexT>(~IndexT(0));

    TDBCVector<Entry> m_entries;            // 0x80 sorted by id once finished
    TDBCVector<DB2LOOKUPENTRY> m_overflow;  // 0x98

    WowDB2Lookup_PI() = default;

    WowDB2Lookup_PI(const WowDB2Lookup_PI& other) : WowDB2Lookup(other) {
        DBCVectorCopy(&this->m_entries, other.m_entries);
        DBCVectorCopy(&this->m_overflow, other.m_overflow);
    }

    ~WowDB2Lookup_PI() override = default;

    // FUN_140599710 (PIbb): sorts the entries by id.
    static void SortEntries(Entry* first, Entry* last) {
        std::sort(first, last, [](const Entry& a, const Entry& b) { return a.m_id < b.m_id; });
    }

    Entry* LowerBound(KeyT key) {
        Entry* first = this->m_entries.begin();
        Entry* last = this->m_entries.end();
        while (first != last) {
            Entry* middle = first + (last - first) / 2;
            if (middle->m_id < key) {
                first = middle + 1;
            } else {
                last = middle;
            }
        }
        return first;
    }

    Entry* FindEntry(uint32_t id) {
        KeyT key = static_cast<KeyT>(id - this->m_init.m_minId);
        Entry* entry = this->LowerBound(key);
        if (entry != this->m_entries.end() && entry->m_id == key) {
            return entry;
        }
        return nullptr;
    }

    void* Resolve(IndexT index) {
        if (index < static_cast<IndexT>(this->m_init.m_recordCount)) {
            return this->m_init.m_records + static_cast<uint64_t>(static_cast<uint32_t>(index) * this->m_init.m_stride);
        }
        uint32_t index2 = static_cast<uint32_t>(static_cast<IndexT>(index - static_cast<IndexT>(this->m_init.m_recordCount)));
        if (index2 < this->m_init.m_recordCount2) {
            return this->m_init.m_records2 + static_cast<uint64_t>(index2 * this->m_init.m_stride2);
        }
        return nullptr;
    }

    // ref: FUN_14058bb60 (PIbb)
    void* Find(uint32_t id) override {
        this->CountLookup(id);
        if (this->m_init.m_minId <= id && id <= this->m_init.m_maxId) {
            Entry* entry = this->FindEntry(id);
            if (entry != nullptr) {
                if (entry->m_index == EMPTY) {
                    return nullptr;
                }
                if (entry->m_index != OVERFLOW_INDEX) {
                    return this->Resolve(entry->m_index);
                }
            }
        }
        return OverflowFind(&this->m_overflow, id);
    }

    // ref: FUN_14058bca0 (PIbb): appended unsorted; Finish sorts.
    void SetIndex(uint32_t id, uint32_t index) override {
        Entry entry;
        entry.m_id = static_cast<KeyT>(id - this->m_init.m_minId);
        entry.m_index = static_cast<IndexT>(index);
        this->m_entries.push_back(entry);
    }

    // ref: FUN_14058bd30 (PIbb): sorts what is there, appends a copy of each source's entry
    // under the copy's id (the copies come sorted by source), sorts those, and merges.
    void ApplyCopies(const uint32_t* pairs) override {
        SortEntries(this->m_entries.begin(), this->m_entries.end());
        if (pairs == nullptr) {
            return;
        }
        uint64_t sorted = this->m_entries.size();
        uint64_t at = 0;
        for (uint32_t i = 0; i < this->m_init.m_copyCount; i++) {
            KeyT source = static_cast<KeyT>(pairs[i * 2 + 1] - this->m_init.m_minId);
            while (this->m_entries[at].m_id < source) {
                at++;
            }
            Entry entry;
            entry.m_id = static_cast<KeyT>(pairs[i * 2] - this->m_init.m_minId);
            entry.m_index = this->m_entries[at].m_index;
            this->m_entries.push_back(entry);
        }
        Entry* middle = this->m_entries.begin() + sorted;
        SortEntries(middle, this->m_entries.end());
        if (this->m_entries.begin() != middle && middle != this->m_entries.end()) {
            std::inplace_merge(this->m_entries.begin(), middle, this->m_entries.end(),
                [](const Entry& a, const Entry& b) { return a.m_id < b.m_id; });
        }
    }

    // ref: FUN_14058bfb0 (PIbb)
    bool SetIndex2(uint32_t id, uint32_t index) override {
        IndexT value = static_cast<IndexT>(index + this->m_init.m_recordCount);
        if (this->m_entries.size() != 0) {
            Entry* entry = this->FindEntry(id);
            if (entry != nullptr) {
                entry->m_index = value;
                return false;
            }
        }
        Entry entry;
        entry.m_id = static_cast<KeyT>(id - this->m_init.m_minId);
        entry.m_index = value;
        this->m_entries.push_back(entry);
        return true;
    }

    // ref: FUN_14058c0a0 (PIbb)
    void Finish() override {
        SortEntries(this->m_entries.begin(), this->m_entries.end());
        this->Rebuild();
    }

    // ref: FUN_14058c0f0 (PIbb)
    void SetRecord(uint32_t id, void* record) override {
        if (this->m_init.m_minId <= id && id <= this->m_init.m_maxId) {
            Entry* entry = this->FindEntry(id);
            if (entry != nullptr) {
                entry->m_index = OVERFLOW_INDEX;
            }
        }
        OverflowSet(&this->m_overflow, id, record);
        this->Changed();
    }

    // ref: FUN_14058c1b0 (PIbb)
    bool ClearRecord(uint32_t id, void* record) override {
        if (record == nullptr) {
            return false;
        }
        bool cleared = false;
        if (this->m_init.m_minId <= id && id <= this->m_init.m_maxId) {
            Entry* entry = this->FindEntry(id);
            if (entry != nullptr) {
                if (entry->m_index == EMPTY) {
                    return false;
                }
                if (entry->m_index != OVERFLOW_INDEX) {
                    if (this->Resolve(entry->m_index) != record) {
                        return false;
                    }
                    entry->m_index = EMPTY;
                    cleared = true;
                }
            }
        }
        if (!cleared && !OverflowClear(&this->m_overflow, id, record)) {
            return false;
        }
        this->Changed();
        return true;
    }

    // ref: FUN_140588f80
    bool IsEmpty() override {
        return this->m_entries.size() == 0 && this->m_overflow.size() == 0;
    }

    // ref: FUN_14058ba30 (PIbb)
    WowDB2Lookup* Clone() override {
        auto copy = new (LookupAlloc<WowDB2Lookup_PI>(s_lookupHeader, 0x95)) WowDB2Lookup_PI(*this);
        copy->m_dirty = 1;
        if (copy->m_locks == 0) {
            copy->Rebuild();
        }
        return copy;
    }

    // ref: FUN_14058c2c0 (PIbb)
    void Next(DB2LOOKUPITER* it) override {
        int32_t index = it->m_index + 1;
        it->m_index = index;
        int32_t total = static_cast<int32_t>(this->m_overflow.size() + this->m_entries.size());
        while (index < total) {
            void* record = this->Record(it);
            if (IsLive(this, record)) {
                it->m_record = record;
                return;
            }
            index++;
            it->m_index = index;
        }
        it->m_index = -2;
        it->m_pad = 0;
        it->m_record = nullptr;
    }

    // ref: FUN_14058c350 (PIbb)
    void* Record(const DB2LOOKUPITER* it) override {
        int32_t entries = static_cast<int32_t>(this->m_entries.size());
        if (entries <= it->m_index) {
            return this->m_overflow[it->m_index - entries].m_record;
        }
        IndexT index = this->m_entries[it->m_index].m_index;
        if (index < EMPTY) {
            return this->Resolve(index);
        }
        return nullptr;
    }

    // ref: FUN_14058c3c0 (PIbb)
    uint32_t Id(const DB2LOOKUPITER* it) override {
        int32_t entries = static_cast<int32_t>(this->m_entries.size());
        if (it->m_index < entries) {
            return static_cast<uint32_t>(this->m_entries[it->m_index].m_id) + this->m_init.m_minId;
        }
        return this->m_overflow[it->m_index - entries].m_id;
    }

    // ref: FUN_1405891c0
    uint32_t OverflowCount() override {
        return static_cast<uint32_t>(this->m_overflow.size());
    }

    // ref: FUN_14058b3b0 (PIbb)
    uint64_t MemoryUsed() override {
        return this->m_entries.size() * sizeof(Entry) + this->m_overflow.size() * 0x10 + 200;
    }

    // ref: FUN_14058bb10 (PIbb)
    void Init(const DB2LOOKUPINIT* init) override {
        this->m_init = *init;
        uint32_t count = init->m_recordCount2 + init->m_copyCount + init->m_indexCount;
        if (count != 0) {
            this->m_entries.reserve(count);
        }
    }

    // ref: FUN_140589240
    uint64_t Count() override {
        return this->m_overflow.size() + this->m_entries.size();
    }

    // ref: FUN_14058bac0 (PIbb), FUN_14058cb60 (PIsb), FUN_14058d5b0 (PIss), FUN_14058f470 (PIib),
    // FUN_140591e40 (PIis), FUN_140592e40 (PIii)
    static uint32_t Estimate(const DB2LOOKUPINIT* init) {
        if (init->m_sparseCount != 0) {
            return 0xFFFFFFFF;
        }
        uint32_t extra = init->m_recordCount2 + init->m_copyCount;
        uint32_t count = init->m_indexCount + extra;
        if (count == 0) {
            return 0xFFFFFFFF;
        }
        if (extra + init->m_recordCount > static_cast<uint32_t>(EMPTY) - 1) {
            return 0xFFFFFFFF;
        }
        if constexpr (sizeof(KeyT) < 4) {
            if (init->m_maxId - init->m_minId + 1 > static_cast<uint32_t>(std::numeric_limits<KeyT>::max())) {
                return 0xFFFFFFFF;
            }
        }
        return count * static_cast<uint32_t>(sizeof(Entry));
    }
};

// ---- the sorted wrapper ------------------------------------------------------------------

// Iterates in the meta's record order (meta 0xa0) instead of by id.
template <class Base>
class WowDB2Lookup_Sorted : public Base {
    public:
    TDBCVector<DB2LOOKUPSORTED> m_sorted;

    WowDB2Lookup_Sorted() = default;

    WowDB2Lookup_Sorted(const WowDB2Lookup_Sorted& other) : Base(other) {
        DBCVectorCopy(&this->m_sorted, other.m_sorted);
    }

    // ref: FUN_14058e120 (Null)
    ~WowDB2Lookup_Sorted() override = default;

    // ref: FUN_14058de00 (Null), FUN_14058e570 (IX8)
    WowDB2Lookup* Clone() override {
        auto copy = new (LookupAlloc<WowDB2Lookup_Sorted>(s_lookupHeader, 0x95)) WowDB2Lookup_Sorted(*this);
        copy->m_dirty = 1;
        if (copy->m_locks == 0) {
            copy->Rebuild();
        }
        return copy;
    }

    // ref: FUN_14058dee0 (Null), FUN_14058e260 (IX8)
    void Next(DB2LOOKUPITER* it) override {
        int32_t index = it->m_index + 1;
        it->m_index = index;
        if (static_cast<int32_t>(this->m_sorted.size()) <= index) {
            it->m_index = -2;
            it->m_pad = 0;
            it->m_record = nullptr;
            return;
        }
        it->m_record = this->m_sorted[index].m_record;
    }

    // ref: FUN_14058df30 (Null), FUN_14058e2b0 (IX8)
    void* Record(const DB2LOOKUPITER* it) override {
        return this->m_sorted[it->m_index].m_record;
    }

    // ref: FUN_14058df50 (Null), FUN_14058e2d0 (IX8)
    uint32_t Id(const DB2LOOKUPITER* it) override {
        return this->m_sorted[it->m_index].m_id;
    }

    // ref: FUN_14058df70 (Null), FUN_14058e640 (IX8)
    uint64_t MemoryUsed() override {
        return Base::MemoryUsed() + this->m_sorted.size() * 0x10;
    }

    // ref: FUN_14058df90 (Null), FUN_14058e670 (IX8): every live record through the base's
    // own iteration, then sorted by the meta's order (FUN_1405973b0).
    void Rebuild() override {
        this->m_sorted.clear();
        this->m_sorted.reserve(Base::Count());
        DB2LOOKUPITER it = { -1, 0, nullptr };
        Base::Next(&it);
        while (it.m_index != -2) {
            DB2LOOKUPSORTED entry;
            entry.m_record = Base::Record(&it);
            entry.m_id = Base::Id(&it);
            entry.m_pad = 0;
            this->m_sorted.push_back(entry);
            Base::Next(&it);
        }
        auto order = this->m_init.m_order;
        std::sort(this->m_sorted.begin(), this->m_sorted.end(), [order](const DB2LOOKUPSORTED& a, const DB2LOOKUPSORTED& b) {
            const void* left = &a;
            const void* right = &b;
            return order(reinterpret_cast<const void* const*>(left), reinterpret_cast<const void* const*>(right));
        });
        this->m_dirty = 0;
    }
};

// ---- the strategy table ------------------------------------------------------------------

template <class T>
WowDB2Lookup* LookupNew() {
    return new (LookupAlloc<T>(s_lookupFile, 0x3c0)) T();
}

uint32_t SparseEstimate(const DB2LOOKUPINIT* init) {
    // ref: FUN_1405894f0
    if (init->m_sparseCount == 0) {
        return 0xFFFFFFFF;
    }
    return (init->m_recordCount2 + init->m_sparseCount) * 0x10;
}

WowDB2Lookup* NotPorted() {
    FROST_UNIMPLEMENTED();  // the sparse and multi-id strategies: the loader's mask never asks for them
    return nullptr;
}

uint32_t MultiEstimate(const DB2LOOKUPINIT*) {
    return 0xFFFFFFFF;
}

struct DB2LOOKUPSTRATEGY {
    uint64_t m_flags;
    const char* m_name;
    WowDB2Lookup* (*m_create)();
    WowDB2Lookup* (*m_createSorted)();
    uint32_t (*m_estimate)(const DB2LOOKUPINIT* init);
};

using IX8 = WowDB2Lookup_IX<uint8_t>;
using IX16 = WowDB2Lookup_IX<uint16_t>;
using IX32 = WowDB2Lookup_IX<uint32_t>;
using PIbb = WowDB2Lookup_PI<uint8_t, uint8_t>;
using PIsb = WowDB2Lookup_PI<uint16_t, uint8_t>;
using PIss = WowDB2Lookup_PI<uint16_t, uint16_t>;
using PIib = WowDB2Lookup_PI<uint32_t, uint8_t>;
using PIis = WowDB2Lookup_PI<uint32_t, uint16_t>;
using PIii = WowDB2Lookup_PI<uint32_t, uint32_t>;

// DAT_1426b6690
const DB2LOOKUPSTRATEGY s_strategies[] = {
    { 0x1, "Null", LookupNew<WowDB2Lookup_Null>, LookupNew<WowDB2Lookup_Sorted<WowDB2Lookup_Null>>, WowDB2Lookup_Null::Estimate },
    { 0x2, "Vec ", LookupNew<WowDB2Lookup_Vec>, LookupNew<WowDB2Lookup_Sorted<WowDB2Lookup_Vec>>, WowDB2Lookup_Vec::Estimate },
    { 0x4, "IX8 ", LookupNew<IX8>, LookupNew<WowDB2Lookup_Sorted<IX8>>, IX8::Estimate },
    { 0x4, "IX16", LookupNew<IX16>, LookupNew<WowDB2Lookup_Sorted<IX16>>, IX16::Estimate },
    { 0x4, "IX32", LookupNew<IX32>, LookupNew<WowDB2Lookup_Sorted<IX32>>, IX32::Estimate },
    { 0x8, "PIbb", LookupNew<PIbb>, LookupNew<WowDB2Lookup_Sorted<PIbb>>, PIbb::Estimate },
    { 0x8, "PIsb", LookupNew<PIsb>, LookupNew<WowDB2Lookup_Sorted<PIsb>>, PIsb::Estimate },
    { 0x8, "PIss", LookupNew<PIss>, LookupNew<WowDB2Lookup_Sorted<PIss>>, PIss::Estimate },
    { 0x8, "PIib", LookupNew<PIib>, LookupNew<WowDB2Lookup_Sorted<PIib>>, PIib::Estimate },
    { 0x8, "PIis", LookupNew<PIis>, LookupNew<WowDB2Lookup_Sorted<PIis>>, PIis::Estimate },
    { 0x8, "PIii", LookupNew<PIii>, LookupNew<WowDB2Lookup_Sorted<PIii>>, PIii::Estimate },
    // ref: FUN_140596880, FUN_140596930
    { 0x10, "Spar", NotPorted, NotPorted, SparseEstimate },
    // ref: FUN_140596a00, FUN_140596b00, FUN_140596c00
    { 0x20, "Mbb", NotPorted, NotPorted, MultiEstimate },
    { 0x20, "Mss", NotPorted, NotPorted, MultiEstimate },
    { 0x20, "Mii", NotPorted, NotPorted, MultiEstimate },
};

}

// ref: FUN_1405a0640
DB2LOOKUPITER* WowDB2LookupBegin(WowDB2Lookup* lookup, DB2LOOKUPITER* it) {
    it->m_record = nullptr;
    it->m_index = -1;
    lookup->Next(it);
    return it;
}

// ref: FUN_1405a0670
DB2LOOKUPITER* WowDB2LookupEnd(WowDB2Lookup* lookup, DB2LOOKUPITER* it) {
    (void)lookup;
    it->m_pad = 0;
    it->m_record = nullptr;
    it->m_index = -2;
    return it;
}

// ref: FUN_1405a0690
void WowDB2LookupLockFinish(WowDB2Lookup* lookup) {
    lookup->m_init.m_finishLocks++;
}

// ref: FUN_1405a06b0
void WowDB2LookupUnlockFinish(WowDB2Lookup* lookup) {
    lookup->m_init.m_finishLocks--;
    if (lookup->m_init.m_finishLocks == 0 && (lookup->m_init.m_finishFlags & 2) != 0) {
        lookup->Finish();
    }
}

// ref: FUN_1405a06a0
void WowDB2LookupLock(WowDB2Lookup* lookup) {
    lookup->m_locks++;
}

// ref: FUN_1405a06d0
void WowDB2LookupUnlock(WowDB2Lookup* lookup) {
    lookup->m_locks--;
    if (lookup->m_locks == 0 && lookup->m_dirty != 0) {
        lookup->Rebuild();
    }
}

// ref: FUN_1405a07a0
WowDB2Lookup* WowDB2LookupCreate(uint32_t mask, const DB2LOOKUPINIT* init) {
    const DB2LOOKUPSTRATEGY* best = nullptr;
    uint32_t bestCost = 0xFFFFFFFF;
    for (const DB2LOOKUPSTRATEGY& strategy : s_strategies) {
        if ((strategy.m_flags & mask) == 0) {
            continue;
        }
        uint32_t cost = strategy.m_estimate(init);
        if (cost != 0xFFFFFFFF) {
            cost = (cost + 7) & 0xFFFFFFF8;
        }
        if (best == nullptr || cost < bestCost) {
            best = &strategy;
            bestCost = cost;
        }
    }
    int32_t vectorCost = -1;
    if (init->m_sparseCount == 0) {
        if (init->m_recordCount2 + init->m_copyCount + init->m_recordCount == 0) {
            vectorCost = 0;
        } else {
            vectorCost = static_cast<int32_t>((init->m_maxId - init->m_minId) * 8 + 8);
        }
    }
    s_lookupBytes += bestCost;
    s_vectorBytes += vectorCost;
    WowDB2Lookup* lookup = init->m_order == nullptr ? best->m_create() : best->m_createSorted();
    lookup->Init(init);
    lookup->m_78 = 0;
    return lookup;
}

// ---- the parent lookups ------------------------------------------------------------------

// ref: FUN_140588430
WowDB2ParentLookup::~WowDB2ParentLookup() = default;

// ref: FUN_1405883d0
DB2PARENTITER* WowDB2ParentLookup::End(DB2PARENTITER* it) {
    it->m_entry = 0xFFFFFFFF;
    it->m_overflow = 0xFFFFFFFF;
    it->m_parent = 0xFFFFFFFF;
    it->m_source = 0xFFFFFFFF;
    return it;
}

namespace {

// The parent -> first pair map (empty 0x80000001, removed 0x80000000): FUN_1405a1db0 rehashes.
struct DB2PARENTMAP {
    struct Entry {
        int32_t m_key;
        uint32_t m_first;
    };

    uint64_t m_capacity = 8;        // 0x00
    Entry* m_entries = nullptr;     // 0x08
    uint64_t m_used = 0;            // 0x10
    uint64_t m_deleted = 0;         // 0x18
    float m_maxLoad = 0.75f;        // 0x20

    static constexpr int32_t EMPTY = static_cast<int32_t>(0x80000001);
    static constexpr int32_t DELETED = static_cast<int32_t>(0x80000000);

    static uint64_t Hash(int32_t key) {
        uint64_t k = static_cast<uint64_t>(static_cast<int64_t>(key));
        k = (k >> 33 ^ k) * 0xFF51AFD7ED558CCDull;
        k = (k >> 33 ^ k) * 0xC4CEB9FE1A85EC53ull;
        return k >> 33 ^ k;
    }

    Entry* AllocateEntries(uint64_t count) {
        auto entries = static_cast<Entry*>(SMemAlloc(count * sizeof(Entry), 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
        for (uint64_t i = 0; i < count; i++) {
            entries[i].m_key = EMPTY;
        }
        return entries;
    }

    void Init() {
        this->m_entries = this->AllocateEntries(8);
    }

    void Free() {
        SMemFree(this->m_entries, s_dbcTypesFile, 0x103, 0);
        this->m_entries = nullptr;
    }

    // ref: FUN_1405a1db0
    void Rehash(uint64_t count) {
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
        if (capacity == this->m_capacity && this->m_deleted == 0) {
            return;
        }
        Entry* entries = this->AllocateEntries(capacity);
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            Entry* old = &this->m_entries[i];
            if (old->m_key == EMPTY || (this->m_deleted != 0 && old->m_key == DELETED)) {
                continue;
            }
            uint64_t index = Hash(old->m_key) & (capacity - 1);
            for (uint64_t step = 1; entries[index].m_key != EMPTY; step++) {
                index = (index + step) & (capacity - 1);
            }
            entries[index] = *old;
        }
        SMemFree(this->m_entries, s_dbcTypesFile, 0x103, 0);
        this->m_used -= this->m_deleted;
        this->m_entries = entries;
        this->m_capacity = capacity;
        this->m_deleted = 0;
    }

    uint32_t* Insert(int32_t key) {
        float load = static_cast<float>(this->m_used + 1) / static_cast<float>(this->m_capacity);
        if (this->m_maxLoad < load) {
            this->Rehash(0);
        }
        uint64_t mask = this->m_capacity - 1;
        uint64_t index = Hash(key) & mask;
        Entry* removed = nullptr;
        for (uint64_t step = 1;; step++) {
            Entry* slot = &this->m_entries[index];
            if (slot->m_key == EMPTY) {
                break;
            }
            if (slot->m_key == DELETED) {
                if (removed == nullptr) {
                    removed = slot;
                }
            } else if (slot->m_key == key) {
                return &slot->m_first;
            }
            index = (index + step) & mask;
        }
        Entry* slot = removed != nullptr ? removed : &this->m_entries[index];
        slot->m_key = key;
        slot->m_first = 0;
        this->m_used++;
        return &slot->m_first;
    }

    Entry* Find(int32_t key) {
        uint64_t mask = this->m_capacity - 1;
        uint64_t index = Hash(key) & mask;
        for (uint64_t step = 1; this->m_entries[index].m_key != EMPTY; step++) {
            Entry* slot = &this->m_entries[index];
            if ((this->m_deleted == 0 || slot->m_key != DELETED) && slot->m_key == key) {
                return slot;
            }
            index = (index + step) & mask;
        }
        return nullptr;
    }
};

template <class Key, class IndexT>
class WowDB2ParentLookup_PI : public WowDB2ParentLookup {
    public:
    using Entry = PIENTRY<Key, IndexT>;
    static constexpr IndexT DELETED_INDEX = static_cast<IndexT>(~IndexT(1));

    TDBCVector<Entry> m_entries;                // 0x38
    TDBCVector<DB2PARENTOVERFLOW> m_overflow;   // 0x50
    DB2PARENTMAP m_map;                         // 0x68

    // ref: FUN_1405904e0 (PIbb)
    WowDB2ParentLookup_PI() {
        std::memset(&this->m_init, 0, sizeof(this->m_init));
        this->m_map.Init();
    }

    // ref: FUN_140587c60 (PIbb)
    ~WowDB2ParentLookup_PI() override {
        this->m_map.Free();
    }

    Key Encode(int32_t parent) const {
        int32_t base = 0 < this->m_init.m_minParent ? this->m_init.m_minParent : 0;
        return static_cast<Key>(parent - base);
    }

    int32_t Decode(Key key) const {
        int32_t base = 0 < this->m_init.m_minParent ? this->m_init.m_minParent : 0;
        return static_cast<int32_t>(key) + base;
    }

    void* IndexRecord(IndexT index) const {
        if (this->m_init.m_sparse != 0) {
            return reinterpret_cast<void*>(static_cast<uintptr_t>(index));
        }
        return this->m_init.m_records + static_cast<uint64_t>(index) * this->m_init.m_stride;
    }

    // ref: FUN_140596d00 (PIbb)
    void Init(const DB2PARENTINIT* init) override {
        this->m_entries.reserve(init->m_count);
    }

    // ref: FUN_140596d10 (PIbb)
    void Add(int32_t parent, uint32_t index) override {
        Entry entry;
        entry.m_id = this->Encode(parent);
        entry.m_index = static_cast<IndexT>(index);
        this->m_entries.push_back(entry);
    }

    // ref: FUN_1405976f0 (PIbb): by parent, then by the meta's order (or the index).
    bool Less(const Entry& a, const Entry& b) const {
        if (a.m_id != b.m_id) {
            return a.m_id < b.m_id;
        }
        auto order = this->m_init.m_order;
        if (order == nullptr) {
            return a.m_index < b.m_index;
        }
        return order(this->IndexRecord(a.m_index), this->IndexRecord(b.m_index));
    }

    // ref: FUN_140596db0 (PIbb)
    void Build() override {
        this->m_map.m_maxLoad = 0.6f;
        std::sort(this->m_entries.begin(), this->m_entries.end(), [this](const Entry& a, const Entry& b) { return this->Less(a, b); });
        int32_t previous = -1;
        for (uint32_t i = 0; static_cast<uint64_t>(static_cast<int32_t>(i)) < this->m_entries.size(); i++) {
            int32_t parent = this->Decode(this->m_entries[i].m_id);
            if (parent != previous) {
                *this->m_map.Insert(parent) = i;
                previous = parent;
            }
        }
        this->m_flags |= 1;
    }

    // ref: FUN_140596f80 (PIbb)
    bool Update(void* previous, void* record, int32_t parent, uint32_t index, bool clear) override {
        if (previous != nullptr) {
            if (clear) {
                for (Entry& entry : this->m_entries) {
                    if (entry.m_index == static_cast<IndexT>(index)) {
                        entry.m_index = DELETED_INDEX;
                        break;
                    }
                }
            }
            for (uint64_t i = 0; i < this->m_overflow.size(); i++) {
                if (this->m_overflow[i].m_record == previous) {
                    this->m_overflow[i] = this->m_overflow[this->m_overflow.size() - 1];
                    this->m_overflow.m_size--;
                    break;
                }
            }
        }
        if (record != nullptr) {
            DB2PARENTOVERFLOW entry;
            entry.m_parent = static_cast<uint32_t>(parent);
            entry.m_pad = 0;
            entry.m_record = record;
            this->m_overflow.push_back(entry);
        }
        if (this->m_locks == 0) {
            this->SortOverflow();
        } else {
            this->m_flags |= 2;
        }
        return true;
    }

    // ref: FUN_1405881b0 (FUN_140599ff0): by parent, then by the meta's order.
    void SortOverflow() override {
        auto order = this->m_init.m_order;
        std::sort(this->m_overflow.begin(), this->m_overflow.end(), [order](const DB2PARENTOVERFLOW& a, const DB2PARENTOVERFLOW& b) {
            if (a.m_parent != b.m_parent) {
                return a.m_parent < b.m_parent;
            }
            return order != nullptr && order(a.m_record, b.m_record);
        });
        this->m_flags &= ~2;
    }

    // ref: FUN_140597190 (PIbb)
    void* Record(const DB2PARENTITER* it) override {
        if (it->m_source == 0) {
            return this->IndexRecord(this->m_entries[it->m_entry].m_index);
        }
        if (it->m_source != 1) {
            return nullptr;
        }
        return this->m_overflow[it->m_overflow].m_record;
    }

    // ref: FUN_14059feb0
    void Settle(DB2PARENTITER* it) {
        uint64_t entry = it->m_entry;
        bool found = false;
        while (entry < this->m_entries.size()) {
            const Entry& e = this->m_entries[entry];
            if (e.m_id != this->Encode(static_cast<int32_t>(it->m_parent))) {
                break;
            }
            if (e.m_index != DELETED_INDEX) {
                found = true;
                break;
            }
            entry++;
            it->m_entry = static_cast<uint32_t>(entry);
        }
        if (!found) {
            it->m_entry = 0xFFFFFFFF;
        }
        uint64_t overflow = it->m_overflow;
        if (this->m_overflow.size() <= overflow || this->m_overflow[overflow].m_parent != it->m_parent) {
            it->m_overflow = 0xFFFFFFFF;
        }
        if (it->m_entry == 0xFFFFFFFF) {
            if (it->m_overflow == 0xFFFFFFFF) {
                this->End(it);
                return;
            }
            it->m_source = 1;
            return;
        }
        uint32_t source = 0;
        if (this->m_init.m_order != nullptr && it->m_overflow != 0xFFFFFFFF) {
            void* record = this->IndexRecord(this->m_entries[it->m_entry].m_index);
            source = this->m_init.m_order(record, this->m_overflow[it->m_overflow].m_record) ? 0 : 1;
        }
        it->m_source = source;
    }

    // ref: FUN_1405971e0 (PIbb)
    DB2PARENTITER* Begin(DB2PARENTITER* it, int32_t parent) override {
        DB2PARENTMAP::Entry* first = this->m_map.Find(parent);
        it->m_entry = first != nullptr ? first->m_first : 0xFFFFFFFF;
        DB2PARENTOVERFLOW* lo = this->m_overflow.begin();
        DB2PARENTOVERFLOW* hi = this->m_overflow.end();
        while (lo != hi) {
            DB2PARENTOVERFLOW* middle = lo + (hi - lo) / 2;
            if (middle->m_parent < static_cast<uint32_t>(parent)) {
                lo = middle + 1;
            } else {
                hi = middle;
            }
        }
        it->m_overflow = lo == this->m_overflow.end() ? 0xFFFFFFFF : static_cast<uint32_t>(lo - this->m_overflow.begin());
        it->m_parent = static_cast<uint32_t>(parent);
        it->m_source = 0;
        this->Settle(it);
        return it;
    }

    // ref: FUN_140597350 (PIbb)
    void Next(DB2PARENTITER* it) override {
        if (it->m_source == 0) {
            it->m_entry++;
        } else if (it->m_source == 1) {
            it->m_overflow++;
        }
        this->Settle(it);
    }

    // ref: FUN_140597370 (PIbb), FUN_1405883f0 (PIsb), FUN_1405893e0 (PIss), FUN_14058a0c0 (PIib),
    // FUN_14058ad50 (PIis), FUN_14058ca80 (PIii)
    static uint32_t Estimate(const DB2PARENTINIT* init) {
        if constexpr (sizeof(IndexT) < 4) {
            if (init->m_count + 2 > static_cast<uint32_t>(std::numeric_limits<IndexT>::max())) {
                return 0xFFFFFFFF;
            }
        }
        if constexpr (sizeof(Key) < 4) {
            if (static_cast<uint32_t>(init->m_maxParent - init->m_minParent) + 1 > static_cast<uint32_t>(std::numeric_limits<Key>::max())) {
                return 0xFFFFFFFF;
            }
        }
        return init->m_count * static_cast<uint32_t>(sizeof(Entry));
    }

    static WowDB2ParentLookup* Create() {
        void* memory = SMemAlloc(sizeof(WowDB2ParentLookup_PI), 0x24, s_lookupFile, SStrHashFnv1a(s_lookupFile), 0x59d, 0);
        return new (memory) WowDB2ParentLookup_PI();
    }
};

struct DB2PARENTSTRATEGY {
    const char* m_name;
    WowDB2ParentLookup* (*m_create)();
    uint32_t (*m_estimate)(const DB2PARENTINIT* init);
};

// DAT_142ccdfa0
const DB2PARENTSTRATEGY s_parentStrategies[] = {
    { "PIbb", WowDB2ParentLookup_PI<uint8_t, uint8_t>::Create, WowDB2ParentLookup_PI<uint8_t, uint8_t>::Estimate },
    { "PIsb", WowDB2ParentLookup_PI<uint16_t, uint8_t>::Create, WowDB2ParentLookup_PI<uint16_t, uint8_t>::Estimate },
    { "PIss", WowDB2ParentLookup_PI<uint16_t, uint16_t>::Create, WowDB2ParentLookup_PI<uint16_t, uint16_t>::Estimate },
    { "PIib", WowDB2ParentLookup_PI<uint32_t, uint8_t>::Create, WowDB2ParentLookup_PI<uint32_t, uint8_t>::Estimate },
    { "PIis", WowDB2ParentLookup_PI<uint32_t, uint16_t>::Create, WowDB2ParentLookup_PI<uint32_t, uint16_t>::Estimate },
    { "PIii", WowDB2ParentLookup_PI<uint32_t, uint32_t>::Create, WowDB2ParentLookup_PI<uint32_t, uint32_t>::Estimate },
};

}

// ref: FUN_1405a06f0
WowDB2ParentLookup* WowDB2ParentLookupCreate(const DB2PARENTINIT* init) {
    const DB2PARENTSTRATEGY* best = nullptr;
    uint32_t bestCost = 0xFFFFFFFF;
    for (const DB2PARENTSTRATEGY& strategy : s_parentStrategies) {
        uint32_t cost = strategy.m_estimate(init);
        if (cost != 0xFFFFFFFF && (best == nullptr || cost < bestCost)) {
            best = &strategy;
            bestCost = cost;
        }
    }
    WowDB2ParentLookup* lookup = best->m_create();
    lookup->Init(init);
    lookup->m_init = *init;
    return lookup;
}
