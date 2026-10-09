#include "framescript/LuaMemPool.hpp"

#include <cstring>
#include <intrin.h>
#include <new>

#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

// The reference's source path, which tags every allocation the pool makes.
const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\lua-wowext\\src\\lmemPool.cpp";
const char s_arrayTag[] = ".?AV?$TSFixedArray@PEAX$07@@";

// Block size and blocks per chunk of each class (0x14295c148, 0x14295c180).
const uint32_t s_blockBytes[14] = { 16, 24, 32, 40, 48, 56, 64, 80, 128, 160, 256, 512, 768, 1024 };
const uint32_t s_blocksPerChunk[14] = { 1024, 1024, 1024, 1024, 1024, 1024, 1024, 512, 512, 256, 256, 128, 64, 64 };

// The smallest class that holds `bytes`, or -1 for the heap.
int32_t ClassFor(size_t bytes) {
    for (int32_t i = 0; i < 14; i++) {
        if (bytes <= s_blockBytes[i]) {
            return i;
        }
    }
    return -1;
}

// TSFixedArray<void*, 8>::ReallocData: exactly `count` slots, the old ones copied over.
// The reference tags the new array with its caller's return address.
// ref: FUN_1402ef6f0
void ChunksRealloc(LMEMPOOLCLASS* cls, uint32_t count, const char* tag) {
    void** old = cls->m_chunks;
    cls->m_chunkCapacity = count;
    cls->m_chunks = nullptr;
    if (count != 0) {
        cls->m_chunks = static_cast<void**>(SMemAlloc(uint64_t(count) * 8, 8, tag, 0, -7, 0));
    }
    if (old) {
        uint32_t keep = count < cls->m_chunkCount ? count : cls->m_chunkCount;
        for (uint32_t i = 0; i < keep; i++) {
            cls->m_chunks[i] = old[i];
        }
        SMemFree(old, s_arrayTag, -2, 0);
    }
}

// Pops a free block, cutting a new chunk into blocks first when the list is empty.
void* ClassAlloc(LMEMPOOLCLASS* cls) {
    void** block = static_cast<void**>(cls->m_free);
    if (!block) {
        block = static_cast<void**>(SMemAlloc(cls->m_chunkBytes, 0xE, s_file, SStrHashFnv1a(s_file), 0x56, 0));
        uint32_t index = cls->m_chunkCount;
        uint32_t count = index + 1;
        if (count != index) {
            if (count == 0) {
                if (cls->m_chunks) {
                    SMemFree(cls->m_chunks, s_arrayTag, -1, 0);
                }
                cls->m_chunkCapacity = 0;
                cls->m_chunkCount = 0;
                cls->m_chunks = nullptr;
            } else {
                ChunksRealloc(cls, count, static_cast<const char*>(_ReturnAddress()));
                cls->m_chunkCount = count;
            }
        }
        cls->m_chunks[index] = block;
        auto base = reinterpret_cast<char*>(block);
        uint64_t last = 1;
        if (cls->m_blocksPerChunk != 1) {
            for (uint64_t i = 0; i < cls->m_blocksPerChunk - 1; i++) {
                *reinterpret_cast<void**>(base + cls->m_blockBytes * i) = base + cls->m_blockBytes * (i + 1);
            }
            last = cls->m_blocksPerChunk;
        }
        *reinterpret_cast<void**>(base + (last - 1) * cls->m_blockBytes) = cls->m_free;
        cls->m_free = block;
    }
    cls->m_free = *block;
    return block;
}

void ClassFree(LMEMPOOLCLASS* cls, void* block) {
    *static_cast<void**>(block) = cls->m_free;
    cls->m_free = block;
}

}  // namespace

// ref: FUN_14242af80
LMEMPOOL* LuaMemPoolCreate() {
    auto pool = static_cast<LMEMPOOL*>(SMemAlloc(sizeof(LMEMPOOL), 0xE, s_file, SStrHashFnv1a(s_file), 0xD3, 0));
    for (int32_t i = 0; i < 14; i++) {
        auto cls = static_cast<LMEMPOOLCLASS*>(SMemAlloc(sizeof(LMEMPOOLCLASS), 0xE, s_file, SStrHashFnv1a(s_file), 0xD5, 0));
        cls->m_chunkCount = 0;
        cls->m_chunks = nullptr;
        cls->m_chunkCapacity = 0;
        cls->m_unused38 = 0;
        pool->m_classes[i] = cls;
        cls->m_blockBytes = s_blockBytes[i];
        cls->m_free = nullptr;
        cls->m_chunkBytes = uint64_t(s_blockBytes[i]) * s_blocksPerChunk[i];
        cls->m_blocksPerChunk = s_blocksPerChunk[i];
    }
    return pool;
}

// ref: FUN_14242ae90
void LuaMemPoolDestroy(LMEMPOOL* pool) {
    if (!pool) {
        return;
    }
    for (int32_t i = 0; i < 14; i++) {
        LMEMPOOLCLASS* cls = pool->m_classes[i];
        if (!cls) {
            continue;
        }
        for (uint32_t n = cls->m_chunkCount; n != 0; n--) {
            SMemFree(cls->m_chunks[n - 1], s_file, 0x32, 0);
        }
        if (cls->m_chunks) {
            SMemFree(cls->m_chunks, s_arrayTag, -2, 0);
        }
        ::operator delete(cls, sizeof(LMEMPOOLCLASS));
    }
    SMemFree(pool, s_file, 0x14D, 0);
}

// ref: FUN_14242b0c0
void* LuaMemPoolRealloc(LMEMPOOL* pool, void* ptr, size_t osize, size_t nsize) {
    int32_t oldClass = ptr ? ClassFor(osize) : -1;

    if (nsize == 0) {
        if (ptr) {
            if (oldClass >= 0) {
                ClassFree(pool->m_classes[oldClass], ptr);
            } else {
                SMemFree(ptr, s_file, 0xFC, 0);
            }
        }
        return nullptr;
    }

    int32_t newClass = ClassFor(nsize);
    if (oldClass == newClass) {
        if (oldClass < 0) {
            return SMemReAlloc(ptr, nsize, 0xE, s_file, SStrHashFnv1a(s_file), 0x123, 0);
        }
        return ptr;
    }

    void* block;
    if (newClass < 0) {
        block = SMemAlloc(nsize, 0xE, s_file, SStrHashFnv1a(s_file), 0x132, 0);
    } else {
        block = ClassAlloc(pool->m_classes[newClass]);
    }
    if (ptr) {
        memcpy(block, ptr, osize < nsize ? osize : nsize);
        if (oldClass < 0) {
            SMemFree(ptr, s_file, 0x13E, 0);
        } else {
            ClassFree(pool->m_classes[oldClass], ptr);
        }
    }
    return block;
}
