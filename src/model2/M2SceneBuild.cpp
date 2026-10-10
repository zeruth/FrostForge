#include <cmath>
#include <cstring>

#include "client/RenderSettings.hpp"
#include "model2/CM2Lighting.hpp"
#include "model2/M2Element.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shaders.hpp"
#include "model2/M2Shared.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "shadereffect/ShaderEffectPipeline.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Scene.cpp: the draw elements a frame's lists are built from -- each
// element's depth, blend and stencil states and the pipeline it draws with.

namespace {

const char s_elementName[] = ".?AVM2Element@@";  // DAT_142da2180
const char s_indexName[] = ".?I";                // DAT_142d61348

// The blend a pass draws a blend mode with (DAT_142721290), and the blend words (DAT_142cfb1a8).
const int32_t s_passBlend[14][8] = {
    { 0, 0, 2, 3, 4, 5, 6, 7 },
    { 0, 1, 2, 3, 4, 5, 6, 7 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 2, 3, 4, 5, 6, 7 },
    { 0, 0, 2, 3, 4, 5, 6, 7 },
    { 0, 0, 2, 3, 4, 5, 6, 7 },
    { 2, 2, 2, 3, 4, 5, 6, 7 },
    { 2, 2, 2, 3, 4, 5, 6, 7 },
    { 2, 2, 2, 3, 4, 5, 6, 7 },
    { 2, 2, 2, 3, 4, 5, 6, 7 },
    { 2, 2, 2, 3, 4, 5, 6, 7 },
    { 2, 2, 2, 3, 4, 5, 6, 7 },
};
const uint16_t s_blendWords[9] = { 0xf000, 0xf001, 0xf002, 0xf00a, 0xf003, 0xf004, 0xf005, 0xf00d, 0x0 };

uint32_t RotateRight(uint32_t value, uint32_t count) {
    return value >> count | value << (32 - count);
}

uint32_t HashTriple(const uint8_t* base, uint32_t first, uint32_t second, uint32_t third) {
    uint32_t a = *reinterpret_cast<const uint32_t*>(base + first);
    uint32_t b = *reinterpret_cast<const uint32_t*>(base + second);
    uint32_t c = *reinterpret_cast<const uint32_t*>(base + third);
    return RotateRight(b, 0x13) ^ RotateRight(c, 9) ^ RotateRight(a, 0x19);
}

}  // namespace

// ref: FUN_1411e55a0
void M2ElementsReserve(M2SceneVector<M2Element>* elements, uint64_t count) {
    uint64_t field = elements->m_capacity;
    if ((field & 0x7fffffffffffffffull) >= count) {
        return;
    }
    uint64_t capacity = (field >> 1 & 0x3fffffffffffffffull) + (field & 0x7fffffffffffffffull);
    if (capacity < count) {
        capacity = count;
    }
    elements->m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
    auto data = static_cast<M2Element*>(SMemAlloc(capacity * sizeof(M2Element), 0xb, s_elementName,
        SStrHashFnv1a(s_elementName), -6, 0));
    if (elements->m_size != 0) {
        std::memcpy(data, elements->m_data, elements->m_size * sizeof(M2Element));  // FUN_1411cf470
    }
    if (static_cast<int64_t>(elements->m_capacity) >= 0) {
        SMemFree(elements->m_data, s_elementName, -6, 0);
    }
    elements->m_capacity &= 0x7fffffffffffffffull;
    elements->m_data = data;
}

// ref: FUN_140323120
void M2IndicesReserve(M2SceneVector<uint32_t>* indices, uint64_t count) {
    uint64_t field = indices->m_capacity;
    if ((field & 0x7fffffffffffffffull) >= count) {
        return;
    }
    uint64_t capacity = (field >> 1 & 0x3fffffffffffffffull) + (field & 0x7fffffffffffffffull);
    if (capacity < count) {
        capacity = count;
    }
    indices->m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
    auto data = static_cast<uint32_t*>(SMemAlloc(capacity * 4, 0xb, s_indexName, SStrHashFnv1a(s_indexName), -6, 0));
    for (uint64_t i = 0; i < indices->m_size; i++) {
        data[i] = indices->m_data[i];
    }
    if (static_cast<int64_t>(indices->m_capacity) >= 0) {
        SMemFree(indices->m_data, s_indexName, -6, 0);
    }
    indices->m_capacity &= 0x7fffffffffffffffull;
    indices->m_data = data;
}

void M2IndicesPush(M2SceneVector<uint32_t>* indices, uint32_t value) {
    uint64_t capacity = indices->m_capacity & 0x7fffffffffffffffull;
    if (indices->m_size == capacity) {
        M2IndicesReserve(indices, capacity + 1);
    }
    indices->m_data[indices->m_size] = value;
    indices->m_size++;
}

// A new element at the list's end with its keys cleared, and its number (inlined throughout).
M2Element* M2SceneDrawListAddElement(M2SceneDrawList* list, uint32_t* index) {
    uint64_t capacity = list->m_elements.m_capacity & 0x7fffffffffffffffull;
    if (list->m_elements.m_size == capacity) {
        M2ElementsReserve(&list->m_elements, capacity + 1);
    }
    M2Element* element = &list->m_elements.m_data[list->m_elements.m_size];
    element->m_18 = 0;
    element->m_flags = 0;
    element->m_pipeline = nullptr;
    element->m_effect = nullptr;
    element->m_68 = 0;
    element->m_70 = 0;
    element->m_74 = 0;
    list->m_elements.m_size++;
    *index = static_cast<uint32_t>(list->m_elementCount);
    list->m_elementCount++;
    return element;
}

// ref: FUN_1411ddc40
void M2SceneDrawListAddToPass(CM2Scene* scene, M2SceneDrawList* list, uint32_t pass, uint32_t element) {
    (void)scene;
    M2IndicesPush(&list->m_lists[pass], element);
}

// ref: FUN_1411bc6e0
int32_t CM2BaseModel::GetEffectType() {
    if (this->m_params.m_effect != nullptr) {
        return *static_cast<const int32_t*>(this->m_params.m_effect);
    }
    return 0;
}

// ref: FUN_1411bc6f0
float CM2BaseModel::GetEffectStrength() {
    if (this->m_params.m_effect != nullptr) {
        return static_cast<const float*>(this->m_params.m_effect)[1];
    }
    return 0.0f;
}

// ref: FUN_1411c65c0
bool M2EffectTypeOpaque(int32_t type) {
    return static_cast<uint32_t>(type - 3) <= 3 || type == 8;
}

// Whether a batch draws through the instanced path (FUN_1411c6170).
// ref: FUN_1411c6170
bool CM2Model::BatchInstanced(const uint8_t* batch) {
    if ((g_m2Cache.m_flags & 0x10) != 0 && (this->m_modelFlags & 2) != 0 && (this->m_flags & BASE_SHADOWED) == 0
        && (*batch & 0x10) != 0 && (this->m_lightingUsed == nullptr || this->m_lightingUsed->m_pointCount == 0)
        && 0.99998999f < this->m_params.m_fade) {  // DAT_1427211fc
        return true;
    }
    return false;
}

// A model's bone matrices (FUN_1411ba500) and bone count when loaded (FUN_1411ba430).
// ref: FUN_1411c02e0
uint32_t CM2Model::GetShadowBatchCount() {
    if (this->m_600 != nullptr) {
        return static_cast<const uint16_t*>(this->m_600)[1];
    }
    return this->m_shared->m_skins[this->m_lod].m_profile->m_shadowBatches.Count();
}

// ref: FUN_1411ba500
C44Matrix* CM2Model::GetBoneMatrices() {
    return this->m_animState->m_boneMatrices;
}

// ref: FUN_1411ba430
uint16_t CM2Model::GetBoneCount() {
    if ((this->m_flags & BASE_LOADED) != 0) {
        return static_cast<uint16_t>(this->m_animState->m_bones->Count());
    }
    return 0;
}

// Whether a material blends with lighting (FUN_1411d7c80): lit and of a lit blend mode.
// ref: FUN_1411d7c80
bool M2MaterialLit(const uint16_t* material) {
    return (*material & 1) == 0 && g_m2ProjectedBlendLit[material[1]] != 0;
}

// The element's depth-stencil state, blend and stencil reference for a pass (FUN_1411e2560).
// ref: FUN_1411e2560
void M2SceneElementState(CM2BaseModel* model, const uint8_t* settings, bool outlined, uint16_t blendMode,
    uint16_t materialFlags, uint8_t msaa, uint32_t textureKind, uint8_t pass, uint32_t flags, int32_t effectKind,
    uint32_t kind, uint64_t* depthStencil, uint16_t* blend, uint32_t* raster) {
    *depthStencil = 0;
    *raster = 0;
    *blend = 0;
    uint32_t row = 0;
    if (pass == 0) {
        row = 5;
    } else if ((materialFlags & 0x10) == 0 || (flags & 0x200) != 0) {
        if ((materialFlags >> 0xe & 1) != 0 || ((flags & 1) != 0 && model->m_0d != 0)) {
            row = 5;
        } else if ((flags & 0x20) != 0 && (msaa == 0 || (flags & 0x200) != 0)) {
            row = 3;
        }
    } else {
        row = 1;
    }
    uint64_t state = g_m2DepthStencil[row][model->m_0d * 2 + (msaa != 0 ? 1 : 0)];
    *depthStencil = state;
    auto bytes = reinterpret_cast<uint8_t*>(depthStencil);
    if (model->m_type == 0) {
        if (pass == 8) {
            bytes[3] = 0;
            bytes[2] |= 0x10;
            bytes[0] = static_cast<uint8_t>(state) | 2;
            bytes[7] = 2;
        } else if ((static_cast<CM2Model*>(model)->m_shared->m_data->m_flags & 0x8000000) != 0) {
            bytes[3] = static_cast<uint8_t>(state >> 0x18) | 0x10;
            bytes[2] = static_cast<uint8_t>(state >> 0x10) | 0x10;
        }
    }
    uint16_t word = s_blendWords[8];
    if ((flags & 1) == 0) {
        word = s_blendWords[s_passBlend[pass][blendMode]];
    }
    *blend = word;
    if (effectKind != 0 && model->m_params.m_effect != nullptr) {
        bool assign = true;
        if (effectKind == 1 || effectKind == 2) {
            *depthStencil = g_m2DepthStencil[(materialFlags & 0x10) != 0 ? 1 : 2][0];
            word = s_blendWords[4];
        } else if (effectKind == 5) {
            if (pass == 0) {
                *depthStencil = g_m2DepthStencil[5][model->m_0d * 2];
                word = s_blendWords[0];
            } else if ((pass & 0xf8) == 0 && pass != 5) {
                assign = false;
            } else {
                uint32_t index = model->m_0d;
                if ((flags & 1) == 0) {
                    if (textureKind == 4) {
                        *depthStencil = g_m2DepthStencil[1][index * 2];
                        word = s_blendWords[4];
                    } else {
                        *depthStencil = g_m2DepthStencil[3][index * 2];
                        word = s_blendWords[7];
                    }
                } else {
                    *depthStencil = g_m2DepthStencil[0][index * 2];
                    word = s_blendWords[8];
                }
            }
        } else if (effectKind == 6 && (flags & 1) == 0) {
            *depthStencil = g_m2DepthStencil[(materialFlags & 0x10) != 0 ? 1 : 2][0];
            word = s_blendWords[7];
        } else {
            assign = false;
        }
        if (assign) {
            *blend = word;
        }
    }
    if ((flags >> 4 & 1) != 0) {
        *blend = s_blendWords[8];
    }
    uint8_t reference = 0;
    if ((materialFlags & 4) == 0 && kind != 0) {
        reference = static_cast<uint8_t>(outlined + 1);
        if ((model->m_flags >> 0x1a & 1) != 0) {
            reference = static_cast<uint8_t>((reference != 2) + 1);
        }
    }
    const uint32_t* table = settings[0x5c0] == 1 ? g_m2OutlineStencilFill : g_m2OutlineStencil;
    *raster = table[reference];
}

// The element's lighting key: the lighting's colours and directions, the batch's colour and
// the model's colour (FUN_1411ddae0).
// ref: FUN_1411ddae0
void M2SceneElementLightKey(CM2Scene* scene, M2Element* element) {
    (void)scene;
    CM2Model* model = nullptr;
    if (element->m_model->m_type == 0) {
        model = static_cast<CM2Model*>(element->m_model);
    }
    auto lighting = reinterpret_cast<const uint8_t*>(model->m_lightingUsed);
    uint32_t direction = HashTriple(lighting, 0x94, 0x98, 0x9c);
    uint32_t ambient = HashTriple(lighting, 0x58, 0x5c, 0x60);
    uint32_t directional = HashTriple(lighting, 0x7c, 0x80, 0x84);
    uint32_t key = RotateRight(direction, 0xd) ^ RotateRight(ambient, 0xf) ^ RotateRight(directional, 0x1b)
        ^ RotateRight(*reinterpret_cast<const uint32_t*>(lighting + 0x104), 0xd) ^ element->m_batchIndex
        ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(model->m_shared));
    uint16_t color = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(element->m_batch) + 8);
    if (color < model->m_shared->m_data->m_colors.Count()) {
        auto state = reinterpret_cast<const uint8_t*>(model->m_colors) + color * 0x20;
        uint32_t value = HashTriple(state, 8, 0xc, 0x10);
        key ^= RotateRight(value, 0x1d);
    }
    auto base = reinterpret_cast<const uint8_t*>(model);
    uint32_t add = HashTriple(base, 0x374, 0x378, 0x37c);
    uint32_t mul = HashTriple(base, 0x368, 0x36c, 0x370);
    key = RotateRight(add, 0xd) ^ RotateRight(mul, 0xf) ^ key;
    key = (key >> 0x10 ^ key) * 0x85ebca6b;
    key = (key >> 0xd ^ key) * 0xc2b2ae35;
    element->m_18 = key >> 0x10 ^ key;
}

namespace {

// The material a batch draws with (data +0x70, by the batch's +10).
const M2Material* BatchMaterial(CM2Model* model, const M2Element* element) {
    auto batch = static_cast<const M2Batch*>(element->m_batch);
    return &model->m_shared->m_data->m_materials[batch->m_materialIndex];
}

uint32_t ElementKind(const M2Element* element) {
    return static_cast<uint32_t>(element->m_sortKey >> 0x1f) & 0xf;
}

uint32_t ElementTextureKind(const M2Element* element) {
    return static_cast<uint32_t>(element->m_batchKey >> 0x20) & 0xf;
}

}  // namespace

// A model batch's pipeline: its permutations from the material, the pass and the lighting, and
// its states for the pass (FUN_1411d7840). `second` builds the second pass's form.
// ref: FUN_1411d7840
void M2SceneElementPipeline(CM2Scene* scene, M2Element* element, uint32_t second, int32_t faded, CShaderEffect* effect,
    uint8_t pass, uint32_t* vertexPermute, uint32_t* pixelPermute) {
    CM2Model* model = nullptr;
    if (element->m_model->m_type == 0) {
        model = static_cast<CM2Model*>(element->m_model);
    }
    CM2ModelLighting* lighting = model->m_lightingUsed;
    const M2Material* material = BatchMaterial(model, element);
    element->SetPipeline(nullptr);
    element->m_effect = nullptr;
    uint16_t materialFlags = material->m_flags;
    uint8_t lit = (materialFlags & 1) == 0 && g_m2ProjectedBlendLit[material->m_blendMode] != 0 ? 1 : 0;
    uint32_t bones = element->m_section->m_boneInfluences;
    uint32_t skeletal = model->m_shared->m_data->m_flags >> 0x12 & 1;
    uint32_t external = 0;
    uint32_t shadowed = 0;
    if (ElementKind(element) == 3) {
        if (bones == 0) {
            bones = 1;
        }
        external = scene->m_cache->m_flags >> 8 & 1;
    }
    uint32_t alphaWrite;
    uint16_t fog;
    if (second == 0) {
        uint32_t flags = element->m_flags;
        uint16_t write = 0;
        if (static_cast<int8_t>(flags) < 0) {
            faded = 0;
            write = 1;
        } else if ((scene->m_flags & 0x20) == 0) {
            write = 1;
        } else {
            flags |= 0x100;
            element->m_flags = flags;
            write = 0;
        }
        uint32_t noShadow = ~((scene->m_flags >> 4) & (lighting->m_flags >> 4)) & 1;
        if (g_renderSettings.m_shadowMode == 0) {
            noShadow = 1;
        }
        fog = write;
        if (noShadow != 0 || (materialFlags & 0x100) != 0 || (materialFlags & 1) != 0) {
            alphaWrite = 0;
        } else {
            element->m_flags = flags | 0x40;
            shadowed = 1;
            alphaWrite = write;
        }
    } else {
        fog = 1;
        alphaWrite = 0;
    }
    uint32_t flags = element->m_flags;
    uint32_t modelFlags2 = model->m_modelFlags2;
    uint8_t outline = g_m2OutlineEnabled && scene->m_daf0 != 0 && (materialFlags & 0x1000) != 0 ? 1 : 0;
    uint32_t target = pass != 0 ? static_cast<uint32_t>(scene->m_db08) : 0;
    CShaderEffect* fallback = M2CacheDefaultEffect();
    bool isDefault = element->m_64 != 1 && effect == fallback;
    uint16_t instanced = 1;
    if (!isDefault || (element->m_flags & 0x2000) == 0) {
        instanced = 0;
    }
    uint8_t secondPass = second != 0 ? 1 : 0;
    uint16_t vs = ShaderEffectModelVertexPermute(secondPass, (flags & 0x8002) != 0, lit, bones,
        static_cast<uint16_t>(alphaWrite), static_cast<uint16_t>(external), static_cast<uint16_t>(skeletal), instanced);
    uint16_t ps;
    if (!isDefault) {
        ps = ShaderEffectModelPixelPermute(secondPass, fog, static_cast<uint16_t>(shadowed), faded != 0, outline, 0,
            static_cast<uint8_t>(modelFlags2 >> 0xe & 1), target == 7);
    } else {
        ps = ShaderEffectUnlitPixelPermute(static_cast<uint8_t>(instanced), static_cast<uint16_t>(element->m_flags >> 0xb & 1),
            secondPass, static_cast<uint16_t>(shadowed), fog, 0, target == 7);
    }
    uint64_t depthStencil;
    uint16_t blend;
    uint32_t raster;
    M2SceneElementState(model, static_cast<const uint8_t*>(scene->m_038), (scene->m_flags >> 3 & 1) != 0,
        material->m_blendMode, materialFlags, g_renderSettings.m_msaaSamples, ElementTextureKind(element), pass,
        element->m_flags, static_cast<int32_t>(element->m_64), ElementKind(element), &depthStencil, &blend, &raster);
    GxPipeline* pipeline = ShaderEffectGetPipeline(effect, vs, ps, depthStencil, blend, raster, 0x14, 3, target, nullptr);
    element->SetPipeline(pipeline);
    element->m_effect = effect;
    if (vertexPermute != nullptr) {
        *vertexPermute = vs;
    }
    if (pixelPermute != nullptr) {
        *pixelPermute = ps;
    }
}

// The same for a batch drawn through the instanced path, its permutations built from the parts
// the first pass worked out (FUN_1411d7ca0).
// ref: FUN_1411d7ca0
void M2SceneElementPipelineInstanced(CM2Scene* scene, M2Element* element, int32_t faded, CShaderEffect* effect,
    uint8_t pass, int32_t* parts) {
    auto model = static_cast<CM2Model*>(element->m_model);
    CM2ModelLighting* lighting = model->m_lightingUsed;
    const M2Material* material = BatchMaterial(model, element);
    uint16_t materialFlags = material->m_flags;
    int32_t lit = (materialFlags & 1) == 0 && g_m2ProjectedBlendLit[material->m_blendMode] != 0 ? 4 : 0;
    int32_t write = 1;
    uint32_t bones = element->m_section->m_boneInfluences;
    uint32_t external = 0;
    if (ElementKind(element) == 3) {
        if (bones == 0) {
            bones = 1;
        }
        external = scene->m_cache->m_flags >> 8 & 1;
    }
    uint32_t flags = element->m_flags;
    int32_t fade;
    if (static_cast<int8_t>(flags) < 0) {
        fade = 0;
    } else {
        fade = faded;
        if ((scene->m_flags & 0x20) != 0) {
            flags |= 0x100;
            element->m_flags = flags;
            write = 0;
        }
    }
    int32_t shadow = 0;
    int32_t shadowWrite = 0;
    if ((~((lighting->m_flags >> 4) & (scene->m_flags >> 4)) & 1) == 0 && (materialFlags & 0x100) == 0 && (materialFlags & 1) == 0) {
        flags |= 0x40;
        element->m_pipeline = nullptr;
        element->m_flags = flags;
        shadow = 2;
        shadowWrite = write;
    }
    int32_t outline = g_m2OutlineEnabled && scene->m_daf0 != 0 && (materialFlags & 0x1000) != 0 ? 4 : 0;
    if (2 < bones) {
        bones = 2;
    }
    uint32_t vs = ((flags & 0x8002) != 0 ? 2 : 0) + lit + (bones + (shadowWrite + parts[0] * 2) * 3) * 8 + external;
    CShaderEffect* fallback = M2CacheDefaultEffect();
    uint32_t ps;
    if (element->m_64 == 1 || effect != fallback) {
        int32_t base = scene->m_db08 == 7 ? 0x1a : 0x18;
        ps = shadow + (base + fade + (parts[1] + static_cast<int32_t>(ShaderEffectVertexPull()) * 3) * 4) * 8 + outline + write;
    } else {
        ShaderEffectVertexPull();
        ps = 0;
    }
    uint64_t depthStencil;
    uint16_t blend;
    uint32_t raster;
    M2SceneElementState(model, static_cast<const uint8_t*>(scene->m_038), (scene->m_flags >> 3 & 1) != 0,
        material->m_blendMode, materialFlags, g_renderSettings.m_msaaSamples, ElementTextureKind(element), pass,
        element->m_flags, static_cast<int32_t>(element->m_64), ElementKind(element), &depthStencil, &blend, &raster);
    GxPipeline* pipeline = ShaderEffectGetPipeline(effect, static_cast<uint16_t>(vs), static_cast<uint16_t>(ps), depthStencil,
        blend, raster, 0x14, 3, static_cast<uint32_t>(scene->m_db08), nullptr);
    element->SetPipeline(pipeline);
    element->m_effect = effect;
}

// A batch's depth prepass pipeline, of the scene's prepass effect (FUN_1411d7fc0).
// ref: FUN_1411d7fc0
void M2SceneElementPrepassPipeline(CM2Scene* scene, M2Element* element, int32_t second, uint8_t pass) {
    CM2Model* model = nullptr;
    if (element->m_model->m_type == 0) {
        model = static_cast<CM2Model*>(element->m_model);
    }
    const M2Material* material = BatchMaterial(model, element);
    uint8_t bones = element->m_section->m_boneInfluences;
    uint16_t skeletal = static_cast<uint16_t>(model->m_shared->m_data->m_flags >> 0x12 & 1);
    uint8_t external = 0;
    if (ElementKind(element) == 3) {
        if (bones == 0) {
            bones = 1;
        }
        external = static_cast<uint8_t>(scene->m_cache->m_flags >> 8 & 1);
    }
    uint16_t vs = ShaderEffectPrepassVertexPermute(second != 0, (element->m_flags & 0x8002) != 0, bones, external, skeletal);
    uint16_t ps = ShaderEffectPrepassPixelPermute(second != 0);
    uint64_t depthStencil;
    uint16_t blend;
    uint32_t raster;
    M2SceneElementState(element->m_model, static_cast<const uint8_t*>(scene->m_038), (scene->m_flags >> 3 & 1) != 0,
        material->m_blendMode, material->m_flags, g_renderSettings.m_msaaSamples, ElementTextureKind(element), pass,
        element->m_flags, static_cast<int32_t>(element->m_64), ElementKind(element), &depthStencil, &blend, &raster);
    GxPipeline* pipeline = ShaderEffectGetPipeline(static_cast<CShaderEffect*>(scene->m_effectPrepass), vs, ps,
        depthStencil, blend, raster, 0x14, 3, 0, nullptr);
    element->SetPipeline(pipeline);
    element->m_effect = static_cast<CShaderEffect*>(scene->m_effectPrepass);
}

namespace {

// The blend modes the lit sphere draws opaque (DAT_142721268).
const int32_t s_blendOpaque[8] = { 1, 1, 0, 1, 0, 1, 1, 0 };
// A blend mode's blend in the effect pass (DAT_142721390).
const int32_t s_effectBlend[8] = { 2, 2, 2, 3, 4, 5, 6, 7 };

}  // namespace

// What the procedural effect's shaders take from a batch's own: its vertex shader kind among
// {0, 9, 7, 2, 0xc} and its pixel shader kind among {1, 6, 0x24, 7} (FUN_1411dd9d0).
// ref: FUN_1411dd9d0
int32_t* M2SceneEffectShaderParts(CM2Scene* scene, int32_t* out, const M2Batch* batch) {
    (void)scene;
    static const int32_t s_vertexKinds[5] = { 0, 9, 7, 2, 0xc };  // DAT_142721a10
    static const int32_t s_vertexParts[5] = { 0, 0, 0, 1, 1 };
    static const int32_t s_pixelKinds[4] = { 1, 6, 0x24, 7 };     // DAT_142721a20
    static const int32_t s_pixelParts[4] = { 0, 1, 1, 2 };
    out[0] = 0;
    out[1] = 0;
    uint16_t texCoords;
    int32_t vertexKind = static_cast<int32_t>(M2GetVertexShaderKind(batch->m_textureCount, batch->m_shaderId, &texCoords));
    for (uint32_t i = 0; i < 5; i++) {
        if (vertexKind == s_vertexKinds[i]) {
            out[0] = s_vertexParts[i];
            break;
        }
    }
    int32_t pixelKind = static_cast<int32_t>(M2GetPixelShaderKind(batch->m_textureCount, batch->m_shaderId));
    for (uint32_t i = 0; i < 4; i++) {
        if (pixelKind == s_pixelKinds[i]) {
            out[1] = s_pixelParts[i];
            return out;
        }
    }
    return out;
}

// A flipbook impostor batch's pipeline (FUN_1411e07a0).
// ref: FUN_1411e07a0
void M2SceneElementFlipbookPipeline(CM2Scene* scene, M2Element* element, const M2Material* material, uint8_t pass) {
    uint32_t write = 1;
    if (static_cast<int8_t>(element->m_flags) >= 0 && (scene->m_flags & 0x20) != 0) {
        element->m_flags |= 0x100;
        write = 0;
    }
    uint8_t unlit = material->m_flags & 1;
    uint32_t shadow = 0;
    uint32_t shadowWrite = 0;
    uint32_t lit = 0;
    bool first = pass != 0 && unlit == 0 && (lit = 1, g_m2ProjectedBlendLit[material->m_blendMode] != 0);
    if (!first) {
        lit = 0;
    }
    if ((first || pass != 0)
        && (~((element->m_model->m_lightingUsed->m_flags >> 4) & (scene->m_flags >> 4)) & 1) == 0
        && (material->m_flags & 0x100) == 0 && unlit == 0) {
        element->m_flags |= 0x40;
        shadow = 2;
        element->m_pipeline = nullptr;
        if (write == 1) {
            shadowWrite = shadow;
        }
    }
    uint32_t kind = static_cast<uint32_t>(element->m_sortKey >> 0x1f) & 0xf;
    uint32_t external = 0;
    if (kind == 3) {
        external = scene->m_cache->m_flags >> 8 & 1;
    }
    uint64_t depthStencil = 0;
    uint32_t raster = 0;
    uint16_t blend = 0;
    M2SceneElementState(element->m_model, static_cast<const uint8_t*>(scene->m_038), (scene->m_flags >> 3 & 1) != 0,
        material->m_blendMode, material->m_flags, g_renderSettings.m_msaaSamples,
        static_cast<uint32_t>(element->m_batchKey >> 0x20) & 0xf, pass, element->m_flags,
        static_cast<int32_t>(element->m_64), kind, &depthStencil, &blend, &raster);
    uint32_t faded = element->m_model->m_params.m_fade < 1.0f ? 2 : 0;
    uint32_t noPass = pass == 0 ? 1 : 0;
    auto effect = static_cast<CShaderEffect*>(scene->m_effectFlipbook);
    GxPipeline* pipeline = ShaderEffectGetPipeline(effect, static_cast<uint16_t>(shadowWrite + (noPass + external * 2) * 4 + lit),
        static_cast<uint16_t>(shadow + (faded + noPass) * 4 + write), depthStencil, blend, raster, 0x14, 3,
        static_cast<uint32_t>(scene->m_db08), nullptr);
    element->SetPipeline(pipeline);
    element->m_effect = effect;
}

// A procedural effect batch's pipeline, from the parts its batch's shaders give
// (FUN_1411e09e0).
// ref: FUN_1411e09e0
void M2SceneElementProceduralPipeline(CM2Scene* scene, M2Element* element, const M2Material* material, uint8_t pass,
    uint8_t variant, int32_t faded, const int32_t* parts) {
    uint32_t bones = element->m_section->m_boneInfluences;
    uint32_t external = 0;
    if ((static_cast<uint32_t>(element->m_sortKey >> 0x1f) & 0xf) == 3) {
        if (bones == 0) {
            bones = 1;
        }
        external = scene->m_cache->m_flags >> 8 & 1;
    }
    uint32_t flags = element->m_flags;
    if (2 < bones) {
        bones = 2;
    }
    uint32_t vs = ((flags & 0x8002) != 0 ? 2 : 0) + external + (bones + (parts[1] * 2 + variant) * 3) * 4;
    uint32_t write = 1;
    if (static_cast<int8_t>(flags) >= 0 && (scene->m_flags & 0x20) != 0) {
        write = 0;
        element->m_flags = flags | 0x100;
    }
    uint32_t pull = ShaderEffectVertexPull();
    int32_t target = pass != 0 ? scene->m_db08 : 0;
    int32_t extra = (target == 7 ? 2 : 0) + faded;
    uint64_t depthStencil = 0;
    uint32_t raster = 0;
    uint16_t blend = 0;
    M2SceneElementState(element->m_model, static_cast<const uint8_t*>(scene->m_038), (scene->m_flags >> 3 & 1) != 0,
        material->m_blendMode, material->m_flags, g_renderSettings.m_msaaSamples,
        static_cast<uint32_t>(element->m_batchKey >> 0x20) & 0xf, pass, element->m_flags,
        static_cast<int32_t>(element->m_64), static_cast<uint32_t>(element->m_sortKey >> 0x1f) & 0xf, &depthStencil, &blend,
        &raster);
    uint32_t ps = write
        + ((((pass != 0 ? 4 : 0) + extra + (parts[3] + ((pull * 3 + parts[0]) * 3 + parts[2]) * 2) * 8) * 2 + variant) * 2);
    auto effect = static_cast<CShaderEffect*>(scene->m_effectProcedural);
    GxPipeline* pipeline = ShaderEffectGetPipeline(effect, static_cast<uint16_t>(vs), static_cast<uint16_t>(ps), depthStencil,
        blend, raster, 0x14, 3, static_cast<uint32_t>(target), nullptr);
    element->SetPipeline(pipeline);
    element->m_effect = effect;
}

// A model effect's element for a batch: the effect pass replacing or added to the batch's own
// (FUN_1411d8240). `parts` comes back with the procedural effect's shader parts; `skip` is set
// when the batch is left to the procedural pass.
// ref: FUN_1411d8240
bool M2SceneEffectElement(CM2Scene* scene, M2SceneDrawList* list, M2Element* element, const uint32_t* elementIndex,
    float strength, int32_t effectType, int32_t front, int32_t back, int32_t noAlpha, uint8_t pass, CShaderEffect* effect,
    uint32_t vertexPermute, int32_t pixelPermute, int32_t* parts, uint8_t* skip) {
    const float threshold = 0.99998999f;  // DAT_1427211fc
    bool extra = false;
    bool scaleOriginal = true;
    bool translucent = strength < threshold;
    bool markKind = true;
    bool scaleExtra = true;
    bool edge = false;
    bool instanced = false;
    CShaderEffect* passEffect = nullptr;
    int32_t effectParts[2] = { 0, 0 };
    CM2Model* model = nullptr;
    if (element->m_model->m_type == 0) {
        model = static_cast<CM2Model*>(element->m_model);
    }
    float fade = model->m_params.m_fade;
    const M2Material* material = &model->m_shared->m_data->m_materials[static_cast<const M2Batch*>(element->m_batch)->m_materialIndex];
    parts[0] = 0;
    parts[1] = 0;
    parts[2] = 0;
    parts[3] = 0;
    int32_t fadedArg = fade < 1.0f ? 1 : 0;
    *skip = 0;
    bool doExtra;
    bool scaleOriginalNow;
    switch (effectType) {
    case 1: {
        bool lookParts;
        if (noAlpha == 0) {
            if (s_blendOpaque[material->m_blendMode] == 0) {
                passEffect = static_cast<CShaderEffect*>(scene->m_effectLitTransparent);
                lookParts = true;
                instanced = true;
            } else {
                passEffect = static_cast<CShaderEffect*>(scene->m_effectLitOpaque);
                lookParts = false;
            }
        } else {
            passEffect = static_cast<CShaderEffect*>(scene->m_effectLitNoAlpha);
            lookParts = false;
        }
        extra = scene->m_defaultTexture != nullptr;
        markKind = extra;
        doExtra = false;
        scaleOriginalNow = true;
        if (lookParts) {
            M2SceneEffectShaderParts(scene, effectParts, static_cast<const M2Batch*>(element->m_batch));
            markKind = extra;
            doExtra = edge;
            scaleOriginalNow = scaleOriginal;
        }
        break;
    }
    case 2:
        passEffect = static_cast<CShaderEffect*>(scene->m_effectShadowMonk);
        extra = true;
        instanced = true;
        M2SceneEffectShaderParts(scene, effectParts, static_cast<const M2Batch*>(element->m_batch));
        markKind = extra;
        doExtra = edge;
        scaleOriginalNow = scaleOriginal;
        break;
    case 3:
        passEffect = static_cast<CShaderEffect*>(scene->m_effectAvatar);
        extra = true;
        instanced = true;
        scaleOriginal = element->m_alpha < threshold;
        if (!translucent) {
            element->m_pipeline = nullptr;
        }
        M2SceneEffectShaderParts(scene, effectParts, static_cast<const M2Batch*>(element->m_batch));
        markKind = extra;
        doExtra = edge;
        scaleOriginalNow = scaleOriginal;
        break;
    case 5: {
        const uint8_t* modelEffect = static_cast<const uint8_t*>(element->m_model->m_params.m_effect);
        doExtra = false;
        scaleOriginalNow = true;
        if (modelEffect == nullptr) {
            break;
        }
        uint32_t effectFlags = *reinterpret_cast<const uint32_t*>(modelEffect + 0x7c);
        bool pass20 = (effectFlags & 0x20) != 0;
        if (scene->m_daf0 == 0 ? !pass20 : false) {
            return true;
        }
        if (pass20 && s_blendOpaque[material->m_blendMode] == 0) {
            *skip = 1;
            return true;
        }
        extra = material->m_blendMode != 4;
        passEffect = static_cast<CShaderEffect*>(scene->m_effectProcedural);
        doExtra = true;
        element->m_pipeline = nullptr;
        scaleOriginalNow = false;
        uint64_t depthStencil = 0;
        uint32_t raster = 0;
        uint16_t blend = 0;
        uint32_t flags = (scene->m_daf0 != 0 ? 0x20u : 0u) | element->m_flags | 0x200;
        element->m_flags = flags;
        M2SceneElementState(element->m_model, static_cast<const uint8_t*>(scene->m_038), (scene->m_flags >> 3 & 1) != 0,
            material->m_blendMode, material->m_flags, g_renderSettings.m_msaaSamples,
            static_cast<uint32_t>(element->m_batchKey >> 0x20) & 0xf, pass, flags, static_cast<int32_t>(element->m_64),
            static_cast<uint32_t>(element->m_sortKey >> 0x1f) & 0xf, &depthStencil, &blend, &raster);
        GxPipeline* pipeline = ShaderEffectGetPipeline(effect, static_cast<uint16_t>(vertexPermute),
            static_cast<uint16_t>(pixelPermute), depthStencil, blend, raster, 0x14, 3, static_cast<uint32_t>(scene->m_db08), nullptr);
        element->SetPipeline(pipeline);
        element->m_effect = effect;
        parts[0] = material->m_blendMode == 0 ? 0 : (material->m_blendMode != 4) + 1;
        int32_t shaderParts[2];
        M2SceneEffectShaderParts(scene, shaderParts, static_cast<const M2Batch*>(element->m_batch));
        parts[1] = shaderParts[0];
        parts[2] = shaderParts[1];
        parts[3] = static_cast<int32_t>(effectFlags >> 5 & 1);
        scaleExtra = false;
        break;
    }
    case 6:
        passEffect = static_cast<CShaderEffect*>(scene->m_effectEdgeGlow);
        extra = true;
        edge = true;
        scaleOriginal = false;
        instanced = true;
        M2SceneEffectShaderParts(scene, effectParts, static_cast<const M2Batch*>(element->m_batch));
        markKind = extra;
        doExtra = edge;
        scaleOriginalNow = scaleOriginal;
        break;
    case 7: {
        if (pass == 0) {
            return true;
        }
        uint32_t flags = element->m_flags;
        CM2BaseModel* owner = element->m_model;
        element->m_pipeline = nullptr;
        uint16_t materialFlags = material->m_flags;
        uint32_t row = 0;
        if ((materialFlags & 0x10) == 0 || (flags & 0x200) != 0) {
            if ((materialFlags >> 0xe & 1) == 0 && ((flags & 1) == 0 || owner->m_0d == 0)) {
                if ((flags & 0x20) != 0 && (g_renderSettings.m_msaaSamples == 0 || (flags & 0x200) != 0)) {
                    row = 3;
                }
            } else {
                row = 5;
            }
        } else {
            row = 1;
        }
        uint64_t depthStencil = g_m2DepthStencil[row][owner->m_0d * 2 + (g_renderSettings.m_msaaSamples != 0 ? 1 : 0)];
        if (owner->m_type == 0) {
            depthStencil = ((depthStencil & 0x00ffffff00ffffffull) | 0x100002ull | (2ull << 0x38));
        }
        uint16_t blend = s_effectBlend[material->m_blendMode] < 9 ? s_blendWords[s_effectBlend[material->m_blendMode]] : 0;
        if ((flags & 1) != 0) {
            blend = s_blendWords[8];
        }
        if ((flags >> 4 & 1) != 0) {
            blend = s_blendWords[8];
        }
        uint8_t reference = 0;
        if ((materialFlags & 4) == 0 && (element->m_sortKey >> 0x1f & 0xf) != 0) {
            reference = static_cast<uint8_t>((scene->m_flags >> 3 & 1) + 1);
            if ((owner->m_flags >> 0x1a & 1) != 0) {
                reference = static_cast<uint8_t>((reference != 2) + 1);
            }
        }
        const uint32_t* table = static_cast<const uint8_t*>(scene->m_038)[0x5c0] == 1 ? g_m2OutlineStencilFill : g_m2OutlineStencil;
        uint32_t raster = table[reference];
        uint16_t first;
        uint16_t zero;
        if (effect == M2CacheDefaultEffect()) {
            first = ShaderEffectUnlitPixelPermute(0, 0, 0, 0, 0, 1, 0);
            zero = ShaderEffectUnlitPixelPermute(0, 0, 0, 0, 0, 0, 0);
        } else {
            first = ShaderEffectModelPixelPermute(0, 0, 0, 0, 0, 1, 0, 0);
            zero = ShaderEffectModelPixelPermute(0, 0, 0, 0, 0, 0, 0, 0);
        }
        GxPipeline* pipeline = ShaderEffectGetPipeline(effect, static_cast<uint16_t>(vertexPermute),
            static_cast<uint16_t>(pixelPermute + (first - zero)), depthStencil, blend, raster, 0x14, 3,
            static_cast<uint32_t>(scene->m_db08), nullptr);
        element->SetPipeline(pipeline);
        element->m_64 = static_cast<uint32_t>(effectType);
        return true;
    }
    case 8:
        element->m_pipeline = nullptr;
        element->m_64 = static_cast<uint32_t>(effectType);
        M2SceneElementFlipbookPipeline(scene, element, material, pass);
        return true;
    default:
        element->m_64 = static_cast<uint32_t>(effectType);
        return true;
    }
    if (extra && (translucent || doExtra)) {
        float alpha = element->m_alpha;
        uint8_t extraPass = static_cast<uint8_t>(((element->m_model->m_flags2 & 1) | 2) << 2);
        if (scaleOriginalNow) {
            element->m_alpha = (1.0f - strength) * alpha;
        }
        uint32_t index;
        M2Element* added = M2SceneDrawListAddElement(list, &index);
        *added = list->m_elements.m_data[*elementIndex];
        if ((static_cast<uint32_t>(added->m_sortKey >> 0x1f) & 0xf) == 3) {
            added->m_sortKey = (added->m_sortKey & 0xfffffff97fffffffull) | 0x100000000ull;
        }
        float distance = added->m_distance - 0.0099999998f;  // DAT_142636334
        added->m_flags &= 0xfffffcfe;
        added->m_pipeline = nullptr;
        added->m_distance = distance;
        if (!scaleExtra) {
            strength = 1.0f;
        }
        added->m_alpha = strength * alpha;
        added->SetFaded(true);
        if (front != 0) {
            M2SceneDrawListAddToPass(scene, list, extraPass, index);
            pass = extraPass;
        }
        if (back != 0) {
            M2SceneDrawListAddToPass(scene, list, 10, index);
            pass = 10;
        }
        element = added;
    }
    if (markKind) {
        fadedArg = 0;
        element->m_64 = static_cast<uint32_t>(effectType);
    }
    if (passEffect != nullptr) {
        if (scene->m_effectProcedural == nullptr || effectType != 5) {
            if (instanced) {
                M2SceneElementPipelineInstanced(scene, element, fadedArg, passEffect, pass, effectParts);
            } else {
                M2SceneElementPipeline(scene, element, 0, fadedArg, passEffect, pass, nullptr, nullptr);
            }
        } else {
            M2SceneElementProceduralPipeline(scene, element, material, pass, 0, fadedArg, parts);
        }
    }
    return true;
}

// A model's shadow batches as object-fill elements in pass 4 (FUN_1411d1150).
// ref: FUN_1411d1150
void M2SceneShadowElements(CM2Scene* scene, M2SceneDrawList* list, CM2Model* model, float distance) {
    uint32_t count = model->GetShadowBatchCount();
    if (count == 0) {
        return;
    }
    const float threshold = 0.0039215689f;  // DAT_142632528
    float colorAlpha = model->m_params.m_colorAlpha;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* batch;
        if (model->m_600 == nullptr) {
            if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
                model->WaitForLoad(0);
            }
            const M2SkinProfile* profile = model->m_shared->m_skins[model->m_lod].m_profile;
            batch = reinterpret_cast<const uint8_t*>(profile->m_shadowBatches.Data()) + i * 0xc;
            uint16_t section = *reinterpret_cast<const uint16_t*>(batch + 4);
            if (((1 << (section & 7)) & model->m_lodBoneMasks[model->m_lod][section >> 3]) == 0) {
                continue;
            }
        } else {
            batch = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(model->m_600) + 0x18) + i * 0xc;
        }
        if (batch == nullptr) {
            continue;
        }
        if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
            model->WaitForLoad(0);
        }
        float alpha = colorAlpha;
        uint16_t color = *reinterpret_cast<const uint16_t*>(batch + 8);
        if (color < model->m_shared->m_data->m_colors.Count()) {
            alpha *= *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(model->m_colors) + color * 0x20 + 0x1c);
        }
        if ((*batch & 0x40) == 0) {
            uint16_t weight = *reinterpret_cast<const uint16_t*>(batch + 10);
            alpha *= reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(model->m_textureWeights) + weight * 0xc)[2];
        }
        if (alpha <= threshold) {
            continue;
        }
        uint32_t index;
        M2Element* element = M2SceneDrawListAddElement(list, &index);
        const M2SkinSection* sections;
        if (model->m_600 == nullptr) {
            sections = model->m_shared->m_skins[model->m_lod].m_sections;
        } else {
            sections = *reinterpret_cast<const M2SkinSection* const*>(static_cast<const uint8_t*>(model->m_600) + 0x20);
        }
        const M2SkinSection* section = &sections[*reinterpret_cast<const uint16_t*>(batch + 4)];
        element->Initialize(10, 0, model, section, 0.0f, model->m_screenSize, distance, 0, 0);
        element->SetShadowBatch(i, batch);
        uint8_t bones = section->m_boneInfluences;
        if (2 < bones) {
            bones = 2;
        }
        uint32_t raster = g_m2OutlineStencil[1 + ((model->m_flags >> 0x1a) & 1)];
        GxPipeline* pipeline = ObjectFillGetPipeline(static_cast<uint8_t>(~(batch[1] >> 1) & 1), bones,
            ObjectFillDepthStencil(), raster, s_blendWords[0], 0x14, 3, static_cast<uint32_t>(scene->m_db08));
        element->SetPipeline(pipeline);
        M2IndicesPush(&list->m_lists[4], index);
    }
}

uint32_t M2BlendModeIndex(uint8_t blend);
int32_t M2SceneOcclusionTest(CM2Scene* scene, const float* box);

// A model's batches (or shadow batches) as outline elements in pass 2 or 3 (FUN_1411d1530).
// ref: FUN_1411d1530
void M2SceneOutlineElements(CM2Scene* scene, M2SceneDrawList* list, CM2Model* model, float distance) {
    uint32_t count = model->GetShadowBatchCount();
    bool useBatches = false;
    if (count == 0) {
        if ((model->m_modelFlags2 & 0x200) == 0) {
            return;
        }
        if (model->m_600 == nullptr) {
            count = model->m_shared->m_skins[model->m_lod].m_profile->m_batches.Count();
        } else {
            count = *static_cast<const uint16_t*>(model->m_600);
        }
        useBatches = true;
    }
    CM2BaseModel* root = model;
    while ((root->m_flags >> 0x12 & 1) != 0 && root->m_parent != nullptr) {
        root = root->m_parent;
    }
    float colorAlpha = model->m_params.m_colorAlpha;
    float fade = model->m_params.m_fade;
    uint8_t outlinePass = static_cast<uint8_t>((root->m_flags >> 0x19) & 1);
    const float minimum = 0.001f;  // DAT_142618f58
    if (colorAlpha < minimum || fade < minimum || count == 0) {
        return;
    }
    M2Data* data = model->m_shared->m_data;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* shadow = nullptr;
        const M2Batch* batch = nullptr;
        if (model->m_600 == nullptr) {
            if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
                model->WaitForLoad(0);
            }
            const M2SkinProfile* profile = model->m_shared->m_skins[model->m_lod].m_profile;
            uint16_t section;
            if (useBatches) {
                batch = &profile->m_batches[i];
                section = batch->m_skinSectionIndex;
            } else {
                shadow = reinterpret_cast<const uint8_t*>(profile->m_shadowBatches.Data()) + i * 0xc;
                section = *reinterpret_cast<const uint16_t*>(shadow + 4);
            }
            if (((1 << (section & 7)) & model->m_lodBoneMasks[model->m_lod][section >> 3]) == 0) {
                continue;
            }
        } else if (useBatches) {
            batch = reinterpret_cast<const M2Batch*>(*reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(model->m_600) + 0x10) + i * 0x18);
        } else {
            shadow = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(model->m_600) + 0x18) + i * 0xc;
        }
        const M2Material* material = nullptr;
        uint16_t color;
        if (shadow == nullptr) {
            if (batch == nullptr) {
                continue;
            }
            material = &data->m_materials[batch->m_materialIndex - batch->m_materialLayer];
            if ((reinterpret_cast<const uint8_t*>(batch)[6] & 8) != 0 || material->m_blendMode != 0) {
                continue;
            }
            color = batch->m_colorIndex;
        } else {
            if ((*shadow & 8) != 0) {
                continue;
            }
            color = *reinterpret_cast<const uint16_t*>(shadow + 8);
        }
        if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
            model->WaitForLoad(0);
        }
        data = model->m_shared->m_data;
        float alpha = colorAlpha;
        if (color < data->m_colors.Count()) {
            alpha *= *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(model->m_colors) + color * 0x20 + 0x1c);
        }
        uint8_t first = shadow != nullptr ? *shadow : batch->m_flags;
        if ((first & 0x40) == 0) {
            uint16_t weight;
            if (shadow == nullptr) {
                weight = data->m_textureWeightCombos[batch->m_textureWeightComboIndex];
            } else {
                weight = *reinterpret_cast<const uint16_t*>(shadow + 10);
            }
            alpha *= reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(model->m_textureWeights) + weight * 0xc)[2];
        }
        if (alpha <= 0.0039215689f) {  // DAT_142632528
            continue;
        }
        uint32_t index;
        M2Element* element = M2SceneDrawListAddElement(list, &index);
        uint16_t sectionIndex = shadow != nullptr ? *reinterpret_cast<const uint16_t*>(shadow + 4) : batch->m_skinSectionIndex;
        const M2SkinSection* sections;
        if (model->m_600 == nullptr) {
            sections = model->m_shared->m_skins[model->m_lod].m_sections;
        } else {
            sections = *reinterpret_cast<const M2SkinSection* const*>(static_cast<const uint8_t*>(model->m_600) + 0x20);
        }
        const M2SkinSection* section = &sections[sectionIndex];
        element->Initialize(outlinePass + 8, 0, model, section, 0.0f, model->m_screenSize, distance, 0, 0);
        if (shadow == nullptr) {
            element->SetBatch(i, batch, reinterpret_cast<const uint16_t*>(material));
        } else {
            element->SetShadowBatch(i, shadow);
        }
        if (alpha * fade < 1.0f) {
            element->SetFaded(true);
        }
        uint8_t bones = section->m_boneInfluences;
        if (2 < bones) {
            bones = 2;
        }
        uint8_t variant;
        const uint32_t* stencil;
        const uint32_t* fill;
        uint64_t depthStencil;
        bool shadowed = (model->m_flags >> 0x1a & 1) != 0;
        if (shadow != nullptr && (shadow[1] & 1) != 0 && outlinePass == 0) {
            variant = static_cast<uint8_t>(~(shadow[1] >> 1) & 1);
            stencil = &g_m2OutlineStencil[0];
            fill = &g_m2OutlineStencilFill[0];
            depthStencil = g_m2DepthStencil[3][0];
        } else {
            variant = shadow != nullptr ? static_cast<uint8_t>(~(shadow[1] >> 1) & 1) : 0;
            stencil = shadowed ? &g_m2OutlineStencil[2] : &g_m2OutlineStencil[1];
            fill = shadowed ? &g_m2OutlineStencilFill[2] : &g_m2OutlineStencilFill[1];
            depthStencil = outlinePass != 0 ? g_m2DepthStencil[4][0] : g_m2DepthStencil[3][0];
        }
        uint16_t blend = s_blendWords[0];
        if (shadow != nullptr) {
            blend = s_blendWords[~(shadow[1] >> 1) & 1];
        }
        if (static_cast<const uint8_t*>(scene->m_038)[0x5c0] == 1) {
            stencil = fill;
        }
        GxPipeline* pipeline = OutlineGetPipeline(static_cast<uint8_t>(data->m_flags >> 0x12 & 1), variant, bones,
            depthStencil, *stencil, blend, 0x14, 3, 8);
        element->SetPipeline(pipeline);
        M2IndicesPush(&list->m_lists[outlinePass + 2], index);
    }
}

// An emitter's element and its pass (FUN_1411d1c20). `count` counts the additive and
// alpha-key emitters drawn.
// ref: FUN_1411d1c20
void M2SceneParticleElement(CM2Scene* scene, M2SceneDrawList* list, CParticleEmitter2* emitter, CM2Model* model,
    float distance, float alpha, int32_t front, int32_t back, int32_t* count) {
    if (!ParticleEmitterHasParticles(emitter)) {
        return;
    }
    if (emitter->m_type == 1) {
        return;
    }
    if (emitter->m_blend == 10 || emitter->m_blend == 3) {
        (*count)++;
    }
    uint32_t index;
    M2Element* element = M2SceneDrawListAddElement(list, &index);
    uint32_t extraFlags = 0;
    uint8_t pass;
    if (emitter->m_type == 3) {
        if (static_cast<const uint8_t*>(scene->m_038)[0x5bc] != 0) {
            return;
        }
        if (front == 0) {
            return;
        }
        pass = 1;
    } else if (1 < emitter->m_blend || alpha < 0.99998999f) {
        if ((emitter->m_flags & 0x40000) == 0) {
            if ((model->m_flags2 & 1) == 0) {
                if (front == 0) {
                    pass = back != 0 ? 10 : 0xe;
                } else {
                    pass = 8;
                    if (back != 0) {
                        M2SceneDrawListAddToPass(scene, list, 10, index);
                        extraFlags = 2;
                    }
                }
            } else {
                pass = 0xc;
            }
        } else {
            pass = 10;
        }
    } else {
        pass = static_cast<uint8_t>((~(model->m_createFlags >> 3) & 1) | 6);
    }
    if ((scene->m_flags & 0x800) != 0 && (model->m_flags2 & 8) != 0) {
        extraFlags |= 0x8000;
    }
    element->Initialize(5, 0, model, nullptr, alpha, model->m_screenSize, distance,
        *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(emitter) + 0x14), 0);
    element->SetEmitter(emitter);
    element->m_flags |= extraFlags;
    uint8_t renderFlags = emitter->m_renderFlags;
    uint16_t materialFlags = static_cast<uint16_t>((~renderFlags & 1) + 4);
    if ((renderFlags & 2) == 0) {
        materialFlags |= 2;
    }
    if ((renderFlags & 4) == 0) {
        materialFlags |= 0x10;
    }
    if ((renderFlags & 8) != 0) {
        materialFlags |= 0x1000;
    }
    CShaderEffect* effect = g_particleEffects[emitter->m_vertexFormat];
    uint32_t blendMode = M2BlendModeIndex(emitter->m_blend);
    uint8_t outline = 0;
    if (g_m2OutlineEnabled && scene->m_daf0 != 0 && (materialFlags & 0x1000) != 0) {
        outline = 1;
        if ((emitter->m_flags & 0x44000) == 0x44000) {
            outline = 0;
        }
    }
    uint8_t lit = (materialFlags & 1) == 0 && g_m2ProjectedBlendLit[blendMode & 0xffff] != 0 ? 1 : 0;
    uint16_t vs = ShaderEffectModelVertexPermute(0, extraFlags != 0, lit, 0, 0, 0, 0, 0);
    uint16_t additive = (static_cast<uint16_t>(blendMode) == 1 && 0.0f < alpha * 0.50196081f) ? 1 : 0;  // DAT_1427219d8
    int32_t target = pass != 1 ? scene->m_db08 : 0xf;
    uint16_t ps = ShaderEffectParticlePixelPermute(outline, additive, target == 7);
    uint64_t depthStencil = 0;
    uint32_t raster = 0;
    uint16_t blend = 0;
    M2SceneElementState(model, static_cast<const uint8_t*>(scene->m_038), (scene->m_flags >> 3 & 1) != 0,
        static_cast<uint16_t>(blendMode), materialFlags, g_renderSettings.m_msaaSamples,
        static_cast<uint32_t>(element->m_batchKey >> 0x20) & 0xf, pass, element->m_flags,
        static_cast<int32_t>(element->m_64), static_cast<uint32_t>(element->m_sortKey >> 0x1f) & 0xf, &depthStencil, &blend,
        &raster);
    uint8_t vertexFormat = ParticleEmitterVertexFormat(emitter);
    GxPipeline* pipeline = ShaderEffectGetPipeline(effect, vs, ps, depthStencil, blend, raster, vertexFormat, 3,
        static_cast<uint32_t>(target), nullptr);
    element->SetPipeline(pipeline);
    element->m_effect = effect;
    if (scene->m_db28 != 0 && pass == 8 && (model->m_flags >> 2 & 1) != 0 && (model->m_flags & 1) != 0) {
        float box[6];
        int32_t visible = M2SceneOcclusionTest(scene, model->GetBounds(box));
        if (visible == 1) {
            pass = 9;
        } else if (visible == 2 || visible == 3) {
            if (visible == 3) {
                M2SceneDrawListAddToPass(scene, list, 9, index);
            }
            pass = 8;
        }
    }
    M2SceneDrawListAddToPass(scene, list, pass, index);
}

// A blend mode's index among the eight M2 blends (FUN_1411d81a0).
// ref: FUN_1411d81a0
uint32_t M2BlendModeIndex(uint8_t blend) {
    switch (blend) {
    case 1:
        return 1;
    case 2:
        return 2;
    case 3:
        return 4;
    case 4:
        return 5;
    case 5:
        return 6;
    case 10:
        return 3;
    case 0xd:
        return 7;
    default:
        return 0;
    }
}

// Whether a box is behind the world's occluders: 1 hidden, 2 seen, 3 partly (FUN_1411e44c0).
// The world scene's occlusion (scene +0xdb28) is not ported; UI scenes have none and never ask.
// ref: FUN_1411e44c0
int32_t M2SceneOcclusionTest(CM2Scene* scene, const float* box) {
    (void)box;
    if (scene->m_db28 == 0) {
        return 0;
    }
    FROST_UNIMPLEMENTED();
    return 0;
}
