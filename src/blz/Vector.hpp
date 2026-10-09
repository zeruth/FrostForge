#ifndef BLZ_VECTOR_HPP
#define BLZ_VECTOR_HPP

// blz::vector<T> and blz's open-addressing hash map, as the engine instantiates them: storage
// from Storm tagged with the element's RTTI name, growth by half again (at least one), and
// bit 63 of the capacity word marking storage the vector does not own. The reference has one
// copy of each per element type; these templates stand for all of them.

#include "blz/Murmur.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

namespace blz {

constexpr uint64_t VECTOR_NOT_OWNED = 0x8000000000000000ull;

template <class T, const char* Tag>
class vector {
    public:
    T* m_data = nullptr;
    uint64_t m_size = 0;
    uint64_t m_capacity = 0;

    vector() = default;
    vector(const vector&) = delete;
    vector& operator=(const vector&) = delete;

    // Takes the storage over (what a reallocation of a vector of vectors does).
    vector(vector&& other) noexcept : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity) {
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    ~vector() {
        this->clear();
        this->release_storage();
    }

    uint64_t size() const {
        return this->m_size;
    }

    uint64_t capacity() const {
        return this->m_capacity & ~VECTOR_NOT_OWNED;
    }

    T* begin() {
        return this->m_data;
    }

    T* end() {
        return this->m_data + this->m_size;
    }

    T& operator[](uint64_t index) {
        return this->m_data[index];
    }

    T& back() {
        return this->m_data[this->m_size - 1];
    }

    // Room for at least `count`: half again the capacity, or `count` if that is more. The
    // elements move over; the old storage is freed when owned.
    void reserve(uint64_t count) {
        uint64_t capacity = this->capacity();
        if (capacity >= count) {
            return;
        }
        uint64_t grown = ((this->m_capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + capacity;
        if (grown < count) {
            grown = count;
        }
        this->m_capacity = ((this->m_capacity ^ grown) & VECTOR_NOT_OWNED) ^ grown;
        auto data = static_cast<T*>(SMemAlloc(grown * sizeof(T), 0xb, Tag, SStrHashFnv1a(Tag), -6, 0));
        for (uint64_t i = 0; i < this->m_size; i++) {
            new (&data[i]) T(std::move(this->m_data[i]));
            this->m_data[i].~T();
        }
        this->release_storage();
        this->m_capacity &= ~VECTOR_NOT_OWNED;
        this->m_data = data;
    }

    // push_back's growth: half again, at least one more.
    void grow_for_push() {
        uint64_t capacity = this->capacity();
        if (this->m_size == capacity && capacity < capacity + 1) {
            uint64_t half = (this->m_capacity >> 1) & 0x3FFFFFFFFFFFFFFFull;
            this->reserve(capacity + (capacity + 1 <= capacity + half ? half : 1));
        }
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        this->grow_for_push();
        T* slot = new (&this->m_data[this->m_size]) T(std::forward<Args>(args)...);
        this->m_size++;
        return *slot;
    }

    // Grows with value-initialized elements, or shrinks destroying the tail.
    void resize(uint64_t count) {
        if (count < this->m_size) {
            for (uint64_t i = count; i < this->m_size; i++) {
                this->m_data[i].~T();
            }
            this->m_size = count;
        } else if (count > this->m_size) {
            this->reserve(count);
            for (uint64_t i = this->m_size; i < count; i++) {
                new (&this->m_data[i]) T();
            }
            this->m_size = count;
        }
    }

    // Removes [first, last), the tail moving down.
    void erase(uint64_t first, uint64_t last) {
        uint64_t count = last - first;
        for (uint64_t i = first; i + count < this->m_size; i++) {
            this->m_data[i] = std::move(this->m_data[i + count]);
        }
        for (uint64_t i = this->m_size - count; i < this->m_size; i++) {
            this->m_data[i].~T();
        }
        this->m_size -= count;
    }

    void clear() {
        for (uint64_t i = 0; i < this->m_size; i++) {
            this->m_data[i].~T();
        }
        this->m_size = 0;
    }

    T* data() {
        return this->m_data;
    }

    void push_back(const T& value) {
        this->emplace_back(value);
    }

    void pop_back() {
        this->m_size--;
        this->m_data[this->m_size].~T();
    }

    // Removes the element at `index` by moving the last one into its place.
    void erase_unordered(uint64_t index) {
        if (index != this->m_size - 1) {
            this->m_data[index] = std::move(this->m_data[this->m_size - 1]);
        }
        this->pop_back();
    }

    protected:
    void release_storage() {
        if ((this->m_capacity & VECTOR_NOT_OWNED) == 0 && this->m_data != nullptr) {
            SMemFree(this->m_data, Tag, -6, 0);
        }
    }
};

// blz::embedded_vector<T, N>: a vector whose first N elements live in the object, the storage
// marked not owned until it outgrows them.
template <class T, uint64_t N, const char* Tag>
class embedded_vector : public vector<T, Tag> {
    public:
    embedded_vector() {
        this->m_data = reinterpret_cast<T*>(this->m_inline);
        this->m_capacity = N | VECTOR_NOT_OWNED;
    }

    // A heap block is taken over; elements in the source's own storage move one by one.
    embedded_vector(embedded_vector&& other) noexcept {
        this->m_data = reinterpret_cast<T*>(this->m_inline);
        this->m_capacity = N | VECTOR_NOT_OWNED;
        if ((other.m_capacity & VECTOR_NOT_OWNED) == 0) {
            this->m_data = other.m_data;
            this->m_size = other.m_size;
            this->m_capacity = other.m_capacity;
            other.m_data = reinterpret_cast<T*>(other.m_inline);
            other.m_size = 0;
            other.m_capacity = N | VECTOR_NOT_OWNED;
            return;
        }
        this->reserve(other.m_size);
        for (uint64_t i = 0; i < other.m_size; i++) {
            new (&this->m_data[i]) T(std::move(other.m_data[i]));
        }
        this->m_size = other.m_size;
        other.clear();
    }

    private:
    alignas(T) uint8_t m_inline[N * sizeof(T)];
};

// blz::deque<T>: a power-of-two map of blocks of `PerBlock` elements, indexed from `m_offset`.
// Only the operations the engine uses on its free lists are here.
template <class T, uint64_t PerBlock, const char* MapTag, const char* BlockTag>
class deque {
    public:
    T** m_map = nullptr;            // 0x00
    uint64_t m_mapSize = 0;         // 0x08
    uint64_t m_offset = 0;          // 0x10
    uint64_t m_size = 0;            // 0x18

    deque() = default;
    deque(const deque&) = delete;
    deque& operator=(const deque&) = delete;

    uint64_t size() const {
        return this->m_size;
    }

    T& at(uint64_t index) {
        uint64_t slot = this->m_offset + index;
        return this->m_map[(slot / PerBlock) & (this->m_mapSize - 1)][slot % PerBlock];
    }

    T& back() {
        return this->at(this->m_size - 1);
    }

    void pop_back() {
        this->m_size--;
    }

    void clear() {
        this->m_offset = 0;
        this->m_size = 0;
    }

    // ref: FUN_141ef4e00 (the uint32 instance): grows the map when the back is at a block
    // boundary and the map is full, then hands out the next slot.
    T* push_back_slot() {
        if (((this->m_size + this->m_offset) % PerBlock) == 0 && this->m_mapSize <= (this->m_size + PerBlock) / PerBlock) {
            uint64_t mapSize = this->m_mapSize == 0 ? 8 : this->m_mapSize;
            while (mapSize < this->m_mapSize + 1) {
                mapSize *= 2;
            }
            auto map = static_cast<T**>(SMemAlloc(mapSize * sizeof(T*), 0xb, MapTag, SStrHashFnv1a(MapTag), -6, 0));
            uint64_t first = (this->m_offset / PerBlock) & (this->m_mapSize - 1);
            if (this->m_mapSize != 0) {
                std::memcpy(map, this->m_map + first, (this->m_mapSize - first) * sizeof(T*));
                std::memcpy(map + (this->m_mapSize - first), this->m_map, first * sizeof(T*));
            }
            std::memset(map + this->m_mapSize, 0, (mapSize - this->m_mapSize) * sizeof(T*));
            if (this->m_map != nullptr) {
                SMemFree(this->m_map, MapTag, -6, 0);
            }
            this->m_offset %= PerBlock;
            this->m_map = map;
            this->m_mapSize = mapSize;
        }
        uint64_t slot = this->m_size + this->m_offset;
        this->m_size++;
        T** block = &this->m_map[(slot / PerBlock) & (this->m_mapSize - 1)];
        if (*block == nullptr) {
            *block = static_cast<T*>(SMemAlloc(PerBlock * sizeof(T), 0xb, BlockTag, SStrHashFnv1a(BlockTag), -6, 0));
        }
        return &(*block)[slot % PerBlock];
    }
};

// blz's open-addressing map from a pointer-sized key (0 empty, ~0 removed): linear probing
// from the key's fmix64, at most `m_maxLoad` full counting removals, power-of-two tables.
template <class V, const char* Tag>
class pointer_map {
    public:
    struct entry {
        uint64_t m_key;
        V m_value;
    };

    uint64_t m_capacity = 0;        // 0x00
    entry* m_entries = nullptr;     // 0x08
    uint64_t m_size = 0;            // 0x10: live and removed
    uint64_t m_removed = 0;         // 0x18
    float m_maxLoad = 0.75f;        // 0x20

    pointer_map() = default;
    pointer_map(const pointer_map&) = delete;
    pointer_map& operator=(const pointer_map&) = delete;

    pointer_map(pointer_map&& other) noexcept
        : m_capacity(other.m_capacity), m_entries(other.m_entries), m_size(other.m_size), m_removed(other.m_removed),
          m_maxLoad(other.m_maxLoad) {
        other.m_capacity = 0;
        other.m_entries = nullptr;
        other.m_size = 0;
        other.m_removed = 0;
    }

    ~pointer_map() {
        if (this->m_entries != nullptr) {
            SMemFree(this->m_entries, Tag, -6, 0);
        }
    }

    // The table at construction: 8 empty buckets.
    void init(float maxLoad) {
        this->m_capacity = 8;
        this->m_entries = static_cast<entry*>(SMemAlloc(8 * sizeof(entry), 0xb, Tag, SStrHashFnv1a(Tag), -6, 0));
        for (uint64_t i = 0; i < 8; i++) {
            this->m_entries[i].m_key = 0;
        }
        this->m_maxLoad = maxLoad;
    }

    // Empties the table, keeping its buckets.
    void clear() {
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            this->m_entries[i].m_key = 0;
        }
        this->m_size = 0;
        this->m_removed = 0;
    }

    // Rehashes when `count` entries would pass the load limit (FUN_1422e6ce0, FUN_1422e6d60).
    bool reserve_for(uint64_t count) {
        float load = static_cast<float>(count) / static_cast<float>(this->m_capacity);
        if (load < this->m_maxLoad || load == this->m_maxLoad) {
            return false;
        }
        this->rehash(0);
        return true;
    }

    // The value for `key`, inserted value-initialized when missing (FUN_1422ccbf0): the probe
    // remembers the first removed slot and reuses it.
    V* insert(uint64_t key, bool* inserted = nullptr) {
        this->reserve_for(this->m_size + 1);
        uint64_t mask = this->m_capacity - 1;
        uint64_t index = fmix64(key) & mask;
        entry* removed = nullptr;
        for (uint64_t step = 0;; step++) {
            entry* slot = &this->m_entries[index];
            if (slot->m_key == 0) {
                if (removed != nullptr) {
                    slot = removed;
                }
                slot->m_key = key;
                new (&slot->m_value) V();
                this->m_size++;
                if (inserted != nullptr) {
                    *inserted = true;
                }
                return &slot->m_value;
            }
            if (slot->m_key == ~0ull) {
                if (removed == nullptr) {
                    removed = slot;
                }
            } else if (slot->m_key == key) {
                if (inserted != nullptr) {
                    *inserted = false;
                }
                return &slot->m_value;
            }
            index = (index + step + 1) & mask;
        }
    }

    // Marks `key` removed (FUN_1422da5d0's loop).
    void erase(uint64_t key) {
        if (this->m_capacity == 0) {
            return;
        }
        uint64_t mask = this->m_capacity - 1;
        uint64_t index = fmix64(key) & mask;
        for (uint64_t step = 0; this->m_entries[index].m_key != 0; step++) {
            entry* slot = &this->m_entries[index];
            if ((this->m_removed == 0 || slot->m_key != ~0ull) && slot->m_key == key) {
                slot->m_key = ~0ull;
                this->m_removed++;
                return;
            }
            index = (index + step + 1) & mask;
        }
    }

    // Calls `fn(key, value)` for each live entry.
    template <class Fn>
    void for_each(Fn fn) {
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            entry* slot = &this->m_entries[i];
            if (slot->m_key != 0 && (this->m_removed == 0 || slot->m_key != ~0ull)) {
                fn(slot->m_key, slot->m_value);
            }
        }
    }

    V* find(uint64_t key) {
        if (this->m_capacity == 0) {
            return nullptr;
        }
        uint64_t mask = this->m_capacity - 1;
        uint64_t index = fmix64(key) & mask;
        for (uint64_t step = 0;; step++) {
            entry* slot = &this->m_entries[index];
            if (slot->m_key == key) {
                return &slot->m_value;
            }
            if (slot->m_key == 0) {
                return nullptr;
            }
            index = (index + step + 1) & mask;
        }
    }

    // The table for at least `count` live entries at the load limit (FUN_1422c6a10 for the
    // state tracker's instance): removals are dropped as the entries move.
    void rehash(uint64_t count) {
        float live = static_cast<float>(this->m_size - this->m_removed);
        float needed = live / this->m_maxLoad;
        uint64_t wanted = static_cast<uint64_t>(needed);
        if (live > static_cast<float>(wanted) * this->m_maxLoad) {
            wanted++;
        }
        if (wanted < count) {
            wanted = count;
        }
        uint64_t capacity = 8;
        while (capacity < wanted) {
            capacity *= 2;
        }
        if (capacity == this->m_capacity && this->m_removed == 0) {
            return;
        }
        auto entries = static_cast<entry*>(SMemAlloc(capacity * sizeof(entry), 0xb, Tag, SStrHashFnv1a(Tag), -6, 0));
        for (uint64_t i = 0; i < capacity; i++) {
            entries[i].m_key = 0;
        }
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            entry* old = &this->m_entries[i];
            if (old->m_key == 0 || old->m_key == ~0ull) {
                continue;
            }
            uint64_t mask = capacity - 1;
            uint64_t index = fmix64(old->m_key) & mask;
            for (uint64_t step = 0; entries[index].m_key != 0;) {
                step++;
                index = (index + step) & mask;
            }
            new (&entries[index].m_value) V(std::move(old->m_value));
            entries[index].m_key = old->m_key;
        }
        if (this->m_entries != nullptr) {
            SMemFree(this->m_entries, Tag, -6, 0);
        }
        this->m_size -= this->m_removed;
        this->m_removed = 0;
        this->m_entries = entries;
        this->m_capacity = capacity;
    }
};

}

#endif
