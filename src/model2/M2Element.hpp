#ifndef MODEL2_M2_ELEMENT_HPP
#define MODEL2_M2_ELEMENT_HPP

// M2Element (0x88): one draw a scene's draw list holds -- a model's batch, shadow batch or
// particle emitter with the pipeline it draws with and the keys the lists are sorted by -- and
// Model3::PipelineCache, the per-scene cache of the pipelines those draws use.

#include <cstddef>
#include <cstdint>

#include "os/Sync.hpp"

class CM2BaseModel;
class CParticleEmitter2;
struct GxPipeline;
struct GXVERTEXBUFFERFORMAT;
struct M2SkinSection;

struct M2Element {
    // Bits 0-1 the model's LOD (3 past 2), 2 a flag, 3-30 the model's file, 31-60 the pipeline,
    // 61-63 the kind.
    uint64_t m_sortKey;                 // 0x00
    // Bits 3-17 the batch, 18-26 the material, 27-31 and 32-35 the textures, 36-39 the
    // section's level, 40-63 the model.
    uint64_t m_batchKey;                // 0x08
    CM2BaseModel* m_model;              // 0x10
    uint32_t m_18;                      // 0x18
    uint32_t m_flags;                   // 0x1c 0x80 faded, 0x4000 a shadow batch, 0x8000 ...
    float m_depth;                      // 0x20
    float m_screenSize;                 // 0x24 the model's
    uint32_t m_pass;                    // 0x28
    uint32_t m_2c;                      // 0x2c
    union {
        uint32_t m_batchIndex;          // 0x30
        CParticleEmitter2* m_emitter;   // 0x30
    };
    uint32_t m_38;                      // 0x38
    uint32_t m_3c;                      // 0x3c
    const void* m_batch;                // 0x40 the M2Batch or the shadow batch
    const M2SkinSection* m_section;     // 0x48
    GxPipeline* m_pipeline;             // 0x50
    uint64_t m_vertexFormat;            // 0x58
    int32_t m_60;                       // 0x60 -1
    uint32_t m_64;                      // 0x64
    uint64_t m_68;                      // 0x68
    uint16_t m_70;                      // 0x70
    uint16_t m_pad72;
    uint32_t m_74;                      // 0x74
    uint8_t m_78[0x10];                 // 0x78

    // ref: FUN_141205160
    void Initialize(uint32_t kind, uint64_t a3, CM2BaseModel* model, const M2SkinSection* section, float depth,
        float screenSize, uint32_t pass, uint32_t a9, uint32_t flags);
    // The emitter drawn (FUN_1412052c0).
    void SetEmitter(CParticleEmitter2* emitter);
    // The pipeline drawn with (FUN_141205340).
    void SetPipeline(GxPipeline* pipeline);
    // A batch (FUN_141205370) with its texture combo; a ribbon (FUN_141205410); a shadow batch
    // (FUN_141205510).
    void SetBatch(uint32_t index, const void* batch, const uint16_t* textures);
    void SetRibbon(uint32_t index);
    void SetShadowBatch(uint32_t index, const void* batch);
    // ref: FUN_141205560
    void SetFaded(bool faded);
};
static_assert(sizeof(M2Element) == 0x88, "M2Element is 0x88 bytes");
static_assert(offsetof(M2Element, m_pipeline) == 0x50, "M2Element pipeline at 0x50");

// What a pipeline is asked for by (0x3c, read by FUN_1412059f0).
struct M3PIPELINEREQUEST {
    uint32_t m_vertexShader;            // 0x00
    uint32_t m_pixelShader;             // 0x04
    uint16_t m_vsPermute;               // 0x08
    uint16_t m_pad0a;
    uint16_t m_psPermute;               // 0x0c
    uint16_t m_pad0e;
    GXVERTEXBUFFERFORMAT** m_layouts;   // 0x10
    uint32_t m_layoutCount;             // 0x18
    uint32_t m_pad1c;
    uint64_t m_depthStencil;            // 0x20
    uint32_t m_raster;                  // 0x28
    uint8_t m_blend[8];                 // 0x2c
    uint16_t m_34;                      // 0x34
    uint16_t m_pad36;
    uint32_t m_targetKind;              // 0x38
};

// Model3::PipelineCache::Key and its pipeline (0x98): the key carries its vertex layouts inline,
// and byte 0x27 is the bucket's state (0 used, 1 empty, 2 deleted).
struct M3PIPELINEENTRY {
    uint64_t m_depthStencil;            // 0x00
    uint32_t m_raster;                  // 0x08
    uint32_t m_vertexShader;            // 0x0c
    uint32_t m_pixelShader;             // 0x10
    uint16_t m_vsPermute;               // 0x14
    uint16_t m_psPermute;               // 0x16
    uint32_t m_targetKind;              // 0x18
    uint8_t m_blend[8];                 // 0x1c
    uint16_t m_24;                      // 0x24
    uint8_t m_layoutCount;              // 0x26
    uint8_t m_state;                    // 0x27
    GXVERTEXBUFFERFORMAT* m_layouts[0xd];  // 0x28
    GxPipeline* m_pipeline;             // 0x90
};
static_assert(sizeof(M3PIPELINEENTRY) == 0x98, "a pipeline cache entry is 0x98 bytes");

// Model3::PipelineCache (0x50, FUN_1411d0040): an open-addressed table read without the lock,
// and a deque of the pipelines built since, under it.
struct M2PipelineCache {
    uint64_t m_bucketCount;             // 0x00 8
    M3PIPELINEENTRY* m_buckets;         // 0x08
    uint64_t m_count;                   // 0x10
    uint64_t m_deleted;                 // 0x18
    float m_maxLoad;                    // 0x20 0.5
    uint32_t m_pad24;
    M3PIPELINEENTRY** m_blocks;         // 0x28 eight entries a block
    uint64_t m_blockCount;              // 0x30
    uint64_t m_first;                   // 0x38
    uint64_t m_size;                    // 0x40
    OSMUTEX m_lock;                     // 0x48

    M2PipelineCache();
    // The pipeline slot for a key, the pipeline built when missing (FUN_141205580).
    GxPipeline** Get(const M3PIPELINEENTRY* key);
    // A key built from a request, and its pipeline (FUN_1412059f0).
    GxPipeline* Get(const M3PIPELINEREQUEST* request);
    // A new entry at the deque's end (FUN_141205b10).
    M3PIPELINEENTRY* PushBack();
    // Every pipeline destroyed and both containers emptied (FUN_1411d9360).
    void Clear();
};
static_assert(sizeof(M2PipelineCache) == 0x50, "M2PipelineCache is 0x50 bytes");

// The shared depth-stencil states' targets (DAT_1430a12e0, FUN_141205970 when the device comes
// up and FUN_1412059c0 when it goes), and the block a pass draws with (FUN_141205ab0).
void M2ElementDeviceCreated();
void M2ElementDeviceLost();
const void* M2ElementSelectBlock(int32_t pass, const void* block);

#endif
