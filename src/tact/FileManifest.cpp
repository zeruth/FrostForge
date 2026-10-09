#include "tact/FileManifest.hpp"
#include "blz/Murmur.hpp"
#include "tact/Common.hpp"
#include "tact/FileRead.hpp"
#include "tact/ImmediateRead.hpp"
#include "tact/Instance.hpp"
#include <cstring>
#include <new>

namespace tact {

namespace {

const char s_readerFile[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\file_manifest\\file_manifest_reader.cpp";
const char s_noTag[] = "";  // DAT_1426178ba

const uint32_t s_tvfsMagic = 0x53465654;  // "TVFS" (DAT_1428ecae8)

uint32_t ReadBE32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t RoundUpPow2(uint32_t value) {
    if ((value & (value - 1)) != 0) {
        int32_t bit = 31;
        while (((value | 1) >> bit) == 0) {
            bit--;
        }
        value = 1u << ((bit + 1) & 31);
    }
    return value;
}

// The bytes a value up to `limit` takes.
uint8_t OffsetSize(uint32_t limit) {
    if (limit > 0xFFFFFF) {
        return 4;
    }
    if (limit > 0xFFFF) {
        return 3;
    }
    return static_cast<uint8_t>((limit > 0xFF) + 1);
}

}

// --- RootReader ------------------------------------------------------------------------------

// ref: FUN_142111870
void RootReader::Destroy() {
    this->~RootReader();
    auto function = static_cast<blz::function_allocator*>(GetAllocator());
    function->m_function(function->m_context, this, 0, 0, true, nullptr, 0);
}

void RootReader::AddRef() {
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&this->m_refs));
}

void RootReader::Release() {
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(&this->m_refs)) == 0) {
        this->Destroy();
    }
}

// ref: FUN_1421112c0
RootReader** RootReaderCreate(RootReader** out, uint32_t size, uint8_t** data) {
    void* memory = PackageAlloc(GetAllocator(), uint64_t(size) + 0x48, 8, true, s_noTag, 0);
    if (!memory) {
        *out = nullptr;
        *data = reinterpret_cast<uint8_t*>(sizeof(RootReader));
        return out;
    }
    auto reader = new (memory) RootReader();
    reader->m_refs = 0;
    reader->m_data = nullptr;
    reader->m_size = 0;
    reader->m_version = 0;
    reader->m_eKeySize = 0;
    reader->m_pKeySize = 0;
    reader->m_cftOffsetSize = 0;
    reader->m_eSpecOffsetSize = 0;
    reader->m_21 = 0;
    reader->m_maxDepth = 0;
    reader->m_flags = 0;
    reader->m_pathTableOffset = 0;
    reader->m_pathTableSize = 0;
    reader->m_vfsTableOffset = 0;
    reader->m_vfsTableSize = 0;
    reader->m_cftTableOffset = 0;
    reader->m_cftTableSize = 0;
    reader->m_eSpecTableOffset = 0;
    reader->m_eSpecTableSize = 0;
    *data = reinterpret_cast<uint8_t*>(reader) + sizeof(RootReader);
    *out = reader;
    reader->AddRef();
    return out;
}

// The TVFS header: version 1, key sizes, flags, then the path, VFS, container file and
// e-spec tables (offset and size each, big-endian).
// ref: FUN_142111bf0
bool RootReader::Parse(const uint8_t* data, uint32_t size) {
    this->m_data = data;
    this->m_size = size;
    if (size < 0x26) {
        blz::log(g_logCategory, 4, s_readerFile, 0x9d,
            "Detected truncated file manifest. Only got %u bytes, but minimum header size is %u bytes.", size, 0x26);
        this->m_data = nullptr;
        this->m_size = 0;
        return false;
    }
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != s_tvfsMagic) {
        blz::log(g_logCategory, 4, s_readerFile, 0xa5, "Invalid magic string in file manifest.");
        this->m_data = nullptr;
        this->m_size = 0;
        return false;
    }
    uint8_t version = data[4];
    this->m_version = version;
    if (version == 0 || version >= 2) {
        blz::log(g_logCategory, 4, s_readerFile, 0xb2,
            "Unsupported file manifest version: %u. This client only supports non-zero versions <= %u", version, 1);
        this->m_data = nullptr;
        this->m_size = 0;
        return false;
    }
    this->m_eKeySize = data[6];
    this->m_pKeySize = data[7];
    this->m_flags = ReadBE32(data + 8);
    this->m_pathTableOffset = ReadBE32(data + 0xc);
    this->m_pathTableSize = ReadBE32(data + 0x10);
    this->m_vfsTableOffset = ReadBE32(data + 0x14);
    this->m_vfsTableSize = ReadBE32(data + 0x18);
    this->m_cftTableOffset = ReadBE32(data + 0x1c);
    this->m_cftTableSize = ReadBE32(data + 0x20);
    this->m_maxDepth = static_cast<uint16_t>(uint32_t(data[0x24]) << 8 | data[0x25]);
    if ((data[0xb] & 2) == 0) {
        blz::log(g_logCategory, 3, s_readerFile, 0xef,
            "File manifest is missing encoding specifiers. Reads of block encoded files may be slower, and non-block encoded files will fail to load.");
    } else {
        this->m_eSpecTableOffset = ReadBE32(data + 0x26);
        this->m_eSpecTableSize = ReadBE32(data + 0x2a);
        this->m_eSpecOffsetSize = OffsetSize(this->m_eSpecTableSize);
    }
    this->m_cftOffsetSize = OffsetSize(this->m_cftTableSize);
    return true;
}

// --- ROOTMAP / KEYSET ------------------------------------------------------------------------

uint32_t ROOTMAP::Hash(const uint8_t key[16]) {
    return blz::murmur3_32(key, 9, 0);
}

// The entry takes the reader over when it is new.
// ref: FUN_1420e84d0
ROOTMAP::ENTRY* ROOTMAP::Insert(const uint8_t key[16], RootReader** reader, bool* inserted) {
    uint32_t slot = (this->m_bucketCount - 1) & Hash(key);
    if (this->m_buckets) {
        for (uint32_t i = this->m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_next[i]) {
            if (std::memcmp(this->m_entries[i].m_key, key, 16) == 0) {
                *inserted = false;
                return &this->m_entries[i];
            }
        }
    }
    uint32_t index = this->m_count;
    if (index == this->m_capacity) {
        this->Reserve(this->m_capacity * 2);
    }
    this->m_next[index] = this->m_buckets[slot];
    this->m_buckets[slot] = index;
    this->m_count++;
    ENTRY* entry = &this->m_entries[index];
    std::memcpy(entry->m_key, key, 16);
    entry->m_reader = *reader;
    *reader = nullptr;
    if (this->m_bucketCount < this->m_count) {
        this->Rehash(this->m_bucketCount * 2);
    }
    *inserted = true;
    return entry;
}

// ref: FUN_1420e9660
void ROOTMAP::Reserve(uint32_t capacity) {
    if (!this->m_buckets) {
        this->Rehash(this->m_bucketCount);
    }
    if (capacity < 8) {
        capacity = 8;
    }
    auto entries = static_cast<ENTRY*>(this->m_allocator->allocate(uint64_t(capacity) * sizeof(ENTRY), 0x10));
    auto next = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
    for (uint32_t i = 0; i < this->m_count; i++) {
        std::memcpy(entries[i].m_key, this->m_entries[i].m_key, 16);
        entries[i].m_reader = this->m_entries[i].m_reader;
        this->m_entries[i].m_reader = nullptr;
        next[i] = this->m_next[i];
    }
    this->m_allocator->deallocate(this->m_entries);
    this->m_allocator->deallocate(this->m_next);
    this->m_entries = entries;
    this->m_next = next;
    this->m_capacity = capacity;
}

// ref: FUN_1420e83e0
void ROOTMAP::Rehash(uint32_t buckets) {
    buckets = RoundUpPow2(buckets);
    auto table = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(buckets) * 4, 0x10));
    if (buckets != 0) {
        std::memset(table, 0xFF, uint64_t(buckets) * 4);
    }
    if (this->m_entries) {
        for (uint32_t i = 0; i < this->m_count; i++) {
            uint32_t slot = Hash(this->m_entries[i].m_key) & (buckets - 1);
            this->m_next[i] = table[slot];
            table[slot] = i;
        }
    }
    this->m_allocator->deallocate(this->m_buckets);
    this->m_bucketCount = buckets;
    this->m_buckets = table;
}

// The same body as the casc archive map's clear (one function in the reference).
// ref: FUN_1420e8790
void ROOTMAP::Clear() {
    if (this->m_buckets) {
        for (uint32_t i = 0; i < this->m_bucketCount; i++) {
            this->m_buckets[i] = 0xFFFFFFFF;
        }
    }
    for (uint32_t i = 0; i < this->m_count; i++) {
        if (this->m_entries[i].m_reader) {
            this->m_entries[i].m_reader->Release();
        }
    }
    this->m_count = 0;
}

bool KEYSET::Contains(const uint8_t key[16]) const {
    if (!this->m_buckets) {
        return false;
    }
    uint32_t slot = (this->m_bucketCount - 1) & blz::murmur3_32(key, 9, 0);
    for (uint32_t i = this->m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_next[i]) {
        if (std::memcmp(this->m_entries[i], key, 16) == 0) {
            return true;
        }
    }
    return false;
}

// ref: FUN_1420e2000
uint8_t* KEYSET::Append(uint32_t slot) {
    uint32_t index = this->m_count;
    if (index == this->m_capacity) {
        this->Reserve(this->m_capacity * 2);
    }
    this->m_next[index] = this->m_buckets[slot];
    this->m_buckets[slot] = index;
    this->m_count++;
    return this->m_entries[index];
}

// ref: FUN_1420e2170
void KEYSET::Reserve(uint32_t capacity) {
    if (!this->m_buckets) {
        this->Rehash(this->m_bucketCount);
    }
    if (capacity < 8) {
        capacity = 8;
    }
    auto entries = static_cast<uint8_t(*)[16]>(this->m_allocator->allocate(uint64_t(capacity) * 16, 0x10));
    auto next = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
    for (uint32_t i = 0; i < this->m_count; i++) {
        std::memcpy(entries[i], this->m_entries[i], 16);
        next[i] = this->m_next[i];
    }
    this->m_allocator->deallocate(this->m_entries);
    this->m_allocator->deallocate(this->m_next);
    this->m_entries = entries;
    this->m_next = next;
    this->m_capacity = capacity;
}

// ref: FUN_1420e2240 (rounding up, then FUN_1420e20a0)
void KEYSET::Rehash(uint32_t buckets) {
    buckets = RoundUpPow2(buckets);
    auto table = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(buckets) * 4, 0x10));
    if (buckets != 0) {
        std::memset(table, 0xFF, uint64_t(buckets) * 4);
    }
    if (this->m_entries) {
        for (uint32_t i = 0; i < this->m_count; i++) {
            uint32_t slot = blz::murmur3_32(this->m_entries[i], 9, 0) & (buckets - 1);
            this->m_next[i] = table[slot];
            table[slot] = i;
        }
    }
    this->m_allocator->deallocate(this->m_buckets);
    this->m_bucketCount = buckets;
    this->m_buckets = table;
}

// ref: FUN_1420d5fd0
void KEYSET::Destroy() {
    if (this->m_buckets) {
        for (uint32_t i = 0; i < this->m_bucketCount; i++) {
            this->m_buckets[i] = 0xFFFFFFFF;
        }
    }
    this->m_count = 0;
    this->m_allocator->deallocate(this->m_buckets);
    this->m_allocator->deallocate(this->m_entries);
    this->m_allocator->deallocate(this->m_next);
}

// --- FILEMANIFEST ----------------------------------------------------------------------------

// ref: FUN_1420e8610
void FILEMANIFEST::Init(Instance* instance) {
    this->m_instance = instance;
    this->m_current = nullptr;
    this->m_roots.m_allocator = GetAllocator();
    this->m_roots.m_buckets = nullptr;
    this->m_roots.m_entries = nullptr;
    this->m_roots.m_next = nullptr;
    this->m_roots.m_count = 0;
    this->m_roots.m_capacity = 0;
    this->m_roots.m_bucketCount = 0x10;
    this->m_vfsKeys.m_buckets = nullptr;
    this->m_vfsKeys.m_entries = nullptr;
    this->m_vfsKeys.m_next = nullptr;
    this->m_vfsKeys.m_allocator = GetAllocator();
    this->m_vfsKeys.m_count = 0;
    this->m_vfsKeys.m_capacity = 0;
    this->m_vfsKeys.m_bucketCount = 0x10;
    InitializeSRWLock(&this->m_lock);
}

// ref: FUN_1420e8680
void FILEMANIFEST::Destroy() {
    AcquireSRWLockExclusive(&this->m_lock);
    this->m_roots.Clear();
    if (this->m_vfsKeys.m_buckets) {
        for (uint32_t i = 0; i < this->m_vfsKeys.m_bucketCount; i++) {
            this->m_vfsKeys.m_buckets[i] = 0xFFFFFFFF;
        }
    }
    this->m_vfsKeys.m_count = 0;
    RootReader* current = this->m_current;
    if (current) {
        this->m_current = nullptr;
        current->Release();
    }
    ReleaseSRWLockExclusive(&this->m_lock);
    this->m_vfsKeys.Destroy();
    this->m_roots.Clear();
    this->m_roots.m_allocator->deallocate(this->m_roots.m_buckets);
    this->m_roots.m_allocator->deallocate(this->m_roots.m_entries);
    this->m_roots.m_allocator->deallocate(this->m_roots.m_next);
    if (this->m_current) {
        this->m_current->Release();
    }
}

// Parses the root into its reader, files the reader under the root's e-key as the current
// one, and notes the VFS sub-manifests' e-keys.
// ref: FUN_1420e9020
uint8_t FILEMANIFEST::Load(const CONFIGRECORD* root, const CONFIGRECORD* vfs, uint64_t vfsCount, const uint8_t* data,
    RootReader** reader) {
    FILEKEY key;
    root->CopyKeyTo(&key);
    if (!(*reader)->Parse(data, key.m_csize)) {
        return 3;
    }
    AcquireSRWLockExclusive(&this->m_lock);
    bool inserted;
    ROOTMAP::ENTRY* entry = this->m_roots.Insert(key.m_ekey, reader, &inserted);
    RootReader* previous = this->m_current;
    RootReader* current = entry->m_reader;
    if (previous != current) {
        this->m_current = current;
        if (current) {
            current->AddRef();
        }
        if (previous) {
            previous->Release();
        }
    }
    ReleaseSRWLockExclusive(&this->m_lock);
    uint8_t keySize = this->m_current->m_eKeySize;
    for (uint64_t i = 0; i < vfsCount; i++) {
        uint8_t truncated[16];
        std::memcpy(truncated, vfs[i].m_ekey, keySize);
        std::memset(truncated + keySize, 0, 0x10 - keySize);
        if (this->m_vfsKeys.Contains(truncated)) {
            continue;
        }
        uint8_t* added = this->m_vfsKeys.Append(blz::murmur3_32(truncated, 9, 0) & (this->m_vfsKeys.m_bucketCount - 1));
        if (added) {
            std::memcpy(added, truncated, 16);
        }
        if (this->m_vfsKeys.m_bucketCount < this->m_vfsKeys.m_count) {
            this->m_vfsKeys.Rehash(this->m_vfsKeys.m_bucketCount * 2);
        }
    }
    return 0;
}

// --- TVFS lookup -----------------------------------------------------------------------------

namespace {

// ref: FUN_142134570
bool IsSeparator(char c) {
    return c == '/' || c == ':';
}

uint32_t HexValue(char c) {
    uint32_t value = static_cast<uint32_t>(c - '0');
    if (value > 9) {
        if (static_cast<uint32_t>(c - 'a') < 6) {
            value = static_cast<uint32_t>(c - 0x57);
        } else if (static_cast<uint32_t>(c - 'A') < 6) {
            value = static_cast<uint32_t>(c - 0x37);
        } else {
            value = 0;
        }
    }
    return value;
}

}

// Matches a path against one name fragment; '#' toggles hex pairs (raw bytes), and with
// flag 8 the path is folded to lower case. Returns the bytes matched.
// ref: FUN_142111ef0
uint8_t RootReader::MatchFragment(const char** path, bool* hex, const uint8_t* fragment, uint8_t length) {
    uint32_t flags = this->m_flags;
    const char* p = *path;
    char c = *p;
    const uint8_t* at = fragment;
    for (; c != '\0' && at < fragment + length; at++) {
        c = *p;
        *path = p + 1;
        bool checkSeparator;
        if (c == '#') {
            *hex = !*hex;
            c = **path;
            (*path)++;
            if (c == '\0') {
                break;
            }
            checkSeparator = true;
        } else {
            checkSeparator = !*hex;
        }
        if (checkSeparator && IsSeparator(c)) {
            break;
        }
        if (!*hex) {
            if ((flags >> 3 & 1) != 0 && static_cast<uint8_t>(c - 'A') < 0x1a) {
                c += 0x20;
            }
        } else {
            char low = **path;
            if (static_cast<uint8_t>(low - '0') > 9 && static_cast<uint8_t>(low - 'a') > 5 &&
                static_cast<uint8_t>(low - 'A') > 5) {
                return 0;
            }
            (*path)++;
            c = static_cast<char>(HexValue(c) * 0x10 + HexValue(low));
        }
        if (static_cast<uint8_t>(c) != *at) {
            break;
        }
        p = *path;
        c = *p;
    }
    if (*hex && **path == '#') {
        (*path)++;
        *hex = false;
    }
    return static_cast<uint8_t>(at - fragment);
}

// Walks the path table: a node is name fragments (a length byte and the bytes), 0 as a path
// separator, then 0xff and a big-endian value - a VFS offset, or with the top bit a folder
// of that many bytes. Returns the value of the node the path names (or the node itself for a
// folder), leaving *path past what matched.
// ref: FUN_142111390
const uint8_t* RootReader::Walk(const char** path) {
    const uint8_t* node = this->m_data + this->m_pathTableOffset;
    const uint8_t* end = node + this->m_pathTableSize;
    const char* cur = *path;
    if (!cur || *cur == '\0') {
        return node;
    }
    bool hex = false;
    for (;;) {
        if (end <= node) {
            return nullptr;
        }
        uint8_t b = *node;
        const uint8_t* next = node + 1;
        bool skipValue = false;
        if (b == 0xff) {
            b = *next;
            next = node + 5;
            if (static_cast<int8_t>(b) < 0) {
                end = next + ((ReadBE32(node + 1) & 0x7FFFFFFF) - 4);
            }
        } else if (b == 0) {
            if (*cur == '/') {
                *path = cur + 1;
            } else {
                b = *next;
                while (b != 0xff) {
                    next += uint64_t(b) + 1;
                    b = *next;
                }
                skipValue = true;
            }
        } else {
            bool matchHex = hex;
            uint8_t matched = this->MatchFragment(&cur, &matchHex, next, b);
            next += b;
            if (matched < b) {
                b = *next;
                while (b != 0xff) {
                    next += uint64_t(b) + 1;
                    b = *next;
                }
                skipValue = true;
            } else {
                *path = cur;
                char c = *cur;
                if (c == '\0' || c == ':') {
                    if (*next != 0xff) {
                        return nullptr;
                    }
                    if (static_cast<int8_t>(next[1]) >= 0) {
                        return next + 1;
                    }
                    return static_cast<uint8_t>(next[5] + 1) != 0 ? nullptr : next + 6;
                }
                hex = matchHex;
                if (c == '/') {
                    b = *next;
                    if (cur[1] == '\0') {
                        if (b != 0xff) {
                            return b == 0 ? next + 1 : next;
                        }
                        if (static_cast<int8_t>(next[1]) >= 0) {
                            return nullptr;
                        }
                        return next[5] != 0 ? nullptr : next + 6;
                    }
                    if (b == 0) {
                        *path = cur + 1;
                        next++;
                    } else if (b != 0xff) {
                        *path = cur + 1;
                    }
                } else if (*next != 0xff) {
                    return nullptr;
                }
            }
        }
        if (skipValue) {
            uint8_t first = next[1];
            uint64_t size = 4;
            if (static_cast<int8_t>(first) < 0) {
                size = ReadBE32(next + 1) & 0x7FFFFFFF;
            }
            next = next + 1 + size;
        }
        cur = *path;
        node = next;
        if (*cur == '\0') {
            return nullptr;
        }
    }
}

// A container file table entry: e-key, e-size, e-spec offset, c-size, and with the flags the
// c-key and a patch entry.
// ref: FUN_1421120a0
bool RootReader::ResolveCft(uint32_t offset, FILEKEY* key) {
    uint32_t flags = this->m_flags;
    const uint8_t* entry = this->m_data + this->m_cftTableOffset + offset;
    key->m_eSpecFlags = this->m_eKeySize;
    std::memcpy(key->m_ekey, entry, this->m_eKeySize);
    std::memset(key->m_ekey + this->m_eKeySize, 0, 0x10 - this->m_eKeySize);
    const uint8_t* p = entry + this->m_eKeySize;
    key->m_esize = ReadBE32(p);
    p += 4;
    const char* eSpec = nullptr;
    if ((flags >> 1 & 1) != 0) {
        uint64_t position;
        switch (this->m_eSpecOffsetSize) {
        case 1:
            position = p[0];
            break;
        case 2:
            position = uint32_t(p[0]) << 8 | p[1];
            break;
        case 3:
            position = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
            break;
        case 4:
            position = ReadBE32(p);
            break;
        default:
            position = 0xFFFFFFFF;
            break;
        }
        eSpec = reinterpret_cast<const char*>(this->m_data + this->m_eSpecTableOffset + position);
    }
    key->m_eSpec = eSpec;
    p += this->m_eSpecOffsetSize;
    key->m_csize = ReadBE32(p);
    p += 4;
    if ((flags & 1) != 0) {
        std::memcpy(key->m_ckey, p, 16);
        p += 16;
    } else {
        std::memset(key->m_ckey, 0, 16);  // DAT_1428ecaf0
    }
    if ((flags >> 2 & 1) != 0) {
        key->m_31 = *p;
        key->m_38 = reinterpret_cast<uint64_t>(p);
    } else {
        key->m_31 = 0;
        key->m_38 = 0;
    }
    return true;
}

// A VFS entry: one span (count 1) whose container file table offset is resolved.
// ref: FUN_1421118b0
bool RootReader::ResolveVfs(uint32_t offset, FILEKEY* key) {
    if (static_cast<int32_t>(offset) < 0) {
        return false;
    }
    const uint8_t* entry = this->m_data + this->m_vfsTableOffset + offset;
    if (entry[0] != 1) {
        blz::log(g_logCategory, 4, s_readerFile, 0x2f2, "Encountered unsupported file type in file manifest: %u", entry[0]);
        return false;
    }
    uint32_t cft = 0xFFFFFFFF;
    switch (this->m_cftOffsetSize) {
    case 1:
        cft = entry[9];
        break;
    case 2:
        cft = uint32_t(entry[9]) << 8 | entry[10];
        break;
    case 3:
        cft = uint32_t(entry[9]) << 16 | uint32_t(entry[10]) << 8 | entry[11];
        break;
    case 4:
        cft = ReadBE32(entry + 9);
        break;
    default:
        break;
    }
    return this->ResolveCft(cft, key);
}

// ref: FUN_1421119d0
bool RootReader::Lookup(const char** path, FILEKEY* key) {
    const uint8_t* node = this->Walk(path);
    if (!node || **path == '/') {
        return false;
    }
    return this->ResolveVfs(ReadBE32(node), key);
}

// --- FILEMANIFEST lookups --------------------------------------------------------------------

// ref: FUN_1420e6b00
ROOTMAP::ENTRY* FILEMANIFEST::FindRoot(const uint8_t key[16]) {
    uint32_t hash = blz::murmur3_32(key, 9, 0);
    if (!this->m_roots.m_buckets) {
        return nullptr;
    }
    for (uint32_t i = this->m_roots.m_buckets[(this->m_roots.m_bucketCount - 1) & hash]; i != 0xFFFFFFFF;
         i = this->m_roots.m_next[i]) {
        if (std::memcmp(this->m_roots.m_entries[i].m_key, key, 16) == 0) {
            return &this->m_roots.m_entries[i];
        }
    }
    return nullptr;
}

// ref: FUN_1420e9200
uint8_t FILEMANIFEST::RegisterSubRoot(const FILEKEY* key, const uint8_t* data, RootReader** reader, RootReader** out) {
    if (!(*reader)->Parse(data, key->m_csize)) {
        return 3;
    }
    AcquireSRWLockExclusive(&this->m_lock);
    bool inserted;
    ROOTMAP::ENTRY* entry = this->m_roots.Insert(key->m_ekey, reader, &inserted);
    RootReader* found = entry->m_reader;
    RootReader* previous = *out;
    if (previous != found) {
        *out = found;
        if (found) {
            found->AddRef();
        }
        if (previous) {
            previous->Release();
        }
    }
    ReleaseSRWLockExclusive(&this->m_lock);
    return 0;
}

// A VFS sub-manifest named by a path: read right away, or through a file read when it is not
// all local, then parsed and kept.
// ref: FUN_1420e92c0
uint8_t FILEMANIFEST::LoadSubRoot(const FILEKEY* key, RootReader** out) {
    uint8_t* data = nullptr;
    RootReader* reader = nullptr;
    RootReaderCreate(&reader, key->m_csize, &data);
    uint32_t read = 0;
    uint8_t result = ImmediateReadByKey(this->m_instance, key, data, 0, key->m_csize, &read, 0);
    if (result != 0 || read != key->m_csize) {
        OPHANDLE handle;
        CreateFileRead(this->m_instance, &handle, nullptr, nullptr);
        AsyncState* state = handle.m_state;
        FileReadStart(state, const_cast<FILEKEY*>(key), data, 0, key->m_csize, 2, 0);
        uint64_t opId = state->m_opId;
        if (handle.m_lock) {
            *handle.m_lock = 0;
        }
        state->Release();
        result = this->m_instance->m_ops.WaitResult(opId, nullptr);
        if (result != 0) {
            char text[0x21];
            KeyToString(text, sizeof(text), key->m_ekey);
            blz::log(g_logCategory, 4,
                "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\file_manifest\\file_manifest_cache.cpp",
                0x19c, "Failed to read file manifest: %s. Error (%u): %s", text, result, ErrorString(result));
            if (reader) {
                reader->Release();
            }
            return result;
        }
    }
    result = this->RegisterSubRoot(key, data, &reader, out);
    if (reader) {
        reader->Release();
    }
    return result;
}

// A path, ':' separating the VFS manifests it passes through.
// ref: FUN_1420e8c70
uint8_t FILEMANIFEST::LookupPath(const char* path, FILEKEY* key, RootReader** reader, uint8_t loadMissing) {
    RootReader* current = this->m_current;
    if (*reader != current) {
        RootReader* previous = *reader;
        *reader = current;
        if (current) {
            current->AddRef();
        }
        if (previous) {
            previous->Release();
        }
    }
    const char* cur = path;
    if (!(*reader)->Lookup(&cur, key)) {
        return 6;
    }
    while (*cur == ':') {
        cur++;
        AcquireSRWLockShared(&this->m_lock);
        ROOTMAP::ENTRY* entry = this->FindRoot(key->m_ekey);
        RootReader* sub = nullptr;
        if (entry) {
            sub = entry->m_reader;
            if (sub) {
                sub->AddRef();
            }
        }
        ReleaseSRWLockShared(&this->m_lock);
        if (!sub) {
            if (!loadMissing) {
                return 6;
            }
            uint8_t result = this->LoadSubRoot(key, &sub);
            if (result != 0) {
                if (sub) {
                    sub->Release();
                }
                return result;
            }
        }
        RootReader* previous = *reader;
        if (previous != sub) {
            *reader = sub;
            if (sub) {
                sub->AddRef();
            }
            if (previous) {
                previous->Release();
            }
        }
        bool found = (*reader)->Lookup(&cur, key);
        if (sub) {
            sub->Release();
        }
        if (!found) {
            return 6;
        }
    }
    return 0;
}

// ".tact:" names: the build config's own files.
// ref: FUN_1420e8ec0
uint8_t FILEMANIFEST::LookupSpecial(const char* name, FILEKEY* key) {
    BUILDCONFIG& config = this->m_instance->m_buildConfig;
    const CONFIGRECORD* record = nullptr;
    if (std::strcmp(name, "download_manifest") == 0) {
        record = &config.m_records[0];
    } else if (std::strcmp(name, "encoding_table") == 0) {
        record = &config.m_records[3];
    } else if (std::strcmp(name, "install_manifest") == 0) {
        record = &config.m_records[1];
    } else if (std::strcmp(name, "size_manifest") == 0) {
        record = &config.m_records[2];
    }
    if (record) {
        record->CopyKeyTo(key);
    }
    static const uint8_t s_zero[16] = {};  // DAT_1428e7498
    return std::memcmp(key->m_ckey, s_zero, 16) == 0 ? 6 : 0;
}

// The file a path names; an empty path is the root itself. Without an e-spec in the
// manifest, the encoding table supplies it.
// ref: FUN_1420e8820
uint8_t FILEMANIFEST::Lookup(const char* path, FILEKEY* key, uint8_t loadMissing) {
    if (!path || *path == '\0') {
        this->m_instance->m_buildConfig.m_records[4].CopyKeyTo(key);
        return 0;
    }
    if (std::strncmp(path, ".tact:", 6) == 0) {
        return this->LookupSpecial(path + 6, key);
    }
    RootReader* reader = nullptr;
    uint8_t result = this->LookupPath(path, key, &reader, loadMissing);
    if (result == 0) {
        if ((!key->m_eSpec || *key->m_eSpec == '\0') && this->m_instance->m_encodingTable.m_valid) {
            uint8_t keySize = this->m_current->m_eKeySize;
            if (key->m_eSpecFlags < keySize) {
                keySize = key->m_eSpecFlags;
            }
            this->m_instance->m_encodingTable.FindESpec(key->m_ekey, keySize, &key->m_eSpec);
        }
        result = 0;
    }
    if (reader) {
        reader->Release();
    }
    return result;
}

}
