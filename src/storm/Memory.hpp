#ifndef STORM_MEMORY_HPP
#define STORM_MEMORY_HPP

#include <cstddef>
#include <cstdint>

// SMemAlloc flags
#define SMEM_FLAG_ZEROMEMORY 0x8
#define SMEM_FLAG_NOHOOKS    0x10000000

// Hooks an allocator installs over the default CRT-backed one (SMemInstallHooks). Only the
// first, second and fourth are used by SMemAlloc/SMemFree; the rest are stored for others.
typedef void* (*SMEMALLOCHOOK)(uint64_t bytes, int32_t a2, const char* tag, int32_t line, bool zero);
typedef bool (*SMEMFREEHOOK)(void* ptr);
typedef void* (*SMEMOWNERHOOK)(void* ptr, int32_t* out);
typedef void* (*SMEMREALLOCHOOK)(void* ptr, uint64_t bytes, int32_t a2, const char* tag, int32_t line, bool zero);

// Allocates `bytes`. 3.4.3 passes the allocation site as tag + hash of the tag + line: the
// tag is __FILE__ or, for typed allocations, the object's RTTI type name.
void* SMemAlloc(uint64_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line, uint32_t flags);

// SMemAlloc with an alignment; above 16 it over-allocates and skips the hooks.
void* SMemAllocAligned(uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag, uint32_t tagHash, int32_t line,
    uint32_t flags);

uint64_t SMemSize(void* ptr);
void* SMemReAlloc(void* ptr, uint64_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line, uint32_t flags);
void* SMemAllocAlignedSize(uint64_t* allocated, uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag,
    uint32_t tagHash, int32_t line, uint32_t flags);
void* SMemReAllocAligned(void* ptr, uint64_t bytes, uint64_t alignment, int32_t a2, const char* tag, uint32_t tagHash,
    int32_t line, uint32_t flags);
void* SMemReAllocAlignedSize(void* ptr, uint64_t* allocated, uint64_t bytes, uint64_t alignment, int32_t a2,
    const char* tag, uint32_t tagHash, int32_t line, uint32_t flags);

// Frees a block. `tag`/`line` name the call site for the corruption report.
bool SMemFree(void* ptr, const char* tag, int32_t line, uint32_t flags);

// Development aid (FROST_NODELOG=1): a watched block that SMemFree frees logs the freeing stack.
// The owner unwatches a block before freeing it itself.
bool MemoryWatchEnabled();
void MemoryWatchBlock(void* block, bool watch);
void MemoryWatchFreed(void* ptr, const char* tag);
// FROST_GUARD_HEAP: the stack that freed the block holding `address`, logged, when it was freed.
void MemoryDescribeFreed(uint64_t address);
// FROST_GUARD_HEAP=world: start guarding new blocks (at Enter World).
void MemoryGuardStart();
// FROST_HEAP_VALIDATE=1: HeapValidate the process heap, logging the first checkpoint that fails.
void MemoryHeapCheck(const char* where, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0);
// FROST_GUARD_HEAP=world: report writes into quarantined (freed) blocks.
void MemoryQuarantineScan();

// Storm's tagged global new (Memory.cpp); the plain new/new[]/delete/delete[] are replaced
// there too. Use as `new (a2, tag, tagHash, line) T(...)`.
void* operator new(size_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line);
void operator delete(void* ptr, int32_t a2, const char* tag, uint32_t tagHash, int32_t line) noexcept;
void* operator new[](size_t bytes, int32_t a2, const char* tag, uint32_t tagHash, int32_t line);
void operator delete[](void* ptr, int32_t a2, const char* tag, uint32_t tagHash, int32_t line) noexcept;

void SMemInstallHooks(SMEMALLOCHOOK alloc, SMEMFREEHOOK free, void* hook3, SMEMOWNERHOOK owner,
    void* hook5, void* hook6, void* hook7, void* hook8);

// Registers the memory system's exit handlers (an onexit hook and an atexit hook).
void MemoryRegisterExitHandlers();

// Initializes the two allocator heaps and installs the eight allocator hooks.
void MemoryInstallAllocatorHooks();

#endif
