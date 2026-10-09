#ifndef DB_DBC_TYPES_HPP
#define DB_DBC_TYPES_HPP

// The growable arrays of WoW/Source/DB/DBCTypes.h, which the client database keeps all of its
// tables and indices in: data, size, and a capacity whose bit 63 marks storage the array does
// not own. Storage comes from Storm under DBCTypes.h's tag (allocated at line 0xfb, freed at
// 0xff); growth is half again the capacity, or the requested size if that is more. The
// reference has one copy per element type; this template stands for all of them.

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include "storm/Memory.hpp"
#include "storm/String.hpp"

extern const char s_dbcTypesFile[];

constexpr uint64_t DBC_VECTOR_NOT_OWNED = 0x8000000000000000ull;

template <class T>
class TDBCVector {
    public:
    T* m_data = nullptr;            // 0x00
    uint64_t m_size = 0;            // 0x08
    uint64_t m_capacity = 0;        // 0x10

    TDBCVector() = default;
    TDBCVector(const TDBCVector&) = delete;
    TDBCVector& operator=(const TDBCVector&) = delete;

    // Takes the storage over (what growing an array of arrays does).
    TDBCVector(TDBCVector&& other) noexcept : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity) {
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }

    ~TDBCVector() {
        this->release();
    }

    uint64_t size() const {
        return this->m_size;
    }

    uint64_t capacity() const {
        return this->m_capacity & ~DBC_VECTOR_NOT_OWNED;
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

    const T& operator[](uint64_t index) const {
        return this->m_data[index];
    }

    // Frees owned storage and empties the array.
    void release() {
        if ((this->m_capacity & DBC_VECTOR_NOT_OWNED) == 0 && this->m_data != nullptr) {
            SMemFree(this->m_data, s_dbcTypesFile, 0xff, 0);
        }
        this->m_data = nullptr;
        this->m_size = 0;
        this->m_capacity = 0;
    }

    // Room for `count`: half again the capacity, or `count` when that is more.
    void reserve(uint64_t count) {
        uint64_t capacity = this->capacity();
        if (count <= capacity) {
            return;
        }
        uint64_t grown = ((this->m_capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + capacity;
        if (grown < count) {
            grown = count;
        }
        this->m_capacity = ((this->m_capacity ^ grown) & DBC_VECTOR_NOT_OWNED) ^ grown;
        auto data = static_cast<T*>(SMemAlloc(grown * sizeof(T), 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
        for (uint64_t i = 0; i < this->m_size; i++) {
            new (&data[i]) T(std::move(this->m_data[i]));
        }
        if ((this->m_capacity & DBC_VECTOR_NOT_OWNED) == 0 && this->m_data != nullptr) {
            SMemFree(this->m_data, s_dbcTypesFile, 0xff, 0);
        }
        this->m_data = data;
        this->m_capacity &= ~DBC_VECTOR_NOT_OWNED;
    }

    // Grows filling with `value`, or shrinks.
    void resize(uint64_t count, const T& value) {
        if (count < this->m_size) {
            this->m_size = count;
            return;
        }
        this->reserve(count);
        for (uint64_t i = this->m_size; i < count; i++) {
            new (&this->m_data[i]) T(value);
        }
        this->m_size = count;
    }

    void resize(uint64_t count) {
        this->resize(count, T());
    }

    T& push_back(const T& value) {
        uint64_t capacity = this->capacity();
        if (this->m_size == capacity) {
            this->reserve(capacity + 1);
        }
        T* slot = new (&this->m_data[this->m_size]) T(value);
        this->m_size++;
        return *slot;
    }

    // Opens a slot at `index` (the tail moves up) and returns it.
    T* insert_slot(uint64_t index) {
        uint64_t capacity = this->capacity();
        if (this->m_size == capacity) {
            this->reserve(capacity + 1);
        }
        if (index < this->m_size) {
            std::memmove(&this->m_data[index + 1], &this->m_data[index], (this->m_size - index) * sizeof(T));
        }
        this->m_size++;
        return &this->m_data[index];
    }

    void clear() {
        this->m_size = 0;
    }
};

// Copies `other`'s elements into fresh owned storage (the lookups' copy constructors).
template <class T>
void DBCVectorCopy(TDBCVector<T>* dest, const TDBCVector<T>& other) {
    dest->m_data = nullptr;
    dest->m_size = 0;
    dest->m_capacity = 0;
    if (other.m_size == 0) {
        return;
    }
    dest->m_data = static_cast<T*>(SMemAlloc(other.m_size * sizeof(T), 0x24, s_dbcTypesFile, SStrHashFnv1a(s_dbcTypesFile), 0xfb, 0));
    std::memcpy(dest->m_data, other.m_data, other.m_size * sizeof(T));
    dest->m_size = other.m_size;
    dest->m_capacity = other.m_size;
}

#endif
