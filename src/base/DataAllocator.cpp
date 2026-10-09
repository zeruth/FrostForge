#include "base/DataAllocator.hpp"

#include <cstring>

#include "storm/Memory.hpp"

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Base\\CDataAllocator.cpp";

}

// ref: FUN_142430da0
CDataAllocator::CDataAllocator(uint32_t blockBytes, uint32_t blocksPerChunk) {
    this->m_blockBytes = blockBytes < 8 ? 8 : blockBytes;
    this->m_inUse = 0;
    this->m_chunks = 0;
    this->m_chunkList = nullptr;
    this->m_free = nullptr;
    this->m_warned = false;
    this->m_blocksPerChunk = blocksPerChunk == 0 ? 1 : blocksPerChunk;
}

// ref: FUN_142430de0
CDataAllocator::~CDataAllocator() {
    while (void* chunk = this->m_chunkList) {
        this->m_chunkList = *static_cast<void**>(chunk);
        SMemFree(chunk, s_file, 0xB7, 0);
    }
    this->m_inUse = 0;
    this->m_free = nullptr;
}

// ref: FUN_142430e30
void CDataAllocator::Clear(const char* tag, int32_t line) {
    if (this->m_chunkList) {
        const char* chunkTag = tag ? tag : s_file;
        int32_t chunkLine = tag ? line : 0xB7;
        do {
            void* chunk = this->m_chunkList;
            this->m_chunkList = *static_cast<void**>(chunk);
            SMemFree(chunk, chunkTag, chunkLine, 0);
        } while (this->m_chunkList);
    }
    this->m_inUse = 0;
    this->m_free = nullptr;
}

// ref: FUN_142430eb0
void* CDataAllocator::GetData(bool zero, const char* tag, int32_t line, int32_t heap) {
    const char* chunkTag = nullptr;
    int32_t chunkLine = 0;
    if (this->m_blocksPerChunk != 1) {
        chunkTag = tag;
        chunkLine = line;
    }
    if (this->m_free == nullptr) {
        auto chunk = static_cast<void**>(SMemAlloc(uint64_t(this->m_blocksPerChunk) * this->m_blockBytes + 8, heap,
            chunkTag ? chunkTag : s_file, 0, chunkTag ? chunkLine : 0x10F, 0));
        uint32_t chunks = ++this->m_chunks;
        if (!this->m_warned && chunks > 9999) {
            this->m_warned = true;
        }
        auto block = reinterpret_cast<char*>(chunk + 1);
        this->m_free = block;
        for (uint32_t i = 1; i < this->m_blocksPerChunk; i++) {
            char* next = block + this->m_blockBytes;
            *reinterpret_cast<void**>(block) = next;
            block = next;
        }
        *reinterpret_cast<void**>(block) = nullptr;
        *chunk = this->m_chunkList;
        this->m_chunkList = chunk;
    }
    void* block = this->m_free;
    this->m_free = *static_cast<void**>(block);
    if (zero) {
        memset(block, 0, this->m_blockBytes);
    }
    this->m_inUse++;
    return block;
}

// ref: FUN_142430fb0
void CDataAllocator::PutData(void* block) {
    *static_cast<void**>(block) = this->m_free;
    this->m_inUse--;
    this->m_free = block;
}
