#include "db/WowClientSparseDB2.hpp"

#include <cstring>
#include <new>

#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

#include <windows.h>

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_1405a4550(unsigned long long, unsigned long long);   // WowClientSparseDB2::Load
unsigned long long FUN_1405a48c0(...);
unsigned long long FUN_1405a4980(...);
unsigned long long FUN_1405a3470(...);
unsigned long long FUN_1405a4e30(...);
unsigned long long FUN_1405a3550(...);
unsigned long long FUN_1405a46d0(...);
unsigned long long FUN_1405a48b0(...);
unsigned long long FUN_1405a37f0(...);
unsigned long long FUN_1405a4040(...);
unsigned long long FUN_1405a3cd0(...);
unsigned long long FUN_1405a4f50(...);
unsigned long long FUN_1405a49c0(...);
unsigned long long FUN_1405a4a40(...);
unsigned long long FUN_1405a3c80(...);
unsigned long long FUN_1405a3c60(...);
unsigned long long FUN_140447c30(...);  // the record id (0x78): Spell, ItemSparse, ConversationLine, SceneScriptText
unsigned long long FUN_140385ab0(...);
unsigned long long FUN_14038a6a0(...);
unsigned long long FUN_14042b9c0(...);
unsigned long long FUN_140447cc0(...);  // the record freed (0x80), the same order
unsigned long long FUN_140385b50(...);
unsigned long long FUN_14038a720(...);
unsigned long long FUN_14042ba90(...);
unsigned long long FUN_140447c50();     // SpellRec_C's record (slot 0x140)
unsigned long long FUN_140385ad0();     // ItemSparseRec_C's
unsigned long long FUN_14038a6c0();     // ConversationLineRec_C's
unsigned long long FUN_14042ba30();     // SceneScriptTextRec_C's
}

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

// FUN_14057e200 over the sparse slots (0x58 FUN_1405a46d0, 0xb8 FUN_1405a4040 the catalog,
// 0x120/0x128 the section bounds, 0xf0/0xf8 records), then the field offsets, the ring
// (FUN_1405a58a0) and the cache file (FUN_1402cff40): the reference's own, translated, over this
// object (its layout is the reference's, asserted in the header). Until 2026-10-21 a stub that
// loaded nothing, so a sparse table's catalog lookup (0x240) stayed null and the first Spell
// lookup on entering the world called through it.
// translated: FUN_1405a4550
bool WowClientSparseDB2::Load(bool errors) {
    __try {
        return (FUN_1405a4550(reinterpret_cast<unsigned long long>(this), errors ? 1ull : 0ull) & 0xff) != 0;
    } __except (GenFault(0x1405a4550ull, GetExceptionInformation())) {
        return false;
    }
}

// The sparse class's own slots, the reference's code translated over this object (see the
// header). Each a guarded call: a fault in one is logged, and the slot returns zero.
#define SPARSE_SLOT(addr, call)                                                                   \
    __try {                                                                                       \
        return call;                                                                              \
    } __except (GenFault(addr, GetExceptionInformation())) {                                      \
    }

namespace {
unsigned long long Self(WowClientSparseDB2* self) {
    return reinterpret_cast<unsigned long long>(self);
}
unsigned long long Ptr(const void* p) {
    return reinterpret_cast<unsigned long long>(p);
}
}

// translated: FUN_1405a48c0
void WowClientSparseDB2::Slot08() {
    SPARSE_SLOT(0x1405a48c0ull, (void)FUN_1405a48c0(Self(this)))
}

// translated: FUN_1405a4980
void WowClientSparseDB2::Slot10() {
    SPARSE_SLOT(0x1405a4980ull, (void)FUN_1405a4980(Self(this)))
}

// translated: FUN_1405a3470
bool WowClientSparseDB2::Slot18() {
    SPARSE_SLOT(0x1405a3470ull, (FUN_1405a3470(Self(this)) & 0xff) != 0)
    return false;
}

// translated: FUN_1405a4e30
void WowClientSparseDB2::Unload(bool failed) {
    SPARSE_SLOT(0x1405a4e30ull, (void)FUN_1405a4e30(Self(this), failed ? 1ull : 0ull))
}

// translated: FUN_1405a3550
void WowClientSparseDB2::Slot40() {
    SPARSE_SLOT(0x1405a3550ull, (void)FUN_1405a3550(Self(this)))
}

// translated: FUN_1405a46d0
bool WowClientSparseDB2::LoadSection(void* file, int32_t section, bool errors) {
    SPARSE_SLOT(0x1405a46d0ull, (FUN_1405a46d0(Self(this), Ptr(file), static_cast<unsigned long long>(
        static_cast<uint32_t>(section)), errors ? 1ull : 0ull) & 0xff) != 0)
    return false;
}

// translated: FUN_1405a48b0
bool WowClientSparseDB2::ValidateMeta() {
    SPARSE_SLOT(0x1405a48b0ull, (FUN_1405a48b0(Self(this)) & 0xff) != 0)
    return false;
}

// translated: FUN_1405a37f0
CASFILE** WowClientSparseDB2::FileSlot() {
    SPARSE_SLOT(0x1405a37f0ull, reinterpret_cast<CASFILE**>(FUN_1405a37f0(Self(this))))
    return nullptr;
}

// translated: FUN_1405a4040
bool WowClientSparseDB2::ReadSectionExtra(CDataStore* store, bool errors, uint32_t section) {
    SPARSE_SLOT(0x1405a4040ull, (FUN_1405a4040(Self(this), Ptr(store), errors ? 1ull : 0ull,
        static_cast<unsigned long long>(section)) & 0xff) != 0)
    return false;
}

// translated: FUN_1405a3cd0
uint32_t WowClientSparseDB2::SparseCount() {
    SPARSE_SLOT(0x1405a3cd0ull, static_cast<uint32_t>(FUN_1405a3cd0(Self(this))))
    return 0;
}

// translated: FUN_1405a4f50
void* WowClientSparseDB2::DeserializeRecord(CDataStore* store) {
    SPARSE_SLOT(0x1405a4f50ull, reinterpret_cast<void*>(FUN_1405a4f50(Self(this), Ptr(store))))
    return nullptr;
}

// translated: FUN_1405a49c0
void WowClientSparseDB2::SetRecord(uint32_t id, void* record, void* previous, int32_t parent) {
    SPARSE_SLOT(0x1405a49c0ull, (void)FUN_1405a49c0(Self(this), static_cast<unsigned long long>(id), Ptr(record),
        Ptr(previous), static_cast<unsigned long long>(static_cast<uint32_t>(parent))))
}

// translated: FUN_1405a4a40
void WowClientSparseDB2::UpdateParent(void* record, int32_t parent, uint32_t id, void* previous) {
    SPARSE_SLOT(0x1405a4a40ull, (void)FUN_1405a4a40(Self(this), Ptr(record),
        static_cast<unsigned long long>(static_cast<uint32_t>(parent)), static_cast<unsigned long long>(id),
        Ptr(previous)))
}

// translated: FUN_1405a3c80
uint32_t WowClientSparseDB2::SectionSize(uint32_t section) {
    SPARSE_SLOT(0x1405a3c80ull, static_cast<uint32_t>(FUN_1405a3c80(Self(this), static_cast<unsigned long long>(section))))
    return 0;
}

// translated: FUN_1405a3c60
uint32_t WowClientSparseDB2::SectionOffset(uint32_t section) {
    SPARSE_SLOT(0x1405a3c60ull, static_cast<uint32_t>(FUN_1405a3c60(Self(this), static_cast<unsigned long long>(section))))
    return 0;
}

#undef SPARSE_SLOT

// (FUN_1402112e0: the file stays open for the records read on demand)
void WowClientSparseDB2::CloseFile(CASFILE* file) {
    (void)file;
}

// (FUN_14026bea0: no record block to copy)
bool WowClientSparseDB2::CopySection(CDataStore* store, bool errors, uint32_t section) {
    (void)store;
    (void)errors;
    (void)section;
    return true;
}

// (FUN_14026bea0: no copy table)
bool WowClientSparseDB2::ReadCopyTable(CDataStore* store, bool errors, uint32_t section,
    TDBCVector<DB2DUPLICATE>* duplicates) {
    (void)store;
    (void)section;
    (void)errors;
    (void)duplicates;
    return true;
}

// The file already open (0x248).
// ref: FUN_14036b130
CASFILE* WowClientSparseDB2::OpenFile(bool errors) {
    (void)errors;
    return this->m_file;
}

// (FUN_1402112e0)
void WowClientSparseDB2::CloseFile2(CASFILE* file) {
    (void)file;
}

// The AutoCode initializer's tail: a pool of 0x200 records a heap, Storm category 0x24.
// ref: FUN_1405a5650
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

extern WowClientSparseDB2Table g_conversationLineDB;    // 0x142e463a0
extern WowClientSparseDB2Table g_itemSparseDB;          // 0x142e5ed20
extern WowClientSparseDB2Table g_sceneScriptTextDB;     // 0x142e736b0
extern WowClientSparseDB2Table g_spellDB;               // 0x142e7c6a0

// A record from the table's pool, zeroed as its class's slot 0x140 does it: the reference's
// own, translated (each knows its record's fields).
void* WowClientSparseDB2Table::NewRecord() {
    unsigned long long (*make)() = nullptr;
    if (this == &g_spellDB) {
        make = FUN_140447c50;
    } else if (this == &g_itemSparseDB) {
        make = FUN_140385ad0;
    } else if (this == &g_conversationLineDB) {
        make = FUN_14038a6c0;
    } else if (this == &g_sceneScriptTextDB) {
        make = FUN_14042ba30;
    }
    if (make == nullptr) {
        return nullptr;
    }
    __try {
        return reinterpret_cast<void*>(make());
    } __except (GenFault(0x140447c50ull, GetExceptionInformation())) {
        return nullptr;
    }
}

// A record's id, and a record given back to the pool: each table's own (slots 0x78 / 0x80),
// translated.
uint64_t WowClientSparseDB2Table::RecordId(void* record) {
    unsigned long long (*id)(...) = this == &g_spellDB ? FUN_140447c30
        : this == &g_itemSparseDB ? FUN_140385ab0
        : this == &g_conversationLineDB ? FUN_14038a6a0
        : this == &g_sceneScriptTextDB ? FUN_14042b9c0 : nullptr;
    if (id == nullptr) {
        return 0;
    }
    __try {
        return id(reinterpret_cast<unsigned long long>(this), reinterpret_cast<unsigned long long>(record));
    } __except (GenFault(0x140447c30ull, GetExceptionInformation())) {
        return 0;
    }
}

void WowClientSparseDB2Table::FreeRecord(void* record) {
    unsigned long long (*free)(...) = this == &g_spellDB ? FUN_140447cc0
        : this == &g_itemSparseDB ? FUN_140385b50
        : this == &g_conversationLineDB ? FUN_14038a720
        : this == &g_sceneScriptTextDB ? FUN_14042ba90 : nullptr;
    if (free == nullptr) {
        return;
    }
    __try {
        free(reinterpret_cast<unsigned long long>(this), reinterpret_cast<unsigned long long>(record));
    } __except (GenFault(0x140447cc0ull, GetExceptionInformation())) {
    }
}

// How many records the eviction ring keeps (slot 0x148: FUN_140385e40 Spell and ItemSparse,
// FUN_14038ab70 ConversationLine, FUN_14042bd50 SceneScriptText).
uint32_t WowClientSparseDB2Table::MaxCachedRecords(uint32_t count, uint32_t size) {
    (void)count;
    (void)size;
    if (this == &g_conversationLineDB) {
        return 0x40;
    }
    if (this == &g_sceneScriptTextDB) {
        return 0x400;
    }
    return 2000;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1405a4550=GenForwardMissing")
// ---- end of link fallbacks ----
