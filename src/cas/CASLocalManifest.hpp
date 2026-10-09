#ifndef CAS_CAS_LOCAL_MANIFEST_HPP
#define CAS_CAS_LOCAL_MANIFEST_HPP

// The local manifest: loose files under the install that stand in for archive content.
// Initialization walks the directory tree from the client's working directory; every file
// that is not on the exclusion list is entered by the name hash of its path (relative to the
// root, after an extension handler has had its say: ".tga" is looked up as ".blp", ".wav"
// as ".ogg"), and by the file data id the build manifest gives that name -- or a negative one
// of its own when the name is unknown.

#include "blz/Function.hpp"
#include <cstddef>
#include <cstdint>

namespace cas {

// 0x10c bytes.
struct LocalManifestEntry {
    char m_path[0x104];             // 0x000: the file on disk
    int32_t m_fileDataId;           // 0x104
    uint8_t m_byId;                 // 0x108: entered in the by-id table
};
static_assert(sizeof(LocalManifestEntry) == 0x10c, "LocalManifestEntry is 0x10c bytes");

// blz::chained_hash_node<pair<uint64_t const, LocalManifestEntry>> (0x120 bytes).
struct LOCALNAMENODE {
    LOCALNAMENODE* m_next;
    uint64_t m_key;
    LocalManifestEntry m_value;
};
static_assert(sizeof(LOCALNAMENODE) == 0x120, "LOCALNAMENODE is 0x120 bytes");

// blz::chained_hash_node<pair<int const, LocalManifestEntry*>> (0x18 bytes).
struct LOCALIDNODE {
    LOCALIDNODE* m_next;
    int32_t m_key;
    LocalManifestEntry* m_value;
};
static_assert(sizeof(LOCALIDNODE) == 0x18, "LOCALIDNODE is 0x18 bytes");

// blz::chained_hash_map: buckets of singly linked nodes, keyed by fmix64 of the key.
template <class NODE>
struct LOCALTABLE {
    uint64_t m_capacity;            // 0x00
    NODE** m_buckets;               // 0x08: allocated on first insert
    uint64_t m_count;               // 0x10
    float m_maxLoad;                // 0x18

    void Rehash(uint64_t minimum);
    void Clear();
    void Destroy();
};
typedef LOCALTABLE<LOCALNAMENODE> LOCALNAMETABLE;
typedef LOCALTABLE<LOCALIDNODE> LOCALIDTABLE;
static_assert(sizeof(LOCALNAMETABLE) == 0x20, "LOCALTABLE is 0x20 bytes");

// 0x148 bytes (DAT_142e20860).
struct LOCALMANIFEST {
    char m_root[0x104];             // 0x000
    int32_t m_nextLocalId;          // 0x104: counts down from 0 for names the build lacks
    LOCALNAMETABLE m_byName;        // 0x108
    LOCALIDTABLE m_byId;            // 0x128

    void Construct();
    void Destroy();
    // Sets the root and rescans it.
    void SetRoot(const char* root);
    // Enters one file (relative to `root`, or as given when root is null); returns its id.
    int32_t Add(int32_t fileDataId, const char* path, const char* root, uint8_t byId,
        blz::function_storage* onExisting);
    // The loose file entered under an id, or null.
    LocalManifestEntry* FindById(int32_t fileDataId);
    // The path of the loose file entered under a name hash (and its id), or null.
    const char* FindPathByName(uint64_t nameHash, int32_t* fileDataId);
};
static_assert(offsetof(LOCALMANIFEST, m_byName) == 0x108, "LOCALMANIFEST layout");
static_assert(offsetof(LOCALMANIFEST, m_byId) == 0x128, "LOCALMANIFEST layout");
static_assert(sizeof(LOCALMANIFEST) == 0x148, "LOCALMANIFEST is 0x148 bytes");

// What a directory walk passes down (0x30 bytes): the directory so far, the manifest, and a
// callback for a file whose name is already entered.
struct LOCALSCAN {
    const char* m_directory;
    LOCALMANIFEST* m_manifest;
    blz::function_storage m_onExisting;
};
static_assert(sizeof(LOCALSCAN) == 0x30, "LOCALSCAN is 0x30 bytes");

// Maps a loose file's name to the name it is looked up by: (dot extension, native path,
// destination, destination size).
typedef bool (*LOCALFILEHANDLER)(const char* extension, const char* path, char* dest, uint64_t size);

// Registers a handler for an extension (with its dot; matched lower-cased).
void LocalManifestRegisterHandler(LOCALFILEHANDLER handler, const char* extension);
// Frees the handler table (an atexit destructor in the reference).
void LocalManifestDestroyHandlers();

// The name hash a root entry is keyed by: lookup3 over the upper-cased, back-slashed path.
uint64_t LocalManifestNameHash(const char* path);

}

#endif
