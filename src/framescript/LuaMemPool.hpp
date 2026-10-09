#ifndef FRAMESCRIPT_LUA_MEM_POOL_HPP
#define FRAMESCRIPT_LUA_MEM_POOL_HPP

// The allocator behind FrameScript's Lua state (Engine/Source/lua-wowext/src/lmemPool.cpp):
// fourteen size classes from 16 to 1024 bytes carved out of chunks and recycled through free
// lists, and Storm's heap for anything larger.

#include <cstddef>
#include <cstdint>

// One size class (0x40 bytes).
struct LMEMPOOLCLASS {
    // TSFixedArray<void*, 8>: the chunks
    uint32_t m_chunkCount;      // 0x00
    uint32_t m_unused04;
    void** m_chunks;            // 0x08
    uint32_t m_chunkCapacity;   // 0x10
    uint32_t m_unused14;
    void* m_free;               // 0x18 free blocks, linked through their first word
    uint64_t m_chunkBytes;      // 0x20
    uint64_t m_blockBytes;      // 0x28
    uint64_t m_blocksPerChunk;  // 0x30
    uint64_t m_unused38;        // 0x38
};
static_assert(sizeof(LMEMPOOLCLASS) == 0x40, "LMEMPOOLCLASS is 0x40 bytes");

// The pool: one class per entry of the size table (0x70 bytes).
struct LMEMPOOL {
    LMEMPOOLCLASS* m_classes[14];
};

LMEMPOOL* LuaMemPoolCreate();
void LuaMemPoolDestroy(LMEMPOOL* pool);
// lua_Alloc semantics: nsize 0 frees, a null ptr allocates, otherwise resizes.
void* LuaMemPoolRealloc(LMEMPOOL* pool, void* ptr, size_t osize, size_t nsize);

#endif
