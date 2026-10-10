#ifndef STORM_ARRAY_TS_FIXED_ARRAY_HPP
#define STORM_ARRAY_TS_FIXED_ARRAY_HPP

// Storm's TSFixedArray<T> as 3.4.3 lays it out (0x18): a count, the data and what is
// allocated. The reference instantiates it per element type (FUN_142451c80 for C2Vector,
// FUN_141294170 for C3Vector, FUN_1424512c0's realloc for bytes, ...); the bodies are the same
// shape, so this template stands for them. Tag is the instantiation's type name, which is what
// the reference frees under.

#include "storm/Memory.hpp"
#include <cstdint>
#include <cstring>

template <class T, const char* Tag>
struct TSFixedArray {
    uint32_t m_count = 0;           // 0x00
    uint32_t m_pad04 = 0;
    T* m_data = nullptr;            // 0x08
    uint32_t m_alloc = 0;           // 0x10
    uint32_t m_pad14 = 0;

    // Storage for exactly `count`, the elements that fit kept. The count is the caller's.
    void ReallocData(uint32_t count) {
        T* old = this->m_data;
        this->m_alloc = count;
        this->m_data = nullptr;
        if (count != 0) {
            this->m_data = static_cast<T*>(SMemAlloc(uint64_t(count) * sizeof(T), 8, Tag, 0, -7, 0));
        }
        if (old != nullptr) {
            uint32_t keep = this->m_count < count ? this->m_count : count;
            for (uint32_t i = 0; i < keep; i++) {
                this->m_data[i] = old[i];
            }
            SMemFree(old, Tag, -2, 0);
        }
    }

    // `count` elements; the new ones zeroed. Zero frees the storage.
    void SetCount(uint32_t count) {
        if (count == this->m_count) {
            return;
        }
        if (count == 0) {
            if (this->m_data != nullptr) {
                SMemFree(this->m_data, Tag, -2, 0);
            }
            this->m_alloc = 0;
            this->m_count = 0;
            this->m_data = nullptr;
            return;
        }
        this->ReallocData(count);
        if (this->m_count < count) {
            std::memset(this->m_data + this->m_count, 0, uint64_t(count - this->m_count) * sizeof(T));
        }
        this->m_count = count;
    }

    // The destructor's free.
    void Free() {
        if (this->m_data != nullptr) {
            SMemFree(this->m_data, Tag, -2, 0);
        }
    }
};

#endif
