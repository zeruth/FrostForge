#include "shadereffect/ShaderEffectPipeline.hpp"

#include <cstring>

#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxShader.hpp"
#include "os/Sync.hpp"
#include "shadereffect/PipelineMap.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "base/Status.hpp"
#include "texture/Texture.hpp"

namespace {

// The object fill effect's pipelines (DAT_142cfc940, new ones DAT_142cfc960 under
// DAT_1430a79f0) and the outline's (DAT_142cfc3c0, DAT_142cfc3e0 under DAT_1430a5ce0).
blz::chained_hash_header s_fillPipelines;
blz::chained_hash_header s_fillNewPipelines;
OSMUTEX s_fillLock;
blz::chained_hash_header s_outlinePipelines;
blz::chained_hash_header s_outlineNewPipelines;
OSMUTEX s_outlineLock;

// The fill's texture (DAT_1430a79f8, FileDataID 1273707) and depth-stencil state (DAT_1430a7a00).
HTEXTURE s_fillTexture;
uint64_t s_fillDepthStencil;

// ref: FUN_1412b6b20
void ObjectFillOnDeviceCreated(void* param) {
    (void)param;
    CStatus status;
    s_fillTexture = TextureCreate(0x136f6b, 0x82091, &status, 0, 3, 0);
    s_fillDepthStencil = 0x1f1bull | (6ull << 0x20);
}

// ref: FUN_1412b6bb0
void ObjectFillOnDeviceDestroyed(void* param) {
    (void)param;
    TextureFree(&s_fillTexture);
    PipelineMapDestroyPipelines(&s_fillPipelines);
    PipelineMapClear(&s_fillPipelines);
    PipelineMapDestroyPipelines(&s_fillNewPipelines);
    PipelineMapClear(&s_fillNewPipelines);
}

struct LOCKINIT {
    LOCKINIT() {
        OsMutexInit(&s_fillLock);
        OsMutexInit(&s_outlineLock);
    }
};
LOCKINIT s_lockInit;

// The object fill's shader for a kind and variant (DAT_142727738: {kind, pad, variant}).
struct FILLSHADER {
    uint8_t m_kind;
    uint8_t m_pad;
    uint16_t m_variant;
};
const FILLSHADER s_fillShaders[6] = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 0, 2 }, { 1, 0, 0 }, { 1, 0, 1 }, { 1, 0, 2 } };

// A pipeline from a key under a lock, through a pair of maps (the body FUN_1412b66c0 and
// FUN_1412b0750 share).
GxPipeline* GetKeyedPipeline(const PIPELINEHASH& hash, blz::chained_hash_header* cache, blz::chained_hash_header* fresh,
    OSMUTEX* lock, const GXPIPELINEKEY& key, const char* name, uint64_t owner) {
    if (PIPELINENODE* cached = PipelineMapFind(cache, hash)) {
        return cached->m_pipeline;
    }
    OsMutexLock(lock);
    GxPipeline* pipeline;
    if (PIPELINENODE* existing = PipelineMapFind(fresh, hash)) {
        pipeline = existing->m_pipeline;
    } else {
        pipeline = GxPipelineCreate(&key, name);
        PipelineMapInsert(fresh, hash)->m_pipeline = pipeline;
        TRANSIENTPIPELINEMAP transient = { owner, fresh, cache };
        TransientPipelineMapAdd(&transient);
    }
    OsMutexUnlock(lock);
    return pipeline;
}

}  // namespace

// ref: FUN_1412ac8a0
GxPipeline* ShaderEffectGetPipeline(CShaderEffect* effect, uint16_t vsPermute, uint16_t psPermute,
    uint64_t depthStencil, uint16_t blend, uint32_t raster, uint8_t vertexFormat, uint8_t topology,
    uint32_t targetKind, SHADEREFFECTREQUEST* request) {
    if (request != nullptr) {
        request->m_vsPermute = vsPermute;
        request->m_psPermute = psPermute;
        request->m_depthStencil = depthStencil;
        request->m_blend = blend;
        request->m_raster = raster;
        request->m_vertexFormat = vertexFormat;
        request->m_topology = topology;
        request->m_targetKind = targetKind;
    }
    PIPELINEHASH hash;
    PipelineHashMake(&hash, vsPermute, psPermute, depthStencil, blend, raster, vertexFormat, topology & 7, targetKind, 0);
    if (PIPELINENODE* cached = PipelineMapFind(&effect->m_pipelines, hash)) {
        return cached->m_pipeline;
    }
    OsMutexLock(&effect->m_lock);
    GxPipeline* pipeline;
    PIPELINENODE* existing = PipelineMapFind(&effect->m_newPipelines, hash);
    if (existing != nullptr) {
        pipeline = existing->m_pipeline;
    } else {
        GXPIPELINEDESC desc;
        std::memset(&desc, 0, sizeof(desc));
        desc.m_vsPermute = vsPermute;
        desc.m_gsPermute = 0;
        desc.m_psPermute = psPermute;
        desc.m_topology = topology;
        std::memcpy(&desc.m_cull, &raster, 4);
        std::memcpy(&desc.m_depthFlags, &depthStencil, 8);
        desc.m_vertexShader = GxShaderVertexId(static_cast<int32_t>(effect->m_vertexShader));
        desc.m_pixelShader = GxShaderPixelId(static_cast<int32_t>(effect->m_pixelShader));
        std::memcpy(desc.m_blend[0], &blend, 2);
        if (targetKind == 7 && GxDeviceSupports(0xa100, 2)) {
            desc.m_independentBlend = 1;
            desc.m_blend[1][0] = 10;
            desc.m_blend[1][1] = static_cast<uint8_t>(((blend >> 8) & 0xf3) | 3);
        }
        GXVERTEXBUFFERFORMAT** layouts = nullptr;
        desc.m_layoutCount = GxVertexFormatLayouts(vertexFormat, &layouts);
        desc.m_layouts = layouts;
        RENDERTARGETFORMATS formats = RenderTargetFormats(targetKind);
        std::memcpy(desc.m_targetFormats, &formats, 4);
        desc.m_depthFormat = formats.m_depth;
        desc.m_samplesLog2 = targetKind - 6 < 2 ? g_renderSettings.m_msaaSamples : 0;
        PIPELINENODE* node = PipelineMapInsert(&effect->m_newPipelines, hash);
        pipeline = GxPipelineCreateDesc(&desc, "CShaderEffect");
        node->m_pipeline = pipeline;
        TRANSIENTPIPELINEMAP transient = { reinterpret_cast<uint64_t>(effect), &effect->m_newPipelines, &effect->m_pipelines };
        TransientPipelineMapAdd(&transient);
    }
    OsMutexUnlock(&effect->m_lock);
    return pipeline;
}

namespace {

// A key read back into a request (the body FUN_1412ae030 has twice).
void ReadKey(const PIPELINENODE* node, SHADEREFFECTREQUEST* request) {
    uint64_t bits = node->m_key.m_bits;
    request->m_depthStencil = node->m_key.m_depthStencil;
    request->m_blend = node->m_key.m_blend;
    request->m_raster = node->m_key.m_raster;
    request->m_vsPermute = static_cast<uint16_t>(bits >> 0x10);
    request->m_psPermute = static_cast<uint16_t>(bits);
    request->m_topology = static_cast<uint8_t>(bits >> 0x20) & 7;
    request->m_vertexFormat = static_cast<uint8_t>(bits >> 0x23);
    request->m_targetKind = static_cast<uint32_t>(bits >> 0x2d) & 0x3ff;
    request->m_extra = static_cast<uint8_t>(bits >> 0x37);
}

const PIPELINENODE* FindByPipeline(const blz::chained_hash_header* map, const GxPipeline* pipeline) {
    if (map->m_buckets == nullptr) {
        return nullptr;
    }
    for (uint64_t i = 0; i < map->m_bucketCount; i++) {
        for (auto node = static_cast<const PIPELINENODE*>(map->m_buckets[i]); node != nullptr; node = node->m_next) {
            if (node->m_pipeline == pipeline) {
                return node;
            }
        }
    }
    return nullptr;
}

}  // namespace

// ref: FUN_1412ae030
bool ShaderEffectFindRequest(CShaderEffect* effect, GxPipeline* pipeline, SHADEREFFECTREQUEST* request) {
    if (const PIPELINENODE* node = FindByPipeline(&effect->m_pipelines, pipeline)) {
        ReadKey(node, request);
        return true;
    }
    bool found = false;
    OsMutexLock(&effect->m_lock);
    if (const PIPELINENODE* node = FindByPipeline(&effect->m_newPipelines, pipeline)) {
        ReadKey(node, request);
        found = true;
    }
    OsMutexUnlock(&effect->m_lock);
    return found;
}

// ref: FUN_1412b66c0
GxPipeline* ObjectFillGetPipeline(uint8_t kind, uint32_t variant, uint64_t depthStencil, uint32_t raster,
    uint16_t blend, uint8_t vertexFormat, uint8_t topology, uint32_t targetKind) {
    uint32_t i = 0;
    for (; i < 6; i++) {
        if (s_fillShaders[i].m_kind == kind && s_fillShaders[i].m_variant == variant) {
            break;
        }
    }
    bool second = i < 6 && s_fillShaders[i].m_kind != 0;
    uint16_t vsPermute = static_cast<uint16_t>((second ? 3 : 0) + (i < 6 ? s_fillShaders[i].m_variant : 0));
    PIPELINEHASH hash;
    PipelineHashMake(&hash, vsPermute, second, depthStencil, blend, raster, vertexFormat, topology, targetKind, 0);
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_vsPermute = vsPermute;
    key.m_psPermute = second;
    key.m_topology = topology;
    key.m_samplesLog2 = g_renderSettings.m_msaaSamples;
    std::memcpy(key.m_raster, &raster, 4);
    std::memcpy(key.m_depthStencil, &depthStencil, 8);
    RENDERTARGETFORMATS formats = RenderTargetFormats(targetKind);
    std::memcpy(key.m_targetFormats, &formats, 4);
    key.m_depthFormat = formats.m_depth;
    key.m_vertexShader = 0x61;
    key.m_pixelShader = 0xa7;
    key.m_vertexFormat = vertexFormat;
    key.m_blend = blend;
    return GetKeyedPipeline(hash, &s_fillPipelines, &s_fillNewPipelines, &s_fillLock, key, "ObjectFillEffect", 2);
}

// ref: FUN_1412b0750
GxPipeline* OutlineGetPipeline(uint8_t skinned, uint8_t extra, uint16_t base, uint64_t depthStencil, uint32_t raster,
    uint16_t blend, uint8_t vertexFormat, uint8_t topology, uint32_t targetKind) {
    uint16_t vsPermute = static_cast<uint16_t>((skinned * 2 + extra) * 3 + base);
    PIPELINEHASH hash;
    PipelineHashMake(&hash, vsPermute, extra, depthStencil, blend, raster, vertexFormat, topology, targetKind, 0);
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_vsPermute = vsPermute;
    key.m_psPermute = extra;
    key.m_topology = topology;
    key.m_samplesLog2 = 0;
    std::memcpy(key.m_raster, &raster, 4);
    std::memcpy(key.m_depthStencil, &depthStencil, 8);
    RENDERTARGETFORMATS formats = RenderTargetFormats(targetKind);
    std::memcpy(key.m_targetFormats, &formats, 4);
    key.m_depthFormat = formats.m_depth;
    key.m_vertexShader = 99;
    key.m_pixelShader = 0xa9;
    key.m_vertexFormat = vertexFormat;
    key.m_blend = blend;
    return GetKeyedPipeline(hash, &s_outlinePipelines, &s_outlineNewPipelines, &s_outlineLock, key, "OutlineBuffer", 3);
}

uint32_t ShaderEffectVertexPull() {
    auto data = reinterpret_cast<const uint8_t*>(GxSelectGpuToUse()->m_data);
    return (*reinterpret_cast<const uint32_t*>(data + 0x224) >> 5) & 1;
}

// ref: FUN_1412b6af0
void ObjectFillInitialize() {
    GxAddDeviceCreatedCallback(ObjectFillOnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(ObjectFillOnDeviceDestroyed, nullptr);
}

// ref: FUN_1412b6670
void ObjectFillShutdown() {
    TextureFree(&s_fillTexture);
    GxRemoveDeviceCreatedCallback(ObjectFillOnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(ObjectFillOnDeviceDestroyed, nullptr);
    ObjectFillOnDeviceDestroyed(nullptr);
}

uint64_t ObjectFillDepthStencil() {
    return s_fillDepthStencil;
}

// ref: FUN_1412b2120
bool ShaderEffectOutlinesOn() {
    return g_renderSettings.m_outlines != 0;
}

// ref: FUN_1412ad080
uint16_t ShaderEffectModelVertexPermute(uint8_t a1, uint8_t a2, uint8_t a3, uint32_t a4, uint16_t a5, uint16_t a6,
    uint16_t a7, uint16_t a8) {
    uint16_t level = a4 < 3 ? static_cast<uint16_t>(a4) : 2;
    uint32_t value = static_cast<uint32_t>(static_cast<uint16_t>(
        ((((((a8 * 2 + a7) * 2 + a1) * 2 + a5) * 3 + level) * 2 + a3) * 2 + a2))) * 2;
    return static_cast<uint16_t>(value + a6);
}

// ref: FUN_1412ad0f0
uint16_t ShaderEffectPrepassVertexPermute(uint8_t a1, uint8_t a2, uint32_t a3, uint8_t a4, uint16_t a5) {
    uint16_t level = a3 < 3 ? static_cast<uint16_t>(a3) : 2;
    uint32_t value = static_cast<uint32_t>(static_cast<uint16_t>((((a5 * 2 + a1) * 3 + level) * 2 + a4))) * 2;
    return static_cast<uint16_t>(value + a2);
}

// ref: FUN_1412acc90
uint16_t ShaderEffectModelPixelPermute(uint8_t a1, uint16_t a2, uint16_t a3, uint8_t a4, uint8_t a5, uint8_t a6,
    uint8_t a7, uint16_t a8) {
    uint32_t pull = ShaderEffectVertexPull();
    uint32_t value = ((((((((pull + 2) * 2 + a8) * 2 + a6) * 2 + a4) * 2 + a5) * 2 + a1) * 2 + a3) * 2 + a2) * 2;
    return static_cast<uint16_t>(value + a7);
}

// ref: FUN_1412ace70
uint16_t ShaderEffectUnlitPixelPermute(uint8_t a1, uint16_t a2, uint8_t a3, uint16_t a4, uint16_t a5, uint16_t a6,
    uint16_t a7) {
    uint32_t pull = ShaderEffectVertexPull();
    uint32_t value = ((((((pull + 2) * 2 + a7) * 2 + a6) * 2 + a1) * 2 + a2) * 2 + a3) * 2 + a4;
    value *= 2;
    return static_cast<uint16_t>(value + a5);
}

// ref: FUN_1412ace40
uint16_t ShaderEffectProjectedPixelPermute(uint16_t a1) {
    return static_cast<uint16_t>((ShaderEffectVertexPull() + 2) * 2 + a1);
}

// ref: FUN_1412acde0
uint16_t ShaderEffectParticlePixelPermute(uint8_t a1, uint16_t a2, uint16_t a3) {
    return static_cast<uint16_t>((((ShaderEffectVertexPull() + 2) * 2 + a3) * 2 + a1) * 2 + a2);
}

// ref: FUN_1412acd30
uint16_t ShaderEffectPrepassPixelPermute(uint8_t a1) {
    return a1;
}
