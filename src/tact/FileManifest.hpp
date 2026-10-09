#ifndef TACT_FILE_MANIFEST_HPP
#define TACT_FILE_MANIFEST_HPP

// pkg/tact/3.6.39/src/file_manifest: the build's file manifest (the "vfs-root" TVFS file).
// The root data lives right after a small reader header in one block (the root reader); the
// manifest keeps the readers by root e-key, the current one, and the set of e-keys of the VFS
// sub-manifests the build config lists.

#include "blz/Allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace tact {

class Instance;
struct CONFIGRECORD;
struct FILEKEY;

// vtable 0x1428ecb00; 0x48 bytes, the root data after it.
class RootReader {
    public:
    volatile int32_t m_refs;        // 0x08
    const uint8_t* m_data;          // 0x10
    uint32_t m_size;                // 0x18
    uint8_t m_version;              // 0x1c
    uint8_t m_eKeySize;             // 0x1d
    uint8_t m_pKeySize;             // 0x1e
    uint8_t m_cftOffsetSize;        // 0x1f: bytes of an offset into the container file table
    uint8_t m_eSpecOffsetSize;      // 0x20
    uint8_t m_21;                   // 0x21
    uint16_t m_maxDepth;            // 0x22
    uint32_t m_flags;               // 0x24: 2 the e-spec table is present
    uint32_t m_pathTableOffset;     // 0x28
    uint32_t m_pathTableSize;       // 0x2c
    uint32_t m_vfsTableOffset;      // 0x30
    uint32_t m_vfsTableSize;        // 0x34
    uint32_t m_cftTableOffset;      // 0x38
    uint32_t m_cftTableSize;        // 0x3c
    uint32_t m_eSpecTableOffset;    // 0x40
    uint32_t m_eSpecTableSize;      // 0x44

    virtual ~RootReader() = default;
    virtual void Destroy();

    void AddRef();
    void Release();
    bool Parse(const uint8_t* data, uint32_t size);
    const uint8_t* Walk(const char** path);
    uint8_t MatchFragment(const char** path, bool* hex, const uint8_t* fragment, uint8_t length);
    bool Lookup(const char** path, FILEKEY* key);
    bool ResolveVfs(uint32_t offset, FILEKEY* key);
    bool ResolveCft(uint32_t offset, FILEKEY* key);
};
static_assert(offsetof(RootReader, m_data) == 0x10, "RootReader layout");
static_assert(offsetof(RootReader, m_flags) == 0x24, "RootReader layout");
static_assert(sizeof(RootReader) == 0x48, "RootReader is 0x48 bytes");

// A reader with room for `size` bytes of root data, which *data points at.
RootReader** RootReaderCreate(RootReader** out, uint32_t size, uint8_t** data);

// The readers by root e-key (0x2c bytes; entries hold a reference).
struct ROOTMAP {
    struct ENTRY {
        uint8_t m_key[16];
        RootReader* m_reader;
    };

    uint32_t* m_buckets;            // 0x00
    ENTRY* m_entries;               // 0x08
    uint32_t* m_next;               // 0x10
    blz::allocator* m_allocator;    // 0x18
    uint32_t m_bucketCount;         // 0x20
    uint32_t m_count;               // 0x24
    uint32_t m_capacity;            // 0x28

    static uint32_t Hash(const uint8_t key[16]);
    ENTRY* Insert(const uint8_t key[16], RootReader** reader, bool* inserted);
    void Reserve(uint32_t capacity);
    void Rehash(uint32_t buckets);
    void Clear();
};

// A set of 16-byte keys (0x2c bytes).
struct KEYSET {
    uint32_t* m_buckets;            // 0x00
    uint8_t (*m_entries)[16];       // 0x08
    uint32_t* m_next;               // 0x10
    blz::allocator* m_allocator;    // 0x18
    uint32_t m_bucketCount;         // 0x20
    uint32_t m_count;               // 0x24
    uint32_t m_capacity;            // 0x28

    bool Contains(const uint8_t key[16]) const;
    uint8_t* Append(uint32_t slot);
    void Reserve(uint32_t capacity);
    void Rehash(uint32_t buckets);
    void Destroy();
};

// 0x78 bytes, at instance+0x300.
struct FILEMANIFEST {
    Instance* m_instance;           // 0x00
    SRWLOCK m_lock;                 // 0x08
    RootReader* m_current;          // 0x10
    ROOTMAP m_roots;                // 0x18
    KEYSET m_vfsKeys;               // 0x48

    void Init(Instance* instance);
    void Destroy();
    uint8_t Load(const CONFIGRECORD* root, const CONFIGRECORD* vfs, uint64_t vfsCount, const uint8_t* data,
        RootReader** reader);
    uint8_t Lookup(const char* path, FILEKEY* key, uint8_t loadMissing);
    uint8_t LookupPath(const char* path, FILEKEY* key, RootReader** reader, uint8_t loadMissing);
    uint8_t LookupSpecial(const char* name, FILEKEY* key);
    ROOTMAP::ENTRY* FindRoot(const uint8_t key[16]);
    uint8_t LoadSubRoot(const FILEKEY* key, RootReader** out);
    uint8_t RegisterSubRoot(const FILEKEY* key, const uint8_t* data, RootReader** reader, RootReader** out);
};
static_assert(offsetof(FILEMANIFEST, m_roots) == 0x18, "FILEMANIFEST layout");
static_assert(offsetof(FILEMANIFEST, m_vfsKeys) == 0x48, "FILEMANIFEST layout");
static_assert(sizeof(FILEMANIFEST) == 0x78, "FILEMANIFEST is 0x78 bytes");

}

#endif
