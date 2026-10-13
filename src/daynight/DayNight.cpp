#include "daynight/DayNight.hpp"

#include "base/DataAllocator.hpp"
#include "base/Matrix.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowDB2Lookup.hpp"
#include "gx/Gx.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/GxPipeline.hpp"
#include "client/RenderSettings.hpp"
#include <cstring>
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldScene.hpp"
#include <cmath>
#include <new>

const char s_dnTagU8[] = ".?AV?$TSFixedArray@E$07@@";
const char s_dnTagFloat[] = ".?AV?$TSFixedArray@M$07@@";
const char s_dnTagC3Vector[] = ".?AV?$TSFixedArray@VC3Vector@@$07@@";
const char s_dnTagC2Vector[] = ".?AV?$TSFixedArray@VC2Vector@@$07@@";
const char s_dnTagArgb[] = ".?AV?$TSFixedArray@VCArgb@@$07@@";
const char s_dnTagU16[] = ".?AV?$TSFixedArray@G$07@@";
const char s_dnTagArgbVector[] = ".?AVCArgb@@";
const char s_dnTagTexPtr[] = ".?AV?$unique_ptr@UGxTex@@UGxResourceDelete@@@blz@@";
const char s_dnTagInt[] = ".H";
const char s_dnTagAreaOverride[] = ".?AV?$TSFixedArray@UAreaLightOverride@DayNight@@$07@@";
const char s_dnTagZoneLight[] = ".?AVCZoneLight@DayNight@@";
const char s_dnTagPairIntFloat[] = ".?AU?$pair@HM@blz@@";

namespace {

const char s_dayNightFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\DayNight\\DayNight.cpp";
const char s_dnIntFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\DayNight\\DNInt.cpp";
const char s_overrideNodeTag[] =
    ".?AU?$chained_hash_node@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@blz@@V?$allocator@D@2@@blz@@"
    "PEAUDNOverrideSky@DayNight@@@blz@@@blz@@";
const char s_overrideTableTag[] =
    ".PEAU?$chained_hash_node@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@blz@@V?$allocator@D@2@@blz@@"
    "PEAUDNOverrideSky@DayNight@@@blz@@@blz@@";
const char s_stringTag[] = ".D";

// DAT_142d4ca20: every handle, linked at the tail.
TSList<DAYNIGHT, TSGetLink<DAYNIGHT>, 8> s_handles(offsetof(DAYNIGHT, m_link));

// DAT_1447aada8: the CelestialBody patch callback is registered.
bool s_celestialRegistered;

// The planets' textures and scales (the sun, the first and second moons).
constexpr int32_t SUN_GLARE_TEXTURE = 0x2d76d;
constexpr int32_t MOON_GLARE_TEXTURE = 0x2d746;
constexpr int32_t SUN_TEXTURE = 0x2d76c;
constexpr int32_t MOON_TEXTURE = 0x2d743;
constexpr int32_t MOON2_TEXTURE = 0x2d744;
constexpr float MOON_SCALE = 1.75f;         // DAT_142721200
constexpr float MOON2_SCALE2 = 1.7f;        // DAT_142841484
constexpr int32_t SKY_MODEL = 0x1fe45;

constexpr float POSITION_EPSILON = 2.3841858e-07f;  // DAT_14261a954

// A Light row as the gatherer reads it: a position, then the continent at +0x14.
struct LIGHTROW {
    float m_position[3];            // 0x00
    float m_0c[2];
    int16_t m_continent;            // 0x14
};

// The override skies' nodes (blz::chained_hash_node<pair<const string, DNOverrideSky*>>).
struct OVERRIDENODE {
    OVERRIDENODE* m_next;           // 0x00
    char* m_string;                 // 0x08
    uint64_t m_length;              // 0x10
    int64_t m_capacity;             // 0x18 negative: inline
    uint8_t m_20[0x10];
    void* m_sky;                    // 0x30 DNOverrideSky (0x80)
};

// One override sky's teardown (its model, its light list, its load request and itself).
void DestroyOverrideSky(void* sky) {
    (void)sky;
    FROST_UNIMPLEMENTED();  // DNOverrideSky: FUN_1411e7880 on +0x38, the .H vector at +0x58, FUN_140613680
}

}  // namespace

// ---- the colour grading --------------------------------------------------------------------

namespace {

const char s_colorGradingPtrTag[] = ".PEAVDNColorGrading@DayNight@@";
const char s_texturePtrTag[] = ".PEAVCTexture@@";

// Every colour grading.
blz::vector<DNColorGrading*, s_colorGradingPtrTag> s_colorGradings;  // DAT_1447aa370

// The grading blend, one pipeline per pixel shader permutation.
GxPipeline* s_blendPipelines[15];  // DAT_1447aa390

}  // namespace

// The device came (back): the blend pipelines, and every grading's targets again.
// ref: FUN_14244f8b0
void DNColorGradingOnDeviceCreated(void* param) {
    (void)param;
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_topology = 4;
    key.m_raster[2] = 1;
    key.m_depthStencil[0] = 0x20;
    key.m_depthStencil[4] = 7;
    RENDERTARGETFORMATS formats = RenderTargetFormats(5);
    std::memcpy(key.m_targetFormats, &formats, 4);
    key.m_depthFormat = reinterpret_cast<const uint8_t*>(&formats)[4];
    key.m_vertexShader = 0x47;
    key.m_pixelShader = 0x1a;
    key.m_blend = 0xf000;
    for (uint16_t i = 0; i < 15; i++) {
        key.m_psPermute = i;
        s_blendPipelines[i] = GxPipelineCreate(&key, "BlendTextures");
    }
    for (uint64_t i = 0; i < s_colorGradings.size(); i++) {
        s_colorGradings[i]->CreateTargets();
    }
}

// The device is going: the blend pipelines, and every grading's targets.
// ref: FUN_14244f9f0
void DNColorGradingOnDeviceDestroyed(void* param) {
    (void)param;
    for (int32_t i = 0; i < 15; i++) {
        GxPipelineDestroy(&s_blendPipelines[i]);
    }
    for (uint64_t i = 0; i < s_colorGradings.size(); i++) {
        auto textures = reinterpret_cast<void**>(s_colorGradings[i]->m_data);
        GxTexDestroy(&textures[0]);
        textures[0] = nullptr;
        GxRenderTargetsClear(reinterpret_cast<GXRENDERTARGETS*>(s_colorGradings[i]->m_data + 0x10));
        GxTexDestroy(&textures[1]);
        textures[1] = nullptr;
        GxRenderTargetsClear(reinterpret_cast<GXRENDERTARGETS*>(s_colorGradings[i]->m_data + 0x78));
    }
}

// ref: FUN_14244f3b0
DNColorGrading::DNColorGrading() {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    s_colorGradings.push_back(this);
    this->CreateTargets();
}

// ref: FUN_14244fdb0
void DNColorGrading::CreateTargets() {
    auto textures = reinterpret_cast<void**>(this->m_data);
    for (int32_t i = 0; i < 2; i++) {
        if (!GxTexCreateParams(0, 0x400, 0x20, 1, 2, 2, 0xc2001, "ColorGradingRenderTarget", &textures[i])) {
            textures[i] = nullptr;
            return;
        }
        auto targets = reinterpret_cast<GXRENDERTARGETS*>(this->m_data + 0x10 + i * 0x68);
        GxRenderTargetsAttach(targets, 1, static_cast<gxDx12Backend::Tex*>(textures[i]));
        GxRenderTargetsCount(targets);
    }
}

// ref: FUN_14244f5f0
DNColorGrading::~DNColorGrading() {
    auto textures = reinterpret_cast<void**>(this->m_data);
    GxTexDestroy(&textures[0]);
    textures[0] = nullptr;
    GxRenderTargetsClear(reinterpret_cast<GXRENDERTARGETS*>(this->m_data + 0x10));
    GxTexDestroy(&textures[1]);
    textures[1] = nullptr;
    GxRenderTargetsClear(reinterpret_cast<GXRENDERTARGETS*>(this->m_data + 0x78));
    auto loaded = DNField<HTEXTURE*>(this->m_data, 0x108);
    uint64_t count = DNField<uint64_t>(this->m_data, 0x110);
    for (uint64_t i = 0; i < count; i++) {
        TextureFree(&loaded[i]);
    }
    DNField<uint64_t>(this->m_data, 0x110) = 0;
    DNField<uint64_t>(this->m_data, 0xf8) = 0;
    uint64_t kept = 0;
    for (uint64_t i = 0; i < s_colorGradings.size(); i++) {
        if (s_colorGradings[i] != this) {
            s_colorGradings[kept++] = s_colorGradings[i];
        }
    }
    s_colorGradings.erase(kept, s_colorGradings.size());
    if (static_cast<int64_t>(DNField<uint64_t>(this->m_data, 0x118)) >= 0) {
        SMemFree(DNField<void*>(this->m_data, 0x108), s_texturePtrTag, -6, 0);
        DNField<void*>(this->m_data, 0x108) = nullptr;
    }
    if (static_cast<int64_t>(DNField<uint64_t>(this->m_data, 0x100)) >= 0) {
        SMemFree(DNField<void*>(this->m_data, 0xf0), s_texturePtrTag, -6, 0);
        DNField<void*>(this->m_data, 0xf0) = nullptr;
    }
}

// ref: FUN_142450020
void DNColorGrading::ReleaseUnused() {
    // the loaded textures at +0x108 (count +0x110), those used this frame at +0xf0 (+0xf8)
    HTEXTURE* texture = DNField<HTEXTURE*>(this->m_data, 0x108);
    while (texture != DNField<HTEXTURE*>(this->m_data, 0x108) + DNField<uint64_t>(this->m_data, 0x110)) {
        HTEXTURE* used = DNField<HTEXTURE*>(this->m_data, 0xf0);
        HTEXTURE* usedEnd = used + DNField<uint64_t>(this->m_data, 0xf8);
        for (; used != usedEnd; used++) {
            if (*used == *texture) {
                break;
            }
        }
        if (used != usedEnd) {
            texture++;
            continue;
        }
        TextureFree(texture);
        HTEXTURE* end = DNField<HTEXTURE*>(this->m_data, 0x108) + DNField<uint64_t>(this->m_data, 0x110);
        std::memmove(texture, texture + 1, static_cast<size_t>(end - (texture + 1)) * sizeof(HTEXTURE));
        DNField<uint64_t>(this->m_data, 0x110)--;
    }
    DNField<uint64_t>(this->m_data, 0xf8) = 0;
}

// ---- the scene -----------------------------------------------------------------------------

// ref: FUN_1424587b0
DNScene::DNScene(CWorldScene* worldScene, CWorldView* view) {
    this->m_1bf0 = 0;
    std::memset(this->m_1bf9, 0, sizeof(this->m_1bf9));
    this->m_overrideBuckets = 8;
    this->m_overrideCount = 0;
    this->m_overrideTable = nullptr;
    this->m_overrideLoad = 1.0f;
    this->m_1c80 = 0;
    std::memset(this->m_1ecc, 0, sizeof(this->m_1ecc));
    this->m_m2Scene = nullptr;
    this->m_colorGrading = nullptr;
    this->m_time = 0;
    this->m_blends = this->m_blendInline;
    this->m_blendCount = 0;
    this->m_blendCapacity = 0x8000000000000010ull;
    this->m_blendCount2 = 0;
    this->m_blends2 = this->m_blendInline2;
    this->m_blendCapacity2 = 0x8000000000000010ull;
    std::memset(this->m_2040, 0, sizeof(this->m_2040));
    this->m_loaded = 0;
    this->m_1601 = 0;
    this->m_1602 = 1;
    this->m_1603 = 1;
    this->m_1bf8 = 0;
    this->m_1c3c = 8000.0f;
    this->m_1c40 = 8000.0f;
    this->m_1ec8 = 0;
    this->m_skyModel = nullptr;
    this->m_worldScene = worldScene;
    void* env = worldScene != nullptr ? WorldSceneGetEnv(worldScene) : nullptr;
    this->m_m2Scene = M2CacheCreateScene(0, worldScene, view, env);
    this->m_time = FrameTime_GetTime();
    void* memory = SMemAlloc(sizeof(DNColorGrading), 0x27, s_dnIntFile, SStrHashFnv1a(s_dnIntFile), 0x40, 0);
    this->m_colorGrading = new (memory) DNColorGrading();
    this->m_2060 = 0;
    std::memset(this->m_2040, 0, sizeof(this->m_2040));
}

// ref: FUN_142458e70
void DNScene::CreateDeviceObjects() {
    if (GxDevExists()) {
        this->m_sunGlare.CreateQuery();
        this->m_moonGlare.CreateQuery();
    }
    if (!s_celestialRegistered) {
        g_celestialBodyDB.AddPatchCallback(DayNightOnCelestialBodyPatched, nullptr);
        s_celestialRegistered = true;
    }
}

// ref: FUN_142458ed0
void DNScene::DestroyDeviceObjects() {
    this->m_sunGlare.DestroyQuery();
    this->m_moonGlare.DestroyQuery();
    this->m_clouds.DestroyTextures();
    if (s_celestialRegistered) {
        g_celestialBodyDB.RemovePatchCallback(DayNightOnCelestialBodyPatched);
        s_celestialRegistered = false;
    }
}

// ref: FUN_142458b20
DNScene::~DNScene() {
    if (this->m_loaded != 0) {
        this->UnloadMap();
    }
    M2SceneRelease(this->m_m2Scene);
    DNColorGrading* grading = this->m_colorGrading;
    this->m_m2Scene = nullptr;
    if (grading != nullptr) {
        delete grading;
    }
    this->DestroyDeviceObjects();
    if (static_cast<int64_t>(this->m_blendCapacity2) >= 0) {
        SMemFree(this->m_blends2, s_dnTagPairIntFloat, -6, 0);
        this->m_blends2 = nullptr;
    }
    if (static_cast<int64_t>(this->m_blendCapacity) >= 0) {
        SMemFree(this->m_blends, s_dnTagPairIntFloat, -6, 0);
        this->m_blends = nullptr;
    }
    // m_areaLights free their storage as blz::vector does (the reference walks them backwards).
    this->m_areaOverrides.Free();
    if (this->m_overrideTable != nullptr) {
        for (uint64_t bucket = 0; bucket < this->m_overrideBuckets; bucket++) {
            auto node = static_cast<OVERRIDENODE*>(this->m_overrideTable[bucket]);
            while (node != nullptr) {
                OVERRIDENODE* next = node->m_next;
                if (node->m_capacity >= 0) {
                    SMemFree(node->m_string, s_stringTag, -6, 0);
                }
                SMemFree(node, s_overrideNodeTag, -6, 0);
                node = next;
            }
        }
        SMemFree(this->m_overrideTable, s_overrideTableTag, -6, 0);
        this->m_overrideTable = nullptr;
    }
    // m_sky, the glares, m_clouds and m_light run their destructors.
    // m_zoneLights frees its storage as blz::vector does.
}

// ref: FUN_142443a40
void DNScene::UnloadMap() {
    if (this->m_overrideTable != nullptr) {
        for (uint64_t bucket = 0; bucket < this->m_overrideBuckets; bucket++) {
            for (auto node = static_cast<OVERRIDENODE*>(this->m_overrideTable[bucket]); node != nullptr;
                 node = node->m_next) {
                DestroyOverrideSky(node->m_sky);
            }
        }
        for (uint64_t bucket = 0; bucket < this->m_overrideBuckets; bucket++) {
            auto node = static_cast<OVERRIDENODE*>(this->m_overrideTable[bucket]);
            while (node != nullptr) {
                OVERRIDENODE* next = node->m_next;
                if (node->m_capacity >= 0) {
                    SMemFree(node->m_string, s_stringTag, -6, 0);
                }
                SMemFree(node, s_overrideNodeTag, -6, 0);
                node = next;
            }
            this->m_overrideTable[bucket] = nullptr;
        }
    }
    this->m_overrideCount = 0;
    this->m_light.Field<uint64_t>(0x80) = 0;
    this->m_light.Field<uint64_t>(0x88) = 0;
    this->m_light.Field<uint64_t>(0xb8) = 0;
    this->m_light.Field<uint64_t>(0xd0) = 0;
    this->m_light.Field<uint64_t>(0xc0) = 0;
    this->m_light.Field<uint64_t>(0xd8) = 0;
    this->m_light.Field<uint64_t>(0xc8) = 0;
    this->m_light.Field<uint64_t>(0xe0) = 0;
    for (uint32_t i = 0; i < this->m_areaOverrides.m_count; i++) {
        FROST_UNIMPLEMENTED();  // FUN_142430fb0(&DAT_1447aa310, the override's +8, 0, 0)
    }
    this->m_areaOverrides.m_count = 0;
    this->m_sky.Release();
    this->m_clouds.Release();
    for (DNPLANET& planet : this->m_planets) {
        if (planet.Field<HTEXTURE>(0x10) != nullptr) {
            planet.Release();
        }
    }
    this->m_sunGlare.Release();
    this->m_moonGlare.Release();
    if (this->m_skyModel != nullptr) {
        this->m_skyModel->Release(1);
    }
    this->m_skyModel = nullptr;
    for (auto& lights : this->m_areaLights) {
        if (lights.size() != 0) {
            lights.erase(0, lights.size());
        }
    }
    this->m_loaded = 0;
}

// The SetMap body (FUN_142442fe0 through the handle).
void DNScene::SetMap(int32_t mapId) {
    if (this->m_loaded != 0) {
        this->UnloadMap();
    }
    this->m_light.SetMap(mapId);
    DayNightGatherLights(&this->m_areaLights[0], mapId);
    this->m_loaded = 1;
    this->m_1601 = 0;
    this->m_1603 = 1;
    this->m_sky.Initialize();
    this->m_clouds.Initialize();
    this->m_sunGlare.Initialize(SUN_GLARE_TEXTURE, &this->m_planets[0]);
    this->m_moonGlare.Initialize(MOON_GLARE_TEXTURE, &this->m_planets[1]);
    this->m_planets[0].Initialize(SUN_TEXTURE, 1.0f, 1.0f);
    this->m_planets[1].Initialize(MOON_TEXTURE, MOON_SCALE, 1.0f);
    this->m_planets[2].Initialize(MOON2_TEXTURE, 1.0f, MOON2_SCALE2);
    this->m_skyModel = this->m_m2Scene->CreateModel(SKY_MODEL, 0x100, 1, 0);
}

// ---- the module ----------------------------------------------------------------------------

namespace {

// A device came up: every handle's scene makes its device objects again.
// ref: FUN_142442270
void OnDeviceCreated(void* param) {
    (void)param;
    for (DAYNIGHT* handle = s_handles.Head(); handle != nullptr; handle = s_handles.Next(handle)) {
        handle->m_scene->CreateDeviceObjects();
    }
}

// The device is going away: every handle's scene lets go of its device objects.
// ref: FUN_142442460
void OnDeviceDestroyed(void* param) {
    (void)param;
    for (DAYNIGHT* handle = s_handles.Head(); handle != nullptr; handle = s_handles.Next(handle)) {
        handle->m_scene->DestroyDeviceObjects();
    }
}

}  // namespace

extern "C" unsigned long long FUN_14244fd20();
// translated: FUN_14244fd20
extern "C" unsigned long long FUN_14244f9b0();
// translated: FUN_14244f9b0

// ref: FUN_142442f50
void DayNightInitialize() {
    GxAddDeviceCreatedCallback(&OnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
    // the colour grading's device callbacks and its texture (0x1167fd)
    FUN_14244fd20();
}

// ref: FUN_1424422d0
void DayNightShutdown() {
    GxRemoveDeviceCreatedCallback(&OnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
    for (DAYNIGHT* handle = s_handles.Head(); handle != nullptr; handle = s_handles.Next(handle)) {
        handle->m_scene->DestroyDeviceObjects();
    }
    FUN_14244f9b0();
    // the area overrides' Light copies (DAT_1447aa310), still the image's pool
    reinterpret_cast<CDataAllocator*>(0x1447aa310ull)->Clear(".?AVLightRec_C@@", -2);
}

// ---- the handle ----------------------------------------------------------------------------

// ref: FUN_142442100
DAYNIGHT* DayNightCreate(CWorldScene* scene, CWorldView* view) {
    auto handle = static_cast<DAYNIGHT*>(
        SMemAlloc(sizeof(DAYNIGHT), 1, s_dayNightFile, SStrHashFnv1a(s_dayNightFile), 0x66, 0));
    handle->m_0c = 0;
    handle->m_link.m_prevlink = nullptr;
    handle->m_link.m_next = nullptr;
    void* memory = SMemAlloc(sizeof(DNScene), 1, s_dayNightFile, SStrHashFnv1a(s_dayNightFile), 199, 0);
    handle->m_scene = new (memory) DNScene(scene, view);
    handle->m_08 = 0;
    s_handles.LinkToTail(handle);
    handle->m_scene->CreateDeviceObjects();
    return handle;
}

// ref: FUN_142442370
void DayNightDestroy(DAYNIGHT* dayNight) {
    dayNight->m_link.Unlink();
    if (dayNight != nullptr) {
        if (dayNight->m_scene != nullptr) {
            delete dayNight->m_scene;
            dayNight->m_scene = nullptr;
        }
        dayNight->m_link.Unlink();
        operator delete(dayNight);
    }
}

// ref: FUN_142442fe0
void DayNightSetMap(DAYNIGHT* dayNight, int32_t mapId) {
    dayNight->m_scene->SetMap(mapId);
}

// ref: FUN_142443100
void DayNightSetArea(DAYNIGHT* dayNight, int32_t areaId, int32_t index) {
    DayNightGatherLights(&dayNight->m_scene->m_areaLights[index], areaId);
}

// ref: FUN_142453550
void DayNightGatherLights(blz::vector<int32_t, s_dnTagInt>* lights, int32_t areaId) {
    WowDB2Lookup* lookup = g_lightDB.m_lookup;
    int64_t count = 1;
    if (lookup != nullptr) {
        DB2LOOKUPITER it;
        WowDB2LookupBegin(lookup, &it);
        while (it.m_index != -2) {
            auto row = static_cast<const LIGHTROW*>(it.m_record);
            float length = row->m_position[0] * row->m_position[0] + row->m_position[1] * row->m_position[1]
                + row->m_position[2] * row->m_position[2];
            if (row->m_continent == areaId && POSITION_EPSILON <= std::fabs(length)) {
                count++;
            }
            lookup->Next(&it);
        }
    }
    bool global = false;
    lights->resize(static_cast<uint64_t>(count));
    if (lookup != nullptr) {
        DB2LOOKUPITER it;
        WowDB2LookupBegin(lookup, &it);
        if (it.m_index != -2) {
            uint64_t slot = 1;
            do {
                auto row = static_cast<const LIGHTROW*>(it.m_record);
                if (row->m_continent == areaId) {
                    float length = row->m_position[0] * row->m_position[0]
                        + row->m_position[1] * row->m_position[1] + row->m_position[2] * row->m_position[2];
                    if (POSITION_EPSILON <= std::fabs(length)) {
                        (*lights)[slot++] = static_cast<int32_t>(lookup->Id(&it));
                    } else {
                        global = true;
                        (*lights)[0] = static_cast<int32_t>(lookup->Id(&it));
                    }
                }
                lookup->Next(&it);
            } while (it.m_index != -2);
            if (global) {
                return;
            }
        }
    }
    (*lights)[0] = 1;
}

// ref: FUN_142443120
void DayNightOnCelestialBodyPatched(uint32_t a1, uint32_t id, uint32_t a3, void* a4, void* a5) {
    (void)a1;
    (void)a3;
    (void)a4;
    (void)a5;
    for (DAYNIGHT* handle = s_handles.Head(); handle != nullptr; handle = s_handles.Next(handle)) {
        (void)id;
        FROST_UNIMPLEMENTED();  // FUN_142455b50: the fourth planet from the CelestialBody row
    }
}

// Once a frame: every handle's colour grading lets go of the textures it stopped using.
// ref: FUN_142443190
void DayNightReleaseUnused() {
    for (DAYNIGHT* handle = s_handles.Head(); handle != nullptr; handle = s_handles.Next(handle)) {
        handle->m_scene->m_colorGrading->ReleaseUnused();
    }
}

// The view each frame: DayNight's update (FUN_142443200 -> FUN_14245f9d0), next to port.
// ref: FUN_142443200
void DayNightSetView(DAYNIGHT* dayNight, CWorldView* view, const float* position, const float* clip,
    const float* viewMatrix) {
    (void)dayNight;
    (void)view;
    (void)position;
    (void)clip;
    (void)viewMatrix;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_142443dc0
void DayNightUpdate(DAYNIGHT* dayNight, const float* position) {
    dayNight->m_scene->Update(position);
}

// ref: FUN_142443d80
void DayNightCompute(DAYNIGHT* dayNight, int32_t timeMs, float elapsed, float timeSeconds, bool alive, void* env,
    const float* viewMatrix, bool colorGrading, bool zoneLightsSingly) {
    dayNight->m_scene->Compute(timeMs, elapsed, timeSeconds, alive, env, viewMatrix, colorGrading, zoneLightsSingly);
}

// ref: FUN_142443db0
void DayNightPrepareFrame(DAYNIGHT* dayNight, void* env, bool colorGrading) {
    dayNight->m_scene->PrepareFrame(env, colorGrading);
}

// ref: FUN_14245e340
void DNScene::PrepareFrame(void* env, bool colorGrading) {
    if (colorGrading) {
        FROST_UNIMPLEMENTED();  // FUN_1424506d0 x2: the colour grading blends (env's +0x5bc picks the pair)
    }
    (void)env;
    if (this->m_light.Field<uint32_t>(0x15f0) != 0) {
        this->m_clouds.Upload(this);
    }
}

// ref: FUN_142443370
void DayNightSetCamera(DAYNIGHT* dayNight, CWorldView* view, const float* position, const float* heading,
    const float* target, int32_t liquid, float liquidDepth, float farClip) {
    dayNight->m_scene->SetCamera(view, position, heading, target, liquid, liquidDepth, farClip);
}

// ref: FUN_1424438e0
void DayNightSetWeather(DAYNIGHT* dayNight, float strength) {
    // FUN_141265e50, a folded field store.
    DNField<float>(dayNight->m_scene, 0x54) = strength;
}

// ref: FUN_1424438d0
void DayNightSetIndoors(DAYNIGHT* dayNight, bool indoors) {
    // FUN_141f39e00, a folded field store.
    DNField<uint8_t>(dayNight->m_scene, 4) = indoors;
}

// ref: FUN_142443530
void DayNightSetWater(DAYNIGHT* dayNight, bool underwater, const void* fog, bool waterFog, float depth) {
    dayNight->m_scene->SetWater(underwater, fog, waterFog, depth);
}

// ref: FUN_142443210
void DayNightBeginFrame(DAYNIGHT* dayNight) {
    dayNight->m_scene->BeginFrame();
}

// ref: FUN_142442cd0
uint32_t DayNightGetTime(DAYNIGHT* dayNight) {
    return DNField<uint32_t>(dayNight->m_scene, 8);
}

// The reference's body is the shared empty one (FUN_1402ca290): it writes nothing, so the
// caller keeps its defaults.
// ref: FUN_142442e70
bool DayNightGetLightTransform(DAYNIGHT* dayNight, float* value, float* scale, float* offset) {
    (void)dayNight;
    (void)value;
    (void)scale;
    (void)offset;
    return false;
}

// ref: FUN_142442810
uint8_t* DayNightLightBlock(DAYNIGHT* dayNight, const void* env) {
    size_t offset = DNField<uint8_t>(const_cast<void*>(env), 0x5bc) != 0 ? 0x15a4 : 0x1580;
    return reinterpret_cast<uint8_t*>(dayNight->m_scene) + offset;
}

// ref: FUN_142442c30
uint32_t* DayNightWaterColor(DAYNIGHT* dayNight, uint32_t* out, const void* env) {
    DNScene* scene = dayNight->m_scene;
    if (scene->m_1ec8 != 0) {
        *out = DNField<uint32_t>(scene, 0x1ee8);
        return out;
    }
    *out = DNField<uint32_t>(scene, DNField<uint8_t>(const_cast<void*>(env), 0x5bc) != 0 ? 0xf9c : 0xaac);
    return out;
}

// ref: FUN_142442740
MODELFOG* DayNightFog(DAYNIGHT* dayNight, uint32_t index) {
    DNScene* scene = dayNight->m_scene;
    if (index < DNField<uint64_t>(scene, 0x1a0)) {
        return DNField<MODELFOG*>(scene, 0x198) + index;
    }
    return &DNField<MODELFOG>(scene, index != 0 ? 0x574 : 0xf8);
}

// ref: FUN_142442500
MODELFOG* DayNightFogF8(DAYNIGHT* dayNight) {
    return &DNField<MODELFOG>(dayNight->m_scene, 0xf8);
}

// ref: FUN_142442780
MODELFOG* DayNightFog1B0(DAYNIGHT* dayNight) {
    return &DNField<MODELFOG>(dayNight->m_scene, 0x1b0);
}

// ref: FUN_142442790
MODELFOG* DayNightFog250(DAYNIGHT* dayNight) {
    return &DNField<MODELFOG>(dayNight->m_scene, 0x250);
}

// ref: FUN_1424427f0
MODELFOG* DayNightFog2F4(DAYNIGHT* dayNight) {
    return &DNField<MODELFOG>(dayNight->m_scene, 0x2f4);
}

// ref: FUN_142442800
MODELFOG* DayNightFog394(DAYNIGHT* dayNight) {
    return &DNField<MODELFOG>(dayNight->m_scene, 0x394);
}

// ref: FUN_142442a50
uint32_t DayNightFogCount(DAYNIGHT* dayNight, const void* env, bool single) {
    auto bytes = static_cast<const uint8_t*>(env);
    DNScene* scene = dayNight->m_scene;
    if (bytes[0x5be] != 0 && bytes[0x5bc] == 0 && scene->m_1601 == 0 && !single) {
        auto count = static_cast<uint32_t>(DNField<uint64_t>(scene, 0x1a0));
        return count <= 1 ? 1 : count;
    }
    return 1;
}

// ref: FUN_142459720
void DNScene::SetCamera(CWorldView* view, const float* position, const float* heading, const float* target,
    int32_t liquid, float liquidDepth, float farClip) {
    this->m_m2Scene->m_owner = this->m_worldScene;
    this->m_m2Scene->m_030 = view;
    std::memcpy(&DNField<float>(this, 0x14), position, 12);
    std::memcpy(&DNField<float>(this, 0x2c), heading, 12);
    VectorNormalize(&DNField<float>(this, 0x2c));
    std::memcpy(&DNField<float>(this, 0x20), target, 12);
    DNField<int32_t>(this, 0x38) = liquid;
    DNField<float>(this, 0x3c) = liquidDepth;
    DNField<float>(this, 0x44) = farClip;
}

// ref: FUN_14245a8f0
void DNScene::SetWater(bool underwater, const void* fog, bool waterFog, float depth) {
    DNField<uint8_t>(this, 0xa30) = underwater;
    std::memcpy(&DNField<uint8_t>(this, 0xa34), fog, 0x30);
    DNField<uint8_t>(this, 0xa64) = waterFog;
    DNField<float>(this, 0xa68) = underwater ? depth : 0.0f;
}

namespace {

// A planet's texture is in (FUN_142455fd0), and its load priority (FUN_142456b60).
// ref: FUN_142455fd0
bool PlanetLoaded(DNPLANET* planet) {
    auto texture = planet->Field<HTEXTURE>(0x10);
    if (texture == nullptr) {
        return true;
    }
    return TextureIsReady(texture);
}

// ref: FUN_142456b60
void PlanetSetPriority(DNPLANET* planet, int32_t priority) {
    auto texture = planet->Field<HTEXTURE>(0x10);
    if (texture != nullptr) {
        TextureSetPriority(texture, priority);
    }
}

// The sky model is loaded and its textures settled (FUN_142458580), and its priority.
// ref: FUN_142458580
bool SkyModelLoaded(CM2Model* model) {
    auto base = reinterpret_cast<CM2BaseModel*>(model);
    if (base == nullptr) {
        return false;
    }
    return (base->m_flags & 1) != 0 && base->AreTexturesSettled();
}

// ref: FUN_142458750
void SkyModelSetPriority(CM2Model* model, int32_t priority) {
    if (model != nullptr) {
        // FUN_1411e7a00
        reinterpret_cast<CM2BaseModel*>(model)->SetPriority(priority, true);
    }
}

// An override sky's model is loaded (FUN_142460f00), and its priority (FUN_1424611c0).
// ref: FUN_142460f00
bool OverrideSkyLoaded(uint8_t* sky) {
    auto model = DNField<CM2BaseModel*>(sky, 0x38);
    return model != nullptr && (model->m_flags & 1) != 0 && model->AreTexturesSettled();
}

// ref: FUN_1424611c0
void OverrideSkySetPriority(uint8_t* sky, int32_t priority) {
    DNField<CM2BaseModel*>(sky, 0x38)->SetPriority(priority, true);
}

template <class Fn>
void EachOverrideSky(DNScene* scene, Fn fn) {
    void** table = scene->m_overrideTable;
    if (table == nullptr) {
        return;
    }
    for (uint64_t bucket = 0; bucket < scene->m_overrideBuckets; bucket++) {
        for (auto node = static_cast<uint8_t*>(table[bucket]); node != nullptr; node = DNField<uint8_t*>(node, 0)) {
            if (!fn(DNField<uint8_t*>(node, 0x30))) {
                return;
            }
        }
    }
}

}  // namespace

// ref: FUN_14245f870
bool DNScene::SkyLoaded() {
    bool loaded = true;
    EachOverrideSky(this, [&loaded](uint8_t* sky) {
        if (!OverrideSkyLoaded(sky)) {
            loaded = false;
        }
        return loaded;
    });
    if (!loaded) {
        return false;
    }
    return PlanetLoaded(&this->m_planets[0]) && PlanetLoaded(&this->m_planets[1]) && PlanetLoaded(&this->m_planets[2])
        && this->m_sunGlare.TextureLoaded() != 0 && this->m_moonGlare.TextureLoaded() != 0
        && SkyModelLoaded(this->m_skyModel);
}

// ref: FUN_14245fb50
void DNScene::SetSkyPriority(int32_t priority) {
    EachOverrideSky(this, [priority](uint8_t* sky) {
        OverrideSkySetPriority(sky, priority);
        return true;
    });
    PlanetSetPriority(&this->m_planets[0], priority);
    PlanetSetPriority(&this->m_planets[1], priority);
    PlanetSetPriority(&this->m_planets[2], priority);
    PlanetSetPriority(&this->m_planets[3], priority);
    this->m_sunGlare.SetTexturePriority(priority);
    this->m_moonGlare.SetTexturePriority(priority);
    SkyModelSetPriority(this->m_skyModel, priority);
}

// ref: FUN_14245fb20
void DNScene::BeginFrame() {
    if (!this->SkyLoaded()) {
        this->SetSkyPriority(0);
    }
}

// ref: FUN_142442fb0
bool DayNightIsDaytime(DAYNIGHT* dayNight) {
    float time = DNField<float>(dayNight->m_scene, 0xc);
    return 0.22083335f <= time && time < 0.875f;     // DAT_1429798d4, DAT_1429798d8
}

// ref: FUN_142443520
void DayNightSetOverride(DAYNIGHT* dayNight, bool set) {
    dayNight->m_scene->m_1601 = set;
}

// ref: FUN_1424434f0
void DayNightComputeSunDirection(DAYNIGHT* dayNight, const float* viewMatrix) {
    dayNight->m_scene->ComputeSunDirection(viewMatrix);
}
