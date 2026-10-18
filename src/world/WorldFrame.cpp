#include "world/WorldFrame.hpp"

#include "base/Coordinate.hpp"
#include "base/Matrix.hpp"
#include "blz/Function.hpp"
#include "client/GraphicsSettings.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/GxWindow.hpp"
#include "client/RenderSettings.hpp"
#include "client/Resample.hpp"
#include "client/Startup.hpp"
#include "console/Var.hpp"
#include "daynight/DayNight.hpp"
#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "model2/CM2Lighting.hpp"
#include "model2/M2Camera.hpp"
#include "model2/M2Scene.hpp"
#include "shadereffect/ShaderEffectPipeline.hpp"
#include "storm/Memory.hpp"
#include "ui/CSimpleModel.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "threadpool/ThreadPool.hpp"
#include "ui/FrameTime.hpp"
#include "util/Fnv.hpp"
#include "util/Unimplemented.hpp"
#include "world/SceneOcclusion.hpp"
#include "world/WorldClip.hpp"
#include "world/WorldMap.hpp"
#include "world/WorldQueries.hpp"
#include "world/WorldScene.hpp"
#include "world/WorldView.hpp"
#include <cmath>
#include <cstring>
#include <new>
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

// WoW/Source/WorldClient2: a scene's frame. World.cpp's entry points the model frames call
// (create, animate, update, the camera view), the scene's animate pass (WorldScene.cpp) and
// the view's camera latch (WorldView.cpp).

bool Sub14134F980();

namespace {

const char s_worldFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WorldClient2\\World.cpp";

// DAT_1430aba60: cleared when a scene loads its map here and by the world's startup.
uint32_t s_1430aba60;  // DAT_1430aba60

// The world's player position (DAT_1430abb28), what the particulates follow.
float s_playerPosition[3];
// The camera position the world last set (FUN_141352f70, not ported): wind volumes and weather
// are judged from it.
float s_cameraPosition[3];          // DAT_1430abb08

// The world's graphics quality and the raid one (DAT_142cfdb6c, DAT_142cfdb70), 6 until the
// world's startup reads the CVars (FUN_141354f80).
uint32_t s_graphicsQuality = 6;
uint32_t s_raidGraphicsQuality = 6;

// The frame-time budget by graphics quality, in microseconds (DAT_142d00bf8).
const uint32_t s_frameBudget[10] = {1700000, 2000000, 2600000, 3100000, 4100000, 5000000, 5500000, 6000000, 7700000,
    11000000};

// The world's LOD scale (DAT_142d011b4).
float s_lodScale = 1.0f;

// The renderer's feature switches (DAT_142ccbc00): set by the client's option setup.
uint8_t s_features[8] = {1, 1, 1, 0, 0, 0, 0, 0};

int32_t s_frameRect[4];             // DAT_1430aba78: the frame's screen rectangle
uint32_t s_loadSortMask = 0x3f;     // DAT_142cfdb68: what the map loader sorts, 0 under worldLoadSort 0

// The last frame's view arguments (FUN_141352f70 keeps its 13th to 17th), cleared by the
// world's startup; the last three again once the frame is done.
float s_frameViewScale;             // DAT_1430ab9e4
uint32_t s_frameView14;             // DAT_1430aba18
uint32_t s_frameView16;             // DAT_1430aba1c
uint64_t s_frameView15;             // DAT_1430aba20
uint32_t s_frameView17;             // DAT_1430aba28

}  // namespace

// ref: FUN_14134ec00
float* WorldGetCameraPosition() {
    return s_cameraPosition;
}

// ref: FUN_141365480
int32_t* WorldGetFrameRect() {
    return s_frameRect;
}

// DAT_1430ab790: DayNight's zone lights and light run on a pool job.
uint8_t g_dayNightOnJob;

// ---- small world state ---------------------------------------------------------------------

// ref: FUN_141354f80
void WorldReadGraphicsQuality() {
    CVar* quality = CVarLookup(t2util::ConstFnvHashStringI("graphicsQuality"));
    if (quality != nullptr) {
        s_graphicsQuality = static_cast<uint32_t>(quality->m_intValue);
    }
    CVar* raid = CVarLookup(t2util::ConstFnvHashStringI("RAIDgraphicsQuality"));
    if (raid != nullptr) {
        s_raidGraphicsQuality = static_cast<uint32_t>(raid->m_intValue);
    }
}

// ---- the world's startup -------------------------------------------------------------------

extern "C" {
unsigned long long FUN_1413cb1c0();
unsigned long long FUN_142526ef0();
unsigned long long FUN_14139ca60();
unsigned long long FUN_14136db90();
unsigned long long FUN_141396800();
unsigned long long FUN_14134dfc0(unsigned long long);
unsigned long long FUN_14134dfe0(unsigned long long);
unsigned long long FUN_141355300();
unsigned long long FUN_1413afb30();
unsigned long long FUN_1413cf1d0();
unsigned long long FUN_14139aa40();
unsigned long long FUN_14139a4f0(unsigned long long);
}
// translated: FUN_1413cb1c0
// translated: FUN_142526ef0
// translated: FUN_14139ca60
// translated: FUN_14136db90
// translated: FUN_141396800
// translated: FUN_14134dfc0
// translated: FUN_14134dfe0
// translated: FUN_141355300
// translated: FUN_1413afb30
// translated: FUN_1413cf1d0
// translated: FUN_14139aa40
// translated: FUN_14139a4f0

namespace {

template <class F>
void WorldGuarded(uint64_t address, F&& body) {
    __try {
        body();
    } __except (GenFault(address, GetExceptionInformation())) {
    }
}

// Every texture is loaded (FUN_141355300): the world's caches hear of it.
void OnAllTexturesLoaded(void* storage) {
    (void)storage;
    WorldGuarded(0x141355300, [] { FUN_141355300(); });
}

}  // namespace

// The world's subsystems set up in order: the scenes, DayNight, the map, the particulates and
// the liquids, the device callbacks, the all-textures-loaded hook, the map objects, and the
// world's telemetry stream.
// ref: FUN_14134ec50
void WorldInitialize() {
    WorldGuarded(0x1413cb1c0, [] { FUN_1413cb1c0(); });
    DayNightInitialize();
    WorldGuarded(0x142526ef0, [] { FUN_142526ef0(); });
    WorldGuarded(0x14139ca60, [] { FUN_14139ca60(); });
    WorldGuarded(0x14136db90, [] { FUN_14136db90(); });
    WorldGuarded(0x141396800, [] { FUN_141396800(); });
    s_frameViewScale = 0.0f;
    s_frameView14 = 0;
    s_frameView15 = 0;
    s_frameView16 = 0;
    s_frameView17 = 0;
    GxAddDeviceCreatedCallback(reinterpret_cast<GXDEVICECALLBACK>(&FUN_14134dfc0), nullptr);
    GxAddDeviceDestroyedCallback(reinterpret_cast<GXDEVICECALLBACK>(&FUN_14134dfe0), nullptr);
    blz::function_storage loaded;
    loaded.m_invoke = reinterpret_cast<void*>(&OnAllTexturesLoaded);
    loaded.m_manager = &blz::function_trivial_manager;
    TextureAddAllLoadedCallback(&loaded, reinterpret_cast<uint64_t>(&WorldInitialize));
    s_1430aba60 = 0;
    WorldGuarded(0x1413afb30, [] { FUN_1413afb30(); });
    WorldGuarded(0x1413cf1d0, [] { FUN_1413cf1d0(); });
    WorldGuarded(0x14139aa40, [] { FUN_14139aa40(); });
    WorldGuarded(0x14139a4f0, [] { FUN_14139a4f0(0); });
    WorldGuarded(0x14139a4f0, [] { FUN_14139a4f0(1); });
    // the world's stream (DAT_142e91d10), at its default flags
    TelemetryStreamEnable(reinterpret_cast<void*>(0x142e91d10ull), 4);
}

extern "C" unsigned long long FUN_141383630();
extern "C" unsigned long long FUN_14136e890();
extern "C" unsigned long long FUN_141196d60(unsigned long long);
extern "C" unsigned long long FUN_141398500();
extern "C" unsigned long long FUN_141399160();
extern "C" unsigned long long FUN_141386d50(unsigned long long);
// translated: FUN_141383630
// translated: FUN_14136e890
// translated: FUN_141196d60
// translated: FUN_141398500
// translated: FUN_141399160
// translated: FUN_141386d50
// Once a frame from the client loop (`inWorld` in the game state): the world's caches let go of
// what went unused (colour gradings, map areas, scene vertices, models, textures), the load
// sort is read, and the map objects are updated.
// ref: FUN_141350fd0
void WorldFrameUpdate(bool inWorld) {
    if (!GxDevExists()) {
        int32_t* rect = WorldGetFrameRect();
        rect[2] = 0x7fff;
        rect[3] = 0x7fff;
    }
    DayNightReleaseUnused();
    WorldMapsReleaseUnusedAreas(inWorld);
    WorldScenesResetVertices();
    FUN_141383630();
    FUN_14136e890();
    FUN_141196d60(FrameTime_GetTime());
    // two calls to an empty function (0x1402112e0) sit here
    FUN_141398500();
    FUN_141399160();
    CVar* loadSort = CVarLookup(t2util::FnvHashStringI("orldLoadSort", 0xaf63ea4c86020456ull));  // "worldLoadSort"
    s_loadSortMask = 0x3f;
    if (loadSort != nullptr && loadSort->m_intValue == 0) {
        s_loadSortMask = 0;
    }
    WorldReadGraphicsQuality();
    struct {
        uint32_t m_elapsed;
        float m_distance;
    } update = {FrameTime_GetElapsed(), 1111.1111f};    // 0x448ae38e
    FUN_141386d50(reinterpret_cast<uintptr_t>(&update));
    BspCacheUpdate();
}

// ref: FUN_14134e7e0
uint32_t WorldGraphicsQuality() {
    return GraphicsSettingsMode() == 1 ? s_raidGraphicsQuality : s_graphicsQuality;
}

// ref: FUN_14135cf20
uint32_t WorldFrameBudget() {
    uint32_t quality = WorldGraphicsQuality();
    return s_frameBudget[quality < 9 ? quality : 9];
}

// ref: FUN_14134e950
float* WorldPlayerPosition() {
    return s_playerPosition;
}

// ref: FUN_1413c0930
float WorldLodScaleMinimum() {
    return 0.5f;    // DAT_14261a988
}

// ref: FUN_1413c1020
void WorldSetLodScale(float scale) {
    if (scale < 0.5f) {             // DAT_14261a988
        s_lodScale = 0.5f;
        return;
    }
    if (2.0f <= scale) {            // DAT_142632534
        scale = 2.0f;
    }
    s_lodScale = scale;
}

// ref: FUN_140309ec0
bool RenderFeatureEnabled(int32_t feature) {
    return s_features[feature] != 0;
}

// The water detail setting (FUN_142526ee0).
// ref: FUN_142526ee0
uint8_t WorldReflectionMode() {
    return g_renderSettings.m_reflectionMode;
}

// ref: FUN_142526ee0
uint8_t WorldWaterDetail() {
    return g_renderSettings.m_waterDetail;
}

// ---- the stubs the animate pass reaches ----------------------------------------------------

// The global jobs a view that asks for them waits on (DAT_1447ce6a8), and their count.
// ref: FUN_14252ce30
void WorldWaitGlobalJobs() {
    FROST_UNIMPLEMENTED();  // FUN_14235a5e0 on DAT_1447ce6a8, then DAT_142d5e7fc += 3
}

// The frame's screen rectangle reset (DAT_1430aba78, DAT_1430aba80).
// ref: FUN_141365b20
void WorldResetFrameRect() {
    s_frameRect[0] = 0;
    s_frameRect[1] = 0;
    s_frameRect[2] = 0;
    s_frameRect[3] = 0;
    if (!GxDevExists()) {
        s_frameRect[2] = 0x7fff;
        s_frameRect[3] = 0x7fff;
    }
}

// ref: FUN_1413c9380
void WorldSceneUpdateFadeouts(CWorldScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413c9f40
void WorldSceneUpdateDoodadFades(CWorldScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1414291f0
void WorldMapSetPosition(CWorldMap* map, const float* position) {
    (void)map;
    (void)position;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14142a270
void WorldMapUpdateAreas(CWorldMap* map) {
    (void)map;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141428f70
void WorldMapUpdateChunks(CWorldMap* map) {
    (void)map;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413b2b30
void WorldMapAnimate(CWorldMap* map, void* weather) {
    (void)map;
    (void)weather;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141425d80
void WorldScene030Update(void* object, CWorldView* view) {
    (void)object;
    (void)view;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141392ff0
void MapWeatherUpdate(void* weather, CWorldScene* scene, const void* camera, bool outdoors, bool flag) {
    (void)weather;
    (void)scene;
    (void)camera;
    (void)outdoors;
    (void)flag;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413975e0
void ParticulateVolumesUpdate(void* particulates, const void* camera, const float* player, bool daytime, float depth) {
    (void)particulates;
    (void)camera;
    (void)player;
    (void)daytime;
    (void)depth;
    FROST_UNIMPLEMENTED();
}


// ref: FUN_142527370
void WorldRunFrameCallbacks() {
    FROST_UNIMPLEMENTED();  // DAT_1447ce201: the two callback lists at DAT_1447ce218, DAT_1447ce238
}

// ref: FUN_141370460
void WorldFadeUpdate(void* fade, float elapsed, const void* camera) {
    (void)fade;
    (void)elapsed;
    (void)camera;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413793d0
void WorldUpdateObjectFades(CWorldScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();  // FUN_141379400, then FUN_1413cad10 while DAT_1430abef8
}

// ref: FUN_1413ca0b0
void WorldSceneUpdateFog(CWorldScene* scene, CWorldView* view) {
    (void)scene;
    (void)view;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413c9760
void WorldSceneFinishFrame(CWorldScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();
}

// ---- the view -------------------------------------------------------------------------------

// ref: FUN_1413da870
void CWorldView::SetOcclusionCulling(bool enabled) {
    (void)enabled;
    FROST_UNIMPLEMENTED();  // the view's 0x308 occluder object (FUN_1413a6a40) at +0x238
}

namespace {

// The LOD factor of a field of view (FUN_1413c08e0): the reciprocal of the view's width at
// unit distance, kept within [0.01, 4].
// ref: FUN_1413c08e0
float WorldViewFovLodFactor(float fov) {
    if (fov <= 0.001f) {  // DAT_142618f58
        fov = 0.001f;
    }
    float half = std::tan(fov * 0.5f);  // FUN_1424154b0
    float width = half + half;
    if (half + half <= 0.01f) {  // DAT_142636334
        width = 0.01f;
    }
    if (4.0f <= width) {  // DAT_1426c975c
        width = 4.0f;
    }
    return 1.0f / width;
}

// The LOD factor of a target size (FUN_1413c0bc0): its scaled height over 1080, kept within
// [1, 2].
// ref: FUN_1413c0bc0
float WorldViewSizeLodFactor(const int32_t* size) {
    int32_t scaled[2];
    RenderSettingsScaledSize(scaled, size);
    float factor = static_cast<float>(scaled[1]) * 0.00092592591f;  // DAT_142727208
    if (factor <= 1.0f) {
        factor = 1.0f;
    }
    if (2.0f <= factor) {  // DAT_142632534
        factor = 2.0f;
    }
    return factor;
}

}  // namespace

// The frame's camera from the one the caller set: the lists emptied, the camera block, the
// scene's camera position, the view direction, the target size, the frame index, how the
// frame is drawn (+0x6b4: 0 forward, 1 with a depth prepass, 2 the same multisampled) and the
// LOD factors.
// ref: FUN_1413dddd0
void CWorldView::UpdateCamera(const int32_t* lod) {
    FROST_UNIMPLEMENTED();  // the cull's per-frame reset (FUN_141444be0)
    WorldViewListClear(&this->m_lists[0]);
    WorldViewListClear(&this->m_lists[1]);
    this->m_camera = this->m_cameraSet;
    std::memcpy(this->m_3f0 + 0x64, this->m_scene->m_position, 12);
    float* direction = reinterpret_cast<float*>(this->m_3f0 + 0x70);
    float dx = this->m_camera.m_target[0] - this->m_camera.m_position[0];
    float dy = this->m_camera.m_target[1] - this->m_camera.m_position[1];
    float dz = this->m_camera.m_target[2] - this->m_camera.m_position[2];
    float scale = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
    direction[0] = dx * scale;
    direction[1] = dy * scale;
    direction[2] = dz * scale;
    this->m_targetSize[0] = lod[0];
    this->m_targetSize[1] = lod[1];
    // The frustum's corners from the view and projection (FUN_1413c4440 over FUN_14068cef0), and
    // from them the bounds (+0x31c), the chunk range (+0x30c), the planes (FUN_1413463d0) and
    // the depth ratio (+0x47c).
    FROST_UNIMPLEMENTED();
    CWorldView* main = this->m_scene->m_view;
    if (main == this) {
        this->m_index += static_cast<uint32_t>(this->m_scene->m_renders.size());
    } else {
        this->m_index = main->m_index + this->m_renderIndex;
    }
    if ((this->m_flags & 2) == 0 && (this->m_flags >> 9 & 1) != 0) {
        this->m_6b4 = (g_renderSettings.m_msaaSamples != 0) + 1;  // DAT_142cfbfb4
    } else {
        this->m_6b4 = 0;
    }
    this->m_scale[0] = WorldViewFovLodFactor(this->m_camera.m_fov);
    this->m_scale[1] = WorldViewSizeLodFactor(this->m_targetSize);
}

// ref: FUN_1413de3a0
void CWorldView::BuildFrustum() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413de760
void CWorldView::Cull() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413da610
void CWorldView::UpdateOccluders(bool occluders) {
    (void)occluders;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413dddb0
void CWorldView::SetOcclusion(bool enabled) {
    SceneOcclusionSetEnabled(&this->m_occlusion, enabled);
}

// ref: FUN_1413dea50
bool CWorldView::CullsSkipped() {
    return ((reinterpret_cast<uint8_t*>(this->m_cull)[0x161e8] >> 5) & 1) != 0;
}

// ref: FUN_1413de160
void CWorldView::SetCamera(const float* position, const float* target, float fov, float aspect) {
    CWorldScene* scene = this->m_scene;
    CWorldMap* map = scene->m_maps[scene->m_mapIndex];
    this->m_cameraSet.m_nearClip = scene->GetNearClip();
    this->m_cameraSet.m_farClip = scene->GetFarClip(map);
    this->m_cameraSet.m_24 = scene->GetFarClip3(map);
    float farClip2 = scene->GetFarClip2(map);
    this->m_cameraSet.m_fov = fov;
    this->m_cameraSet.m_aspect = aspect;
    this->m_cameraSet.m_farClip2 = farClip2;
    std::memcpy(this->m_cameraSet.m_position, position, 12);
    std::memcpy(this->m_cameraSet.m_target, target, 12);
}

// ref: FUN_1413c59b0
void CWorldView::LatchCamera() {
    float view[16];
    std::memcpy(view, this->m_matrices[4], sizeof(view));
    float move[3] = {-this->m_camera.m_position[0], -this->m_camera.m_position[1], -this->m_camera.m_position[2]};
    MatrixTranslate(view, move);
    std::memcpy(this->m_matrices[1], view, sizeof(view));
    std::memcpy(this->m_matrices[2], this->m_matrices[4], sizeof(view));
    std::memcpy(this->m_matrices[3], this->m_matrices[5], sizeof(view));
    this->m_cameraLast = this->m_cameraSet;
}

// ---- the scene ------------------------------------------------------------------------------

namespace {

// The pool job the handle runs DayNight's zone lights on (LAB_1413c8a80).
void UpdateZoneLightsJob(void* storage) {
    auto view = *reinterpret_cast<CWorldView**>(static_cast<uint8_t*>(storage) + 8);
    DayNightUpdate(view->m_scene->m_dayNight, view->m_camera.m_position);
}

}  // namespace

// ref: FUN_1413c80a0
void CWorldScene::Animate(CWorldView* view, const int32_t* lod, float elapsed, uint32_t flags) {
    CM2Scene* m2Scene = this->m_m2Scene;
    if ((view->m_flags & 0x4000) != 0) {
        WorldWaitGlobalJobs();
    }
    WorldResetFrameRect();
    uint32_t viewFlags = view->m_flags | 0x10;
    viewFlags = (flags & 2) != 0 ? viewFlags | 0x40 : viewFlags & ~0x40u;
    view->m_flags = viewFlags;
    if (ShaderEffectOutlinesOn() && (flags & 1) != 0) {
        view->m_flags |= 0x200;
    } else {
        view->m_flags &= ~0x200u;
    }
    uint32_t frameFlags = view->m_flags;
    bool shadowed = ((frameFlags >> 9) & 1) != 0 && 0 < g_renderSettings.m_shadowMode;
    view->SetOcclusionCulling(shadowed);
    view->UpdateCamera(lod);
    if (Sub14134F980()) {
        SceneOcclusionSetEnabled(&view->m_occlusion, false);
    } else {
        view->SetOcclusion(this->m_type == 6);
    }
    SceneOcclusionStage(&view->m_occlusion, view, 0);
    SceneOcclusionStage(&view->m_occlusion, view, 1);
    WorldSceneUpdateFadeouts(this);
    WorldSceneUpdateDoodadFades(this);
    for (CWorldMap* map : {this->m_maps[0], this->m_maps[1]}) {
        if (map != nullptr) {
            WorldMapSetPosition(map, this->m_position);
            WorldMapUpdateAreas(map);
        }
    }
    SceneOcclusionStage(&view->m_occlusion, view, 2);
    if (this->m_dayNight != nullptr) {
        this->m_dayNight->m_0c = g_dayNightOnJob;
        if (this->m_dayNight->m_0c == 0) {
            DayNightUpdate(view->m_scene->m_dayNight, view->m_camera.m_position);
        } else {
            ThreadPoolGroupSetCount(this->m_groups[0], 1);
            blz::function_storage task;
            task.m_invoke = reinterpret_cast<void*>(&UpdateZoneLightsJob);
            task.m_manager = blz::function_trivial_manager;
            *reinterpret_cast<void**>(task.m_storage) = nullptr;
            *reinterpret_cast<CWorldView**>(task.m_storage + 8) = view;
            ThreadPoolSubmit(1, &task, 2, this->m_groups[0]);
            if (task.m_manager != blz::function_trivial_manager) {
                task.m_manager(task.m_storage, nullptr, 0);
            }
        }
    }
    view->m_flags &= ~0x100u;
    view->BuildFrustum();
    view->Cull();
    this->m_67c[0] = view->m_690 != 0;
    if (view->m_gathered[0].m_00 == nullptr && view->m_gathered[1].m_00 == nullptr) {
        view->m_flags |= 0x100;
    }
    this->m_a70 = view->m_300;
    this->m_a78 = view->m_308;
    this->UpdateDayNight(view);
    struct {
        uint64_t m_zero[2];
        CWorldScene* m_scene;
        CWorldView* m_view;
    } frame = {{0, 0}, this, view};
    CRenderCallbackList& callbacks = this->m_callbacks[5];
    for (CRenderCallback* callback = callbacks.Head(); callback != nullptr; callback = callbacks.Next(callback)) {
        callback->m_function(callback->m_param, callback->m_data, &frame);
    }
    view->UpdateOccluders(this->m_67c[0] != 0);
    SceneOcclusionStage(&view->m_occlusion, view, 3);
    for (CWorldMap* map : {this->m_maps[0], this->m_maps[1]}) {
        if (map != nullptr) {
            WorldMapUpdateChunks(map);
            WorldMapAnimate(map, this->m_038);
        }
    }
    SceneOcclusionStage(&view->m_occlusion, view, 4);
    if (this->m_030 != nullptr && view->m_690 != 0) {
        WorldScene030Update(this->m_030, view);
    }
    if (this->m_038 != nullptr) {
        bool skipped = view->CullsSkipped();
        bool outdoors = view->m_gathered[0].m_00 == nullptr && view->m_gathered[1].m_00 == nullptr;
        MapWeatherUpdate(this->m_038, this, &view->m_camera, outdoors, skipped);
    }
    if (this->m_particulates != nullptr) {
        bool daytime = this->m_dayNight == nullptr ? true : DayNightIsDaytime(this->m_dayNight);
        ParticulateVolumesUpdate(this->m_particulates, &view->m_camera, WorldPlayerPosition(), daytime, this->m_a68);
    }
    MapWindUpdate();
    WorldRunFrameCallbacks();
    m2Scene->m_d738 = WorldWaterDetail() != 0;
    uint32_t frameTime = GxDeviceFrameTime();
    this->m_frameTimeSum -= this->m_frameTimes[this->m_frameTimeIndex];
    this->m_frameTimes[this->m_frameTimeIndex] = frameTime;
    this->m_frameTimeSum += frameTime;
    this->m_frameTimeIndex++;
    if (this->m_frameTimeIndex == 0x3c) {
        this->m_frameTimeIndex = 0;
    }
    CVar* dynamicLod = CVarLookup(t2util::ConstFnvHashStringI("DynamicLod"));
    if (dynamicLod != nullptr && dynamicLod->m_intValue == 0) {
        this->m_lodScale = 1.0f;
        std::memcpy(this->m_lodPosition, this->m_position, 12);
    } else {
        float budget = static_cast<float>(static_cast<uint64_t>(WorldFrameBudget())) * 1e-04f;       // DAT_1426c6f80
        float average = static_cast<float>(static_cast<uint64_t>(this->m_frameTimeSum / 0x3c)) * 1e-04f;
        float minimum = WorldLodScaleMinimum();
        float dy = this->m_position[1] - this->m_lodPosition[1];
        float dx = this->m_position[0] - this->m_lodPosition[0];
        float dz = this->m_position[2] - this->m_lodPosition[2];
        if (1e-06f < dy * dy + dx * dx + dz * dz) {                 // DAT_14261a95c
            float step = FrameTime_GetElapsedSeconds();
            float scale;
            if (budget < average) {
                scale = this->m_lodScale - step * 0.05f;           // DAT_14261a974
            } else {
                scale = step * 0.05f + this->m_lodScale;
            }
            if (minimum <= scale) {
                minimum = scale < 1.0f ? scale : 1.0f;
            }
            this->m_lodScale = minimum;
            std::memcpy(this->m_lodPosition, this->m_position, 12);
        }
    }
    WorldSetLodScale(this->m_lodScale);
    if (this->m_058 != nullptr && static_cast<uint8_t*>(this->m_058)[0x170] != 0) {
        WorldFadeUpdate(this->m_058, elapsed, &view->m_camera);
    }
    WorldUpdateObjectFades(this);
    if (this->m_dayNight != nullptr) {
        ThreadPoolGroupWait(this->m_groups[1]);
        DayNightPrepareFrame(this->m_dayNight, &this->m_farClip, (view->m_flags & 0x40) != 0);
        if (this->m_maps[0] != nullptr) {
            DayNightSetOverride(this->m_dayNight, (this->m_maps[0]->Flags() & 1) != 0);
        }
    }
    WorldSceneUpdateFog(this, view);
    CWorldMap* current = this->m_maps[this->m_mapIndex];
    float center[4] = {0.0f, 0.0f, 0.0f, 32.0f};
    if (current != nullptr) {
        std::memcpy(center, current->Position(), 12);
    }
    CM2ModelLighting* lighting = &this->m_lighting;
    M2ModelLightingBegin(lighting, nullptr, center);
    M2ModelLightingAdd(lighting, &this->m_lightA88);
    uint32_t lights = 1;
    if (this->m_67c[0] != 0 || (current != nullptr && (current->Flags() & 1) != 0)) {
        lights = 7;
    }
    this->m_lighting.m_fogIndex = lights;
    if (m2Scene != nullptr) {
        if ((view->m_flags & 0x1000) == 0) {
            m2Scene->m_flags |= 0x41;
        }
        m2Scene->m_flags = shadowed ? m2Scene->m_flags | 0x10 : m2Scene->m_flags & ~0x10u;
        m2Scene->m_flags = RenderFeatureEnabled(1) ? m2Scene->m_flags | 0x100 : m2Scene->m_flags & ~0x100u;
        m2Scene->m_flags = RenderFeatureEnabled(2) ? m2Scene->m_flags | 0x400 : m2Scene->m_flags & ~0x400u;
        if (((frameFlags >> 9) & 1) != 0) {
            view->m_flags |= 0x800;
            m2Scene->m_flags = (m2Scene->m_flags | 0x20) & ~0x200u;
        } else {
            view->m_flags &= ~0x800u;
            m2Scene->m_flags = (m2Scene->m_flags & ~0x20u) | 0x200;
        }
    }
    WorldSceneFinishFrame(this);
}

// ref: FUN_1413c5980
void CWorldScene::LatchView(CWorldView* view) {
    view->LatchCamera();
    if (this->m_m2Scene != nullptr) {
        this->m_m2Scene->LatchView();
    }
}

// ---- World.cpp's entry points --------------------------------------------------------------

// ref: FUN_14134dd50
CWorldScene* WorldSceneCreate(uint32_t flags, int32_t type, bool light) {
    void* memory = SMemAlloc(sizeof(CWorldScene), 0x27, s_worldFile, SStrHashFnv1a(s_worldFile), 0x1c2, 0);
    auto scene = new (memory) CWorldScene(flags, type);
    scene->AddRef();
    if (light) {
        float origin[3] = {0.0f, 0.0f, 0.0f};
        s_1430aba60 = 0;
        uint8_t phase = GxFrameSetPhase(0);
        scene->SetCameraPosition(origin);
        scene->LoadMap(0, -1, false, 0, -1, 1);
        GxFrameSetPhase(phase);
    }
    return scene;
}

int32_t WorldSceneRelease(CWorldScene* scene) {
    return scene->Release();
}

// ref: FUN_14134e8a0
uint32_t* WorldSceneGetParams(CWorldScene* scene) {
    return &scene->m_view->m_flags;
}

// ref: FUN_1413571e0
void WorldSceneParamsSetFlags(uint32_t* params, uint32_t mask, bool set) {
    if (params == nullptr) {
        return;
    }
    if (set) {
        *params |= mask;
        return;
    }
    *params &= ~mask;
}

// ref: FUN_1413c3460
CM2Model* WorldSceneCreateModelByName(CWorldScene* scene, const char* name, uint32_t flags, int32_t priority,
    int32_t lod) {
    return scene->m_m2Scene->CreateModel(name, flags, static_cast<uint8_t>(priority), static_cast<uint8_t>(lod));
}

// ref: FUN_1413c3450
CM2Model* WorldSceneCreateModelById(CWorldScene* scene, int32_t fileDataId, uint32_t flags, int32_t priority,
    int32_t lod) {
    return scene->m_m2Scene->CreateModel(fileDataId, flags, static_cast<uint8_t>(priority), static_cast<uint8_t>(lod));
}

// The scene's frame: the GPU work flushed, the camera placed, the scene animated, and with
// `latch` the viewport applied and the view latched for the draw.
// ref: FUN_141352ea0
void WorldSceneAnimateFrame(CWorldScene* scene, CWorldView* view, const float* position, const float* target,
    const int32_t* lod, float elapsed, uint32_t flags, bool latch) {
    GxFrameWorkFlush(2);
    uint8_t phase = GxFramePhaseOpen() ? 1 : 0;
    GxFrameSetPhase(0);
    scene->SetCameraPosition(position);
    view->SetCamera(position, target, 0.9424778f, 1.7777778f);     // DAT_142734af8, DAT_14261a9b4
    scene->Animate(view, lod, elapsed, flags);
    if (latch) {
        GxXformApplyViewport();
        scene->LatchView(view);
    }
    GxFrameSetPhase(phase);
}

// ref: FUN_1413574e0
void WorldSceneAnimate(CWorldScene* scene, uint32_t* params, float elapsed, const float* position,
    const float* target, uint32_t flags, bool latch) {
    float camera[3] = {0.0f, 0.0f, 0.0f};
    float look[3] = {0.0f, 0.0f, 0.0f};
    if (position != nullptr) {
        std::memcpy(camera, position, 12);
    }
    if (target != nullptr) {
        std::memcpy(look, target, 12);
    }
    const int32_t lod[2] = {4, 4};
    auto view = reinterpret_cast<CWorldView*>(params);
    WorldSceneAnimateFrame(scene, view, camera, look, lod, elapsed, flags, latch);
}

// ref: FUN_1413573b0
void WorldSceneUpdate(CWorldScene* scene, uint32_t* params, bool skipDrawParams, const float* position) {
    auto view = reinterpret_cast<CWorldView*>(params);
    float camera[3] = {0.0f, 0.0f, 0.0f};
    if (position != nullptr) {
        std::memcpy(camera, position, 12);
    }
    CM2Scene* m2Scene = scene->m_m2Scene;
    bool collect = m2Scene->m_collectDrawParams;
    if (skipDrawParams) {
        m2Scene->m_collectDrawParams = false;
    }
    float minimum[3];
    float maximum[3];
    GxXformGetViewport(&minimum[0], &maximum[0], &minimum[1], &maximum[1], &minimum[2], &maximum[2]);
    m2Scene->SetView(reinterpret_cast<const C3Vector*>(camera), reinterpret_cast<const C44Matrix*>(view->m_matrices[4]),
        reinterpret_cast<const C44Matrix*>(view->m_matrices[5]), minimum, maximum, true);
    m2Scene->Animate(false);
    m2Scene->m_collectDrawParams = collect;
}

// ref: FUN_1424d1a00
void* WorldSceneViewFromCamera(void* out, CM2Camera* camera) {
    auto m = static_cast<float*>(out);
    MatrixIdentity(m);
    m[16] = 0.9424778f;     // 0x3f71463b
    m[17] = 0.2f;
    m[18] = 100.0f;
    m[19] = 1.0f;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float target[3] = {0.0f, 0.0f, 0.0f};
    M2CameraGetVector(camera, CM2Camera::VECTOR_POSITION, position);
    M2CameraGetVector(camera, CM2Camera::VECTOR_TARGET, target);
    float roll = M2CameraGetFloat(camera, CM2Camera::FLOAT_ROLL);
    float f[3] = {target[0] - position[0], target[1] - position[1], target[2] - position[2]};
    float length = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
    if (2.3841858e-07f < length) {          // DAT_14261a954
        float scale = 1.0f / std::sqrt(length);
        f[0] *= scale;
        f[1] *= scale;
        f[2] *= scale;
    }
    // FUN_14240ad20
    float s = std::sin(roll);
    float c = std::cos(roll);
    float uy = s;
    float uz = c;
    if (std::fabs(std::fabs((f[1] + f[0]) * s + f[2] * c) - 1.0f) < 1.1920929e-07f) {  // DAT_1426c6f7c
        uy = c;
        uz = s;
    }
    float ux = s;
    // The side from the up candidate, the up from the side, each normalised.
    float r[3] = {f[2] * uy - f[1] * uz, f[0] * uz - f[2] * ux, f[1] * ux - f[0] * uy};
    float v[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (2.3841858e-07f < length) {
        float scale = 1.0f / std::sqrt(length);
        v[0] *= scale;
        v[1] *= scale;
        v[2] *= scale;
    }
    float w[3] = {f[2] * v[1] - f[1] * v[2], f[0] * v[2] - f[2] * v[0], f[1] * v[0] - f[0] * v[1]};
    length = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
    if (2.3841858e-07f < length) {
        float scale = 1.0f / std::sqrt(length);
        w[0] *= scale;
        w[1] *= scale;
        w[2] *= scale;
    }
    m[0] = f[0];
    m[1] = f[1];
    m[2] = f[2];
    m[4] = w[0];
    m[5] = w[1];
    m[6] = w[2];
    m[8] = v[0];
    m[9] = v[1];
    m[10] = v[2];
    m[12] = position[0];
    m[13] = position[1];
    m[14] = position[2];
    float fov = M2CameraGetFloat(camera, CM2Camera::FLOAT_FIELD_OF_VIEW);
    m[16] = fov < 0.0f ? 0.0f : std::min(fov, 6.2831855f);
    m[17] = std::max(M2CameraGetFloat(camera, CM2Camera::FLOAT_NEAR_CLIP), 0.0f);
    m[18] = std::max(M2CameraGetFloat(camera, CM2Camera::FLOAT_FAR_CLIP), 0.0f);
    return out;
}

// The camera's view (its rotation, the translation left to the camera position) and its
// perspective projection at `aspect`.
// ref: FUN_1424d1db0
void CameraViewMatrices(const float* camera, float aspect, float* view, float* projection) {
    view[0] = -camera[4];
    view[1] = camera[8];
    view[2] = camera[0];
    view[3] = 0.0f;
    view[4] = -camera[5];
    view[5] = camera[9];
    view[6] = camera[1];
    view[7] = 0.0f;
    view[8] = -camera[6];
    view[9] = camera[10];
    view[10] = camera[2];
    view[11] = 0.0f;
    view[12] = 0.0f;
    view[13] = 0.0f;
    view[14] = 0.0f;
    view[15] = 1.0f;
    float perspective[16];
    MatrixIdentity(perspective);
    MatrixPerspective(camera[16], aspect, camera[17], camera[18], perspective);
    std::memcpy(projection, perspective, 0x40);
}

namespace {

void MatrixMultiply4(const float* a, const float* b, float* out) {
    for (int32_t row = 0; row < 4; row++) {
        for (int32_t column = 0; column < 4; column++) {
            out[row * 4 + column] = a[row * 4 + 3] * b[12 + column] + a[row * 4 + 2] * b[8 + column]
                + a[row * 4 + 1] * b[4 + column] + a[row * 4] * b[column];
        }
    }
}

}  // namespace

// The view of a frame's rectangle: its insets turned to the window's axes, the camera's (or
// the orthographic) matrices, the rectangle and insets taken to the screen's units, and a
// projection that lands the scene on the rectangle inside the current viewport. The scene is
// then animated and latched with that view, given the camera's clips and the frame's fog, and
// drawn.
// ref: FUN_1424d0a40
void WorldSceneRenderView(CWorldScene* scene, uint32_t* params, const float* camera, MODELFRAMEVIEW* block,
    FRAMEBUFFER* sceneBuffer, FRAMEBUFFER* sharedBuffer, float elapsed, uint32_t flags) {
    GxRenderContext* context = g_gxRenderContext;
    const float epsilon = 1.0e-5f;     // DAT_1426c75c4
    float viewportZ0 = context->Field<float>(0x2c);
    float viewportY0 = context->Field<float>(0x24);
    float viewportZ1 = context->Field<float>(0x30);
    float viewportY1 = context->Field<float>(0x28);
    float viewportX0 = context->Field<float>(0x1c);
    float viewportX1 = context->Field<float>(0x20);
    if (!(epsilon < std::fabs(viewportX1 - viewportX0)) || !(epsilon < std::fabs(viewportY1 - viewportY0))) {
        return;
    }
    float* rect = block->m_rect;
    float* insets = block->m_insets;
    float top = insets[0];
    insets[0] = insets[2];
    insets[3] = -insets[3];
    insets[2] = -top;
    float view[16];
    float projection[16];
    MatrixIdentity(view);
    MatrixIdentity(projection);
    float position[3] = {0.0f, 0.0f, 0.0f};
    if ((block->m_hasTransform & 1) == 0) {
        float width = (insets[3] + rect[3]) - (rect[1] + insets[1]);
        float height = (insets[2] + rect[2]) - (insets[0] + rect[0]);
        if (!(epsilon < width) || !(epsilon < height)) {
            return;
        }
        CameraViewMatrices(camera, width / height, view, projection);
        float scale = camera[19];
        position[0] = scale * camera[12];
        position[1] = scale * camera[13];
        position[2] = scale * camera[14];
    } else {
        float origin[2] = {rect[1], rect[0]};
        M2CameraGetOrthoMatrices(rect, origin, nullptr, view, projection);
    }
    // To the screen's units: x across, y up.
    const float unitsX = g_coordinateSpace.m_diagonalWidth;    // DAT_142ccb9f4
    const float unitsY = g_coordinateSpace.m_diagonalHeight;   // DAT_142ccb9f8
    rect[1] /= unitsX;
    rect[0] /= unitsY;
    rect[3] /= unitsX;
    rect[2] /= unitsY;
    insets[1] /= unitsX;
    insets[0] /= unitsY;
    insets[3] /= unitsX;
    insets[2] /= unitsY;
    block->m_translation[0] /= unitsX;
    block->m_translation[1] /= unitsY;
    float translationX = block->m_translation[0];
    float translationY = block->m_translation[1];
    const float* target = &context->Field<float>(8);
    float scaleY = target[2];
    float scaleX = target[3];
    if (!(epsilon < scaleX) || !(epsilon < scaleY)) {
        return;
    }
    // The rectangle in the target's pixels.
    float left = (insets[1] + rect[1] + translationX) * scaleX;
    float upper = (insets[2] + rect[2] + translationY) * scaleY;
    float halfWidth = ((insets[3] + rect[3] + translationX) * scaleX - left) * 0.5f;
    float halfHeight = (upper - (rect[0] + insets[0] + translationY) * scaleY) * 0.5f;
    float centerX = halfWidth + left;
    float centerY = upper - halfHeight;
    // The viewport's mapping from clip space to pixels, inverted.
    float viewportMap[16] = {};
    viewportMap[0] = (scaleX * viewportX1 - scaleX * viewportX0) * 0.5f;
    viewportMap[5] = (scaleY * viewportY1 - scaleY * viewportY0) * 0.5f;
    viewportMap[10] = viewportZ1 - viewportZ0;
    viewportMap[12] = viewportMap[0] + scaleX * viewportX0;
    viewportMap[13] = scaleY * viewportY1 - viewportMap[5];
    viewportMap[14] = viewportZ0;
    viewportMap[15] = 1.0f;
    float inverse[16];
    MatrixInverse(viewportMap, inverse, MatrixDeterminant(viewportMap));
    // The frame's mapping from clip space to its rectangle's pixels.
    float rectMap[16] = {};
    rectMap[0] = halfWidth;
    rectMap[5] = halfHeight;
    rectMap[10] = viewportZ1 - viewportZ0;
    rectMap[12] = centerX;
    rectMap[13] = centerY;
    rectMap[14] = viewportZ0;
    rectMap[15] = 1.0f;
    float placed[16];
    MatrixMultiply4(projection, rectMap, placed);
    float final[16];
    MatrixMultiply4(placed, inverse, final);
    context->SetScissor(rect);
    M2SetProjectionConstants(context, final);
    auto worldView = reinterpret_cast<CWorldView*>(params);
    std::memcpy(worldView->m_matrices[5], final, 0x40);
    std::memcpy(worldView->m_matrices[4], view, 0x40);
    WorldSceneAnimate(scene, params, 0.0f, position, nullptr, flags, false);
    scene->LatchView(worldView);
    scene->m_nearClip = camera[17];
    scene->m_farClip = camera[18];
    block->m_fog.m_words[39] = 0x3f800000;
    scene->m_fogs[0] = block->m_fog;
    CM2Scene* m2Scene = scene->m_m2Scene;
    m2Scene->m_flags = s_features[1] ? (m2Scene->m_flags | 0x100) : (m2Scene->m_flags & ~0x100u);
    m2Scene->m_flags = s_features[2] ? (m2Scene->m_flags | 0x400) : (m2Scene->m_flags & ~0x400u);
    WORLDRENDERPARAMS render;
    std::memset(&render, 0, sizeof(render));
    render.m_scene = sceneBuffer;
    render.m_shared = sharedBuffer;
    render.m_passes = 6;
    std::memcpy(render.m_position, position, 12);
    render.m_elapsed = elapsed;
    render.m_size[0] = sceneBuffer->m_size[0];
    render.m_size[1] = sceneBuffer->m_size[1];
    render.m_view = worldView;
    scene->Render(&render);
}

// The frame drawn through a buffer of the window's size: cleared, with a second set of
// targets sharing its depth when `flags` bit 0 asks, the view drawn into it, then resolved (or
// moved) and resampled into `target` (the window's frame buffer when null) with the full
// viewport, and the scissor put back.
// ref: FUN_1424d1370
void WorldSceneRenderFrame(CWorldScene* scene, uint32_t* params, void* view, const void* frame, void* target,
    float elapsed, uint32_t flags) {
    GxRenderContext* context = g_gxRenderContext;
    context->UnbindTextures();
    GXSIZE resolution;
    WindowGetResolution(&resolution);
    int32_t size[2] = {resolution.m_width, resolution.m_height};
    uint8_t samples = g_renderSettings.m_msaaSamples;
    uint8_t quality = g_renderSettings.m_msaaQuality;
    FRAMEBUFFER sceneBuffer;
    FrameBufferInit(&sceneBuffer);
    sceneBuffer.m_width = size[0];
    sceneBuffer.m_height = size[1];
    FRAMEBUFFERTARGET& color = sceneBuffer.m_color[0];
    color.m_format = 2;
    color.m_samples = samples;
    color.m_08 = quality;
    color.m_renderTarget = 1;
    color.m_shaderResource = samples == 0 ? 1 : 0;
    FRAMEBUFFERTARGET& depth = sceneBuffer.m_depth;
    depth.m_format = 0x1e;
    depth.m_samples = samples;
    depth.m_08 = quality;
    depth.m_renderTarget = 1;
    depth.m_shaderResource = samples == 0 ? 1 : 0;
    depth.m_10 = samples == 0 ? 1 : 0;
    FrameBufferBind(&sceneBuffer);
    context->Clear(sceneBuffer.m_targets.m_count != 0 ? &sceneBuffer.m_targets : nullptr, 0x3f, 0, 0.0f, 0, 0);
    float scissor[4] = {};
    context->GetScissor(scissor);
    FRAMEBUFFER sharedBuffer;
    FrameBufferInit(&sharedBuffer);
    GxRenderTargetsClear(&sharedBuffer.m_targets);
    if (flags & 1) {
        FrameBufferSetupSharedDepth(&sharedBuffer, size,
            sceneBuffer.m_targets.m_count != 0 ? &sceneBuffer.m_targets : nullptr);
        FrameBufferBind(&sharedBuffer);
    }
    MODELFRAMEVIEW block;
    std::memcpy(&block, frame, sizeof(block));
    WorldSceneRenderView(scene, params, static_cast<const float*>(view), &block, &sceneBuffer, &sharedBuffer, elapsed,
        flags);
    FRAMEBUFFER resolved;
    FrameBufferInit(&resolved);
    if (samples != 0) {
        FrameBufferResolve(context, &sceneBuffer, &resolved);
    } else {
        FrameBufferMove(&resolved, &sceneBuffer);
    }
    auto destination = static_cast<FRAMEBUFFER*>(target);
    if (destination == nullptr) {
        destination = WindowGetFrameBuffer();
    }
    if (destination->m_bound == 0) {
        return;
    }
    float* viewport = &context->Field<float>(0x1c);
    if (viewport[0] != 0.0f || viewport[1] != 1.0f || viewport[2] != 0.0f || viewport[3] != 1.0f
        || viewport[4] != 0.0f || viewport[5] != 1.0f) {
        viewport[0] = 0.0f;
        viewport[1] = 1.0f;
        viewport[2] = 0.0f;
        viewport[3] = 1.0f;
        viewport[4] = 0.0f;
        viewport[5] = 1.0f;
        context->Field<uint8_t>(0x18) = 1;
    }
    float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};      // DAT_1426c8890
    WorldSceneSetRect(full, scene);
    RESAMPLEOPTIONS options = {};
    // The reference reads the destination's colour texture at a null +0x48 when it has none.
    gxDx12Backend::Tex* destTex = destination->m_targets.m_texs[1];
    ResampleToTarget(context, WorldSceneGetEnv(scene), resolved.m_targets.m_texs[1], destTex, &options, true);
    FrameBufferRelease(&resolved);
    context->SetScissor(scissor);
    if (sharedBuffer.m_targets.m_count != 0) {
        FrameBufferRelease(&sharedBuffer);
    }
}

// ref: FUN_141352960
void WorldSceneSetRect(const float* rect, CWorldScene* scene) {
    std::memcpy(scene->m_0c8, rect, 0x10);
}


// ref: FUN_141357340
bool WorldSceneModelReady(CWorldScene* scene, const char* name, uint8_t priority) {
    if (scene == nullptr) {
        return WorldScenesModelReady(name, priority);
    }
    return scene->m_m2Scene->ModelFileReady(name, priority);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cfdb68 = (void*)&s_loadSortMask;
extern "C" void* const gen_p_1430ab9e4 = (void*)&s_frameViewScale;
extern "C" void* const gen_p_1430aba18 = (void*)&s_frameView14;
extern "C" void* const gen_p_1430aba1c = (void*)&s_frameView16;
extern "C" void* const gen_p_1430aba20 = (void*)&s_frameView15;
extern "C" void* const gen_p_1430aba28 = (void*)&s_frameView17;
extern "C" void* const gen_p_1430aba60 = (void*)&s_1430aba60;
extern "C" void* const gen_p_1430aba78 = (void*)&s_frameRect;
extern "C" void* const gen_p_1430abb08 = (void*)&s_cameraPosition;
// ---- end of generated globals ----
