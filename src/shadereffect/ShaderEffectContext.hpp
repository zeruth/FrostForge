#ifndef SHADEREFFECT_SHADER_EFFECT_CONTEXT_HPP
#define SHADEREFFECT_SHADER_EFFECT_CONTEXT_HPP

// The shader constants a model draw fills in (ShaderEffect.cpp, 0x9b0 bytes, made on the stack
// of each scene pass): the lights, the per-batch block the pixel shaders read and the per-frame
// block the vertex shaders read, each with the flag that says it must be uploaded again.
//
// The blocks, by the stage and slot they go to:
//   0x1b0 the lights (pixel 4, 0x404): eight 0x80-byte lights and their count at 0x5b0
//   0x5b4 the light colours and direction (pixel 8, 0x90)
//   0x644 the batch (pixel 0, 0x190), dirty at 0x904
//   0x7d4 the frame (vertex 0, 0x120), dirty at 0x905; its 0x30..0x70 are the two texture
//         matrices kept at 0x970
//   0x8f4 four floats (vertex 1, 0x10)

#include <cstddef>
#include <cstdint>

class GxRenderContext;
struct CM2ModelLighting;
struct RENDERSETTINGS;

class ShaderEffectContext {
    public:
    enum : size_t {
        LIGHTS = 0x1b0,
        LIGHT_COUNT = 0x5b0,
        LIGHT_BLOCK = 0x5b4,
        BATCH = 0x644,
        FRAME = 0x7d4,
        BATCH_DIRTY = 0x904,
        FRAME_DIRTY = 0x905,
        TEXTURE_MATRICES = 0x970,
        SIZE = 0x9b0,
    };

    // ref: FUN_1412aad60
    ShaderEffectContext();

    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(this->m_data + offset);
    }
    template <class T>
    const T& Field(size_t offset) const {
        return *reinterpret_cast<const T*>(this->m_data + offset);
    }

    // The lights block to the pixel stage (FUN_1412adb30).
    void UploadLights(GxRenderContext* context);
    // A model's lights through the view, at most `maxLights` of them, and uploaded
    // (FUN_1412adbf0); nothing without lighting.
    void SetLights(GxRenderContext* context, const CM2ModelLighting* lighting, const float* view,
        const float* translation, uint32_t maxLights);
    // The height plane through the view matrix into the light block, uploaded when asked
    // (FUN_1412aecb0).
    void SetViewPlane(GxRenderContext* context, const float* view, bool upload);
    // A model's light colours and its light's direction, through `transform` when `transformed`
    // (FUN_1412ad7f0); the light block is uploaded when `upload` and there is a context.
    void SetLighting(GxRenderContext* context, const CM2ModelLighting* lighting, const float* transform,
        int32_t transformed, bool upload);
    // The fourth light colour (FUN_1412ad690), the light block uploaded the same way.
    void SetLightColor(GxRenderContext* context, const float* color, bool upload);
    // The light block to the pixel stage (FUN_1412aeab0).
    void UploadLightBlock(GxRenderContext* context);
    // ref: FUN_1412adb20
    void SetLit(uint8_t lit);
    // ref: FUN_1412ad070
    float Get160() const;

    // The batch block, uploaded when it changed (FUN_1412aea30): the first `size` bytes.
    void FlushBatch(GxRenderContext* context, uint32_t size);
    // The batch's base block and the word after the frame's (FUN_1412adfd0).
    void SetBatchBase(const float* block, float value);
    // The batch's sample count, alpha reference and the two after it (FUN_1412ad740).
    void SetSampling(const RENDERSETTINGS* settings, float alphaRef, uint8_t kind);
    // ref: FUN_1412ae9a0
    void SetBatch694(float value);
    // ref: FUN_1412ae010
    void SetBatch698(uint16_t value);
    // 1 or 0 (FUN_1412ae970).
    void SetBatch69c(bool set);
    // 1 or 0, flagged only when it changes (FUN_1412ae930).
    void SetBatch6a0(bool set);
    // `count` vectors from `first` at 0x6a4 (FUN_1412ad7b0).
    void SetBatchVectors(const float* vectors, uint32_t first, uint32_t count);
    // The batch's first vector and the frame's at 0x7a4 (FUN_1412ae870).
    void SetBatchColor(const float* color);
    // A texture's matrix: identity (FUN_1412ae4f0) or its first two rows, kept for the frame or
    // the batch (FUN_1412ae3c0).
    void ResetTextureMatrix(uint32_t index);
    void SetTextureMatrix(const float* matrix, uint32_t index, bool batch);
    // The batch's three vectors at 0x6e4 (FUN_1412adad0).
    void SetBatchColors(const float* a, const float* b, const float* c);
    // ref: FUN_1412ae820
    void SetBatch724(const float* vector);
    // ref: FUN_1412adb00
    void SetBatch734(const float* a, const float* b);
    // ref: FUN_1412ad5b0
    void SetBatch754(const float* vector);
    // ref: FUN_1412ad5e0
    void SetBatch7b4(float a, float b);
    // The batch's blend kind and the three after it (FUN_1412adf50).
    void SetBlend(const RENDERSETTINGS* settings, uint8_t blend, uint32_t lit, uint8_t a5, uint8_t a6);

    // The frame block's vectors.
    // ref: FUN_1412ad590
    void SetFrame7d4(const float* vector);
    // ref: FUN_1412ae890
    void SetFrame7e4(const float* vector);
    // ref: FUN_1412acf60
    float* GetFrame7e4(float* out) const;
    // ref: FUN_1412ae8d0
    void SetFrame7ec(float value);
    // ref: FUN_1412ae8b0
    void SetFrame7f0(float value);
    // ref: FUN_1412ae8e0
    void SetFrame7f4(const float* vector);
    // ref: FUN_1412ad600
    void SetFrame844(const float* vector);
    // Three vectors at 0x864 (FUN_1412ae900), or two and a third (FUN_1412ae780).
    void SetFrame864(const float* vectors);
    void SetFrame864(const float* pair, const float* third);
    // The frame block copied to `out` with the texture matrices in it, and its end: 0xc0 bytes
    // on when `short`, else 0x120 (FUN_1412acf70).
    uint8_t* CopyFrame(uint8_t* out, bool shortBlock);
    // The frame's matrices reset and the whole block uploaded (FUN_1412aed20).
    void ResetFrame(GxRenderContext* context);
    // The four floats at 0x8f4 and their upload (FUN_1412ad620).
    void SetVertex8f4(GxRenderContext* context, float a, float b, float c);

    uint8_t m_data[SIZE];
};
static_assert(sizeof(ShaderEffectContext) == 0x9b0, "ShaderEffectContext is 0x9b0 bytes");

// The fog constants (0xe0 bytes): those of a fog (FUN_1412abc80) seen from `camera` through
// `view`, or the defaults (FUN_1412abb80).
float* ShaderEffectFogConstants(float* out, const uint8_t* fog, float a3, const float* camera, const float* view);
float* ShaderEffectDefaultFogConstants(float* out);
// A world environment's fogs (0xa0 bytes each): the default one (FUN_1412af9e0), one by index
// (FUN_1412af9f0) and an index's kind (FUN_1412afa10).
const uint8_t* ShaderEffectEnvironmentDefaultFog(const uint8_t* environment);
const uint8_t* ShaderEffectEnvironmentFog(const uint8_t* environment, uint32_t index);
uint32_t ShaderEffectEnvironmentFogKind(const uint8_t* environment, uint32_t index);
// The environment's screen rectangle as a scale and offset (FUN_1412af940).
float* ShaderEffectEnvironmentRect(const uint8_t* environment, float* out);
// The scene's texture to stage 2 slot 0x10 (white when none) and its sampler to slot 0xd
// (FUN_1412aa7b0).
void ShaderEffectBindSceneTexture(void* tex, GxRenderContext* context);
// Variable-rate shading for a blend: combiners on unless blended or partly transparent
// (FUN_1412aec30), when VALAR shading is on (FUN_1412af1d0).
void ShaderEffectSetShadingRate(GxRenderContext* context, uint8_t blend, float alpha);
bool ShaderEffectShadingRateOn();
// A sampler key of a texture's usage word (FUN_1412ac7a0): its filter, unless 3 which the
// settings give, and its three address modes.
uint64_t* ShaderEffectSamplerKey(uint64_t* out, uint32_t usage, const RENDERSETTINGS* settings);
// A sampler key of a texture's usage with the address modes replaced (the draws' inline
// packing): filter and the W mode kept, U and V given.
uint64_t ShaderEffectTextureSampler(uint32_t usage, uint8_t addressU, uint8_t addressV);

// A light's 3x4 matrix through the view (FUN_1412b4200): the z axis turned to `position`,
// scaled by `scale`, moved there.
void ShaderEffectLightMatrix(const float* view, const float* position, float scale, float* out);
// Builds the lights block from a light list (FUN_1412abf70).
void ShaderEffectBuildLights(uint8_t* lights, void* const* list, const float* view, const float* translation,
    uint32_t count);

// DAT_1430a5b88: a float every reader takes as is; nothing in the client writes it.
extern float g_shaderEffect5b88;
// DAT_1430a5c48: VALAR shading's readiness; nothing in the client sets it.
extern bool g_vrsValarReady;

#endif
