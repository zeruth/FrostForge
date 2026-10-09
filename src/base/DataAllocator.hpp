#ifndef BASE_DATA_ALLOCATOR_HPP
#define BASE_DATA_ALLOCATOR_HPP

// CDataAllocator (Engine/Source/Base/CDataAllocator.cpp): a pool of fixed-size blocks carved
// from chunks of `blocksPerChunk`, recycled through a free list. Chunks are only returned when
// the whole pool is cleared.

#include <cstdint>

class CDataAllocator {
    public:
    uint32_t m_blockBytes;      // 0x00 (at least 8)
    uint32_t m_blocksPerChunk;  // 0x04
    uint32_t m_inUse;           // 0x08
    uint32_t m_chunks;          // 0x0C
    void* m_chunkList;          // 0x10 chunks, linked through their first word
    void* m_free;               // 0x18 free blocks, linked through their first word
    bool m_warned;              // 0x20 set once past 9999 chunks

    CDataAllocator(uint32_t blockBytes, uint32_t blocksPerChunk);
    ~CDataAllocator();

    // A block (zeroed when asked). A chunk of one block is tagged with the allocator's own
    // source position, not the caller's.
    void* GetData(bool zero, const char* tag, int32_t line, int32_t heap);
    void PutData(void* block);
    // Frees every chunk, tagged with the caller's position when one is given.
    void Clear(const char* tag, int32_t line);
};

#endif
