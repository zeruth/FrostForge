#ifndef SHADEREFFECT_SHADER_EFFECT_SHADOW_HPP
#define SHADEREFFECT_SHADER_EFFECT_SHADOW_HPP

// The shadow maps the model draws sample (ShaderEffect.cpp, 0x1412b2000 and on): the cascades
// of each kind and their textures, filled by the shadow renderer (FUN_1412b26a0, not yet
// ported; until it is every count stays 0 and the binds below fall back as the reference does
// before its first shadow frame).

#include <cstdint>

class GxRenderContext;

// One cascade (0x70): its texture first.
struct SHADOWCASCADE {
    void* m_tex;                        // 0x00
    uint8_t m_08[0x68];
};
static_assert(sizeof(SHADOWCASCADE) == 0x70, "a shadow cascade is 0x70 bytes");

// The cascades in use by kind (DAT_1430a7950), each kind's cascades (DAT_1430a6130, 0x1c0
// apart), the texture the slots past them get (DAT_1430a5d80), the cascades' constants
// (DAT_1430a64b0, 0x30 bytes) and the map the scene-less binds take (DAT_1430a7c98).
extern uint16_t g_shadowCascadeCount[2];
extern SHADOWCASCADE g_shadowCascades[2][4];
extern void* g_shadowDefaultTex;
extern float g_shadowConstants[12];
extern void* g_shadowMapTex;
// The shadow band's constants (DAT_1430a7960, 0x60 bytes) and its scale over the band's texture
// size (DAT_1430a79c0, 0x20), both filled by the shadow renderer.
extern float g_shadowBandConstants[24];
extern float g_shadowBandScale[8];

// Copies of them (FUN_1412b3f60, FUN_1412b3fb0).
void ShaderEffectShadowBandConstants(float* out);
void ShaderEffectShadowBandScale(float* out);

// The shadow textures to stage 2 (FUN_1412b5c40): white at slot 0xc when off, else each
// cascade from slot 6 with the default and its sampler.
void ShaderEffectBindShadows(GxRenderContext* context, int32_t on);
// The same for a scene that keeps its own (FUN_1412b5cf0): `shadows` +0x48 when off, else the
// cascades of `kind`, the default up to the kind's slots, and the cascade constants.
void ShaderEffectBindSceneShadows(GxRenderContext* context, int32_t on, uint32_t kind, const uint8_t* shadows);
// The shadow parameters a batch reads (FUN_1412b3fd0): {0, cascades, 20 over the last
// cascade's distance, 0}, or {1 when `lit`, 0, 0, 0} when off.
float* ShaderEffectShadowParams(float* out, GxRenderContext* context, int32_t on, int32_t lit, uint32_t kind);
// A shadow map to stage 2 slot 0xb, the global one for none (FUN_1412bca90).
void ShaderEffectBindShadowMap(GxRenderContext* context, void* tex);

#endif
