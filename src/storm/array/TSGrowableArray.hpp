#ifndef STORM_ARRAY_TS_GROWABLE_ARRAY_HPP
#define STORM_ARRAY_TS_GROWABLE_ARRAY_HPP

// Storm's TSGrowableArray<T> as 3.4.3 lays it out (0x20): TSFixedArray's count, data and
// allocation, then the growth granularity. The reference instantiates it per element type
// (FUN_141235370 for WowConnection*); the bodies are the same shape, so this template stands
// for them. Tag is the instantiation's type name, which the old storage is freed under; the
// new storage is tagged with the caller's return address, as the reference does.

#include "storm/Memory.hpp"
#include <cstdint>

template <class T, const char* Tag>
struct TSGrowableArray {
    uint32_t m_count = 0;           // 0x00
    uint32_t m_pad04 = 0;
    T* m_data = nullptr;            // 0x08
    uint32_t m_alloc = 0;           // 0x10
    uint32_t m_pad14 = 0;
    uint32_t m_chunk = 0;           // 0x18: 0 until the array first grows past 0x20
    uint32_t m_pad1c = 0;

    // Room for `count` more. Below 0x20 elements the storage is rounded up to the count's
    // highest set bit; from 0x20 on, to multiples of 0x20 (which is then kept).
    // ref: FUN_141235370 (the WowConnection* instantiation)
    void Reserve(uint32_t count, const char* tag) {
        uint32_t total = this->m_count + count;
        if (total <= this->m_alloc) {
            return;
        }
        uint32_t chunk = this->m_chunk;
        if (chunk == 0) {
            if (total < 0x20) {
                chunk = total;
                for (uint32_t low = (total - 1) & total; low != 0; low = (low - 1) & low) {
                    chunk = low;
                }
                if (chunk == 0) {
                    chunk = 1;
                }
            } else {
                chunk = 0x20;
                this->m_chunk = 0x20;
            }
        }
        if (total % chunk != 0) {
            total += chunk - total % chunk;
        }
        T* old = this->m_data;
        this->m_alloc = total;
        this->m_data = nullptr;
        if (total != 0) {
            this->m_data = static_cast<T*>(SMemAlloc(uint64_t(total) * sizeof(T), 8, tag, 0, -7, 0));
        }
        if (old != nullptr) {
            uint32_t keep = this->m_count <= total ? this->m_count : total;
            for (uint32_t i = 0; i < keep; i++) {
                this->m_data[i] = old[i];
            }
            SMemFree(old, Tag, -2, 0);
        }
    }

    void Add(const T& value, const char* tag) {
        this->Reserve(1, tag);
        this->m_data[this->m_count++] = value;
    }
};

#endif
