#include "tact/U32Set.hpp"
#include "tact/Common.hpp"
#include <cstring>

namespace tact {

uint32_t U32SET::Hash(uint32_t value) {
    uint32_t h = (value >> 16 ^ value) * 0x85EBCA6Bu;
    h = (h >> 13 ^ h) * 0xC2B2AE35u;
    return h >> 16 ^ h;
}

// ref: FUN_1420f9bd0
void U32SET::Init() {
    this->m_allocator = GetBufferAllocator();
    this->m_buckets = nullptr;
    this->m_entries = nullptr;
    this->m_next = nullptr;
    this->m_count = 0;
    this->m_capacity = 0;
    this->m_bucketCount = 0x10;
}

// ref: FUN_1420d5fd0
void U32SET::Destroy() {
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

// Fresh storage for a copy: an empty bucket table and room for `capacity` entries.
// ref: FUN_1420dd320
void U32SET::Allocate(uint32_t buckets, uint32_t capacity) {
    if (buckets != 0) {
        this->m_buckets = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(buckets) * 4, 0x10));
        std::memset(this->m_buckets, 0xFF, uint64_t(buckets) * 4);
    }
    if (capacity != 0) {
        this->m_entries = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
        this->m_next = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
    }
    this->m_bucketCount = buckets;
    this->m_capacity = capacity;
    this->m_count = 0;
}

// Takes the source's storage when both share an allocator, otherwise copies it.
// ref: FUN_1420dd430
void U32SET::Assign(U32SET* source) {
    if (this->m_allocator == source->m_allocator) {
        this->m_buckets = source->m_buckets;
        source->m_buckets = nullptr;
        this->m_entries = source->m_entries;
        source->m_entries = nullptr;
        this->m_next = source->m_next;
        source->m_next = nullptr;
        this->m_bucketCount = source->m_bucketCount;
        this->m_count = source->m_count;
        source->m_count = 0;
        this->m_capacity = source->m_capacity;
        source->m_capacity = 0;
        return;
    }
    if (!source->m_buckets) {
        this->m_bucketCount = 0;
        this->m_count = 0;
        this->m_capacity = 0;
        this->m_bucketCount = source->m_bucketCount;
        return;
    }
    this->Allocate(source->m_bucketCount, source->m_capacity);
    for (uint32_t i = 0; i < source->m_count; i++) {
        uint32_t value = source->m_entries[i];
        if (this->Contains(value)) {
            continue;
        }
        uint32_t* entry = this->Append(Hash(value) & (this->m_bucketCount - 1));
        if (entry) {
            *entry = source->m_entries[i];
        }
        if (this->m_bucketCount < this->m_count) {
            this->Rehash(this->m_bucketCount * 2);
        }
    }
}

bool U32SET::Contains(uint32_t value) const {
    if (!this->m_buckets) {
        return false;
    }
    for (uint32_t i = this->m_buckets[Hash(value) & (this->m_bucketCount - 1)]; i != 0xFFFFFFFF; i = this->m_next[i]) {
        if (this->m_entries[i] == value) {
            return true;
        }
    }
    return false;
}

// ref: FUN_1420dd590
uint32_t* U32SET::Append(uint32_t slot) {
    uint32_t index = this->m_count;
    if (index == this->m_capacity) {
        uint32_t capacity = this->m_capacity * 2;
        if (!this->m_buckets) {
            this->Rehash(this->m_bucketCount);
        }
        if (capacity < 8) {
            capacity = 8;
        }
        auto entries = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
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

// ref: FUN_1420dd6d0
void U32SET::Rehash(uint32_t buckets) {
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
            uint32_t slot = Hash(this->m_entries[i]) & (buckets - 1);
            this->m_next[i] = table[slot];
            table[slot] = i;
        }
    }
    this->m_allocator->deallocate(this->m_buckets);
    this->m_bucketCount = buckets;
    this->m_buckets = table;
}

// ref: FUN_1420f9c10
bool U32SET::Insert(uint32_t value, uint32_t** entry) {
    uint32_t slot = Hash(value) & (this->m_bucketCount - 1);
    if (this->m_buckets) {
        for (uint32_t i = this->m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_next[i]) {
            if (this->m_entries[i] == value) {
                if (entry) {
                    *entry = &this->m_entries[i];
                }
                return false;
            }
        }
    }
    uint32_t* added = this->Append(slot);
    if (added) {
        *added = value;
    }
    if (this->m_bucketCount < this->m_count) {
        this->Rehash(this->m_bucketCount * 2);
    }
    if (entry) {
        *entry = added;
    }
    return true;
}

}
