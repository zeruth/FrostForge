#ifndef BLZ_ALLOC_STRING_HPP
#define BLZ_ALLOC_STRING_HPP

#include "blz/Allocator.hpp"
#include <cstdint>

namespace blz {

// The allocator-carrying string the packages (tact, casc) use (0x20 bytes). The first word is
// the allocator with bit 0 set while the text is inline: up to 23 characters in bytes 8..0x1e,
// and byte 0x1f holds 23 minus the length, so a full inline string ends in its own terminator.
// Out of line, word 1 is the heap text, +0x10 the length and +0x14 the capacity.
struct alloc_string {
    uint64_t m_allocator;  // 0x00: allocator | inline flag
    union {
        struct {
            char* m_data;        // 0x08
            uint32_t m_size;     // 0x10
            uint32_t m_capacity; // 0x14
            uint64_t m_unused;   // 0x18
        };
        char m_inline[24];       // 0x08..0x1f, byte 0x1f = 23 - length
    };

    static const uint32_t INLINE_CAPACITY = 0x17;

    void Init(allocator* allocator);
    void Destroy();

    bool IsInline() const {
        return (this->m_allocator & 1) != 0;
    }
    allocator* Allocator() const {
        return reinterpret_cast<allocator*>(this->m_allocator & ~uint64_t(1));
    }
    const char* c_str() const {
        return this->IsInline() ? this->m_inline : this->m_data;
    }
    char* data() {
        return this->IsInline() ? this->m_inline : this->m_data;
    }
    uint32_t size() const {
        return this->IsInline() ? INLINE_CAPACITY - static_cast<uint8_t>(this->m_inline[23]) : this->m_size;
    }

    void Reserve(uint32_t capacity);
    void Resize(uint32_t length);
    void Assign(const char* text, uint32_t length);
    void Insert(uint32_t position, const char* text, uint32_t length);
    void MoveConstruct(alloc_string* source);
};
static_assert(sizeof(alloc_string) == 0x20, "alloc_string is 0x20 bytes");

}

#endif
