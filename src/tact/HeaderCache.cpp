#include "tact/HeaderCache.hpp"

#include <cstring>
#include <new>

#include "blz/Allocator.hpp"
#include "blz/Murmur.hpp"
#include "tact/Codec.hpp"
#include "tact/Common.hpp"

namespace tact {

namespace {

uint32_t KeyHash(const uint8_t ekey[16]) {
    return blz::murmur3_32(ekey, 9, 0);
}

bool KeyEqual(const uint8_t a[16], const uint8_t b[16]) {
    return std::memcmp(a, b, 16) == 0;
}

void Unlink(HEADERCACHELINK* link) {
    link->m_prev->m_next = link->m_next;
    link->m_next->m_prev = link->m_prev;
    link->m_next = link;
    link->m_prev = link;
}

// Links `link` first (most recent) after the list head.
void LinkFirst(HEADERCACHELINK* head, HEADERCACHELINK* link) {
    HEADERCACHELINK* first = head->m_next;
    link->m_prev = first->m_prev;
    link->m_next = first;
    first->m_prev = link;
    link->m_prev->m_next = link;
}

HEADERCACHEENTRY* EntryOf(HEADERCACHELINK* link) {
    return reinterpret_cast<HEADERCACHEENTRY*>(reinterpret_cast<uint8_t*>(link) - offsetof(HEADERCACHEENTRY, m_link));
}

// FUN_1420dbb30: the buckets rebuilt at a power of two.
void Rebucket(HEADERCACHE* cache, uint32_t count) {
    if ((count & (count - 1)) != 0) {
        int32_t bit = 31;
        while (((count | 1) >> bit) == 0) {
            bit--;
        }
        count = 1u << ((bit + 1) & 31);
    }
    auto buckets = static_cast<uint32_t*>(cache->m_allocator->allocate(uint64_t(count) * 4, 0x10));
    if (count != 0) {
        std::memset(buckets, 0xFF, uint64_t(count) * 4);
    }
    if (cache->m_entries != nullptr) {
        for (uint32_t i = 0; i < cache->m_count; i++) {
            uint32_t slot = KeyHash(cache->m_entries[i].m_ekey) & (count - 1);
            cache->m_next[i] = buckets[slot];
            buckets[slot] = i;
        }
    }
    cache->m_allocator->deallocate(cache->m_buckets);
    cache->m_bucketCount = count;
    cache->m_buckets = buckets;
}

// FUN_1420db9c0: the entry arrays grown; the recency links move with the entries.
void GrowEntries(HEADERCACHE* cache, uint32_t capacity) {
    auto entries = static_cast<HEADERCACHEENTRY*>(cache->m_allocator->allocate(uint64_t(capacity) * sizeof(HEADERCACHEENTRY), 0x10));
    auto next = static_cast<uint32_t*>(cache->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
    for (uint32_t i = 0; i < cache->m_count; i++) {
        HEADERCACHEENTRY* from = &cache->m_entries[i];
        HEADERCACHEENTRY* to = &entries[i];
        std::memcpy(to->m_ekey, from->m_ekey, 16);
        to->m_value = from->m_value;
        from->m_value = nullptr;
        if (from->m_link.m_next == &from->m_link) {
            to->m_link.m_next = &to->m_link;
            to->m_link.m_prev = &to->m_link;
        } else {
            to->m_link.m_next = from->m_link.m_next;
            to->m_link.m_prev = from->m_link.m_prev;
            to->m_link.m_prev->m_next = &to->m_link;
            to->m_link.m_next->m_prev = &to->m_link;
        }
        next[i] = cache->m_next[i];
    }
    cache->m_allocator->deallocate(cache->m_entries);
    cache->m_allocator->deallocate(cache->m_next);
    cache->m_entries = entries;
    cache->m_next = next;
    cache->m_entryCapacity = capacity;
}

HEADERCACHEENTRY* FindEntry(HEADERCACHE* cache, const uint8_t ekey[16]) {
    if (cache->m_buckets == nullptr) {
        return nullptr;
    }
    for (uint32_t i = cache->m_buckets[KeyHash(ekey) & (cache->m_bucketCount - 1)]; i != 0xFFFFFFFF; i = cache->m_next[i]) {
        if (KeyEqual(cache->m_entries[i].m_ekey, ekey)) {
            return &cache->m_entries[i];
        }
    }
    return nullptr;
}

// FUN_1420dafa0: the entry for `ekey`, appended empty when missing.
HEADERCACHEENTRY* FindOrAdd(HEADERCACHE* cache, const uint8_t ekey[16], bool* added) {
    HEADERCACHEENTRY* found = FindEntry(cache, ekey);
    if (found != nullptr) {
        *added = false;
        return found;
    }
    uint32_t slot = KeyHash(ekey) & (cache->m_bucketCount - 1);
    uint32_t index = cache->m_count;
    if (index == cache->m_entryCapacity) {
        uint32_t capacity = cache->m_entryCapacity * 2;
        if (cache->m_buckets == nullptr) {
            Rebucket(cache, cache->m_bucketCount);
            slot = KeyHash(ekey) & (cache->m_bucketCount - 1);
        }
        if (capacity < 8) {
            capacity = 8;
        }
        GrowEntries(cache, capacity);
    }
    cache->m_next[index] = cache->m_buckets[slot];
    cache->m_buckets[slot] = index;
    cache->m_count++;
    HEADERCACHEENTRY* entry = &cache->m_entries[index];
    std::memcpy(entry->m_ekey, ekey, 16);
    entry->m_value = nullptr;
    entry->m_link.m_next = &entry->m_link;
    entry->m_link.m_prev = &entry->m_link;
    if (cache->m_bucketCount < cache->m_count) {
        Rebucket(cache, cache->m_bucketCount * 2);
    }
    *added = true;
    return entry;
}

// FUN_1420db130: drops the entry for `ekey`, the last entry moving into its place.
bool Erase(HEADERCACHE* cache, const uint8_t ekey[16]) {
    if (cache->m_buckets == nullptr) {
        return false;
    }
    uint32_t* link = &cache->m_buckets[KeyHash(ekey) & (cache->m_bucketCount - 1)];
    for (uint32_t i = *link; i != 0xFFFFFFFF; i = *link) {
        HEADERCACHEENTRY* entry = &cache->m_entries[i];
        if (!KeyEqual(entry->m_ekey, ekey)) {
            link = &cache->m_next[i];
            continue;
        }
        *link = cache->m_next[i];
        uint32_t last = --cache->m_count;
        Unlink(&entry->m_link);
        HEADERCACHEVALUE* value = entry->m_value;
        entry->m_value = nullptr;
        if (value != nullptr) {
            value->Release();
        }
        if (i == last) {
            return true;
        }
        HEADERCACHEENTRY* moved = &cache->m_entries[last];
        uint32_t* movedLink = &cache->m_buckets[KeyHash(moved->m_ekey) & (cache->m_bucketCount - 1)];
        while (*movedLink != last) {
            movedLink = &cache->m_next[*movedLink];
        }
        std::memcpy(entry->m_ekey, moved->m_ekey, 16);
        entry->m_value = moved->m_value;
        moved->m_value = nullptr;
        if (moved->m_link.m_next == &moved->m_link) {
            entry->m_link.m_next = &entry->m_link;
            entry->m_link.m_prev = &entry->m_link;
        } else {
            entry->m_link.m_next = moved->m_link.m_next;
            entry->m_link.m_prev = moved->m_link.m_prev;
            entry->m_link.m_prev->m_next = &entry->m_link;
            entry->m_link.m_next->m_prev = &entry->m_link;
            moved->m_link.m_next = &moved->m_link;
            moved->m_link.m_prev = &moved->m_link;
        }
        *movedLink = i;
        cache->m_next[i] = cache->m_next[last];
        return true;
    }
    return false;
}

}

// ref: FUN_1420db500
HEADERCACHEVALUE::~HEADERCACHEVALUE() {
    if (this->m_header != nullptr) {
        EncodingHeaderFree(this->m_header);
    }
}

// ref: FUN_1420db810
void HEADERCACHEVALUE::Destroy() {
    blz::allocator* allocator = GetBufferAllocator();
    this->~HEADERCACHEVALUE();
    PackageFree(allocator, this);
}

void HEADERCACHEVALUE::AddRef() {
    InterlockedIncrement(&this->m_refs);
}

void HEADERCACHEVALUE::Release() {
    if (InterlockedDecrement(&this->m_refs) == 0) {
        this->Destroy();
    }
}

// ref: FUN_1420db860
HEADERCACHEVALUE* HEADERCACHE::Find(const uint8_t ekey[16]) {
    AcquireSRWLockExclusive(&this->m_lock);
    HEADERCACHEVALUE* value = nullptr;
    HEADERCACHEENTRY* entry = FindEntry(this, ekey);
    if (entry != nullptr) {
        if (this->m_recent.m_next != &entry->m_link) {
            Unlink(&entry->m_link);
            LinkFirst(&this->m_recent, &entry->m_link);
        }
        value = entry->m_value;
        if (value != nullptr) {
            value->AddRef();
        }
    }
    ReleaseSRWLockExclusive(&this->m_lock);
    return value;
}

// ref: FUN_1420db550
HEADERCACHEVALUE* HEADERCACHE::Insert(const uint8_t ekey[16], ENCODINGHEADER* header) {
    AcquireSRWLockExclusive(&this->m_lock);
    bool added;
    HEADERCACHEENTRY* entry = FindOrAdd(this, ekey, &added);
    HEADERCACHEVALUE* value;
    if (!added) {
        if (header != entry->m_value->m_header) {
            EncodingHeaderFree(header);
        }
        value = entry->m_value;
        if (value != nullptr) {
            value->AddRef();
        }
    } else {
        void* memory = PackageAlloc(GetBufferAllocator(), sizeof(HEADERCACHEVALUE), 8, false, "", 0);
        HEADERCACHEVALUE* fresh = nullptr;
        if (memory != nullptr) {
            fresh = new (memory) HEADERCACHEVALUE();
            std::memcpy(fresh->m_ekey, entry->m_ekey, 16);
            fresh->m_header = header;
        }
        HEADERCACHEVALUE* previous = entry->m_value;
        if (previous != fresh) {
            entry->m_value = fresh;
            if (fresh != nullptr) {
                fresh->AddRef();
            }
            if (previous != nullptr) {
                previous->Release();
            }
        }
        Unlink(&entry->m_link);
        LinkFirst(&this->m_recent, &entry->m_link);
        value = entry->m_value;
        if (value != nullptr) {
            value->AddRef();
        }
        if (this->m_capacity < this->m_count) {
            HEADERCACHELINK* oldest = this->m_recent.m_prev;
            if (oldest != &this->m_recent) {
                Unlink(oldest);
                uint8_t key[16];
                std::memcpy(key, EntryOf(oldest)->m_value->m_ekey, 16);
                Erase(this, key);
            }
        }
    }
    ReleaseSRWLockExclusive(&this->m_lock);
    return value;
}

}
