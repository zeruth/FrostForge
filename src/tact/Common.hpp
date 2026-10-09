#ifndef TACT_COMMON_HPP
#define TACT_COMMON_HPP

// pkg/tact/3.6.39: shared pieces - the package allocators, its growable array, the log
// category and the text-field parsers.

#include "blz/Allocator.hpp"
#include "blz/Log.hpp"
#include <cstdint>
#include <cstring>

namespace tact {

// The log category every tact message carries (DAT_142630cdc).
extern const char g_logCategory[];

// The general allocator and the one file buffers come from.
blz::allocator* GetAllocator();
blz::allocator* GetBufferAllocator();

// The package code often calls its allocator's function directly rather than through the
// interface: fn(context, null, size, alignment, flag, tag, line) to allocate and
// fn(context, ptr, 0, 0, 0, 0, 0) to free.
void* PackageAlloc(blz::allocator* allocator, uint64_t size, uint64_t alignment, bool flag, const char* tag,
    int32_t line);
void PackageFree(blz::allocator* allocator, void* ptr);

// Fields the reference addresses by byte offset inside members whose subsystem is not ported
// yet; replaced by typed members as each one lands.
template <class T>
inline T& FieldAt(void* base, uint64_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// tact's growable array: allocator, data, count, capacity (0x18 bytes). Growth is half again
// the capacity, at least what is needed, at least 8 elements; the block comes from the
// allocator's sized allocate and the old one is freed.
template <class T>
struct Array {
    blz::allocator* m_allocator;
    T* m_data;
    uint32_t m_count;
    uint32_t m_capacity;

    void Init(blz::allocator* allocator) {
        this->m_allocator = allocator;
        this->m_data = nullptr;
        this->m_count = 0;
        this->m_capacity = 0;
    }

    void Reserve(uint32_t count) {
        uint32_t capacity = this->m_capacity;
        if (capacity >= count) {
            return;
        }
        capacity += capacity >> 1;
        if (capacity < count) {
            capacity = count;
        }
        if (capacity < 8) {
            capacity = 8;
        }
        uint64_t allocated;
        auto data = static_cast<T*>(this->m_allocator->allocate_sized(&allocated, uint64_t(capacity) * sizeof(T), alignof(T)));
        std::memcpy(data, this->m_data, uint64_t(this->m_count) * sizeof(T));
        this->m_allocator->deallocate(this->m_data);
        this->m_data = data;
        this->m_capacity = static_cast<uint32_t>(allocated / sizeof(T));
    }

    void Free() {
        if (this->m_data) {
            this->m_allocator->deallocate(this->m_data);
        }
    }
};

// Field parsers for PSV columns: (text, length, destination) -> success.
bool ParseString(const char* text, uint64_t length, void* out);
bool ParseHexKey(const char* text, uint64_t length, void* out);
bool ParseBool(const char* text, uint64_t length, void* out);

// Decodes hex pairs into out (at most outSize bytes, stopping at the first non-hex pair or
// when fewer than two characters remain); returns the pairs decoded.
uint64_t HexDecode(uint8_t* out, uint64_t outSize, const char* text, uint64_t length);

// Lower-case hex of `length` bytes into out (at most outSize - 1 characters, terminated);
// returns the full length the text would have (length * 2).
uint64_t HexEncode(char* out, uint64_t outSize, const uint8_t* data, uint64_t length);

// A 16-byte key as hex, for log messages.
uint64_t KeyToString(char* out, uint64_t outSize, const uint8_t key[16]);

// Statistics in statsd form ("tact.<name>:<value>|c") handed to the instance's callback block
// (instance+0x2d8: {owner, report, context, ...}). kind: 0 counter, 1 gauge, 2 set, 3 timer.
void StatsReport(void* sink, int32_t kind, const char* name, int64_t value, float rate);

// A 16-byte key in hex after optional whitespace: the characters consumed, or 0 unless all
// 16 bytes were there.
uint64_t ParseKey16(const char* text, uint64_t length, uint8_t key[16], bool* error);

// An unsigned decimal with optional whitespace and sign: the characters consumed (0 for
// none); *overflow on overflow or a nonzero negative value.
uint64_t ParseUInt64(const char* text, uint64_t length, uint64_t* value, bool* overflow);

}

#endif
