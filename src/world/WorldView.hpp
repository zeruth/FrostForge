#ifndef WORLD_WORLD_VIEW_HPP
#define WORLD_WORLD_VIEW_HPP

// WoW/Source/WorldClient2/WorldView.cpp: CWorldView (0x718 bytes), one camera's view of a
// world scene. It holds the camera (three copies of the camera block: as set, as of the last
// frame, and current), the matrices built from it, the scene render it draws through, its
// culling hub (0x16578 bytes) and its scene occlusion. A view can make child views for
// reflections (FUN_1413db650).
//
// The view's first word is its flag set, the "params" the model frames switch features in
// (0x3fbf640a off, 0x1000 on, so a model frame's view runs with 0x40409021).

#include "os/Sync.hpp"
#include "storm/list/TSList.hpp"
#include <cstddef>
#include <cstdint>

class CWorldScene;
class CWorldSceneRender;
class CWorldCull;
class SceneOcclusion;
struct THREADPOOLGROUP;

// One camera: where it is, what it looks at, its clips and its lens (0x30).
struct WORLDVIEWCAMERA {
    float m_position[3];            // 0x00
    float m_target[3];              // 0x0c
    float m_nearClip;               // 0x18 0.2
    float m_farClip;                // 0x1c 4400
    float m_farClip2;               // 0x20 4400
    float m_24;                     // 0x24 1300
    float m_fov;                    // 0x28 0.9425 (54 degrees)
    float m_aspect;                 // 0x2c 16:9
};
static_assert(sizeof(WORLDVIEWCAMERA) == 0x30, "WORLDVIEWCAMERA is 0x30 bytes");

// A locked list the view collects into (0x30): the lock, a list, and a few words.
struct WORLDVIEWLIST {
    OSMUTEX m_lock;                 // 0x00
    uint32_t m_linkoffset;          // 0x08
    uint32_t m_pad0c;
    void* m_head[2];                // 0x10 the terminator: prevlink, next (tagged)
    THREADPOOLGROUP* m_group;       // 0x20 the jobs filling it
    uint16_t m_28;                  // 0x28
    uint16_t m_held;                // 0x2a nonzero: nothing may be added
    uint16_t m_2c;                  // 0x2c
    uint16_t m_pad2e;
};
static_assert(sizeof(WORLDVIEWLIST) == 0x30, "WORLDVIEWLIST is 0x30 bytes");

// Waits for the list's jobs and empties it.
// ref: FUN_1413a2ad0
void WorldViewListClear(WORLDVIEWLIST* list);
// ref: FUN_141364bb0
void WorldViewListDestroy(WORLDVIEWLIST* list);

// What a view gathered of a kind (0x28), cleared by the scene's UnloadMap.
struct WORLDVIEWGATHER {
    void* m_00;                     // 0x00
    uint32_t m_08;                  // 0x08
    uint8_t m_0c[0x10];
    uint32_t m_1c;                  // 0x1c
    uint8_t m_20;                   // 0x20
    uint8_t m_pad21[7];
};
static_assert(sizeof(WORLDVIEWGATHER) == 0x28, "WORLDVIEWGATHER is 0x28 bytes");

class CWorldView {
    public:
    uint32_t m_flags;                       // 0x000
    uint32_t m_index;                       // 0x004
    uint32_t m_renderIndex;                 // 0x008 its render's slot in the scene's list
    uint32_t m_pad00c;
    CWorldScene* m_scene;                   // 0x010
    CWorldView* m_parent;                   // 0x018
    CWorldView* m_children[2];              // 0x020 reflections (FUN_1413db650)
    CWorldSceneRender* m_render;            // 0x030
    CWorldScene* m_038;                     // 0x038 the scene again
    CWorldView* m_040;                      // 0x040 itself
    uint32_t m_048;                         // 0x048 0, 1 (flag 2) or 2 (flag 4)
    uint32_t m_04c;                         // 0x04c 6, or 0x14 with mode flag 2
    uint8_t m_050[12][0x10];                // 0x050
    uint32_t m_110[12][6];                  // 0x110
    uint32_t m_230;                         // 0x230
    uint32_t m_234;
    void* m_238;                            // 0x238 (FUN_1413aa540)
    CWorldCull* m_cull;                     // 0x240
    WORLDVIEWGATHER m_gathered[4];          // 0x248
    uint32_t m_2e8;                         // 0x2e8
    uint32_t m_2ec;                         // 0x2ec
    uint32_t m_2f0;                         // 0x2f0
    uint32_t m_2f4;
    SceneOcclusion* m_occlusion;            // 0x2f8 (the SceneOcclusion handle is this pointer)
    uint64_t m_300;                         // 0x300
    uint32_t m_308;                         // 0x308
    uint8_t m_30c[0x84];                    // 0x30c
    float m_planes[6][4];                   // 0x390 (0, 0, 0, 1) each
    uint8_t m_3f0[0x80];                    // 0x3f0
    uint32_t m_470;                         // 0x470
    float m_scale[3];                       // 0x474 1 each
    WORLDVIEWCAMERA m_cameraSet;            // 0x480 as the caller set it (FUN_1413de160)
    WORLDVIEWCAMERA m_cameraLast;           // 0x4b0 last frame's (FUN_1413c59b0)
    WORLDVIEWCAMERA m_camera;               // 0x4e0 this frame's
    float m_matrices[6][16];                // 0x510 0x550 view at the origin, 0x610 view, 0x650 projection
    uint32_t m_690;                         // 0x690 nonzero: occluders seen
    uint32_t m_694;                         // 0x694
    uint8_t m_698;                          // 0x698
    uint8_t m_pad699[3];
    uint32_t m_69c;                         // 0x69c
    uint32_t m_6a0;                         // 0x6a0
    float m_clipPlane[4];                   // 0x6a4 (0, 0, 1, 0)
    int32_t m_6b4;                          // 0x6b4 4
    WORLDVIEWLIST m_lists[2];               // 0x6b8

    // ref: FUN_1413d9420
    CWorldView(CWorldScene* scene, CWorldView* parent, uint32_t mode);
    // The occluder object (+0x238) made or dropped as occlusion culling is switched.
    // ref: FUN_1413da870
    void SetOcclusionCulling(bool enabled);
    // The camera block from the set one, its matrices and the LOD distances.
    // ref: FUN_1413dddd0
    void UpdateCamera(const int32_t* lod);
    // ref: FUN_1413de3a0
    void BuildFrustum();
    // ref: FUN_1413de760
    void Cull();
    // ref: FUN_1413da610
    void UpdateOccluders(bool occluders);
    // ref: FUN_1413dddb0
    void SetOcclusion(bool enabled);
    // ref: FUN_1413dea50
    bool CullsSkipped();
    // The camera the caller sets: position, target, the clips from the scene, the lens.
    // ref: FUN_1413de160
    void SetCamera(const float* position, const float* target, float fov, float aspect);
    // This frame's matrices and camera kept as last frame's.
    // ref: FUN_1413c59b0
    void LatchCamera();
    // ref: FUN_1413de240
    void UpdateDetailTarget();
    // ref: FUN_1413da730
    void CullFinish();
    // The view's list `index` run as pool jobs.
    // ref: FUN_1413dd130
    void RunList(uint32_t index);
    // ref: FUN_1413da5b0
    void ClearLists();
    // ref: FUN_1413db650
    CWorldView* GetChild(uint32_t index);
    // ref: FUN_1413dcc10
    bool WantsReflection();
    // ref: FUN_1413dcc30
    bool WantsRefraction();
    // ref: FUN_1413db0c0
    float WaterHeight();
    // The liquid the camera is in and its depth (+0x690, +0x694).
    // ref: FUN_141351b90
    uint32_t GetLiquid(float* depth);
    // The water around the camera: its fog block, whether a liquid covers the camera and how
    // deep; 1 when the camera is under it.
    // ref: FUN_1413dd190
    int32_t QueryWater(uint8_t* fog, uint8_t* covered, float* depth);
    // ref: FUN_1413d9ed0
    ~CWorldView();
};
static_assert(offsetof(CWorldView, m_render) == 0x30, "CWorldView layout");
static_assert(offsetof(CWorldView, m_cull) == 0x240, "CWorldView layout");
static_assert(offsetof(CWorldView, m_occlusion) == 0x2f8, "CWorldView layout");
static_assert(offsetof(CWorldView, m_planes) == 0x390, "CWorldView layout");
static_assert(offsetof(CWorldView, m_scale) == 0x474, "CWorldView layout");
static_assert(offsetof(CWorldView, m_camera) == 0x4e0, "CWorldView layout");
static_assert(offsetof(CWorldView, m_matrices) == 0x510, "CWorldView layout");
static_assert(offsetof(CWorldView, m_690) == 0x690, "CWorldView layout");
static_assert(offsetof(CWorldView, m_lists) == 0x6b8, "CWorldView layout");
static_assert(sizeof(CWorldView) == 0x718, "CWorldView is 0x718 bytes");

#endif
