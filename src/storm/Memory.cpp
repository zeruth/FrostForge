#include "storm/Memory.hpp"
#include "storm/ErrorReport.hpp"
#include "util/Unimplemented.hpp"
#include <cstdlib>
#include <cstring>

namespace {

// DAT_142cc8a48: 1 = the CRT-backed default allocator is enabled (its static value).
int32_t s_defaultAllocator = 1;
// DAT_142cc8a4c: whether running out of memory shows a dialog first (static value 0).
uint8_t s_showOutOfMemoryDialog = 0;

// DAT_142e1c450..DAT_142e1c488
SMEMALLOCHOOK s_allocHook;
SMEMFREEHOOK s_freeHook;
void* s_hook3;
SMEMOWNERHOOK s_ownerHook;
SMEMREALLOCHOOK s_reallocHook;  // hook5
void* s_hook6;
void* s_hook7;
void* s_hook8;

}

// Reports that `bytes` could not be allocated at tag/line (optionally through the
// out-of-memory dialog first), under the memory system's lock.
// ref: FUN_140254890
void SMemReportOutOfMemory(const char* tag, int32_t line, uint64_t bytes) {
    (void)tag;
    (void)line;
    (void)bytes;
    (void)s_showOutOfMemoryDialog;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140255140
void* SMemAlloc(uint64_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line, uint32_t flags) {
    (void)tagHash;  // not passed on to the hook, nor used here

    if (s_allocHook && !(flags & SMEM_FLAG_NOHOOKS)) {
        void* block = s_allocHook(bytes, a2, tag, line, (flags & SMEM_FLAG_ZEROMEMORY) != 0);
        if (block) {
            return block;
        }
    }

    uint64_t size = (bytes + 0xF) & ~uint64_t(0xF);

    if (s_defaultAllocator == 1) {
        // a 16-byte header, size | 1 then 0, in front of a 16-byte aligned payload; any
        // gap between them is zero, which is how SMemFree walks back to the header
        uint64_t* header = static_cast<uint64_t*>(
            (flags & SMEM_FLAG_ZEROMEMORY) ? std::calloc(1, size + 0x10) : std::malloc(size + 0x10));
        if (header) {
            header[0] = size | 1;
            header[1] = 0;
            uint64_t* payload = header + 2;
            while (reinterpret_cast<uintptr_t>(payload) & 0xF) {
                *payload++ = 0;
            }
            return payload;
        }
    }

    SMemReportOutOfMemory(tag, line, size);
    return nullptr;
}

// ref: FUN_140255210
bool SMemFree(void* ptr, const char* tag, int32_t line, uint32_t flags) {
    if (!ptr) {
        return true;
    }

    if (s_freeHook && !(flags & SMEM_FLAG_NOHOOKS)) {
        int32_t out = 0;
        if (s_ownerHook(ptr, &out) && s_freeHook(ptr)) {
            return true;
        }
    }

    uint64_t* header = static_cast<uint64_t*>(ptr) - 1;
    uint64_t value = *header;
    if (value == 0) {
        do {
            value = *(header - 1);
            header--;
        } while (value == 0);
        value = static_cast<uint32_t>(value);
    }

    if ((value & 3) != 1) {
        ErrorDisplayFormatted(0x8510007B, nullptr, 0, "Memory corrupted -- %s %d", tag ? tag : "unknown", line);
        return true;
    }

    std::free(header);
    return true;
}

// Aligned allocation. Up to 16 bytes of alignment is what SMemAlloc already gives; beyond
// that the block is over-allocated by `alignment` (bypassing the hooks) and zero-filled one
// qword at a time up to the first aligned address, which is returned. The zero gap is what
// lets SMemFree walk back to the header.
// ref: FUN_140254a00
void* SMemAllocAligned(uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag, uint32_t tagHash, int32_t line,
    uint32_t flags) {
    if (alignment <= 0x10) {
        return SMemAlloc(bytes, a2, tag, tagHash, line, flags);
    }
    auto block = static_cast<uint64_t*>(SMemAlloc(bytes + alignment, a2, tag, tagHash, line, flags | SMEM_FLAG_NOHOOKS));
    do {
        *block++ = 0;
    } while (reinterpret_cast<uintptr_t>(block) & (alignment - 1));
    return block;
}

namespace {

// The global allocation operators' shared body: SMemAlloc's default path without flags (no
// zeroing, hooks always consulted). The hook gets tag/line, the report gets the same.
void* StormNew(uint64_t bytes, int32_t a2, const char* tag, int32_t line) {
    if (s_allocHook) {
        if (void* block = s_allocHook(bytes, a2, tag, line, false)) {
            return block;
        }
    }

    uint64_t size = (bytes + 0xF) & ~uint64_t(0xF);
    if (s_defaultAllocator == 1) {
        if (uint64_t* header = static_cast<uint64_t*>(std::malloc(size + 0x10))) {
            header[0] = size | 1;
            header[1] = 0;
            uint64_t* payload = header + 2;
            while (reinterpret_cast<uintptr_t>(payload) & 0xF) {
                *payload++ = 0;
            }
            return payload;
        }
    }

    SMemReportOutOfMemory(tag, line, size);
    return nullptr;
}

// The global deallocation operators' shared body: SMemFree with the operator's name as the
// tag and line -1.
void StormDelete(void* ptr, const char* name) {
    if (!ptr) {
        return;
    }

    if (s_freeHook) {
        int32_t out = 0;
        if (s_ownerHook(ptr, &out) && s_freeHook(ptr)) {
            return;
        }
    }

    uint64_t* header = static_cast<uint64_t*>(ptr) - 1;
    uint64_t value = *header;
    if (value == 0) {
        do {
            value = *(header - 1);
            header--;
        } while (value == 0);
        value = static_cast<uint32_t>(value);
    }

    if ((value & 3) != 1) {
        ErrorDisplayFormatted(0x8510007B, nullptr, 0, "Memory corrupted -- %s %d", name, -1);
        return;
    }

    std::free(header);
}

}

// Storm replaces the global allocation operators, so every `new` in the client lands in the
// same headered blocks SMemAlloc makes, and SMemFree can release them.

// ref: FUN_140254520
void* operator new(size_t bytes) {
    return StormNew(bytes, 0, nullptr, 0);
}

// The tagged form generated code uses for typed allocations (3.8k call sites): tag is the
// type name, as for SMemAlloc. tagHash is accepted and dropped, as SMemAlloc drops it.
// ref: FUN_1402545c0
void* operator new(size_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line) {
    (void)tagHash;
    return StormNew(bytes, a2, tag, line);
}

// ref: FUN_140254720
void* operator new[](size_t bytes) {
    return StormNew(bytes, 0, "new[]", -1);
}

// The sized form is a thunk to the same body (0x14026bd4c, 4.2k call sites).
// ref: FUN_140254670
void operator delete(void* ptr) noexcept {
    StormDelete(ptr, "delete");
}

void operator delete(void* ptr, size_t) noexcept {
    StormDelete(ptr, "delete");
}

// Matches the tagged new, for a constructor that throws.
void operator delete(void* ptr, int32_t, const char*, uint32_t, int32_t) noexcept {
    StormDelete(ptr, "delete");
}

// The array form of the tagged new is the same body (FUN_1402545c0): the compiler puts the
// element count in front of arrays of destructible types itself.
void* operator new[](size_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line) {
    (void)tagHash;
    return StormNew(bytes, a2, tag, line);
}

void operator delete[](void* ptr, int32_t, const char*, uint32_t, int32_t) noexcept {
    StormDelete(ptr, "delete[]");
}

// ref: FUN_1402547e0
void operator delete[](void* ptr) noexcept {
    StormDelete(ptr, "delete[]");
}

void operator delete[](void* ptr, size_t) noexcept {
    StormDelete(ptr, "delete[]");
}

// ref: FUN_1402550f0
void SMemInstallHooks(SMEMALLOCHOOK alloc, SMEMFREEHOOK free, void* hook3, SMEMOWNERHOOK owner,
    void* hook5, void* hook6, void* hook7, void* hook8) {
    s_hook6 = hook6;
    s_hook7 = hook7;
    s_hook8 = hook8;
    s_reallocHook = reinterpret_cast<SMEMREALLOCHOOK>(hook5);
    s_hook3 = hook3;
    s_allocHook = alloc;
    s_freeHook = free;
    s_ownerHook = owner;
}

// _onexit-style registration of 0x14025b770, then atexit(0x14025b790).
// ref: FUN_14025b7b0
void MemoryRegisterExitHandlers() {
    FROST_UNIMPLEMENTED();
}

// Initializes the heaps at DAT_1430a84a0 and DAT_1430a8c70 and passes eight hooks to
// SMemInstallHooks.
// ref: FUN_1412c46b0
void MemoryInstallAllocatorHooks() {
    FROST_UNIMPLEMENTED();
}

namespace {

// Walks back over the zero gap to a block's header word.
uint64_t* SMemHeader(void* ptr) {
    uint64_t* header = static_cast<uint64_t*>(ptr) - 1;
    while (*header == 0) {
        header--;
    }
    return header;
}

// Resizes a headered block to `size` (already 16-aligned) bytes: in place through the CRT
// for a default-allocator block, else by allocating, copying and freeing the old one.
// ref: FUN_140254f70
void* SMemReAllocBlock(int32_t a2, const char* tag, uint32_t tagHash, int32_t line, void* ptr, uint64_t size) {
    (void)a2;
    (void)tagHash;
    uint64_t* header = SMemHeader(ptr);
    uint64_t value = *header;
    if (static_cast<uint32_t>(value & 3) == static_cast<uint32_t>(s_defaultAllocator) && s_defaultAllocator == 1) {
        auto block = static_cast<uint64_t*>(std::realloc(header, size + 0x10));
        if (block) {
            *block = size | 1;
            return block + (static_cast<uint64_t*>(ptr) - header);
        }
        return nullptr;
    }

    uint64_t* payload = nullptr;
    if (s_defaultAllocator == 1) {
        if (uint64_t* block = static_cast<uint64_t*>(std::malloc(size + 0x10))) {
            block[0] = size | 1;
            block[1] = 0;
            payload = block + 2;
            while (reinterpret_cast<uintptr_t>(payload) & 0xF) {
                *payload++ = 0;
            }
        }
    }
    if (!payload) {
        return nullptr;
    }
    uint64_t old = value & ~uint64_t(0xF);
    std::memcpy(payload, ptr, old < size ? old : size);

    uint64_t* oldHeader = static_cast<uint64_t*>(ptr) - 1;
    uint64_t oldValue = *oldHeader;
    if (oldValue == 0) {
        do {
            oldHeader--;
        } while (*oldHeader == 0);
        oldValue = static_cast<uint32_t>(*oldHeader);
    }
    if ((oldValue & 3) != 1) {
        ErrorDisplayFormatted(0x8510007B, nullptr, 0, "Memory corrupted -- %s %d", tag ? tag : "unknown", line);
        return payload;
    }
    std::free(oldHeader);
    return payload;
}

}

// The size a block was allocated with (rounded to 16), from the owner hook or the header.
// ref: FUN_1402552f0
uint64_t SMemSize(void* ptr) {
    if (s_ownerHook) {
        int32_t out = 0;
        if (uint64_t size = reinterpret_cast<uint64_t>(s_ownerHook(ptr, &out))) {
            return size;
        }
    }
    return *SMemHeader(ptr) & ~uint64_t(0xF);
}

// Resizes a block: SMemAlloc for null, SMemFree for size 0, else the block grows or shrinks
// (new space zeroed with SMEM_FLAG_ZEROMEMORY). The realloc hook goes first.
// ref: FUN_140255350
void* SMemReAlloc(void* ptr, uint64_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line, uint32_t flags) {
    if (s_reallocHook) {
        if (void* block = s_reallocHook(ptr, bytes, a2, tag, line, (flags & SMEM_FLAG_ZEROMEMORY) != 0)) {
            if (ptr && bytes == 0) {
                return nullptr;
            }
            return block;
        }
    }
    if (!ptr) {
        return SMemAlloc(bytes, a2, tag, tagHash, line, flags);
    }
    if (bytes == 0) {
        SMemFree(ptr, tag, line, flags);
        return nullptr;
    }
    uint64_t size = (bytes + 0xF) & ~uint64_t(0xF);
    uint64_t old = *SMemHeader(ptr) & ~uint64_t(0xF);
    if (old == size) {
        return ptr;
    }
    void* block = nullptr;
    if (size < 0xFFFFFFFC) {
        block = SMemReAllocBlock(a2, tag, tagHash, line, ptr, size);
    }
    if (!block) {
        SMemReportOutOfMemory(tag, line, size);
        return nullptr;
    }
    if (old < size && (flags & SMEM_FLAG_ZEROMEMORY)) {
        std::memset(static_cast<uint8_t*>(block) + old, 0, size - old);
    }
    return block;
}

// SMemAllocAligned that also reports the size asked for.
// ref: FUN_140254a90
void* SMemAllocAlignedSize(uint64_t* allocated, uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag,
    uint32_t tagHash, int32_t line, uint32_t flags) {
    *allocated = bytes;
    return SMemAllocAligned(bytes, alignment, a2, tag, tagHash, line, flags);
}

// Aligned resize: SMemReAlloc when the alignment is at most 16 and the result is aligned,
// else a fresh aligned block with the old contents copied in.
// ref: FUN_140254be0
void* SMemReAllocAligned(void* ptr, uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag, uint32_t tagHash,
    int32_t line, uint32_t flags) {
    if (!ptr) {
        return SMemAllocAligned(bytes, alignment, a2, tag, tagHash, line, flags);
    }
    if (bytes == 0) {
        SMemFree(ptr, tag, line, flags);
        return nullptr;
    }
    if (alignment <= 0x10 && s_defaultAllocator == 1) {
        void* block = SMemReAlloc(ptr, bytes, a2, tag, tagHash, line, flags);
        if (block) {
            ptr = block;
            if ((reinterpret_cast<uintptr_t>(block) & (alignment - 1)) == 0) {
                return block;
            }
        }
    }
    void* block = SMemAllocAligned(bytes, alignment, a2, tag, tagHash, line, flags);
    if (block) {
        uint64_t old = *SMemHeader(ptr) & ~uint64_t(0xF);
        std::memcpy(block, ptr, old < bytes ? old : bytes);
    }
    SMemFree(ptr, tag, line, flags);
    return block;
}

// SMemReAllocAligned that also reports the size asked for.
// ref: FUN_140254d80
void* SMemReAllocAlignedSize(void* ptr, uint64_t* allocated, uint64_t bytes, uint64_t alignment, int32_t a2,
    const char* tag, uint32_t tagHash, int32_t line, uint32_t flags) {
    *allocated = bytes;
    return SMemReAllocAligned(ptr, bytes, alignment, a2, tag, tagHash, line, flags);
}
