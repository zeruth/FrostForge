#include "shadereffect/ShaderEffectContext.hpp"

#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "model2/CM2Lighting.hpp"
#include "os/Os.hpp"
#include "storm/ErrorReport.hpp"
#include "texture/Texture.hpp"

float g_shaderEffect5b88;
bool g_vrsValarReady;              // 0x1430a5c48

namespace {

// The up vector the fog's height plane is measured along (DAT_142cfc380).
const float s_up[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
// The identity's rows (DAT_14261a9d0, DAT_14261a9e0, DAT_14261aa00, DAT_14261aa10).
const float s_identity[4][4] = {
    { 1.0f, 0.0f, 0.0f, 0.0f },
    { 0.0f, 1.0f, 0.0f, 0.0f },
    { 0.0f, 0.0f, 1.0f, 0.0f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
};
// The sample-count bits a kind adds (DAT_142912080).
const int32_t s_sampleKinds[8] = { 0, 1, 0, 0, 1, 2, 3, 0 };
// The sampler filters and address modes of a usage word's nibbles (DAT_142726f7c,
// DAT_142726f80).
const uint8_t s_samplerFilters[4] = { 2, 3, 4, 5 };
const uint8_t s_samplerAddress[8] = { 0, 3, 2, 4, 1, 0, 0, 0 };
// The scene texture's sampler key (DAT_142cfc340).
const uint64_t s_sceneSampler = 0x800200000001ull;
// Logged once for a light list with a hole in it (DAT_1430a5c00).
bool s_badLightReported;

// A colour stored blue, green, red, alpha as 0..1 red, green, blue (FUN_141348900).
float* ColorFromBgra(float* out, const uint8_t* bgra) {
    const float scale = 1.0f / 255.0f;
    out[0] = static_cast<float>(bgra[2]) * scale;
    out[1] = static_cast<float>(bgra[1]) * scale;
    out[2] = static_cast<float>(bgra[0]) * scale;
    return out;
}

void Copy(void* dest, const void* source, size_t size) {
    std::memcpy(dest, source, size);
}

}  // namespace

// The constructor zeroes the light (FUN_1412ab3e0) and batch (FUN_1412ab2f0) blocks it embeds,
// and then the whole of each block again; what it leaves is the identities below and both
// blocks flagged.
// ref: FUN_1412aad60
ShaderEffectContext::ShaderEffectContext() {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    this->Field<float>(0x160) = 1.0f;
    this->Field<float>(0x894) = 1.0f;
    this->Field<float>(0x8a8) = 1.0f;
    this->Field<float>(0x8bc) = 1.0f;
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
    this->Field<float>(0x930) = 1.0f;
    this->Field<float>(0x944) = 1.0f;
    this->Field<float>(0x958) = 1.0f;
    this->Field<float>(0x96c) = 1.0f;
}

// ref: FUN_1412adb30
void ShaderEffectContext::UploadLights(GxRenderContext* context) {
    float* dest = context->LockConstants(2, 4, 0x404);
    Copy(dest, this->m_data + LIGHTS, 0x404);
    context->UnlockConstants(2, 4);
}

// ref: FUN_1412adbf0
void ShaderEffectContext::SetLights(GxRenderContext* context, const CM2ModelLighting* lighting, const float* view,
    const float* translation, uint32_t maxLights) {
    if (lighting == nullptr) {
        return;
    }
    uint32_t count = lighting->m_pointCount;
    if (maxLights < count) {
        count = maxLights;
    }
    if (8 < count) {
        count = 8;
    }
    this->Field<uint32_t>(LIGHT_COUNT) = count;
    ShaderEffectBuildLights(this->m_data + LIGHTS, reinterpret_cast<void* const*>(lighting->m_points), view,
        translation, maxLights);
    float* dest = context->LockConstants(2, 4, 0x404);
    Copy(dest, this->m_data + LIGHTS, 0x404);
    context->UnlockConstants(2, 4);
}

// ref: FUN_1412aecb0
void ShaderEffectContext::SetViewPlane(GxRenderContext* context, const float* view, bool upload) {
    float* plane = &this->Field<float>(0x634);
    plane[0] = s_up[3] * view[0xc] + s_up[2] * view[8] + s_up[1] * view[4] + s_up[0] * view[0];
    plane[1] = s_up[3] * view[0xd] + s_up[2] * view[9] + s_up[1] * view[5] + s_up[0] * view[1];
    plane[2] = s_up[3] * view[0xe] + s_up[2] * view[10] + s_up[1] * view[6] + s_up[0] * view[2];
    plane[3] = s_up[3] * view[0xf] + s_up[2] * view[0xb] + s_up[1] * view[7] + s_up[0] * view[3];
    if (!upload) {
        return;
    }
    this->UploadLightBlock(context);
}

// ref: FUN_1412ad7f0
void ShaderEffectContext::SetLighting(GxRenderContext* context, const CM2ModelLighting* lighting,
    const float* transform, int32_t transformed, bool upload) {
    this->Field<uint8_t>(0x164) = 1;
    float* direction = &this->Field<float>(0x5e4);
    const float* light = lighting->m_lightDirection;
    if (transformed == 0) {
        direction[0] = light[0];
        direction[1] = light[1];
        direction[2] = light[2];
        direction[3] = 0.0f;
    } else {
        float z = light[1] * transform[6] + light[0] * transform[2] + light[2] * transform[10];
        direction[0] = light[1] * transform[4] + light[0] * transform[0] + light[2] * transform[8];
        direction[1] = light[1] * transform[5] + light[0] * transform[1] + light[2] * transform[9];
        direction[2] = z;
        direction[3] = 0.0f;
    }
    float length = direction[3] * direction[3] + direction[2] * direction[2] + direction[0] * direction[0]
        + direction[1] * direction[1];
    if (2.3841858e-07f < length) {
        float scale = 1.0f / std::sqrt(length);
        direction[0] *= scale;
        direction[1] *= scale;
        direction[2] *= scale;
        direction[3] *= scale;
    }
    float* colors = &this->Field<float>(0x168);
    colors[0] = lighting->m_ambient[0];
    colors[1] = lighting->m_ambient[1];
    colors[2] = lighting->m_ambient[2];
    colors[3] = 0.0f;
    colors[4] = lighting->m_064[0];
    colors[5] = lighting->m_064[1];
    colors[6] = lighting->m_064[2];
    colors[7] = 0.0f;
    colors[8] = lighting->m_070[0];
    colors[9] = lighting->m_070[1];
    colors[10] = lighting->m_070[2];
    colors[11] = 0.0f;
    float* directional = &this->Field<float>(0x5f4);
    directional[0] = lighting->m_directional[0];
    directional[1] = lighting->m_directional[1];
    directional[2] = lighting->m_directional[2];
    directional[3] = 0.0f;
    // FUN_1411ffd40: the third colour scaled by the settings' light scale.
    float* scaled = &this->Field<float>(0x604);
    scaled[0] = g_renderSettings.m_04 * lighting->m_088[0];
    scaled[1] = g_renderSettings.m_04 * lighting->m_088[1];
    scaled[2] = lighting->m_088[2] * g_renderSettings.m_04;
    scaled[3] = 0.0f;
    this->Field<float>(0x618) = g_renderSettings.m_04;
    Copy(&this->Field<float>(0x854), &this->Field<float>(0x198), 0x10);
    Copy(&this->Field<float>(LIGHT_BLOCK), &this->Field<float>(0x168), 0x30);
    Copy(&this->Field<float>(0x624), &this->Field<float>(0x198), 0x10);
    if (upload && context != nullptr) {
        this->UploadLightBlock(context);
    }
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// ref: FUN_1412ad690
void ShaderEffectContext::SetLightColor(GxRenderContext* context, const float* color, bool upload) {
    Copy(&this->Field<float>(0x198), color, 0x10);
    Copy(&this->Field<float>(0x854), color, 0x10);
    Copy(&this->Field<float>(LIGHT_BLOCK), &this->Field<float>(0x168), 0x30);
    Copy(&this->Field<float>(0x624), &this->Field<float>(0x198), 0x10);
    if (upload && context != nullptr) {
        this->UploadLightBlock(context);
    }
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// ref: FUN_1412aeab0
void ShaderEffectContext::UploadLightBlock(GxRenderContext* context) {
    float* dest = context->LockConstants(2, 8, 0x90);
    Copy(dest, this->m_data + LIGHT_BLOCK, 0x90);
    context->UnlockConstants(2, 8);
}

// ref: FUN_1412adb20
void ShaderEffectContext::SetLit(uint8_t lit) {
    this->Field<uint8_t>(0x164) = lit;
}

// ref: FUN_1412ad070
float ShaderEffectContext::Get160() const {
    return this->Field<float>(0x160);
}

// ref: FUN_1412aea30
void ShaderEffectContext::FlushBatch(GxRenderContext* context, uint32_t size) {
    if (this->Field<uint8_t>(BATCH_DIRTY) == 0) {
        return;
    }
    if (size != 0) {
        float* dest = context->LockConstants(2, 0, size);
        Copy(dest, this->m_data + BATCH, size);
        context->UnlockConstants(2, 0);
    }
    this->Field<uint8_t>(BATCH_DIRTY) = 0;
}

// ref: FUN_1412adfd0
void ShaderEffectContext::SetBatchBase(const float* block, float value) {
    Copy(this->m_data + BATCH, block, 0x40);
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    this->Field<float>(0x7bc) = value;
}

// ref: FUN_1412ad740
void ShaderEffectContext::SetSampling(const RENDERSETTINGS* settings, float alphaRef, uint8_t kind) {
    // FUN_14228a0e0: the sample count.
    uint32_t samples = 1u << ((settings->m_msaaSamples + s_sampleKinds[kind] + settings->m_msaaQuality) & 0x1f);
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    this->Field<float>(0x684) = static_cast<float>(samples);
    this->Field<float>(0x688) = alphaRef;
    this->Field<uint32_t>(0x68c) = 0;
    this->Field<uint32_t>(0x690) = 0;
}

// ref: FUN_1412ae9a0
void ShaderEffectContext::SetBatch694(float value) {
    this->Field<float>(0x694) = value;
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
}

// ref: FUN_1412ae010
void ShaderEffectContext::SetBatch698(uint16_t value) {
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    this->Field<float>(0x698) = static_cast<float>(value);
}

// ref: FUN_1412ae970
void ShaderEffectContext::SetBatch69c(bool set) {
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    this->Field<float>(0x69c) = set ? 1.0f : 0.0f;
}

// ref: FUN_1412ae930
void ShaderEffectContext::SetBatch6a0(bool set) {
    float value = set ? 1.0f : 0.0f;
    if (value != this->Field<float>(0x6a0)) {
        this->Field<float>(0x6a0) = value;
        this->Field<uint8_t>(BATCH_DIRTY) = 1;
    }
}

// ref: FUN_1412ad7b0
void ShaderEffectContext::SetBatchVectors(const float* vectors, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        Copy(&this->Field<float>(0x6a4 + static_cast<size_t>(first + i) * 0x10), vectors + i * 4, 0x10);
    }
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
}

// ref: FUN_1412ae870
void ShaderEffectContext::SetBatchColor(const float* color) {
    Copy(&this->Field<float>(0x6a4), color, 0x10);
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    Copy(&this->Field<float>(0x7a4), color, 0x10);
}

// ref: FUN_1412ae4f0
void ShaderEffectContext::ResetTextureMatrix(uint32_t index) {
    size_t row = static_cast<size_t>(index) * 0x20;
    Copy(&this->Field<float>(TEXTURE_MATRICES + row), s_identity[0], 0x10);
    Copy(&this->Field<float>(TEXTURE_MATRICES + 0x10 + row), s_identity[1], 0x10);
    Copy(&this->Field<float>(0x6e4 + row), s_identity[0], 0x10);
    Copy(&this->Field<float>(0x6f4 + row), s_identity[1], 0x10);
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// The matrix's first two columns as rows go to the batch when `batch`, with the frame's
// identity; else to the frame, with the batch's identity.
// ref: FUN_1412ae3c0
void ShaderEffectContext::SetTextureMatrix(const float* matrix, uint32_t index, bool batch) {
    const float u[4] = { matrix[0], matrix[4], matrix[8], matrix[12] };
    const float v[4] = { matrix[1], matrix[5], matrix[9], matrix[13] };
    size_t row = static_cast<size_t>(index) * 0x20;
    if (batch) {
        Copy(&this->Field<float>(TEXTURE_MATRICES + row), s_identity[0], 0x10);
        Copy(&this->Field<float>(TEXTURE_MATRICES + 0x10 + row), s_identity[1], 0x10);
        Copy(&this->Field<float>(0x6e4 + row), u, 0x10);
        Copy(&this->Field<float>(0x6f4 + row), v, 0x10);
    } else {
        Copy(&this->Field<float>(TEXTURE_MATRICES + row), u, 0x10);
        Copy(&this->Field<float>(TEXTURE_MATRICES + 0x10 + row), v, 0x10);
        Copy(&this->Field<float>(0x6e4 + row), s_identity[0], 0x10);
        Copy(&this->Field<float>(0x6f4 + row), s_identity[1], 0x10);
    }
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// ref: FUN_1412adad0
void ShaderEffectContext::SetBatchColors(const float* a, const float* b, const float* c) {
    Copy(&this->Field<float>(0x6e4), a, 0x10);
    Copy(&this->Field<float>(0x6f4), b, 0x10);
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    Copy(&this->Field<float>(0x704), c, 0x10);
}

// ref: FUN_1412ae820
void ShaderEffectContext::SetBatch724(const float* vector) {
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    Copy(&this->Field<float>(0x724), vector, 0x10);
}

// A texture layer's transform, 0x20 apart from 0x6e4. A second vector lands on the first, as
// the reference stores both at the same place; the dirty flag is left alone.
// ref: FUN_1412ae840
void ShaderEffectContext::SetLayerTransform(const float* vectors, uint32_t layer, uint32_t count) {
    Copy(&this->Field<float>(0x6e4 + layer * 0x20), vectors, 0x10);
    if (1 < count) {
        Copy(&this->Field<float>(0x6e4 + layer * 0x20), vectors + 4, 0x10);
    }
}

// ref: FUN_1412adb00
void ShaderEffectContext::SetBatch734(const float* a, const float* b) {
    Copy(&this->Field<float>(0x744), b, 0x10);
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    Copy(&this->Field<float>(0x734), a, 0x10);
}

// ref: FUN_1412ad5b0
void ShaderEffectContext::SetBatch754(const float* vector) {
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
    Copy(&this->Field<float>(0x754), vector, 0x10);
}

// ref: FUN_1412ad5e0
void ShaderEffectContext::SetBatch7b4(float a, float b) {
    this->Field<float>(0x7b4) = a;
    this->Field<float>(0x7b8) = b;
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
}

// ref: FUN_1412adf50
void ShaderEffectContext::SetBlend(const RENDERSETTINGS* settings, uint8_t blend, uint32_t lit, uint8_t a5, uint8_t a6) {
    uint32_t kind;
    if (blend == 0) {
        kind = 1;
    } else if (blend == 1) {
        if (settings->m_msaaSamples == 0) {
            kind = 2;
        } else {
            kind = (settings->m_flags & 1) + 2;
        }
    } else if (blend == 0xd) {
        kind = 4;
    } else {
        kind = 0;
    }
    this->Field<uint32_t>(0x7c4) = kind;
    this->Field<uint32_t>(0x7cc) = a5;
    this->Field<uint32_t>(0x7d0) = a6;
    this->Field<uint32_t>(0x7c8) = lit;
    this->Field<uint8_t>(BATCH_DIRTY) = 1;
}

// ref: FUN_1412ad590
void ShaderEffectContext::SetFrame7d4(const float* vector) {
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
    Copy(&this->Field<float>(0x7d4), vector, 0x10);
}

// ref: FUN_1412ae890
void ShaderEffectContext::SetFrame7e4(const float* vector) {
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
    Copy(&this->Field<float>(0x7e4), vector, 0x10);
}

// ref: FUN_1412acf60
float* ShaderEffectContext::GetFrame7e4(float* out) const {
    Copy(out, &this->Field<float>(0x7e4), 0x10);
    return out;
}

// ref: FUN_1412ae8d0
void ShaderEffectContext::SetFrame7ec(float value) {
    this->Field<float>(0x7ec) = value;
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// ref: FUN_1412ae8b0
void ShaderEffectContext::SetFrame7f0(float value) {
    this->Field<float>(0x7f0) = value;
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// ref: FUN_1412ae8e0
void ShaderEffectContext::SetFrame7f4(const float* vector) {
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
    Copy(&this->Field<float>(0x7f4), vector, 0x10);
}

// ref: FUN_1412ad600
void ShaderEffectContext::SetFrame844(const float* vector) {
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
    Copy(&this->Field<float>(0x844), vector, 0x10);
}

// ref: FUN_1412ae900
void ShaderEffectContext::SetFrame864(const float* vectors) {
    Copy(&this->Field<float>(0x864), vectors, 0x30);
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// ref: FUN_1412ae780
void ShaderEffectContext::SetFrame864(const float* pair, const float* third) {
    Copy(&this->Field<float>(0x864), pair, 0x20);
    Copy(&this->Field<float>(0x884), third, 0x10);
    this->Field<uint8_t>(FRAME_DIRTY) = 1;
}

// ref: FUN_1412acf70
uint8_t* ShaderEffectContext::CopyFrame(uint8_t* out, bool shortBlock) {
    Copy(out, this->m_data + FRAME, 0x120);
    Copy(out + 0x30, this->m_data + TEXTURE_MATRICES, 0x40);
    this->Field<uint8_t>(FRAME_DIRTY) = 0;
    return out + (shortBlock ? 0xc0 : 0x120);
}

// ref: FUN_1412aed20
void ShaderEffectContext::ResetFrame(GxRenderContext* context) {
    Copy(&this->Field<float>(0x894), s_identity[0], 0x10);
    Copy(&this->Field<float>(0x8a4), s_identity[1], 0x10);
    Copy(&this->Field<float>(0x8b4), s_identity[2], 0x10);
    // The three rows again, by columns.
    float* columns = &this->Field<float>(0x8c4);
    for (uint32_t c = 0; c < 3; c++) {
        for (uint32_t r = 0; r < 4; r++) {
            columns[c * 4 + r] = s_identity[r][c];
        }
    }
    float* dest = context->LockConstants(0, 0, 0x120);
    Copy(dest, this->m_data + FRAME, 0x120);
    context->UnlockConstants(0, 0);
    this->Field<uint8_t>(FRAME_DIRTY) = 0;
}

// ref: FUN_1412ad620
void ShaderEffectContext::SetVertex8f4(GxRenderContext* context, float a, float b, float c) {
    this->Field<float>(0x8fc) = c;
    this->Field<float>(0x8f4) = a;
    this->Field<float>(0x8f8) = b;
    if (context == nullptr) {
        return;
    }
    float* dest = context->LockConstants(0, 1, 0x10);
    Copy(dest, &this->Field<float>(0x8f4), 0x10);
    context->UnlockConstants(0, 1);
}

// ref: FUN_1412abc80
float* ShaderEffectFogConstants(float* out, const uint8_t* fog, float a3, const float* camera, const float* view) {
    auto f = [fog](size_t offset) {
        return *reinterpret_cast<const float*>(fog + offset);
    };
    float height = f(0x14);
    float dx = height * s_up[0] - camera[0];
    float dz = height * s_up[2] - camera[2];
    float dy = height * s_up[1] - camera[1];
    float upX = s_up[1] * view[4] + s_up[0] * view[0] + s_up[2] * view[8];
    float upY = s_up[1] * view[5] + s_up[0] * view[1] + s_up[2] * view[9];
    float upZ = s_up[1] * view[6] + s_up[0] * view[2] + s_up[2] * view[10];
    out[0] = f(4);
    out[1] = f(8);
    out[2] = f(0x10);
    out[3] = a3;
    float end = f(8);
    float range = 1e10f;
    if (end != f(4)) {
        range = 1.0f / (end - f(4));
    }
    out[4] = 1.0f;
    out[5] = end;
    out[6] = range;
    out[7] = f(0xc);
    out[8] = upX;
    out[9] = upY;
    out[10] = upZ;
    float distance = (dy * view[5] + dx * view[1] + dz * view[9]) * upY
        + upX * (dy * view[4] + dx * view[0] + dz * view[8]) + (dy * view[6] + dx * view[2] + dz * view[10]) * upZ;
    out[11] = -distance;
    ColorFromBgra(out + 0xc, fog);
    out[0xf] = f(0x18);
    out[0x10] = f(0x1c);
    ColorFromBgra(out + 0x11, fog + 0x30);
    out[0x14] = f(0x2c);
    ColorFromBgra(out + 0x15, fog + 0x34);
    ColorFromBgra(out + 0x18, fog + 0x38);
    out[0x1b] = f(0x3c);
    out[0x1c] = f(0x40);
    out[0x1d] = g_shaderEffect5b88;
    out[0x1e] = 1.0f;
    out[0x1f] = 1.0f;
    Copy(out + 0x20, fog + 0x4c, 8);
    out[0x22] = f(0x54);
    out[0x23] = f(0x20);
    Copy(out + 0x24, fog + 0x5c, 0x30);
    float low = f(0x24) + 0.001f;
    out[0x30] = low <= f(0x28) ? f(0x28) : low;
    out[0x31] = 0.0f <= f(0x24) ? f(0x24) : 0.0f;
    out[0x32] = f(0x48);
    out[0x33] = f(0x9c);
    ColorFromBgra(out + 0x34, fog + 0x94);
    out[0x37] = f(0x98);
    return out;
}

// ref: FUN_1412abb80
float* ShaderEffectDefaultFogConstants(float* out) {
    auto u = reinterpret_cast<uint32_t*>(out);
    std::memset(out, 0, 0xe0);
    u[1] = 0x4b189680;          // 1e7
    u[4] = 0x3f800000;
    u[5] = 0x501502f9;          // 1e10
    u[6] = 0x2edbe6ff;          // 1e-10
    u[7] = 0x3f800000;
    u[10] = 0x3f800000;
    u[0xb] = 0xc61c4000;        // -10000
    u[0x14] = 0x3f800000;
    u[0x1b] = 0x4b189680;
    u[0x1e] = 0x3f800000;
    u[0x1f] = 0x3f800000;
    u[0x22] = 0x3f800000;
    u[0x26] = 0x3f800000;
    u[0x2a] = 0x3f800000;
    u[0x2e] = 0x3f800000;
    u[0x30] = 0x4b189680;
    u[0x32] = 0x3f800000;
    u[0x33] = 0x3f800000;
    return out;
}

// ref: FUN_1412af9e0
const uint8_t* ShaderEffectEnvironmentDefaultFog(const uint8_t* environment) {
    return environment + 0x518;
}

// ref: FUN_1412af9f0
const uint8_t* ShaderEffectEnvironmentFog(const uint8_t* environment, uint32_t index) {
    return environment + static_cast<size_t>(index) * 0xa0 + 0x18;
}

// ref: FUN_1412afa10
uint32_t ShaderEffectEnvironmentFogKind(const uint8_t* environment, uint32_t index) {
    if (index == 1) {
        return *reinterpret_cast<const uint32_t*>(environment + 0x5b8);
    }
    return 1;
}

// ref: FUN_1412af940
float* ShaderEffectEnvironmentRect(const uint8_t* environment, float* out) {
    auto e = reinterpret_cast<const float*>(environment);
    float right = e[5];
    float left = e[3];
    float bottom = e[4];
    float top = e[2];
    out[0] = (right - left) * 0.5f;
    out[1] = (bottom - top) * -0.5f;
    out[2] = ((left + 1.0f) - (1.0f - right)) * 0.5f;
    out[3] = (((1.0f - bottom) + 1.0f) - top) * 0.5f;
    return out;
}

// ref: FUN_1412aa7b0
void ShaderEffectBindSceneTexture(void* tex, GxRenderContext* context) {
    if (tex == nullptr) {
        tex = TextureTransparentBlack3DGxTex();
    }
    context->BindTexture(2, 0x10, static_cast<gxDx12Backend::Tex*>(tex), 0);
    context->SetSampler(2, 0xd, s_sceneSampler);
}

// ref: FUN_1412aec30
void ShaderEffectSetShadingRate(GxRenderContext* context, uint8_t blend, float alpha) {
    if (!ShaderEffectShadingRateOn() || 1 < blend) {
        return;
    }
    uint8_t combiners[2] = { 0, 1 };
    if (blend == 1 || (0.0f < alpha && alpha < 1.0f)) {
        combiners[1] = 0;
    }
    context->SetShadingRateCombiners(combiners);
}

// ref: FUN_1412af1d0
bool ShaderEffectShadingRateOn() {
    return (g_renderSettings.m_flags & 0xc0) != 0 && g_vrsValarReady;
}

// ref: FUN_1412ac7a0
uint64_t* ShaderEffectSamplerKey(uint64_t* out, uint32_t usage, const RENDERSETTINGS* settings) {
    *out = 0;
    auto bytes = reinterpret_cast<uint8_t*>(out);
    *reinterpret_cast<uint16_t*>(bytes + 4) = 0x8002;
    uint8_t filter;
    if ((usage & 0xf) == 3) {
        filter = settings->m_filterMode;
        *reinterpret_cast<uint16_t*>(bytes + 4) = static_cast<uint16_t>((settings->m_filterLevel & 0x1f) * 2 | 0x8000);
    } else {
        filter = s_samplerFilters[usage & 0xf];
    }
    bytes[0] = filter;
    bytes[1] = s_samplerAddress[usage >> 4 & 0xf];
    bytes[2] = s_samplerAddress[usage >> 8 & 0xf];
    bytes[3] = s_samplerAddress[usage >> 0xc & 0xf];
    return out;
}

uint64_t ShaderEffectTextureSampler(uint32_t usage, uint8_t addressU, uint8_t addressV) {
    uint16_t low = static_cast<uint16_t>(((usage >> 3) & 1) | 0x8002);
    uint16_t high = static_cast<uint16_t>((((static_cast<uint16_t>(usage >> 13) * 2) ^ low) & 0x3e) ^ low);
    uint64_t key = (usage & 7) | (static_cast<uint64_t>(addressU) << 8) | (static_cast<uint64_t>(addressV) << 16)
        | (static_cast<uint64_t>((usage >> 10) & 7) << 24) | (static_cast<uint64_t>(high) << 32);
    return key;
}

// ref: FUN_1412b4200
void ShaderEffectLightMatrix(const float* view, const float* position, float scale, float* out) {
    float m[16] = {
        scale, 0.0f, 0.0f, 0.0f,
        0.0f, scale, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    const float z[3] = { 0.0f, 0.0f, 1.0f };
    float rotation[16];
    MatrixRotationBetween(rotation, z, position);
    MatrixMultiply(m, m, rotation);
    MatrixAddTranslation(m, position);
    float p[16];
    MatrixMultiply(p, m, view);
    for (uint32_t c = 0; c < 3; c++) {
        for (uint32_t r = 0; r < 4; r++) {
            out[c * 4 + r] = p[r * 4 + c];
        }
    }
}

// Each light is 0x80 bytes: its colour twice, its position, its index count, its attenuation
// flag and its 3x4 matrix.
// ref: FUN_1412abf70
void ShaderEffectBuildLights(uint8_t* lights, void* const* list, const float* view, const float* translation,
    uint32_t count) {
    auto& stored = *reinterpret_cast<uint32_t*>(lights + 0x400);
    uint32_t n = stored <= count ? stored : count;
    if (8 < n) {
        n = 8;
    }
    stored = n;
    uint32_t i = 0;
    if (n != 0) {
        float m[16];
        Copy(m, view, sizeof(m));
        if (translation != nullptr) {
            const float back[3] = { -translation[0], -translation[1], -translation[2] };
            MatrixTranslate(m, back);
        }
        for (; i < n; i++) {
            auto light = static_cast<const CM2Lighting*>(list[i]);
            if (light == nullptr) {
                GxLog("Bad CM2Light*: %p", nullptr);
                OsSleepNanoseconds(1000000);
                light = static_cast<const CM2Lighting*>(list[i]);
                if (light == nullptr) {
                    GxLog("Skipping...");
                } else {
                    GxLog("Fix worked: %p", light);
                }
                if (!s_badLightReported) {
                    s_badLightReported = true;
                    ErrorReportNonFatal(0x8512008b,
                        "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\ShaderEffect\\ShaderEffect.cpp", 0x159, 1, 1, 0,
                        "Bad CM2Light* %p new: %p", nullptr, light);
                }
                if (light == nullptr) {
                    stored = i;
                    break;
                }
            }
            auto out = reinterpret_cast<float*>(lights + static_cast<size_t>(i) * 0x80);
            float diffuse[3];
            light->GetDiffuse(diffuse);
            out[0] = diffuse[0];
            out[1] = diffuse[1];
            out[2] = diffuse[2];
            out[3] = 1.0f;
            out[4] = diffuse[0];
            out[5] = diffuse[1];
            out[6] = diffuse[2];
            out[7] = 1.0f;
            bool attenuated = (light->m_flags & 1) != 0;
            float attenuation = attenuated ? light->m_78 : 0.5f;
            const float* position;
            if (translation == nullptr) {
                position = light->m_worldPosition;
                out[8] = light->m_worldPosition[0];
                out[9] = light->m_worldPosition[1];
                out[10] = light->m_worldPosition[2];
            } else {
                position = light->m_position;
                const float* p = light->m_position;
                out[8] = p[1] * m[4] + p[0] * m[0] + p[2] * m[8] + m[12];
                out[10] = p[1] * m[6] + p[0] * m[2] + p[2] * m[10] + m[14];
                out[9] = p[1] * m[5] + p[0] * m[1] + p[2] * m[9] + m[13];
            }
            out[11] = 1.0f;
            ShaderEffectLightMatrix(m, position, attenuation, out + 0x14);
            out[0x12] = attenuated ? 1.0f : 0.0f;
            out[0xc] = 0.0f;
            out[0xd] = 0.0f;
            out[0xe] = 0.0f;
            out[0xf] = static_cast<float>(n);
        }
        if (7 < i) {
            return;
        }
    }
    for (; i < 8; i++) {
        std::memset(lights + static_cast<size_t>(i) * 0x80, 0, 0x40);
    }
}
