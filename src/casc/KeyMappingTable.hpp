#ifndef CASC_KEY_MAPPING_TABLE_HPP
#define CASC_KEY_MAPPING_TABLE_HPP

// pkg/casc/1.4.41/src/dynamic/key_mapping_table.cpp and key_state.cpp: one bucket's index
// file (".idx"). Keys are the first nine bytes of encoding keys; each maps to a span of the
// archives (a 40-bit offset - archive number in the top bits - and a 32-bit size). A table is
// a sorted section written when the table is flushed, then pages of updates appended in place.

#include "blz/Allocator.hpp"
#include "casc/Common.hpp"
#include "os/Os.hpp"
#include <cstddef>
#include <cstdint>

namespace casc {

struct SPAN {
    uint64_t m_offset;
    uint32_t m_size;
    uint32_t m_pad;
};

// A sorted entry (0x12 bytes): key, 40-bit big-endian offset, 32-bit size.
const uint32_t SORTED_ENTRY = 0x12;
// An update entry (0x18 bytes, 21 to a 0x200-byte page): hash, key, offset, size, type.
const uint32_t UPDATE_ENTRY = 0x18;
const uint32_t UPDATES_PER_PAGE = 0x15;
const uint32_t UPDATE_PAGE = 0x200;

// The state of one key: whether it is present, its span, and which parts of it are resident
// (0x138 bytes; up to 16 transitions inline).
struct KEYSTATE {
    blz::allocator* m_allocator;    // 0x00
    STATSINK* m_stats;              // 0x08
    uint8_t m_present;              // 0x10
    SPAN m_span;                    // 0x18
    struct TRANSITION {
        uint64_t m_position;
        int32_t m_delta;
        uint32_t m_pad;
    } m_inline[16];                 // 0x28
    TRANSITION* m_transitions;      // 0x128
    uint32_t m_count;               // 0x130
    uint32_t m_capacity;            // 0x134

    void Init(blz::allocator* allocator, STATSINK* stats);
    void Destroy();
    void Apply(uint8_t type, const SPAN* span);
    void AddSpan(const SPAN* span, bool resident);
    void Normalize();
    void Build(const uint8_t* sorted, const uint8_t* sortedEnd, uint8_t** updates, uint8_t** updatesEnd,
        const uint8_t* records, const uint8_t* recordsEnd);
    bool Covers(const SPAN* span, bool resident);
    int32_t Validate(uint8_t type, const SPAN* span, bool* noop);
    uint32_t RangeCount();
    void Range(uint32_t index, uint64_t* start, uint64_t* end);
};
static_assert(offsetof(KEYSTATE, m_transitions) == 0x128, "KEYSTATE layout");
static_assert(sizeof(KEYSTATE) == 0x138, "KEYSTATE is 0x138 bytes");

// (context, key, sorted range, update pointer range, record range) -> 0 to continue.
typedef int32_t (*KMTVISITOR)(void* context, const uint8_t* key, const uint8_t* sorted, const uint8_t* sortedEnd,
    uint8_t** updates, uint8_t** updatesEnd, const uint8_t* records, const uint8_t* recordsEnd);

// (context, key, span, kind) -> false to stop. kind: 1 not resident, 2 partly, 3 fully.
typedef bool (*KMTSPANVISITOR)(void* context, const uint8_t* key, const SPAN* span, int32_t kind);

// An extra record handed to the enumerator (0x20 bytes).
struct KMTRECORD {
    uint8_t m_key[9];
    uint8_t m_pad[7];
    uint64_t m_offset;
    uint32_t m_size;
    uint32_t m_pad2;
};

// The table (0x78 bytes).
struct KEYMAPPINGTABLE {
    blz::allocator* m_allocator;    // 0x00
    STATSINK* m_stats;              // 0x08
    int32_t m_mode;                 // 0x10
    uint32_t m_bucket;              // 0x14
    OSMAPPING m_mapping;            // 0x18
    uint64_t m_maxSize;             // 0x40
    uint8_t* m_data;                // 0x48
    uint32_t m_size;                // 0x50
    uint8_t* m_sorted;              // 0x58
    uint32_t m_sortedCount;         // 0x60
    uint8_t* m_updates;             // 0x68
    uint32_t m_updatePages;         // 0x70

    void Init();
    void Setup(blz::allocator* allocator, STATSINK* stats);
    void Destroy();
    int32_t Bind(OSFILE* file, int32_t mode, uint32_t bucket, uint64_t maxSize);
    void Unbind();
    bool HasUpdates();
    uint32_t CountKeys();
    int32_t Enumerate(KMTSPANVISITOR visitor, void* context, uint32_t skip);
    int32_t Lookup(const uint8_t key[9], SPAN* span);
    int32_t QuerySpan(const uint8_t key[9], const SPAN* request, SPAN* out, SPAN* stored);
    int32_t GetSpan(const uint8_t key[9], const SPAN* request);
    int32_t ResolveSpan(const uint8_t key[9], const SPAN* request, SPAN* out, SPAN* stored);
    int32_t Update(const uint8_t key[9], uint8_t type, const SPAN* span, SPAN* previous);
    int32_t Write(OSFILE* file, KMTRECORD* records, uint32_t count);

    void Find(const uint8_t key[9], KEYSTATE* state, uint32_t* page, uint32_t* index);
    int32_t Merge(KMTVISITOR visitor, void* context, KMTRECORD* records, uint32_t count);
};
static_assert(offsetof(KEYMAPPINGTABLE, m_maxSize) == 0x40, "KEYMAPPINGTABLE layout");
static_assert(sizeof(KEYMAPPINGTABLE) == 0x78, "KEYMAPPINGTABLE is 0x78 bytes");

}

#endif
