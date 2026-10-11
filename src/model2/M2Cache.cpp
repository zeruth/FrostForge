// Engine/Source/Model2/M2Cache.cpp: the Model2 module's setup, the object pools, and the cache of
// shared model files by FileDataID.

#include <cstdarg>
#include <cstring>
#include <new>

#include "base/ObjectAlloc.hpp"
#include "cas/CAS.hpp"
#include "console/Var.hpp"
#include "model2/M2Init.hpp"
#include "model2/M2Log.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "model2/M2Element.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "os/Sync.hpp"
#include "util/Unimplemented.hpp"

HSLOG g_m2Log;  // DAT_1430a12c8
M2CACHE g_m2Cache;

namespace {

const char s_cacheFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Cache.cpp";
const char s_sharedNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBHPEAVCM2Shared@@@blz@@@blz@@";

// The pools (DAT_1430a0e68.. DAT_1430a0ea0).
TObjectAllocHeap* s_modelPools[2];
CShaderEffect* s_1430a0ed8;  // DAT_1430a0ed8, the default effect
TObjectAllocHeap* s_sharedPool;
TObjectAllocHeap* s_model3Pools[2];
TObjectAllocHeap* s_m3SharedPool;
TObjectAllocHeap* s_particle3Pool;
TObjectAllocHeap* s_particleSharedPool;
// BaseModel.cpp's (DAT_1430a1260, DAT_1430a1268).
TObjectAllocHeap* s_callPool;
TObjectAllocHeap* s_impactPool;

// The CVars (DAT_1430a0ea8..).
CVar* s_mtParticles;
CVar* s_animFrameSkipLod;
CVar* s_hotReloadModels;
CVar* s_forceLodCheck;
CVar* s_textureErrorColors;

uint8_t s_initialized;  // DAT_1430a0f19
bool s_skeletonCacheEnabled;  // DAT_1430a0f20
uint32_t s_maxParticles;  // DAT_1430a0f1c

// The kept model error (DAT_1430a1030) and where it was raised (PTR_DAT_142cfb0f8, DAT_1430a1038).
char* s_error;
const char* s_errorFile;
int32_t s_errorLine;

// The model file bounds from ModelFileData's bounds file: FileDataID -> the box
// (DAT_1430a55f0 and the open-addressed map at DAT_142cfbe38).
struct MODELBOUNDS {
    int32_t m_fileDataId;
    C3Vector m_min;
    C3Vector m_max;
};
struct BOUNDSSLOT {
    int32_t m_key;              // 0 empty, -1 removed
    uint32_t m_pad;
    const MODELBOUNDS* m_value;
};
struct BOUNDSMAP {
    uint64_t m_capacity;        // DAT_142cfbe38
    BOUNDSSLOT* m_slots;        // DAT_142cfbe40
    uint64_t m_count;           // DAT_142cfbe48
    uint64_t m_removed;         // DAT_142cfbe50
};
BOUNDSMAP s_bounds;
void* s_boundsFile;             // DAT_1430a55f0

uint64_t HashInt(int64_t key) {
    uint64_t value = static_cast<uint64_t>(key);
    value = (value >> 0x21 ^ value) * 0xff51afd7ed558ccdull;
    value = (value >> 0x21 ^ value) * 0xc4ceb9fe1a85ec53ull;
    return value >> 0x21 ^ value;
}

struct SHAREDNODE {
    SHAREDNODE* m_next;
    int32_t m_key;
    uint32_t m_pad;
    CM2Shared* m_value;
};

const char s_sharedBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBHPEAVCM2Shared@@@blz@@@blz@@";

// Rehashes the shared map into at least `minimum` buckets, and as many as its entries need at
// its load limit, rounded up to a power of two from 8 (blz::chained_hash rehash). Nothing
// changes when that is the current count and the buckets exist.
// ref: FUN_141198ef0
void ResizeSharedMap(M2CACHE* cache, uint64_t minimum) {
    float load = static_cast<float>(cache->m_sharedCount) / cache->m_sharedMaxLoad;
    uint64_t needed = static_cast<uint64_t>(load);
    if (static_cast<float>(needed) * cache->m_sharedMaxLoad < static_cast<float>(cache->m_sharedCount)) {
        needed++;
    }
    if (needed < minimum) {
        needed = minimum;
    }
    uint64_t count = 8;
    while (count < needed) {
        count *= 2;
    }
    if (count == cache->m_sharedBucketCount && cache->m_sharedBuckets != nullptr) {
        return;
    }
    auto buckets = static_cast<void**>(
        SMemAlloc(count * 8, 0xb, s_sharedBucketTag, SStrHashFnv1a(s_sharedBucketTag), -6, 0));
    std::memset(buckets, 0, count * 8);
    if (cache->m_sharedBuckets != nullptr) {
        for (uint64_t i = 0; i < cache->m_sharedBucketCount; i++) {
            auto node = static_cast<SHAREDNODE*>(cache->m_sharedBuckets[i]);
            while (node != nullptr) {
                SHAREDNODE* next = node->m_next;
                uint64_t slot = HashInt(node->m_key) & (count - 1);
                node->m_next = static_cast<SHAREDNODE*>(buckets[slot]);
                buckets[slot] = node;
                node = next;
            }
        }
    }
    SMemFree(cache->m_sharedBuckets, s_sharedBucketTag, -6, 0);
    cache->m_sharedBuckets = buckets;
    cache->m_sharedBucketCount = count;
}

// The slot for a key, made when missing (FUN_141195bd0).
// ref: FUN_141195bd0
CM2Shared** SharedMapInsert(M2CACHE* cache, const int32_t* key) {
    if (cache->m_sharedBuckets == nullptr) {
        ResizeSharedMap(cache, cache->m_sharedBucketCount);
    }
    int32_t value = *key;
    uint64_t slot = HashInt(value) & (cache->m_sharedBucketCount - 1);
    auto link = reinterpret_cast<SHAREDNODE**>(&cache->m_sharedBuckets[slot]);
    for (SHAREDNODE* node = *link; node != nullptr && node->m_key != value; node = node->m_next) {
        link = &node->m_next;
    }
    if (*link != nullptr) {
        return &(*link)->m_value;
    }
    auto node = static_cast<SHAREDNODE*>(SMemAlloc(sizeof(SHAREDNODE), 0xb, s_sharedNodeTag, SStrHashFnv1a(s_sharedNodeTag), -6, 0));
    node->m_next = static_cast<SHAREDNODE*>(cache->m_sharedBuckets[slot]);
    node->m_key = value;
    node->m_value = nullptr;
    cache->m_sharedBuckets[slot] = node;
    cache->m_sharedCount++;
    float load = static_cast<float>(cache->m_sharedCount) / static_cast<float>(cache->m_sharedBucketCount);
    if (cache->m_sharedMaxLoad < load) {
        ResizeSharedMap(cache, cache->m_sharedBucketCount * 2);
    }
    return &node->m_value;
}

CM2Shared* SharedMapFind(M2CACHE* cache, int32_t key) {
    if (cache->m_sharedBuckets == nullptr) {
        return nullptr;
    }
    uint64_t slot = HashInt(key) & (cache->m_sharedBucketCount - 1);
    for (auto node = static_cast<SHAREDNODE*>(cache->m_sharedBuckets[slot]); node != nullptr; node = node->m_next) {
        if (node->m_key == key) {
            return node->m_value;
        }
    }
    return nullptr;
}

// A model file's bounds from the bounds file (FUN_141283560).
// ref: FUN_141283560
bool ModelBoundsFind(int32_t fileDataId, C3Vector* min, C3Vector* max) {
    if (s_boundsFile == nullptr) {
        return false;
    }
    uint64_t mask = s_bounds.m_capacity - 1;
    uint64_t slot = HashInt(fileDataId) & mask;
    BOUNDSSLOT* found = &s_bounds.m_slots[s_bounds.m_capacity];
    for (uint64_t step = 1; s_bounds.m_slots[slot].m_key != 0; step++) {
        int32_t key = s_bounds.m_slots[slot].m_key;
        if ((s_bounds.m_removed == 0 || key != -1) && key == fileDataId) {
            found = &s_bounds.m_slots[slot];
            break;
        }
        slot = (slot + step) & mask;
    }
    if (found == &s_bounds.m_slots[s_bounds.m_capacity]) {
        return false;
    }
    *min = found->m_value->m_min;
    *max = found->m_value->m_max;
    return true;
}

// The cache's statics (the static initializer of M2Cache.cpp): the lists and the map.
struct CACHEINIT {
    CACHEINIT() {
        g_m2Cache.m_sharedBucketCount = 0x40;
        g_m2Cache.m_sharedMaxLoad = 1.0f;
        g_m2Cache.m_particleBucketCount = 0x40;
        g_m2Cache.m_particleMaxLoad = 1.0f;
        g_m2Cache.m_scenes.m_linkoffset = offsetof(CM2Scene, m_cacheLink);
        g_m2Cache.m_idleShared.m_linkoffset = offsetof(CM2Shared, m_cacheLink);
        g_m2Cache.m_keyframeShared.m_linkoffset = offsetof(CM2Shared, m_keyframeLink);
    }
};
CACHEINIT s_cacheInit;

}

// ---- the log and the model error ----------------------------------------------------------------

// ref: FUN_1411b7d70
void M2SetErrorLocation(const char* file, int32_t line) {
    s_errorFile = file;
    s_errorLine = line;
}

// ref: FUN_1411b7cc0
void M2SetError(const char* format, ...) {
    if (s_error != nullptr) {
        return;
    }
    s_error = static_cast<char*>(SMemAlloc(0x100, 0x12, "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Model.cpp",
        0, 0x3926, 0));
    va_list args;
    va_start(args, format);
    SStrVPrintf(s_error, 0x100, format, args);
    va_end(args);
}

const char* M2GetError() {
    return s_error;
}

// ---- the cache ------------------------------------------------------------------------------

// ref: FUN_141197f50
void M2CacheLock() {
    OsMutexLock(&g_m2Cache.m_sharedLock);
}

// ref: FUN_141198710
void M2CacheUnlock() {
    OsMutexUnlock(&g_m2Cache.m_sharedLock);
}

// Off the map, when the file goes (in CM2Shared's destructor).
void M2CacheRemoveShared(M2CACHE* cache, int32_t fileDataId) {
    if (cache->m_sharedBuckets == nullptr) {
        return;
    }
    uint64_t slot = HashInt(fileDataId) & (cache->m_sharedBucketCount - 1);
    auto link = reinterpret_cast<SHAREDNODE**>(&cache->m_sharedBuckets[slot]);
    while (*link != nullptr && (*link)->m_key != fileDataId) {
        link = &(*link)->m_next;
    }
    if (*link != nullptr) {
        SHAREDNODE* node = *link;
        *link = node->m_next;
        SMemFree(node, s_sharedNodeTag, -6, 0);
        cache->m_sharedCount--;
    }
}

// ref: FUN_1411962a0
CM2Shared* M2CacheGetShared(M2CACHE* cache, int32_t fileDataId, const char* name, uint32_t flags, uint8_t priority) {
    CM2Shared* shared = nullptr;
    if ((flags & 4) == 0) {
        shared = SharedMapFind(cache, fileDataId);
        if (shared == nullptr && (flags & 0x20) != 0) {
            return nullptr;
        }
        if (shared != nullptr) {
            shared->AddRef();
        }
    }
    if (shared == nullptr) {
        void* memory = s_sharedPool->Alloc();
        shared = new (memory) CM2Shared(cache);
        shared->AddRef();
        if (ModelBoundsFind(fileDataId, &shared->m_bounds.m_min, &shared->m_bounds.m_max)) {
            const CAaBox& box = shared->m_bounds;
            if (box.m_min.x <= box.m_max.x && box.m_min.y <= box.m_max.y && box.m_min.z <= box.m_max.z) {
                shared->m_flags |= CM2Shared::SHARED_HAS_BOUNDS;
            }
        }
        if (name == nullptr) {
            if ((flags & 0x200) == 0) {
                shared->LoadById(fileDataId, flags, priority);
            }
            // FUN_14230d6e0
            SStrPrintf(shared->m_name, 0x104, "FileData ID %d", fileDataId);
        } else {
            if ((flags & 0x200) == 0) {
                shared->LoadByName(name, flags, priority);
            }
            char* dest = shared->m_name;
            const char* source = name;
            for (int64_t left = 0x103;;) {
                char c = *source++;
                *dest++ = c;
                if (c == '\0') {
                    break;
                }
                if (--left == 0) {
                    *dest = '\0';
                    break;
                }
            }
        }
        shared->m_fileDataId = fileDataId;
        if ((flags & 4) == 0) {
            *SharedMapInsert(cache, &fileDataId) = shared;
        }
    }
    if (((flags >> 0xf) & 1) == 0) {
        shared->AddUse();
    }
    if (s_skeletonCacheEnabled && shared->m_skeleton == nullptr) {
        // DAT_1430a0f20: the file's skeleton from the skeleton cache (FUN_1411f9cd0)
        FROST_UNIMPLEMENTED();
    }
    return shared;
}

// ref: FUN_141196500
CM2Shared* M2CacheGetSharedByName(M2CACHE* cache, const char* name, uint32_t flags, uint8_t priority) {
    char path[0x110];
    // FUN_140214020: the name with its extension replaced by ".m2"
    const char* dot = nullptr;
    for (const char* p = name; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        } else if (*p == '/' || *p == '\\') {
            dot = nullptr;
        }
    }
    uint64_t stem = static_cast<uint64_t>((dot != nullptr ? dot : name + std::strlen(name)) - name);
    if (stem < 0x104) {
        std::memcpy(path, name, stem);
        SStrPrintf(path + stem, 0x104 - stem, "%s", ".m2");
    } else {
        path[0] = '\0';
    }
    int32_t fileDataId = cas::CASFileGetIdByName(path, 0);
    uint32_t loadFlags = fileDataId != 0 ? flags : (flags | 4);
    return M2CacheGetShared(cache, fileDataId, path, loadFlags, priority);
}

// ---- setup -----------------------------------------------------------------------------------

namespace {

TObjectAllocHeap* MakePool(const char* name, uint32_t size, uint32_t perHeap, int32_t memFlags) {
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x12, s_cacheFile, 0, 0x5f0, 0);
    return new (memory) TObjectAllocHeap(name, size, perHeap, memFlags);
}

}

// The ray-traced shadow pass's compute shaders (DAT_1430a1280, four skinning permutations, and
// DAT_1430a12a0), made when the device can trace rays (FUN_1411f0610) and released with it
// (FUN_1411f0690). The compute shader entry points (device slots 0x298 and 0x2a0) are not
// ported, so a device that can trace rays reports it.
// ref: FUN_1411f0610
void M2ShadowRtCreate() {
    if (GxDeviceCaps()[0xaf] != 0) {
        FROST_UNIMPLEMENTED();  // FUN_142289bb0 "M2ShadowRt Skin" x4, "M2 TC Explode"
    }
}

// ref: FUN_1411f0690
void M2ShadowRtRelease() {
}

// Every scene's and file's device objects remade, the shared states rebuilt (FUN_1411960c0).
// ref: FUN_1411960c0
void M2CacheOnDeviceCreated(void* param) {
    (void)param;
    for (CM2Scene* scene = g_m2Cache.m_scenes.Head(); scene != nullptr; scene = g_m2Cache.m_scenes.Next(scene)) {
        scene->RecreateDeviceObjects();
    }
    for (CM2Shared* shared = g_m2Cache.m_idleShared.Head(); shared != nullptr; shared = g_m2Cache.m_idleShared.Next(shared)) {
        shared->RefreshBatchEffects();
    }
    if (g_m2Cache.m_idleM3.Head() != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_1411ece70: Model3 is not ported, so no M3 file is ever idle
    }
    M2SceneStatesInitialize();
    if (g_m2Cache.m_sharedBuckets != nullptr) {
        for (uint64_t i = 0; i < g_m2Cache.m_sharedBucketCount; i++) {
            for (auto node = static_cast<SHAREDNODE*>(g_m2Cache.m_sharedBuckets[i]); node != nullptr; node = node->m_next) {
                node->m_value->RefreshBatchEffects();
            }
        }
    }
    M2ShadowRtCreate();
    M2ElementDeviceCreated();
}

// Every scene's and file's device objects released (FUN_141196800).
// ref: FUN_141196800
void M2CacheOnDeviceDestroyed(void* param) {
    (void)param;
    for (CM2Scene* scene = g_m2Cache.m_scenes.Head(); scene != nullptr; scene = g_m2Cache.m_scenes.Next(scene)) {
        scene->ReleaseDeviceObjects();
    }
    for (CM2Shared* shared = g_m2Cache.m_keyframeShared.Head(); shared != nullptr; shared = g_m2Cache.m_keyframeShared.Next(shared)) {
        shared->ReleaseGpuBuffers();
    }
    for (CM2Shared* shared = g_m2Cache.m_idleShared.Head(); shared != nullptr; shared = g_m2Cache.m_idleShared.Next(shared)) {
        shared->ReleaseGpuBuffers();
    }
    if (g_m2Cache.m_idleM3.Head() != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_1411ece80
    }
    s_1430a0ed8 = nullptr;
    M2ShadowRtRelease();
    M2ElementDeviceLost();
}

// The render settings changed: every scene's models told (FUN_141198c30).
// ref: FUN_141198c30
void M2CacheOnRenderSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    (void)param;
    for (CM2Scene* scene = g_m2Cache.m_scenes.Head(); scene != nullptr;) {
        CM2Scene* next = g_m2Cache.m_scenes.Next(scene);
        scene->OnRenderSettingsChanged(previous, current);
        scene = next;
    }
}

// The pools, the cache's flags and its device callbacks (FUN_141197720).
// ref: FUN_141197720
void M2CacheInitialize(uint32_t flags, int32_t perHeap) {
    if (g_m2Cache.m_initialized == 0) {
        g_m2Cache.m_flags |= flags & 0x1bd7;
        if (((g_m2Cache.m_flags >> 0xb) & 1) == 0) {
            GxAddDeviceCreatedCallback(M2CacheOnDeviceCreated, nullptr);
            GxAddDeviceDestroyedCallback(M2CacheOnDeviceDestroyed, nullptr);
            RenderSettingsAddHandler(M2CacheOnRenderSettingsChanged, nullptr);
        }
        // FUN_141198ca0 / FUN_141197f90: the CAS hot-fix and manifest handlers that reload
        // models whose files changed.
        s_maxParticles = 0x100;  // FUN_1411a8060
        g_m2Cache.m_initialized = 1;
    } else {
        M2InitLog("Model2: M2Initialize called more than once\n");
    }
    int32_t count = perHeap != 0 ? perHeap : 0x80;
    char name[0x100];
    SStrPrintf(name, sizeof(name), "CM2Model2%d", 0);
    s_modelPools[0] = MakePool(SStrDupA(name, nullptr, 0), 0x690, static_cast<uint32_t>(count), 0x12);
    SStrPrintf(name, sizeof(name), "CM2Model2%d", 1);
    s_modelPools[1] = MakePool(SStrDupA(name, nullptr, 0), 0x690, static_cast<uint32_t>(count), 0x12);
    s_sharedPool = MakePool("CM2Shared", 0x628, 0x80, 0x12);
    SStrPrintf(name, sizeof(name), "CM3Model%d", 0);
    s_model3Pools[0] = MakePool(SStrDupA(name, nullptr, 0), 0xed8, static_cast<uint32_t>(count), 0x12);
    SStrPrintf(name, sizeof(name), "CM3Model%d", 1);
    s_model3Pools[1] = MakePool(SStrDupA(name, nullptr, 0), 0xed8, static_cast<uint32_t>(count), 0x12);
    s_m3SharedPool = MakePool("CM3Shared", 0x510, 0x80, 0x12);
    s_particleSharedPool = MakePool("ParticleShared", 0x468, 0x80, 0x15);
    s_particle3Pool = MakePool("Particle3", 0x680, 0x80, 0x15);
    // FUN_1411e7300: BaseModel's pools
    s_callPool = MakePool("M2 Model Call Pool", 0xa0, 0x10, 0x12);
    s_impactPool = MakePool("M2 Model Impact Pool", 0x18, 0x10, 0x12);
    // FUN_1411c5d90: CM2Model's own pools
    M2ModelInitializePools();
    if ((flags >> 10) & 1) {
        // FUN_1411b7d60
        M2ModelSetFlag400(false);
    }
    // FUN_1411f9fe0: the skeleton cache
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1411974e0
void M2Initialize(uint32_t flags, int32_t perHeap, uint32_t logFlags) {
    // FUN_1402146d0
    SLogCreate("Logs\\Model.log", logFlags, &g_m2Log);
    SLogWrite(g_m2Log, "M2Initialize()");
    s_mtParticles = CVarRegister("mtParticles", "multithread building of particle vertices", 0x440, "1", nullptr, 1,
        false, nullptr, false);
    s_animFrameSkipLod = CVarRegister("animFrameSkipLOD", "animations will skip frames at distance", 1, "0", nullptr, 1,
        false, nullptr, false);
    s_hotReloadModels = CVarRegister("hotReloadModels",
        "Allow an active model to be reloaded when a new version is detected in the bin folder.  If this is disabled, "
        "the model data will only be refreshed after all game objects using the model are deleted",
        1, "1", nullptr, 1, false, nullptr, false);
    s_forceLodCheck = CVarRegister("forceLODCheck",
        "If enabled, we will skip checking DBC for LOD count and every m2 will scan the folder for skin profiles", 1, "0",
        nullptr, 1, false, nullptr, false);
    s_textureErrorColors = CVarRegister("textureErrorColors",
        "If enabled, replaceable textures that aren't specified will be purple", 1, "1", nullptr, 1, false, nullptr,
        false);
    M2CacheInitialize(flags, perHeap);
    if (g_m2Cache.m_initialized != 0) {
        if (s_forceLodCheck->m_intValue == 0) {
            g_m2Cache.m_flags &= ~0x1000u;
        } else {
            g_m2Cache.m_flags |= 0x1000;
        }
    }
    // DAT_1430a0ed0: the module's own scene (FUN_141196e40(9, ...)), made with CM2Scene.
    M2CreateModuleScene();
    char line[0x100];
    SStrPrintf(line, sizeof(line), "M2_USE_LOD: %d", flags & 0x200);
    SLogWrite(g_m2Log, line);
    // FUN_14242bd00
    s_initialized = 1;
}

// A model from the pool its creation flags pick (0x4000), constructed (FUN_141196cf0).
// ref: FUN_141196cf0
CM2Model* M2ModelAlloc(uint32_t flags) {
    void* memory = s_modelPools[(flags >> 0xe) & 1]->Alloc();
    if (memory == nullptr) {
        return nullptr;
    }
    return new (memory) CM2Model();
}

// ref: FUN_141197410
void M2ModelFree(CM2Model* model) {
    TObjectAllocHeap* pool = s_modelPools[(model->m_createFlags >> 0xe) & 1];
    model->~CM2Model();
    pool->Free(model);
}

// "Skin", made with shaders 0x2f and 0xdc when missing, kept until the device goes. The
// reference clears only its low half on device loss (a dword store); the pointer is cleared
// whole here.
// ref: FUN_141196ca0
CShaderEffect* M2CacheDefaultEffect() {
    if (s_1430a0ed8 == nullptr) {
        s_1430a0ed8 = ShaderEffectFind("Skin");
        if (s_1430a0ed8 == nullptr) {
            s_1430a0ed8 = ShaderEffectCreate("Skin", 0x2f, 0xdc);
        }
    }
    return s_1430a0ed8;
}

TObjectAllocHeap* M2GetModelPool(uint32_t which) {
    return s_modelPools[which & 1];
}

TObjectAllocHeap* M2GetSharedPool() {
    return s_sharedPool;
}

TObjectAllocHeap* M2GetCallPool() {
    return s_callPool;
}

TObjectAllocHeap* M2GetImpactPool() {
    return s_impactPool;
}

// ref: FUN_141197460
void M2SharedFree(CM2Shared* shared) {
    shared->~CM2Shared();
    s_sharedPool->Free(shared);
}

// ---- the parts not ported yet ---------------------------------------------------------------

// FUN_1411fa590: the procedural stand-in made of a file that failed.
void M2SharedProceduralRelease(CM2Shared* shared) {
    (void)shared;
    FROST_UNIMPLEMENTED();
}

// FUN_1411f8840: a skeleton file's reference.
void M2SkeletonRelease(void* skeleton) {
    (void)skeleton;
    FROST_UNIMPLEMENTED();
}

int32_t M2AnimFrameSkipLod() {
    return s_animFrameSkipLod->m_intValue;
}

// FUN_141203770: a skeleton's per-model data released.
void M2SkeletonReleaseModel(void* skeleton) {
    (void)skeleton;
    FROST_UNIMPLEMENTED();
}

// FUN_141203400: the skeleton instance's scene.
void M2SkeletonSetScene(void* skeleton, CM2Scene* scene) {
    (void)skeleton;
    (void)scene;
    FROST_UNIMPLEMENTED();
}

// FUN_1411f9c00: a skeleton instance made for a model.
void* M2SkeletonCreateInstance(void* skeleton, CM2Scene* scene, uint32_t flags) {
    (void)skeleton;
    (void)scene;
    (void)flags;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// FUN_141204240: a skeleton instance's reference dropped.
void M2SkeletonInstanceRelease(void* skeleton) {
    (void)skeleton;
    FROST_UNIMPLEMENTED();
}

// FUN_141203470: a skeleton's playbacks for a model dropped.
void M2SkeletonCancelModelPlaybacks(void* skeleton, CM2Model* model) {
    (void)skeleton;
    (void)model;
    FROST_UNIMPLEMENTED();
}

// FUN_1411f9040: a skeleton file's reads waited on.
bool M2SkeletonWaitForLoad(void* skeleton) {
    (void)skeleton;
    FROST_UNIMPLEMENTED();
    return false;
}

// A skeleton file's sequences: never reached until skeleton files are read.
M2Array<M2Sequence>* M2SkeletonSharedSequences(void* skeleton) {
    (void)skeleton;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// The index of sequence `id` through the id hash (FUN_1411fed70): 0xffff when missing.
// ref: FUN_1411fed70
uint16_t M2FindSequenceById(uint32_t id, const M2Array<uint16_t>* hash, const M2Array<M2Sequence>* sequences) {
    uint16_t buckets = hash->m_count;
    if (buckets == 0) {
        for (uint32_t i = 0; i < sequences->m_count; i++) {
            if ((*sequences)[i].m_id == id) {
                return static_cast<uint16_t>(i);
            }
        }
        return 0xffff;
    }
    uint32_t slot = id % buckets;
    uint16_t index = (*hash)[slot];
    for (int32_t step = 1; index != 0xffff; step++) {
        if ((*sequences)[index].m_id == id) {
            return index;
        }
        slot = static_cast<uint32_t>(step * step + static_cast<int32_t>(slot)) % buckets;
        index = (*hash)[slot];
    }
    return 0xffff;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a55f0 = (void*)&s_boundsFile;
// ---- end of generated globals ----
