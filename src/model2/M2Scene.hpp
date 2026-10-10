#ifndef MODEL2_M2_SCENE_HPP
#define MODEL2_M2_SCENE_HPP

// CM2Scene (Engine/Source/Model2/M2Scene.cpp, 0xdb40): the models a world or a UI frame draws
// together, with the camera they are seen through, the lights they are lit by and the lists a
// frame's animation and drawing walk.

#include <cstddef>
#include <cstdint>

#include "blz/Sync.hpp"
#include "model2/M2Element.hpp"
#include "model2/M2Model.hpp"
#include "os/Sync.hpp"
#include "storm/list/TSLink.hpp"
#include "storm/thread/SCritSect.hpp"

struct M2CACHE;
class CM2Lighting;
class GxRenderContext;
class ShaderEffectContext;

// The draw parameters a scene collects per pass (BaseModelDrawParams, 0x20): the model and its
// box (FUN_1411ace60 appends one to a pass's array at scene + 0x328 + pass * 0x18).
struct BaseModelDrawParams {
    CM2BaseModel* m_model;              // 0x00
    float m_box[6];                     // 0x08
};
static_assert(sizeof(BaseModelDrawParams) == 0x20, "BaseModelDrawParams is 0x20 bytes");

// A Storm vector: data, size and capacity, the capacity's top bit set while the data is not
// owned (blz::vector).
template <class T>
struct M2SceneVector {
    T* m_data = nullptr;
    uint64_t m_size = 0;
    uint64_t m_capacity = 0;
};

// The scene's other locked map (0x68): a lock, eight words and a chained hash map.
struct M2SceneLockedMap {
    OSMUTEX m_lock;                             // 0x00
    uint64_t m_08[8];                           // 0x08
    uint64_t m_bucketCount;                     // 0x48 8
    void* m_buckets;                            // 0x50
    uint64_t m_count;                           // 0x58
    float m_maxLoad;                            // 0x60 1
    uint32_t m_pad64;
};
static_assert(sizeof(M2SceneLockedMap) == 0x68, "M2SceneLockedMap is 0x68 bytes");

// The particles' memory (ParticleSystem2.cpp, FUN_14124c1f0): 0x1c0 bytes for each particle a
// scene may hold, and how much of it is taken.
struct M2ParticlePool {
    int32_t m_bytes;                            // 0x00
    uint32_t m_pad04;
    uint8_t* m_data;                            // 0x08
    volatile int32_t m_used;                    // 0x10
    uint32_t m_pad14;
};
static_assert(sizeof(M2ParticlePool) == 0x18, "M2ParticlePool is 0x18 bytes");
// ref: FUN_14124c1f0
void M2ParticlePoolInit(M2ParticlePool* pool, int32_t particles);

// A callback a model queued while its events were collected, run once the frame's animation
// is done (M2ModelCallback, 0x50): kind 0 starts a sequence on a key bone, kind 1 calls
// `m_function`.
struct M2ModelCallback {
    int32_t m_kind;                             // 0x00
    uint32_t m_pad04;
    CM2Model* m_model;                          // 0x08
    uint32_t m_10;                              // 0x10 the key bone, -1 for none
    uint32_t m_14;                              // 0x14
    void* m_function;                           // 0x18
    uint32_t m_20;                              // 0x20 the sequence
    uint32_t m_24;                              // 0x24
    uint8_t m_28[0x10];                         // 0x28
    uint32_t m_38;                              // 0x38 the blend time
    uint32_t m_3c;                              // 0x3c
    float m_40[4];                              // 0x40
};
static_assert(sizeof(M2ModelCallback) == 0x50, "M2ModelCallback is 0x50 bytes");

// One of the scene's 65 draw lists (0x2f8, FUN_1411d0130): fourteen vectors of each kind.
struct M2SceneDrawList {
    CM2Scene* m_scene;                          // 0x000
    M2SceneVector<M2Element> m_elements;        // 0x008
    M2SceneVector<uint32_t> m_lists[0xe];       // 0x020 element indices by pass
    M2SceneVector<uint32_t> m_lists2[0xe];      // 0x170
    M2SceneVector<M2ModelCallback> m_callbacks; // 0x2c0
    float m_screenRect[4];                      // 0x2d8 x0, y0, x1, y1 of its models on screen
    int32_t m_elementCount;                     // 0x2e8 elements made
    uint32_t m_index;                           // 0x2ec its bucket
    // 4 track the screen rectangle, 8 callbacks queued since collected, 0x10 collected again.
    uint32_t m_flags;                           // 0x2f0
    int32_t m_2f4;                              // 0x2f4 summed into the scene's 0xdb30

    M2SceneDrawList();
};
static_assert(sizeof(M2SceneDrawList) == 0x2f8, "M2SceneDrawList is 0x2f8 bytes");

class CM2Scene {
    public:
    // The scene's kind: 5 a UI model frame, 6 the world, 8 ...
    int32_t m_type;                             // 0x0000
    uint32_t m_pad004;
    TSLink<CM2Scene> m_cacheLink;               // 0x0008 on the cache's scene list
    int32_t m_refCount;                         // 0x0018 1
    uint32_t m_pad01c;
    M2CACHE* m_cache;                           // 0x0020
    void* m_owner;                              // 0x0028 the world scene that made it
    void* m_030;                                // 0x0030
    void* m_038;                                // 0x0038
    M2PipelineCache* m_pipelineCacheA;          // 0x0040
    M2PipelineCache* m_pipelineCacheB;          // 0x0048
    M2SceneLockedMap* m_lockedMap;              // 0x0050
    uint64_t m_mapBuckets;                      // 0x0058 8
    void* m_mapData;                            // 0x0060
    uint64_t m_mapCount;                        // 0x0068
    float m_mapLoad;                            // 0x0070 1
    uint32_t m_pad074;
    CM2BaseModel* m_models;                     // 0x0078
    CM2BaseModel* m_released;                   // 0x0080 models whose last reference went this frame
    void* m_088;                                // 0x0088
    void* m_090;                                // 0x0090
    uint32_t m_time;                            // 0x0098 ms, from 300000
    int32_t m_timeStep;                         // 0x009c the last step added to the time
    uint32_t m_frame;                           // 0x00a0
    uint8_t m_0a4;                              // 0x00a4 1
    uint8_t m_pad0a5[3];
    uint32_t m_viewFrame;                       // 0x00a8 the frame the view was last set in
    uint32_t m_0ac;
    uint32_t m_flags;                           // 0x00b0 0x40 no model lighting, 0x200 ...
    float m_0b4;                                // 0x00b4 the object fill's clock
    uint64_t m_0b8;                             // 0x00b8
    CM2Lighting* m_lightings;                   // 0x00c0 linked through +0x90
    uint32_t m_maxLights;                       // 0x00c8 8
    uint32_t m_pad0cc;
    OSMUTEX m_lock0d0;                          // 0x00d0
    SCritSect m_crit0d8;                        // 0x00d8
    OSMUTEX m_locks100[5];                      // 0x0100
    CM2BaseModel* m_buckets[0x40];              // 0x0128
    M2SceneVector<BaseModelDrawParams> m_drawParams[0x40];   // 0x0328 per pass
    M2SceneVector<BaseModelDrawParams> m_drawParams2[0x40];  // 0x0928
    M2ParticlePool m_particles;                 // 0x0f28
    blz::shared_mutex m_sharedLock;             // 0x0f40
    uint64_t m_f60;                             // 0x0f60
    uint64_t m_f68;                             // 0x0f68
    uint64_t m_f70;                             // 0x0f70
    M2SceneVector<M2Element> m_f78;             // 0x0f78 the frame's elements
    M2SceneVector<uint32_t> m_f90[0xe];         // 0x0f90 each pass's elements, in draw order
    M2SceneVector<uint32_t> m_10e0[0xe];        // 0x10e0 each pass's instanced elements
    M2SceneVector<uint32_t> m_1230[0xe];        // 0x1230 the instance run lengths
    M2SceneDrawList m_drawLists[0x41];          // 0x1380
    C44Matrix m_camera;                         // 0xd478 the view, moved to the camera
    C44Matrix m_inverseView;                    // 0xd4b8 the view's inverse
    C44Matrix m_d4f8;                           // 0xd4f8 the projection
    C3Vector m_cameraPosition;                  // 0xd538 also where the wind is sampled from
    float m_viewportMin[3];                     // 0xd544
    float m_viewportMax[3];                     // 0xd550
    uint8_t m_d55c[4];
    void* m_frustum;                            // 0xd560
    float m_d568[4];                            // 0xd568 (0, 0, 0, 1) the fog colour the passes start from
    // The view the passes draw with, latched from the frame's (FUN_1411df600).
    C44Matrix m_drawView;                       // 0xd578
    C44Matrix m_drawInverseView;                // 0xd5b8
    C44Matrix m_drawProjection;                 // 0xd5f8
    C3Vector m_drawCameraPosition;              // 0xd638
    float m_d644[4];                            // 0xd644 set by the world scene
    float m_d654[4];                            // 0xd654
    float m_screenRect[4];                      // 0xd664 its models' on screen
    float m_animateRatio;                       // 0xd674 screen size past which animation is light
    // What draws the projected batches' states, and its parameter (FUN_1411e0780): the world's
    // decals.
    void* m_d678;                               // 0xd678
    void* m_d680;                               // 0xd680
    // Draws a projected batch through the world's decals, and its first argument.
    void (*m_decalCallback)(void* draw);        // 0xd688
    void* m_decalParam;                         // 0xd690
    // Traces a segment against the world: the hit point and how far along it is (1 for none).
    void (*m_traceCallback)(void* query, float* hit, float* t);  // 0xd698
    void* m_d6a0;                               // 0xd6a0
    // The wind: direction, speed and gust (the last argument 0).
    void (*m_windCallback)(float* direction, float* speed, float* gust, void* param);  // 0xd6a8
    uint64_t m_d6b0;                            // 0xd6b0
    // What makes a projected batch's world object (FUN_1411e0c60) and what its query is checked
    // with (FUN_1411e0c70); both are the world scene's.
    void* (*m_projectCallback)(void* query);    // 0xd6b8
    void* m_projectCheckCallback;               // 0xd6c0
    M2SceneVector<uint8_t> m_hitRecs;           // 0xd6c8 0x28-byte M2HitRecs
    int32_t m_d6e0;
    uint32_t m_d6e4;
    int32_t m_d6e8;                             // 0xd6e8
    uint32_t m_d6ec;
    uint8_t m_d6f0[0xd738 - 0xd6f0];
    uint8_t m_d738;                             // 0xd738
    uint8_t m_padd739[3];
    int32_t m_d73c;                             // 0xd73c
    void* m_defaultTexture;                     // 0xd740 FileDataID 449495
    void* m_effectLitOpaque;                    // 0xd748 LitSphere_Additive_Opaque
    void* m_effectLitTransparent;               // 0xd750 LitSphere_Additive_Transparent
    void* m_effectLitNoAlpha;                   // 0xd758 LitSphere_Additive_Transparent_NoAlpha
    void* m_effectShadowMonk;                   // 0xd760
    void* m_effectEdgeGlow;                     // 0xd768
    void* m_effectAvatar;                       // 0xd770
    void* m_effectProcedural;                   // 0xd778
    void* m_effectFlipbook;                     // 0xd780 FlipbookImpostor
    void* m_effectPrepass;                      // 0xd788 Model2PrepassOpaque
    void* m_rayTracing;                         // 0xd790 the physics scene (FUN_14125deb0)
    M2SceneVector<uint32_t> m_d798[0xe];        // 0xd798 the instance grouping tables
    uint64_t m_d8e8[0x40];                      // 0xd8e8 each list's first element once merged
    bool m_collectDrawParams;                   // 0xdae8 1
    bool m_bucketModels;                        // 0xdae9
    uint8_t m_daea[6];
    uint64_t m_daf0;
    uint64_t m_daf8;
    uint64_t m_db00;
    int32_t m_db08;                             // 0xdb08 6, the target kind the passes draw into
    int32_t m_db0c;                             // 0xdb0c additive and alpha-key emitters drawn
    void* m_threadGroupA;                       // 0xdb10
    uint32_t m_db18;
    uint32_t m_bucketCounter;                   // 0xdb1c
    uint8_t m_threadedBuild;                    // 0xdb20 the passes built on the pool
    uint8_t m_db21;
    uint8_t m_db22[6];
    uint64_t m_db28;
    int32_t m_db30;
    uint32_t m_db34;
    void* m_threadGroupB;                       // 0xdb38

    // ref: FUN_1411cf670
    CM2Scene(M2CACHE* cache, int32_t type);
    // The kind, the owner and its two companions, the reserves for the kind, and the scene's
    // textures and effects when there is a renderer to make them with (FUN_1411ddca0).
    void Initialize(int32_t type, void* owner, void* a4, void* a5, bool renderer);
    // Room for `count` entries in the 0x88-byte list and half as many in each per-pass index
    // list (FUN_1411df730).
    void Reserve(uint32_t count);
    // The models' batch effects looked up again and the scene's textures and effects made, when
    // a device comes up (FUN_1411d8ee0), and all of it released when it goes (FUN_1411d92a0).
    void RecreateDeviceObjects();
    void ReleaseDeviceObjects();
    // A model of a file by FileDataID (FUN_1411d8c80) or by path (FUN_1411d8d50), initialized
    // into the scene at `lod`; null when the file or the model cannot be had.
    CM2Model* CreateModel(int32_t fileDataId, uint32_t flags, uint8_t priority, uint8_t lod);
    // Whether a model file and its textures are on disk; what is missing is asked for.
    // ref: FUN_1411e40e0
    bool ModelFileReady(const char* name, uint8_t priority);
    // Reads a model file ahead, for `waiter` when given.
    // ref: FUN_1411cf210
    bool PreloadModel(int32_t fileDataId, uint8_t priority, void* waiter);
    CM2Model* CreateModel(const char* name, uint32_t flags, uint8_t priority, uint8_t lod);
    // A frame's animation over the buckets (FUN_1411d2180), with a frustum (FUN_1411d2150).
    void Animate(bool threaded);
    void Animate(void* frustum, bool threaded);
    // The models whose LOD changed told so, and the list emptied (FUN_1411d77a0).
    void FlushLodChanges();
    // The frame's draw elements from the passes' draw params (FUN_1411d3940); `first` and
    // `second` set the lists' two flags.
    bool BuildElements(uint32_t first, int32_t second);
    // The built lists merged into the scene's elements and pass lists, the instanced elements
    // grouped, and the passes sorted (FUN_1411de670, FUN_1411e3890).
    void FinishElements(uint32_t threaded, uint32_t lists, uint32_t which);
    void SortPasses(uint32_t half);
    // Runs the callbacks the lists queued: one list's, or all of them for null (FUN_1411d74a0).
    void RunCallbacks(M2SceneDrawList* list);
    // The view for a frame (FUN_1411e0f30): the camera's position, the view and projection
    // matrices and the viewport's corners; `advance` steps the frame.
    // A pass's elements drawn (FUN_1411d9460), or one part of `parts` of its plain
    // (FUN_1411dbf60) or instanced (FUN_1411daef0) elements; `shader` holds the constants the
    // draws start from.
    void DrawPass(GxRenderContext* context, ShaderEffectContext* shader, uint8_t pass);
    void DrawPassPart(GxRenderContext* context, ShaderEffectContext* shader, uint8_t pass, uint8_t parts, uint8_t part);
    void DrawPassInstancedPart(GxRenderContext* context, ShaderEffectContext* shader, uint8_t pass, uint8_t parts,
        uint8_t part);
    // The frame's view becomes the one the passes draw with (FUN_1411df600).
    void LatchView();
    void SetView(const C3Vector* cameraPosition, const C44Matrix* view, const C44Matrix* projection,
        const float* viewportMin, const float* viewportMax, bool advance);
    // Every model told the render settings changed (FUN_1411df6e0).
    void OnRenderSettingsChanged(const struct RENDERSETTINGS* previous, const struct RENDERSETTINGS* current);
};
static_assert(offsetof(CM2Scene, m_time) == 0x98, "CM2Scene time at 0x98");
static_assert(offsetof(CM2Scene, m_lock0d0) == 0xd0, "CM2Scene locks at 0xd0");
static_assert(offsetof(CM2Scene, m_locks100) == 0x100, "CM2Scene locks at 0x100");
static_assert(offsetof(CM2Scene, m_buckets) == 0x128, "CM2Scene buckets at 0x128");
static_assert(offsetof(CM2Scene, m_particles) == 0xf28, "CM2Scene particle pool at 0xf28");
static_assert(offsetof(CM2Scene, m_sharedLock) == 0xf40, "CM2Scene shared lock at 0xf40");
static_assert(offsetof(CM2Scene, m_f78) == 0xf78, "CM2Scene list at 0xf78");
static_assert(offsetof(CM2Scene, m_d798) == 0xd798, "CM2Scene vectors at 0xd798");
static_assert(offsetof(CM2Scene, m_defaultTexture) == 0xd740, "CM2Scene texture at 0xd740");
static_assert(offsetof(CM2Scene, m_drawLists) == 0x1380, "CM2Scene draw lists at 0x1380");
static_assert(offsetof(CM2Scene, m_camera) == 0xd478, "CM2Scene camera at 0xd478");
static_assert(offsetof(CM2Scene, m_frustum) == 0xd560, "CM2Scene frustum at 0xd560");
static_assert(offsetof(CM2Scene, m_animateRatio) == 0xd674, "CM2Scene ratio at 0xd674");
static_assert(offsetof(CM2Scene, m_screenRect) == 0xd664, "CM2Scene screen rectangle at 0xd664");
static_assert(offsetof(CM2Scene, m_projectCallback) == 0xd6b8, "CM2Scene projection at 0xd6b8");
static_assert(offsetof(M2SceneDrawList, m_flags) == 0x2f0, "a draw list's flags at 0x2f0");
static_assert(offsetof(CM2Scene, m_hitRecs) == 0xd6c8, "CM2Scene hits at 0xd6c8");
static_assert(offsetof(CM2Scene, m_rayTracing) == 0xd790, "CM2Scene physics at 0xd790");
static_assert(offsetof(CM2Scene, m_collectDrawParams) == 0xdae8, "CM2Scene flags at 0xdae8");
static_assert(offsetof(CM2Scene, m_threadGroupB) == 0xdb38, "CM2Scene thread group at 0xdb38");
static_assert(sizeof(CM2Scene) == 0xdb40, "CM2Scene is 0xdb40 bytes");


inline float M2SceneGetAnimateRatio(CM2Scene* scene) {
    return scene->m_animateRatio;
}

inline bool M2SceneHasWind(CM2Scene* scene) {
    return scene->m_windCallback != nullptr;
}

inline const float* M2SceneGetWindOrigin(CM2Scene* scene) {
    return &scene->m_cameraPosition.x;
}

inline void M2SceneSampleWind(CM2Scene* scene, float* direction, float* speed, float* gust) {
    scene->m_windCallback(direction, speed, gust, nullptr);
}

// The depth-stencil states the scene's passes draw with (DAT_1430a1040, eight passes of four
// pairs) and the stencil references of the outline passes (DAT_1430a1240, DAT_1430a1250), made
// when the device comes up (FUN_1411d9090).
extern uint64_t g_m2DepthStencil[8][8];
// The blend a pass draws a blend mode with (DAT_142721290), the blend words (DAT_142cfb1a8), the
// blend modes drawn opaque by the lit sphere (DAT_142721268) and a blend mode's effect-pass
// blend (DAT_142721390).
extern const int32_t g_m2PassBlend[14][8];
extern const uint16_t g_m2BlendWords[9];
extern const int32_t g_m2BlendOpaque[8];
extern const int32_t g_m2EffectBlend[8];
extern uint32_t g_m2OutlineStencil[3];
extern uint32_t g_m2OutlineStencilFill[3];
void M2SceneStatesInitialize();
// Whether the outline passes are on (DAT_1430a125c): turned on or off (FUN_1411dd600), and off
// (FUN_1411d9280).
extern bool g_m2OutlineEnabled;
void M2SceneSetOutline(bool enabled);
void M2SceneClearOutline();
// The passes drawn (DAT_142cfb1a0, all of them) and a Gx byte the custom model draws clear
// around their call (DAT_14348b641).
extern uint32_t g_m2DrawPassMask;
extern uint8_t g_gx8b641;
// The draws' compiled-out trace (FUN_1411a61b0).
void M2SceneDebugLog(GxRenderContext* context, const char* format, ...);

// A scene of a kind, made, set up and put on the cache's list (FUN_141196e40). `owner` is what
// made it; the two after it are kept beside it.
CM2Scene* M2CacheCreateScene(int32_t type, void* owner, void* a3, void* a4);

// The physics world a scene of a kind keeps (Physics.cpp, FUN_14125deb0): kind 1 for types 1
// and 5, 2 for 8, 3 for 6, none for the rest.
void* PhysicsWorldCreate(int32_t kind);

// The scene's ray tracing (FUN_1411ddad0) and a model's entry in it (FUN_141260140).
void* M2SceneGetRayTracing(CM2Scene* scene);
void RayTracingRegisterModel(void* rayTracing, void* entry, uint32_t size);
// A registered model's instance marked for an update (FUN_14125ff50): `entry` is {index, state}.
void RayTracingMarkDirty(void* rayTracing, const int32_t* entry);
// An instance's matrix (FUN_14125fb10), and an instance's (FUN_14125fab0) and a model's
// (FUN_14125ffb0) removal.
void RayTracingUpdateInstance(void* rayTracing, const int32_t* entry, const float* matrix);
void RayTracingRemoveInstance(void* rayTracing, const int32_t* entry);
void RayTracingRemoveModel(void* rayTracing, const int32_t* entry);

// What a projected batch's draw callbacks are handed (0x50): the projection's world object or
// the scene's decal parameter, the scene's owner and companion, the context, the constants, the
// element, the projection's box, the batch's colour and its flags.
struct M2PROJECTEDDRAW {
    void* m_object;                     // 0x00
    void* m_owner;                      // 0x08
    void* m_030;                        // 0x10
    GxRenderContext* m_context;         // 0x18
    ShaderEffectContext* m_shader;      // 0x20
    M2Element* m_element;               // 0x28
    uint8_t m_box[0x18];                // 0x30
    uint32_t m_color;                   // 0x48 0xffRRGGBB
    uint32_t m_flags;                   // 0x4c 1 lit, 2 blended, 4 two textures, then the projection's
};
static_assert(sizeof(M2PROJECTEDDRAW) == 0x50, "a projected draw is 0x50 bytes");

// The world scene's hooks into the M2 scene (set by CWorldScene's constructor).
void M2SceneSetProjectCallback(CM2Scene* scene, void* (*callback)(void* query));
void M2SceneSetProjectCheckCallback(CM2Scene* scene, void (*callback)(void* draw));
void M2SceneSetTraceCallback(CM2Scene* scene, void (*callback)(void* query, float* hit, float* t), void* param);
void M2SceneSetPhysicsCallback(CM2Scene* scene, void (*callback)(void* states, void* param), void* param);
void M2SceneSetWindCallback(CM2Scene* scene, void (*callback)(const float* position, float* direction,
    float* speed, uint8_t gusts));
void M2SceneSetGroundCallback(CM2Scene* scene, bool (*callback)(const float* position, float* height, void* param));
// Drops the world scene's reference (the cache frees the scene once nothing holds it).
void M2SceneRelease(CM2Scene* scene);

// A copy of a model in the scene (FUN_1411dd490): the same file and state, not yet attached.
CM2BaseModel* M2SceneCloneModel(CM2Scene* scene, CM2BaseModel* model, bool a3);

// ref: FUN_1411ace60
BaseModelDrawParams* M2SceneAddDrawParams(void* array);
// A draw list emptied for a new frame (FUN_1411d75f0), and freed (FUN_1411d0cb0).
void M2SceneDrawListReset(M2SceneDrawList* list);
void M2SceneDrawListFree(M2SceneDrawList* list);
// A list's bucket's models: their callbacks collected (FUN_1411dd760), animated
// (FUN_1411d3410), and lit and unlinked once animated (FUN_1411de480).
void M2SceneDrawListCollectCallbacks(M2SceneDrawList* list);
void M2SceneDrawListAnimate(M2SceneDrawList* list);
void M2SceneDrawListPostAnimate(M2SceneDrawList* list);
// A model's animated box on screen, merged into `rect` (FUN_1411d6fc0).
void M2SceneProjectScreenRect(CM2BaseModel* model, const C44Matrix* view, const C44Matrix* projection,
    const float* viewportMin, const float* viewportMax, float* rect);
// Appends a callback (FUN_1411cee10).
void M2ModelCallbacksPush(M2SceneVector<M2ModelCallback>* callbacks, const M2ModelCallback* callback);
// Sorts element indices by a three-way compare over the elements (FUN_141205120, the introsort
// FUN_141204aa0).
void M2SortIndices(int32_t (*compare)(uint32_t a, uint32_t b, const M2Element* elements), uint32_t* indices,
    uint64_t count, const M2Element* elements);
// A blend mode's index among the eight M2 blends (FUN_1411d81a0).
uint32_t M2BlendModeIndex(uint8_t blend);
// Resizes a callback vector, zeroing what it adds (FUN_1411e5770).
void M2ModelCallbacksResize(M2SceneVector<M2ModelCallback>* callbacks, uint64_t count);
// A box against a frustum's six planes: 0 when outside, else 3 (FUN_141346830).
int32_t M2FrustumTestBox(const float* planes, const float* box);
// The scene's lights into a model's (FUN_1411df990).
void M2SceneGatherLights(CM2Scene* scene, CM2ModelLighting* lighting);

#endif
