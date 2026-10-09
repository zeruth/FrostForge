#ifndef GX_GX_PIPELINE_HPP
#define GX_GX_PIPELINE_HPP

// Gx's pipelines (Engine/Source/Gx): the shaders, vertex layout, blend, raster and
// depth-stencil state and target formats a draw runs with, built once by the device and bound
// as one object.

#include <cstdint>

struct GXVERTEXBUFFERFORMAT;

// What callers describe a pipeline with (0x38 bytes): shader indices into the family's tables
// and the vertex format by number.
struct GXPIPELINEKEY {
    uint16_t m_vsPermute;       // 0x00
    uint16_t m_gsPermute;       // 0x02
    uint16_t m_psPermute;       // 0x04
    uint8_t m_topology;         // 0x06
    uint8_t m_samplesLog2;      // 0x07
    uint8_t m_raster[8];        // 0x08: cull, depth bias, flags, ...
    uint8_t m_depthStencil[8];  // 0x10: flags, stencil masks, function and operations
    uint8_t m_targetFormats[4]; // 0x18
    uint8_t m_depthFormat;      // 0x1c
    uint8_t m_1d[0xb];
    uint32_t m_vertexShader;    // 0x28: an index into the vertex shaders
    uint32_t m_geometryShader;  // 0x2c
    uint32_t m_pixelShader;     // 0x30
    uint8_t m_vertexFormat;     // 0x34
    uint8_t m_35;
    uint16_t m_blend;           // 0x36: the first target's {mode, operation | write mask << 4}
};
static_assert(sizeof(GXPIPELINEKEY) == 0x38, "GXPIPELINEKEY is 0x38 bytes");

// What the device builds a pipeline from (0x50 bytes).
struct GXPIPELINEDESC {
    uint16_t m_vsPermute;       // 0x00
    uint16_t m_gsPermute;       // 0x02
    uint16_t m_psPermute;       // 0x04
    uint8_t m_topology;         // 0x06
    uint8_t m_samplesLog2;      // 0x07
    uint8_t m_cull;             // 0x08
    uint8_t m_depthBias;        // 0x09
    uint8_t m_rasterFlags;      // 0x0a: 2 wireframe, 4 no depth clip
    uint8_t m_0b[5];
    uint8_t m_depthFlags;       // 0x10: 1 test, 2 stencil, 4 write, function << 3
    uint8_t m_stencilReadMask;  // 0x11
    uint8_t m_stencilWriteMask; // 0x12
    uint8_t m_13;
    uint8_t m_stencilFunc;      // 0x14
    uint8_t m_stencilFail;      // 0x15
    uint8_t m_stencilDepthFail; // 0x16
    uint8_t m_stencilPass;      // 0x17
    uint8_t m_targetFormats[4]; // 0x18
    uint8_t m_depthFormat;      // 0x1c
    uint8_t m_1d[0xb];
    int32_t m_vertexShader;     // 0x28: FileDataIDs
    int32_t m_geometryShader;   // 0x2c
    int32_t m_pixelShader;      // 0x30
    uint32_t m_34;
    GXVERTEXBUFFERFORMAT** m_layouts;  // 0x38
    uint32_t m_layoutCount;     // 0x40
    uint8_t m_alphaToCoverage;  // 0x44
    uint8_t m_independentBlend; // 0x45
    uint8_t m_blend[4][2];      // 0x46: {mode, operation | write mask << 4} per target
    uint16_t m_4e;
};
static_assert(sizeof(GXPIPELINEDESC) == 0x50, "GXPIPELINEDESC is 0x50 bytes");

// The device's pipeline object; its contents are the device's.
struct GxPipeline;

// Builds a pipeline (FUN_14228b580) and one from a full desc (FUN_14228b3b0); `name` is for
// messages.
GxPipeline* GxPipelineCreate(const GXPIPELINEKEY* key, const char* name);
GxPipeline* GxPipelineCreateDesc(const GXPIPELINEDESC* desc, const char* name);

#endif
