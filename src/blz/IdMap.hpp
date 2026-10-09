#ifndef BLZ_ID_MAP_HPP
#define BLZ_ID_MAP_HPP

// blz's chained hash map from 64-bit ids to a plain value (0x2c bytes): a power-of-two bucket
// table of entry indices, entries packed in insertion order with a parallel `next` chain, and
// removal by moving the last entry into the hole. The hash is a 64-bit finalizer (constants
// 0xff51afd7ed558ccd, 0xc4ceb9fe1a85ec53) folded to 32 bits. tact's op registry uses the same
// layout with reference-counted values (tact::OPMAP); aio and casc use this one.

#include "blz/Allocator.hpp"
#include <cstdint>
#include <cstring>

namespace blz {

inline uint32_t id_hash(uint64_t id) {
    uint64_t h = (id >> 33 ^ id) * 0xFF51AFD7ED558CCDull;
    h = (h >> 33 ^ h) * 0xC4CEB9FE1A85EC53ull;
    return static_cast<uint32_t>(h >> 33) ^ static_cast<uint32_t>(h);
}

template <class V>
struct id_map {
    struct entry {
        uint64_t m_key;
        V m_value;
    };

    uint32_t* m_buckets;        // 0x00
    entry* m_entries;           // 0x08
    uint32_t* m_next;           // 0x10
    allocator* m_allocator;     // 0x18
    uint32_t m_bucketCount;     // 0x20
    uint32_t m_count;           // 0x24
    uint32_t m_capacity;        // 0x28

    void init(allocator* allocator) {
        this->m_buckets = nullptr;
        this->m_entries = nullptr;
        this->m_next = nullptr;
        this->m_allocator = allocator;
        this->m_bucketCount = 0x10;
        this->m_count = 0;
        this->m_capacity = 0;
    }

    // The same code as tact::OPMAP::Rehash (FUN_1420d4260).
    void rehash(uint32_t buckets) {
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
                uint32_t slot = id_hash(this->m_entries[i].m_key) & (buckets - 1);
                this->m_next[i] = table[slot];
                table[slot] = i;
            }
        }
        this->m_allocator->deallocate(this->m_buckets);
        this->m_bucketCount = buckets;
        this->m_buckets = table;
    }

    // Appends an entry to bucket `slot`, doubling the entry arrays when full (FUN_14213e140).
    entry* append(uint32_t slot) {
        uint32_t index = this->m_count;
        if (index == this->m_capacity) {
            uint32_t capacity = this->m_capacity * 2;
            if (!this->m_buckets) {
                this->rehash(this->m_bucketCount);
            }
            if (capacity < 8) {
                capacity = 8;
            }
            auto entries = static_cast<entry*>(this->m_allocator->allocate(uint64_t(capacity) * sizeof(entry), 0x10));
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

    // Inserts or overwrites (FUN_1421434b0); *inserted says which.
    entry* assign(uint64_t key, V value, bool* inserted) {
        uint32_t slot = id_hash(key) & (this->m_bucketCount - 1);
        if (this->m_buckets) {
            for (uint32_t i = this->m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_next[i]) {
                if (this->m_entries[i].m_key == key) {
                    this->m_entries[i].m_value = value;
                    *inserted = false;
                    return &this->m_entries[i];
                }
            }
        }
        entry* added = this->append(slot);
        added->m_key = key;
        added->m_value = value;
        if (this->m_bucketCount < this->m_count) {
            this->rehash(this->m_bucketCount * 2);
        }
        *inserted = true;
        return added;
    }

    // FUN_14213cd50
    entry* find(uint64_t key) {
        if (!this->m_buckets) {
            return nullptr;
        }
        uint32_t slot = id_hash(key) & (this->m_bucketCount - 1);
        for (uint32_t i = this->m_buckets[slot]; i != 0xFFFFFFFF; i = this->m_next[i]) {
            if (this->m_entries[i].m_key == key) {
                return &this->m_entries[i];
            }
        }
        return nullptr;
    }

    // FUN_14213cbd0
    bool remove(uint64_t key) {
        if (!this->m_buckets) {
            return false;
        }
        uint32_t* link = &this->m_buckets[id_hash(key) & (this->m_bucketCount - 1)];
        for (uint32_t i = *link; i != 0xFFFFFFFF; i = *link) {
            if (this->m_entries[i].m_key == key) {
                *link = this->m_next[i];
                uint32_t last = --this->m_count;
                if (i == last) {
                    return true;
                }
                uint64_t movedKey = this->m_entries[last].m_key;
                uint32_t* movedLink = &this->m_buckets[id_hash(movedKey) & (this->m_bucketCount - 1)];
                uint32_t j = *movedLink;
                while (j != 0xFFFFFFFF && this->m_entries[j].m_key != movedKey) {
                    movedLink = &this->m_next[j];
                    j = this->m_next[j];
                }
                this->m_entries[i] = this->m_entries[last];
                *movedLink = i;
                this->m_next[i] = this->m_next[this->m_count];
                return true;
            }
            link = &this->m_next[i];
        }
        return false;
    }

    // FUN_1420d5fd0
    void destroy() {
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
};

// The keys-only form (entries are just the ids): insert (FUN_14210ee90, FUN_142110a00,
// rehash FUN_142110b30), contains, remove (FUN_142142040).
struct id_set {
    uint32_t* m_buckets;        // 0x00
    uint64_t* m_entries;        // 0x08
    uint32_t* m_next;           // 0x10
    allocator* m_allocator;     // 0x18
    uint32_t m_bucketCount;     // 0x20
    uint32_t m_count;           // 0x24
    uint32_t m_capacity;        // 0x28

    void init(allocator* allocator) {
        this->m_buckets = nullptr;
        this->m_entries = nullptr;
        this->m_next = nullptr;
        this->m_allocator = allocator;
        this->m_bucketCount = 0x10;
        this->m_count = 0;
        this->m_capacity = 0;
    }

    void rehash(uint32_t buckets) {
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
                uint32_t slot = id_hash(this->m_entries[i]) & (buckets - 1);
                this->m_next[i] = table[slot];
                table[slot] = i;
            }
        }
        this->m_allocator->deallocate(this->m_buckets);
        this->m_bucketCount = buckets;
        this->m_buckets = table;
    }

    uint64_t* append(uint32_t slot) {
        uint32_t index = this->m_count;
        if (index == this->m_capacity) {
            uint32_t capacity = this->m_capacity * 2;
            if (!this->m_buckets) {
                this->rehash(this->m_bucketCount);
            }
            if (capacity < 8) {
                capacity = 8;
            }
            auto entries = static_cast<uint64_t*>(this->m_allocator->allocate(uint64_t(capacity) * 8, 0x10));
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

    bool insert(uint64_t key) {
        uint32_t slot = id_hash(key) & (this->m_bucketCount - 1);
        if (this->contains(key)) {
            return false;
        }
        *this->append(slot) = key;
        if (this->m_bucketCount < this->m_count) {
            this->rehash(this->m_bucketCount * 2);
        }
        return true;
    }

    bool contains(uint64_t key) const {
        if (!this->m_buckets) {
            return false;
        }
        for (uint32_t i = this->m_buckets[id_hash(key) & (this->m_bucketCount - 1)]; i != 0xFFFFFFFF; i = this->m_next[i]) {
            if (this->m_entries[i] == key) {
                return true;
            }
        }
        return false;
    }

    bool remove(uint64_t key) {
        if (!this->m_buckets) {
            return false;
        }
        uint32_t* link = &this->m_buckets[id_hash(key) & (this->m_bucketCount - 1)];
        for (uint32_t i = *link; i != 0xFFFFFFFF; i = *link) {
            if (this->m_entries[i] == key) {
                *link = this->m_next[i];
                uint32_t last = --this->m_count;
                if (i == last) {
                    return true;
                }
                uint64_t movedKey = this->m_entries[last];
                uint32_t* movedLink = &this->m_buckets[id_hash(movedKey) & (this->m_bucketCount - 1)];
                uint32_t j = *movedLink;
                while (j != 0xFFFFFFFF && this->m_entries[j] != movedKey) {
                    movedLink = &this->m_next[j];
                    j = this->m_next[j];
                }
                this->m_entries[i] = movedKey;
                *movedLink = i;
                this->m_next[i] = this->m_next[this->m_count];
                return true;
            }
            link = &this->m_next[i];
        }
        return false;
    }

    void destroy() {
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
};

}

#endif
