#ifndef BASE_STRING_ALLOCATOR_HPP
#define BASE_STRING_ALLOCATOR_HPP

// CStringAllocator (Engine/Source/Base/StringAllocator.cpp): copies strings into 4 KB blocks
// that are only ever freed all at once.

#include <cstdint>

class CStringAllocator {
    public:
    struct BLOCK {
        BLOCK* m_next;
        uint32_t m_used;
        char m_data[0xff4];
    };
    static_assert(sizeof(BLOCK) == 0x1000, "a string block is 4 KB");

    BLOCK* m_blocks = nullptr;

    // A copy of `string` (with its terminator) that lives until Clear.
    const char* Allocate(const char* string);
    void Clear();
};

#endif
