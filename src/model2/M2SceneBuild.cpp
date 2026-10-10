#include <cmath>
#include <cstring>

#include "client/RenderSettings.hpp"
#include "model2/CM2Lighting.hpp"
#include "model2/M2Element.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shaders.hpp"
#include "model2/M2Shared.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "shadereffect/ShaderEffectPipeline.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

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
