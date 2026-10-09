#include "blz/Allocator.hpp"
#include "storm/Memory.hpp"

namespace blz {

namespace {

class storm_allocator : public allocator {
    public:
    // ref: FUN_1423637b0
    void* allocate_sized(uint64_t* allocated, uint64_t size, uint64_t alignment) override {
        return SMemAllocAlignedSize(allocated, size, alignment, 0xB, nullptr, 0, -6, 0);
    }

    // ref: FUN_1423637f0
    void* allocate(uint64_t size, uint64_t alignment) override {
        return SMemAllocAligned(size, alignment, 0xB, nullptr, 0, -6, 0);
    }

    // ref: FUN_142363890
    void* reallocate(void* ptr, uint64_t size, uint64_t alignment) override {
        return SMemReAllocAligned(ptr, size, alignment, 0xB, nullptr, 0, -6, 0);
    }

    // ref: FUN_142363840
    void* reallocate_sized(void* ptr, uint64_t* allocated, uint64_t size, uint64_t alignment) override {
        return SMemReAllocAlignedSize(ptr, allocated, size, alignment, 0xB, nullptr, 0, -6, 0);
    }

    // ref: 0x141ed1ea0
    void deallocate(void* ptr) override {
        SMemFree(ptr, nullptr, -6, 0);
    }

    // ref: 0x142363830
    uint64_t size(void* ptr) override {
        return SMemSize(ptr);
    }
};

}

// A function-local static (its destructor registered with atexit).
// ref: FUN_142363a50
allocator* default_allocator() {
    static storm_allocator s_allocator;
    return &s_allocator;
}

// ref: FUN_1420dad20
void* function_allocator::allocate_sized(uint64_t* allocated, uint64_t size, uint64_t alignment) {
    *allocated = size;
    return this->allocate(size, alignment);
}

// ref: FUN_1420dacc0
void* function_allocator::allocate(uint64_t size, uint64_t alignment) {
    return this->m_function(this->m_context, nullptr, size, alignment, this->m_flag, nullptr, 0);
}

// ref: FUN_1420dadc0
void* function_allocator::reallocate(void* ptr, uint64_t size, uint64_t alignment) {
    return this->m_function(this->m_context, ptr, size, alignment, this->m_flag, nullptr, 0);
}

// Only allocation and freeing: a live block asked to resize gives null.
// ref: FUN_1420dae20
void* function_allocator::reallocate_sized(void* ptr, uint64_t* allocated, uint64_t size, uint64_t alignment) {
    if (!ptr) {
        return this->allocate_sized(allocated, size, alignment);
    }
    if (size == 0) {
        this->deallocate(ptr);
    }
    return nullptr;
}

// ref: FUN_1420dad90
void function_allocator::deallocate(void* ptr) {
    if (ptr) {
        this->m_function(this->m_context, ptr, 0, 0, this->m_flag, nullptr, 0);
    }
}

uint64_t function_allocator::size(void* ptr) {
    (void)ptr;
    return 0;
}

}
