#include "shadereffect/ShaderEffectShadow.hpp"

#include <cstring>

#include "client/RenderSettings.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "texture/Texture.hpp"

uint16_t g_shadowCascadeCount[2];
SHADOWCASCADE g_shadowCascades[2][4];
void* g_shadowDefaultTex;
float g_shadowConstants[12];
void* g_shadowMapTex;

namespace {

// The slots each kind binds (DAT_142cfc868, 10 bytes apart) and the cascades' far distances
// (DAT_142cfc8e0), behind the 1 stored before them (DAT_142cfc8dc) that a kind with no cascades
// reads.
const uint16_t s_shadowSlots[2] = { 4, 1 };
const float s_cascadeDistances[5] = { 1.0f, 20.0f, 60.0f, 180.0f, 540.0f };
// The shadow sampler (a comparison sampler).
const uint64_t s_shadowSampler = 0x800300000001ull;

gxDx12Backend::Tex* AsTex(void* tex) {
    return static_cast<gxDx12Backend::Tex*>(tex);
}

}  // namespace

// ref: FUN_1412b5c40
void ShaderEffectBindShadows(GxRenderContext* context, int32_t on) {
    if (on == 0) {
        context->SetTexture(2, 0xc, AsTex(TextureWhiteGxTex()));
        return;
    }
    uint32_t count = g_shadowCascadeCount[0];
    for (uint32_t i = 0; i < count; i++) {
        context->BindTexture(2, i + 6, AsTex(g_shadowDefaultTex), 0);
    }
    context->SetSampler(2, 6, s_shadowSampler);
}

// ref: FUN_1412b5cf0
void ShaderEffectBindSceneShadows(GxRenderContext* context, int32_t on, uint32_t kind, const uint8_t* shadows) {
    if (g_renderSettings.m_shadowMode == 0) {
        return;
    }
    if (on == 0) {
        context->SetTexture(2, 0xc, AsTex(*reinterpret_cast<void* const*>(shadows + 0x48)));
        return;
    }
    uint32_t count = g_shadowCascadeCount[kind];
    for (uint32_t i = 0; i < count; i++) {
        if (i == 0) {
            context->SetTexture(2, i + 6, AsTex(g_shadowCascades[kind][i].m_tex));
        } else {
            context->BindTexture(2, i + 6, AsTex(g_shadowCascades[kind][i].m_tex), 0);
        }
    }
    if (g_shadowDefaultTex != nullptr) {
        for (uint32_t i = count; i < s_shadowSlots[kind]; i++) {
            if (i == 0) {
                context->SetTexture(2, i + 6, AsTex(g_shadowDefaultTex));
            } else {
                context->BindTexture(2, i + 6, AsTex(g_shadowDefaultTex), 0);
            }
        }
    }
    float* dest = context->LockConstants(0, 5, 0x30);
    std::memcpy(dest, g_shadowConstants, 0x30);
    context->UnlockConstants(0, 5);
}

// ref: FUN_1412b3fd0
float* ShaderEffectShadowParams(float* out, GxRenderContext* context, int32_t on, int32_t lit, uint32_t kind) {
    if (g_renderSettings.m_shadowMode == 0) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 0.0f;
        return out;
    }
    if (on == 0) {
        out[0] = lit != 0 ? 1.0f : 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 0.0f;
        return out;
    }
    uint16_t count = g_shadowCascadeCount[kind];
    context->SetTexture(2, 6, AsTex(g_shadowCascades[kind][0].m_tex));
    out[0] = 0.0f;
    out[1] = static_cast<float>(count);
    out[2] = s_cascadeDistances[1] / s_cascadeDistances[count];
    out[3] = 0.0f;
    return out;
}

// ref: FUN_1412bca90
void ShaderEffectBindShadowMap(GxRenderContext* context, void* tex) {
    if (tex == nullptr) {
        tex = TextureGetGxTex(static_cast<HTEXTURE>(g_shadowMapTex));
    }
    context->SetTexture(2, 0xb, AsTex(tex));
}
