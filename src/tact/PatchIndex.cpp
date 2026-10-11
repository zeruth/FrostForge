#include "tact/PatchIndex.hpp"
#include "blz/Murmur.hpp"
#include "tact/Container.hpp"
#include "tact/Instance.hpp"
#include <cstring>

namespace tact {

namespace {

uint32_t ReadU32(PATCHCURSOR* cursor) {
    uint32_t value;
    std::memcpy(&value, cursor->m_cur, 4);
    cursor->m_cur += 4;
    return value;
}

uint16_t ReadU16(PATCHCURSOR* cursor) {
    uint16_t value;
    std::memcpy(&value, cursor->m_cur, 2);
    cursor->m_cur += 2;
    return value;
}

uint8_t ReadU8(PATCHCURSOR* cursor) {
    return *cursor->m_cur++;
}

// A key of `size` bytes into 16, zero-padded.
void ReadKey(PATCHCURSOR* cursor, uint8_t* out, uint8_t size) {
    std::memcpy(out, cursor->m_cur, size);
    if (size < 0x10) {
        std::memset(out + size, 0, 0x10 - size);
    }
    cursor->m_cur += size;
}

// A little-endian offset of `size` bytes.
uint32_t ReadOffset(PATCHCURSOR* cursor, uint32_t size) {
    uint32_t value = 0;
    std::memcpy(&value, cursor->m_cur, size);
    cursor->m_cur += size;
    return value;
}

// The bytes an offset into a string of `length` bytes takes.
uint32_t OffsetBytes(uint32_t length) {
    uint32_t count = 0;
    if (length != 0) {
        uint8_t bits = 0;
        do {
            bits += 8;
            count++;
        } while ((uint64_t(1) << (bits & 0x3F)) <= length);
    }
    return count;
}

// ref: FUN_1420eafd0
void ReadString(PATCHCURSOR* cursor, blz::alloc_string* string, uint32_t length) {
    string->Resize(length);
    std::memcpy(string->data(), cursor->m_cur, length);
    cursor->m_cur += length;
}

// Whether a record goes in: always without an instance, else when the container lacks the
// target and has the source (or the filter vouches for the source and the container has it).
bool Wanted(PATCHINDEX* index, const uint8_t* target, const uint8_t* source, const char* name, const void* record,
    bool filtered) {
    if (!index->m_instance) {
        return true;
    }
    Container* container = index->m_instance->m_container;
    if (container->Contains(target)) {
        return false;
    }
    if (container->Contains(source)) {
        return true;
    }
    if (!filtered || !index->m_filter) {
        return false;
    }
    return index->m_filter(index->m_filterContext, index->m_instance, name, source, record) &&
        index->m_instance->m_container->Contains(source);
}

}

// --- PATCHMAP --------------------------------------------------------------------------------

template <uint32_t SIZE>
void PATCHMAP<SIZE>::Init(blz::allocator* allocator) {
    this->m_allocator = allocator;
    this->m_buckets = nullptr;
    this->m_entries = nullptr;
    this->m_next = nullptr;
    this->m_count = 0;
    this->m_capacity = 0;
    this->m_bucketCount = 0x10;
}

template <uint32_t SIZE>
typename PATCHMAP<SIZE>::ENTRY* PATCHMAP<SIZE>::Find(const uint8_t key[16], uint32_t slot) {
    if (!this->m_buckets) {
        return nullptr;
    }
    for (uint32_t i = this->m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_next[i]) {
        if (std::memcmp(this->m_entries[i].m_bytes, key, 16) == 0) {
            return &this->m_entries[i];
        }
    }
    return nullptr;
}

// ref: FUN_1420eb330 (0x30), FUN_1420eb1b0 (0x50), FUN_1420eb030 (0x58)
template <uint32_t SIZE>
typename PATCHMAP<SIZE>::ENTRY* PATCHMAP<SIZE>::Append(uint32_t slot) {
    uint32_t index = this->m_count;
    if (index == this->m_capacity) {
        uint32_t capacity = this->m_capacity * 2;
        if (!this->m_buckets) {
            this->Rehash(this->m_bucketCount);
        }
        if (capacity < 8) {
            capacity = 8;
        }
        auto entries = static_cast<ENTRY*>(this->m_allocator->allocate(uint64_t(capacity) * SIZE, 0x10));
        auto next = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
        for (uint32_t i = 0; i < this->m_count; i++) {
            entries[i] = this->m_entries[i];
            next[i] = this->m_next[i];
        }
        this->m_allocator->deallocate(this->m_entries);
        this->m_allocator->deallocate(this->m_next);
        this->m_entries = entries;
        this->m_next = next;
        this->m_capacity = capacity;
    }
    this->m_next[index] = this->m_buckets[slot];
    this->m_buckets[slot] = index;
    this->m_count++;
    return &this->m_entries[index];
}

// ref: FUN_1420eb670 (0x30), FUN_1420eb580 (0x50), FUN_1420eb490 (0x58)
template <uint32_t SIZE>
void PATCHMAP<SIZE>::Rehash(uint32_t buckets) {
    if ((buckets & (buckets - 1)) != 0) {
        int32_t bit = 31;
        while (((buckets | 1) >> bit) == 0) {
            bit--;
        }
        buckets = 1u << ((bit + 1) & 31);
    }
    auto table = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(buckets) * 4, 0x10));
    if (buckets != 0) {
        std::memset(table, 0xFF, uint64_t(buckets) * 4);
    }
    if (this->m_entries) {
        for (uint32_t i = 0; i < this->m_count; i++) {
            uint32_t slot = blz::murmur3_32(this->m_entries[i].m_bytes, 9, 0) & (buckets - 1);
            this->m_next[i] = table[slot];
            table[slot] = i;
        }
    }
    this->m_allocator->deallocate(this->m_buckets);
    this->m_bucketCount = buckets;
    this->m_buckets = table;
}

// ref: FUN_1420d5fd0
template <uint32_t SIZE>
void PATCHMAP<SIZE>::Destroy() {
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

template struct PATCHMAP<0x30>;
template struct PATCHMAP<0x50>;
template struct PATCHMAP<0x58>;

namespace {

template <uint32_t SIZE>
// ref: FUN_1420eb490
// ref: FUN_1420eb580
// ref: FUN_1421159e0
void Insert(PATCHMAP<SIZE>* map, const uint8_t* record) {
    uint32_t slot = blz::murmur3_32(record, 9, 0) & (map->m_bucketCount - 1);
    if (map->Find(record, slot)) {
        return;
    }
    auto entry = map->Append(slot);
    if (entry) {
        std::memcpy(entry->m_bytes, record, SIZE);
    }
    if (map->m_bucketCount < map->m_count) {
        map->Rehash(map->m_bucketCount * 2);
    }
}

}

// --- PATCHINDEX ------------------------------------------------------------------------------

// ref: FUN_1420e97a0
void PATCHINDEX::Init() {
    this->m_cursor = nullptr;
    this->m_instance = nullptr;
    this->m_filter = nullptr;
    this->m_filterContext = nullptr;
    this->m_headerSize = 0;
    this->m_version = 1;
    this->m_28 = 0;
    std::memset(this->m_key, 0, sizeof(this->m_key));
    this->m_extra.Init(GetAllocator());
    this->m_names.Init(GetAllocator());
    this->m_fileNames.Init(GetAllocator());
    this->m_paths.Init(GetAllocator());
    this->m_blocks.Init(GetAllocator());
    this->m_patchKeySize = 0;
    this->m_entryKeySize = 0;
    this->m_fileKeySize = 0;
    this->m_patches.Init(GetAllocator());
    this->m_entries.Init(GetAllocator());
    this->m_files.Init(GetAllocator());
}

// ref: FUN_1420e9910
void PATCHINDEX::Destroy() {
    this->m_files.Destroy();
    this->m_entries.Destroy();
    this->m_patches.Destroy();
    if (this->m_blocks.m_data) {
        this->m_blocks.m_allocator->deallocate(this->m_blocks.m_data);
    }
    this->m_paths.Destroy();
    this->m_fileNames.Destroy();
    this->m_names.Destroy();
    this->m_extra.Destroy();
}

// The header: its size, version and a word, the extra block (key and text), then the block
// list.
// ref: FUN_1420eaac0
void PATCHINDEX::ReadHeader() {
    PATCHCURSOR* cursor = this->m_cursor;
    this->m_headerSize = ReadU32(cursor);
    this->m_version = ReadU32(cursor);
    this->m_28 = ReadU32(cursor);
    uint16_t extra = ReadU16(cursor);
    if (extra != 0) {
        uint8_t keySize = ReadU8(cursor);
        std::memcpy(this->m_key, cursor->m_cur, keySize);
        if (keySize < 0x10) {
            std::memset(this->m_key + keySize, 0, 0x10 - keySize);
        }
        cursor->m_cur += keySize;
        uint16_t rest = static_cast<uint16_t>(extra - keySize - 1);
        if (rest != 0) {
            ReadString(cursor, &this->m_extra, rest);
        }
    }
    uint32_t count = ReadU32(cursor);
    if (this->m_blocks.m_count <= count) {
        if (this->m_blocks.m_capacity < count) {
            this->m_blocks.Reserve(count);
        }
        for (uint32_t i = this->m_blocks.m_count; i < count; i++) {
            this->m_blocks.m_data[i] = 0;
        }
    }
    this->m_blocks.m_count = count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = ReadU32(cursor);
        uint32_t size = ReadU32(cursor);
        this->m_blocks.m_data[i] = uint64_t(size) << 32 | type;
    }
}

// Block 2: {source key, size, key, size, size, name offset, target key}.
// ref: FUN_1420ea720
void PATCHINDEX::ReadEntries(uint32_t offset) {
    PATCHCURSOR* cursor = this->m_cursor;
    cursor->m_offset = offset;
    uint32_t count = ReadU32(cursor);
    uint8_t keySize = ReadU8(cursor);
    uint32_t nameBytes = OffsetBytes(this->m_names.size());
    for (uint32_t i = 0; i < count; i++) {
        uint8_t record[0x50] = {};
        ReadKey(cursor, record + 0x10, keySize);
        uint32_t a = ReadU32(cursor);
        std::memcpy(record + 0x20, &a, 4);
        ReadKey(cursor, record + 0x24, keySize);
        uint32_t b = ReadU32(cursor);
        std::memcpy(record + 0x34, &b, 4);
        uint32_t c = ReadU32(cursor);
        std::memcpy(record + 0x38, &c, 4);
        uint32_t name = ReadOffset(cursor, nameBytes);
        ReadKey(cursor, record, keySize);
        const char* namePointer = this->m_names.c_str() + name;
        std::memcpy(record + 0x40, &namePointer, 8);
        uint32_t slot = blz::murmur3_32(record, 9, 0) & (this->m_entries.m_bucketCount - 1);
        if (this->m_entries.Find(record, slot)) {
            continue;
        }
        if (!Wanted(this, record, record + 0x10, nullptr, nullptr, false)) {
            continue;
        }
        record[0x48] = keySize;
        record[0x49] = keySize;
        Insert(&this->m_entries, record);
    }
    this->m_entryKeySize = keySize;
}

// Block 3: {source key, size, target key, name offset}.
// ref: FUN_1420eac60
void PATCHINDEX::ReadPatches(uint32_t offset) {
    PATCHCURSOR* cursor = this->m_cursor;
    cursor->m_offset = offset;
    uint32_t count = ReadU32(cursor);
    uint8_t keySize = ReadU8(cursor);
    uint32_t nameBytes = OffsetBytes(this->m_names.size());
    for (uint32_t i = 0; i < count; i++) {
        uint8_t record[0x30] = {};
        ReadKey(cursor, record + 0x10, keySize);
        uint32_t size = ReadU32(cursor);
        std::memcpy(record + 0x20, &size, 4);
        ReadKey(cursor, record, keySize);
        uint32_t name = ReadOffset(cursor, nameBytes);
        const char* namePointer = this->m_names.c_str() + name;
        std::memcpy(record + 0x28, &namePointer, 8);
        uint32_t slot = blz::murmur3_32(record, 9, 0) & (this->m_patches.m_bucketCount - 1);
        if (this->m_patches.Find(record, slot)) {
            continue;
        }
        if (!Wanted(this, record, record + 0x10, nullptr, nullptr, false)) {
            continue;
        }
        Insert(&this->m_patches, record);
    }
    this->m_patchKeySize = keySize;
}

// Block 5: file patches with their paths after the records.
// ref: FUN_1420e9de0
void PATCHINDEX::ReadPaths(uint32_t offset) {
    PATCHCURSOR* cursor = this->m_cursor;
    cursor->m_offset = offset;
    cursor->m_cur += 4;
    uint32_t count = ReadU32(cursor);
    uint8_t keySize = ReadU8(cursor);
    uint32_t pathsSize = ReadU32(cursor);
    this->m_paths.Resize(pathsSize);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t record[0x58] = {};
        ReadKey(cursor, record, keySize);
        cursor->m_cur += 4;
        uint32_t a = ReadU32(cursor);
        std::memcpy(record + 0x50, &a, 4);
        ReadKey(cursor, record + 0x10, keySize);
        uint32_t b = ReadU32(cursor);
        std::memcpy(record + 0x28, &b, 4);
        const char* path = nullptr;
        if (this->m_paths.size() != 0) {
            uint32_t position = ReadU32(cursor);
            if (position != 0xFFFFFFFF) {
                path = this->m_paths.c_str() + position;
            }
        }
        std::memcpy(record + 0x20, &path, 8);
        uint32_t slot = blz::murmur3_32(record, 9, 0) & (this->m_files.m_bucketCount - 1);
        if (this->m_files.Find(record, slot)) {
            continue;
        }
        if (!Wanted(this, record, record + 0x10, path, record + 0x28, true)) {
            continue;
        }
        std::memset(record + 0x20, 0, 8);
        record[0x54] = keySize;
        Insert(&this->m_files, record);
    }
    if (this->m_fileKeySize == 0) {
        this->m_fileKeySize = keySize;
    }
    uint32_t length = this->m_paths.size();
    std::memcpy(this->m_paths.data(), cursor->m_cur, length);
    cursor->m_cur += length;
}

// Block 6: file patches with their file names first.
// ref: FUN_1420ea1d0
void PATCHINDEX::ReadFiles(uint32_t offset) {
    PATCHCURSOR* cursor = this->m_cursor;
    cursor->m_offset = offset;
    uint8_t header[16];
    std::memcpy(header, cursor->m_cur, 16);
    cursor->m_cur += 16;
    uint8_t keySize = header[1];
    uint16_t skip;
    std::memcpy(&skip, header + 2, 2);
    cursor->m_offset = skip + offset;
    uint32_t count;
    std::memcpy(&count, header + 4, 4);
    uint32_t namesSize;
    std::memcpy(&namesSize, header + 12, 4);
    if (namesSize != 0) {
        ReadString(cursor, &this->m_fileNames, namesSize);
    }
    uint32_t nameBytes = OffsetBytes(this->m_names.size());
    uint32_t fileNameBytes = OffsetBytes(this->m_fileNames.size());
    for (uint32_t i = 0; i < count; i++) {
        uint8_t record[0x58] = {};
        ReadKey(cursor, record + 0x10, keySize);
        const char* fileName = nullptr;
        if (fileNameBytes != 0) {
            uint32_t position = ReadOffset(cursor, fileNameBytes);
            if (position != 0) {
                fileName = this->m_fileNames.c_str() + position;
            }
        }
        std::memcpy(record + 0x20, &fileName, 8);
        uint32_t a = ReadU32(cursor);
        std::memcpy(record + 0x28, &a, 4);
        ReadKey(cursor, record + 0x2c, keySize);
        uint32_t b = ReadU32(cursor);
        std::memcpy(record + 0x3c, &b, 4);
        uint32_t c = ReadU32(cursor);
        std::memcpy(record + 0x40, &c, 4);
        uint32_t name = ReadOffset(cursor, nameBytes);
        if (name != 0) {
            const char* namePointer = this->m_names.c_str() + name;
            std::memcpy(record + 0x48, &namePointer, 8);
        }
        ReadKey(cursor, record, keySize);
        uint32_t d = ReadU32(cursor);
        std::memcpy(record + 0x50, &d, 4);
        uint32_t slot = blz::murmur3_32(record, 9, 0) & (this->m_files.m_bucketCount - 1);
        if (this->m_files.Find(record, slot)) {
            continue;
        }
        if (!Wanted(this, record, record + 0x10, fileName, record + 0x28, true)) {
            continue;
        }
        record[0x54] = keySize;
        std::memset(record + 0x20, 0, 8);
        Insert(&this->m_files, record);
    }
    if (this->m_fileKeySize == 0) {
        this->m_fileKeySize = keySize;
    }
}

// The name strings (block 1) first, then the record blocks in order.
// ref: FUN_1420e9c70
uint8_t PATCHINDEX::Load(PATCHINDEXSOURCE* source) {
    PATCHCURSOR cursor;
    cursor.m_begin = source->m_data;
    cursor.m_cur = source->m_data;
    cursor.m_offset = 0;
    cursor.m_size = source->m_size;
    this->m_cursor = &cursor;
    this->m_instance = source->m_instance;
    this->m_filter = reinterpret_cast<PATCHFILTER>(source->m_260);
    this->m_filterContext = reinterpret_cast<void*>(source->m_268);
    this->ReadHeader();
    uint32_t offset = this->m_headerSize;
    for (uint32_t i = 0; i < this->m_blocks.m_count; i++) {
        uint32_t type = static_cast<uint32_t>(this->m_blocks.m_data[i]);
        if (type == 1) {
            cursor.m_offset = offset;
            uint32_t length = ReadU32(&cursor);
            ReadString(&cursor, &this->m_names, length);
        }
        offset += static_cast<uint32_t>(this->m_blocks.m_data[i] >> 32);
    }
    offset = this->m_headerSize;
    for (uint32_t i = 0; i < this->m_blocks.m_count; i++) {
        uint32_t type = static_cast<uint32_t>(this->m_blocks.m_data[i]);
        if (type == 2) {
            this->ReadEntries(offset);
        } else if (type == 3) {
            this->ReadPatches(offset);
        } else if (type == 5) {
            this->ReadPaths(offset);
        } else if (type == 6) {
            this->ReadFiles(offset);
        }
        offset += static_cast<uint32_t>(this->m_blocks.m_data[i] >> 32);
    }
    this->m_cursor = nullptr;
    return 0;
}

}
