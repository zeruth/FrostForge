// WoW/Source/WorldClient2/WorldScene.cpp's draw: a view's frame prepared (the camera, culling,
// DayNight, the M2 scene's view and animation; FUN_1413c5250) and drawn into a frame's buffers
// (FUN_1413c5ce0), with the pieces of the view, the culling and the occlusion they run.

#include "world/WorldScene.hpp"
#include "base/Matrix.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/RenderSettings.hpp"
#include "daynight/DayNight.hpp"
#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "model2/M2Scene.hpp"
#include "os/Sync.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "shadereffect/ShaderEffectShadow.hpp"
#include "threadpool/ThreadPool.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"
#include "world/CullWorker.hpp"
#include "world/SceneOcclusion.hpp"
#include "world/WorldFrame.hpp"
#include "world/WorldMap.hpp"
#include "world/WorldSceneRender.hpp"
#include "world/WorldStubs.hpp"
#include "world/WorldView.hpp"
#include <cmath>
#include <cstring>
#include <windows.h>

namespace {

template <class T>
T& ViewAt(CWorldView* view, size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(view) + offset);
}

// The frame a scene's render callbacks are handed (0x20).
struct RENDERCALLBACKFRAME {
    uint64_t m_zero[2];
    CWorldScene* m_scene;
    CWorldView* m_view;
};

void RunCallbackList(CRenderCallbackList& callbacks, RENDERCALLBACKFRAME* frame) {
    for (CRenderCallback* callback = callbacks.Head(); callback != nullptr; callback = callbacks.Next(callback)) {
        callback->m_function(callback->m_param, callback->m_data, frame);
    }
}

// A frustum from its eight corners (0xc0): six planes, then the corners.
struct WORLDFRUSTUM {
    float m_planes[6][4];
    float m_corners[8][3];
};
static_assert(sizeof(WORLDFRUSTUM) == 0xc0, "WORLDFRUSTUM is 0xc0 bytes");

// The plane through three points, its normal normalised, facing by their winding.
// ref: FUN_141346ec0
void PlaneFromPoints(float* plane, const float* a, const float* b, const float* c) {
    float nx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
    float ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
    float nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    plane[0] = nx;
    plane[1] = ny;
    plane[2] = nz;
    float length = nx * nx + ny * ny + nz * nz;
    if (2.3841858e-07f < length) {      // DAT_14261a954
        float scale = 1.0f / std::sqrt(length);
        nx *= scale;
        ny *= scale;
        nz *= scale;
        plane[0] = nx;
        plane[1] = ny;
        plane[2] = nz;
    }
    plane[3] = -(ny * a[1] + nx * a[0] + nz * a[2]);
}

// The six planes of the frustum whose corners are `corners` (the near four, then the far
// four); the last plane is the fifth turned around through the third corner.
// ref: FUN_1413463d0
void FrustumFromCorners(WORLDFRUSTUM* frustum, const float* corners) {
    std::memcpy(frustum->m_corners, corners, sizeof(frustum->m_corners));
    float (*p)[3] = frustum->m_corners;
    PlaneFromPoints(frustum->m_planes[0], p[1], p[5], p[6]);
    PlaneFromPoints(frustum->m_planes[1], p[0], p[7], p[4]);
    PlaneFromPoints(frustum->m_planes[2], p[0], p[4], p[5]);
    PlaneFromPoints(frustum->m_planes[3], p[2], p[6], p[7]);
    PlaneFromPoints(frustum->m_planes[4], p[5], p[4], p[6]);
    float nx = -frustum->m_planes[4][0];
    float ny = -frustum->m_planes[4][1];
    float nz = -frustum->m_planes[4][2];
    frustum->m_planes[5][0] = nx;
    frustum->m_planes[5][1] = ny;
    frustum->m_planes[5][2] = nz;
    frustum->m_planes[5][3] = -(ny * p[2][1] + nx * p[2][0] + nz * p[2][2]);
}

// The target scale the environment's shaders read: the targets' size over the frame buffer's,
// and the inverse size (1, 1, 0, 0 without targets).
// ref: FUN_14068ea50
void TargetScale(const int32_t* bufferSize, const float* rect, float* out) {
    out[0] = static_cast<float>(static_cast<int32_t>(rect[2]))
        / (static_cast<float>(bufferSize[0]) * static_cast<float>(static_cast<int32_t>(rect[4])));
    out[1] = static_cast<float>(static_cast<int32_t>(rect[3]))
        / (static_cast<float>(bufferSize[1]) * static_cast<float>(static_cast<int32_t>(rect[5])));
    out[2] = rect[0] / static_cast<float>(static_cast<int32_t>(rect[4]));
    out[3] = rect[1] / static_cast<float>(static_cast<int32_t>(rect[5]));
}

// ref: FUN_1412b8210
void RenderTargetScale(const int32_t* bufferSize, const GXRENDERTARGETS* targets, float* out) {
    if (targets != nullptr) {
        int32_t size[6];
        int32_t* targetSize = GxRenderTargetsSize(targets, size);
        float rect[6] = {0.0f, 0.0f};
        std::memcpy(&rect[2], targetSize, 8);
        std::memcpy(&rect[4], targetSize, 8);
        TargetScale(bufferSize, rect, out);
        return;
    }
    out[3] = 0.0f;
    out[0] = 1.0f;
    out[1] = 1.0f;
    out[2] = 0.0f;
}

// DAT_1430a5b90: the frame passes run without the shadow factor (FUN_1412a9990).
uint8_t s_noShadowFactor;

// ref: FUN_1412a9990
uint8_t FramePassesNoShadowFactor() {
    return s_noShadowFactor;
}

// The frame's scratch memory (DAT_142d4c828..850): a lock, a free list and one list of blocks
// per pool thread. Its allocator (FUN_14242bb50) and setup (FUN_14242bd00) are not ported, so
// nothing is ever handed out.
OSMUTEX s_scratchLock;              // DAT_142d4c828
void* s_scratchFree;                // DAT_142d4c830
void** s_scratchThreads;            // DAT_142d4c848
uint32_t s_scratchThreadCount;      // DAT_142d4c850
uint64_t s_scratchFrame;            // DAT_142d4c820

// Every thread's blocks back on the free list, and a new frame.
// ref: FUN_14242bab0
void FrameScratchReset() {
    OsMutexLock(&s_scratchLock);
    for (uint32_t i = 0; i < s_scratchThreadCount; i++) {
        auto block = static_cast<uint8_t*>(s_scratchThreads[i]);
        while (block != nullptr) {
            auto next = *reinterpret_cast<uint8_t**>(block + 0x20);
            *reinterpret_cast<void**>(block + 0x20) = s_scratchFree;
            s_scratchFree = block;
            block = next;
        }
        s_scratchThreads[i] = nullptr;
    }
    s_scratchFrame++;
    OsMutexUnlock(&s_scratchLock);
}

// The liquid displacement maps (Liquid::DisplMapPerturbation): their grid snapped to the
// camera (FUN_142529c10) and their perturbations stepped (FUN_142529d80). Not ported.
// ref: FUN_142529c10
void LiquidDisplacementSetOrigin(const float* position, const float* cell) {
    (void)position;
    (void)cell;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_142529d80
void LiquidDisplacementUpdate(const uint32_t* time) {
    (void)time;
    FROST_UNIMPLEMENTED();
}

// The liquid targets' per-frame reset (FUN_14252cb00). Not ported.
// ref: FUN_14252cb00
void LiquidTargetsReset() {
    FROST_UNIMPLEMENTED();
}

// Reflections (FUN_14252caa0): water detail above 2 with the second water setting, and the
// view seeing no occluders.
// ref: FUN_14252caa0
bool WaterReflectionsEnabled(uint8_t occluders) {
    bool enabled = WorldWaterDetail() > 2 && WorldReflectionMode() != 0;
    return occluders == 0 && enabled;
}

// Refraction (FUN_14252cae0): water detail above 1.
// ref: FUN_14252cae0
bool WaterRefractionEnabled() {
    return WorldWaterDetail() > 1;
}

// The ray-traced scene's frame (FUN_14125f5c0 through FUN_14125f590). Not ported.
// ref: FUN_14125f590
void RayTracingUpdate(void* rayTracing) {
    (void)rayTracing;
    FROST_UNIMPLEMENTED();
}

// A view list's work as pool jobs, 0x80 at a time (FUN_1413a2c50); its jobs (FUN_1413a2b70)
// are not ported.
// ref: FUN_1413a2b70
void WorldViewListJob(void* storage) {
    (void)storage;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413a2c50
void WorldViewListRun(WORLDVIEWLIST* list) {
    auto head = reinterpret_cast<uintptr_t>(list->m_head[1]);
    if ((head & 1) != 0 || head == 0) {
        return;
    }
    if (list->m_group == nullptr) {
        list->m_group = ThreadPoolGroupCreate(1);
    }
    list->m_held = 0;
    list->m_2c = 0;
    blz::function_storage tasks[0x80];
    for (blz::function_storage& task : tasks) {
        task.m_invoke = nullptr;
        task.m_manager = blz::function_empty_manager;
    }
    uintptr_t node = head;
    while ((node & 1) == 0 && node != 0) {
        uintptr_t next = *reinterpret_cast<uintptr_t*>(node + list->m_linkoffset + 8);
        uint16_t count = list->m_held;
        if (count == 0x80) {
            ThreadPoolGroupSetCount(list->m_group, 0x80);
            ThreadPoolSubmit(list->m_held, tasks, 1, list->m_group);
            ThreadPoolGroupWait(list->m_group);
            count = 0;
            list->m_held = 0;
        }
        blz::function_storage& task = tasks[count];
        task.m_manager(task.m_storage, nullptr, 0);
        task.m_invoke = reinterpret_cast<void*>(&WorldViewListJob);
        task.m_manager = blz::function_trivial_manager;
        *reinterpret_cast<void**>(task.m_storage) = nullptr;
        *reinterpret_cast<uintptr_t*>(task.m_storage + 8) = node;
        list->m_held++;
        node = next;
    }
    if (list->m_held != 0) {
        ThreadPoolGroupSetCount(list->m_group, list->m_held);
        ThreadPoolSubmit(list->m_held, tasks, 1, list->m_group);
    }
    for (uint32_t i = 0x80; i-- > 0;) {
        tasks[i].m_manager(tasks[i].m_storage, nullptr, 0);
    }
}

// The occlusion stage the frame has reached; the first one starts the occluder raster job
// (its job, LAB_142553940, is not ported) when occlusion is on and the cull found occluders.
// ref: FUN_142553860
void SceneOcclusionAdvance(SceneOcclusion* occlusion, CWorldView* view, uint32_t stage) {
    if (stage == 2) {
        ThreadPoolGroupWait(occlusion->Field<THREADPOOLGROUP*>(0x7148));
    }
    occlusion->Field<uint32_t>(0x7124) = stage;
    if (occlusion->Field<uint32_t>(0x80) != 0 && stage == 1) {
        auto occluders = static_cast<int32_t*>(CWorldCullOccluders(view->m_cull));
        occlusion->Field<int32_t*>(0x118) = occluders;
        if (*occluders != 0) {
            FROST_UNIMPLEMENTED();
        }
    }
}

// ref: FUN_14254ff50
void SceneOcclusionWait(SceneOcclusion** holder, CWorldView* view, uint32_t stage) {
    SceneOcclusionAdvance(*holder, view, stage);
}

}  // namespace

// The cull's occluder list (+0x16380, FUN_141446590 through FUN_1413dc570). Not ported.
// ref: FUN_141446590
void* CWorldCullOccluders(CWorldCull* cull) {
    (void)cull;
    FROST_UNIMPLEMENTED();
    static int32_t s_none = 0;
    return &s_none;
}

// The cull's frame finished: the map's areas and objects gathered, the reflection and
// refraction flags set from the water heights (FUN_141445630). Not ported.
// ref: FUN_141445630
void CWorldCullUpdate(CWorldCull* cull) {
    (void)cull;
    FROST_UNIMPLEMENTED();
}

// The water height the cull found under the camera (FUN_141446290). Not ported.
// ref: FUN_141446290
float CWorldCullWaterHeight(CWorldCull* cull) {
    (void)cull;
    FROST_UNIMPLEMENTED();
    return 0.0f;
}

// ---- the view ------------------------------------------------------------------------------

// With flag 0x400 the scene's detail target follows the camera (or the scene's tracked object,
// a little above it). The target's update (FUN_1413aa540, FUN_1413aa640) is not ported.
// ref: FUN_1413de240
void CWorldView::UpdateDetailTarget() {
    if ((this->m_flags & 0x400) == 0) {
        return;
    }
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413da730
void CWorldView::CullFinish() {
    CWorldCullUpdate(this->m_cull);
}

// ref: FUN_1413dd130
void CWorldView::RunList(uint32_t index) {
    WorldViewListRun(&this->m_lists[index]);
}

// ref: FUN_1413da5b0
void CWorldView::ClearLists() {
    for (WORLDVIEWLIST& list : this->m_lists) {
        WorldViewListClear(&list);
    }
}

// The reflection (0) or refraction (1) view, made the first time it is asked for.
// ref: FUN_1413db650
CWorldView* CWorldView::GetChild(uint32_t index) {
    if (this->m_children[index] == nullptr) {
        static const char s_viewFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WorldClient2\\WorldView.cpp";
        void* memory = SMemAlloc(sizeof(CWorldView), 0x27, s_viewFile, SStrHashFnv1a(s_viewFile), 0x79f, 0);
        this->m_children[index] = new (memory) CWorldView(this->m_scene, this, 0x100082);
    }
    return this->m_children[index];
}

// ref: FUN_1413dcc10
bool CWorldView::WantsReflection() {
    return (this->m_cull->m_result.m_data[0] >> 4 & 1) != 0;
}

// ref: FUN_1413dcc30
bool CWorldView::WantsRefraction() {
    return (this->m_cull->m_result.m_data[0] >> 3 & 1) != 0;
}

// ref: FUN_1413db0c0
float CWorldView::WaterHeight() {
    return CWorldCullWaterHeight(this->m_cull);
}

// ---- the scene -----------------------------------------------------------------------------

// Another view than the scene's own: its frustum and cull, whether it sees occluders, and its
// gathered state.
// ref: FUN_1413ca340
void CWorldScene::BeginView(CWorldView* view) {
    view->m_flags &= ~0x100u;
    view->BuildFrustum();
    view->Cull();
    this->m_67c[0] = view->m_690 != 0;
    if (view->m_gathered[0].m_00 == nullptr && view->m_gathered[1].m_00 == nullptr) {
        view->m_flags |= 0x100;
    }
    this->m_a70 = view->m_300;
    this->m_a78 = view->m_308;
}

// The frame for the parameters' view: the occlusion stages and DayNight for a view other than
// the scene's own, the culling finished, the M2 scene given the view, stepped and animated
// against the view's frustum between the render callbacks, and the camera latched.
// ref: FUN_1413c5250
void CWorldScene::PreRender(WORLDRENDERPARAMS* params) {
    GxFrameSetPhase(0);
    this->m_frame++;
    CWorldView* view = params->m_view;
    RENDERCALLBACKFRAME frame = {{0, 0}, this, view};
    if (view != this->m_view) {
        this->BeginView(view);
        this->UpdateDayNight(view);
        SceneOcclusion** occlusion = &view->m_occlusion;
        if (SceneOcclusionGetState(occlusion) == 0 || SceneOcclusionGetState(occlusion) == 5) {
            SceneOcclusionStage(occlusion, view, 0);
            SceneOcclusionStage(occlusion, view, 1);
            SceneOcclusionStage(occlusion, view, 2);
            view->UpdateOccluders(this->m_67c[0] != 0);
            SceneOcclusionStage(occlusion, view, 3);
            SceneOcclusionStage(occlusion, view, 4);
        } else {
            view->UpdateOccluders(this->m_67c[0] != 0);
        }
    }
    if (view->m_flags & 0x4000) {
        WorldWaitGlobalJobs();
    }
    view->UpdateDetailTarget();
    SceneOcclusionStage(&view->m_occlusion, view, 5);
    view->CullFinish();
    SceneOcclusionWait(&view->m_occlusion, view, 1);
    if (view != this->m_view) {
        ThreadPoolGroupWait(this->m_groups[1]);
        if (this->m_dayNight != nullptr) {
            float liquidDepth = 0.0f;
            uint32_t liquid = view->GetLiquid(&liquidDepth);
            DayNightSetCamera(this->m_dayNight, view, &ViewAt<float>(view, 0x4e0), &ViewAt<float>(view, 0x460),
                &ViewAt<float>(view, 0x4ec), static_cast<int32_t>(liquid), liquidDepth, ViewAt<float>(view, 0x4a4));
            uint8_t fog[0x40] = {};
            uint8_t covered = 0;
            float waterDepth = 3.4028235e+38f;
            int32_t underwater = view->QueryWater(fog, &covered, &waterDepth);
            this->m_a68 = waterDepth;
            this->m_a64 = waterDepth;
            if (underwater != 1) {
                this->m_a64 = 0.0f;
                if (waterDepth == 3.4028235e+38f) {     // DAT_14261f708
                    this->m_a68 = 0.0f;
                }
            }
            DayNightSetWater(this->m_dayNight, underwater == 1, fog, covered != 0, waterDepth);
            DayNightComputeSunDirection(this->m_dayNight, view->m_matrices[4]);
            if (this->m_dayNight != nullptr) {
                DayNightPrepareFrame(this->m_dayNight, &this->m_farClip, (view->m_flags & 0x40) != 0);
            }
        }
    }
    CWorldSceneRender* render = params->m_view->m_render;
    render->SetupRayTracing(params->m_80[2] != 0, &this->m_temp, &this->m_19b8);
    CM2Scene* m2Scene = this->m_m2Scene;
    CWorldView* own = this->m_view;
    if (m2Scene != nullptr) {
        m2Scene->m_owner = this;
        m2Scene->m_030 = view;
        if (view->m_flags & 0x400) {
            m2Scene->m_flags |= 0x10;
        } else {
            m2Scene->m_flags &= ~0x10u;
        }
        float minimum[3];
        float maximum[3];
        GxXformGetViewport(&minimum[0], &maximum[0], &minimum[1], &maximum[1], &minimum[2], &maximum[2]);
        m2Scene->SetView(reinterpret_cast<const C3Vector*>(params->m_position),
            reinterpret_cast<const C44Matrix*>(params->m_view->m_matrices[4]),
            reinterpret_cast<const C44Matrix*>(params->m_view->m_matrices[5]), minimum, maximum, true);
        m2Scene->FlushLodChanges();
        if (own == view) {
            int32_t step = static_cast<int32_t>(std::nearbyint(params->m_elapsed * 1000.0f));    // DAT_14261b418
            m2Scene->m_time += step;
            m2Scene->m_timeStep = step;
        }
        RunCallbackList(this->m_callbacks[0], &frame);
        InterlockedExchange(reinterpret_cast<volatile long*>(&m2Scene->m_particles.m_used), 0);
        WORLDFRUSTUM frustum;
        for (float(&plane)[4] : frustum.m_planes) {
            plane[0] = 0.0f;
            plane[1] = 0.0f;
            plane[2] = 1.0f;
            plane[3] = 0.0f;
        }
        std::memset(frustum.m_corners, 0, sizeof(frustum.m_corners));
        FrustumFromCorners(&frustum, &ViewAt<float>(view, 0x334));
        std::memset(this->m_screenRect, 0, sizeof(this->m_screenRect));
        m2Scene->Animate(&frustum, true);
        view->RunList(1);
        render->DispatchRayTracing(params->m_80[2] != 0);
        std::memcpy(this->m_screenRect, m2Scene->m_screenRect, 0x10);
        std::memcpy(&render->m_b08, m2Scene->m_screenRect, 0x10);
        void* rayTracing = M2SceneGetRayTracing(m2Scene);
        if (rayTracing != nullptr) {
            RayTracingUpdate(rayTracing);
        }
        FrameScratchReset();
        RunCallbackList(this->m_callbacks[1], &frame);
        m2Scene->m_030 = this->m_view;
        m2Scene->m_owner = this;
    }
    RunCallbackList(this->m_callbacks[2], &frame);
    view->RunList(0);
    SceneOcclusionWait(&view->m_occlusion, view, 2);
    view->LatchCamera();
    if (this->m_m2Scene != nullptr) {
        this->m_m2Scene->LatchView();
    }
    GxFrameSetPhase(1);
}

// The frame drawn: prepared, the viewport kept inside the target, the liquid displacement
// stepped, the clip plane for an Outland or water view, the reflection and refraction views
// drawn first when the water settings and the view ask, the scene lights gathered, the
// environment set, and the render's passes run into the parameters' buffers (or, without a
// buffer, only the M2 scene's elements built).
// ref: FUN_1413c5ce0
void CWorldScene::Render(WORLDRENDERPARAMS* params) {
    ThreadPoolGroupWait(this->m_groups[2]);
    InterlockedExchange(&this->m_busy, 1);
    GxRenderContext* context = g_gxRenderContext;
    this->PreRender(params);
    float* viewport = &context->Field<float>(0x1c);
    float x0 = viewport[0] > 0.0f ? viewport[0] : 0.0f;
    float x1 = viewport[1] < 1.0f ? viewport[1] : 1.0f;
    float y0 = viewport[2] > 0.0f ? viewport[2] : 0.0f;
    float y1 = viewport[3] < 1.0f ? viewport[3] : 1.0f;
    const float nearDepth = 1.52587890625e-05f;     // DAT_1426c6544
    if (viewport[0] != x0 || viewport[1] != x1 || viewport[2] != y0 || viewport[3] != y1 || viewport[4] != nearDepth
        || viewport[5] != 1.0f) {
        viewport[0] = x0;
        viewport[1] = x1;
        viewport[2] = y0;
        viewport[3] = y1;
        viewport[4] = nearDepth;
        viewport[5] = 1.0f;
        context->Field<uint8_t>(0x18) = 1;
    }
    CWorldView* view = params->m_view;
    bool shadows = view->m_6b4 != 0;
    CWorldSceneRender* render = view->m_render;
    CM2Scene* m2Scene = this->m_m2Scene;
    const float cell[2] = {128.0f, 128.0f};     // DAT_1426c707c
    LiquidDisplacementSetOrigin(reinterpret_cast<const float*>(&this->m_6e8), cell);
    uint32_t time = FrameTime_GetTime();
    LiquidDisplacementUpdate(&time);
    view->m_flags &= ~0x80u;
    CWorldMap* map = this->m_maps[this->m_mapIndex];
    if (map != nullptr) {
        if (map->MapId() == 0x212) {
            const float plane[4] = {0.0f, 0.0f, 1.0f, 800.0f};     // DAT_142738a80
            std::memcpy(view->m_clipPlane, plane, 0x10);
            view->m_flags |= 0x80;
        } else if (WorldMapHasWaterPlane(map) && 1.0f < ViewAt<float>(view, 0x4d0)) {
            float height = ViewAt<float>(view, 0x4d0);
            float nx = ViewAt<float>(view, 0x460);
            float ny = ViewAt<float>(view, 0x464);
            float nz = ViewAt<float>(view, 0x468);
            float length = nx * nx + ny * ny + nz * nz;
            if (2.3841858e-07f < length) {
                float scale = 1.0f / std::sqrt(length);
                nx *= scale;
                ny *= scale;
                nz *= scale;
            }
            float px = nx * height + ViewAt<float>(view, 0x4e0);
            float py = ny * height + ViewAt<float>(view, 0x4e4);
            float pz = nz * height + ViewAt<float>(view, 0x4e8);
            view->m_clipPlane[0] = -nx;
            view->m_clipPlane[1] = -ny;
            view->m_clipPlane[2] = -nz;
            view->m_clipPlane[3] = -(px * -nx + py * -ny + pz * -nz);
            view->m_flags |= 0x80;
        }
        WorldMapUpdateFades(map);
    }
    FRAMEBUFFER* reflectionBuffer = &render->m_buffer070;
    GxRenderTargetsClear(&reflectionBuffer->m_targets);
    FRAMEBUFFER* refractionBuffer = &render->m_buffer160;
    GxRenderTargetsClear(&refractionBuffer->m_targets);
    if (WorldWaterDetail() != 0 && params->m_scene != nullptr) {
        WORLDRENDERPARAMS water;
        std::memset(&water, 0, sizeof(water));
        water.m_passes = 6;
        std::memcpy(water.m_position, params->m_position, 12);
        water.m_elapsed = params->m_elapsed;
        water.m_size[0] = params->m_size[0];
        water.m_size[1] = params->m_size[1];
        water.m_view = view;
        if (WaterReflectionsEnabled(this->m_67c[0]) && view->WantsReflection()) {
            CWorldView* reflection = view->GetChild(0);
            water.m_scene = reflectionBuffer;
            water.m_reflectionMode = WorldReflectionMode();
            water.m_waterHeight = view->WaterHeight();
            water.m_80[2] = params->m_80[2];
            water.m_childView = reflection;
            this->RenderWater(&water);
        }
        if (WaterRefractionEnabled() && view->WantsRefraction()) {
            CWorldView* refraction = view->GetChild(1);
            water.m_scene = refractionBuffer;
            water.m_reflectionMode = 0;
            water.m_waterHeight = 0.0f;
            water.m_childView = refraction;
            this->RenderWater(&water);
        }
    }
    this->GatherLights();
    float targetScale[4] = {};
    FRAMEBUFFER* source = params->m_shared != nullptr ? params->m_shared : params->m_scene;
    if (source != nullptr) {
        RenderTargetScale(source->m_size, source->m_targets.m_count != 0 ? &source->m_targets : nullptr, targetScale);
    }
    float shadowFactor = static_cast<float>(FramePassesNoShadowFactor() ^ 1);
    float color[4];
    std::memcpy(color, MapWeatherColor(this->m_038), 12);
    color[3] = 1.0f;
    float density = MapWeatherDensity(this->m_038);
    float direction[4];
    std::memcpy(direction, MapWeatherDirection(this->m_038), 12);
    direction[3] = density;
    WorldSceneSetEnvironment(this->m_e80, this, &ViewAt<float>(view, 0x4e0), view->m_matrices[1], targetScale,
        direction, color, shadowFactor);
    if (params->m_scene == nullptr) {
        if (m2Scene != nullptr) {
            m2Scene->BuildElements(shadows ? 1 : 0, view->m_6b4 >= 3 ? 1 : 0);
        }
        LiquidTargetsReset();
        view->ClearLists();
    } else {
        params->m_m2Scene = m2Scene;
        params->m_80[0] = shadows ? 1 : 0;
        params->m_reflection = reflectionBuffer;
        params->m_refraction = refractionBuffer;
        params->m_80[1] = view->m_6b4 >= 3 ? 1 : 0;
        render->Begin(this, view, params);
        render->Draw();
        params->m_80[3] = render->m_9d3;
    }
    FrameBufferRelease(reflectionBuffer);
    FrameBufferRelease(refractionBuffer);
    if (InterlockedExchangeAdd(&this->m_busy, -1) == 1 && this->m_m2Scene != nullptr) {
        *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(this->m_m2Scene) + 0xdaf0) = 0;
    }
}

// A reflection or refraction view drawn into its buffer (FUN_1413c7570). Not ported.
// ref: FUN_1413c7570
void CWorldScene::RenderWater(WORLDRENDERPARAMS* params) {
    (void)params;
    FROST_UNIMPLEMENTED();
}

// The scene's lights for its models: the M2 scene's point lights nearest the camera, each
// held in one of three slots that fade a light in over a third of a second when it appears and
// out when it goes; one new light takes the last free slot a frame. The slots' lights go into
// the scene's lighting, and its sphere is taken around them.
// ref: FUN_1413c9880
void CWorldScene::GatherLights() {
    CWorldMap* current = this->m_maps[this->m_mapIndex];
    float center[4] = {0.0f, 0.0f, 0.0f, 32.0f};
    if (current != nullptr) {
        std::memcpy(center, current->Position(), 12);
    }
    CM2ModelLighting nearest;
    M2ModelLightingBegin(&nearest, nullptr, center);
    M2SceneGatherLights(this->m_m2Scene, &nearest);
    uint32_t count = nearest.m_pointCount;
    float step = FrameTime_GetElapsedSeconds() * 3.0f;  // DAT_14262f25c
    int32_t free = -1;
    for (int32_t i = 0; i < 3; i++) {
        WORLDSCENELIGHT& slot = this->m_lights[i];
        if ((slot.m_flags & 4) != 0) {
            free = i;
            continue;
        }
        int32_t found = -1;
        if (slot.m_source != nullptr) {
            for (uint32_t j = 0; j < count; j++) {
                if (nearest.m_points[j] == slot.m_source) {
                    found = static_cast<int32_t>(j);
                    slot.m_light.Assign(nearest.m_points[j]);
                    float color[3];
                    std::memcpy(slot.m_color, slot.m_light.GetDiffuse(color), sizeof(slot.m_color));
                    nearest.m_points[j] = nullptr;
                    break;
                }
            }
        }
        uint8_t flags = found == -1 ? static_cast<uint8_t>((slot.m_flags & 0xfd) | 1)
                                    : static_cast<uint8_t>((slot.m_flags & 0xfe) | 2);
        slot.m_flags = flags;
        if ((flags & 1) != 0) {
            slot.m_fade = slot.m_fade - step;
            if (slot.m_fade < 0.0f) {
                slot.m_fade = 0.0f;
                slot.m_flags = static_cast<uint8_t>((flags & 0xfe) | 4);
                free = i;
            }
        } else if ((flags & 2) != 0) {
            slot.m_fade = step + slot.m_fade;
            if (1.0f < slot.m_fade) {
                slot.m_fade = 1.0f;
                slot.m_flags = flags & 0xfd;
            }
        }
    }
    if (free != -1) {
        for (uint32_t j = 0; j < count; j++) {
            if (nearest.m_points[j] == nullptr) {
                continue;
            }
            WORLDSCENELIGHT& slot = this->m_lights[free];
            slot.m_source = nearest.m_points[j];
            slot.m_fade = 0.0f;
            slot.m_flags = static_cast<uint8_t>((slot.m_flags & 0xfb) | 2);
            slot.m_light.Assign(nearest.m_points[j]);
            float color[3];
            std::memcpy(slot.m_color, slot.m_light.GetDiffuse(color), sizeof(slot.m_color));
            break;
        }
    }
    for (WORLDSCENELIGHT& slot : this->m_lights) {
        if ((slot.m_flags & 4) != 0) {
            continue;
        }
        slot.m_light.m_diffuse[0] = slot.m_fade * slot.m_color[0];
        slot.m_light.m_diffuse[1] = slot.m_color[1] * slot.m_fade;
        slot.m_light.m_diffuse[2] = slot.m_color[2] * slot.m_fade;
        M2ModelLightingAdd(&this->m_lighting, &slot.m_light);
    }
    M2ModelLightingBound(&this->m_lighting);
}

// ref: FUN_1413c4740
void CWorldScene::GetLightConstants(const float* view, uint8_t* out) {
    float scale = 1.0f;  // DAT_142618d64
    float value = 0.0f;
    float offset[2] = {0.0f, 0.0f};
    if (g_renderSettings.m_shadowMode > 1 && this->m_dayNight != nullptr) {
        DayNightGetLightTransform(this->m_dayNight, &value, &scale, offset);
    }
    float transform[16];
    MatrixIdentity(transform);  // DAT_14261a9d0
    MatrixScaleUniform(transform, scale);
    const float move[3] = {offset[0], offset[1], 0.0f};
    MatrixTranslate(transform, move);
    float inverse[16];
    MatrixInverse(view, inverse, MatrixDeterminant(view));
    // The view's inverse times the transform, its first three columns as rows.
    auto constants = reinterpret_cast<float*>(out + 0xb0);
    for (int32_t column = 0; column < 3; column++) {
        for (int32_t row = 0; row < 4; row++) {
            constants[column * 4 + row] = inverse[row * 4 + 0] * transform[0 * 4 + column]
                + inverse[row * 4 + 1] * transform[1 * 4 + column] + inverse[row * 4 + 3] * transform[3 * 4 + column]
                + inverse[row * 4 + 2] * transform[2 * 4 + column];
        }
    }
    constants[0xc] = value;
    constants[0xd] = 1.0f;
    constants[0xe] = 0.0f;
    constants[0xf] = 0.0f;
}

// The environment the frame's shaders read: the shadow band and light constants to pixel
// buffer 9; the scene's eight fogs and four default ones (the last three with their colours
// cleared, set white and set grey); the target scale, the shadow factor, the view's forward
// axis and the day/night and run times, all to pixel buffer 5; the weather's colour and
// direction to pixel buffer 7.
// ref: FUN_141353210
void WorldSceneSetEnvironment(uint8_t (*environment)[0xe0], CWorldScene* scene, const float* position,
    const float* view, const float* targetScale, const float* direction, const float* color, float shadowFactor) {
    auto bytes = reinterpret_cast<uint8_t*>(environment);
    std::memcpy(bytes + 0xa80, targetScale, 0x10);
    std::memcpy(bytes + 0xa90, targetScale, 0x10);
    uint8_t lights[0xf0] = {};
    ShaderEffectShadowBandConstants(reinterpret_cast<float*>(lights));
    ShaderEffectShadowBandScale(reinterpret_cast<float*>(lights + 0x60));
    scene->GetLightConstants(view, lights);
    GxRenderContext* context = g_gxRenderContext;
    std::memcpy(context->LockConstants(2, 9, 0xf0), lights, 0xf0);
    context->UnlockConstants(2, 9);
    auto fogs = reinterpret_cast<const uint8_t*>(&scene->m_farClip);
    float block[0xe0 / 4];
    for (uint32_t i = 0; i < 8; i++) {
        std::memcpy(environment[i], ShaderEffectFogConstants(block, ShaderEffectEnvironmentFog(fogs, i), 0.0f,
            position, view), 0xe0);
    }
    for (uint32_t i = 8; i < 12; i++) {
        std::memcpy(environment[i], ShaderEffectDefaultFogConstants(block), 0xe0);
        if (i == 8) {
            continue;
        }
        // Three floats at 0x30, three at 0x44 and six at 0x54.
        const float fill = i == 9 ? 0.0f : i == 10 ? 1.0f : 0.5f;
        auto words = reinterpret_cast<float*>(environment[i]);
        for (uint32_t word : {0xcu, 0xdu, 0xeu, 0x11u, 0x12u, 0x13u, 0x15u, 0x16u, 0x17u, 0x18u, 0x19u, 0x1au}) {
            words[word] = fill;
        }
    }
    *reinterpret_cast<float*>(bytes + 0xab0) = shadowFactor;
    // The view's third row, normalised.
    auto forward = reinterpret_cast<float*>(bytes + 0xac0);
    float inverseLength = 1.0f / std::sqrt(view[9] * view[9] + view[8] * view[8] + view[10] * view[10]);
    forward[0] = view[8] * inverseLength;
    forward[1] = view[9] * inverseLength;
    forward[2] = view[10] * inverseLength;
    auto times = reinterpret_cast<float*>(bytes + 0xaa0);
    float time = static_cast<float>(DayNightGetTime(scene->m_dayNight));
    float noon = 720.0f;  // DAT_142734b0c
    float fromNoon = noon < time ? noon - (time - noon) : time;
    times[0] = time;
    times[1] = time * 0.00069444446f;      // DAT_142734af0
    times[2] = fromNoon * 0.0013888889f;   // DAT_142722ff4
    times[3] = static_cast<float>(FrameTime_GetRunTime()) * 0.001f;  // DAT_142618f58
    std::memcpy(context->LockConstants(2, 5, 0xad0), bytes, 0xad0);
    context->UnlockConstants(2, 5);
    auto weather = reinterpret_cast<uint8_t*>(context->LockConstants(2, 7, 0x20));
    std::memcpy(weather, color, 0x10);
    std::memcpy(weather + 0x10, direction, 0x10);
    context->UnlockConstants(2, 7);
}

// ref: FUN_1413c2d80
void CWorldScene::RunCallbacks(uint32_t stage, void* frame) {
    CRenderCallbackList& callbacks = this->m_callbacks[stage];
    for (CRenderCallback* callback = callbacks.Head(); callback != nullptr; callback = callbacks.Next(callback)) {
        callback->m_function(callback->m_param, callback->m_data, frame);
    }
}

// ref: FUN_1413c7fa0
void CWorldScene::BeginShadingRate(GxRenderContext* context) {
    if (ShaderEffectShadingRateOn() && this->m_19b8 != nullptr) {
        const uint8_t combiners[2] = {0, 1};
        context->SetShadingRateCombiners(combiners);
        context->Slot138();
        FROST_UNIMPLEMENTED();  // the context's shading rate image (+0x138) set to +0x19b8
    }
}

// ref: FUN_1413c8060
void CWorldScene::EndShadingRate(GxRenderContext* context) {
    if (GxVariableRateShadingSupported(2)) {
        const uint8_t combiners[2] = {0, 0};
        context->SetShadingRateCombiners(combiners);
        FROST_UNIMPLEMENTED();  // the context's shading rate image (+0x138) cleared
    }
}
