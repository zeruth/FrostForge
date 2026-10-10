#ifndef SHADEREFFECT_SHADER_EFFECT_PIPELINE_HPP
#define SHADEREFFECT_SHADER_EFFECT_PIPELINE_HPP

// The pipelines a CShaderEffect draws with, by what they were built from, and the permutation
// indices the model draws pick their vertex and pixel shaders by (ShaderEffect.cpp, 0x1412ac000
// and on).

#include <cstdint>

class CShaderEffect;
struct GxPipeline;

// What a pipeline was asked for (0x20): filled by the lookup and read back from a pipeline.
struct SHADEREFFECTREQUEST {
    uint64_t m_depthStencil;            // 0x00
    uint16_t m_blend;                   // 0x08
    uint16_t m_pad0a;
    uint32_t m_raster;                  // 0x0c
    uint8_t m_vertexFormat;             // 0x10
    uint8_t m_topology;                 // 0x11
    uint16_t m_pad12;
    uint32_t m_targetKind;              // 0x14
    uint16_t m_vsPermute;               // 0x18
    uint16_t m_psPermute;               // 0x1a
    uint8_t m_extra;                    // 0x1c
    uint8_t m_pad1d[3];
};
static_assert(sizeof(SHADEREFFECTREQUEST) == 0x20, "a shader effect request is 0x20 bytes");

// The effect's pipeline for these states, built when missing (FUN_1412ac8a0); `request` is
// filled with what was asked when not null.
GxPipeline* ShaderEffectGetPipeline(CShaderEffect* effect, uint16_t vsPermute, uint16_t psPermute,
    uint64_t depthStencil, uint16_t blend, uint32_t raster, uint8_t vertexFormat, uint8_t topology,
    uint32_t targetKind, SHADEREFFECTREQUEST* request);
// What a pipeline of the effect was built from (FUN_1412ae030).
bool ShaderEffectFindRequest(CShaderEffect* effect, GxPipeline* pipeline, SHADEREFFECTREQUEST* request);

// The object fill (FUN_1412b66c0) and outline (FUN_1412b0750) pipelines.
GxPipeline* ObjectFillGetPipeline(uint8_t kind, uint32_t variant, uint64_t depthStencil, uint32_t raster,
    uint16_t blend, uint8_t vertexFormat, uint8_t topology, uint32_t targetKind);
GxPipeline* OutlineGetPipeline(uint8_t skinned, uint8_t extra, uint16_t base, uint64_t depthStencil, uint32_t raster,
    uint16_t blend, uint8_t vertexFormat, uint8_t topology, uint32_t targetKind);
// The object fill effect's texture and depth-stencil state, made with the device
// (FUN_1412b6b20) and released with it with the effect's pipelines (FUN_1412b6bb0); the setup
// registering both (FUN_1412b6af0) and its shutdown (FUN_1412b6670).
void ObjectFillInitialize();
void ObjectFillShutdown();
// ref: FUN_1412b66b0
uint64_t ObjectFillDepthStencil();
// Whether the GPU pulls its own vertices (its device data's +0x224 bit 5).
uint32_t ShaderEffectVertexPull();
// Whether any scene draws outlines (FUN_1412b2120).
bool ShaderEffectOutlinesOn();

// Vertex shader permutations: the model's (FUN_1412ad080) and the prepass's (FUN_1412ad0f0).
uint16_t ShaderEffectModelVertexPermute(uint8_t a1, uint8_t a2, uint8_t a3, uint32_t a4, uint16_t a5, uint16_t a6,
    uint16_t a7, uint16_t a8);
uint16_t ShaderEffectPrepassVertexPermute(uint8_t a1, uint8_t a2, uint32_t a3, uint8_t a4, uint16_t a5);
// Pixel shader permutations: the model's (FUN_1412acc90), its unlit form (FUN_1412ace70), the
// projected one (FUN_1412ace40), the particles' (FUN_1412acde0) and the prepass's
// (FUN_1412acd30, the value itself).
uint16_t ShaderEffectModelPixelPermute(uint8_t a1, uint16_t a2, uint16_t a3, uint8_t a4, uint8_t a5, uint8_t a6,
    uint8_t a7, uint16_t a8);
uint16_t ShaderEffectUnlitPixelPermute(uint8_t a1, uint16_t a2, uint8_t a3, uint16_t a4, uint16_t a5, uint16_t a6,
    uint16_t a7);
uint16_t ShaderEffectProjectedPixelPermute(uint16_t a1);
uint16_t ShaderEffectParticlePixelPermute(uint8_t a1, uint16_t a2, uint16_t a3);
uint16_t ShaderEffectPrepassPixelPermute(uint8_t a1);

#endif
