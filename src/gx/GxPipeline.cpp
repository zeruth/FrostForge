#include "gx/GxPipeline.hpp"

#include <cstring>

#include "blz/Murmur.hpp"
#include "gx/Gx.hpp"
#include "gx/GxDevice.hpp"
#include "gx/GxShader.hpp"
#include "gx/GxVertexFormat.hpp"

namespace {

// FNV-1a over the first eight bytes of a layout (FUN_142291810).
uint64_t LayoutHash(const GXVERTEXBUFFERFORMAT* layout) {
    auto bytes = reinterpret_cast<const uint8_t*>(layout);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < 8; i++) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    }
    return hash;
}

}

// The key the device caches pipelines by: Murmur3 over the shaders, blend, raster, layouts and
// formats, with the depth-stencil state's own hash in the low ten bits.
// ref: FUN_14228b3b0
GxPipeline* GxPipelineCreateDesc(const GXPIPELINEDESC* desc, const char* name) {
    uint8_t key[0x30] = {};
    auto bytes = reinterpret_cast<const uint8_t*>(desc);
    std::memcpy(key + 0x00, &desc->m_pixelShader, 4);
    std::memcpy(key + 0x04, &desc->m_vertexShader, 4);
    std::memcpy(key + 0x08, &desc->m_psPermute, 2);
    std::memcpy(key + 0x0a, &desc->m_vsPermute, 2);
    std::memcpy(key + 0x0c, bytes + 0x44, 8);
    std::memcpy(key + 0x14, &desc->m_4e, 2);
    key[0x16] = desc->m_topology;
    key[0x17] = desc->m_samplesLog2;
    std::memcpy(key + 0x18, bytes + 0x08, 4);
    key[0x1a] = static_cast<uint8_t>((bytes[0x0a] & 7) | 0xa8);
    uint64_t layouts = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < desc->m_layoutCount; i++) {
        uint64_t hash = LayoutHash(desc->m_layouts[i]);
        for (uint32_t k = 0; k < 8; k++) {
            layouts = (layouts ^ ((hash >> (k * 8)) & 0xff)) * 0x100000001b3ull;
        }
    }
    std::memcpy(key + 0x20, &layouts, 8);
    std::memcpy(key + 0x28, desc->m_targetFormats, 4);
    key[0x2c] = desc->m_depthFormat;
    uint64_t state[2];
    uint64_t depthStencil[2];
    blz::murmur3_128(state, key, sizeof(key), 0);
    blz::murmur3_128(depthStencil, bytes + 0x10, 8, 0);
    uint64_t hash = (state[0] & ~0x3ffull) | (depthStencil[0] & 0x3ff);
    return static_cast<CGxDevice*>(g_gxDevice)->PipelineCreate(desc, hash, name);
}

// The key's shader indices become the family's FileDataIDs and its vertex format the device's
// layouts.
// ref: FUN_14228b580
GxPipeline* GxPipelineCreate(const GXPIPELINEKEY* key, const char* name) {
    GXPIPELINEDESC desc;
    std::memcpy(&desc, key, 0x28);
    CGxDevice::VERTEXFORMATREF layouts;
    static_cast<CGxDevice*>(g_gxDevice)->GetVertexFormat(&layouts, key->m_vertexFormat);
    desc.m_layouts = layouts.m_layouts;
    desc.m_layoutCount = layouts.m_state;
    desc.m_vertexShader = GxShaderVertexId(static_cast<int32_t>(key->m_vertexShader));
    desc.m_geometryShader = GxShaderGeometryId(static_cast<int32_t>(key->m_geometryShader));
    desc.m_pixelShader = GxShaderPixelId(static_cast<int32_t>(key->m_pixelShader));
    desc.m_34 = 0;
    desc.m_alphaToCoverage = 0;
    desc.m_independentBlend = 0;
    std::memset(desc.m_blend, 0, sizeof(desc.m_blend));
    std::memcpy(desc.m_blend[0], &key->m_blend, 2);
    desc.m_4e = 0;
    return GxPipelineCreateDesc(&desc, name);
}
