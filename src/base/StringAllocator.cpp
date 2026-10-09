#include "base/StringAllocator.hpp"

#include <cstring>

#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Base\\StringAllocator.cpp";

}

// ref: FUN_142430fc0
const char* CStringAllocator::Allocate(const char* string) {
    uint32_t length = static_cast<uint32_t>(std::strlen(string)) + 1;
    BLOCK* block = this->m_blocks;
    if (block == nullptr || block->m_used + length > sizeof(block->m_data)) {
        block = static_cast<BLOCK*>(SMemAlloc(sizeof(BLOCK), 1, s_file, SStrHashFnv1a(s_file), 0x1f, 0));
        block->m_next = this->m_blocks;
        block->m_used = 0;
        this->m_blocks = block;
    }
    uint32_t used = block->m_used;
    std::memcpy(block->m_data + used, string, length);
    block->m_used = block->m_used + length;
    return block->m_data + used;
}

// ref: FUN_1424310a0
void CStringAllocator::Clear() {
    BLOCK* block = this->m_blocks;
    while (block != nullptr) {
        BLOCK* next = block->m_next;
        operator delete(block, sizeof(BLOCK));
        block = next;
    }
    this->m_blocks = nullptr;
}
