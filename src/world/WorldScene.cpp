#include "world/WorldScene.hpp"

#include "client/RenderSettings.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "daynight/DayNight.hpp"
#include "gx/Gx.hpp"
#include "model2/M2Scene.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "threadpool/ThreadPool.hpp"
#include "ui/FrameTime.hpp"
#include "world/ParticulateVolumeManager.hpp"
#include "world/WorldClip.hpp"
#include "world/WorldFade.hpp"
#include "world/WorldMap.hpp"
#include "world/WorldSceneCallbacks.hpp"
#include "world/WorldSceneRender.hpp"
#include "world/WorldStubs.hpp"
#include "world/WorldView.hpp"
#include <algorithm>
#include <cfloat>
#include <cstring>
#include <new>

const char s_worldViewPtrTag[] = ".PEAVCWorldView@@";
const char s_worldSceneRenderPtrTag[] = ".PEAVCWorldSceneRender@@";
const char s_intTag[] = ".H";

namespace {

const char s_sceneFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WorldClient2\\WorldScene.cpp";
const char s_callbackTag[] =
    ".?AV?$TSList@VCRenderCallback@@V?$TSGetExplicitLink@VCRenderCallback@@@@$07@@";
const char s_verticesTag[] = ".?AV?$TSFixedArray@UGxVertexPNCT2@@$07@@";

CWorldScene* s_currentScene;  // 0x1430ad2f8

// DAT_142d011e8: every scene, linked at the tail as it is made.
TSList<CWorldScene, TSGetLink<CWorldScene>, 8> s_scenes;  // untied: the world's device callbacks (FUN_1413cae30) still walk the image's list

// A map's far clips are held to this while its flag 2 is set (DAT_142738a68).
constexpr float MAP_FAR_CLIP_CAP = 4400.0f;
// The third far clip's caps (DAT_142734b10, DAT_142738a64) and how far under the first it
// is kept (DAT_14273852c).
constexpr float FAR_CLIP3_CAP = 1300.0f;
constexpr float FAR_CLIP3_FLAG_CAP = 777.0f;
constexpr float FAR_CLIP3_MARGIN = 66.666664f;

void* SceneAlloc(size_t size, int32_t line) {
    return SMemAlloc(size, 0x27, s_sceneFile, SStrHashFnv1a(s_sceneFile), line, 0);
}

// The world scene's frame-buffer flush (LAB_1413c50c0): hands its borrowed target back.
void OnFrameBufferFlush(void* param) {
    auto scene = static_cast<CWorldScene*>(param);
    if (scene->m_temp.m_refs > 0) {
        FrameBufferTempRelease(&scene->m_temp);
    }
}

// The far clip CVar against a map's limit and scale (the first lines of FUN_1413c2bc0 and
// its neighbours).
float MapScaledClip(float clip, CWorldMap* map, float scale) {
    if (map != nullptr) {
        if ((map->Flags() & 2) != 0 && MAP_FAR_CLIP_CAP <= clip) {
            clip = MAP_FAR_CLIP_CAP;
        }
        clip = clip * scale;
    }
    return clip;
}

}  // namespace

// ---- the scene's globals -------------------------------------------------------------------

// ref: FUN_141357060
void* WorldSceneGetEnv(CWorldScene* scene) {
    return &scene->m_farClip;
}

// ref: FUN_1413cb140
CWorldScene* WorldSceneGetCurrent() {
    return s_currentScene;
}

// ref: FUN_1413cb310
void WorldSceneSetCurrent(CWorldScene* scene) {
    s_currentScene = scene;
}

// ---- construction --------------------------------------------------------------------------

// ref: FUN_1413c17f0
CWorldScene::CWorldScene(uint32_t flags, int32_t type) {
    this->m_link.m_prevlink = nullptr;
    this->m_link.m_next = nullptr;
    for (THREADPOOLGROUP*& group : this->m_groups) {
        group = nullptr;
    }
    this->m_060 = 0;
    this->m_farClip = 946.0f;
    this->m_nearClip = 0.2f;
    std::memset(this->m_0c8, 0, sizeof(this->m_0c8));
    // m_fogs and m_fog: MODELFOG's constructor (the reference inlines it for the eight).
    this->m_678 = 0;
    this->m_67c[0] = 0;
    this->m_67c[1] = 1;
    this->m_67c[2] = 1;
    this->m_67c[3] = 1;
    this->m_680 = 0;
    this->m_6a0 = 1;
    this->m_6a8 = 0;
    this->m_vertices = nullptr;
    this->m_6b8 = 0;
    this->m_6c0 = 0;
    this->m_6c8 = 0;
    this->m_6d0 = 0;
    this->m_6e8 = 0;
    this->m_6f0 = 0;
    this->m_position[0] = 0.0f;
    this->m_position[1] = 0.0f;
    this->m_position[2] = 0.0f;
    for (int32_t i = 0; i < 16; i++) {
        this->m_718[i] = (i % 5) == 0 ? 1.0f : 0.0f;
        this->m_758[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    }
    for (CRenderCallbackList& list : this->m_callbacks) {
        list.m_linkoffset = offsetof(CRenderCallback, m_link);
    }
    for (auto& row : this->m_828) {
        row[0] = 0.0f;
        row[1] = 0.0f;
        row[2] = 1.0f;
        row[3] = 0.0f;
    }
    std::memset(this->m_888, 0, sizeof(this->m_888));
    this->m_lodPosition[0] = 0.0f;
    this->m_lodPosition[1] = 0.0f;
    this->m_lodPosition[2] = 0.0f;
    this->m_frameTimeIndex = 0;
    this->m_frameTimeSum = 0;
    std::memset(this->m_frameTimes, 0, sizeof(this->m_frameTimes));
    this->m_lodScale = 1.0f;
    this->m_9f4 = 0;
    this->m_9f8 = 0;
    this->m_9fc[0] = FLT_MAX;
    this->m_9fc[1] = FLT_MAX;
    this->m_9fc[2] = FLT_MAX;
    this->m_a08 = -1;
    std::memset(this->m_a0c, 0, sizeof(this->m_a0c));
    this->m_a2c[0] = FLT_MAX;
    this->m_a2c[1] = FLT_MAX;
    this->m_a2c[2] = FLT_MAX;
    this->m_a38 = -1;
    std::memset(this->m_a3c, 0, sizeof(this->m_a3c));
    this->m_a70 = 0;
    this->m_a78 = 0;
    // m_lightA88 and the three below: CM2Lighting's constructor.
    std::memset(&this->m_lighting, 0, 0xa0);
    this->m_lighting.m_maxLights = 8;
    this->m_lighting.m_fogIndex = 0;
    this->m_lighting.m_plane[0] = 0.0f;
    this->m_lighting.m_plane[1] = 0.0f;
    this->m_lighting.m_plane[2] = 1.0f;
    this->m_lighting.m_plane[3] = 0.0f;
    for (WORLDSCENELIGHT& light : this->m_lights) {
        std::memset(light.m_color, 0, sizeof(light.m_color));
        light.m_source = nullptr;
        light.m_08 = 0;
        light.m_fade = 0.0f;
        light.m_flags = static_cast<uint8_t>((light.m_flags & 0xfc) | 4);
    }
    for (auto& entry : this->m_e80) {
        uint8_t* bytes = entry;
        auto u64 = [bytes](size_t offset, uint64_t value) { std::memcpy(bytes + offset, &value, 8); };
        auto u32 = [bytes](size_t offset, uint32_t value) { std::memcpy(bytes + offset, &value, 4); };
        u64(0x20, 0);
        u64(0x28, 0x3f800000);
        u64(0x30, 0);
        u32(0x38, 0);
        u64(0x44, 0);
        u32(0x4c, 0);
        u64(0x54, 0);
        u64(0x5c, 0);
        u64(0x64, 0);
        u64(0x80, 0);
        u32(0x88, 0);
        for (size_t offset = 0x90; offset < 0xc0; offset += 8) {
            u64(offset, 0);
        }
        u64(0xd0, 0);
        u32(0xd8, 0);
    }
    std::memset(this->m_1900, 0, sizeof(this->m_1900));
    this->m_1940 = 0;
    this->m_1948 = 0;
    this->m_screenRect[0] = 0.0f;
    this->m_screenRect[1] = 0.0f;
    this->m_screenRect[2] = 0.0f;
    this->m_screenRect[3] = 0.0f;
    this->m_196c = 1.0f;
    std::memset(this->m_1970, 0, 0x20);
    this->m_busy = 0;
    this->m_19a4 = 8;
    this->m_19a8 = nullptr;
    this->m_19b0 = nullptr;
    this->m_19b8 = nullptr;
    this->m_19c0 = nullptr;
    this->m_19c8 = nullptr;
    this->m_19d0 = nullptr;
    FrameBufferTempClear(&this->m_temp);

    this->m_maps[0] = nullptr;
    this->m_maps[1] = nullptr;
    this->m_a54 = static_cast<uint8_t>(this->m_a54 & 0xfc);
    this->m_mapIndex = 0;
    this->m_dayNight = nullptr;
    this->m_030 = nullptr;
    this->m_038 = nullptr;
    this->m_040 = nullptr;
    this->m_particulates = nullptr;
    this->m_type = type;
    this->m_a64 = 0.0f;
    this->m_a68 = 0.0f;
    this->m_view = nullptr;
    this->m_view = this->CreateView(flags);
    if ((flags & 8) != 0 && WorldSceneGetCurrent() == nullptr) {
        WorldSceneSetCurrent(this);
    }

    if (type == 6) {
        this->m_1950 = WorldScene1950Create(SceneAlloc(0x68, 0x130));
        this->m_6d8 = WorldScene6d8Create(SceneAlloc(0x7878, 0x131));
        this->m_6e0 = WorldScene6e0Create(SceneAlloc(0x38, 0x132));
    } else {
        this->m_1950 = nullptr;
        this->m_6d8 = nullptr;
        this->m_6e0 = nullptr;
    }
    this->m_700 = nullptr;
    this->m_a80 = nullptr;

    this->m_m2Scene = M2CacheCreateScene(type, this, this->m_view, &this->m_farClip);
    M2SceneSetProjectCallback(this->m_m2Scene, WorldSceneProjectModel);
    M2SceneSetProjectCheckCallback(this->m_m2Scene, WorldSceneProjectCheck);
    M2SceneSetTraceCallback(this->m_m2Scene, WorldSceneTrace, this);
    M2SceneSetPhysicsCallback(this->m_m2Scene, WorldScenePhysics, this);
    M2SceneSetWindCallback(this->m_m2Scene, WorldSceneSampleWind);
    M2SceneSetGroundCallback(this->m_m2Scene, WorldSceneGroundHeight);

    void* memory = SceneAlloc(sizeof(CParticulateVolumeManager), 0x151);
    this->m_particulates = new (memory) CParticulateVolumeManager();
    this->m_groups[0] = ThreadPoolGroupCreate(2);
    this->m_groups[1] = ThreadPoolGroupCreate(2);
    this->m_groups[2] = ThreadPoolGroupCreate(2);
    this->m_groups[3] = ThreadPoolGroupCreate(2);
    memory = SceneAlloc(sizeof(CWorldFade), 0x160);
    this->m_058 = new (memory) CWorldFade();

    this->ClearCallbacks();
    FrameBufferCacheAddFlushCallback(OnFrameBufferFlush, this);
    GxAddDeviceDestroyedCallback(WorldSceneOnDeviceDestroyed, this);
    RenderSettingsAddHandler(WorldSceneOnRenderSettings, this);

    this->m_8e8 = 0;
    this->m_farClips[0] = 705.0f;
    this->m_farClips[1] = 705.0f;
    this->m_farClips[2] = 705.0f;
    this->m_a6c = 400.0f;
    this->m_frame = 1;
    this->m_1968 = 0;
    s_scenes.LinkToTail(this);
    this->m_refCount = 0;
}

// The constructor's inlined TSList clear, list by list.
void CWorldScene::ClearCallbacks() {
    for (CRenderCallbackList& list : this->m_callbacks) {
        while (CRenderCallback* callback = list.Head()) {
            callback->m_link.Unlink();
            SMemFree(callback, s_callbackTag, -2, 0);
        }
    }
}

// ref: FUN_1413c24e0
CWorldScene::~CWorldScene() {
    if (WorldSceneGetCurrent() == this) {
        WorldSceneSetCurrent(nullptr);
    }
    for (uint32_t slot = 0; slot < 2; slot++) {
        this->UnloadMap(slot);
    }
    if (this->m_040 != nullptr) {
        SkySceneDestroy(this->m_040);
        operator delete(this->m_040);
        this->m_040 = nullptr;
    }
    if (this->m_m2Scene != nullptr) {
        M2SceneRelease(this->m_m2Scene);
        this->m_m2Scene = nullptr;
    }
    WorldSceneFreeFadeouts();
    if (this->m_particulates != nullptr) {
        delete this->m_particulates;
        this->m_particulates = nullptr;
    }
    if (this->m_6d8 != nullptr) {
        WorldScene6d8Destroy(this->m_6d8);
        operator delete(this->m_6d8);
        this->m_6d8 = nullptr;
    }
    if (this->m_6e0 != nullptr) {
        WorldScene6e0Destroy(this->m_6e0);
        operator delete(this->m_6e0);
        this->m_6e0 = nullptr;
    }
    for (THREADPOOLGROUP*& group : this->m_groups) {
        if (group != nullptr) {
            ThreadPoolGroupRelease(group);
            group = nullptr;
        }
    }
    if (this->m_058 != nullptr) {
        delete static_cast<CWorldFade*>(this->m_058);
        this->m_058 = nullptr;
    }
    if (this->m_a80 != nullptr) {
        WorldSceneDeleteA80(this->m_a80);
        this->m_a80 = nullptr;
    }
    if (this->m_1950 != nullptr) {
        WorldScene1950Release();
        WorldScene1950Destroy(this->m_1950);
        operator delete(this->m_1950);
    }
    if (this->m_temp.m_refs > 0) {
        FrameBufferTempRelease(&this->m_temp);
    }
    FrameBufferCacheRemoveFlushCallback(OnFrameBufferFlush, this);
    GxRemoveDeviceDestroyedCallback(WorldSceneOnDeviceDestroyed, this);
    if (this->m_19b8 != nullptr) {
        WorldCaptureDestroy(this->m_19b8);
        void* buffer = this->m_19c0;
        this->m_19b8 = nullptr;
        this->m_19c0 = nullptr;
        if (buffer != nullptr) {
            GxDeviceRelease0f8(&this->m_19c0, buffer);
        }
        this->m_19c8 = nullptr;
    }
    RenderSettingsRemoveHandler(WorldSceneOnRenderSettings, this);
    if (this->m_view != nullptr) {
        uint64_t index = 0;
        while (index < this->m_views.size() && this->m_views[index] != this->m_view) {
            index++;
        }
        this->m_views.erase(index, index + 1);
        delete this->m_view;
        this->m_view = nullptr;
    }
    if (this->m_19c0 != nullptr) {
        GxDeviceRelease0f8(&this->m_19c0, this->m_19c0);
    }
    if (this->m_19b0 != nullptr) {
        GxDeviceRelease0e0(&this->m_19b0, this->m_19b0);
    }
    if (this->m_19a8 != nullptr) {
        GxDeviceRelease0e0(&this->m_19a8, this->m_19a8);
    }
    // m_lightDE0..m_lightA88 unlink in their destructors; m_callbacks clear in theirs.
    if (this->m_vertices != nullptr) {
        SMemFree(this->m_vertices, s_verticesTag, -2, 0);
    }
    // m_688, m_renders and m_views free their storage as blz::vector does.
    this->m_link.Unlink();
}

// ref: FUN_1413c29d0
void CWorldScene::AddRef() {
    this->m_refCount++;
}

// ref: FUN_1413c5bb0
int32_t CWorldScene::Release() {
    int32_t count = --this->m_refCount;
    if (count == 0) {
        delete this;
        return 0;
    }
    return count;
}

// ref: FUN_1413c36c0
CWorldView* CWorldScene::CreateView(uint32_t flags) {
    void* memory = SceneAlloc(sizeof(CWorldView), 0x2c7);
    auto view = new (memory) CWorldView(this, nullptr, (flags & 1) != 0 ? 4 : 1);
    bool first = this->m_views.size() == 0;
    this->m_views.push_back(view);
    uint32_t features = first ? 0x7fffa428 : 0x7fffa028;
    if ((flags & 1) == 0) {
        features = first ? 0x7fffe428 : 0x7fffe028;
    }
    if ((flags & 2) != 0) {
        features &= 0xffefffff;
    }
    if ((flags & 4) != 0) {
        features &= 0xf7ffffff;
    }
    view->m_flags |= features;
    return view;
}

// ref: FUN_1413c35c0
void CWorldScene::LoadMap(uint32_t slot, int32_t mapId, bool a4, uint8_t a5, int32_t a6, uint8_t a7) {
    void* memory = SceneAlloc(sizeof(CWorldMap), 0x242);
    this->m_maps[slot] = new (memory) CWorldMap(this, slot);
    WorldMapLink(this->m_maps[slot]);
    this->m_maps[slot]->SetCameraPosition(this->m_position);
    CWorldMap* map = this->m_maps[slot];
    if (!a4) {
        map->LoadNone(mapId, slot != 0);
    } else {
        map->Load(mapId, a5, a6, a7);
    }
}

// ref: FUN_1413c3a40
void CWorldScene::UnloadMap(uint32_t slot) {
    CWorldView* view = this->m_view;
    if (view != nullptr) {
        for (WORLDVIEWGATHER& gather : view->m_gathered) {
            gather.m_00 = nullptr;
            gather.m_08 = 0;
            gather.m_1c = 0;
            gather.m_20 = 0;
        }
    }
    WorldSceneFreeObjectFadeouts();
    if (this->m_6d8 != nullptr) {
        WorldScene6d8Unload(this->m_6d8);
    }
    if (this->m_6e0 != nullptr) {
        WorldScene6e0Unload(this->m_6e0);
    }
    if (this->m_maps[slot] != nullptr) {
        this->m_maps[slot]->Unload();
        delete this->m_maps[slot];
        this->m_maps[slot] = nullptr;
    }
    if (this->m_maps[0] == nullptr && this->m_maps[1] == nullptr) {
        if (this->m_dayNight != nullptr) {
            DayNightDestroy(this->m_dayNight);
            this->m_dayNight = nullptr;
        }
        if (this->m_030 != nullptr) {
            WorldScene030Destroy(this->m_030);
            operator delete(this->m_030);
            this->m_030 = nullptr;
        }
        if (this->m_038 != nullptr) {
            MapWeatherDestroy(this->m_038);
            operator delete(this->m_038);
            this->m_038 = nullptr;
        }
    }
    if (this->m_maps[0] == nullptr && this->m_maps[1] == nullptr) {
        this->m_farClips[0] = 705.0f;
        this->m_farClips[1] = 705.0f;
        this->m_farClips[2] = 705.0f;
        this->m_a6c = 400.0f;
    }
    if (this->m_040 != nullptr) {
        SkySceneUnload(this->m_040);
    }
}

// ref: FUN_1413c88f0
void CWorldScene::SetCameraPosition(const float* position) {
    CWorldMap* map = this->m_maps[this->m_mapIndex];
    float clip = MapScaledClip(WorldGetFarClipCVar(), map, map != nullptr ? map->FarClipScale() : 1.0f);
    float farClip = this->m_farClips[0];
    if (this->m_farClips[0] <= clip) {
        farClip = clip;
    }
    if (farClip <= this->m_farClips[1]) {
        farClip = this->m_farClips[1];
    }
    if (farClip <= this->m_farClips[2]) {
        farClip = this->m_farClips[2];
    }
    this->m_farClip = WorldClampFarClip(farClip);
    WorldGetNearClipCVar();
    this->m_nearClip = WorldGetNearClip();
    this->m_position[0] = position[0];
    this->m_position[1] = position[1];
    this->m_position[2] = position[2];
}

// ref: FUN_1413c2bc0
float CWorldScene::GetFarClip(CWorldMap* map) {
    float clip = MapScaledClip(WorldGetFarClipCVar(), map, map != nullptr ? map->FarClipScale() : 1.0f);
    float farClip = this->m_farClips[0];
    if (this->m_farClips[0] <= clip) {
        farClip = clip;
    }
    if (farClip <= this->m_farClips[1]) {
        farClip = this->m_farClips[1];
    }
    if (farClip <= this->m_farClips[2]) {
        farClip = this->m_farClips[2];
    }
    return WorldClampFarClip(farClip);
}

// ref: FUN_1413c2c20
float CWorldScene::GetFarClip2(CWorldMap* map) {
    float clip = MapScaledClip(WorldGetFarClip2CVar(), map, map != nullptr ? map->FarClip2Scale() : 1.0f);
    float farClip = this->m_farClips[0];
    if (this->m_farClips[0] <= clip) {
        farClip = clip;
    }
    if (farClip <= this->m_farClips[1]) {
        farClip = this->m_farClips[1];
    }
    return WorldClampFarClip2(farClip);
}

// ref: FUN_1413c2c80
float CWorldScene::GetFarClip3(CWorldMap* map) {
    float farClip = this->GetFarClip(map);
    float clip = WorldGetFarClip3CVar();
    if (map != nullptr) {
        if ((map->Flags() & 2) == 0) {
            auto area = static_cast<uint8_t*>(map->Field<void*>(0x28));
            if ((area == nullptr || (*reinterpret_cast<uint32_t*>(area + 0xdc) & 0x100) == 0)
                && FAR_CLIP3_CAP <= clip) {
                clip = FAR_CLIP3_CAP;
            }
        } else if (FAR_CLIP3_FLAG_CAP <= clip) {
            clip = FAR_CLIP3_FLAG_CAP;
        }
        clip = clip * map->FarClip2Scale();
    }
    float third = this->m_a6c;
    if (this->m_a6c <= clip) {
        third = clip;
    }
    if (farClip <= third) {
        third = farClip - FAR_CLIP3_MARGIN;
    }
    return WorldClampFarClip3(third);
}

// ref: FUN_1413c2d60
float CWorldScene::GetNearClip() {
    WorldGetNearClipCVar();
    return WorldGetNearClip();
}

// ref: FUN_1413c31f0
void CWorldScene::CreateMapObjects() {
    if (this->m_dayNight != nullptr) {
        DayNightDestroy(this->m_dayNight);
        this->m_dayNight = nullptr;
    }
    if (this->m_030 != nullptr) {
        WorldScene030Destroy(this->m_030);
        operator delete(this->m_030);
        this->m_030 = nullptr;
    }
    if (this->m_038 != nullptr) {
        MapWeatherDestroy(this->m_038);
        operator delete(this->m_038);
        this->m_038 = nullptr;
    }
    if (this->m_dayNight == nullptr) {
        this->m_dayNight = DayNightCreate(this, this->m_view);
        CWorldMap* map = this->m_maps[0];
        DayNightSetMap(this->m_dayNight, map->MapId());
        for (int32_t i = 0; i < 0x18; i++) {
            auto area = static_cast<uint8_t*>(map->Area(i));
            if (area != nullptr) {
                DayNightSetArea(this->m_dayNight, *reinterpret_cast<int32_t*>(area + 0xfc), i);
            }
        }
        DayNightSetView(this->m_dayNight, this->m_view, map->Position(), &this->m_farClip, this->m_view->m_matrices[4]);
    }
    if (this->m_038 == nullptr) {
        this->m_038 = MapWeatherCreate(SceneAlloc(0x1378, 0x2a3), this);
    }
    if (this->m_030 == nullptr) {
        this->m_030 = WorldScene030Create(SceneAlloc(0xfa50, 0x2a9), 0.027777778f, 30.0f, 0x2d771);
    }
}

// ref: FUN_14134e6c0
float WorldSceneFarClip(CWorldScene* scene) {
    if (scene != nullptr) {
        return scene->GetFarClip(scene->m_maps[scene->m_mapIndex]);
    }
    return WorldGetFarClipCVar();
}

// ref: FUN_14134e860
float WorldSceneFarClip3(CWorldScene* scene) {
    if (scene != nullptr) {
        return scene->GetFarClip3(scene->m_maps[scene->m_mapIndex]);
    }
    return WorldGetFarClip3CVar();
}


// The Map row's flags word `index` (Map.Flags[3], FUN_14134cac0).
// ref: FUN_14134cac0
uint32_t MapRowFlags(const uint8_t* fields, uint32_t index) {
    return DB2FieldArrayU32(fields, 0x15, 0x18, index, &g_mapDB);
}

namespace {

// The pool job the handle runs the light on (LAB_1413c8a60).
void UpdateLightJob(void* storage) {
    auto view = *reinterpret_cast<CWorldView**>(static_cast<uint8_t*>(storage) + 8);
    CWorldScene* scene = view->m_scene;
    if (scene->m_dayNight != nullptr) {
        scene->UpdateLight(view, scene->m_dayNight);
    }
}

}  // namespace

// ref: FUN_1413c8990
void CWorldScene::UpdateDayNight(CWorldView* view) {
    if (this->m_dayNight == nullptr) {
        return;
    }
    DayNightBeginFrame(this->m_dayNight);
    if (this->m_dayNight->m_0c != 0) {
        ThreadPoolGroupSetCount(this->m_groups[1], 1);
        blz::function_storage task;
        task.m_invoke = reinterpret_cast<void*>(&UpdateLightJob);
        task.m_manager = blz::function_trivial_manager;
        *reinterpret_cast<void**>(task.m_storage) = nullptr;
        *reinterpret_cast<CWorldView**>(task.m_storage + 8) = view;
        ThreadPoolSubmit(1, &task, 2, this->m_groups[1]);
        if (task.m_manager != blz::function_trivial_manager) {
            task.m_manager(task.m_storage, nullptr, 0);
        }
        return;
    }
    CWorldScene* scene = view->m_scene;
    if (scene->m_dayNight != nullptr) {
        scene->UpdateLight(view, scene->m_dayNight);
    }
}

// ref: FUN_1413c8aa0
void CWorldScene::UpdateLight(CWorldView* view, DAYNIGHT* dayNight) {
    uint32_t skip = view->m_flags & 0x1004;
    float liquidDepth = 0.0f;
    uint32_t liquid = view->GetLiquid(&liquidDepth);
    CWorldMap* map = this->m_maps[0];
    float farClip = view->m_cameraSet.m_24;
    if (map != nullptr) {
        auto area = static_cast<uint8_t*>(map->Area(0));
        if (area == nullptr || (*reinterpret_cast<uint32_t*>(area + 0xdc) & 0x100) == 0) {
            farClip = std::min(farClip, FAR_CLIP3_CAP);
        }
    }
    DayNightSetCamera(dayNight, view, view->m_camera.m_position, reinterpret_cast<float*>(view->m_3f0 + 0x70),
        view->m_camera.m_target, static_cast<int32_t>(liquid), liquidDepth, farClip);
    float weather = 0.0f;
    float weatherScale = 1.0f;
    if (this->m_038 != nullptr) {
        weather = *reinterpret_cast<float*>(static_cast<uint8_t*>(this->m_038) + 0x1168);
        weatherScale = *reinterpret_cast<float*>(static_cast<uint8_t*>(this->m_038) + 0x1184);
    }
    DayNightSetWeather(dayNight, weatherScale * weather);
    uint8_t waterFog[0x30] = {};
    uint8_t covered = 0;
    float depth = FLT_MAX;
    int32_t water = view->QueryWater(waterFog, &covered, &depth);
    if (skip == 0 && map != nullptr) {
        bool local = false;
        auto row = static_cast<const uint8_t*>(g_mapDB.GetRecord(static_cast<uint32_t>(map->MapId()), false, &local));
        if (row != nullptr) {
            DayNightSetIndoors(dayNight, ((MapRowFlags(row + 0x18, 2) >> 1) & 1) != 0);
        }
    }
    this->m_a68 = depth;
    this->m_a64 = depth;
    if (water != 1) {
        this->m_a64 = 0.0f;
        if (depth == FLT_MAX) {
            this->m_a68 = 0.0f;
        }
    }
    DayNightSetWater(dayNight, water == 1, waterFog, covered != 0, depth);
    ThreadPoolGroupWait(this->m_groups[0]);
    if (skip == 0) {
        bool zoneLightsSingly = true;
        if (map != nullptr) {
            bool local = false;
            auto row = static_cast<const uint8_t*>(g_mapDB.GetRecord(static_cast<uint32_t>(map->MapId()), false, &local));
            if (row != nullptr) {
                zoneLightsSingly = ((MapRowFlags(row + 0x18, 1) >> 27) & 1) != 0;
            }
        }
        bool colorGrading = (view->m_flags & 0x40) != 0;
        bool alive = WorldPlayerAlive();
        float seconds = FrameTime_GetTimeSeconds();
        float elapsed = FrameTime_GetElapsedSeconds();
        auto ms = static_cast<int32_t>(FrameTime_GetTime());
        DayNightCompute(dayNight, ms, elapsed, seconds, alive, &this->m_farClip, view->m_matrices[4], colorGrading,
            zoneLightsSingly);
    }
    uint8_t* light = DayNightLightBlock(dayNight, &this->m_farClip);
    this->m_lightA88.SetLightType(CM2Lighting::LIGHT_DIRECTIONAL);
    this->m_lightA88.SetEnabled(1);
    this->m_lightA88.SetDirection(reinterpret_cast<float*>(light));
    constexpr float BYTE = 1.0f / 255.0f;   // DAT_142632528
    auto color = [](float* out, const uint8_t* bgr) {
        out[0] = static_cast<float>(bgr[2]) * BYTE;
        out[1] = static_cast<float>(bgr[1]) * BYTE;
        out[2] = static_cast<float>(bgr[0]) * BYTE;
    };
    color(this->m_lightA88.m_ambient[0], light + 0x10);
    color(this->m_lightA88.m_ambient[1], light[0x1a] == 0 && light[0x19] == 0 && light[0x18] == 0 ? light + 0x10 : light + 0x18);
    color(this->m_lightA88.m_ambient[2], light[0x16] == 0 && light[0x15] == 0 && light[0x14] == 0 ? light + 0x10 : light + 0x14);
    color(this->m_lightA88.m_diffuse, light + 0xc);
    uint32_t waterColor = 0;
    color(reinterpret_cast<float*>(this->m_lightA88.m_pad64),
        reinterpret_cast<const uint8_t*>(DayNightWaterColor(dayNight, &waterColor, &this->m_farClip)));
    this->m_fogs[0] = *DayNightFog2F4(dayNight);
    this->m_fogs[1] = *DayNightFog(dayNight, 0);
    this->m_fogs[2] = *DayNightFog(dayNight, 1);
    this->m_fogs[3] = *DayNightFog(dayNight, 2);
    this->m_fogs[4] = *DayNightFog(dayNight, 3);
    this->m_fogs[5] = *DayNightFog1B0(dayNight);
    this->m_fogs[6] = *DayNightFog394(dayNight);
    this->m_fogs[7] = *DayNightFog250(dayNight);
    this->m_678 = DayNightFogCount(dayNight, &this->m_farClip, false);
    this->m_fog = *DayNightFogF8(dayNight);
}

// Once a frame: every scene's vertex array starts the frame empty.
// ref: FUN_1413cad70
void WorldScenesResetVertices() {
    for (CWorldScene* scene = s_scenes.Head(); scene != nullptr; scene = s_scenes.Next(scene)) {
        // FUN_141438520 on the array at +0x6a0: its count
        scene->m_6a8 = 0;
    }
}

// Whether any scene has the model file ready.
// ref: FUN_1413cb320
bool WorldScenesModelReady(const char* name, uint8_t priority) {
    for (CWorldScene* scene = s_scenes.Head(); scene != nullptr; scene = s_scenes.Next(scene)) {
        if (scene->m_m2Scene->ModelFileReady(name, priority)) {
            return true;
        }
    }
    return false;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1413cae30=GenForwardMissing")
// ---- end of link fallbacks ----
