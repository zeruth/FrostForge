#include "shadereffect/ShaderEffectManager.hpp"

#include <cstring>
#include <new>

#include "blz/Sync.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPipeline.hpp"
#include "shadereffect/PipelineMap.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

const char s_managerFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\ShaderEffect\\ShaderEffectManager.cpp";
const char s_effectNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CB_KPEAVCShaderEffect@@@blz@@@blz@@";
const char s_effectBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CB_KPEAVCShaderEffect@@@blz@@@blz@@";

// chained_hash_node<pair<uint64_t const, CShaderEffect*>> (0x18 bytes).
struct EFFECTNODE {
    EFFECTNODE* m_next;
    uint64_t m_key;
    CShaderEffect* m_effect;
};

// The effects by name hash (DAT_142cfc2f0) and their lock (DAT_1430a5b60).
blz::chained_hash_header s_effects;
blz::shared_mutex s_effectsLock;

uint64_t NameHash(const char* name) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint64_t length = std::strlen(name); length != 0; length--) {
        hash = (hash ^ static_cast<uint8_t>(*name++)) * 0x100000001b3ull;
    }
    return hash;
}

// ref: FUN_1412a4380
void EffectsRehash(uint64_t count) {
    blz::chained_rehash<EFFECTNODE>(&s_effects, count, s_effectBucketTag);
}

// ref: FUN_1412a42b0
void EffectsRemove(uint64_t key) {
    EFFECTNODE* node = blz::chained_unlink<EFFECTNODE>(&s_effects, key);
    if (node != nullptr) {
        SMemFree(node, s_effectNodeTag, -6, 0);
    }
}

// ref: FUN_1412ab6a0
void DeleteEffect(CShaderEffect* effect) {
    if (effect != nullptr) {
        effect->~CShaderEffect();
        SMemFree(effect, s_managerFile, -2, 0);
    }
}

// The model effects, made with the device.
// ref: FUN_1412a3440
void OnDeviceCreated(void* param);

// The effects' pipelines go when the multisampling changes.
// ref: FUN_1412a3dc0
void OnRenderSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    (void)param;
    if (previous->m_msaaSamples == current->m_msaaSamples) {
        return;
    }
    s_effectsLock.lock();
    blz::chained_for_each<EFFECTNODE>(&s_effects, [](EFFECTNODE* node) {
        CShaderEffect* effect = node->m_effect;
        PipelineMapDestroyPipelines(&effect->m_pipelines);
        PipelineMapClear(&effect->m_pipelines);
        PipelineMapDestroyPipelines(&effect->m_newPipelines);
        PipelineMapClear(&effect->m_newPipelines);
    });
    s_effectsLock.unlock();
}

// ref: FUN_1412a3440
void OnDeviceCreated(void* param) {
    (void)param;
    ShaderEffectCreate("EdgeGlow", 0x8f, 0x53);
    ShaderEffectCreate("LitSphere_Additive_Transparent", 0x5a, 0x9a);
    ShaderEffectCreate("LitSphere_Additive_Opaque", 0x59, 0x99);
    ShaderEffectCreate("LitSphere_Additive_Transparent_NoAlpha", 0x59, 0x9b);
    ShaderEffectCreate("Projected_Lit_Opaque_Single_Texture", 0x2b, 0xcc);
    ShaderEffectCreate("Projected_Lit_Add_Single_Texture", 0x2b, 0xcb);
    ShaderEffectCreate("Projected_Unlit_Opaque_Single_Texture", 0xb, 0xcc);
    ShaderEffectCreate("Projected_Unlit_Add_Single_Texture", 0xb, 0xcb);
    ShaderEffectCreate("Projected_Lit_Two_Texture", 0x2d, 0xce);
    ShaderEffectCreate("Projected_Unlit_Two_Texture", 0xd, 0xce);
    ShaderEffectCreate("Projected_Lit_Three_Texture", 0x2c, 0xcd);
    ShaderEffectCreate("Projected_Unlit_Three_Texture", 0xc, 0xcd);
    ShaderEffectCreate("Model2PrepassOpaque", 0x77, 0xb9);
    ShaderEffectCreate("Particle_Lit_T1", 0x69, 0xb3);
    ShaderEffectCreate("Particle_Unlit_T1", 0x6a, 0xb3);
    ShaderEffectCreate("Particle_Unlit_2ColorTex_3AlphaTex", 0x6b, 0xb0);
    ShaderEffectCreate("Particle_Unlit_3ColorTex_3AlphaTex", 0x6b, 0xb1);
    ShaderEffectCreate("Particle_Unlit_3ColorTex_3AlphaTex_UV", 0x6c, 0xb2);
    ShaderEffectCreate("Particle_Refraction", 0x6b, 0xd1);
    ShaderEffectCreate("ShadowMonk", 0x8f, 0xd9);
    ShaderEffectCreate("Avatar", 3, 0x17);
    ShaderEffectCreate("FlipbookImpostor", 0x42, 0x75);
    RenderSettingsAddHandler(&OnRenderSettingsChanged, nullptr);
}

// ref: FUN_1412a36f0
void OnDeviceDestroyed(void* param) {
    (void)param;
    RenderSettingsRemoveHandler(&OnRenderSettingsChanged, nullptr);
    s_effectsLock.lock();
    blz::chained_for_each<EFFECTNODE>(&s_effects, [](EFFECTNODE* node) {
        node->m_effect->m_50 = 0;
        DeleteEffect(node->m_effect);
    });
    if (s_effects.m_buckets != nullptr) {
        for (uint64_t i = 0; i < s_effects.m_bucketCount; i++) {
            for (auto node = static_cast<EFFECTNODE*>(s_effects.m_buckets[i]); node != nullptr;) {
                EFFECTNODE* next = node->m_next;
                SMemFree(node, s_effectNodeTag, -6, 0);
                node = next;
            }
            s_effects.m_buckets[i] = nullptr;
        }
    }
    s_effects.m_size = 0;
    s_effectsLock.unlock();
}

}

CShaderEffect::CShaderEffect() {
    this->m_nameHash = 0;
    this->m_50 = 0;
    this->m_vertexShader = 0;
    this->m_pixelShader = 0;
    OsMutexInit(&this->m_lock);
}

CShaderEffect::~CShaderEffect() {
    this->ReleasePipelines();
    TRANSIENTPIPELINEMAP transient = { reinterpret_cast<uint64_t>(this), &this->m_newPipelines, &this->m_pipelines };
    TransientPipelineMapRemove(&transient);
    PipelineMapDestroy(&this->m_newPipelines);
    PipelineMapDestroy(&this->m_pipelines);
}

bool CShaderEffect::Initialize(uint32_t vertexShader, uint32_t pixelShader) {
    this->m_vertexShader = vertexShader;
    this->m_pixelShader = pixelShader;
    return true;
}

// ref: FUN_1412ac4a0
void CShaderEffect::ReleasePipelines() {
    PipelineMapDestroyPipelines(&this->m_pipelines);
    PipelineMapClear(&this->m_pipelines);
    PipelineMapDestroyPipelines(&this->m_newPipelines);
    PipelineMapClear(&this->m_newPipelines);
}

// ref: FUN_1412a30e0
CShaderEffect* ShaderEffectCreate(const char* name, uint32_t vertexShader, uint32_t pixelShader) {
    uint64_t hash = NameHash(name);
    s_effectsLock.lock();
    EFFECTNODE* found = blz::chained_find<EFFECTNODE>(&s_effects, hash);
    if (found != nullptr) {
        CShaderEffect* effect = found->m_effect;
        s_effectsLock.unlock();
        return effect;
    }
    void* memory = SMemAlloc(sizeof(CShaderEffect), 1, s_managerFile, SStrHashFnv1a(s_managerFile), 0xa8, 0);
    auto effect = new (memory) CShaderEffect();
    bool inserted;
    EFFECTNODE* node = blz::chained_insert<EFFECTNODE>(&s_effects, hash, &inserted, s_effectNodeTag, s_effectBucketTag);
    node->m_effect = effect;
    effect->m_nameHash = hash;
    if (!effect->Initialize(vertexShader, pixelShader)) {
        EffectsRemove(hash);
        DeleteEffect(effect);
        effect = nullptr;
    }
    s_effectsLock.unlock();
    return effect;
}

// ref: FUN_1412a38a0
CShaderEffect* ShaderEffectFind(const char* name) {
    s_effectsLock.lock_shared();
    uint64_t hash = NameHash(name);
    EFFECTNODE* node = blz::chained_find<EFFECTNODE>(&s_effects, hash);
    CShaderEffect* effect = node != nullptr ? node->m_effect : nullptr;
    s_effectsLock.unlock_shared();
    return effect;
}

// ref: FUN_1412a3680
void ShaderEffectDestroy(CShaderEffect** effect) {
    s_effectsLock.lock();
    if (*effect != nullptr) {
        EffectsRemove((*effect)->m_nameHash);
        DeleteEffect(*effect);
        *effect = nullptr;
    }
    s_effectsLock.unlock();
}

// ref: FUN_1412a3990
void ShaderEffectManagerInitialize() {
    GxAddDeviceCreatedCallback(&OnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
    GxAddFrameStartCallback(&TransientPipelineMapMerge, nullptr);
}

// ref: FUN_1412a3640
void ShaderEffectManagerShutdown() {
    GxRemoveDeviceCreatedCallback(&OnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
    GxRemoveFrameStartCallback(&TransientPipelineMapMerge, nullptr);
    OnDeviceDestroyed(nullptr);
}
