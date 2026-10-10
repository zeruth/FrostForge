#ifndef MODEL2_M2_SHARED_HPP
#define MODEL2_M2_SHARED_HPP

// CM2Shared (Engine/Source/Model2/M2Shared.cpp, 0x620 bytes in a 0x628 ObjectAlloc slot): one
// .m2 file as every model made from it shares it. The file is read asynchronously and used in
// place (M2Init.cpp); the chunks around the MD20 data name the skin profiles, textures and other
// files by FileDataID. Once the data is in, a skin profile is chosen and read, and the models
// waiting on the file are initialized.

#include <cstdint>

#include "model2/M2Data.hpp"
#include "os/Sync.hpp"
#include "storm/list/TSLink.hpp"
#include "storm/list/TSList.hpp"

class CAsyncObject;
class CM2Model;
class CShaderEffect;
struct GxBuf;
struct M2CACHE;
struct CM2Shared;
class CM2Scene;
class CM3Shared;
struct CM2SequenceLoad;
struct CM2KeyframeData;
class TObjectAllocHeap;

// A skin profile being read (0x28).
struct CM2SkinProfileLoad {
    TSLink<CM2SkinProfileLoad> m_link;  // 0x00
    CAsyncObject* m_asyncObject;        // 0x10
    CM2Shared* m_shared;                // 0x18
    uint32_t m_index;                   // 0x20 the profile slot
    uint32_t m_pad24;
};
static_assert(sizeof(CM2SkinProfileLoad) == 0x28, "CM2SkinProfileLoad is 0x28 bytes");

// A bone file (BFID) being read (0x28).
struct CM2BoneOffsetLoad {
    TSLink<CM2BoneOffsetLoad> m_link;   // 0x00
    CAsyncObject* m_asyncObject;        // 0x10
    CM2Shared* m_shared;                // 0x18
    uint32_t m_index;                   // 0x20
    uint32_t m_pad24;
};
static_assert(sizeof(CM2BoneOffsetLoad) == 0x28, "CM2BoneOffsetLoad is 0x28 bytes");

// A bone file's bone remap (0x20).
struct CM2BoneOffset {
    // 1 loading, 2 loaded
    uint32_t m_flags;                   // 0x00
    uint32_t m_pad04;
    uint16_t* m_boneIndices;            // 0x08 by the model's bone index (0xffff for none)
    uint16_t* m_offsets;                // 0x10 the BOMT chunk
    void* m_data;                       // 0x18 the file
};
static_assert(sizeof(CM2BoneOffset) == 0x20, "CM2BoneOffset is 0x20 bytes");

// A skin profile in use (0x58).
struct CM2SkinProfileData {
    uint16_t m_maxBones;                // 0x00 the most bones a skin section uses
    uint16_t m_bonesPerSlot;            // 0x02 0x100 / (m_maxBones + 1)
    uint16_t m_boneCount;               // 0x04 cached by FUN_1411a7f70
    uint16_t m_lastBone;                // 0x06
    M2SkinProfile* m_profile;           // 0x08
    M2SkinSection* m_sections;          // 0x10
    CShaderEffect** m_effects;          // 0x18 one per batch
    GxBuf* m_indexBuf;                  // 0x20
    GxBuf* m_vertexBuf;                 // 0x28
    void* m_indexView;                  // 0x30 ray tracing
    void* m_vertexView;                 // 0x38
    void* m_rtObject;                   // 0x40
    int32_t m_rtGeometry;               // 0x48 -1 for none
    uint32_t m_pad4c;
    void* m_rtTexcoords;                // 0x50
};
static_assert(sizeof(CM2SkinProfileData) == 0x58, "CM2SkinProfileData is 0x58 bytes");

struct CM2Shared {
    // m_flags.
    enum : uint32_t {
        SHARED_LOADED = 0x1,
        SHARED_FROM_NAME_FLAG = 0x2,
        SHARED_IN_SKELETON = 0x4,
        SHARED_SEQUENCES_PENDING = 0x8,
        SHARED_SEQUENCES_FREED = 0x10,
        SHARED_FILE_FREED = 0x20,
        SHARED_ERROR = 0x40,
        SHARED_FILE_NAME = 0x80,
        SHARED_MULTIPLE_SKINS = 0x100,
        SHARED_HAS_SPECIAL_BONES = 0x200,
        SHARED_HAS_BOUNDS = 0x400,
        SHARED_CAMERA_FIXED = 0x800,
        SHARED_HAS_PHYSICS_BONES = 0x1000,
        SHARED_HAS_SHADOW_BATCHES = 0x2000,
        SHARED_PRIORITY_BIT = 0x4000,
        SHARED_TINTED = 0x8000,
        SHARED_EXTENDED_PARTICLES = 0x10000,
        SHARED_FROM_NAME = 0x40000,
    };

    int32_t m_refCount;                 // 0x000
    int32_t m_useCount;                 // 0x004
    M2CACHE* m_cache;                   // 0x008
    uint32_t m_flags;                   // 0x010
    uint8_t m_priority;                 // 0x014
    uint8_t m_skinsLoaded;              // 0x015 one bit per profile slot
    uint8_t m_pad16[2];
    // LDV1: 0x8000 set, 4 a distance, < 0 the levels below are by DB.
    int16_t m_lodFlags;                 // 0x018
    uint8_t m_lodCount;                 // 0x01a
    uint8_t m_lodMax;                   // 0x01b
    float m_lodDistance;                // 0x01c
    uint8_t m_lodLevels[8];             // 0x020
    float m_lodScale;                   // 0x028
    float m_boundsRadius;               // 0x02c
    C3Vector m_center;                  // 0x030
    uint32_t m_pad3c;
    TSLink<CM2Shared> m_cacheLink;      // 0x040 the cache's idle list
    uint32_t m_releaseFrame;            // 0x050
    char m_name[0x104];                 // 0x054
    int32_t m_fileDataId;               // 0x158
    uint32_t m_pad15c;
    CAsyncObject* m_asyncObject;        // 0x160
    CM2Model* m_waitingModels;          // 0x168 models to initialize once loaded
    CM2Model** m_waitingTail;           // 0x170
    void* m_178;                        // 0x178
    void* m_180;                        // 0x180
    TSList<CM2SequenceLoad, TSGetLink<CM2SequenceLoad>, 8> m_sequenceLoads;  // 0x188
    TSList<CM2KeyframeData, TSGetLink<CM2KeyframeData>, 8> m_keyframeData;   // 0x1a0
    TSList<CM2SkinProfileLoad, TSGetLink<CM2SkinProfileLoad>, 8> m_skinProfileLoads;  // 0x1b8
    void* m_fileData;                   // 0x1d0
    uint32_t m_fileSize;                // 0x1d8
    uint32_t m_pad1dc;
    M2Data* m_data;                     // 0x1e0
    uint32_t m_dataSize;                // 0x1e8
    CAaBox m_bounds;                    // 0x1ec
    uint32_t m_pad204;
    uint64_t* m_animFileIds;            // 0x208 AFID: {id, variation} -> file
    uint32_t m_animFileCount;           // 0x210
    uint32_t m_pad214;
    int32_t* m_skinFileIds;             // 0x218 SFID
    uint32_t m_skinFileCount;           // 0x220
    uint32_t m_pad224;
    int32_t* m_textureFileIds;          // 0x228 TXID
    uint32_t* m_geometryParticleIds;    // 0x230 GPID
    uint32_t* m_recursiveParticleIds;   // 0x238 RPID
    int32_t m_physicsFileId;            // 0x240 PFID
    int32_t m_skeletonFileId;           // 0x244 SKID
    int32_t* m_boneFileIds;             // 0x248 BFID
    uint16_t* m_textureCombiners;       // 0x250 TXAC
    uint16_t* m_particleCombiners;      // 0x258
    M2Array<M2ExtendedParticle>* m_extendedParticles;  // 0x260 EXP2
    CM2SkinProfileData m_skins[8];      // 0x268
    void* m_block;                      // 0x528 the arrays below in one allocation
    void** m_textures;                  // 0x530 HTEXTURE per M2Texture
    uint32_t* m_boneOrder;              // 0x538
    uint32_t* m_sequenceState;          // 0x540
    void* m_physics;                    // 0x548
    uint32_t m_550;                     // 0x550
    uint32_t m_pad554;
    void* m_558;                        // 0x558
    TSList<CM2BoneOffsetLoad, TSGetLink<CM2BoneOffsetLoad>, 8> m_boneOffsetLoads;  // 0x560
    uint32_t m_boneFileCount;           // 0x578
    uint32_t m_pad57c;
    CM2BoneOffset* m_boneOffsets;       // 0x580
    TSLink<CM2Shared> m_588;            // 0x588
    void* m_particleData;               // 0x598 0x14 per particle emitter
    float m_tint[4];                    // 0x5a0
    void* m_skeleton;                   // 0x5b0 CM2SkeletonShared
    void* m_parentEventData;            // 0x5b8 PEDC
    void* m_parentAnimData;             // 0x5c0 PADC
    void* m_parentSequenceBounds;       // 0x5c8 PSBC
    void* m_blocklistAnimData;          // 0x5d0 PABC
    void* m_waterfallData;              // 0x5d8 WFV3
    void* m_edgeFades;                  // 0x5e0 EDGF
    uint32_t m_edgeFadeCount;           // 0x5e8
    uint32_t m_pad5ec;
    void* m_nerf;                       // 0x5f0 NERF
    void* m_detailLayers;               // 0x5f8 DETL
    void* m_particleGeosets;            // 0x600 PGD1
    void* m_dbocs;                      // 0x608 DBOC
    uint32_t m_dbocCount;               // 0x610
    uint32_t m_pad614;
    void* m_afra;                       // 0x618 AFRA

    // ref: FUN_1411a3e50
    explicit CM2Shared(M2CACHE* cache);
    // ref: FUN_1411a4460
    ~CM2Shared();

    // ref: FUN_1411a4e70
    int32_t AddRef();
    // ref: FUN_1411a4ef0
    void AddUse();
    // A model's reference: a use and a reference (FUN_1411a4df0), and its release, the last one
    // putting the file on the cache's idle list (FUN_1411abed0). The count left.
    int32_t AddModelRef();
    int32_t ReleaseModelRef();

    // Starts the read (FUN_1411aa6f0 by FileDataID, FUN_1411aa7c0 by name).
    void LoadById(int32_t fileDataId, uint32_t flags, uint8_t priority);
    void LoadByName(const char* name, uint32_t flags, uint8_t priority);

    // The file's chunks (FUN_1411a6de0): false when the data is corrupt.
    bool ParseChunks(uint32_t* data, uint32_t size);
    // The MD21 chunk's MD20 data (FUN_1411abcf0).
    bool InitializeData(M2Data* data, uint32_t size);
    // After the chunks (FUN_1411a6d50): chooses and reads the skin profiles.
    bool InitializeLoaded();
    // ref: FUN_1411a9640
    bool InitializeSkinsAndBones();
    // Reads skin profile `index` (FUN_1411aabd0).
    void LoadSkinProfile(uint32_t slot, uint32_t index, bool lod);
    // A skin profile read (FUN_1411aa100).
    bool InitializeSkinProfile(uint32_t slot);
    // The bone files (FUN_1411aa8a0).
    bool LoadBoneFiles();
    // Everything is in: the waiting models initialize (FUN_1411a7bf0).
    void OnFullyLoaded();
    // The textures (FUN_1411a5fd0).
    void CreateTextures();
    // The EXP2 data (FUN_1411a61c0).
    void InitializeParticleData();

    // Releases the file and what came out of it (FUN_1411a5470, FUN_1411a52e0, FUN_1411a6cd0,
    // FUN_1411a7d10, FUN_1411a5640).
    void CancelSequenceLoads();
    void CancelLoads();
    void ReleaseTextures();
    void ReleaseBoneData();
    void ClearChunkData();

    // The name or "Model not loaded" (FUN_1411a85d0).
    const char* GetDisplayName();
    // The bones (the skeleton's when there is one, FUN_1411a86a0).
    M2Array<M2CompBone>* GetBones();
    // A skin profile's buffers (FUN_1411a83e0 index, FUN_1411a8700 vertex).
    GxBuf* GetIndexBuf(uint32_t slot);
    GxBuf* GetVertexBuf(uint32_t slot);
    // A batch's shader effect (FUN_1411a8190).
    CShaderEffect* GetBatchEffect(const M2Batch* batch);
};
static_assert(sizeof(CM2Shared) == 0x620, "CM2Shared is 0x620 bytes");

// The model cache (DAT_142cfaee0, Engine/Source/Model2/M2Cache.cpp): the scenes, the shared
// files by FileDataID, and the shared files no model uses any more, kept a while.
struct M2CACHE {
    TSList<CM2Scene, TSGetLink<CM2Scene>, 8> m_scenes;  // 0x00
    uint32_t m_initialized;                             // 0x18 DAT_142cfaef8
    uint32_t m_flags;                                   // 0x1c DAT_142cfaefc
    // blz::chained_hash_map<int, CM2Shared*>
    uint64_t m_sharedBucketCount;                       // 0x20
    void** m_sharedBuckets;                             // 0x28
    uint64_t m_sharedCount;                             // 0x30
    float m_sharedMaxLoad;                              // 0x38
    uint32_t m_pad3c;
    // blz::chained_hash_map<int, CParticleShared*>
    uint64_t m_particleBucketCount;                     // 0x40
    void** m_particleBuckets;                           // 0x48
    uint64_t m_particleCount;                           // 0x50
    float m_particleMaxLoad;                            // 0x58
    uint32_t m_pad5c;
    uint8_t m_60[0x40];                                 // 0x60
    uint32_t m_frame;                                   // 0xa0 DAT_142cfaf80
    uint32_t m_padA4;
    TSList<CM2Scene, TSGetLink<CM2Scene>, 8> m_a8;      // 0xa8
    TSList<CM2Shared, TSGetLink<CM2Shared>, 8> m_idleShared;  // 0xc0
    TSList<CM3Shared, TSGetLink<CM3Shared>, 8> m_idleM3;  // 0xd8
    TSList<CM3Shared, TSGetLink<CM3Shared>, 8> m_f0;      // 0xf0
    OSMUTEX m_sharedLock;                               // 0x108
    OSMUTEX m_m3Lock;                                   // 0x110
    uint32_t m_118;                                     // 0x118
    uint32_t m_idleFrames;                              // 0x120 frames an unused file is kept
    uint32_t m_pad124;
};

extern M2CACHE g_m2Cache;

// The shared file of a FileDataID, made and its read started when not cached
// (FUN_1411962a0). `name` is the path when known.
CM2Shared* M2CacheGetShared(M2CACHE* cache, int32_t fileDataId, const char* name, uint32_t flags, uint8_t priority);
// The same by path, with ".m2" on the end (FUN_141196500).
CM2Shared* M2CacheGetSharedByName(M2CACHE* cache, const char* name, uint32_t flags, uint8_t priority);
// The cache's lock (FUN_141197f50, FUN_141198710).
void M2CacheLock();
void M2CacheUnlock();
// Off the map, as the file goes.
void M2CacheRemoveShared(M2CACHE* cache, int32_t fileDataId);
// ref: FUN_141197720
void M2CacheInitialize(uint32_t flags, int32_t perHeap);
// The module's setup: the log, the CVars, the pools and the module scene (FUN_1411974e0).
void M2Initialize(uint32_t flags, int32_t perHeap, uint32_t logFlags);
// The pools (DAT_1430a0e68 by the creation flag 0x4000, DAT_1430a0e78).
TObjectAllocHeap* M2GetModelPool(uint32_t which);
TObjectAllocHeap* M2GetSharedPool();
// BaseModel's pools (DAT_1430a1260 the calls, DAT_1430a1268 the impacts).
TObjectAllocHeap* M2GetCallPool();
TObjectAllocHeap* M2GetImpactPool();
// The file's destruction back into its pool (FUN_141197460).
void M2SharedFree(CM2Shared* shared);

// A file read's memory (FUN_1412be750).
void M2FreeFileData(void* data);
// A skin profile's buffers go (FUN_1411a6bf0).
void M2SkinProfileRelease(CM2SkinProfileData* skin);
// The procedural stand-in of a failed file (FUN_1411fa590) and a skeleton's reference
// (FUN_1411f8840).
void M2SharedProceduralRelease(CM2Shared* shared);
void M2SkeletonRelease(void* skeleton);
// The index of sequence `id` through the id hash, 0xffff when missing (FUN_1411fed70).
uint16_t M2FindSequenceById(uint32_t id, const M2Array<uint16_t>* hash, const M2Array<M2Sequence>* sequences);

#endif
