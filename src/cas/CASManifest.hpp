#ifndef CAS_CAS_MANIFEST_HPP
#define CAS_CAS_MANIFEST_HPP

// Engine/Source/CAS/CASManifest.cpp: the build manifest, the game's ".root" file read into two
// open-addressing tables -- entries by file data id, and pointers to them by name hash.
//
// The root is a run of blocks {count, content flags, locale flags, count file data id deltas,
// then the records}. A legacy root (no "TSFM" magic) interleaves {ckey, name hash} per record;
// the TSFM form has every ckey, then every name hash, and since version 1 a block whose content
// flags carry 0x10000000 has no name hashes at all. Only blocks matching the client's locale and
// content filter are taken, in two passes: the text locale, then the speech locale (the blocks
// with content bit 31).

#include <cstddef>
#include <cstdint>

class CStatus;

namespace cas {

// One file (0x24 bytes).
#pragma pack(push, 4)
struct BuildManifestEntry {
    uint64_t m_nameHash;            // 0x00
    uint32_t m_fileDataId;          // 0x08: 0 empty, -1 removed
    uint32_t m_localeFlags;         // 0x0c
    uint32_t m_contentFlags;        // 0x10
    uint8_t m_ckey[16];             // 0x14
};
#pragma pack(pop)
static_assert(sizeof(BuildManifestEntry) == 0x24, "BuildManifestEntry is 0x24 bytes");

// What the manifest is filtered by (0x10 bytes).
struct MANIFESTLOCALE {
    uint32_t m_region;              // 3 (EU): enUS also takes enGB, ptBR also takes ptPT
    uint32_t m_text;                // the locale index of the first pass
    uint32_t m_speech;              // the locale index of the second pass
    uint8_t m_alternate;            // overrideArchive: takes the alternate-data blocks (content 0x80)
    uint8_t m_pad[3];
};
static_assert(sizeof(MANIFESTLOCALE) == 0x10, "MANIFESTLOCALE is 0x10 bytes");

// Entries by file data id (FNV-1a 64 over its four bytes), linear probing with a growing step.
struct ENTRYTABLE {
    uint64_t m_capacity;            // 0x00
    BuildManifestEntry* m_entries;  // 0x08
    uint64_t m_count;               // 0x10
    uint64_t m_removed;             // 0x18
    float m_maxLoad;                // 0x20

    BuildManifestEntry* Insert(const BuildManifestEntry* entry, bool* inserted);
    void Rehash(uint64_t capacity);
    BuildManifestEntry* First() const;
    BuildManifestEntry* Next(BuildManifestEntry* entry) const;
    BuildManifestEntry* End() const {
        return this->m_entries + this->m_capacity;
    }
};
static_assert(sizeof(ENTRYTABLE) == 0x28, "ENTRYTABLE is 0x28 bytes");

// Pointers to the entries, by name hash (its low bits are the slot).
struct NAMETABLE {
    uint64_t m_capacity;            // 0x00
    BuildManifestEntry** m_slots;   // 0x08
    uint64_t m_count;               // 0x10
    uint64_t m_removed;             // 0x18
    float m_maxLoad;                // 0x20

    BuildManifestEntry** Insert(BuildManifestEntry* entry, bool* inserted);
    void Rehash(uint64_t capacity);
    void Reserve(uint64_t count);
};
static_assert(sizeof(NAMETABLE) == 0x28, "NAMETABLE is 0x28 bytes");

// 0x60 bytes.
struct BUILDMANIFEST {
    ENTRYTABLE m_byId;              // 0x00
    NAMETABLE m_byName;             // 0x28
    MANIFESTLOCALE m_locale;        // 0x50

    void Construct();
    bool Parse(const uint8_t* const* data, uint64_t size, const MANIFESTLOCALE* locale, uint8_t a5,
        CStatus* status, const uint32_t* capacity);
};
static_assert(offsetof(BUILDMANIFEST, m_byName) == 0x28, "BUILDMANIFEST layout");
static_assert(offsetof(BUILDMANIFEST, m_locale) == 0x50, "BUILDMANIFEST layout");
static_assert(sizeof(BUILDMANIFEST) == 0x60, "BUILDMANIFEST is 0x60 bytes");

// What the CAS keeps (0x40 bytes): the current manifest and the 10-byte diff entries of an
// in-place update (BuildManifestDiffEntry; the update path is not ported).
struct CASMANIFEST {
    BUILDMANIFEST* m_current;       // 0x00
    BUILDMANIFEST* m_pending;       // 0x08: a second manifest (the in-place update path)
    uint8_t m_10;                   // 0x10
    uint64_t m_diffCapacity;        // 0x18
    uint8_t* m_diffs;               // 0x20
    uint64_t m_diffCount;           // 0x28
    uint64_t m_diffRemoved;         // 0x30
    float m_diffMaxLoad;            // 0x38

    void Construct();
    // Reads a root into a new manifest and swaps it in; the old one is freed.
    bool Load(const uint8_t* const* data, uint64_t size, const MANIFESTLOCALE* locale, CStatus* status);
    // Frees both manifests and the diff table (the destructor, inlined at the shutdown).
    void Destroy();

    BuildManifestEntry* FindByName(uint64_t nameHash) const;
    BuildManifestEntry* FindById(uint32_t fileDataId) const;
};
static_assert(sizeof(CASMANIFEST) == 0x40, "CASMANIFEST is 0x40 bytes");

// The current manifest and its first live entry, kept beside it under a lock
// (DAT_142e25810/18/20, lock DAT_142e25828).
struct MANIFESTCURSOR {
    BUILDMANIFEST* m_manifest;
    BuildManifestEntry* m_entry;
    BuildManifestEntry* m_end;
};
extern MANIFESTCURSOR g_manifestCursor;

}

#endif
