#include "cas/CASManifest.hpp"
#include "base/Status.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <windows.h>
#include <cstring>

namespace cas {

MANIFESTCURSOR g_manifestCursor;

namespace {

const char s_entryTag[] = ".?AUBuildManifestEntry@@";
const char s_slotTag[] = ".PEAUBuildManifestEntry@@";
const char s_diffTag[] = ".?AUBuildManifestDiffEntry@@";

// DAT_142e25828
SRWLOCK s_cursorLock = SRWLOCK_INIT;  // DAT_142e25828
// DAT_142e25808: the walk has started
volatile uint8_t s_cursorStarted;

const uint32_t TSFM = 0x4d465354;
const uint32_t CONTENT_NO_NAME_HASH = 0x10000000;
const uint64_t ZERO_NAME_HASH = 0;  // DAT_142631c58

// What the filling pass writes: the manifest, and the named-file counter with its switch.
struct FILLTARGET {
    BUILDMANIFEST* m_manifest;
    uint32_t* m_named;
    uint8_t* m_countNamed;
};

// The parse state both passes share (0x50 bytes, on the parser's stack).
struct PARSECONTEXT {
    const uint8_t* m_blocks;        // 0x00
    uint64_t m_size;                // 0x08
    MANIFESTLOCALE m_locale;        // 0x10
    CStatus* m_status;              // 0x20
    uint8_t m_legacy;               // 0x28
    uint8_t m_namedSeparately;      // 0x29: the TSFM header counts named files apart from all files
    uint8_t m_2a;                   // 0x2a
    union {
        uint32_t* m_total;          // 0x30: the counting pass
        FILLTARGET m_fill;          // 0x30: the filling pass
    };
    int32_t* m_counted;             // 0x48
};
static_assert(sizeof(PARSECONTEXT) == 0x50, "PARSECONTEXT is 0x50 bytes");

uint64_t FnvFileDataId(const BuildManifestEntry* entry) {
    auto bytes = reinterpret_cast<const uint8_t*>(&entry->m_fileDataId);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (int32_t i = 0; i < 4; i++) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    }
    return hash;
}

bool IsLive(const ENTRYTABLE* table, const BuildManifestEntry* entry) {
    return entry->m_fileDataId != 0 && (table->m_removed == 0 || entry->m_fileDataId != 0xFFFFFFFF);
}

// The capacity a table needs for `count` live entries: count / maxLoad, rounded up, at least
// `minimum`, then the next power of two from 8.
uint64_t TableCapacity(uint64_t live, float maxLoad, uint64_t minimum) {
    float count = static_cast<float>(live);
    uint64_t need = static_cast<uint64_t>(count / maxLoad);
    if (count > static_cast<float>(need) * maxLoad) {
        need++;
    }
    if (need < minimum) {
        need = minimum;
    }
    uint64_t capacity = 8;
    if (need > 8) {
        do {
            capacity *= 2;
        } while (capacity < need);
    }
    return capacity;
}

void FreeManifest(BUILDMANIFEST* manifest) {
    SMemFree(manifest->m_byName.m_slots, s_slotTag, -6, 0);
    SMemFree(manifest->m_byId.m_entries, s_entryTag, -6, 0);
    operator delete(manifest, sizeof(BUILDMANIFEST));
}

// The block filter both passes apply. `speech` is the second pass: it takes only the blocks
// with content bit 31, which the first pass excludes.
uint32_t ExcludedContent(uint8_t alternate, uint8_t speech) {
    uint32_t excluded = (alternate ^ 1u) * 0x80 + 0x110;
    if (speech == 0) {
        excluded |= 0x80000000;
    }
    return excluded;
}

// ref: FUN_1402d4fb0
bool FillBlocks(const uint8_t* blocks, uint64_t size, uint32_t localeMask, uint8_t alternate, uint8_t speech,
    uint8_t legacy, uint8_t namedSeparately, uint8_t a8, CStatus* status, FILLTARGET* target, int32_t* counted) {
    (void)a8;
    uint32_t excluded = ExcludedContent(alternate, speech);
    auto end = reinterpret_cast<const uint32_t*>(blocks + size);
    auto block = reinterpret_cast<const uint32_t*>(blocks);
    while (block < end) {
        uint32_t count = block[0];
        const uint32_t* deltas = block + 3;
        const uint32_t* ckeys = deltas + count;
        const uint32_t* names;
        const uint32_t* next;
        uint64_t nameStride;
        if (legacy == 0) {
            if (namedSeparately == 0 || (block[1] & CONTENT_NO_NAME_HASH) == 0) {
                nameStride = 8;
                names = ckeys + uint64_t(count) * 4;
                next = names + uint64_t(count) * 2;
            } else {
                names = reinterpret_cast<const uint32_t*>(&ZERO_NAME_HASH);
                next = ckeys + uint64_t(count) * 4;
                nameStride = 0;
            }
        } else {
            nameStride = 0x18;
            names = ckeys + 4;
            next = ckeys + uint64_t(count) * 6;
        }
        if (end < next) {
            status->Add(3, "build manifest is truncated");
            return false;
        }

        uint32_t speechBit = static_cast<uint32_t>(speech) << 31;
        if ((speechBit == 0 || (block[1] & speechBit) != 0) && (block[1] & excluded) == 0
            && (block[2] & localeMask) != 0) {
            if (counted == nullptr) {
                int32_t fileDataId = 0;
                for (uint32_t left = count; left != 0; left--) {
                    BuildManifestEntry entry;
                    std::memcpy(&entry.m_nameHash, names, 8);
                    entry.m_fileDataId = static_cast<uint32_t>(fileDataId + static_cast<int32_t>(*deltas));
                    std::memcpy(entry.m_ckey, ckeys, 16);
                    entry.m_localeFlags = block[2];
                    entry.m_contentFlags = block[1];
                    bool inserted;
                    target->m_manifest->m_byId.Insert(&entry, &inserted);
                    if (*target->m_countNamed != 0 && (entry.m_contentFlags & CONTENT_NO_NAME_HASH) == 0) {
                        (*target->m_named)++;
                    }
                    deltas++;
                    fileDataId = static_cast<int32_t>(entry.m_fileDataId) + 1;
                    ckeys = reinterpret_cast<const uint32_t*>(
                        reinterpret_cast<const uint8_t*>(ckeys) + (static_cast<uint32_t>(legacy) * 8 + 0x10));
                    names = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(names) + nameStride);
                }
            } else {
                *counted += static_cast<int32_t>(count);
            }
        }
        block = next;
    }
    return true;
}

// The counting pass over one locale bit (the body FUN_1402d6820 repeats per bit).
bool CountBlocks(PARSECONTEXT* context, uint32_t localeMask, uint8_t speech) {
    uint32_t excluded = ExcludedContent(context->m_locale.m_alternate, speech);
    auto end = reinterpret_cast<const uint32_t*>(context->m_blocks + context->m_size);
    auto block = reinterpret_cast<const uint32_t*>(context->m_blocks);
    while (block < end) {
        uint64_t count = block[0];
        uint64_t words;
        if (context->m_legacy == 0 && context->m_namedSeparately != 0 && (block[1] & CONTENT_NO_NAME_HASH) != 0) {
            words = count * 5;
        } else {
            words = count * 7;
        }
        const uint32_t* next = block + words + 3;
        if (end < next) {
            context->m_status->Add(3, "build manifest is truncated");
            return false;
        }
        if (((speech & 1) == 0 || (block[1] & (static_cast<uint32_t>(speech) << 31)) != 0) && (block[1] & excluded) == 0
            && (block[2] & localeMask) != 0 && context->m_total != nullptr) {
            *context->m_total += block[0];
        }
        block = next;
    }
    return true;
}

// ref: FUN_1402d6820
bool CountPass(PARSECONTEXT* context, uint32_t region, uint32_t locale, uint8_t speech) {
    if (region == 3) {
        if (locale == 1) {
            return CountBlocks(context, 0x200, speech) && CountBlocks(context, 0x2, speech);
        }
        if (locale == 0xe) {
            return CountBlocks(context, 0x10000, speech) && CountBlocks(context, 0x4000, speech);
        }
    }
    return CountBlocks(context, 1u << (locale & 0x1f), speech);
}

bool FillWith(PARSECONTEXT* context, uint32_t mask, uint8_t speech) {
    return FillBlocks(context->m_blocks, context->m_size, mask, context->m_locale.m_alternate, speech,
        context->m_legacy, context->m_namedSeparately, context->m_2a, context->m_status, &context->m_fill,
        context->m_counted);
}

// ref: FUN_1402d6600
bool FillPass(PARSECONTEXT* context, uint32_t region, uint32_t locale, uint8_t speech) {
    if (region == 3) {
        if (locale == 1) {
            return FillWith(context, 0x200, speech) && FillWith(context, 0x2, speech);
        }
        if (locale == 0xe) {
            return FillWith(context, 0x10000, speech) && FillWith(context, 0x4000, speech);
        }
    }
    return FillWith(context, 1u << (locale & 0x1f), speech);
}

}

// ref: FUN_1402da000
BuildManifestEntry* ENTRYTABLE::Insert(const BuildManifestEntry* entry, bool* inserted) {
    uint64_t capacity = this->m_capacity;
    float load = static_cast<float>(this->m_count + 1) / static_cast<float>(capacity);
    if (this->m_maxLoad < load) {
        this->Rehash(0);
        capacity = this->m_capacity;
    }
    BuildManifestEntry* entries = this->m_entries;
    uint64_t slot = (capacity - 1) & FnvFileDataId(entry);
    BuildManifestEntry* reuse = nullptr;
    for (uint64_t step = 1;; step++) {
        BuildManifestEntry* candidate = &entries[slot];
        uint32_t id = candidate->m_fileDataId;
        if (id == 0) {
            this->m_count++;
            if (reuse) {
                candidate = reuse;
            }
            *candidate = *entry;
            *inserted = true;
            return candidate;
        }
        if (id == 0xFFFFFFFF) {
            if (!reuse) {
                reuse = candidate;
            }
        } else if (id == entry->m_fileDataId) {
            *inserted = false;
            return candidate;
        }
        slot = (slot + step) & (capacity - 1);
    }
}

// ref: FUN_1402dac60
void ENTRYTABLE::Rehash(uint64_t minimum) {
    uint64_t capacity = TableCapacity(this->m_count - this->m_removed, this->m_maxLoad, minimum);
    if (capacity == this->m_capacity && this->m_removed == 0) {
        return;
    }
    auto entries = static_cast<BuildManifestEntry*>(
        SMemAlloc(capacity * sizeof(BuildManifestEntry), 0xB, s_entryTag, SStrHashFnv1a(s_entryTag), -6, 0));
    for (uint64_t i = 0; i < capacity; i++) {
        entries[i].m_fileDataId = 0;
    }
    BuildManifestEntry* old = this->m_entries;
    for (BuildManifestEntry* entry = old; entry != old + this->m_capacity; entry++) {
        if (IsLive(this, entry)) {
            uint64_t slot = (capacity - 1) & FnvFileDataId(entry);
            for (uint64_t step = 1; entries[slot].m_fileDataId != 0; step++) {
                slot = (slot + step) & (capacity - 1);
            }
            entries[slot] = *entry;
        }
    }
    SMemFree(old, s_entryTag, -6, 0);
    this->m_count -= this->m_removed;
    this->m_removed = 0;
    this->m_entries = entries;
    this->m_capacity = capacity;
}

BuildManifestEntry* ENTRYTABLE::First() const {
    BuildManifestEntry* entry = this->m_entries;
    if (!IsLive(this, entry)) {
        entry = this->Next(entry);
    }
    return entry;
}

BuildManifestEntry* ENTRYTABLE::Next(BuildManifestEntry* entry) const {
    BuildManifestEntry* end = this->End();
    while (++entry < end) {
        if (IsLive(this, entry)) {
            break;
        }
    }
    return entry;
}

// ref: FUN_1402d5e50
BuildManifestEntry** NAMETABLE::Insert(BuildManifestEntry* entry, bool* inserted) {
    float load = static_cast<float>(this->m_count + 1) / static_cast<float>(this->m_capacity);
    if (this->m_maxLoad < load) {
        this->Rehash(0);
    }
    uint64_t capacity = this->m_capacity;
    BuildManifestEntry** slots = this->m_slots;
    uint64_t slot = (capacity - 1) & entry->m_nameHash;
    for (uint64_t step = 1;; step++) {
        BuildManifestEntry* candidate = slots[slot];
        if (candidate == nullptr) {
            this->m_count++;
            slots[slot] = entry;
            *inserted = true;
            return &slots[slot];
        }
        if (candidate->m_nameHash == entry->m_nameHash) {
            *inserted = false;
            return &slots[slot];
        }
        slot = (slot + step) & (capacity - 1);
    }
}

// ref: FUN_1402da7f0
void NAMETABLE::Rehash(uint64_t minimum) {
    uint64_t capacity = TableCapacity(this->m_count - this->m_removed, this->m_maxLoad, minimum);
    if (capacity == this->m_capacity && this->m_removed == 0) {
        return;
    }
    auto slots = static_cast<BuildManifestEntry**>(
        SMemAlloc(capacity * sizeof(BuildManifestEntry*), 0xB, s_slotTag, SStrHashFnv1a(s_slotTag), -6, 0));
    for (uint64_t i = 0; i < capacity; i++) {
        slots[i] = nullptr;
    }
    BuildManifestEntry** old = this->m_slots;
    for (BuildManifestEntry** it = old; it != old + this->m_capacity; it++) {
        BuildManifestEntry* entry = *it;
        if (entry != nullptr) {
            uint64_t slot = (capacity - 1) & entry->m_nameHash;
            for (uint64_t step = 1; slots[slot] != nullptr; step++) {
                slot = (slot + step) & (capacity - 1);
            }
            slots[slot] = entry;
        }
    }
    SMemFree(old, s_slotTag, -6, 0);
    this->m_count -= this->m_removed;
    this->m_removed = 0;
    this->m_slots = slots;
    this->m_capacity = capacity;
}

// ref: FUN_1402daef0
void NAMETABLE::Reserve(uint64_t count) {
    this->Rehash(static_cast<uint64_t>(static_cast<float>(count) / this->m_maxLoad + 0.5f));
}

// ref: FUN_1402d6320
void BUILDMANIFEST::Construct() {
    this->m_byId.m_capacity = 8;
    this->m_byId.m_count = 0;
    this->m_byId.m_removed = 0;
    this->m_byId.m_entries = static_cast<BuildManifestEntry*>(
        SMemAlloc(8 * sizeof(BuildManifestEntry), 0xB, s_entryTag, SStrHashFnv1a(s_entryTag), -6, 0));
    for (uint64_t i = 0; i < this->m_byId.m_capacity; i++) {
        this->m_byId.m_entries[i].m_fileDataId = 0;
    }
    this->m_byId.m_maxLoad = 0.75f;
    this->m_byName.m_capacity = 8;
    this->m_byName.m_count = 0;
    this->m_byName.m_removed = 0;
    this->m_byName.m_slots = static_cast<BuildManifestEntry**>(
        SMemAlloc(8 * sizeof(BuildManifestEntry*), 0xB, s_slotTag, SStrHashFnv1a(s_slotTag), -6, 0));
    for (uint64_t i = 0; i < this->m_byName.m_capacity; i++) {
        this->m_byName.m_slots[i] = nullptr;
    }
    this->m_byName.m_maxLoad = 0.75f;
}

// ref: FUN_1402d8830
bool BUILDMANIFEST::Parse(const uint8_t* const* data, uint64_t size, const MANIFESTLOCALE* locale, uint8_t a5,
    CStatus* status, const uint32_t* capacity) {
    auto header = reinterpret_cast<const uint32_t*>(*data);
    if (size < 4) {
        status->Add(3, "build manifest is truncated");
        return false;
    }

    uint8_t legacy;
    uint32_t named;
    uint32_t total;
    uint32_t headerSize = 0;
    if (header[0] == TSFM) {
        legacy = 0;
        if (size < 0xc) {
            status->Add(3, "build manifest is truncated");
            return false;
        }
        named = header[2];
        total = header[1];
        if (named == 1) {
            // {magic, header size, version 1, total, named}
            headerSize = total < 5 ? 4 : total;
            if (size < headerSize) {
                status->Add(3, "build manifest is truncated");
                return false;
            }
            named = header[4];
            total = header[3];
        } else {
            headerSize = 0xc;
        }
    } else {
        // A legacy root has no header; 28 bytes a record is the most it can hold.
        legacy = 1;
        named = static_cast<uint32_t>(size / 28);
        total = named;
    }
    uint8_t namedSeparately = named != total;
    const uint8_t* blocks = *data + headerSize;

    for (uint64_t i = 0; i < this->m_byName.m_capacity; i++) {
        this->m_byName.m_slots[i] = nullptr;
    }
    this->m_byName.m_count = 0;
    this->m_byName.m_removed = 0;
    for (uint64_t i = 0; i < this->m_byId.m_capacity; i++) {
        this->m_byId.m_entries[i].m_fileDataId = 0;
    }
    this->m_byId.m_count = 0;
    this->m_byId.m_removed = 0;

    PARSECONTEXT context = {};
    uint64_t want;
    if (capacity == nullptr) {
        uint32_t counted = 0;
        context.m_blocks = blocks;
        context.m_size = size - headerSize;
        context.m_locale = *locale;
        context.m_status = status;
        context.m_legacy = legacy;
        context.m_namedSeparately = namedSeparately;
        context.m_2a = a5;
        context.m_total = &counted;
        if (!CountPass(&context, locale->m_region, locale->m_text, 0)) {
            return false;
        }
        if (!CountPass(&context, locale->m_region, locale->m_speech, 1)) {
            return false;
        }
        float count = static_cast<float>(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(counted))));
        want = static_cast<uint64_t>(count / this->m_byId.m_maxLoad + 0.5f);
    } else {
        want = *capacity;
    }
    this->m_byId.Rehash(want);

    uint32_t namedCount = namedSeparately ? 0 : named;
    context.m_blocks = blocks;
    context.m_size = size - headerSize;
    context.m_locale = *locale;
    context.m_status = status;
    context.m_legacy = legacy;
    context.m_namedSeparately = namedSeparately;
    context.m_2a = a5;
    context.m_fill.m_manifest = this;
    context.m_fill.m_named = &namedCount;
    context.m_fill.m_countNamed = &namedSeparately;
    context.m_counted = nullptr;
    if (!FillPass(&context, locale->m_region, locale->m_text, 0)
        || !FillPass(&context, locale->m_region, locale->m_speech, 1)) {
        return false;
    }

    bool inserted;
    if (!namedSeparately) {
        this->m_byName.Reserve(this->m_byId.m_count - this->m_byId.m_removed);
        for (BuildManifestEntry* entry = this->m_byId.First(); entry != this->m_byId.End();
             entry = this->m_byId.Next(entry)) {
            this->m_byName.Insert(entry, &inserted);
        }
    } else {
        this->m_byName.Reserve(static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(namedCount))));
        for (BuildManifestEntry* entry = this->m_byId.First(); entry != this->m_byId.End();
             entry = this->m_byId.Next(entry)) {
            if ((entry->m_contentFlags & CONTENT_NO_NAME_HASH) == 0) {
                this->m_byName.Insert(entry, &inserted);
            }
        }
    }
    return true;
}

// ref: FUN_1402d61f0
void CASMANIFEST::Construct() {
    this->m_current = nullptr;
    this->m_pending = nullptr;
    this->m_10 = 0;
    this->m_diffCapacity = 8;
    this->m_diffCount = 0;
    this->m_diffRemoved = 0;
    this->m_diffs = static_cast<uint8_t*>(SMemAlloc(0x50, 0xB, s_diffTag, SStrHashFnv1a(s_diffTag), -6, 0));
    for (uint64_t i = 0; i < this->m_diffCapacity; i++) {
        uint8_t* diff = this->m_diffs + i * 10;
        diff[9] = 0;
        std::memset(diff, 0, 4);
    }
    this->m_diffMaxLoad = 0.75f;
    auto manifest = static_cast<BUILDMANIFEST*>(SMemAlloc(sizeof(BUILDMANIFEST), 10, __FILE__, SStrHashFnv1a(__FILE__), 0x68, 0));
    manifest->Construct();
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&this->m_current), manifest);
}

// ref: FUN_1402d6530
void CASMANIFEST::Destroy() {
    if (this->m_current != nullptr) {
        FreeManifest(this->m_current);
    }
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&this->m_current), nullptr);
    if (this->m_pending != nullptr) {
        FreeManifest(this->m_pending);
    }
    this->m_pending = nullptr;
    SMemFree(this->m_diffs, s_diffTag, -6, 0);
}

// ref: FUN_1402d7c60
BuildManifestEntry* CASMANIFEST::FindByName(uint64_t nameHash) const {
    const NAMETABLE& table = this->m_current->m_byName;
    uint64_t slot = (table.m_capacity - 1) & nameHash;
    for (uint64_t step = 1; table.m_slots[slot] != nullptr; step++) {
        if (table.m_slots[slot]->m_nameHash == nameHash) {
            return table.m_slots[slot];
        }
        slot = (slot + step) & (table.m_capacity - 1);
    }
    return nullptr;
}

// ref: FUN_1402d7ce0
BuildManifestEntry* CASMANIFEST::FindById(uint32_t fileDataId) const {
    const ENTRYTABLE& table = this->m_current->m_byId;
    BuildManifestEntry key;
    key.m_fileDataId = fileDataId;
    uint64_t slot = (table.m_capacity - 1) & FnvFileDataId(&key);
    for (uint64_t step = 1; table.m_entries[slot].m_fileDataId != 0; step++) {
        uint32_t id = table.m_entries[slot].m_fileDataId;
        if ((table.m_removed == 0 || id != 0xFFFFFFFF) && id == fileDataId) {
            return &table.m_entries[slot];
        }
        slot = (slot + step) & (table.m_capacity - 1);
    }
    return nullptr;
}

// ref: FUN_1402d70d0
void CASMANIFEST::BeginWalk() {
    if (s_cursorStarted) {
        return;
    }
    AcquireSRWLockExclusive(&s_cursorLock);
    if (!s_cursorStarted) {
        InterlockedExchange8(reinterpret_cast<volatile char*>(&s_cursorStarted), 1);
        BUILDMANIFEST* manifest = this->m_current;
        g_manifestCursor.m_end = manifest->m_byId.End();
        g_manifestCursor.m_entry = manifest->m_byId.First();
        g_manifestCursor.m_manifest = manifest;
    }
    ReleaseSRWLockExclusive(&s_cursorLock);
}

// ref: FUN_1402d7010
void CASMANIFEST::NextEntry(BuildManifestEntry* out) {
    AcquireSRWLockExclusive(&s_cursorLock);
    BuildManifestEntry* entry = g_manifestCursor.m_entry;
    if (entry == this->m_current->m_byId.End()) {
        out->m_fileDataId = 0;
    } else {
        *out = *entry;
        g_manifestCursor.m_entry = g_manifestCursor.m_manifest->m_byId.Next(entry);
    }
    ReleaseSRWLockExclusive(&s_cursorLock);
}

// ref: FUN_1402d8440
bool CASMANIFEST::Load(const uint8_t* const* data, uint64_t size, const MANIFESTLOCALE* locale, CStatus* status) {
    auto manifest = static_cast<BUILDMANIFEST*>(SMemAlloc(sizeof(BUILDMANIFEST), 9, __FILE__, SStrHashFnv1a(__FILE__), 0x239, 0));
    manifest->Construct();
    manifest->m_locale = *locale;
    if (!manifest->Parse(data, size, locale, 0, status, nullptr)) {
        FreeManifest(manifest);
        return false;
    }

    AcquireSRWLockExclusive(&s_cursorLock);
    BUILDMANIFEST* old = this->m_current;
    g_manifestCursor.m_end = manifest->m_byId.End();
    g_manifestCursor.m_entry = manifest->m_byId.First();
    g_manifestCursor.m_manifest = manifest;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&this->m_current), manifest);
    if (old != nullptr) {
        FreeManifest(old);
    }
    ReleaseSRWLockExclusive(&s_cursorLock);
    return true;
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142e25828 = (void*)&cas::s_cursorLock;
// ---- end of generated globals ----
