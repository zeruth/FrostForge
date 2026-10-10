#include "world/SceneOcclusion.hpp"

#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>

namespace {

const char s_meshTag[] = ".?AUDeferredOcclusionMeshRender@CSceneOcclusionInt@@";
const char s_occlusionFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\SceneOcclusion\\SceneOcclusion.cpp";

void StoreU64(uint8_t* base, size_t offset, uint64_t value) {
    std::memcpy(base + offset, &value, 8);
}

void StoreU32(uint8_t* base, size_t offset, uint32_t value) {
    std::memcpy(base + offset, &value, 4);
}

// An identity matrix written as the reference writes it: the diagonal ones as the low halves
// of 8-byte stores, every other word zero.
void StoreIdentity(uint8_t* base, size_t offset) {
    for (int32_t i = 0; i < 16; i++) {
        StoreU32(base, offset + i * 4, (i % 5) == 0 ? 0x3f800000 : 0);
    }
}

}  // namespace

// ref: FUN_142553b50
void SceneOcclusionRasterInit(uint8_t* raster) {
    StoreU32(raster, 0x47c, 0x1f);
    StoreU64(raster, 0x00, 0);
    StoreU32(raster, 0x1a4, 0);
    raster[0x1b8] = 0;
    raster[0x1b9] = 0;
    std::memset(raster + 0x10, 0, 0xc0);
}

// ref: FUN_142551810
void SceneOcclusionBufferInit(uint8_t* buffer) {
    for (size_t plane = 0x04; plane < 0x64; plane += 0x10) {
        StoreU64(buffer, plane, 0);
        StoreU64(buffer, plane + 8, 0x3f800000);
    }
    std::memset(buffer + 0x64, 0, 0x60);
    StoreIdentity(buffer, 0xc4);
    StoreU32(buffer, 0x104, 0);
    StoreU32(buffer, 0x108, 0);
    for (size_t axis = 0x10c; axis < 0x16c; axis += 0x10) {
        StoreU64(buffer, axis, 0x3f800000);
        StoreU64(buffer, axis + 8, 0);
    }
    std::memset(buffer + 0x16c, 0, 0x58);
    StoreIdentity(buffer, 0x1c4);
    StoreIdentity(buffer, 0x204);
    StoreU64(buffer, 0x250, 0);
    SceneOcclusionRasterInit(buffer + 0x260);
    StoreU64(buffer, 0x6e0, 0);
    StoreU64(buffer, 0x6e8, 0);
    StoreU64(buffer, 0x6f0, 0);
}

// ref: FUN_142551700
SceneOcclusion::SceneOcclusion(SceneOcclusion** holder) {
    StoreU64(this->m_data, 0xa0, 0);
    StoreU64(this->m_data, 0xa8, 0);
    StoreU32(this->m_data, 0xb0, 0);
    StoreU64(this->m_data, 0xbc, 0);
    StoreU32(this->m_data, 0xc4, 0);
    StoreIdentity(this->m_data, 0xc8);
    for (uint32_t i = 0; i < BUFFER_COUNT; i++) {
        SceneOcclusionBufferInit(this->m_data + 0x120 + i * BUFFER_SIZE);
    }
    StoreU64(this->m_data, 0x7128, 0);
    OsMutexInit(&this->Field<OSMUTEX>(0x7138));
    this->Field<SceneOcclusion**>(0x7150) = holder;
    StoreU64(this->m_data, 0x7140, 0);
    StoreU64(this->m_data, 0x7148, 0);
    StoreU32(this->m_data, 0x7120, 0);
    StoreU32(this->m_data, 0x7124, 0);
}

// ref: FUN_142552520
void SceneOcclusion::Reset() {
    std::memset(this->m_data, 0, 0x88);
    StoreU64(this->m_data, 0x7128, 0);
    StoreU64(this->m_data, 0x7130, 0);
    StoreU64(this->m_data, 0x118, 0);
    this->Field<THREADPOOLGROUP*>(0x7140) = ThreadPoolGroupCreate(2);
    this->Field<THREADPOOLGROUP*>(0x7148) = ThreadPoolGroupCreate(1);
    StoreU32(this->m_data, 0x80, 0);
}

// ref: FUN_14254fe70
void SceneOcclusionCreate(SceneOcclusion** holder) {
    void* memory = SMemAlloc(sizeof(SceneOcclusion), 1, s_occlusionFile, SStrHashFnv1a(s_occlusionFile), 0x18, 0);
    auto occlusion = new (memory) SceneOcclusion(holder);
    *holder = occlusion;
    occlusion->Reset();
}

// ref: FUN_14254fe20
uint32_t SceneOcclusionGetState(SceneOcclusion** holder) {
    return (*holder)->Field<uint32_t>(0x7120);
}

// ref: FUN_142551ab0
void SceneOcclusionShutdown(SceneOcclusion* occlusion) {
    (void)occlusion;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14254fd60
void SceneOcclusionDestroy(SceneOcclusion** holder) {
    SceneOcclusionShutdown(*holder);
    SceneOcclusion* occlusion = *holder;
    if (occlusion != nullptr) {
        for (uint32_t i = SceneOcclusion::BUFFER_COUNT; i-- > 0;) {
            uint8_t* meshes = occlusion->m_data + 0x120 + i * SceneOcclusion::BUFFER_SIZE + 0x6e0;
            if (*reinterpret_cast<int64_t*>(meshes + 0x10) >= 0) {
                SMemFree(*reinterpret_cast<void**>(meshes), s_meshTag, -6, 0);
                *reinterpret_cast<void**>(meshes) = nullptr;
            }
        }
        operator delete(occlusion);
    }
    *holder = nullptr;
}
