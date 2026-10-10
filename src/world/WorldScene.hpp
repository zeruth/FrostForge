#ifndef WORLD_WORLD_SCENE_HPP
#define WORLD_WORLD_SCENE_HPP

// WoW/Source/WorldClient2/WorldScene.cpp: CWorldScene (0x1a10 bytes), the world renderer's
// scene. The game world has one; every model frame of the UI makes another (type 5, with the
// default lighting of no map) so its model is drawn, lit and fogged the way the world draws it.
// A scene owns its CM2Scene, its views (CWorldView, one per camera), the scene renders those
// views draw through (CWorldSceneRender), up to two maps, the fog and light state the views
// share, and the callbacks other systems hook into its frame.
//
// Much of the layout is only known from the constructor's stores; those fields are named by
// offset until a reader says what they hold.

#include "blz/Vector.hpp"
#include "model2/CM2Lighting.hpp"
#include "client/FrameBufferCache.hpp"
#include "storm/list/TSList.hpp"
#include "world/ModelFog.hpp"
#include <cstddef>
#include <cstdint>

class CM2Scene;
class CWorldMap;
class CWorldView;
class CWorldSceneRender;
class CParticulateVolumeManager;
struct DAYNIGHT;
struct THREADPOOLGROUP;

extern const char s_worldViewPtrTag[];          // ".PEAVCWorldView@@"
extern const char s_worldSceneRenderPtrTag[];   // ".PEAVCWorldSceneRender@@"
extern const char s_intTag[];                   // ".H"

// A function a system hooks into one of the scene's frame stages (0x30): called with its
// parameter, its sixteen bytes of data and the frame.
class CRenderCallback {
    public:
    void (*m_function)(void* param, void* data, void* frame);  // 0x00
    void* m_param;                                              // 0x08
    uint8_t m_data[0x10];                                       // 0x10
    TSLink<CRenderCallback> m_link;                             // 0x20
};
static_assert(sizeof(CRenderCallback) == 0x30, "CRenderCallback is 0x30 bytes");

// TSList<CRenderCallback, TSGetExplicitLink<CRenderCallback>, 8>: the link offset is a field.
typedef TSList<CRenderCallback, TSGetLink<CRenderCallback>, 8> CRenderCallbackList;

class CWorldScene {
    public:
    // The callback lists, by stage. The frame (FUN_1413c5250) runs 0 before the M2 scene
    // draws, 1 after it and 2 at the end; the animate pass (FUN_1413c80a0) runs 5.
    enum : uint32_t {
        CALLBACK_COUNT = 6,
    };

    TSLink<CWorldScene> m_link;                     // 0x000 every scene (FUN_1413cac90)
    CWorldMap* m_maps[2];                           // 0x010
    uint32_t m_mapIndex;                            // 0x020 the map m_maps holds the current one at
    uint32_t m_pad024;
    DAYNIGHT* m_dayNight;                           // 0x028 the map's sky and light (FUN_142442100)
    void* m_030;                                    // 0x030 0xfa50 bytes (FUN_141423ae0)
    void* m_038;                                    // 0x038 0x1378 bytes (FUN_14138a820): the lights
    void* m_040;                                    // 0x040 0x20 bytes (FUN_14143bcc0 frees it)
    CParticulateVolumeManager* m_particulates;      // 0x048
    CM2Scene* m_m2Scene;                            // 0x050
    void* m_058;                                    // 0x058 0x178 bytes (FUN_141370120)
    uint8_t m_060;                                  // 0x060
    uint8_t m_pad061[7];
    CWorldView* m_view;                             // 0x068 the scene's own view
    blz::vector<CWorldView*, s_worldViewPtrTag> m_views;                     // 0x070
    blz::vector<CWorldSceneRender*, s_worldSceneRenderPtrTag> m_renders;     // 0x088
    THREADPOOLGROUP* m_groups[4];                   // 0x0a0
    float m_farClip;                                // 0x0c0 946
    float m_nearClip;                               // 0x0c4 0.2
    uint32_t m_0c8[4];                              // 0x0c8
    MODELFOG m_fogs[8];                             // 0x0d8 one per view slot (FUN_1413520c0)
    MODELFOG m_fog;                                 // 0x5d8
    uint32_t m_678;                                 // 0x678
    uint8_t m_67c[4];                               // 0x67c 0, 1, 1, 1; [0] the view sees occluders
    uint16_t m_680;                                 // 0x680
    uint8_t m_pad682[6];
    blz::vector<int32_t, s_intTag> m_688;           // 0x688
    uint32_t m_6a0;                                 // 0x6a0 1
    uint32_t m_6a4;
    uint32_t m_6a8;                                 // 0x6a8 the vertex array's count
    uint32_t m_pad6ac;
    void* m_vertices;                               // 0x6b0 TSFixedArray<GxVertexPNCT2> storage
    uint32_t m_6b8;                                 // 0x6b8
    uint32_t m_pad6bc;
    uint32_t m_6c0;                                 // 0x6c0
    uint32_t m_pad6c4;
    uint64_t m_6c8;                                 // 0x6c8
    uint32_t m_6d0;                                 // 0x6d0
    uint32_t m_pad6d4;
    void* m_6d8;                                    // 0x6d8 type 6 only: 0x7878 bytes (FUN_141439100)
    void* m_6e0;                                    // 0x6e0 type 6 only: 0x38 bytes (FUN_14143a9e0)
    uint64_t m_6e8;                                 // 0x6e8
    uint32_t m_6f0;                                 // 0x6f0
    float m_position[3];                            // 0x6f4 the camera's, as the scene last saw it
    void* m_700;                                   // 0x700 an object with a position at +0x7c
    uint64_t m_708[2];                              // 0x708
    float m_718[16];                                // 0x718 identity
    float m_758[16];                                // 0x758 identity
    CRenderCallbackList m_callbacks[CALLBACK_COUNT];    // 0x798
    float m_828[6][4];                              // 0x828 (0, 0, 1, 0) each
    uint8_t m_888[0x60];                            // 0x888
    uint32_t m_8e8;                                 // 0x8e8
    float m_lodPosition[3];                         // 0x8ec where the dynamic LOD last moved
    uint32_t m_frameTimeIndex;                      // 0x8f8 into m_frameTimes, wraps at 60
    uint32_t m_frameTimeSum;                        // 0x8fc
    uint32_t m_frameTimes[60];                      // 0x900
    float m_lodScale;                               // 0x9f0 1
    uint32_t m_9f4;                                 // 0x9f4
    uint32_t m_9f8;                                 // 0x9f8
    float m_9fc[3];                                 // 0x9fc FLT_MAX
    int32_t m_a08;                                  // 0xa08 -1
    uint8_t m_a0c[0x20];                            // 0xa0c
    float m_a2c[3];                                 // 0xa2c FLT_MAX
    int32_t m_a38;                                  // 0xa38 -1
    uint8_t m_a3c[0x18];                            // 0xa3c
    uint8_t m_a54;                                  // 0xa54 bits 0-1
    uint8_t m_pada55[3];
    float m_farClips[3];                            // 0xa58 705 each; the far clip is their max
    float m_a64;                                    // 0xa64 the occlusion's depth
    float m_a68;                                    // 0xa68
    float m_a6c;                                    // 0xa6c 400
    uint64_t m_a70;                                 // 0xa70 the view's 0x300
    uint32_t m_a78;                                 // 0xa78 the view's 0x308
    uint32_t m_pada7c;
    void* m_a80;                                    // 0xa80 deleted through its vtable's +8
    CM2Lighting m_lightA88;                         // 0xa88
    uint8_t m_b20[0x100];                           // 0xb20 the lights gathered (FUN_141200000)
    uint64_t m_c20;                                 // 0xc20 8; 0xc24 1 or 7
    uint64_t m_c28;                                 // 0xc28
    float m_c30;                                    // 0xc30 1
    uint32_t m_c34;
    uint64_t m_c38;
    uint64_t m_c40;                                 // 0xc40
    uint64_t m_c48;                                 // 0xc48
    uint64_t m_c50;                                 // 0xc50
    uint32_t m_c58;                                 // 0xc58
    uint32_t m_padc5c;
    CM2Lighting m_lightC60;                         // 0xc60
    uint8_t m_cf8;                                  // 0xcf8 bits 0-1 cleared, bit 2 set
    uint8_t m_padcf9[7];
    uint64_t m_d00;                                 // 0xd00
    uint64_t m_d08;                                 // 0xd08
    uint64_t m_d10;                                 // 0xd10
    uint32_t m_d18;                                 // 0xd18
    uint32_t m_padd1c;
    CM2Lighting m_lightD20;                         // 0xd20
    uint8_t m_db8;                                  // 0xdb8
    uint8_t m_paddb9[7];
    uint64_t m_dc0;                                 // 0xdc0
    uint64_t m_dc8;                                 // 0xdc8
    uint64_t m_dd0;                                 // 0xdd0
    uint32_t m_dd8;                                 // 0xdd8
    uint32_t m_paddc;
    CM2Lighting m_lightDE0;                         // 0xde0
    uint8_t m_e78;                                  // 0xe78
    uint8_t m_pade79[7];
    uint8_t m_e80[12][0xe0];                        // 0xe80 (FUN_141353210)
    uint64_t m_1900[6];                             // 0x1900
    uint64_t m_1930[2];
    uint64_t m_1940;                                // 0x1940
    uint32_t m_1948;                                // 0x1948
    uint32_t m_pad194c;
    void* m_1950;                                   // 0x1950 type 6 only: 0x68 bytes (FUN_1412a5ce0)
    float m_screenRect[4];                          // 0x1958 the M2 scene's, after the frame
    uint32_t m_1968;                                // 0x1968
    float m_196c;                                   // 0x196c 1
    uint8_t m_1970[0x24];                           // 0x1970
    uint32_t m_frame;                               // 0x1994 counts frames, from 1
    int32_t m_refCount;                             // 0x1998
    volatile long m_busy;                           // 0x199c a render is in flight
    int32_t m_type;                                 // 0x19a0 5 a model frame's, 6 the world's
    uint32_t m_19a4;                                // 0x19a4 8
    void* m_19a8;                                   // 0x19a8 released by FUN_1422883e0
    void* m_19b0;                                   // 0x19b0 released by FUN_1422883e0
    void* m_19b8;                                   // 0x19b8 (FUN_1412af4e0)
    void* m_19c0;                                   // 0x19c0 released by FUN_142288400
    void* m_19c8;                                   // 0x19c8
    void* m_19d0;                                   // 0x19d0
    FRAMEBUFFERTEMP m_temp;                         // 0x19d8

    // ref: FUN_1413c17f0
    CWorldScene(uint32_t flags, int32_t type);
    // ref: FUN_1413c24e0
    ~CWorldScene();

    // ref: FUN_1413c29d0
    void AddRef();
    // ref: FUN_1413c5bb0
    int32_t Release();

    // A view of the scene (0x718), linked into m_views; flags 1 a reflection, 2 and 4 drop
    // view features.
    // ref: FUN_1413c36c0
    CWorldView* CreateView(uint32_t flags);
    // The map in a slot: map -1 is no map, which is what a model frame's scene loads.
    // ref: FUN_1413c35c0
    void LoadMap(uint32_t slot, int32_t mapId, bool a4, uint8_t a5, int32_t a6, uint8_t a7);
    // Drops a map slot's map and what was made for it.
    // ref: FUN_1413c3a40
    void UnloadMap(uint32_t slot);
    // The far and near clip from the map and the CVars, and the camera's position.
    // ref: FUN_1413c88f0
    void SetCameraPosition(const float* position);
    // The scene's map objects, sky, lights and particle systems, made for the current map.
    // ref: FUN_1413c31f0
    void CreateMapObjects();
    // The far clip: the CVar's, against the map's, clamped to m_farClips.
    // ref: FUN_1413c2bc0
    float GetFarClip(CWorldMap* map);
    // ref: FUN_1413c2c20
    float GetFarClip2(CWorldMap* map);
    // The third: the CVar under the map's caps, kept under the first.
    // ref: FUN_1413c2c80
    float GetFarClip3(CWorldMap* map);
    // ref: FUN_1413c2d60
    float GetNearClip();

    // Empties the callback lists (the constructor's inlined TSList clear).
    void ClearCallbacks();
};
static_assert(offsetof(CWorldScene, m_m2Scene) == 0x50, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_view) == 0x68, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_farClip) == 0xc0, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_fogs) == 0xd8, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_fog) == 0x5d8, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_6d8) == 0x6d8, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_position) == 0x6f4, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_718) == 0x718, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_callbacks) == 0x798, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_828) == 0x828, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_8e8) == 0x8e8, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_lodScale) == 0x9f0, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_farClips) == 0xa58, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_lightA88) == 0xa88, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_c20) == 0xc20, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_lightC60) == 0xc60, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_lightD20) == 0xd20, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_lightDE0) == 0xde0, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_e80) == 0xe80, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_1950) == 0x1950, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_frame) == 0x1994, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_type) == 0x19a0, "CWorldScene layout");
static_assert(offsetof(CWorldScene, m_temp) == 0x19d8, "CWorldScene layout");
static_assert(sizeof(CWorldScene) == 0x1a10, "CWorldScene is 0x1a10 bytes");

// The scene's environment: its clip distances, fogs and light block (+0xc0), what the M2
// scenes and DayNight write into.
// The far clip and the third far clip of a scene's current map, or the CVars' without a scene.
// ref: FUN_14134e6c0
float WorldSceneFarClip(CWorldScene* scene);
// ref: FUN_14134e860
float WorldSceneFarClip3(CWorldScene* scene);

// ref: FUN_141357060
void* WorldSceneGetEnv(CWorldScene* scene);

// The scene being drawn into, when one is (DAT_1430ad2f8).
// ref: FUN_1413cb140
CWorldScene* WorldSceneGetCurrent();
// ref: FUN_1413cb310
void WorldSceneSetCurrent(CWorldScene* scene);

#endif
