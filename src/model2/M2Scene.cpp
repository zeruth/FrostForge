#include "model2/M2Scene.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "base/Matrix.hpp"
#include "cas/CAS.hpp"
#include "fileservices/AsyncFile.hpp"
#include "model2/M2Init.hpp"
#include "base/Status.hpp"
#include "client/RenderSettings.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/Gx.hpp"
#include "model2/M2Shared.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

// Engine/Source/Model2/M2Scene.cpp.

namespace {

// The array's tag (DAT_142da1b90).
const char* const s_drawParamsName = "AUBaseModelDrawParams";

// A growable array of draw parameters: data, count, capacity (top bit set while the data is
// not owned).
struct DRAWPARAMSARRAY {
    BaseModelDrawParams* m_data;
    uint64_t m_count;
    uint64_t m_capacity;
};

const char* const s_sceneFile = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Scene.cpp";
const char* const s_cacheFile = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Cache.cpp";
const char* const s_particleFile = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\ParticleSystem\\ParticleSystem2.cpp";
const char* const s_newFile = "D:\\BuildServer\\A\\work-git\\wow\\Storm\\H\\SMemNew.h";
// DAT_142da2080, DAT_142da20f8, DAT_142d61348.
const char* const s_pipelinePairName = ".?AU?$pair@$$CBUKey@PipelineCache@Model3@@V?$unique_ptr@UGxPipeline@@UGxResourceDelete@@@blz@@@blz@@";
const char* const s_hitRecName = ".?AUM2HitRec@@";
const char* const s_indexName = ".?I";

// Grows a vector to at least `count`: half again its capacity, or `count` when that is more.
uint64_t GrowCapacity(uint64_t capacityField, uint64_t count) {
    uint64_t grown = (capacityField >> 1 & 0x3fffffffffffffffull) + (capacityField & 0x7fffffffffffffffull);
    return grown < count ? count : grown;
}

// ref: FUN_1411e53d0
void M2HitRecsReserve(M2SceneVector<uint8_t>* recs, uint64_t count) {
    uint64_t field = recs->m_capacity;
    if ((field & 0x7fffffffffffffffull) >= count) {
        return;
    }
    uint64_t capacity = GrowCapacity(field, count);
    recs->m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
    auto data = static_cast<uint8_t*>(SMemAlloc(capacity * 0x28, 0xb, s_hitRecName, SStrHashFnv1a(s_hitRecName), -6, 0));
    if (recs->m_size != 0) {
        std::memcpy(data, recs->m_data, recs->m_size * 0x28);
    }
    if (static_cast<int64_t>(recs->m_capacity) >= 0) {
        SMemFree(recs->m_data, s_hitRecName, -6, 0);
    }
    recs->m_capacity &= 0x7fffffffffffffffull;
    recs->m_data = data;
}

// The 0x88-byte entries: FUN_1411e4920 allocates, FUN_1411cf470 moves them over.
// ref: FUN_1411e5510
void M2SceneEntriesReserve(M2SceneVector<uint8_t>* entries, uint64_t count) {
    if ((entries->m_capacity & 0x7fffffffffffffffull) >= count) {
        return;
    }
    uint64_t field = entries->m_capacity;
    uint64_t capacity = GrowCapacity(field, count);
    entries->m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
    auto data = static_cast<uint8_t*>(SMemAlloc(capacity * 0x88, 0x12, s_newFile, SStrHashFnv1a(s_newFile), 0xc6, 0));
    if (entries->m_size != 0) {
        std::memcpy(data, entries->m_data, entries->m_size * 0x88);
    }
    if (static_cast<int64_t>(entries->m_capacity) >= 0) {
        SMemFree(entries->m_data, s_newFile, 0xca, 0);
    }
    entries->m_capacity &= 0x7fffffffffffffffull;
    entries->m_data = data;
}

// FUN_1411e4860 allocates; the caller copies and frees.
void M2SceneIndicesReserve(M2SceneVector<uint32_t>* indices, uint64_t count) {
    if ((indices->m_capacity & 0x7fffffffffffffffull) >= count) {
        return;
    }
    uint64_t field = indices->m_capacity;
    uint64_t capacity = GrowCapacity(field, count);
    indices->m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
    auto data = static_cast<uint32_t*>(SMemAlloc(capacity * 4, 0xb, s_indexName, SStrHashFnv1a(s_indexName), -6, 0));
    for (uint64_t i = 0; i < indices->m_size; i++) {
        data[i] = indices->m_data[i];
    }
    if (static_cast<int64_t>(indices->m_capacity) >= 0) {
        SMemFree(indices->m_data, s_indexName, -6, 0);
    }
    indices->m_capacity &= 0x7fffffffffffffffull;
    indices->m_data = data;
}

// Inlined in FUN_1411ddca0.
void M2SceneDrawParamsReserve(M2SceneVector<BaseModelDrawParams>* params, uint64_t count) {
    uint64_t field = params->m_capacity;
    if ((field & 0x7fffffffffffffffull) >= count) {
        return;
    }
    uint64_t capacity = GrowCapacity(field, count);
    params->m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
    auto data = static_cast<BaseModelDrawParams*>(SMemAlloc(capacity << 5, 0xb, s_drawParamsName, SStrHashFnv1a(s_drawParamsName), -6, 0));
    for (uint64_t i = 0; i < params->m_size; i++) {
        data[i] = params->m_data[i];
    }
    if (static_cast<int64_t>(params->m_capacity) >= 0) {
        SMemFree(params->m_data, s_drawParamsName, -6, 0);
    }
    params->m_capacity &= 0x7fffffffffffffffull;
    params->m_data = data;
}

const uint32_t s_chunkMd20 = 0x3032444d;  // 'MD20'
const uint32_t s_chunkMd21 = 0x3132444d;  // 'MD21'
const uint32_t s_chunkTxid = 0x44495854;  // 'TXID'

// A chunk's header: its tag and its size.
struct M2FILECHUNK {
    uint32_t m_tag;
    uint32_t m_size;
};

// Whether an open model file's textures are all on disk: the model's data (the whole file
// for a bare MD20, the MD21 chunk otherwise) and the TXID chunk after it, each texture's
// residency checked and the partly resident ones fetched. A texture that is not known at all
// fails at once (and, as in the reference, leaves the TXID copy unfreed).
// ref: FUN_1411e41d0
bool ModelFileResident(cas::CASFILE* file) {
    void* data = nullptr;
    uint32_t size = 0;
    M2FILECHUNK chunk;
    uint32_t read = 0;
    while (cas::CASFileReadEx(file, &chunk, 8, &read, 0, -1)) {
        size = chunk.m_size;
        void* buffer = data;
        if (chunk.m_tag == s_chunkMd20 || chunk.m_tag == s_chunkMd21) {
            if (chunk.m_tag == s_chunkMd20) {
                cas::CASFileSetPosition(file, 0, 0);
                size = static_cast<uint32_t>(cas::CASFileGetSize(file));
            }
            buffer = M2AlignedAlloc(size, s_sceneFile, 0x6bf, 0x12, 0);
            if (data != nullptr) {
                M2FreeFileData(data);
            }
            if (!cas::CASFileReadEx(file, buffer, size, &read, 0, -1)) {
                if (buffer != nullptr) {
                    M2FreeFileData(buffer);
                }
                data = nullptr;
                break;
            }
        } else {
            cas::CASFileSetPosition(file, size, 1);
        }
        data = buffer;
        if (buffer != nullptr) {
            break;
        }
    }
    int32_t* textureIds = nullptr;
    read = 1;
    for (;;) {
        if (read == 0 || !cas::CASFileReadEx(file, &chunk, 8, &read, 0, -1)) {
            break;
        }
        if (chunk.m_tag == s_chunkTxid) {
            auto ids = static_cast<int32_t*>(
                SMemAlloc(chunk.m_size, 0x12, s_sceneFile, SStrHashFnv1a(s_sceneFile), 0x6df, 0));
            if (!cas::CASFileReadEx(file, ids, chunk.m_size, &read, 0, -1)) {
                break;
            }
            textureIds = ids;
            break;
        }
        cas::CASFileSetPosition(file, chunk.m_size, 1);
    }
    cas::CASFileClose(&file);
    M2INITCONTEXT context;
    bool resident = false;
    if (M2InitAll(data, size, static_cast<M2Data*>(data), &context)) {
        resident = true;
        uint16_t count = *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(data) + 0x50);
        for (uint32_t i = 0; i < count; i++) {
            if (textureIds == nullptr) {
                continue;
            }
            uint8_t residency = cas::CASFileGetResidency(textureIds[i], 0, 0);
            if (residency == 0) {
                M2FreeFileData(data);
                return false;
            }
            if (residency != 2) {
                AsyncFilePrefetchById(textureIds[i]);
                resident = false;
            }
        }
        if (textureIds != nullptr) {
            SMemFree(textureIds, s_sceneFile, 0x74a, 0);
        }
    }
    if (data != nullptr) {
        M2FreeFileData(data);
    }
    return resident;
}

}  // namespace

// ref: FUN_1411ace60
BaseModelDrawParams* M2SceneAddDrawParams(void* array) {
    auto params = static_cast<DRAWPARAMSARRAY*>(array);
    uint64_t capacityField = params->m_capacity;
    uint64_t count = params->m_count;
    uint64_t capacity = capacityField & 0x7fffffffffffffffull;
    if (count == capacity && capacity < capacity + 1) {
        uint64_t grow = capacityField >> 1 & 0x3fffffffffffffffull;
        uint64_t step = 1;
        if (capacity + 1 <= capacity + grow) {
            step = grow;
        }
        uint64_t newCapacity = step + capacity;
        params->m_capacity = ((newCapacity ^ capacityField) & 0x8000000000000000ull) ^ newCapacity;
        auto data = static_cast<BaseModelDrawParams*>(SMemAlloc(newCapacity * sizeof(BaseModelDrawParams), 0xb, s_drawParamsName, 0, -6, 0));
        BaseModelDrawParams* old = params->m_data;
        count = params->m_count;
        if (count != 0) {
            std::memcpy(data, old, count * sizeof(BaseModelDrawParams));
            old = params->m_data;
            count = params->m_count;
        }
        if (static_cast<int64_t>(params->m_capacity) >= 0) {
            SMemFree(old, s_drawParamsName, -6, 0);
            count = params->m_count;
        }
        params->m_capacity &= 0x7fffffffffffffffull;
        params->m_data = data;
    }
    BaseModelDrawParams* entry = &params->m_data[count];
    std::memset(entry, 0, sizeof(BaseModelDrawParams));
    params->m_count = params->m_count + 1;
    return entry;
}

// A box (min, max) against six planes: 0 when it is wholly outside one of them, else 3.
// ref: FUN_141346830
int32_t M2FrustumTestBox(const float* planes, const float* box) {
    const float* corners[2] = {box + 3, box};
    for (int32_t i = 0; i < 6; i++) {
        const float* plane = planes + i * 4;
        float distance = corners[std::signbit(plane[0]) ? 1 : 0][0] * plane[0]
            + corners[std::signbit(plane[1]) ? 1 : 0][1] * plane[1]
            + corners[std::signbit(plane[2]) ? 1 : 0][2] * plane[2] + plane[3];
        if (distance < -0.019444413f) {  // DAT_142734910
            return 0;
        }
    }
    return 3;
}

// ref: FUN_1411df990
void M2SceneGatherLights(CM2Scene* scene, CM2ModelLighting* lighting) {
    for (CM2Lighting* light = scene->m_directionalLights; light != nullptr; light = light->m_next) {
        M2ModelLightingAdd(lighting, light);
    }
    for (CM2Lighting* light = scene->m_pointLights; light != nullptr;) {
        CM2Lighting* next = light->m_next;
        M2ModelLightingAdd(lighting, light);
        light = next;
    }
}

// ref: FUN_1411ddad0
void* M2SceneGetRayTracing(CM2Scene* scene) {
    return scene->m_rayTracing;
}

// The ray tracing is never made yet: with it null, nothing registers.
extern "C" unsigned long long FUN_141260140(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_141260140
void RayTracingRegisterModel(void* rayTracing, void* entry, uint32_t size) {
    __try {
        FUN_141260140((unsigned long long)rayTracing, (unsigned long long)entry, (unsigned long long)size);
    } __except (GenFault(0x141260140ull, GetExceptionInformation())) {
    }
}

// The ray tracing object is never made yet, so these are not reached.
extern "C" unsigned long long FUN_14125fb10(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_14125fb10
void RayTracingUpdateInstance(void* rayTracing, const int32_t* entry, const float* matrix) {
    __try {
        FUN_14125fb10((unsigned long long)rayTracing, (unsigned long long)entry, (unsigned long long)matrix);
    } __except (GenFault(0x14125fb10ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14125fab0(unsigned long long, unsigned long long);
// translated: FUN_14125fab0
void RayTracingRemoveInstance(void* rayTracing, const int32_t* entry) {
    __try {
        FUN_14125fab0((unsigned long long)rayTracing, (unsigned long long)entry);
    } __except (GenFault(0x14125fab0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14125ffb0(unsigned long long, unsigned long long);
// translated: FUN_14125ffb0
void RayTracingRemoveModel(void* rayTracing, const int32_t* entry) {
    __try {
        FUN_14125ffb0((unsigned long long)rayTracing, (unsigned long long)entry);
    } __except (GenFault(0x14125ffb0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_14125ff50
void RayTracingMarkDirty(void* rayTracing, const int32_t* entry) {
    int32_t index = entry[0];
    auto base = static_cast<uint8_t*>(rayTracing);
    if (index == -1 || entry[1] != 2 || *reinterpret_cast<int32_t*>(base + 0x68) <= index) {
        return;
    }
    uint8_t* instance = (*reinterpret_cast<uint8_t***>(base + 0x60))[index];
    if (instance != nullptr) {
        instance[0x80] |= 1;
    }
}

// ref: FUN_1411d0040
M2PipelineCache::M2PipelineCache() {
    this->m_bucketCount = 8;
    this->m_count = 0;
    this->m_deleted = 0;
    this->m_buckets = static_cast<M3PIPELINEENTRY*>(SMemAlloc(0x4c0, 0xb, s_pipelinePairName, SStrHashFnv1a(s_pipelinePairName), -6, 0));
    for (uint64_t i = 0; i < this->m_bucketCount; i++) {
        this->m_buckets[i].m_state = 1;  // FUN_141205ae0: empty
    }
    this->m_maxLoad = 0.75f;
    this->m_blocks = nullptr;
    this->m_blockCount = 0;
    this->m_first = 0;
    this->m_size = 0;
    OsMutexInit(&this->m_lock);
    this->m_maxLoad = 0.5f;
}

// ref: FUN_1411d0130
M2SceneDrawList::M2SceneDrawList() {
    this->m_elements = {};
    for (uint32_t i = 0; i < 0xe; i++) {
        this->m_lists[i] = {};
        this->m_lists2[i] = {};
    }
    this->m_callbacks = {};
    std::memset(this->m_screenRect, 0, sizeof(this->m_screenRect));
}

// ref: FUN_14124c1f0
void M2ParticlePoolInit(M2ParticlePool* pool, int32_t particles) {
    pool->m_bytes = particles * 0x1c0;
    pool->m_data = static_cast<uint8_t*>(SMemAlloc(static_cast<uint64_t>(particles * 0x1c0), 0x15, s_particleFile,
        SStrHashFnv1a(s_particleFile), 0xe13, 0));
    InterlockedExchange(reinterpret_cast<volatile long*>(&pool->m_used), 0);
}

CM2Scene::CM2Scene(M2CACHE* cache, int32_t type) {
    this->m_refCount = 1;
    this->m_cacheLink.m_prevlink = nullptr;
    this->m_cacheLink.m_next = nullptr;
    this->m_owner = nullptr;
    this->m_030 = nullptr;
    this->m_038 = nullptr;
    this->m_cache = cache;
    void* memory = SMemAlloc(sizeof(M2PipelineCache), 1, s_sceneFile, SStrHashFnv1a(s_sceneFile), 0x221, 0);
    this->m_pipelineCacheA = new (memory) M2PipelineCache();
    memory = SMemAlloc(sizeof(M2PipelineCache), 1, s_sceneFile, SStrHashFnv1a(s_sceneFile), 0x222, 0);
    this->m_pipelineCacheB = new (memory) M2PipelineCache();
    auto map = static_cast<M2SceneLockedMap*>(SMemAlloc(sizeof(M2SceneLockedMap), 1, s_sceneFile, SStrHashFnv1a(s_sceneFile), 0x223, 0));
    OsMutexInit(&map->m_lock);
    for (uint32_t i = 0; i < 8; i++) {
        map->m_08[i] = 0;
    }
    map->m_bucketCount = 8;
    map->m_count = 0;
    map->m_buckets = nullptr;
    map->m_maxLoad = 1.0f;
    this->m_lockedMap = map;
    this->m_mapBuckets = 8;
    this->m_mapCount = 0;
    this->m_mapData = nullptr;
    this->m_mapLoad = 1.0f;
    this->m_models = nullptr;
    this->m_released = nullptr;
    this->m_088 = nullptr;
    this->m_090 = nullptr;
    this->m_time = 300000;
    this->m_timeStep = 0;
    this->m_frame = 0;
    this->m_0a4 = 1;
    this->m_viewFrame = 0;
    this->m_0ac = 0;
    this->m_flags = 0;
    this->m_0b4 = 0.0f;
    this->m_directionalLights = nullptr;
    this->m_pointLights = nullptr;
    this->m_maxLights = 8;
    OsMutexInit(&this->m_lock0d0);
    this->m_crit0d8.Initialize();
    for (uint32_t i = 0; i < 5; i++) {
        OsMutexInit(&this->m_locks100[i]);
    }
    for (uint32_t i = 0; i < 0x40; i++) {
        this->m_drawParams[i] = {};
        this->m_drawParams2[i] = {};
    }
    this->m_particles.m_bytes = 0;
    this->m_particles.m_data = nullptr;
    this->m_particles.m_used = 0;
    new (&this->m_sharedLock) blz::shared_mutex();
    this->m_f60 = 0;
    this->m_f68 = 0;
    this->m_f70 = 0;
    this->m_f78 = {};
    for (uint32_t i = 0; i < 0xe; i++) {
        this->m_f90[i] = {};
        this->m_10e0[i] = {};
        this->m_1230[i] = {};
    }
    for (uint32_t i = 0; i < 0x41; i++) {
        new (&this->m_drawLists[i]) M2SceneDrawList();
    }
    MatrixIdentity(this->m_camera.m);
    MatrixIdentity(this->m_inverseView.m);
    MatrixIdentity(this->m_d4f8.m);
    std::memset(&this->m_cameraPosition, 0, 0xd55c - 0xd538);
    this->m_d568[0] = 0.0f;
    this->m_d568[1] = 0.0f;
    this->m_d568[2] = 1.0f;  // overwritten below with the identity rotation
    this->m_d568[3] = 0.0f;
    MatrixIdentity(this->m_drawView.m);
    MatrixIdentity(this->m_drawInverseView.m);
    MatrixIdentity(this->m_drawProjection.m);
    this->m_drawCameraPosition = C3Vector {};
    std::memset(this->m_d644, 0, sizeof(this->m_d644));
    std::memset(this->m_d654, 0, sizeof(this->m_d654));
    std::memset(this->m_screenRect, 0, sizeof(this->m_screenRect));
    this->m_animateRatio = 10000.0f;
    this->m_d678 = nullptr;
    this->m_d680 = nullptr;
    this->m_decalCallback = nullptr;
    this->m_decalParam = nullptr;
    this->m_traceCallback = nullptr;
    this->m_d6a0 = nullptr;
    this->m_windCallback = nullptr;
    this->m_d6b0 = 0;
    this->m_projectCallback = nullptr;
    this->m_projectCheckCallback = nullptr;
    this->m_hitRecs = {};
    this->m_d6e0 = 0;
    this->m_d6e4 = 0;
    this->m_d6e8 = 0;
    std::memset(this->m_d6f0, 0, 0xd70c - 0xd6f0);
    this->m_d738 = 0;
    this->m_d73c = 0;
    this->m_defaultTexture = nullptr;
    this->m_effectLitOpaque = nullptr;
    this->m_effectLitTransparent = nullptr;
    this->m_effectLitNoAlpha = nullptr;
    this->m_effectShadowMonk = nullptr;
    this->m_effectEdgeGlow = nullptr;
    this->m_effectAvatar = nullptr;
    this->m_effectProcedural = nullptr;
    this->m_effectFlipbook = nullptr;
    this->m_effectPrepass = nullptr;
    this->m_rayTracing = nullptr;
    for (uint32_t i = 0; i < 0xe; i++) {
        this->m_d798[i] = {};
    }
    std::memset(this->m_d8e8, 0, sizeof(this->m_d8e8));
    this->m_threadGroupA = nullptr;
    this->m_db18 = 0;
    this->m_bucketCounter = 0;
    this->m_threadGroupB = nullptr;
    this->m_collectDrawParams = true;
    this->m_bucketModels = false;
    this->m_daf0 = 0;
    this->m_daf8 = 0;
    this->m_db00 = 0;
    this->m_db08 = 6;
    this->m_db0c = 0;
    this->m_threadedBuild = 0;
    this->m_db21 = 0;
    this->m_db28 = 0;
    this->m_db30 = 0;
    for (uint32_t i = 0; i < 0x40; i++) {
        this->m_buckets[i] = nullptr;
    }
    M2HitRecsReserve(&this->m_hitRecs, 0x200);
    this->m_threadGroupB = ThreadPoolGroupCreate(2);
    this->m_threadGroupA = ThreadPoolGroupCreate(2);
    M2ParticlePoolInit(&this->m_particles, type != 6 ? 500 : 25000);
    // DAT_14261aa10: the identity rotation.
    this->m_d568[0] = 0.0f;
    this->m_d568[1] = 0.0f;
    this->m_d568[2] = 0.0f;
    this->m_d568[3] = 1.0f;
    this->m_frustum = nullptr;
}

// ref: FUN_1411df730
void CM2Scene::Reserve(uint32_t count) {
    if (count < 2) {
        count = 2;
    }
    M2SceneEntriesReserve(reinterpret_cast<M2SceneVector<uint8_t>*>(&this->m_f78), count);
    uint64_t half = count >> 1;
    for (uint32_t i = 0; i < 0xe; i++) {
        M2SceneIndicesReserve(&this->m_f90[i], half);
        M2SceneIndicesReserve(&this->m_1230[i], half);
    }
}

uint64_t g_m2DepthStencil[8][8];
uint32_t g_m2OutlineStencil[3];
uint32_t g_m2OutlineStencilFill[3];
bool g_m2OutlineEnabled;

// ref: FUN_1411d9090
void M2SceneStatesInitialize() {
    // The depth flags of each pass (the first word of each pair on the stack).
    static const uint8_t s_passFlags[8] = { 0x15, 0x11, 0xd, 9, 0x38, 0x15, 0x19, 5 };
    static const uint8_t s_stencilMasks[4] = { 4, 0, 0, 1 };  // DAT_142cfc378
    for (uint32_t pass = 0; pass < 8; pass++) {
        uint8_t flags = s_passFlags[pass] & 0xfd;
        for (uint32_t i = 0; i < 4; i++) {
            uint64_t mask = s_stencilMasks[i];
            uint64_t first = pass == 5 ? 2 : 0;
            g_m2DepthStencil[pass][i * 2] = 0x0200000700000000ull | mask << 0x18 | 0xff0000ull | (flags | first);
            uint64_t second = (pass == 5 || pass != 1) ? 2 : 0;
            g_m2DepthStencil[pass][i * 2 + 1] = 0x0200000700000000ull | mask << 0x18 | 0xff0000ull | (flags | second);
        }
    }
    for (uint32_t i = 0; i < 3; i++) {
        g_m2OutlineStencil[i] = 0x10000 | i;
        g_m2OutlineStencilFill[i] = 0x30000 | i;
    }
    // DAT_142cfb1c0, cleared here.
}

// ref: FUN_1411dd600
void M2SceneSetOutline(bool enabled) {
    if (g_m2OutlineEnabled != enabled) {
        if (enabled) {
            RenderSettingsAddOutline();
            g_m2OutlineEnabled = true;
            return;
        }
        RenderSettingsRemoveOutline();
        g_m2OutlineEnabled = false;
    }
}

// ref: FUN_1411d9280
void M2SceneClearOutline() {
    if (g_m2OutlineEnabled) {
        RenderSettingsRemoveOutline();
        g_m2OutlineEnabled = false;
    }
}

// ref: FUN_1411d8ee0
void CM2Scene::RecreateDeviceObjects() {
    for (CM2BaseModel* model = this->m_models; model != nullptr; model = model->m_sceneNext) {
        if (model->m_type == 0) {
            static_cast<CM2Model*>(model)->m_shared->RefreshBatchEffects();
        } else if (model->m_type == 3) {
            FROST_UNIMPLEMENTED();  // FUN_1411ece70: an M3 model's effects (Model3 is not ported)
        }
        model->RefreshBatchEffects();
    }
    CStatus status;
    this->m_defaultTexture = TextureCreate(0x6dbd7, 0x82001, &status, 0, 1, 0);
    this->m_effectLitTransparent = ShaderEffectFind("LitSphere_Additive_Transparent");
    this->m_effectLitOpaque = ShaderEffectFind("LitSphere_Additive_Opaque");
    this->m_effectLitNoAlpha = ShaderEffectFind("LitSphere_Additive_Transparent_NoAlpha");
    this->m_effectAvatar = ShaderEffectFind("Avatar");
    this->m_effectProcedural = ShaderEffectFind("Procedural");
    if (this->m_effectProcedural == nullptr) {
        this->m_effectProcedural = ShaderEffectCreate("Procedural", 0x7c, 0xbf);
    }
    this->m_effectFlipbook = ShaderEffectFind("FlipbookImpostor");
    if (this->m_effectFlipbook == nullptr) {
        this->m_effectFlipbook = ShaderEffectCreate("FlipbookImpostor", 0x42, 0x75);
    }
    this->m_effectShadowMonk = ShaderEffectFind("ShadowMonk");
    this->m_effectEdgeGlow = ShaderEffectFind("EdgeGlow");
    this->m_effectPrepass = ShaderEffectFind("Model2PrepassOpaque");
}

// ref: FUN_1411d92a0
void CM2Scene::ReleaseDeviceObjects() {
    for (CM2BaseModel* model = this->m_models; model != nullptr; model = model->m_sceneNext) {
        if (model->m_type == 0) {
            static_cast<CM2Model*>(model)->m_shared->ReleaseGpuBuffers();
        } else if (model->m_type == 3) {
            FROST_UNIMPLEMENTED();  // FUN_1411ece80
        }
        model->ReleaseGpuData();
    }
    for (CM2BaseModel* model = this->m_released; model != nullptr; model = model->m_sceneNext) {
        if (model->m_type == 0) {
            static_cast<CM2Model*>(model)->m_shared->ReleaseGpuBuffers();
        }
        model->ReleaseGpuData();
    }
    TextureFree(reinterpret_cast<HTEXTURE*>(&this->m_defaultTexture));
    this->m_pipelineCacheA->Clear();
    this->m_pipelineCacheB->Clear();
}

// ref: FUN_1411d8c80
CM2Model* CM2Scene::CreateModel(int32_t fileDataId, uint32_t flags, uint8_t priority, uint8_t lod) {
    this->m_crit0d8.Enter();
    CM2Shared* shared = M2CacheGetShared(this->m_cache, fileDataId, nullptr, flags, priority);  // FUN_141196280
    CM2Model* model = nullptr;
    if (shared != nullptr) {
        model = M2ModelAlloc(flags);
        if (((this->m_type - 1) & ~4u) == 0) {
            flags |= 0x2000;
        }
        if (model != nullptr && !model->Initialize(this, shared, nullptr, lod, flags)) {
            M2ModelFree(model);
            model = nullptr;
        }
        shared->ReleaseModelRef();
    }
    this->m_crit0d8.Leave();
    return model;
}

// ref: FUN_1411d8d50
CM2Model* CM2Scene::CreateModel(const char* name, uint32_t flags, uint8_t priority, uint8_t lod) {
    if (name == nullptr || *name == '\0') {
        return nullptr;
    }
    this->m_crit0d8.Enter();
    CM2Shared* shared = M2CacheGetSharedByName(this->m_cache, name, flags, priority);
    CM2Model* model = nullptr;
    if (shared != nullptr) {
        model = M2ModelAlloc(flags);
        if (((this->m_type - 1) & ~4u) == 0) {
            flags |= 0x2000;
        }
        if (!model->Initialize(this, shared, nullptr, lod, flags)) {
            M2ModelFree(model);
            model = nullptr;
        }
        shared->ReleaseModelRef();
    }
    this->m_crit0d8.Leave();
    return model;
}

// ref: FUN_1411e0f30
void CM2Scene::SetView(const C3Vector* cameraPosition, const C44Matrix* view, const C44Matrix* projection,
    const float* viewportMin, const float* viewportMax, bool advance) {
    if (this->m_0a4 != 0 && advance) {
        this->m_frame++;
    }
    this->m_camera = *view;
    this->m_d4f8 = *projection;
    std::memcpy(this->m_viewportMin, viewportMin, sizeof(this->m_viewportMin));
    std::memcpy(this->m_viewportMax, viewportMax, sizeof(this->m_viewportMax));
    C3Vector back = { -cameraPosition->x, -cameraPosition->y, -cameraPosition->z };
    MatrixTranslate(this->m_camera.m, &back.x);
    float determinant = MatrixDeterminant(this->m_camera.m);
    C44Matrix inverse;
    MatrixInverse(this->m_camera.m, inverse.m, determinant);
    this->m_inverseView = inverse;
    this->m_cameraPosition = *cameraPosition;
    this->m_viewFrame = this->m_frame;
}

// ref: FUN_1411df6e0
void CM2Scene::OnRenderSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current) {
    for (CM2BaseModel* model = this->m_models; model != nullptr; model = model->m_sceneNext) {
        model->OnRenderSettingsChanged(previous, current);
    }
}

// ref: FUN_1411d9360
void M2PipelineCache::Clear() {
    for (uint64_t at = this->m_first; at != this->m_first + this->m_size; at++) {
        GxPipeline* pipeline = this->m_blocks[(at >> 3) & (this->m_blockCount - 1)][at & 7].m_pipeline;
        if (pipeline != nullptr) {
            GxPipelineDestroy(&pipeline);
        }
    }
    this->m_first = 0;
    this->m_size = 0;
    for (uint64_t i = 0; i < this->m_bucketCount; i++) {
        M3PIPELINEENTRY* entry = &this->m_buckets[i];
        if (entry->m_state != 1 && entry->m_state != 2 && entry->m_pipeline != nullptr) {
            GxPipeline* pipeline = entry->m_pipeline;
            GxPipelineDestroy(&pipeline);
        }
        entry->m_state = 1;
    }
    this->m_deleted = 0;
    this->m_count = 0;
}

// ref: FUN_1411ddca0
void CM2Scene::Initialize(int32_t type, void* owner, void* a4, void* a5, bool renderer) {
    this->m_038 = a5;
    this->m_owner = owner;
    this->m_030 = a4;
    this->m_type = type;
    uint64_t perPass;
    uint64_t perPass2;
    if (type == 6) {
        this->Reserve(20000);
        perPass = 0x32;
        this->m_bucketModels = true;
        perPass2 = 0x19;
    } else {
        this->Reserve(0x100);
        perPass = 10;
        perPass2 = 5;
    }
    for (uint32_t i = 0; i < 0x40; i++) {
        M2SceneDrawParamsReserve(&this->m_drawParams[i], perPass);
    }
    for (uint32_t i = 0; i < 0x40; i++) {
        M2SceneDrawParamsReserve(&this->m_drawParams2[i], perPass2);
    }
    if (renderer) {
        this->RecreateDeviceObjects();  // inlined here in the reference
    }
    int32_t kind = ((this->m_type - 1) & ~4) == 0 ? 1 : 0;
    if (this->m_type == 8) {
        kind = 2;
    } else if (this->m_type == 6) {
        kind = 3;
    }
    this->m_rayTracing = PhysicsWorldCreate(kind);
}

// ref: FUN_141196e40
CM2Scene* M2CacheCreateScene(int32_t type, void* owner, void* a3, void* a4) {
    void* memory = SMemAlloc(sizeof(CM2Scene), 0x12, s_cacheFile, SStrHashFnv1a(s_cacheFile), 0x735, 0);
    auto scene = new (memory) CM2Scene(&g_m2Cache, type);
    bool renderer = GxDevExists() && (g_m2Cache.m_flags & 0x800) == 0;
    scene->Initialize(type, owner, a3, a4, renderer);
    g_m2Cache.m_scenes.LinkToHead(scene);
    return scene;
}

// The world (Physics.cpp, 0x98 bytes, FUN_14125d7a0), a slot in the physics engine's own world
// table (FUN_1424681d0, 0x850-byte worlds at 0x142463ae0 and on), kept in the list at
// DAT_1430a1ff0: the reference's, translated. A null world is what it returns for kind 0 and on
// a CPU without SSE2, and every user of the scene's world checks for it.
extern "C" unsigned long long FUN_14125deb0(unsigned long long);
// translated: FUN_14125deb0
void* PhysicsWorldCreate(int32_t kind) {
    if (kind == 0) {
        return nullptr;
    }
    __try {
        return reinterpret_cast<void*>(FUN_14125deb0(static_cast<uint32_t>(kind)));
    } __except (GenFault(0x14125deb0ull, GetExceptionInformation())) {
        return nullptr;
    }
}

// ref: FUN_1411e0c60
void M2SceneSetProjectCallback(CM2Scene* scene, void* (*callback)(void* query)) {
    scene->m_projectCallback = callback;
}

// ref: FUN_1411e0c70
void M2SceneSetProjectCheckCallback(CM2Scene* scene, void (*callback)(void* draw)) {
    scene->m_projectCheckCallback = reinterpret_cast<void*>(callback);
}

// ref: FUN_1411e0c30
void M2SceneSetTraceCallback(CM2Scene* scene, void (*callback)(void* query, float* hit, float* t), void* param) {
    scene->m_traceCallback = callback;
    scene->m_d6a0 = callback != nullptr ? param : nullptr;
}

// ref: FUN_1411e0780
void M2SceneSetPhysicsCallback(CM2Scene* scene, void (*callback)(void* states, void* param), void* param) {
    scene->m_d678 = reinterpret_cast<void*>(callback);
    scene->m_d680 = callback != nullptr ? param : nullptr;
}

// ref: FUN_1411dfa10
void M2SceneSetWindCallback(CM2Scene* scene, void (*callback)(const float* position, float* direction,
    float* speed, uint8_t gusts)) {
    scene->m_windCallback = reinterpret_cast<decltype(scene->m_windCallback)>(callback);
}

// ref: FUN_1411e0c50
void M2SceneSetGroundCallback(CM2Scene* scene, bool (*callback)(const float* position, float* height, void* param)) {
    scene->m_d6b0 = reinterpret_cast<uint64_t>(callback);
}

// ref: FUN_1411df6d0
void M2SceneRelease(CM2Scene* scene) {
    scene->m_refCount--;
}

// Ported with CM2Scene.
extern "C" unsigned long long FUN_1411dd490(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_1411dd490
CM2BaseModel* M2SceneCloneModel(CM2Scene* scene, CM2BaseModel* model, bool a3) {
    __try {
        return reinterpret_cast<CM2BaseModel*>(FUN_1411dd490((unsigned long long)scene, (unsigned long long)model, (unsigned long long)a3));
    } __except (GenFault(0x1411dd490ull, GetExceptionInformation())) {
        return nullptr;
    }
}

// A file on disk is checked with its textures; one that is not is looked for in the cache and
// otherwise read ahead.
// ref: FUN_1411e40e0
bool CM2Scene::ModelFileReady(const char* name, uint8_t priority) {
    uint8_t residency = cas::CASGetResidencyByName(name, 0, 0, 0);
    if (residency == 0) {
        return false;
    }
    cas::CASFILE* file = nullptr;
    if (residency == 2 && cas::CASFileOpenByName(name, &file, 0)) {
        return ModelFileResident(file);
    }
    if (name == nullptr) {
        return false;
    }
    CM2Shared* shared = M2CacheGetSharedByName(this->m_cache, name, 0x20, priority);
    if (shared == nullptr) {
        char path[0x104];
        ReplaceExtension(path, sizeof(path), name, ".m2");
        this->PreloadModel(cas::CASFileGetIdByName(path, 0), priority, nullptr);
        return false;
    }
    shared->ReleaseModelRef();
    return false;
}

// The scene's preload table (+0x58, keyed by content key) and its async reads
// (FUN_1405db600 with FUN_1411df380 / FUN_1411d3610) are not ported; a model file that is not
// on disk is not read ahead.
extern "C" unsigned long long FUN_1411cf210(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_1411cf210
bool CM2Scene::PreloadModel(int32_t fileDataId, uint8_t priority, void* waiter) {
    __try {
        return (FUN_1411cf210((unsigned long long)this, (unsigned long long)fileDataId, (unsigned long long)priority, (unsigned long long)waiter) & 0xff) != 0;
    } __except (GenFault(0x1411cf210ull, GetExceptionInformation())) {
        return false;
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_141196280=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1411cf210=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1411dd490=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1411ece70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1411ece80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141205ae0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14125deb0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14125fab0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14125fb10=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14125ffb0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141260140=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1411e53d0 = (void*)&M2HitRecsReserve;
extern "C" void* const gen_f_1411e5510 = (void*)&M2SceneEntriesReserve;
// ---- end of generated globals ----
