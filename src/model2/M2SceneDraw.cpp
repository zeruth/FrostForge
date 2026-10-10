#include "model2/M2Scene.hpp"

#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/GxVertexFormat.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "model2/CM2Lighting.hpp"
#include "model2/M2Camera.hpp"
#include "model2/M2Data.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Ribbon.hpp"
#include "model2/M2Shaders.hpp"
#include "model2/M2Shared.hpp"
#include "os/Sync.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "shadereffect/ShaderEffectPipeline.hpp"
#include "shadereffect/ShaderEffectShadow.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Scene.cpp: drawing a pass. The pass's sorted elements are walked in
// order; each model batch sets its textures, its batch and frame constants and its geometry
// against what the element before it set, so a run of one model's batches only changes what
// differs.

uint32_t g_m2DrawPassMask = 0xffffffff;
uint8_t g_gx8b641;

namespace {

// What a pass draw carries from element to element (0xae0 on the stack of the pass's draw): the
// render context, the scene, each element's model, batch and material beside the previous
// element's, and the shader constants.
struct M2SceneDrawContext {
    GxRenderContext* m_context;         // 0x000
    const uint8_t* m_environment;       // 0x008 the scene's world environment (its fogs)
    float m_identity[16];               // 0x010
    CM2Scene* m_scene;                  // 0x050
    M2CACHE* m_cache;                   // 0x058
    M2Data* m_data;                     // 0x060 the model's file
    void* m_sceneTexture;               // 0x068
    uint8_t m_pass;                     // 0x070
    uint8_t m_pad71[7];
    M2Element* m_element;               // 0x078
    M2Element* m_prevElement;           // 0x080
    int32_t m_kind;                     // 0x088 the element's kind
    int32_t m_prevKind;                 // 0x08c
    CM2Model* m_model;                  // 0x090
    CM2Model* m_prevModel;              // 0x098
    CM2Shared* m_shared;                // 0x0a0
    CM2Shared* m_prevShared;            // 0x0a8
    int32_t m_lod;                      // 0x0b0
    int32_t m_prevLod;                  // 0x0b4
    CM2ModelLighting* m_lighting;       // 0x0b8
    CM2ModelLighting* m_prevLighting;   // 0x0c0
    uint8_t m_lit;                      // 0x0c8
    uint8_t m_prevLit;                  // 0x0c9
    uint8_t m_combiners;                // 0x0ca the batch's texture combiners are in use
    uint8_t m_padcb;
    int32_t m_fogKind;                  // 0x0cc
    int32_t m_prevFogKind;              // 0x0d0
    uint32_t m_padd4;
    const float* m_edgeFade;            // 0x0d8 the material's EDGF entry
    const float* m_prevEdgeFade;        // 0x0e0
    const float* m_dboc;                // 0x0e8 the material's DBOC entry
    const float* m_prevDboc;            // 0x0f0
    const M2Batch* m_batch;             // 0x0f8
    const M2Batch* m_prevBatch;         // 0x100
    const M2SkinSection* m_section;     // 0x108
    const M2SkinSection* m_prevSection; // 0x110
    const M2Material* m_material;       // 0x118
    const M2Material* m_prevMaterial;   // 0x120
    M2Material m_defaultMaterial;       // 0x128 unlit, blend 0
    uint32_t m_sceneFlag8;              // 0x12c the scene's flag 8
    ShaderEffectContext m_shader;       // 0x130
};
static_assert(offsetof(M2SceneDrawContext, m_kind) == 0x88, "the draw context's kind at 0x88");
static_assert(offsetof(M2SceneDrawContext, m_fogKind) == 0xcc, "the draw context's fog at 0xcc");
static_assert(offsetof(M2SceneDrawContext, m_batch) == 0xf8, "the draw context's batch at 0xf8");
static_assert(offsetof(M2SceneDrawContext, m_shader) == 0x130, "the draw context's constants at 0x130");
static_assert(sizeof(M2SceneDrawContext) == 0xae0, "the draw context is 0xae0 bytes");

// The passes that read the shadow maps (DAT_142721470).
const uint8_t s_passShadows[16] = { 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0 };
// A blend mode's fog kind (DAT_142721248), whether it adds (DAT_142721450) and the shader blend
// kind the pixel constants carry (DAT_1427228d8, FUN_1411fb3e0).
const uint32_t s_blendFogKind[8] = { 1, 1, 1, 2, 2, 3, 4, 2 };
const uint32_t s_blendAdds[8] = { 0, 0, 0, 1, 1, 0, 0, 1 };
const uint8_t s_blendShaderKind[8] = { 0, 1, 2, 10, 3, 4, 5, 13 };
// The address modes of an M2Texture's flags (DAT_142721480, DAT_142721488).
const uint8_t s_textureWrapU[8] = { 0, 1, 0, 1, 3, 2, 4, 0 };
const uint8_t s_textureWrapV[8] = { 0, 0, 1, 1, 3, 2, 4, 0 };
// The default sampler (DAT_142cfb1c0).
const uint64_t s_defaultSampler = 0x800200000001ull;
// The batch base blocks without a fog (DAT_142721a00, DAT_142721a50) and the object fill's
// colour when the model has no highlight (DAT_142721a30).
const float s_noFogParams[4] = { 8.0f, 8.0f, 8.0f, 0.0f };
const float s_noFogKinds[4] = { 8.0f, 8.0f, 8.0f, 1.0f };
const float s_fillNoColor[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

gxDx12Backend::Tex* AsTex(void* tex) {
    return static_cast<gxDx12Backend::Tex*>(tex);
}

uint32_t TexUsage(const void* tex) {
    return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(tex) + 0x14);
}

uint32_t ElementKind(const M2Element* element) {
    return static_cast<uint32_t>(element->m_sortKey >> 31) & 0xf;
}

CM2Model* AsModel(CM2BaseModel* model) {
    return model->m_type == 0 ? static_cast<CM2Model*>(model) : nullptr;
}

// The model effect's fields (the "M2 Model Effect Pool" entries a model's m_effect and
// M2BaseModelGetEffect give).
template <class T>
T EffectField(const void* effect, size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(effect) + offset);
}

// The error-report records the draws push: a pass (LAB_1411cf450), a batch (FUN_1411cf410), a
// ribbon, outline or fill (FUN_1411cf3e0) and a model (LAB_1411cf3c0).
struct PASSCONTEXT {
    ERRORCONTEXT m_base;
    const char* m_format;               // 0x10
    uint8_t m_pass;                     // 0x18
};

void DescribePass(ERRORCONTEXT* base, uint64_t size, char* buffer) {
    auto context = reinterpret_cast<PASSCONTEXT*>(base);
    SStrPrintf(buffer, static_cast<uint32_t>(size), context->m_format, context->m_pass);
}

struct BATCHCONTEXT {
    ERRORCONTEXT m_base;
    const char* m_format;               // 0x10
    uint16_t m_material;                // 0x18
    uint16_t m_section;                 // 0x1a
    uint32_t m_batch;                   // 0x1c
    int32_t m_fileDataId;               // 0x20
};

void DescribeBatch(ERRORCONTEXT* base, uint64_t size, char* buffer) {
    auto context = reinterpret_cast<BATCHCONTEXT*>(base);
    SStrPrintf(buffer, static_cast<uint32_t>(size), context->m_format, context->m_fileDataId, context->m_batch,
        context->m_section, context->m_material);
}

struct ITEMCONTEXT {
    ERRORCONTEXT m_base;
    const char* m_format;               // 0x10
    uint32_t m_batch;                   // 0x18
    int32_t m_fileDataId;               // 0x1c
};

void DescribeItem(ERRORCONTEXT* base, uint64_t size, char* buffer) {
    auto context = reinterpret_cast<ITEMCONTEXT*>(base);
    SStrPrintf(buffer, static_cast<uint32_t>(size), context->m_format, context->m_fileDataId, context->m_batch);
}

struct MODELCONTEXT {
    ERRORCONTEXT m_base;
    const char* m_format;               // 0x10
    int32_t m_fileDataId;               // 0x18
};

void DescribeModel(ERRORCONTEXT* base, uint64_t size, char* buffer) {
    auto context = reinterpret_cast<MODELCONTEXT*>(base);
    SStrPrintf(buffer, static_cast<uint32_t>(size), context->m_format, context->m_fileDataId);
}

// A crash-report record carrying a model's memory (LAB_1411ace50).
struct MODELMEMORY {
    ERRORMEMORYCONTEXT m_base;
    CM2BaseModel* m_model;              // 0x10
};

void ReportModelMemory(ERRORMEMORYCONTEXT* base, ERRORMEMORYREPORT report) {
    report(reinterpret_cast<MODELMEMORY*>(base)->m_model, 0x688);
}

// The default fog a pass starts from (built on the stack of FUN_1411d9650): white, from 0 to
// 1000, nothing below -10000.
void DefaultPassFog(uint8_t* fog) {
    std::memset(fog, 0, 0xa0);
    auto u = reinterpret_cast<uint32_t*>(fog);
    auto f = reinterpret_cast<float*>(fog);
    u[0] = 0xffffffff;
    f[2] = 1000.0f;
    f[3] = 1.0f;
    f[4] = 1.0f;
    f[5] = -10000.0f;
    f[7] = 1.0f;
    f[10] = 0.001f;
    u[12] = 0xffffffff;
    u[13] = 0xffffffff;
    u[14] = 0xffffffff;
    f[15] = 10000.0f;
    f[18] = 1.0f;
    f[25] = 1.0f;
    f[29] = 1.0f;
    f[33] = 1.0f;
    f[35] = 1e7f;
}

// A model's matrix as three columns, then its bones' through `lookup`'s bone combos; a section
// without bones gets the model's matrix again when `repeat`. Each matrix is written whole, its
// fourth row scratch the next one overwrites.
void WriteBoneMatrices(float* out, CM2Model* model, const C44Matrix* bones, CM2Model* lookup,
    const M2SkinSection* section, bool repeat) {
    auto transpose = [](float* dest, const float* m) {
        dest[0] = m[0];
        dest[1] = m[4];
        dest[2] = m[8];
        dest[3] = m[12];
        dest[4] = m[1];
        dest[5] = m[5];
        dest[6] = m[9];
        dest[7] = m[13];
        dest[8] = m[2];
        dest[9] = m[6];
        dest[10] = m[10];
        dest[11] = m[14];
        dest[12] = m[3];
        dest[13] = m[7];
        dest[14] = m[11];
        dest[15] = m[15];
    };
    transpose(out, model->m_world.m);
    float* bone = out + 0xc;
    if (section->m_boneCount == 0) {
        if (repeat) {
            transpose(bone, model->m_world.m);
        }
        return;
    }
    const uint16_t* combos = static_cast<const uint16_t*>(lookup->m_468);
    if (combos == nullptr) {
        combos = lookup->m_shared->m_data->m_boneCombos.Data();
    }
    for (uint32_t i = 0; i < section->m_boneCount; i++) {
        transpose(bone, bones[combos[section->m_boneComboIndex + i]].m);
        bone += 0xc;
    }
}

// A material's edge fade (FUN_1411a8140) and DBOC entry (FUN_1411a80f0) in the shared file.
const float* SharedEdgeFade(const CM2Shared* shared, uint32_t material) {
    auto list = static_cast<const uint8_t*>(shared->m_edgeFades);
    if (list != nullptr) {
        for (uint32_t i = 0; i < shared->m_edgeFadeCount; i++) {
            if (list[i * 0x18 + 0xe] == material) {
                return reinterpret_cast<const float*>(list + i * 0x18);
            }
        }
    }
    return nullptr;
}

const float* SharedDboc(const CM2Shared* shared, uint32_t material) {
    for (uint32_t i = 0; i < shared->m_dbocCount; i++) {
        auto entry = static_cast<const uint8_t*>(shared->m_dbocs) + i * 0x10;
        if (*reinterpret_cast<const uint16_t*>(entry + 8) == material) {
            return reinterpret_cast<const float*>(entry);
        }
    }
    return nullptr;
}

// The skin profile's index buffer (FUN_1411ac380) and its vertices as stream 0 (FUN_1411ac3e0).
void SharedSetIndexBuffer(CM2Shared* shared, GxRenderContext* context, uint32_t lod) {
    context->SetIndexBuffer(shared->GetIndexBuf(lod), 1);
}

void SharedSetVertexBuffer(CM2Shared* shared, GxRenderContext* context, uint32_t lod) {
    GxSetVertexStream(context, shared->GetVertexBuf(lod), 0x14);
}

// The pipeline, the batch's sampling and constants, and the section's indices drawn.
void DrawSection(M2SceneDrawContext* ctx) {
    ctx->m_context->SetPipeline(ctx->m_element->m_pipeline);
    ctx->m_shader.SetSampling(&g_renderSettings, ctx->m_model->m_params.m_fade, 0);
    ctx->m_shader.FlushBatch(ctx->m_context, 400);
    const M2SkinSection* section = ctx->m_section;
    ctx->m_context->DrawIndexed(section->IndexCount(), 1, section->IndexStart(), 0, 0, 0);
}

// The vertex constants of a batch: the frame block, then the model's and bones' matrices
// from 0xc0; the frame block is cut there when `shortBlock`.
void WriteBatchMatrices(M2SceneDrawContext* ctx, bool shortBlock, bool repeat) {
    uint32_t size = (ctx->m_section->m_boneCount + 6) * 0x30;
    auto dest = reinterpret_cast<uint8_t*>(ctx->m_context->LockConstants(0, 0, size));
    ctx->m_shader.CopyFrame(dest, shortBlock);
    WriteBoneMatrices(reinterpret_cast<float*>(dest + 0xc0), ctx->m_model, ctx->m_model->GetBoneMatrices(), ctx->m_model,
        ctx->m_section, repeat);
    ctx->m_context->UnlockConstants(0, 0);
}

}  // namespace

// The draws' debug trace: compiled out of the client, its arguments still evaluated.
// ref: FUN_1411a61b0
void M2SceneDebugLog(GxRenderContext* context, const char* format, ...) {
    (void)context;
    (void)format;
}

namespace {

// A batch's texture at `slot`: its combo's texture at `offset` with the M2Texture's address
// modes, `overrideTex` bound in its place when given, grey when it is missing.
// ref: FUN_1411d6e10
void BindBatchTexture(M2SceneDrawContext* ctx, uint32_t slot, uint32_t offset, void* overrideTex) {
    M2Data* data = ctx->m_data;
    uint16_t index = data->m_textureCombos[ctx->m_batch->m_textureComboIndex + offset];
    void* bound;
    uint64_t sampler;
    void* tex = nullptr;
    if (index < data->m_textures.Count() && ctx->m_model->m_textures[index] != nullptr
        && (tex = TextureGetGxTex(static_cast<HTEXTURE>(ctx->m_model->m_textures[index]))) != nullptr) {
        bound = overrideTex != nullptr ? overrideTex : tex;
        uint16_t flags = static_cast<uint16_t>(data->m_textures[index].m_flags);
        sampler = ShaderEffectTextureSampler(TexUsage(tex), s_textureWrapU[flags], s_textureWrapV[flags]);
    } else {
        sampler = s_defaultSampler;
        bound = ctx->m_pass < 8 ? TextureGreyGxTex() : TextureTransparentGreyGxTex();
    }
    ctx->m_context->BindTexture(2, slot, AsTex(bound), 0);
    ctx->m_context->SetSampler(2, slot, sampler);
}

// The model's replaceable effect textures from `slot` on, black for the missing ones; the next
// free slot (FUN_1411e0c80).
// ref: FUN_1411e0c80
uint32_t BindEffectTextures(M2SceneDrawContext* ctx, uint32_t slot) {
    const uint16_t kinds[5] = { 0, 0, 0, 0, 3 };
    int32_t first = static_cast<int32_t>(slot) - 1;
    if (first < 0) {
        first = 0;
    }
    auto textures = static_cast<HTEXTURE*>(ctx->m_model->m_4e0);
    for (int32_t k = first; k < 3; k++) {
        if (textures[k] == nullptr) {
            ctx->m_context->BindTexture(2, slot, AsTex(TextureBlackGxTex()), 0);
            ctx->m_context->SetSampler(2, slot, s_defaultSampler);
        } else {
            void* tex = TextureGetGxTexOrDefaultRecreate(textures[k]);
            uint64_t sampler = ShaderEffectTextureSampler(TexUsage(tex), s_textureWrapU[kinds[k]], s_textureWrapV[kinds[k]]);
            ctx->m_context->BindTexture(2, slot, AsTex(tex), 0);
            ctx->m_context->SetSampler(2, slot, sampler);
        }
        slot++;
    }
    return slot;
}

// A texture transform of the batch's combo into texture matrix `index`, or the identity
// (FUN_1411e0e30).
// ref: FUN_1411e0e30
void SetBatchTextureMatrix(M2SceneDrawContext* ctx, uint32_t offset, uint32_t index) {
    M2Data* data = ctx->m_data;
    uint16_t transform = data->m_textureTransformCombos[ctx->m_batch->m_textureTransformComboIndex + offset];
    if (transform < data->m_textureTransforms.Count()) {
        ctx->m_shader.SetTextureMatrix(ctx->m_model->m_textureMatrices[transform].m, index, ctx->m_combiners != 0);
        return;
    }
    ctx->m_shader.ResetTextureMatrix(index);
}

// The batch's texture combiner weights, texture coordinate count, texture matrices by its
// vertex shader and its texture weights (FUN_1411e2840).
// ref: FUN_1411e2840
void SetBatchTextureConstants(M2SceneDrawContext* ctx) {
    ShaderEffectContext* shader = &ctx->m_shader;
    if (3 < static_cast<uint32_t>(ctx->m_kind)) {
        shader->ResetTextureMatrix(0);
        shader->ResetTextureMatrix(1);
        shader->SetBatch698(0);
        return;
    }
    float combiners[4] = {};
    if (ctx->m_combiners != 0) {
        const uint8_t* table = reinterpret_cast<const uint8_t*>(ctx->m_shared->m_textureCombiners);
        uint32_t material = ctx->m_batch->m_materialIndex;
        combiners[0] = static_cast<float>(table[material * 2]);
        combiners[1] = static_cast<float>(table[material * 2 + 1]);
    }
    shader->SetBatch724(combiners);
    uint16_t texCoords = 0;
    uint32_t kind = M2GetVertexShaderKind(ctx->m_batch->m_textureCount, ctx->m_batch->m_shaderId, &texCoords);
    shader->SetBatch698(texCoords);
    bool common = false;
    switch (kind) {
    case 0:
    case 3:
    case 4:
    case 6:
    case 7:
    case 8:
    case 9:
    case 0xd:
    case 0x11:
        common = true;
        break;
    case 10:
        SetBatchTextureMatrix(ctx, 0, 1);
        break;
    case 0xb:
        SetBatchTextureMatrix(ctx, 2, 1);
        common = true;
        break;
    case 0xe:
        SetBatchTextureMatrix(ctx, 2, 0);
        SetBatchTextureMatrix(ctx, 1, 1);
        common = true;
        break;
    case 2:
    case 0xc:
    case 0xf:
    case 0x12:
        SetBatchTextureMatrix(ctx, 1, 1);
        common = true;
        break;
    default:
        break;
    }
    if (common) {
        SetBatchTextureMatrix(ctx, 0, 0);
    }
    const M2Batch* batch = ctx->m_batch;
    float weights[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    if ((batch->m_flags & 0x40) != 0) {
        uint32_t count = batch->m_textureCount;
        if (4 < count) {
            count = 4;
        }
        M2Data* data = ctx->m_data;
        for (uint32_t i = 0; i < count; i++) {
            uint16_t weight = data->m_textureWeightCombos[batch->m_textureWeightComboIndex + i];
            weights[i] = ctx->m_model->m_textureWeights[weight].m_value;
        }
    }
    if (ctx->m_element->m_64 == 0) {
        shader->SetBatchColor(weights);
    }
}

// The batch's textures: the effect's own for effect kinds 1 and 5, the replaceable effect
// textures for flagged elements, then the batch's; how many slots were used (FUN_1411e2d60).
// ref: FUN_1411e2d60
uint32_t BindBatchTextures(M2SceneDrawContext* ctx) {
    CM2Model* model = ctx->m_model;
    SetBatchTextureConstants(ctx);
    if (3 < static_cast<uint32_t>(ctx->m_kind)) {
        return 0;
    }
    bool effectTextures = false;
    bool faded = false;
    uint32_t count = ctx->m_batch->m_textureCount;
    M2Element* element = ctx->m_element;
    int32_t effectKind = static_cast<int32_t>(element->m_64);
    uint32_t result;
    uint32_t first = 0;
    if (effectKind == 1) {
        void* tex = TextureGetGxTexOrDefaultRecreate(static_cast<HTEXTURE>(ctx->m_scene->m_defaultTexture));
        uint64_t sampler = ShaderEffectTextureSampler(TexUsage(tex), 0, 0);
        if ((model->m_flags & 1) == 0) {
            model->WaitForLoad(0);
        }
        uint16_t blend = model->m_shared->m_data->m_materials[ctx->m_batch->m_materialIndex].m_blendMode;
        if (g_m2BlendOpaque[blend] != 0) {
            ctx->m_context->BindTexture(2, 0, AsTex(tex), 0);
            ctx->m_context->SetSampler(2, 0, sampler);
            return 1;
        }
        ctx->m_context->BindTexture(2, 2, AsTex(tex), 0);
        ctx->m_context->SetSampler(2, 2, sampler);
        if (2 < count) {
            count = 2;
        }
        if (count == 0) {
            count = 1;
        }
        result = 3;
    } else if (effectKind == 5) {
        if (2 < count) {
            count = 2;
        }
        if (count == 0) {
            count = 1;
        }
        result = count + 3;
        void* effect = M2BaseModelGetEffect(element->m_model);
        if (effect != nullptr) {
            for (uint32_t k = 0; k < 3; k++) {
                uint8_t wrap = 1;
                if (k == 0) {
                    uint32_t flags = EffectField<uint32_t>(effect, 0x7c);
                    if ((flags & 0x20) != 0 && EffectField<int32_t>(effect, 0x78) != 1) {
                        wrap = 0;
                    }
                    if ((flags >> 8 & 1) != 0) {
                        wrap = 0;
                    }
                }
                auto handle = EffectField<HTEXTURE>(effect, 0x80 + k * 8);
                void* tex = nullptr;
                if (handle == nullptr || (tex = TextureGetGxTex(handle)) == nullptr) {
                    tex = TextureBlackGxTex();
                    if (tex == nullptr) {
                        return 0;
                    }
                }
                uint64_t sampler = ShaderEffectTextureSampler(TexUsage(tex), wrap, wrap);
                ctx->m_context->BindTexture(2, k + 2, AsTex(tex), 0);
                ctx->m_context->SetSampler(2, k + 2, sampler);
            }
        }
    } else {
        result = count;
        uint32_t flags = element->m_flags;
        if ((flags & 0x1400) != 0
            && ((1u << (effectKind & 0x1f) & 0x4c) == 0 || (effectKind == 6 && (flags & 1) != 0))) {
            result = BindEffectTextures(ctx, count);
            effectTextures = true;
            auto textures = static_cast<HTEXTURE*>(model->m_4e0);
            uint32_t elementFlags = ctx->m_element->m_flags;
            faded = ((elementFlags >> 10 & 1) != 0 && textures[3] != nullptr)
                || ((elementFlags >> 0xc & 1) != 0 && textures[4] != nullptr);
            if ((elementFlags >> 0xd & 1) != 0) {
                const void* effect = model->m_effect;
                float vector[4] = { model->m_648, EffectField<float>(effect, 8), EffectField<float>(effect, 0x98),
                    EffectField<float>(effect, 0x9c) };
                ctx->m_shader.SetBatchVectors(vector, 0, 1);
                float scale = 1.0f;
                if ((EffectField<uint8_t>(effect, 0x7c) & 0x80) != 0) {
                    scale = model->GetRenderScale();
                }
                float time = static_cast<float>(ctx->m_scene->m_time) * 0.001f;
                float scroll[12] = {};
                for (uint32_t c = 0; c < 3; c++) {
                    float u = time * EffectField<float>(effect, 0x18 + c * 4);
                    float v = time * EffectField<float>(effect, 0x24 + c * 4);
                    scroll[c * 4] = u - std::floor(u);
                    scroll[c * 4 + 1] = v - std::floor(v);
                    scroll[c * 4 + 2] = scale * EffectField<float>(effect, 0x30 + c * 4);
                    scroll[c * 4 + 3] = scale * EffectField<float>(effect, 0x3c + c * 4);
                }
                ctx->m_shader.SetBatchVectors(scroll, 1, 3);
                uint32_t effectFlags = EffectField<uint32_t>(effect, 0x7c);
                ctx->m_shader.SetBatch69c((effectFlags >> 9 & 1) != 0);
                ctx->m_shader.SetFrame7ec(static_cast<float>(effectFlags & 7));
                ctx->m_shader.SetFrame7f0(0.0f);
                const float rows[12] = {
                    1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                };
                ctx->m_shader.SetFrame864(rows);
                const uint32_t slots[3] = { 4, 5, 0xf };
                for (uint32_t t = 0; t < 3; t++) {
                    auto handle = EffectField<HTEXTURE>(model->m_effect, 0x80 + t * 8);
                    if (handle == nullptr) {
                        continue;
                    }
                    void* tex = TextureGetGxTexOrDefaultRecreate(handle);
                    uint64_t sampler = ShaderEffectTextureSampler(TexUsage(tex), 1, 1);
                    ctx->m_context->BindTexture(2, slots[t], AsTex(tex), 0);
                    ctx->m_context->SetSampler(2, slots[t], sampler);
                }
                HTEXTURE fade = nullptr;
                if ((elementFlags >> 10 & 1) != 0 && textures[3] != nullptr) {
                    fade = textures[3];
                } else if ((elementFlags >> 0xc & 1) != 0 && textures[4] != nullptr) {
                    fade = textures[4];
                }
                if (fade != nullptr) {
                    BindBatchTexture(ctx, 0xe, 0, TextureGetGxTexOrDefaultRecreate(fade));
                }
            }
        }
    }
    for (uint32_t i = first; i < count; i++) {
        BindBatchTexture(ctx, i, i, nullptr);
    }
    if (!faded) {
        if (!effectTextures) {
            return result;
        }
        int32_t kind = static_cast<int32_t>(ctx->m_element->m_64);
        if ((1u << (kind & 0x1f) & 0x4c) != 0) {
            if (kind != 6) {
                return result;
            }
            if ((ctx->m_element->m_flags & 1) == 0) {
                return result;
            }
        }
        const float none[4] = {};
        ctx->m_shader.SetBatchVectors(none, 0, 1);
        ctx->m_shader.SetBatch69c(false);
        return result;
    }
    if (4.76837158203125e-07f <= std::fabs(model->m_648 - 1.0f)) {
        if (4.76837158203125e-07f <= std::fabs(model->m_648)) {
            return result;
        }
        const float none[4] = {};
        ctx->m_shader.SetBatchVectors(none, 0, 1);
        ctx->m_shader.SetBatch69c(false);
        return result;
    }
    auto textures = static_cast<HTEXTURE*>(model->m_4e0);
    uint32_t elementFlags = ctx->m_element->m_flags;
    bool bind = true;
    HTEXTURE fade = nullptr;
    if ((elementFlags >> 10 & 1) != 0) {
        fade = textures[3];
    } else if ((elementFlags >> 0xc & 1) != 0) {
        fade = textures[4];
    } else {
        bind = false;
    }
    if (bind) {
        BindBatchTexture(ctx, 0, 0, TextureGetGxTexOrDefaultRecreate(fade));
    }
    const float one[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    ctx->m_shader.SetBatchVectors(one, 0, 1);
    ctx->m_shader.SetBatch69c(true);
    return result;
}


// The fog and lighting a batch draws with (FUN_1411e1150): for a lit material the model's
// light colours, its point lights and its shadow parameters; the fog kind its blend and the
// scene give, the fog's constants when `fog`, and the batch's base block; in the prepass only
// the lighting's plane distance.
// ref: FUN_1411e1150
void SetBatchLighting(M2SceneDrawContext* ctx, uint32_t pass, bool fog) {
    ShaderEffectContext* shader = &ctx->m_shader;
    CM2Scene* scene = ctx->m_scene;
    if (pass == 0) {
        float plane[4] = {};
        if ((ctx->m_lighting->m_flags & 8) != 0) {
            plane[0] = ctx->m_lighting->m_118;
        }
        shader->SetFrame7e4(plane);
        return;
    }
    float shadow[4] = {};
    const M2Material* material = ctx->m_material;
    if ((material->m_flags & 1) == 0 && g_m2ProjectedBlendLit[material->m_blendMode] != 0) {
        ctx->m_lit = 1;
        uint32_t flags = ctx->m_element->m_flags;
        shader->SetLighting(ctx->m_context, ctx->m_lighting, scene->m_drawView.m, 0, true);
        int32_t modelLights = static_cast<int32_t>(~(flags >> 8) & 1);
        if (modelLights != 0 && (scene->m_flags & 0x40) == 0) {
            shader->SetLights(ctx->m_context, ctx->m_lighting, scene->m_drawView.m, nullptr, scene->m_maxLights);
        }
        if ((ctx->m_element->m_flags & 0x40) != 0) {
            uint16_t lightingFlags = ctx->m_lighting->m_flags;
            float params[4];
            ShaderEffectShadowParams(params, ctx->m_context, modelLights, lightingFlags & 8, (lightingFlags & 8) != 0 ? 1 : 0);
            shadow[0] = params[0];
            shadow[1] = params[1];
            shadow[2] = params[2];
            shadow[3] = params[3];
            float plane[4] = {};
            if ((lightingFlags & 8) != 0) {
                plane[0] = ctx->m_lighting->m_118;
            }
            shader->SetFrame7e4(plane);
        }
    } else {
        ctx->m_lit = 0;
        shader->SetLit(0);
    }
    int32_t fogKind = 0;
    if (scene->m_d738 == 0 || ctx->m_pass != 10) {
        if ((material->m_flags & 2) == 0) {
            fogKind = static_cast<int32_t>(s_blendFogKind[material->m_blendMode]);
        }
    } else if ((ctx->m_element->m_flags & 1) == 0 && s_blendAdds[material->m_blendMode] == 0) {
        fogKind = 6 - (ctx->m_environment[0x5bc] != 0 ? 1 : 0);
    }
    ctx->m_fogKind = fogKind;
    CM2Model* model = AsModel(ctx->m_model);
    if (fogKind == 0 || ctx->m_environment[0x5be] == 0) {
        float block[16];
        std::memcpy(block, s_noFogParams, 0x10);
        block[4] = 0.0f;
        block[5] = 0.0f;
        block[6] = 0.0f;
        block[7] = ctx->m_environment[0x5be] != 0 ? 1.0f : 0.0f;
        std::memcpy(block + 8, s_noFogKinds, 0x10);
        std::memcpy(block + 12, shadow, 0x10);
        if (model != nullptr) {
            // FUN_1411bd8f0, its value unused here.
            M2ModelAfraScale(model);
        }
        shader->SetBatchBase(block, 1.0f);
        if (fog) {
            float constants[0x38];
            ShaderEffectDefaultFogConstants(constants);
            float* dest = ctx->m_context->LockConstants(0, 3, 0xe0);
            std::memcpy(dest, constants, 0xe0);
            ctx->m_context->UnlockConstants(0, 3);
        }
    } else {
        CM2ModelLighting* lighting = ctx->m_lighting;
        float height = 0.0f;
        uint32_t fogIndex = lighting->m_fogIndex;
        if (fogKind == 5) {
            fogIndex = static_cast<uint8_t>(~static_cast<uint8_t>(lighting->m_flags)) >> 7 | 6;
        } else if (fogKind == 6 && (lighting->m_flags & 0x40) != 0) {
            fogIndex = 7;
            const float* position = &ctx->m_model->m_world.m[12];
            if (position[1] * lighting->m_plane[1] + position[0] * lighting->m_plane[0] + position[2] * lighting->m_plane[2] < 0.0f) {
                float ray[6] = { 0.0f, 0.0f, 0.0f, position[0], position[1], position[2] };
                float t = 0.0f;
                float hit[3] = {};
                if (RayPlaneIntersect(ray, lighting->m_plane, &t, hit, 0.01f)) {
                    height = hit[2];
                }
            }
        }
        float fogWeight = g_shaderEffect5b88;
        if (model != nullptr && 7 < pass) {
            fogWeight = 1.0f;
        }
        float fogScale;
        if (ctx->m_fogKind == 2) {
            fogWeight = 0.0f;
            fogScale = 9.0f;
        } else {
            fogScale = 10.0f;
            if (ctx->m_fogKind != 3) {
                fogScale = static_cast<float>(static_cast<int32_t>(fogIndex));
                if (ctx->m_fogKind == 4) {
                    fogWeight = 0.5f;
                    fogScale = 11.0f;
                }
            }
        }
        if (fog) {
            const uint8_t* fogData = fogIndex == 1 ? ShaderEffectEnvironmentDefaultFog(ctx->m_environment)
                                                   : ShaderEffectEnvironmentFog(ctx->m_environment, fogIndex);
            float constants[0x38];
            ShaderEffectFogConstants(constants, fogData, height, &scene->m_drawCameraPosition.x, scene->m_drawView.m);
            float* dest = ctx->m_context->LockConstants(0, 3, 0xe0);
            std::memcpy(dest, constants, 0xe0);
            ctx->m_context->UnlockConstants(0, 3);
        }
        uint32_t fogTableKind = ShaderEffectEnvironmentFogKind(ctx->m_environment, fogIndex);
        float block[16];
        block[0] = static_cast<float>(fogIndex);
        block[1] = static_cast<float>(fogIndex);
        block[2] = fogScale;
        block[3] = 0.0f;
        block[4] = height;
        block[5] = 0.0f;
        block[6] = 0.0f;
        block[7] = fogWeight;
        if (fogIndex == 1) {
            block[8] = 2.0f;
            block[9] = 3.0f;
            block[10] = 4.0f;
            block[11] = static_cast<float>(fogTableKind);
        } else {
            std::memcpy(block + 8, s_noFogKinds, 0x10);
        }
        std::memcpy(block + 12, shadow, 0x10);
        float scale = 1.0f;
        if (model != nullptr) {
            scale = M2ModelAfraScale(model);
        }
        shader->SetBatchBase(block, scale);
    }
    ShaderEffectBindSceneTexture(ctx->m_sceneTexture, ctx->m_context);
}

// The fog colour the frame block carries, the lighting's own plane when the element is flagged
// so (negated in the reflection pass), set when it changed (FUN_1411e10b0).
// ref: FUN_1411e10b0
void SetBatchFogColor(M2SceneDrawContext* ctx) {
    float color[4];
    std::memcpy(color, ctx->m_scene->m_d568, 0x10);
    if ((ctx->m_element->m_flags & 2) != 0) {
        std::memcpy(color, ctx->m_lighting->m_plane, 0x10);
        if (ctx->m_pass == 10) {
            color[0] = -color[0];
            color[1] = -color[1];
            color[2] = -color[2];
            color[3] = -color[3];
        }
    }
    const float* current = &ctx->m_shader.Field<float>(ShaderEffectContext::FRAME);
    if (color[0] != current[0] || color[1] != current[1] || color[2] != current[2] || color[3] != current[3]) {
        ctx->m_shader.SetFrame7d4(color);
    }
}

// The model effect's constants by kind (FUN_1411dfa20); kind 5, the effect texture scroll, only
// in the passes that draw it.
// ref: FUN_1411dfa20
void SetEffectConstants(M2SceneDrawContext* ctx, M2Element* element, uint8_t pass) {
    CM2BaseModel* base = element->m_model;
    const void* effect = base->m_params.m_effect;
    if (effect == nullptr) {
        return;
    }
    ShaderEffectContext* shader = &ctx->m_shader;
    auto e = [effect](size_t offset) {
        return EffectField<float>(effect, offset);
    };
    switch (element->m_64) {
    case 1: {
        const float v[4] = { e(8), e(0xc), e(0x10), 1.0f };
        shader->SetBatchVectors(v, 0, 1);
        break;
    }
    case 2: {
        const float a[4] = { e(8), e(0xc), e(0x10), e(0x20) };
        const float b[4] = { e(0x14), e(0x18), e(0x1c), e(0x24) };
        shader->SetBatchVectors(a, 0, 1);
        shader->SetBatchVectors(b, 1, 1);
        break;
    }
    case 3: {
        const float a[4] = { e(8), e(0xc), e(0x10), 0.0f };
        const float b[4] = { e(0x14), e(0x18), e(0x1c), 1.0f };
        shader->SetBatchVectors(a, 0, 1);
        shader->SetBatchVectors(b, 1, 1);
        break;
    }
    case 4: {
        CM2BaseModel* model = base->m_type == 0 ? base : nullptr;
        const float v[4] = { 0.3f, 0.59f, 0.11f, model->GetEffectStrength() };
        shader->SetBatch754(v);
        break;
    }
    case 5: {
        if (pass != 0 && pass < 8 && pass != 5) {
            break;
        }
        CM2Model* model = AsModel(base);
        shader->SetFrame7ec(static_cast<float>(M2ModelEffectAxis(model)));
        uint32_t effectFlags = EffectField<uint32_t>(effect, 0x7c);
        shader->SetFrame7f0(static_cast<float>(effectFlags >> 8 & 1));
        float first = e(4);
        const float base0[4] = { first, e(8), e(0x98), e(0x9c) };
        shader->SetBatchVectors(base0, 0, 1);
        float sectionMatch = 1.0f;
        int32_t sectionId = EffectField<int32_t>(effect, 0x74);
        if (-1 < sectionId) {
            CM2Model* owner = AsModel(element->m_model);
            const uint8_t* sections;
            if (owner->m_600 == nullptr) {
                sections = reinterpret_cast<const uint8_t*>(owner->m_shared->m_skins[owner->m_lod].m_sections);
            } else {
                sections = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(owner->m_600) + 0x20);
            }
            uint16_t index = static_cast<const M2Batch*>(element->m_batch)->m_skinSectionIndex;
            if (*reinterpret_cast<const uint16_t*>(sections + static_cast<size_t>(index) * 0x30)
                != static_cast<uint32_t>(sectionId)) {
                sectionMatch = 0.0f;
            }
        }
        float block[0x14];
        block[0] = ctx->m_material->m_blendMode != 7 ? 0.0f : 1.0f;
        block[1] = e(0x70);
        block[2] = static_cast<float>(EffectField<int32_t>(effect, 0x78));
        block[3] = sectionMatch;
        block[4] = e(0x4c);
        block[5] = e(0x50);
        block[6] = e(0x54);
        block[7] = e(0x48);
        block[8] = static_cast<float>(EffectField<uint8_t>(effect, 0x6c));
        block[9] = static_cast<float>(EffectField<uint8_t>(effect, 0x6d));
        block[10] = static_cast<float>(EffectField<uint8_t>(effect, 0x6e));
        block[11] = static_cast<float>(EffectField<uint8_t>(effect, 0x6f));
        block[12] = first * e(0x58);
        block[13] = first * e(0x5c);
        block[14] = first * e(0x60);
        block[15] = e(0x64);
        block[16] = (effectFlags & 0x400) != 0 ? 1.0f : 0.0f;
        block[17] = (ctx->m_material->m_flags & 4) != 0 ? 1.0f : 0.0f;
        block[18] = e(0x68);
        block[19] = 0.0f;
        float* dest = ctx->m_context->LockConstants(2, 2, 0x50);
        std::memcpy(dest, block, 0x50);
        ctx->m_context->UnlockConstants(2, 2);
        const uint32_t slots[3] = { 1, 2, 3 };
        float time = static_cast<float>(ctx->m_scene->m_time) * 0.001f;
        float scale = 1.0f;
        if ((effectFlags & 0x80) != 0) {
            scale = element->m_model->GetRenderScale();
        }
        uint32_t skip;
        uint32_t count;
        if ((effectFlags & 0x20) == 0) {
            if ((effectFlags >> 8 & 1) == 0) {
                const float rows[12] = {
                    1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                };
                shader->SetFrame864(rows);
                skip = 0;
                count = 3;
            } else {
                C44Matrix world;
                MatrixIdentity(world.m);
                element->m_model->GetRenderMatrix(&world);
                float inverse[16];
                M2ModelEffectInverse(model, inverse);
                float product[16];
                MatrixMultiply(product, world.m, inverse);
                float columns[12];
                for (uint32_t c = 0; c < 3; c++) {
                    for (uint32_t r = 0; r < 4; r++) {
                        columns[c * 4 + r] = product[r * 4 + c];
                    }
                }
                shader->SetFrame864(columns);
                const float vector[4] = { e(0xa0) * e(0x98), (e(0xa4) * e(0x9c) + first + first) - 1.0f, e(0xa0), e(0xa4) };
                shader->SetBatchVectors(vector, 1, 1);
                skip = 1;
                count = 2;
            }
        } else {
            skip = EffectField<int32_t>(effect, 0x78) != 1 ? 1 : 0;
            if (skip != 0) {
                const float vector[4] = { e(0x98), e(0x9c), e(0xa0), e(0xa4) };
                shader->SetBatchVectors(vector, 1, 1);
            }
            count = 3 - skip;
        }
        for (uint32_t k = 0; k < count; k++) {
            uint32_t c = k + skip;
            float u = time * e(0x18 + c * 4);
            float v = time * e(0x24 + c * 4);
            const float vector[4] = { u - std::floor(u), v - std::floor(v), scale * e(0x30 + c * 4), scale * e(0x3c + c * 4) };
            shader->SetBatchVectors(vector, slots[c], 1);
        }
        break;
    }
    case 6: {
        const float a[4] = { (EffectField<uint8_t>(effect, 0x20) & 1) != 0 ? 1.0f : 0.0f, e(0x14),
            static_cast<float>(ctx->m_material->m_blendMode), (ctx->m_material->m_flags & 4) != 0 ? 1.0f : 0.0f };
        shader->SetBatchVectors(a, 1, 1);
        if ((element->m_flags & 1) != 0) {
            return;
        }
        float strength = e(0x18);
        float intensity = e(4);
        const float b[4] = { strength * e(8) * intensity, strength * e(0xc) * intensity, strength * e(0x10) * intensity,
            e(0x1c) };
        shader->SetBatchVectors(b, 0, 1);
        break;
    }
    case 7: {
        CM2BaseModel* root = base;
        for (CM2BaseModel* parent = base->m_parent; parent != nullptr; parent = parent->m_parent) {
            root = parent;
        }
        float bottom = 0.0f;
        float height = 1.0f;
        M2ModelSequenceHeight(static_cast<CM2Model*>(root), &bottom, &height);
        float scale = root->GetRenderScale();
        const float* world = root->m_world.m;
        float inverseHeight = 1.0f / (scale * height);
        uint8_t blendWord = static_cast<uint8_t>(g_m2BlendWords[g_m2EffectBlend[(element->m_batchKey >> 32) & 0xf]]);
        uint32_t blend = M2BlendModeIndex(blendWord);
        float axis[4] = { world[8], world[9], world[10], 0.0f };
        axis[3] = blend - 3 < 2 ? 1.0f : 0.0f;
        float length = world[11] * world[11] + axis[2] * axis[2] + axis[0] * axis[0] + axis[1] * axis[1];
        if (2.3841858e-07f < length) {
            float inverse = 1.0f / std::sqrt(length);
            axis[0] *= inverse;
            axis[1] *= inverse;
            axis[2] *= inverse;
        }
        uint32_t effectFlags = EffectField<uint32_t>(effect, 0x40);
        float block[0x1c];
        block[0] = e(8);
        block[1] = e(0xc);
        block[2] = e(0x10);
        block[3] = (ctx->m_material->m_flags & 4) != 0 ? 1.0f : 0.0f;
        block[4] = e(0x14);
        block[5] = e(0x18);
        block[6] = e(0x1c);
        block[7] = ctx->m_material->m_blendMode != 0 ? 0.0f : 1.0f;
        block[8] = e(0x20);
        block[9] = e(0x24);
        block[10] = e(0x28);
        block[11] = e(0x2c);
        block[12] = e(0x34);
        block[13] = e(0x38);
        block[14] = e(0x3c);
        block[15] = e(0x30);
        block[16] = bottom * world[8] + world[12];
        block[17] = bottom * world[9] + world[13];
        block[18] = bottom * world[10] + world[14];
        block[19] = inverseHeight;
        block[20] = axis[0];
        block[21] = axis[1];
        block[22] = axis[2];
        block[23] = axis[3];
        block[24] = (effectFlags & 2) != 0 ? 1.0f : 0.0f;
        block[25] = e(4);
        block[26] = (effectFlags & 4) != 0 ? 1.0f : 0.0f;
        block[27] = (effectFlags & 8) != 0 ? 1.0f : 0.0f;
        float* dest = ctx->m_context->LockConstants(2, 2, 0x70);
        std::memcpy(dest, block, 0x70);
        ctx->m_context->UnlockConstants(2, 2);
        break;
    }
    default:
        break;
    }
}

// A batch's colour constants, against the element before it: its model's colour multiply and
// add with the batch's animated colour, its alpha, the impact the model carries, the edge fade
// and DBOC of its material, its blend kind and shading rate (FUN_1411e19c0).
// ref: FUN_1411e19c0
void SetBatchConstants(M2SceneDrawContext* ctx) {
    ShaderEffectContext* shader = &ctx->m_shader;
    M2Element* element = ctx->m_element;
    CM2BaseModel* elementModel = element->m_model;
    if (elementModel->m_type != 0) {
        if (elementModel->m_type == 3) {
            CM2Model* model = ctx->m_model;
            const float color[4] = { model->m_params.m_color.x, model->m_params.m_color.y, model->m_params.m_color.z, 1.0f };
            shader->SetFrame844(color);
            const float add[4] = { model->m_params.m_colorAdd.x, model->m_params.m_colorAdd.y,
                model->m_params.m_colorAdd.z, 0.0f };
            shader->SetLightColor(nullptr, add, false);
            shader->SetBatch694(model->m_alpha);
        }
    } else {
        bool reset = true;
        if (ctx->m_kind == ctx->m_prevKind && ctx->m_material == ctx->m_prevMaterial) {
            M2Element* prev = ctx->m_prevElement;
            uint32_t flagA = element->m_flags & 1;
            uint32_t flagB = prev->m_flags & 1;
            CM2BaseModel* prevModel = prev->m_model;
            if (flagA == flagB && prev->m_alpha == element->m_alpha && element->m_64 == 0 && prev->m_64 == 0
                && ((prevModel->m_flags ^ elementModel->m_flags) >> 26 & 1) == 0) {
                int32_t a = flagA != 0 && ctx->m_model->m_0d != 0 ? 1 : 0;
                int32_t b = flagB != 0 && prevModel->m_0d != 0 ? 1 : 0;
                if (a == b && !M2BaseModelEitherHasImpact(elementModel, prevModel)) {
                    reset = false;
                }
            }
        }
        if (reset) {
            const float zero[4] = {};
            shader->SetBatch754(zero);
            if (ctx->m_element->m_64 != 0) {
                SetEffectConstants(ctx, ctx->m_element, ctx->m_pass);
            }
            const M2Material* material = ctx->m_material;
            const M2Material* prevMaterial = ctx->m_prevMaterial;
            if ((ctx->m_kind != ctx->m_prevKind || prevMaterial == nullptr
                    || ((material->m_flags ^ prevMaterial->m_flags) & 0x1000) != 0)
                && g_m2OutlineEnabled && ctx->m_scene->m_daf0 != 0 && (material->m_flags & 0x1000) != 0
                && ctx->m_pass != 0 && ctx->m_pass != 5) {
                auto outline = reinterpret_cast<const uint8_t*>(ctx->m_scene->m_daf0);
                ctx->m_context->BindTexture(2, 10, *reinterpret_cast<gxDx12Backend::Tex* const*>(outline + 0x48), 0);
                ctx->m_context->SetSampler(2, 10, 0x800200000000ull);
            }
        }
        element = ctx->m_element;
        if (ElementKind(element) < 4) {
            bool colors = true;
            const M2Material* prevMaterial = ctx->m_prevMaterial;
            if (ctx->m_kind == ctx->m_prevKind && ctx->m_shared == ctx->m_prevShared && ctx->m_lit == ctx->m_prevLit
                && prevMaterial != nullptr && ctx->m_material->m_blendMode == prevMaterial->m_blendMode
                && ctx->m_prevBatch != nullptr && ctx->m_batch->m_colorIndex == ctx->m_prevBatch->m_colorIndex
                && ctx->m_prevElement->m_alpha == element->m_alpha) {
                CM2Model* prevModel = ctx->m_prevModel;
                CM2Model* model = ctx->m_model;
                if (prevModel->m_params.m_color.x == model->m_params.m_color.x
                    && prevModel->m_params.m_color.y == model->m_params.m_color.y
                    && prevModel->m_params.m_color.z == model->m_params.m_color.z
                    && prevModel->m_params.m_colorAdd.x == model->m_params.m_colorAdd.x
                    && prevModel->m_params.m_colorAdd.y == model->m_params.m_colorAdd.y
                    && prevModel->m_params.m_colorAdd.z == model->m_params.m_colorAdd.z
                    && !M2BaseModelEitherHasImpact(elementModel, prevModel)) {
                    colors = false;
                }
            }
            if (colors) {
                CM2Model* model = ctx->m_model;
                uint32_t additive = 0;
                float mult[3];
                float add[3];
                uint16_t blend = ctx->m_material->m_blendMode;
                if (blend == 5 || blend == 6) {
                    float value = blend == 5 ? 1.0f : 0.5f;
                    additive = 1;
                    add[0] = value;
                    add[1] = value;
                    add[2] = value;
                    mult[0] = 0.0f;
                    mult[1] = 0.0f;
                    mult[2] = 0.0f;
                } else {
                    mult[0] = model->m_params.m_color.x;
                    mult[1] = model->m_params.m_color.y;
                    mult[2] = model->m_params.m_color.z;
                    uint16_t colorIndex = ctx->m_batch->m_colorIndex;
                    if (colorIndex < ctx->m_data->m_colors.Count()) {
                        const M2ColorState* color = &model->m_colors[colorIndex];
                        mult[0] *= color->m_color.m_value.x;
                        mult[1] *= color->m_color.m_value.y;
                        mult[2] *= color->m_color.m_value.z;
                    }
                    add[0] = model->m_params.m_colorAdd.x;
                    add[1] = model->m_params.m_colorAdd.y;
                    add[2] = model->m_params.m_colorAdd.z;
                    if (ctx->m_lit == 0) {
                        add[0] += mult[0];
                        add[1] += mult[1];
                        add[2] += mult[2];
                        mult[0] = 0.0f;
                        mult[1] = 0.0f;
                        mult[2] = 0.0f;
                    }
                }
                const float multiply[4] = { mult[0], mult[1], mult[2], 1.0f };
                shader->SetFrame844(multiply);
                const float offset[4] = { add[0], add[1], add[2], 0.0f };
                shader->SetLightColor(nullptr, offset, false);
                shader->SetBatch694(ctx->m_element->m_alpha);
                shader->SetBatch69c(additive != 0);
                bool shadowed = (static_cast<CM2Model*>(elementModel)->m_modelFlags2 & 0x4000) != 0
                    && (ctx->m_element->m_batchKey >> 32 & 0xf) == 4;
                shader->SetBatch6a0(shadowed);
                auto impact = static_cast<const float*>(elementModel->m_impact);
                if (impact == nullptr || reinterpret_cast<const uint8_t*>(impact)[0xf] == 0
                    || 1 < ctx->m_material->m_blendMode) {
                    const float zero[4] = {};
                    shader->SetBatch734(zero, zero);
                } else {
                    const float* v = ctx->m_scene->m_drawView.m;
                    const float* p = ctx->m_scene->m_drawProjection.m;
                    float position[4] = {
                        impact[1] * v[4] + impact[0] * v[0] + impact[2] * v[8] + v[12],
                        impact[1] * v[5] + impact[0] * v[1] + impact[2] * v[9] + v[13],
                        impact[1] * v[6] + impact[0] * v[2] + impact[2] * v[10] + v[14],
                        0.0f,
                    };
                    float vp[16];
                    MatrixMultiply(vp, v, p);
                    float sphere[4];
                    elementModel->GetAnimatedSphere(sphere);
                    float scale = 1.0f / std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    const float moved[3] = { v[0] * scale * 0.001f + sphere[0], v[1] * scale * 0.001f + sphere[1],
                        sphere[2] + v[2] * scale * 0.001f };
                    float d[3];
                    for (uint32_t c = 0; c < 3; c++) {
                        d[c] = (moved[0] * vp[c] + moved[1] * vp[4 + c] + moved[2] * vp[8 + c] + vp[12 + c])
                            - (sphere[0] * vp[c] + sphere[1] * vp[4 + c] + sphere[2] * vp[8 + c] + vp[12 + c]);
                    }
                    position[3] = std::sqrt(d[1] * d[1] + d[0] * d[0] + d[2] * d[2]) * 999.99994f;
                    uint32_t packed = reinterpret_cast<const uint32_t*>(impact)[3];
                    float intensity = impact[4] * (1.0f / 255.0f);
                    const float color[4] = {
                        static_cast<float>(packed >> 0x10 & 0xff) * intensity,
                        static_cast<float>(packed >> 8 & 0xff) * intensity,
                        static_cast<float>(packed & 0xff) * intensity,
                        static_cast<float>(packed >> 0x18) * (1.0f / 255.0f),
                    };
                    shader->SetBatch734(position, color);
                }
            }
            if (ctx->m_edgeFade != ctx->m_prevEdgeFade || ctx->m_prevBatch == nullptr) {
                const float* fade = ctx->m_edgeFade;
                if (fade == nullptr || (static_cast<uint8_t>(ctx->m_batch->m_geosetIndex) & 8) == 0) {
                    shader->SetVertex8f4(ctx->m_context, 0.72f, 0.385f, 1.22f);
                } else {
                    shader->SetVertex8f4(ctx->m_context, fade[0], fade[1], fade[2]);
                }
            }
            if (ctx->m_dboc != ctx->m_prevDboc || ctx->m_prevBatch == nullptr) {
                const float* dboc = ctx->m_dboc;
                if (dboc == nullptr) {
                    shader->SetBatch7b4(0.6666667f, 1.5f);
                } else {
                    shader->SetBatch7b4(dboc[0], dboc[1]);
                }
            }
        } else if (ctx->m_kind != ctx->m_prevKind || ctx->m_prevElement->m_alpha != element->m_alpha) {
            const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            shader->SetFrame844(one);
            const float zero[4] = {};
            shader->SetLightColor(nullptr, zero, false);
            shader->SetBatch694(ctx->m_element->m_alpha);
            shader->SetBatch69c(false);
        }
        uint8_t blendKind = s_blendShaderKind[ctx->m_material->m_blendMode];
        if (ctx->m_element->m_64 == 6) {
            blendKind = 0x11;
        } else if (ctx->m_element->m_64 == 8) {
            blendKind = 0;
        }
        uint32_t lit = (ctx->m_material->m_flags & 1) == 0 && g_m2ProjectedBlendLit[ctx->m_material->m_blendMode] != 0 ? 1 : 0;
        shader->SetBlend(&g_renderSettings, blendKind, lit, 0, 0);
        float fade = ctx->m_model->m_params.m_fade;
        ShaderEffectSetShadingRate(ctx->m_context, s_blendShaderKind[ctx->m_material->m_blendMode], fade);
    }
    const float zero[4] = {};
    shader->SetFrame7f4(zero);
    shader->UploadLightBlock(ctx->m_context);
    ctx->m_prevMaterial = ctx->m_material;
}

// The geometry: the model's merged buffer for a flagged element, else the profile's indices,
// and the profile's vertices; each only when the element before drew from other ones.
void BindBatchGeometry(M2SceneDrawContext* ctx) {
    CM2Model* model = ctx->m_model;
    if ((ctx->m_element->m_flags & 4) == 0) {
        if (ctx->m_kind != ctx->m_prevKind || ctx->m_shared != ctx->m_prevShared
            || (ctx->m_prevElement->m_flags & 4) != 0 || static_cast<int32_t>(model->m_lod) != ctx->m_prevLod) {
            SharedSetIndexBuffer(ctx->m_shared, ctx->m_context, model->m_lod);
            model = ctx->m_model;
        }
    } else if (ctx->m_kind != ctx->m_prevKind || model != ctx->m_prevModel) {
        M2ModelBindGpuBatchIndices(model, ctx->m_context);
        model = ctx->m_model;
    }
    if (ctx->m_kind != ctx->m_prevKind || ctx->m_shared != ctx->m_prevShared || ctx->m_lod != ctx->m_prevLod) {
        SharedSetVertexBuffer(ctx->m_shared, ctx->m_context, model->m_lod);
    }
}

// The element's batch, section and material, with the material's edge fade and DBOC entry
// when `lookups`.
void SetElementBatch(M2SceneDrawContext* ctx, bool lookups) {
    ctx->m_batch = static_cast<const M2Batch*>(ctx->m_element->m_batch);
    ctx->m_section = ctx->m_element->m_section;
    ctx->m_material = &ctx->m_data->m_materials[ctx->m_batch->m_materialIndex];
    if (lookups) {
        ctx->m_edgeFade = SharedEdgeFade(ctx->m_shared, ctx->m_batch->m_materialIndex);
        ctx->m_dboc = SharedDboc(ctx->m_shared, ctx->m_batch->m_materialIndex);
    }
}

// A model batch (FUN_1411d9d90).
// ref: FUN_1411d9d90
void DrawBatch(M2SceneDrawContext* ctx) {
    M2Element* element = ctx->m_element;
    const M2Batch* elementBatch = static_cast<const M2Batch*>(element->m_batch);
    BATCHCONTEXT context;
    int32_t fileDataId = ctx->m_model->GetFileDataId();
    ErrorContextPush(&context.m_base, DescribeBatch);
    context.m_format = "M2 Batch: model=%d batch=%u skinSection=%u material=%u";
    context.m_material = elementBatch->m_materialIndex;
    context.m_section = elementBatch->m_skinSectionIndex;
    context.m_batch = element->m_batchIndex;
    context.m_fileDataId = fileDataId;
    ctx->m_model->GetFileDataId();
    MODELMEMORY memory;
    ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
    memory.m_model = ctx->m_model;
    SetElementBatch(ctx, true);
    const C44Matrix* bones = ctx->m_model->GetBoneMatrices();
    SetBatchLighting(ctx, ctx->m_pass, false);
    SetBatchFogColor(ctx);
    SetBatchConstants(ctx);
    BindBatchTextures(ctx);
    CM2Model* model = ctx->m_model;
    if ((model->m_modelFlags & 0x8000) != 0 && ctx->m_kind != 0 && ctx->m_kind != 5) {
        float a[4];
        float b[4];
        float c[4];
        GxColorToFloat4(a, model->m_debugColors[0]);
        GxColorToFloat4(b, ctx->m_model->m_debugColors[1]);
        GxColorToFloat4(c, ctx->m_model->m_debugColors[2]);
        ctx->m_shader.SetBatchColors(a, b, c);
    }
    (void)bones;
    if (ctx->m_kind != ctx->m_prevKind || ctx->m_model != ctx->m_prevModel
        || ctx->m_shader.Field<uint8_t>(ShaderEffectContext::FRAME_DIRTY) != 0
        || ctx->m_section->m_boneComboIndex != ctx->m_prevSection->m_boneComboIndex) {
        WriteBatchMatrices(ctx, true, true);
    }
    BindBatchGeometry(ctx);
    DrawSection(ctx);
    ErrorMemoryContextPop(&memory.m_base);
    ErrorContextPop(&context.m_base);
}

// A batch of the depth prepass (FUN_1411dd020): the full frame block before the matrices, no
// material lookups.
// ref: FUN_1411dd020
void DrawPrepassBatch(M2SceneDrawContext* ctx) {
    M2Element* element = ctx->m_element;
    const M2Batch* elementBatch = static_cast<const M2Batch*>(element->m_batch);
    BATCHCONTEXT context;
    int32_t fileDataId = ctx->m_model->GetFileDataId();
    ErrorContextPush(&context.m_base, DescribeBatch);
    context.m_format = "M2 Prepass Batch: model=%d batch=%u skinSection=%u material=%u";
    context.m_material = elementBatch->m_materialIndex;
    context.m_section = elementBatch->m_skinSectionIndex;
    context.m_batch = element->m_batchIndex;
    context.m_fileDataId = fileDataId;
    MODELMEMORY memory;
    ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
    memory.m_model = ctx->m_model;
    ctx->m_model->GetFileDataId();
    SetElementBatch(ctx, false);
    SetBatchLighting(ctx, ctx->m_pass, false);
    SetBatchFogColor(ctx);
    SetBatchConstants(ctx);
    BindBatchTextures(ctx);
    if (ctx->m_kind != ctx->m_prevKind || ctx->m_model != ctx->m_prevModel
        || ctx->m_shader.Field<uint8_t>(ShaderEffectContext::FRAME_DIRTY) != 0
        || ctx->m_section->m_boneComboIndex != ctx->m_prevSection->m_boneComboIndex) {
        WriteBatchMatrices(ctx, false, false);
    }
    if ((ctx->m_element->m_flags & 4) == 0) {
        if (ctx->m_kind != ctx->m_prevKind || ctx->m_shared != ctx->m_prevShared
            || (ctx->m_prevElement->m_flags & 4) != 0) {
            SharedSetIndexBuffer(ctx->m_shared, ctx->m_context, ctx->m_model->m_lod);
        }
    } else if (ctx->m_kind != ctx->m_prevKind || ctx->m_model != ctx->m_prevModel) {
        M2ModelBindGpuBatchIndices(ctx->m_model, ctx->m_context);
    }
    if (ctx->m_kind != ctx->m_prevKind || ctx->m_shared != ctx->m_prevShared || ctx->m_lod != ctx->m_prevLod) {
        SharedSetVertexBuffer(ctx->m_shared, ctx->m_context, ctx->m_model->m_lod);
    }
    DrawSection(ctx);
    ErrorMemoryContextPop(&memory.m_base);
    ErrorContextPop(&context.m_base);
}

// One model of a doodad run: its matrix and bones, looked up through the run's own model.
void WriteDoodadMatrices(M2SceneDrawContext* ctx, float* out, const M2Element* doodad) {
    CM2Model* model = AsModel(doodad->m_model);
    WriteBoneMatrices(out, model, model->GetBoneMatrices(), ctx->m_model, ctx->m_section, false);
}

// A run of doodads drawing the same batch, one draw each (FUN_1411da350).
// ref: FUN_1411da350
void DrawDoodadBatch(M2SceneDrawContext* ctx, const M2Element* elements, uint32_t count, const uint32_t* indices) {
    SetElementBatch(ctx, true);
    uint8_t lod = ctx->m_model->m_lod;
    SetBatchLighting(ctx, ctx->m_pass, false);
    SetBatchFogColor(ctx);
    SetBatchConstants(ctx);
    BindBatchTextures(ctx);
    ctx->m_shader.FlushBatch(ctx->m_context, 400);
    SharedSetIndexBuffer(ctx->m_shared, ctx->m_context, lod);
    SharedSetVertexBuffer(ctx->m_shared, ctx->m_context, lod);
    const M2SkinSection* section = ctx->m_section;
    uint32_t start = section->IndexStart();
    uint32_t indexCount = section->IndexCount();
    ctx->m_context->SetPipeline(ctx->m_element->m_pipeline);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size = (ctx->m_section->m_boneCount + 6) * 0x30;
        auto dest = reinterpret_cast<uint8_t*>(ctx->m_context->LockConstants(0, 0, size));
        auto matrices = reinterpret_cast<float*>(ctx->m_shader.CopyFrame(dest, true));
        WriteDoodadMatrices(ctx, matrices, &elements[indices[i]]);
        ctx->m_context->UnlockConstants(0, 0);
        ctx->m_context->DrawIndexed(indexCount, 1, start, 0, 0, 0);
    }
}

// The same instanced: as many doodads per draw as the profile's bone slots allow, each
// instance's matrices after a count header (FUN_1411da6c0).
// ref: FUN_1411da6c0
void DrawDoodadBatchInstanced(M2SceneDrawContext* ctx, const M2Element* elements, uint32_t count,
    const uint32_t* indices) {
    SetElementBatch(ctx, true);
    uint8_t lod = ctx->m_model->m_lod;
    SetBatchLighting(ctx, ctx->m_pass, false);
    SetBatchFogColor(ctx);
    SetBatchConstants(ctx);
    BindBatchTextures(ctx);
    ctx->m_shader.FlushBatch(ctx->m_context, 400);
    uint32_t perDraw = ctx->m_shared->m_skins[lod].m_bonesPerSlot;
    SharedSetIndexBuffer(ctx->m_shared, ctx->m_context, lod);
    SharedSetVertexBuffer(ctx->m_shared, ctx->m_context, lod);
    const M2SkinSection* section = ctx->m_section;
    uint32_t start = section->IndexStart();
    uint32_t indexCount = section->IndexCount();
    ctx->m_context->SetPipeline(ctx->m_element->m_pipeline);
    for (uint32_t done = 0; done < count; done += perDraw) {
        uint32_t batch = perDraw;
        if (count - done < perDraw) {
            batch = count - done;
        }
        uint32_t bones = ctx->m_section->m_boneCount + 1;
        uint32_t size = (bones * batch + 6) * 0x30;
        auto dest = reinterpret_cast<uint8_t*>(ctx->m_context->LockConstants(0, 0, size));
        auto header = reinterpret_cast<int32_t*>(ctx->m_shader.CopyFrame(dest, true));
        header[0] = static_cast<int32_t>(bones);
        header[1] = 0;
        header[2] = 0;
        header[3] = 0;
        auto out = reinterpret_cast<float*>(header + 0xc);
        for (uint32_t i = 0; i < batch; i++) {
            WriteDoodadMatrices(ctx, out, &elements[indices[done + i]]);
            out += bones * 0xc;
        }
        ctx->m_context->UnlockConstants(0, 0);
        ctx->m_context->DrawIndexed(indexCount, batch, start, 0, 0, 0);
    }
}

// The draw context of a pass: the scene's environment, cache and flag, the constants copied from
// the caller's, and the scene texture they carry.
void InitializeDrawContext(M2SceneDrawContext* ctx, CM2Scene* scene, GxRenderContext* context,
    const ShaderEffectContext* shader) {
    ctx->m_context = context;
    ctx->m_environment = static_cast<const uint8_t*>(scene->m_038);
    MatrixIdentity(ctx->m_identity);
    ctx->m_scene = scene;
    ctx->m_cache = scene->m_cache;
    ctx->m_sceneFlag8 = scene->m_flags & 8;
    ctx->m_defaultMaterial.m_flags = 0;
    ctx->m_defaultMaterial.m_blendMode = 0;
    std::memcpy(&ctx->m_shader, shader, sizeof(ShaderEffectContext));
    ctx->m_sceneTexture = shader->Field<void*>(0x908);
}

// An element of a model of type 2 or 3 (the other arms of FUN_1411d9650: FUN_141257110 draws
// type 2, FUN_1411dbca0 type 3). Neither type's elements are built yet (FUN_1411d60e0 and
// FUN_1411d5950 are stubs), so nothing reaches here.
void DrawOtherModelElement(M2SceneDrawContext* ctx, M2Element* element) {
    (void)ctx;
    (void)element;
    FROST_UNIMPLEMENTED();
}

// The custom draw a model's m_590 object does in the element kind 7 pass slot (its vtable's
// second entry).
class M2ModelCustomDraw {
    public:
    virtual void Slot00();
    virtual void Draw(GxRenderContext* context, CM2Model* model, CM2ModelLighting* lighting, const C44Matrix* view,
        const C44Matrix* projection) = 0;
};

// The projection a batch is built into (FUN_1411be9e0 fills it from +0x10).
struct M2BATCHPROJECTION {
    void* m_owner;                      // 0x00
    void* m_030;                        // 0x08
    uint8_t m_box[0x18];                // 0x10
    float m_pair[8];                    // 0x28
    float m_third[4];                   // 0x48
    uint32_t m_flags;                   // 0x58
    uint32_t m_pad5c;
};

// A projected batch (FUN_1411daa80): drawn by the world scene's projection callback when it has
// one, else by the scene's decal callback with the batch's projection.
// ref: FUN_1411daa80
void DrawProjectedBatch(M2SceneDrawContext* ctx) {
    M2Element* element = ctx->m_element;
    auto elementBatch = static_cast<const M2Batch*>(element->m_batch);
    int32_t fileDataId = ctx->m_model->GetFileDataId();
    BATCHCONTEXT context;
    ErrorContextPush(&context.m_base, DescribeBatch);
    context.m_format = "M2 BatchProj: model=%d batch=%u skinSection=%u material=%u";
    context.m_material = elementBatch->m_materialIndex;
    context.m_section = elementBatch->m_skinSectionIndex;
    context.m_batch = element->m_batchIndex;
    context.m_fileDataId = fileDataId;
    CM2Model* model = ctx->m_model;
    MODELMEMORY memory;
    ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
    memory.m_model = model;
    if ((model->m_modelFlags & 0x10001) != 0x10000) {
        M2SceneDebugLog(ctx->m_context, "BatchProj: model=%s index=%d", ctx->m_model->GetName(),
            ctx->m_element->m_batchIndex);
        SetElementBatch(ctx, false);
        auto combiners = reinterpret_cast<const uint8_t*>(ctx->m_shared->m_textureCombiners);
        uint32_t materialIndex = ctx->m_batch->m_materialIndex;
        if (combiners != nullptr && (combiners[materialIndex * 2] != 0 || combiners[materialIndex * 2 + 1] != 0)) {
            ctx->m_combiners = 1;
        }
        uint32_t slots = BindBatchTextures(ctx);
        SetBatchLighting(ctx, ctx->m_pass, true);
        SetBatchFogColor(ctx);
        SetBatchConstants(ctx);
        int32_t passBlend = g_m2PassBlend[ctx->m_pass][ctx->m_material->m_blendMode];
        uint32_t twoTextures = passBlend == 5 || slots < 2 ? 0 : 4;
        CM2Model* drawn = ctx->m_model;
        float color[3] = { drawn->m_params.m_color.x, drawn->m_params.m_color.y, drawn->m_params.m_color.z };
        if (ctx->m_batch->m_colorIndex < ctx->m_data->m_colors.Count()) {
            const M2ColorState* state = &drawn->m_colors[ctx->m_batch->m_colorIndex];
            color[0] *= state->m_color.m_value.x;
            color[1] *= state->m_color.m_value.y;
            color[2] *= state->m_color.m_value.z;
        }
        color[0] += drawn->m_params.m_colorAdd.x;
        color[1] += drawn->m_params.m_colorAdd.y;
        color[2] += drawn->m_params.m_colorAdd.z;
        for (float& c : color) {
            if (c < 0.0f) {
                c = 0.0f;
            } else if (1.0f <= c) {
                c = 1.0f;
            }
        }
        M2PROJECTEDDRAW draw = {};
        draw.m_color = 0xff000000u | static_cast<uint32_t>(static_cast<uint8_t>(std::lround(color[0] * 255.0f))) << 16
            | static_cast<uint32_t>(static_cast<uint8_t>(std::lround(color[1] * 255.0f))) << 8
            | static_cast<uint32_t>(static_cast<uint8_t>(std::lround(color[2] * 255.0f)));
        uint32_t blended = passBlend != 5 ? 2 : 0;
        uint32_t flags = (ctx->m_lit != 0 ? 1 : 0) | blended | twoTextures;
        draw.m_context = ctx->m_context;
        draw.m_shader = &ctx->m_shader;
        draw.m_element = ctx->m_element;
        CM2Scene* scene = ctx->m_scene;
        draw.m_owner = scene->m_owner;
        draw.m_030 = scene->m_030;
        if (scene->m_projectCheckCallback == nullptr || scene->m_projectCallback == nullptr) {
            if (scene->m_decalCallback != nullptr) {
                M2BATCHPROJECTION projection = {};
                projection.m_owner = scene->m_owner;
                projection.m_030 = scene->m_030;
                model->BuildBatchProjection(&projection, ctx->m_section);
                ctx->m_shader.SetFrame864(projection.m_pair, projection.m_third);
                ctx->m_shader.FlushBatch(ctx->m_context, 400);
                draw.m_object = scene->m_decalParam;
                std::memcpy(draw.m_box, projection.m_box, sizeof(draw.m_box));
                uint32_t projected = projection.m_flags;
                draw.m_flags = (((projected & 4) * 2 | (flags & 6)) * 2 | (projected & 3)) * 2 | (flags & 1);
                scene->m_decalCallback(&draw);
            }
        } else {
            draw.m_object = model->m_5f8[ctx->m_element->m_batchIndex];
            draw.m_flags = flags;
            reinterpret_cast<void (*)(M2PROJECTEDDRAW*)>(scene->m_projectCheckCallback)(&draw);
        }
    }
    ErrorMemoryContextPop(&memory.m_base);
    ErrorContextPop(&context.m_base);
}

// The model's matrix and its bones' at a vertex slot.
void WriteSlotMatrices(M2SceneDrawContext* ctx, uint32_t slot, uint32_t size, uint32_t offset) {
    auto dest = reinterpret_cast<uint8_t*>(ctx->m_context->LockConstants(0, slot, size));
    WriteBoneMatrices(reinterpret_cast<float*>(dest + offset), ctx->m_model, ctx->m_model->GetBoneMatrices(),
        ctx->m_model, ctx->m_section, false);
    ctx->m_context->UnlockConstants(0, slot);
}

// The outline and fill passes' geometry: the merged indices for a model that has them.
void BindOutlineGeometry(M2SceneDrawContext* ctx) {
    CM2Model* model = ctx->m_model;
    if (model != ctx->m_prevModel || ctx->m_kind != ctx->m_prevKind) {
        if (model->m_600 == nullptr) {
            SharedSetIndexBuffer(ctx->m_shared, ctx->m_context, model->m_lod);
        } else {
            M2ModelBindGpuBatchIndices(model, ctx->m_context);
        }
    }
    if (ctx->m_kind != ctx->m_prevKind || ctx->m_shared != ctx->m_prevShared || ctx->m_lod != ctx->m_prevLod) {
        SharedSetVertexBuffer(ctx->m_shared, ctx->m_context, ctx->m_model->m_lod);
    }
}

// A batch's texture with the address modes its M2Texture's flags give directly (bit 0 U, bit 1
// V), grey when missing.
void BindFlaggedTexture(M2SceneDrawContext* ctx, uint32_t slot, uint32_t index, bool greyWhenMissing) {
    void* tex = nullptr;
    auto handle = static_cast<HTEXTURE>(ctx->m_model->m_textures[index]);
    if (handle != nullptr) {
        tex = TextureGetGxTex(handle);
    }
    if (tex == nullptr) {
        if (!greyWhenMissing) {
            return;
        }
        tex = TextureGreyGxTex();
    }
    uint8_t flags = static_cast<uint8_t>(ctx->m_model->m_shared->m_data->m_textures[index].m_flags);
    uint64_t sampler = ShaderEffectTextureSampler(TexUsage(tex), flags & 1, flags >> 1 & 1);
    ctx->m_context->BindTexture(2, slot, AsTex(tex), 0);
    ctx->m_context->SetSampler(2, slot, sampler);
}

// An outline batch (FUN_1411dc7a0): the matrices, the outline's alpha with flag 0x40 for small,
// faded or forced models, and the batch's first texture for its alpha.
// ref: FUN_1411dc7a0
void DrawOutline(M2SceneDrawContext* ctx, bool forced) {
    M2Element* element = ctx->m_element;
    int32_t fileDataId = ctx->m_model->GetFileDataId();
    ITEMCONTEXT context;
    ErrorContextPush(&context.m_base, DescribeItem);
    context.m_format = "M2 Outline: model=%d batch=%u";
    context.m_batch = element->m_batchIndex;
    context.m_fileDataId = fileDataId;
    MODELMEMORY memory;
    ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
    memory.m_model = ctx->m_model;
    M2SceneDebugLog(ctx->m_context, "Outline Batch: model=%s index=%d", ctx->m_model->GetName(),
        ctx->m_element->m_batchIndex);
    auto outlineBatch = static_cast<const uint8_t*>(ctx->m_element->m_batch);
    ctx->m_section = ctx->m_element->m_section;
    BindOutlineGeometry(ctx);
    ctx->m_context->SetPipeline(ctx->m_element->m_pipeline);
    if (ctx->m_kind != ctx->m_prevKind || ctx->m_model != ctx->m_prevModel
        || ctx->m_section->m_boneComboIndex != ctx->m_prevSection->m_boneComboIndex) {
        WriteSlotMatrices(ctx, 1, ctx->m_section->m_boneCount * 0x30 + 0x40, 0);
    }
    CM2BaseModel* root = ctx->m_model;
    while ((root->m_flags >> 0x12 & 1) != 0 && root->m_parent != nullptr) {
        root = root->m_parent;
    }
    uint32_t flags = root->m_33c;
    float box[6];
    root->GetBox(box);
    float size;
    if (box[3] < box[0] || box[4] < box[1] || box[5] < box[2]) {
        size = 0.0f;
    } else {
        float dx = box[3] - box[0];
        float dy = box[4] - box[1];
        float smaller = dx < dy ? dx : dy;
        size = box[5] - box[2];
        if (smaller < size) {
            size = dy <= dx ? dy : dx;
        }
    }
    float strength = ctx->m_model->GetEffectStrength();
    int32_t effectType = ctx->m_model->GetEffectType();
    if (forced || (ctx->m_element->m_flags & 0x80) != 0 || (0.0f < strength && effectType != 4) || size < 0.6f) {
        flags |= 0x40;
    }
    float* dest = ctx->m_context->LockConstants(2, 1, 0x10);
    dest[0] = static_cast<float>(flags) * (1.0f / 255.0f);
    dest[1] = 0.0f;
    dest[2] = 0.0f;
    dest[3] = 0.0f;
    ctx->m_context->UnlockConstants(2, 1);
    if (outlineBatch != nullptr && (outlineBatch[1] & 2) == 0) {
        uint32_t index;
        if ((ctx->m_element->m_flags & 0x4000) == 0) {
            M2Data* data = ctx->m_model->m_shared->m_data;
            index = data->m_textureCombos[static_cast<const M2Batch*>(ctx->m_element->m_batch)->m_textureComboIndex];
        } else {
            index = *reinterpret_cast<const uint16_t*>(outlineBatch + 6);
        }
        BindFlaggedTexture(ctx, 0, index, true);
    }
    const M2SkinSection* section = ctx->m_section;
    ctx->m_context->DrawIndexed(section->IndexCount(), 1, section->IndexStart(), 0, 0, 0);
    ErrorMemoryContextPop(&memory.m_base);
    ErrorContextPop(&context.m_base);
}

// An object fill batch (FUN_1411dc130): the model's local matrix with the fill's screen
// parameters, its highlight colour, the matrices and the batch's texture.
// ref: FUN_1411dc130
void DrawObjectFill(M2SceneDrawContext* ctx) {
    M2Element* element = ctx->m_element;
    int32_t fileDataId = ctx->m_model->GetFileDataId();
    ITEMCONTEXT context;
    ErrorContextPush(&context.m_base, DescribeItem);
    context.m_format = "M2 Object Fill: model=%d batch=%u";
    context.m_batch = element->m_batchIndex;
    context.m_fileDataId = fileDataId;
    MODELMEMORY memory;
    ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
    memory.m_model = ctx->m_model;
    ctx->m_section = ctx->m_element->m_section;
    auto fillBatch = static_cast<const uint8_t*>(ctx->m_element->m_batch);
    BindOutlineGeometry(ctx);
    ctx->m_context->SetPipeline(ctx->m_element->m_pipeline);
    float* local = ctx->m_context->LockConstants(0, 1, 0x40);
    const float* m = ctx->m_model->m_local.m;
    for (uint32_t c = 0; c < 4; c++) {
        for (uint32_t r = 0; r < 4; r++) {
            local[c * 4 + r] = m[r * 4 + c];
        }
    }
    float sphere[4];
    ctx->m_model->GetAnimatedSphere(sphere);
    const float* v = ctx->m_scene->m_drawView.m;
    const float position[4] = {
        sphere[1] * v[4] + sphere[0] * v[0] + sphere[2] * v[8] + v[12],
        sphere[1] * v[5] + sphere[0] * v[1] + sphere[2] * v[9] + v[13],
        sphere[1] * v[6] + sphere[0] * v[2] + sphere[2] * v[10] + v[14],
        sphere[1] * v[7] + sphere[0] * v[3] + sphere[2] * v[11] + v[15],
    };
    float rect[4];
    ShaderEffectEnvironmentRect(ctx->m_environment, rect);
    float params[4];
    ObjectFillParams(params, position, rect, ctx->m_scene->m_0b4);
    local[12] = params[0];
    local[13] = params[1];
    local[14] = params[2];
    local[15] = params[3];
    ctx->m_context->UnlockConstants(0, 1);
    CM2BaseModel* root = ctx->m_model;
    for (CM2BaseModel* parent = root->m_parent; parent != nullptr; parent = parent->m_parent) {
        root = parent;
    }
    const float* color = s_fillNoColor;
    if ((root->m_33c & 0x3f) != 0) {
        color = ObjectFillColor(static_cast<uint8_t>(root->m_33c & 0x3f));
    }
    float* dest = ctx->m_context->LockConstants(2, 1, 0x10);
    std::memcpy(dest, color, 0x10);
    ctx->m_context->UnlockConstants(2, 1);
    if (ctx->m_kind != ctx->m_prevKind || ctx->m_model != ctx->m_prevModel
        || ctx->m_section->m_boneComboIndex != ctx->m_prevSection->m_boneComboIndex) {
        WriteSlotMatrices(ctx, 2, (ctx->m_section->m_boneCount + 1) * 0x30, 0);
    }
    if ((fillBatch[1] & 2) == 0) {
        uint16_t index = *reinterpret_cast<const uint16_t*>(fillBatch + 6);
        if (ctx->m_model->m_textures[index] != nullptr) {
            BindFlaggedTexture(ctx, 1, index, false);
        }
    }
    const M2SkinSection* section = ctx->m_section;
    ctx->m_context->DrawIndexed(section->IndexCount(), 1, section->IndexStart(), 0, 0, 0);
    ErrorMemoryContextPop(&memory.m_base);
    ErrorContextPop(&context.m_base);
}

// A run of particle emitters merged into one transient vertex buffer (FUN_1411db0d0): the run
// keeps going while the emitters batch, share the model matrix, alpha, lighting and plane and
// fit the buffer; how many elements past the first it took.
// ref: FUN_1411db0d0
uint32_t DrawParticleRun(M2SceneDrawContext* ctx, uint32_t first, const M2Element* elements, const uint32_t* indices,
    uint64_t end) {
    const M2Element* lead = &elements[indices[first]];
    CParticleEmitter2* leadEmitter = lead->m_emitter;
    uint32_t stride = GxVertexFormatStride(ParticleEmitterVertexFormat(leadEmitter));
    float alpha = lead->m_alpha;
    CM2Model* leadModel = AsModel(lead->m_model);
    uint32_t leadFlags = lead->m_flags;
    const C44Matrix* leadMatrix = leadModel->m_558;
    CM2ModelLighting* leadLighting = leadModel->m_lightingUsed;
    const float* plane = (leadFlags & 2) == 0 ? ctx->m_scene->m_d568 : leadLighting->m_plane;
    auto emitterCounts = [](const CParticleEmitter2* emitter, uint32_t* vertices, uint32_t* indices) {
        uint32_t live = static_cast<uint32_t>(emitter->m_list.m_live.m_size);
        uint32_t perVertex = emitter->m_verticesPerParticle;
        uint32_t fit = static_cast<uint32_t>(0x4000 / static_cast<uint64_t>(perVertex));
        uint32_t count = live <= fit ? live : fit;
        *vertices = count * perVertex;
        *indices = count * emitter->m_indicesPerParticle;
    };
    uint32_t vertexTotal;
    uint32_t indexTotal;
    emitterCounts(leadEmitter, &vertexTotal, &indexTotal);
    uint32_t taken = 1;
    for (uint64_t i = first + 1; i < end; i++) {
        const M2Element* next = &elements[indices[i]];
        if (ElementKind(next) != 5) {
            break;
        }
        CParticleEmitter2* emitter = next->m_emitter;
        CM2Model* model = AsModel(next->m_model);
        uint32_t vertices;
        uint32_t nextIndices;
        emitterCounts(emitter, &vertices, &nextIndices);
        if (!ParticleEmitterBatchable(emitter, leadEmitter) || model->m_558 != leadMatrix || next->m_alpha != alpha
            || static_cast<uint32_t>(0x100000 / static_cast<uint64_t>(stride)) < vertices + vertexTotal
            || 0x40000 < indexTotal + nextIndices) {
            break;
        }
        CM2ModelLighting* lighting = model->m_lightingUsed;
        const float epsilon = 1.0f / 255.0f;
        if ((emitter->m_renderFlags & 1) != 0
            && (epsilon <= std::fabs(lighting->m_ambient[0] - leadLighting->m_ambient[0])
                || epsilon <= std::fabs(lighting->m_ambient[1] - leadLighting->m_ambient[1])
                || epsilon <= std::fabs(lighting->m_ambient[2] - leadLighting->m_ambient[2])
                || epsilon <= std::fabs(lighting->m_directional[0] - leadLighting->m_directional[0])
                || epsilon <= std::fabs(lighting->m_directional[1] - leadLighting->m_directional[1])
                || epsilon <= std::fabs(lighting->m_directional[2] - leadLighting->m_directional[2]))) {
            break;
        }
        uint32_t flags = next->m_flags;
        if ((flags & 0x8002) != (leadFlags & 0x8002)) {
            break;
        }
        const float* other = nullptr;
        if ((flags & 2) == 0) {
            if ((flags >> 0xf & 1) != 0) {
                other = ctx->m_scene->m_d568;
            }
        } else {
            other = lighting->m_plane;
        }
        if (other != nullptr
            && (plane[0] != other[0] || plane[1] != other[1] || plane[2] != other[2] || plane[3] != other[3])) {
            break;
        }
        vertexTotal += vertices;
        indexTotal += nextIndices;
        taken++;
    }
    if (vertexTotal == 0 || indexTotal == 0) {
        ctx->m_kind = -1;
        return 0;
    }
    elements[indices[first]].m_model->GetFileDataId();
    GxBuf* buf = GxBufTransient(ctx->m_context, 0, stride, vertexTotal);
    auto vertices = static_cast<uint8_t*>(ctx->m_context->BufLock(buf));
    uint32_t drawnVertices = 0;
    uint32_t drawnIndices = 0;
    CParticleEmitter2* emitter = nullptr;
    for (uint32_t k = 0; k < taken; k++) {
        emitter = elements[indices[first + k]].m_emitter;
        ParticleEmitterRender(emitter, ctx->m_context, &ctx->m_scene->m_drawView, ctx->m_model->m_558, vertices,
            &ctx->m_scene->m_particles);
        uint32_t written = emitter->m_drawnCount * emitter->m_verticesPerParticle;
        drawnIndices += emitter->m_drawnCount * emitter->m_indicesPerParticle;
        drawnVertices += written;
        vertices += static_cast<uint64_t>(written * stride);
    }
    ctx->m_context->BufUnlock(buf, 0);
    if (drawnVertices == 0 || drawnIndices == 0) {
        ctx->m_kind = -1;
        return taken - 1;
    }
    SetBatchLighting(ctx, ctx->m_pass, true);
    SetBatchFogColor(ctx);
    SetBatchConstants(ctx);
    ParticleEmitterSetTextureMatrix(emitter, &ctx->m_shader);
    if (ctx->m_scene->m_daf0 != 0) {
        ParticleEmitterBindOutline(emitter, ctx->m_context, reinterpret_cast<const uint8_t*>(ctx->m_scene->m_daf0));
    }
    ParticleEmitterSetConstants(emitter, ctx->m_context, &ctx->m_shader, reinterpret_cast<const float*>(ctx->m_environment));
    ctx->m_context->SetPipeline(ctx->m_element->m_pipeline);
    ctx->m_shader.FlushBatch(ctx->m_context, 400);
    ctx->m_shader.ResetFrame(ctx->m_context);
    uint8_t shadingRate = g_renderSettings.m_26;
    if (ctx->m_scene->m_db30 < 1000) {
        shadingRate = 0;
    }
    ParticleEmitterDrawBatch(emitter, ctx->m_context, buf, drawnVertices, drawnIndices, shadingRate);
    return taken - 1;
}

// A particle emitter (FUN_1411dce00): its own material from its blend and render flags, drawn
// alone, or merged with the emitters after it when the cache batches particles.
// ref: FUN_1411dce00
uint32_t DrawParticles(M2SceneDrawContext* ctx, uint32_t index, const M2Element* elements, const uint32_t* indices,
    uint64_t end) {
    int32_t fileDataId = ctx->m_model->GetFileDataId();
    MODELCONTEXT context;
    ErrorContextPush(&context.m_base, DescribeModel);
    context.m_format = "M2 Particle: model=%d";
    context.m_fileDataId = fileDataId;
    MODELMEMORY memory;
    ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
    memory.m_model = ctx->m_model;
    CParticleEmitter2* emitter = ctx->m_element->m_emitter;
    uint8_t renderFlags = emitter->m_renderFlags;
    uint16_t flags = static_cast<uint16_t>((~renderFlags & 1) + 4);
    if ((renderFlags & 2) == 0) {
        flags |= 2;
    }
    if ((renderFlags & 4) == 0) {
        flags |= 0x10;
    }
    if ((renderFlags & 8) != 0) {
        flags |= 0x1000;
    }
    M2Material material;
    material.m_flags = flags;
    material.m_blendMode = static_cast<uint16_t>(M2BlendModeIndex(emitter->m_blend));
    uint32_t consumed = 0;
    ctx->m_material = &material;
    ctx->m_prevMaterial = nullptr;
    ctx->m_shader.SetBatch698(0);
    if ((ctx->m_cache->m_flags & 0x40) == 0) {
        if (ParticleEmitterTexturesReady(emitter)) {
            ctx->m_model->GetFileDataId();
            SetBatchLighting(ctx, ctx->m_pass, true);
            SetBatchFogColor(ctx);
            SetBatchConstants(ctx);
            ParticleEmitterSetConstants(emitter, ctx->m_context, &ctx->m_shader,
                reinterpret_cast<const float*>(ctx->m_environment));
            ctx->m_shader.ResetFrame(ctx->m_context);
            ctx->m_shader.FlushBatch(ctx->m_context, 400);
            ctx->m_context->SetPipeline(ctx->m_element->m_pipeline);
            ParticleEmitterRender(emitter, ctx->m_context, &ctx->m_scene->m_drawView, ctx->m_model->m_558, nullptr,
                &ctx->m_scene->m_particles);
        }
    } else {
        consumed = DrawParticleRun(ctx, index, elements, indices, end);
    }
    ErrorMemoryContextPop(&memory.m_base);
    ErrorContextPop(&context.m_base);
    return consumed;
}

// The element before becomes the one compared against.
void AdvancePrevious(M2SceneDrawContext* ctx) {
    ctx->m_prevElement = ctx->m_element;
    ctx->m_prevKind = ctx->m_kind;
    ctx->m_prevModel = ctx->m_model;
    ctx->m_prevShared = ctx->m_shared;
    ctx->m_prevLod = ctx->m_lod;
    ctx->m_prevLighting = ctx->m_lighting;
    ctx->m_prevLit = ctx->m_lit;
    ctx->m_prevFogKind = ctx->m_fogKind;
    ctx->m_prevBatch = ctx->m_batch;
    ctx->m_prevSection = ctx->m_section;
    ctx->m_prevEdgeFade = ctx->m_edgeFade;
    ctx->m_prevDboc = ctx->m_dboc;
}

// A model element by its kind (FUN_1411db730); `index` moves past the elements a doodad run or
// a particle run took.
// ref: FUN_1411db730
void DrawModelElement(M2SceneDrawContext* ctx, M2Element* element, uint32_t* index, M2Element* elements,
    const uint32_t* indices, const uint32_t* runs, uint64_t end) {
    ctx->m_element = element;
    ctx->m_kind = static_cast<int32_t>(ElementKind(element));
    auto model = static_cast<CM2Model*>(element->m_model);
    ctx->m_model = model;
    ctx->m_shared = model->m_shared;
    ctx->m_lighting = model->m_lightingUsed;
    ctx->m_data = model->m_shared->m_data;
    ctx->m_lod = model->m_lod;
    ctx->m_batch = nullptr;
    ctx->m_section = nullptr;
    ctx->m_material = &ctx->m_defaultMaterial;
    ctx->m_lit = 1;
    ctx->m_fogKind = 1;
    ctx->m_combiners = 0;
    switch (ctx->m_kind) {
    case 0:
        DrawProjectedBatch(ctx);
        break;
    case 1:
        return;
    case 2:
        if ((element->m_flags & 0x10) == 0) {
            DrawBatch(ctx);
        } else {
            DrawPrepassBatch(ctx);
        }
        break;
    case 3:
        if (runs == nullptr) {
            GxLog("M2ELEMENT_BATCH_DOODAD: a:%u | b:%p | c:%zu | d:%u | e:%d", *index, nullptr, end, ctx->m_kind,
                model->GetFileDataId());
        } else {
            auto batch = static_cast<const M2Batch*>(element->m_batch);
            uint32_t count = runs[*index];
            const uint32_t* run = &indices[*index];
            int32_t fileDataId = model->GetFileDataId();
            BATCHCONTEXT context;
            ErrorContextPush(&context.m_base, DescribeBatch);
            context.m_format = "M2 Doodad Batch: model=%d batch=%u skinSection=%u material=%u";
            context.m_material = batch->m_materialIndex;
            context.m_section = batch->m_skinSectionIndex;
            context.m_batch = element->m_batchIndex;
            context.m_fileDataId = fileDataId;
            MODELMEMORY memory;
            ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
            memory.m_model = ctx->m_model;
            M2SceneDebugLog(ctx->m_context, "BatchDoodad: model=%s index=%d batchedCount=%d order=%d",
                ctx->m_model->GetName(), ctx->m_element->m_batchIndex, count, *run);
            if ((ctx->m_cache->m_flags & 0x100) == 0) {
                DrawDoodadBatch(ctx, elements, count, run);
            } else {
                DrawDoodadBatchInstanced(ctx, elements, count, run);
            }
            ErrorMemoryContextPop(&memory.m_base);
            ErrorContextPop(&context.m_base);
            *index = *index - 1 + runs[*index];
        }
        break;
    case 4: {
        int32_t fileDataId = model->GetFileDataId();
        ITEMCONTEXT context;
        ErrorContextPush(&context.m_base, DescribeItem);
        context.m_format = "M2 Ribbon: model=%d batch=%u";
        context.m_batch = element->m_batchIndex;
        context.m_fileDataId = fileDataId;
        MODELMEMORY memory;
        ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
        memory.m_model = ctx->m_model;
        ctx->m_model->GetFileDataId();
        CM2Scene* scene = ctx->m_scene;
        RIBBONDRAWPARAMS params;
        params.m_context = ctx->m_context;
        params.m_matrix = ctx->m_model->m_558;
        params.m_camera = &scene->m_drawCameraPosition;
        RibbonEmitterDraw(ctx->m_model->m_ribbons[ctx->m_element->m_batchIndex], &params, &scene->m_drawView,
            &scene->m_drawProjection);
        M2SetProjectionConstants(ctx->m_context, scene->m_drawProjection.m);
        ErrorMemoryContextPop(&memory.m_base);
        ErrorContextPop(&context.m_base);
        break;
    }
    case 5:
        *index += DrawParticles(ctx, *index, elements, indices, end);
        break;
    case 6:
        break;
    case 7: {
        CM2Scene* scene = ctx->m_scene;
        auto custom = static_cast<M2ModelCustomDraw*>(model->m_590);
        OsMutexLock(&scene->m_locks100[4]);
        BindBatchTextures(ctx);
        SetBatchLighting(ctx, ctx->m_pass, false);
        SetBatchFogColor(ctx);
        SetBatchConstants(ctx);
        ctx->m_shader.FlushBatch(ctx->m_context, 400);
        uint8_t saved = g_gx8b641;
        g_gx8b641 = 0;
        custom->Draw(ctx->m_context, ctx->m_model, ctx->m_model->m_lightingUsed, &scene->m_drawView,
            &scene->m_drawProjection);
        g_gx8b641 = saved;
        M2SetProjectionConstants(ctx->m_context, scene->m_drawProjection.m);
        OsMutexUnlock(&scene->m_locks100[4]);
        break;
    }
    case 8:
        DrawOutline(ctx, false);
        break;
    case 9:
        DrawOutline(ctx, true);
        break;
    case 10:
        DrawObjectFill(ctx);
        break;
    default:
        return;
    }
    AdvancePrevious(ctx);
}

// The pass's elements from `start`, `count` of them, through a pass's view of the context
// (FUN_1411d9650): the frame's constants first, then each shown element by its model's type.
// `runs` holds the instance run lengths when the indices are the instanced list.
// ref: FUN_1411d9650
void DrawElements(M2SceneDrawContext* ctx, uint8_t pass, M2Element* elements, const uint32_t* indices,
    const uint32_t* runs, uint32_t start, uint64_t count) {
    if (count == 0) {
        return;
    }
    ctx->m_prevElement = nullptr;
    ctx->m_element = nullptr;
    ctx->m_prevModel = nullptr;
    ctx->m_model = nullptr;
    ctx->m_prevShared = nullptr;
    ctx->m_shared = nullptr;
    ctx->m_data = nullptr;
    ctx->m_prevLighting = nullptr;
    ctx->m_lighting = nullptr;
    ctx->m_lit = 0;
    ctx->m_prevLit = 0;
    ctx->m_prevBatch = nullptr;
    ctx->m_batch = nullptr;
    ctx->m_prevSection = nullptr;
    ctx->m_section = nullptr;
    ctx->m_prevMaterial = nullptr;
    ctx->m_material = nullptr;
    ctx->m_prevEdgeFade = nullptr;
    ctx->m_edgeFade = nullptr;
    ctx->m_prevDboc = nullptr;
    ctx->m_dboc = nullptr;
    ctx->m_pass = 0xff;
    ctx->m_kind = -1;
    ctx->m_prevKind = -1;
    ctx->m_lod = -1;
    ctx->m_prevLod = -1;
    ctx->m_fogKind = -1;
    ctx->m_prevFogKind = -1;
    CM2Scene* scene = ctx->m_scene;
    ctx->m_shader.SetViewPlane(ctx->m_context, scene->m_drawView.m, false);
    ctx->m_shader.SetFrame7d4(scene->m_d568);
    uint8_t fog[0xa0];
    DefaultPassFog(fog);
    float constants[0x38];
    ShaderEffectFogConstants(constants, fog, 0.0f, &scene->m_drawCameraPosition.x, scene->m_drawView.m);
    float* dest = ctx->m_context->LockConstants(0, 3, 0xe0);
    std::memcpy(dest, constants, 0xe0);
    ctx->m_context->UnlockConstants(0, 3);
    float vectors[8] = {};
    if (((pass - 8) & 0xf9) != 0 || pass == 0xe) {
        std::memcpy(vectors, scene->m_d654, 0x10);
        std::memcpy(vectors + 4, scene->m_d644, 0x10);
    }
    dest = ctx->m_context->LockConstants(2, 7, 0x20);
    std::memcpy(dest, vectors, 0x20);
    ctx->m_context->UnlockConstants(2, 7);
    int32_t shadows = 1;
    if (static_cast<uint8_t>(pass - 5) < 3 && (scene->m_flags & 0x20) != 0) {
        shadows = 0;
    }
    if (s_passShadows[pass] != 0) {
        if ((scene->m_flags & 0x10) == 0) {
            ShaderEffectBindShadows(ctx->m_context, shadows);
        } else {
            ShaderEffectBindSceneShadows(ctx->m_context, shadows, 0, reinterpret_cast<const uint8_t*>(scene->m_db00));
        }
    }
    if ((scene->m_flags & 0x40) != 0) {
        ctx->m_shader.UploadLights(ctx->m_context);
    }
    if (shadows == 0) {
        ShaderEffectBindShadowMap(ctx->m_context, reinterpret_cast<void*>(scene->m_daf8));
    }
    const float w[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ctx->m_shader.SetFrame7e4(w);
    ctx->m_shader.SetSampling(&g_renderSettings, 0.0f, 0);
    ctx->m_pass = pass;
    uint32_t i = start;
    uint64_t last = start + count;
    while (i < last) {
        M2Element* element = &elements[indices[i]];
        if (runs != nullptr || (element->m_model->m_flags >> 0xb & 1) == 0 || ElementKind(element) - 5 < 2) {
            uint16_t type = element->m_model->m_type;
            if (type == 0) {
                DrawModelElement(ctx, element, &i, elements, indices, runs, last);
            } else if (type == 2 || type == 3) {
                DrawOtherModelElement(ctx, element);
            }
        }
        i++;
    }
}

}  // namespace

// ref: FUN_1411d9460
void CM2Scene::DrawPass(GxRenderContext* context, ShaderEffectContext* shader, uint8_t pass) {
    if ((g_m2DrawPassMask >> (pass & 0x1f) & 1) == 0) {
        return;
    }
    PASSCONTEXT errorContext;
    ErrorContextPush(&errorContext.m_base, DescribePass);
    M2SceneDrawContext ctx;
    InitializeDrawContext(&ctx, this, context, shader);
    errorContext.m_format = "M2Scene Draw Pass %d";
    errorContext.m_pass = pass;
    shader->UploadLights(context);
    if (this->m_threadedBuild != 0) {
        ThreadPoolGroupWait(static_cast<THREADPOOLGROUP*>(this->m_threadGroupA));
    }
    DrawElements(&ctx, pass, this->m_f78.m_data, this->m_f90[pass].m_data, nullptr, 0, this->m_f90[pass].m_size);
    DrawElements(&ctx, pass, this->m_f78.m_data, this->m_10e0[pass].m_data, this->m_1230[pass].m_data, 0,
        this->m_10e0[pass].m_size);
    ErrorContextPop(&errorContext.m_base);
}

// ref: FUN_1411daef0
void CM2Scene::DrawPassInstancedPart(GxRenderContext* context, ShaderEffectContext* shader, uint8_t pass, uint8_t parts,
    uint8_t part) {
    if ((g_m2DrawPassMask >> (pass & 0x1f) & 1) == 0) {
        return;
    }
    PASSCONTEXT errorContext;
    ErrorContextPush(&errorContext.m_base, DescribePass);
    M2SceneDrawContext ctx;
    InitializeDrawContext(&ctx, this, context, shader);
    errorContext.m_format = "M2Scene Draw Pass %d";
    errorContext.m_pass = pass;
    if (this->m_threadedBuild != 0) {
        ThreadPoolGroupWait(static_cast<THREADPOOLGROUP*>(this->m_threadGroupA));
    }
    uint32_t total = static_cast<uint32_t>(this->m_10e0[pass].m_size);
    uint32_t each = total / parts;
    uint32_t rest = total % parts;
    uint32_t extra = 0;
    if (part != 0) {
        extra = part;
        if (rest <= static_cast<uint32_t>(part - 1)) {
            extra = rest;
        }
    }
    DrawElements(&ctx, pass, this->m_f78.m_data, this->m_10e0[pass].m_data, this->m_1230[pass].m_data,
        part * each + extra, each + (part < rest ? 1 : 0));
    ErrorContextPop(&errorContext.m_base);
}

// ref: FUN_1411dbf60
void CM2Scene::DrawPassPart(GxRenderContext* context, ShaderEffectContext* shader, uint8_t pass, uint8_t parts,
    uint8_t part) {
    if ((g_m2DrawPassMask >> (pass & 0x1f) & 1) == 0) {
        return;
    }
    PASSCONTEXT errorContext;
    ErrorContextPush(&errorContext.m_base, DescribePass);
    M2SceneDrawContext ctx;
    InitializeDrawContext(&ctx, this, context, shader);
    errorContext.m_format = "M2Scene Draw Pass %d";
    errorContext.m_pass = pass;
    if (this->m_threadedBuild != 0) {
        ThreadPoolGroupWait(static_cast<THREADPOOLGROUP*>(this->m_threadGroupA));
    }
    uint32_t total = static_cast<uint32_t>(this->m_f90[pass].m_size);
    uint32_t each = total / parts;
    uint32_t rest = total % parts;
    uint32_t extra = 0;
    if (part != 0) {
        extra = part;
        if (rest <= static_cast<uint32_t>(part - 1)) {
            extra = rest;
        }
    }
    DrawElements(&ctx, pass, this->m_f78.m_data, this->m_f90[pass].m_data, nullptr, part * each + extra,
        each + (part < rest ? 1 : 0));
    ErrorContextPop(&errorContext.m_base);
}

// The view set for the frame becomes the one the passes draw with.
// ref: FUN_1411df600
void CM2Scene::LatchView() {
    this->m_drawView = this->m_camera;
    this->m_drawProjection = this->m_d4f8;
    this->m_drawInverseView = this->m_inverseView;
    this->m_drawCameraPosition = this->m_cameraPosition;
}

namespace {

// The merged geometry's index upload (LAB_1411ace40 over FUN_1411ad6b0): the indices of each
// GPU batch range's sections that the LOD's bone mask keeps.
// ref: FUN_1411ad6b0
void WriteGpuBatchIndices(void* param, void* data) {
    CM2Model* model = *static_cast<CM2Model**>(param);
    M2SkinProfile* profile = model->m_shared->m_skins[model->m_lod].m_profile;
    auto out = static_cast<uint16_t*>(data);
    auto gpu = static_cast<const uint8_t*>(model->m_600);
    uint16_t ranges = *reinterpret_cast<const uint16_t*>(gpu + 4);
    for (uint32_t r = 0; r < ranges; r++) {
        gpu = static_cast<const uint8_t*>(model->m_600);
        auto range = *reinterpret_cast<const uint32_t* const*>(gpu + 0x28) + r * 2;
        for (uint32_t b = range[0]; b <= range[1]; b++) {
            uint16_t sectionIndex = profile->m_batches[b].m_skinSectionIndex;
            const uint8_t* mask = model->m_lodBoneMasks[model->m_lod];
            if (((1u << (sectionIndex & 7)) & mask[sectionIndex >> 3]) == 0) {
                continue;
            }
            const M2SkinSection* section = &profile->m_skinSections[sectionIndex];
            const uint16_t* indices = profile->m_indices.m_count != 0 ? profile->m_indices.Data() : nullptr;
            uint32_t count = section->IndexCount();
            std::memcpy(out, indices + section->IndexStart(), static_cast<size_t>(count) * 2);
            out += count;
        }
    }
}

}  // namespace

// ref: FUN_1411cb4c0
bool M2ModelBindGpuBatchIndices(CM2Model* model, GxRenderContext* context) {
    auto gpu = static_cast<uint8_t*>(model->m_600);
    auto& buf = *reinterpret_cast<GxBuf**>(gpu + 0x30);
    if (buf == nullptr || (GxDevExists() && (buf->m_flags & 0x3000000) != 0x3000000)) {
        GxDeviceCallLock lock;
        if (buf == nullptr) {
            model->GetName();
            GXBUFINFO info = {};
            info.m_size = static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(gpu + 8)) * 2;
            info.m_bind = 1;
            info.m_format = 1;
            GxBuf* created = GxBufCreateFromInfo(&info);
            GxBuf* old = buf;
            buf = created;
            if (old != nullptr) {
                GxBufDestroy(&old);
            }
            if (GxDeviceCaps()[0x2f] != 0) {
                FROST_UNIMPLEMENTED();  // FUN_142288bb0: the ray-tracing view of the indices
            }
        }
        if (GxDevExists() && (buf->m_flags & 0x3000000) != 0x3000000) {
            CM2Model* param = model;
            g_gxUploadInterface->BufUpdate(buf, 0, &WriteGpuBatchIndices, &param);
        }
    }
    context->SetIndexBuffer(buf, 1);
    return true;
}

// ref: FUN_1411bd8f0
float M2ModelAfraScale(CM2Model* model) {
    if (model->m_shared != nullptr && model->m_shared->m_afra != nullptr) {
        return *static_cast<const float*>(model->m_shared->m_afra);
    }
    return 1.0f;
}

// The model's flattest axis for its effect: the effect's own when it says so, else the
// smallest extent of its first sequence's bounds (FUN_1411be430).
// ref: FUN_1411be430
uint32_t M2ModelEffectAxis(CM2Model* model) {
    if (model->m_effect2 != nullptr) {
        return EffectField<uint32_t>(model->m_effect2, 0x7c) & 7;
    }
    if (model->m_parent != nullptr) {
        void* effect = M2BaseModelGetEffect(model);
        if (effect != nullptr && (EffectField<uint32_t>(effect, 0x7c) >> 8 & 1) != 0) {
            return EffectField<uint32_t>(effect, 0x7c) & 7;
        }
    }
    uint32_t axis = 0;
    if ((model->m_flags & 1) != 0) {
        M2Array<M2Sequence>* sequences = model->m_animState->m_sequences;
        uint16_t index = M2FindSequenceById(0, model->m_animState->m_sequenceHash, sequences);
        if (index != 0xffff) {
            const CAaBox* box = &(*sequences)[index].m_bounds.m_extent;
            float dy = box->m_max.y - box->m_min.y;
            float dx = box->m_max.x - box->m_min.x;
            float smaller = dx <= dy ? dx : dy;
            axis = dy < dx ? 1 : 0;
            if (box->m_max.z - box->m_min.z < smaller) {
                axis = 2;
            }
        }
    }
    return axis;
}

// The inverse of the local matrix of the model that has the effect: this one or its parent
// (FUN_1411be3b0).
// ref: FUN_1411be3b0
float* M2ModelEffectInverse(CM2Model* model, float* out) {
    CM2BaseModel* source = model;
    if (model->m_effect2 == nullptr) {
        source = model->m_parent;
    }
    std::memcpy(out, source->m_local.m, 0x40);
    MatrixAffineInverse(out, std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]));
    return out;
}

// The bottom and height of the model's playing sequence's bounds, blended toward the sequence
// it blends with (FUN_1411ba260); 0 and 1 for a model not loaded or flagged 0x2000.
// ref: FUN_1411ba260
void M2ModelSequenceHeight(CM2Model* model, float* bottom, float* height) {
    if ((model->m_flags & 1) == 0 || (model->m_modelFlags & 0x2000) != 0) {
        *bottom = 0.0f;
        *height = 1.0f;
        return;
    }
    M2BoneState* state = model->m_animState->m_boneStates;
    auto bounds = [model](const M2SequenceTime* sequence) {
        // The sequence at +8 of the playing record, from the source's sequences or, for a
        // parent's, from the file's parent sequence bounds.
        auto bytes = reinterpret_cast<const uint8_t*>(sequence);
        uint16_t index = *reinterpret_cast<const uint16_t*>(bytes + 8);
        auto sequences = *reinterpret_cast<M2Array<M2Sequence>* const*>(bytes + 0x38);
        auto box = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(&(*sequences)[index]) + 0x20);
        auto parentBounds = static_cast<const M2Array<uint8_t>*>(model->m_shared->m_parentSequenceBounds);
        if (parentBounds != nullptr && (bytes[0x58] & 1) != 0) {
            box = reinterpret_cast<const float*>(parentBounds->Data() + static_cast<size_t>(index) * 0x1c);
        }
        return box;
    };
    const float* box = bounds(state->m_sequence);
    *height = box[5] - box[2];
    *bottom = box[2];
    if (state->m_blendSequence != nullptr) {
        const float* blend = bounds(state->m_blendSequence);
        float weight = state->m_blendWeight;
        float blendBottom = blend[2];
        *height = (blend[5] - blendBottom) * weight + (1.0f - weight) * *height;
        *bottom = (1.0f - weight) * *bottom + blendBottom * weight;
    }
}
