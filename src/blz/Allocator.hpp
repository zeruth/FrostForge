#ifndef BLZ_ALLOCATOR_HPP
#define BLZ_ALLOCATOR_HPP

#include <cstdint>

namespace blz {

// The allocator interface the blz containers and the packages (tact, casc) allocate through.
// Slot order follows the reference vtables (0x14292bf38): +0x08 allocate with the size
// reported, +0x10 allocate, +0x18 reallocate, +0x20 reallocate with the size reported,
// +0x30 deallocate, +0x38 size of a block.
class allocator {
    public:
    virtual ~allocator() = default;
    virtual void* allocate_sized(uint64_t* allocated, uint64_t size, uint64_t alignment) = 0;
    virtual void* allocate(uint64_t size, uint64_t alignment) = 0;
    virtual void* reallocate(void* ptr, uint64_t size, uint64_t alignment) = 0;
    virtual void* reallocate_sized(void* ptr, uint64_t* allocated, uint64_t size, uint64_t alignment) = 0;
    virtual void unused28() {}
    virtual void deallocate(void* ptr) = 0;
    virtual uint64_t size(void* ptr) = 0;
};

// The process allocator: Storm's aligned allocator under tag 0xb, line -6.
allocator* default_allocator();

// An allocator that forwards everything to a realloc-style function with a context, as the
// packages configure theirs: fn(context, ptr, size, alignment, flag, tag, line).
typedef void* (*realloc_function)(void* context, void* ptr, uint64_t size, uint64_t alignment, bool flag,
    const char* tag, int32_t line);

class function_allocator : public allocator {
    public:
    realloc_function m_function;
    void* m_context;
    bool m_flag;  // what the two package allocators differ by: 1 for the general one

    function_allocator(realloc_function function, void* context, bool flag)
        : m_function(function), m_context(context), m_flag(flag) {}

    void* allocate_sized(uint64_t* allocated, uint64_t size, uint64_t alignment) override;
    void* allocate(uint64_t size, uint64_t alignment) override;
    void* reallocate(void* ptr, uint64_t size, uint64_t alignment) override;
    void* reallocate_sized(void* ptr, uint64_t* allocated, uint64_t size, uint64_t alignment) override;
    void deallocate(void* ptr) override;
    uint64_t size(void* ptr) override;
};

}

#endif
