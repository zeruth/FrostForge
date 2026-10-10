#ifndef MODEL2_M2_MODEL_HPP
#define MODEL2_M2_MODEL_HPP

// CM2BaseModel (Engine/Source/Model2/BaseModel.cpp, 0x3a8) and CM2Model (M2Model.cpp, in a
// 0x690 ObjectAlloc slot). A base model is anything a scene places: it keeps its matrices,
// bounds, the model it is attached to and the models attached to it, and the calls made on it
// before it was loaded, which run in order once it is. A CM2Model is one instance of a shared
// .m2 file with its own animation state, bones and draw data; made before its file is in, it
// waits on the file's list and is initialized when the file is.
//
// The virtual functions are declared in the reference's vtable order (CM2BaseModel 0x142721a60,
// CM2Model 0x142720d70, 66 slots); callers reach most of them through the vtable.

#include <cstddef>
#include <cstdint>

#include "model2/CM2Lighting.hpp"
#include "model2/M2Data.hpp"
#include "model2/M2Track.hpp"

struct CM2Shared;
class CM2BaseModel;
class CM2Scene;
class CM2Model;
class CM2Camera;
class TObjectAllocHeap;
class CParticleEmitter2;
class CRibbonEmitter;
class CTexture;

struct C44Matrix {
    float m[16];
};

// A model's draw effect (0xb0, the "M2 Model Effect Pool"): its kind, strength and textures.
struct M2ModelEffect {
    int32_t m_type;                     // 0x00 5, 8 (the LOD fade)
    float m_strength;                   // 0x04
    uint8_t m_08[0x80 - 0x08];
    void* m_textures[3];                // 0x80 HTEXTUREs
    uint8_t m_98[8];
    float m_a0;                         // 0xa0 1
    float m_a4;                         // 0xa4 1
    uint8_t m_a8[8];
};
static_assert(sizeof(M2ModelEffect) == 0xb0, "M2ModelEffect is 0xb0 bytes");
// A cleared effect from the pool (FUN_1411c69e0).
M2ModelEffect* M2ModelEffectAlloc();
void M2ModelEffectFree(M2ModelEffect* effect);

// What a bone plays, as FUN_1411bac00 reports it (0x74).
struct M2SequenceInfo {
    uint32_t m_id;                      // 0x00 -1 when the bone keeps no sequence of its own
    uint32_t m_variation;               // 0x04
    int32_t m_time;                     // 0x08 ms into the sequence
    uint32_t m_slotTime;                // 0x0c
    float m_speed;                      // 0x10
    int32_t m_startTime;                // 0x14
    int32_t m_endTime;                  // 0x18
    uint16_t m_blendLeft;               // 0x1c ms of blending left
    uint16_t m_sequence;                // 0x1e the index, 0xffff
    uint32_t m_flags;                   // 0x20 the sequence's
    bool m_pending;                     // 0x24
    bool m_ended;                       // 0x25
    bool m_flag80;                      // 0x26 the bone's flag 0x80
    uint8_t m_27;
    uint32_t m_repeats;                 // 0x28
    C44Matrix m_override;               // 0x2c identity unless the bone has one
    uint8_t m_blendKind;                // 0x6c
    uint8_t m_6d;
    uint16_t m_fixedBlend;              // 0x6e
    float m_blendMax;                   // 0x70
};
static_assert(sizeof(M2SequenceInfo) == 0x74, "M2SequenceInfo is 0x74 bytes");

// The bounds a model collects over its animation (0x44, "M2 Accum Bounds Pool").
struct M2AccumBounds {
    CAaBox m_box;                       // 0x00 the animated boxes so far
    CAaBox m_localBox;                  // 0x18 the same through the inverse render matrix
    C3Vector m_center;                  // 0x30
    float m_radius;                     // 0x3c
    uint32_t m_40;
};
static_assert(sizeof(M2AccumBounds) == 0x44, "M2AccumBounds is 0x44 bytes");

// The engine's interlocked reference counts (FUN_1402c8fb0 up, FUN_1402c8fc0 down): the new count.
int32_t M2RefIncrement(int32_t* count);
int32_t M2RefDecrement(int32_t* count);

// A call made on a model before it was loaded (the "M2 Model Call Pool", 0xa0): run in order by
// InitializeLoaded. The arguments depend on the type.
struct M2ModelCall {
    int32_t m_type;                     // 0x00 0..0x1d
    uint32_t m_pad04;
    M2ModelCall* m_next;                // 0x08
    uint32_t m_frame;                   // 0x10 the scene's time when queued
    uint32_t m_pad14;
    uint32_t m_args[0x22];              // 0x18
};
static_assert(sizeof(M2ModelCall) == 0xa0, "M2ModelCall is 0xa0 bytes");

// The effect a model draws with (FUN_1411e6cc0).
void* M2BaseModelGetEffect(CM2BaseModel* model);
// A call from the pool, its type -1 (FUN_1411e75d0), and one freed with what it holds
// (FUN_1411e6b40).
M2ModelCall* M2ModelCallAlloc();
void M2ModelCallFree(M2ModelCall** call);

// An attachment made later in the cache's update (0x18).
struct M2PendingAttach {
    CM2BaseModel* m_model;              // 0x00
    CM2BaseModel* m_parent;             // 0x08
    uint32_t m_id;                      // 0x10
    uint32_t m_pad14;
};

// What a model draws with this frame, combined down from its parent (0x38 at
// CM2BaseModel+0x348; FUN_1411acff0 starts it, FUN_1411ce630 combines it).
struct CM2ModelParams {
    void* m_effect;                     // 0x00 the model's own effect or the parent's
    C44Matrix* m_matrix;                // 0x08
    float m_alpha;                      // 0x10 1
    float m_colorAlpha;                 // 0x14 1
    float m_fade;                       // 0x18 1, and the distance fade
    uint32_t m_flags;                   // 0x1c bit 0 cleared
    C3Vector m_color;                   // 0x20 1, 1, 1
    C3Vector m_colorAdd;                // 0x2c
};
static_assert(sizeof(CM2ModelParams) == 0x38, "CM2ModelParams is 0x38 bytes");

struct RENDERSETTINGS;

class CM2BaseModel {
    public:
    // m_flags.
    enum : uint32_t {
        BASE_LOADED = 0x1,
        BASE_CHILDREN_LOADED = 0x2,
        BASE_VISIBLE = 0x4,                 // propagated down from the parent
        BASE_SHOWN = 0x8,                   // without a parent; 0x10 with one
        BASE_ATTACH_SHOWN = 0x10,
        BASE_READY_CHECKED = 0x40,
        BASE_ALL_READY = 0x80,
        BASE_TEXTURES_READY = 0x100,
        BASE_MATRIX_CURRENT = 0x200,
        BASE_INVERSE_CURRENT = 0x400,
        BASE_CULLED = 0x800,
        BASE_NO_PARTICLES = 0x2000,
        BASE_DETACHED_MATRIX = 0x4000,      // ignores the parent's matrix
        BASE_CAMERA_ROTATION = 0x8000,      // takes the camera's rotation, keeping its scale
        BASE_CAMERA_FACING = 0x10000,       // takes the camera's rotation as it is
        BASE_ATTACHED_HIDDEN = 0x1000000,
        BASE_SHADOWED = 0x4000000,
        BASE_FROM_DONOR = 0x8000000,
        BASE_SHARED_FLAG_40 = 0x20000000,
    };

    uint32_t m_flags;                   // 0x008
    uint8_t m_flags2;                   // 0x00c bit 1 propagates, bit 2 has collision bounds
    uint8_t m_0d;                       // 0x00d
    uint16_t m_type;                    // 0x00e 0 CM2Model, 4 a base model
    CM2BaseModel* m_parent;             // 0x010 what this is attached to
    CM2Scene* m_scene;                  // 0x018
    CM2BaseModel** m_sceneLink;         // 0x020 the scene's model list (+0x78) or release list (+0x80)
    CM2BaseModel* m_sceneNext;          // 0x028
    CM2BaseModel** m_bucketLink;        // 0x030 one of the scene's 64 buckets (+0x128)
    CM2BaseModel* m_bucketNext;         // 0x038
    CM2BaseModel* m_children;           // 0x040 attached to this
    CM2BaseModel** m_siblingLink;       // 0x048
    CM2BaseModel* m_siblingNext;        // 0x050
    C44Matrix* m_extraMatrix;           // 0x058 the "M2 Model Matrix Pool"
    M2ModelCall* m_calls;               // 0x060
    M2ModelCall** m_callsTail;          // 0x068
    int32_t m_attachId;                 // 0x070 -1 when not attached
    uint32_t m_attachIndex;             // 0x074 0xffff for none
    int32_t m_refCount;                 // 0x078
    uint32_t m_createFlags;             // 0x07c
    C44Matrix m_world;                  // 0x080
    C44Matrix m_view;                   // 0x0c0 the world matrix through the scene's camera
    C44Matrix m_local;                  // 0x100
    C44Matrix m_inverse;                // 0x140 of the world matrix, made on demand
    CAaBox m_bounds;                    // 0x180
    CAaBox m_collisionBounds;           // 0x198
    float m_collisionRadius;            // 0x1b0
    C3Vector m_center;                  // 0x1b4
    float m_radius;                     // 0x1c0
    CAaBox m_fileBounds;                // 0x1c4 the shared file's
    C3Vector m_collisionCenter;         // 0x1dc
    float m_collisionSphereRadius;      // 0x1e8
    uint32_t m_pad1ec;
    CM2ModelLighting m_lighting;        // 0x1f0
    CM2ModelLighting* m_lightingUsed;   // 0x310 this one's or the parent's
    void (*m_lightingCallback)(CM2BaseModel* model, CM2ModelLighting* lighting, void* param);  // 0x318
    void* m_lightingParam;              // 0x320
    float m_alpha;                      // 0x328
    float m_32c;                        // 0x32c
    float m_330;                        // 0x330
    float m_334;                        // 0x334
    uint32_t m_matrixFrame;             // 0x338 the scene frame the matrices were made in
    uint32_t m_33c;                     // 0x33c
    uint8_t m_lodMin;                   // 0x340
    uint8_t m_lod;                      // 0x341
    uint8_t m_lodTarget;                // 0x342
    uint8_t m_lodRequested;             // 0x343
    uint32_t m_pad344;
    CM2ModelParams m_params;            // 0x348
    uint32_t m_createTime;              // 0x380 the scene's time when made
    uint32_t m_pad384;
    void (*m_loadCallback)(CM2BaseModel* model, void* param);  // 0x388
    void* m_loadParam;                  // 0x390
    void* m_impact;                     // 0x398 the "M2 Model Impact Pool" (0x18)
    uint8_t m_3a0[8];

    // ref: FUN_1411e5ad0
    CM2BaseModel();

    // The vtable (0x142721a60).
    // ref: FUN_1411e5f20
    virtual ~CM2BaseModel();
    virtual void WaitForLoad(int32_t a2) = 0;                                       // 0x008
    virtual float* GetBounds(float* out) = 0;                                       // 0x010 a box
    virtual bool AreTexturesReady() = 0;                                            // 0x018
    virtual int32_t GetLoadState() = 0;                                             // 0x020 2 ready
    virtual void WaitForAll() = 0;                                                  // 0x028
    virtual void WaitForTextures() = 0;                                             // 0x030
    virtual bool SetPriority(int32_t priority, bool textures) = 0;                  // 0x038
    virtual float* GetAnimatedSphere(float* out) = 0;                               // 0x040
    virtual float* GetAnimatedBox(float* out, bool effects) = 0;                    // 0x048
    virtual float* GetBoundingBox(float* out) = 0;                                  // 0x050
    virtual float GetBoundsRadius() = 0;                                            // 0x058
    virtual void DetachChildrenAt(int32_t id) = 0;                                  // 0x060
    virtual void MarkRayTracingDirty() = 0;                                         // 0x068
    virtual void SetAttachMatrix(C44Matrix* matrix) = 0;                            // 0x070
    virtual void DisableLights() = 0;                                               // 0x078
    virtual void CollectEvents(CM2Scene* scene, void* callbacks) = 0;               // 0x080
    virtual int32_t GetFileDataId() = 0;                                            // 0x088
    virtual bool GetContentKey(uint8_t* key) = 0;                                   // 0x090
    virtual bool IsSameFile(CM2BaseModel* other) = 0;                               // 0x098
    virtual bool IsFileDataId(int32_t fileDataId) = 0;                              // 0x0a0
    virtual bool IsFileName(const char* name) = 0;                                  // 0x0a8
    virtual void CountFiles(int64_t* loaded, int64_t* total) = 0;                   // 0x0b0
    virtual bool AnimateAttachment(int32_t index, bool none) = 0;                   // 0x0b8
    virtual void GetAttachmentWorld(int32_t index, bool none, C44Matrix* out) = 0;  // 0x0c0
    // The attachment point with an id, 0xffff for none.
    virtual uint32_t GetAttachmentIndex(uint32_t id) = 0;                           // 0x0c8
    virtual void GetAttachmentMatrix(uint32_t index, C44Matrix* out) = 0;           // 0x0d0
    virtual CM2Shared* GetShared() = 0;                                             // 0x0d8
    // ref: FUN_1411e68e0
    virtual void Detach(bool notify);                                               // 0x0e0
    // ref: FUN_1411e7630
    virtual void OnChildrenChanged();                                               // 0x0e8
    // ref: FUN_1411e6220
    virtual bool AttachTo(CM2BaseModel* parent, uint32_t id, const float* position, bool flag);  // 0x0f0
    // An attachment id as this model knows it (the same for a base model).
    // ref: FUN_1411e6cd0
    virtual uint32_t MapAttachmentId(uint32_t id);                                  // 0x0f8
    // ref: FUN_1411e7eb0
    virtual void SetShown(bool shown);                                              // 0x100
    // ref: FUN_1411e5fa0
    virtual void QueueAttachChild(CM2BaseModel* child, uint32_t id, bool flag);     // 0x108
    // ref: FUN_1411e6080
    virtual void QueueCallback(CM2BaseModel* target, void* callback);               // 0x110
    // ref: FUN_1411e6140
    virtual void QueueAttachChildAt(CM2BaseModel* child, uint32_t a3, uint32_t a4, bool flag);  // 0x118
    // ref: FUN_1411e8210
    virtual void UpdateMatrices(const C44Matrix* parent, const CM2ModelParams* params, uint32_t a4, uint32_t a5,
        uint32_t a6);  // 0x120
    // ref: FUN_1411e7660
    virtual void PrepareLighting(int32_t pass, uint32_t propagate);                 // 0x128
    // ref: FUN_1411e66e0
    virtual void UpdateCulling();                                                   // 0x130
    // ref: FUN_1411e6dd0
    virtual float* GetBox(float* out);                                              // 0x138
    // ref: FUN_1411e7080
    virtual float* GetSphere(float* out);                                           // 0x140
    virtual float* GetBox2(float* out);                                             // 0x148 FUN_1411e6dd0
    virtual float* GetSphere2(float* out);                                          // 0x150 FUN_1411e7080
    // ref: FUN_1411e6c20
    virtual bool GetAttachmentMatrixByIndex(uint32_t index, C44Matrix* out);        // 0x158
    // ref: FUN_14026bea0
    virtual bool IsAttachmentVisible(uint32_t index);                               // 0x160
    // ref: FUN_1411e77d0
    virtual void ClearLoaded();                                                     // 0x168
    virtual void OnRemove();                                                        // 0x170
    // ref: FUN_1411e8c80
    virtual void SetWorldMatrix(const C44Matrix* world);                            // 0x178
    virtual void ReleaseGpuData();                                                  // 0x180
    virtual void RefreshBatchEffects();                                             // 0x188
    virtual void OnRenderSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current);  // 0x190
    virtual void OnMatricesUpdated();                                               // 0x198
    virtual bool AreTexturesSettled();                                              // 0x1a0
    virtual void SetTimeScale(float scale);                                         // 0x1a8
    virtual void GetFacets(const float* box, const float* matrix, void* facets);    // 0x1b0
    // ref: FUN_1411e6ce0
    virtual const char* GetName();                                                  // 0x1b8
    virtual const char* GetName2(char* buffer, size_t size);                        // 0x1c0
    virtual const char* GetName3(char* buffer, size_t size);                        // 0x1c8
    // ref: FUN_1411e6a10
    virtual void RemoveFromScene();                                                 // 0x1d0
    // ref: FUN_1411e6430
    virtual void AddToScene(CM2Scene* scene);                                       // 0x1d8
    // ref: FUN_1402ca290
    virtual uint8_t GetMaxLod();                                                    // 0x1e0
    virtual const void* GetLodData();                                               // 0x1e8
    virtual bool HasGpuBatches();                                                   // 0x1f0
    // ref: FUN_1411e6600
    virtual void OnLodChanged();                                                    // 0x1f8
    virtual void UpdateLodEffect();                                                 // 0x200
    // ref: FUN_1411e6cf0
    virtual float GetScaledRadius(C3Vector* center, float* radius);                 // 0x208

    // ref: FUN_1411e6070
    void AddRef();
    // Drops a reference; the last moves the model to the scene's release list. The count left.
    // ref: FUN_1411e7880
    uint32_t Release(uint32_t count);
    // ref: FUN_1411e74e0
    bool IsReady();
    // ref: FUN_1411e7570
    bool AreChildrenLoaded();
    // ref: FUN_1411e8190
    void RunLoadCallbacks();
    // ref: FUN_1411e6510
    void FreeCalls();
    // ref: FUN_1411e67b0
    void Unlink();
    // ref: FUN_1411e77e0
    void SetVisibleFlags(bool visible, bool a3, bool a4);
    // ref: FUN_1411e7c70
    void SetLoadCallback(void (*callback)(CM2BaseModel* model, void* param), void* param);
    // ref: FUN_1411e7fa0
    void SetLocalMatrix(const float* matrix);
    // ref: FUN_1411e6ac0
    void InvalidateMatrices();
    // ref: FUN_1411e7970
    void SetBucketed(int32_t bucketed);
    // ref: FUN_1411e71a0
    bool IsShownInTree();
    // ref: FUN_1411e7ce0
    bool SetLodTree(uint32_t lod);
    // ref: FUN_1411e7c90
    void SetLod(uint32_t lod, bool notify);
    // ref: FUN_1411e7d90
    void SetLodMin(uint8_t lod);
    // ref: FUN_1411e7e70
    void SetField330(float value);
    // ref: FUN_1411e7db0
    void SetFlag400000(bool set);
    // ref: FUN_1411e7e00
    void SetShadowed(bool set);
    // ref: FUN_1411e7a10
    void SetDetachedMatrix(int32_t detached);
    // ref: FUN_1411e8090
    void UpdateLighting(void* param);
    // ref: FUN_1411e8bc0
    void RecomputeMatrices();
    // ref: FUN_1411e8f60
    void EnsureLoaded();
    // ref: FUN_1411e6e00
    float* GetTreeBounds(float* out);
    // ref: FUN_1411e70a0
    float* GetTreeSphere(float* out);
    // ref: FUN_1411e6b90
    int32_t GetTreeLoadState();
    // ref: FUN_1411e71e0
    void GetRenderMatrix(C44Matrix* out);
    // ref: FUN_1411e7250
    C3Vector* GetRenderPosition(C3Vector* out);
    // ref: FUN_1411e72a0
    float GetRenderScale();
    // ref: FUN_1411e7ee0
    void SetPositionScale(const C3Vector* position, float rotation, float scale);
    // ref: FUN_1411e7b40
    void SetImpact(const C3Vector* position, const uint32_t* data, uint32_t a4);
};
static_assert(offsetof(CM2BaseModel, m_lighting) == 0x1f0, "CM2BaseModel lighting at 0x1f0");
static_assert(offsetof(CM2BaseModel, m_params) == 0x348, "CM2BaseModel params at 0x348");
static_assert(sizeof(CM2BaseModel) == 0x3a8, "CM2BaseModel is 0x3a8 bytes");

// A bone's animation state (0x90).
struct M2BoneState {
    M2TrackState<C3Vector> m_translation;   // 0x00
    M2TrackState<C4Quaternion> m_rotation;  // 0x14
    M2TrackState<C3Vector> m_scale;         // 0x2c
    M2SequenceTime* m_sequence;             // 0x40
    M2SequenceTime* m_blendSequence;        // 0x48
    C44Matrix* m_override;                  // 0x50 a matrix set from outside
    // the bone's flags (low half), 0x2000 own sequence, 0x4000 own blend
    uint32_t m_flags;                       // 0x58
    uint32_t m_5c;                          // 0x5c the sequence id asked for, -1
    uint16_t m_variation;                   // 0x60 its variation
    // The bones with a primary slot of their own, linked by index from M2AnimState::m_ownerHead.
    uint16_t m_ownerNext;                   // 0x62 0xffff
    uint32_t m_64;
    uint16_t* m_ownerLink;                  // 0x68 the link pointing here
    int32_t m_blendEnd;                     // 0x70 the clock the blend ends at
    float m_blendRate;                      // 0x74 per ms
    float m_blendMax;                       // 0x78 1
    float m_blendWeight;                    // 0x7c
    float m_weight;                         // 0x80 1
    float m_inheritedWeight;                // 0x84 1, times the parent's
    uint8_t m_blendKind;                    // 0x88 5 (see M2SequenceBlendTime)
    uint8_t m_89;
    uint16_t m_fixedBlend;                  // 0x8a
    uint32_t m_8c;
};
static_assert(sizeof(M2BoneState) == 0x90, "M2BoneState is 0x90 bytes");
static_assert(offsetof(M2BoneState, m_sequence) == offsetof(M2AnimTarget, m_sequence), "M2BoneState sequence");
static_assert(offsetof(M2BoneState, m_blendWeight) == offsetof(M2AnimTarget, m_blendWeight), "M2BoneState blend");

// A colour's state (0x20).
struct M2ColorState {
    M2TrackState<C3Vector> m_color;         // 0x00
    M2TrackState<float> m_alpha;            // 0x14
};
static_assert(sizeof(M2ColorState) == 0x20, "M2ColorState is 0x20 bytes");

// A texture transform's state (0x40).
struct M2TextureTransformState {
    M2TrackState<C3Vector> m_translation;   // 0x00
    M2TrackState<C4Quaternion> m_rotation;  // 0x14
    M2TrackState<C3Vector> m_scale;         // 0x2c
};
static_assert(sizeof(M2TextureTransformState) == 0x40, "M2TextureTransformState is 0x40 bytes");

// A camera's state (0x48).
struct M2CameraState {
    M2TrackState<C3Vector> m_position;      // 0x00
    M2TrackState<C3Vector> m_target;        // 0x14
    M2TrackState<float> m_roll;             // 0x28
    M2TrackState<float> m_fieldOfView;      // 0x34
    CM2Camera* m_camera;                    // 0x40
};
static_assert(sizeof(M2CameraState) == 0x48, "M2CameraState is 0x48 bytes");

// A light's state (0x100): its tracks and the CM2Lighting it drives.
struct M2LightState {
    M2TrackState<C3Vector> m_ambientColor;      // 0x00
    M2TrackState<float> m_ambientIntensity;     // 0x14
    M2TrackState<C3Vector> m_diffuseColor;      // 0x20
    M2TrackState<float> m_diffuseIntensity;     // 0x34
    M2TrackState<float> m_attenuationStart;     // 0x40
    M2TrackState<float> m_attenuationEnd;       // 0x4c
    M2TrackState<uint8_t> m_visibility;         // 0x58
    uint32_t m_64;                              // 0x64 1
    CM2Lighting m_lighting;                     // 0x68
};
static_assert(sizeof(M2LightState) == 0x100, "M2LightState is 0x100 bytes");

// A ribbon emitter's state (0x50).
struct M2RibbonState {
    M2TrackState<C3Vector> m_color;             // 0x00
    M2TrackState<float> m_alpha;                // 0x14 fixed 16
    M2TrackState<float> m_heightAbove;          // 0x20
    M2TrackState<float> m_heightBelow;          // 0x2c
    M2TrackState<uint16_t> m_textureSlot;       // 0x38
    M2TrackState<uint8_t> m_visibility;         // 0x44
};
static_assert(sizeof(M2RibbonState) == 0x50, "M2RibbonState is 0x50 bytes");

// A particle emitter's state (0x90).
struct M2ParticleState {
    M2TrackState<float> m_emissionSpeed;        // 0x00
    M2TrackState<float> m_speedVariation;       // 0x0c
    M2TrackState<C3Vector> m_18;                // 0x18 from the shared extra data
    M2TrackState<float> m_lifespan;             // 0x2c
    M2TrackState<float> m_emissionRate;         // 0x38
    M2TrackState<float> m_zSource;              // 0x44
    M2TrackState<uint8_t> m_enabled;            // 0x50
    M2TrackState<float> m_emissionAreaLength;   // 0x5c
    M2TrackState<float> m_emissionAreaWidth;    // 0x68
    M2TrackState<float> m_verticalRange;        // 0x74
    M2TrackState<float> m_horizontalRange;      // 0x80
    uint8_t m_8c;                               // 0x8c 1
    uint8_t m_8d;                               // 0x8d 1
    uint8_t m_8e;                               // 0x8e
    uint8_t m_8f;
};
static_assert(sizeof(M2ParticleState) == 0x90, "M2ParticleState is 0x90 bytes");

// The model's animation (0x98, FUN_141200610): the file's arrays it animates from.
struct M2AnimState {
    // The model's own source (the first six fields of an M2SequenceSource).
    M2Array<M2CompBone>* m_bones;           // 0x00
    M2Array<uint16_t>* m_keyBoneLookup;     // 0x08
    M2Array<M2Sequence>* m_sequences;       // 0x10
    M2Array<uint16_t>* m_sequenceHash;      // 0x18
    M2Array<M2Attachment>* m_attachments;   // 0x20
    M2Array<uint16_t>* m_attachmentLookup;  // 0x28 attachment index by id
    M2BoneState* m_boneStates;              // 0x30
    uint32_t* m_globalTimes;                // 0x38
    M2TrackState<uint8_t>* m_attachmentStates;  // 0x40
    uint16_t* m_ownerHead;                  // 0x48 the first bone with its own primary slot
    C44Matrix* m_boneMatrices;              // 0x50
    // A parent skeleton's source.
    M2Array<M2CompBone>* m_parentBones;     // 0x58
    M2Array<uint16_t>* m_parentKeyBoneLookup;  // 0x60
    M2Array<M2Sequence>* m_parentSequences; // 0x68
    M2Array<uint16_t>* m_parentHash;        // 0x70
    M2Array<M2Attachment>* m_parentAttachments;  // 0x78
    M2Array<uint16_t>* m_parentAttachmentLookup; // 0x80
    void* m_88;                             // 0x88
    uint32_t m_flags;                       // 0x90 1 has a parent skeleton
    uint32_t m_pad94;
};

// What one pass of bone animation carries (0x108, FUN_1412005a0 starts one): the matrix the
// root bones hang from, scratch matrices and vectors, and the clock.
struct M2AnimateContext {
    C44Matrix m_root;                       // 0x00
    C44Matrix m_parent;                     // 0x40 the parent bone's, adjusted for billboards
    C44Matrix m_local;                      // 0x80 the bone's own
    C3Vector m_pivot;                       // 0xc0
    float m_cc;
    C3Vector m_worldPivot;                  // 0xd0
    float m_dc;
    C3Vector m_scale;                       // 0xe0
    float m_ec;
    int32_t m_time;                         // 0xf0 the scene's
    uint32_t m_frame;                       // 0xf4
    uint32_t m_f8;                          // 0xf8
    uint32_t m_fc;                          // 0xfc
    uint32_t m_100;                         // 0x100
    uint32_t m_104;                         // 0x104
};
static_assert(sizeof(M2AnimateContext) == 0x108, "M2AnimateContext is 0x108 bytes");
static_assert(sizeof(M2AnimState) == 0x98, "M2AnimState is 0x98 bytes");

class CM2Model : public CM2BaseModel {
    public:
    uint32_t m_modelFlags;              // 0x3a8
    uint32_t m_modelFlags2;             // 0x3ac 0x800 drives a skeleton, 0x8000/0x10000 listed files
    uint32_t m_3b0;                     // 0x3b0 0xffff
    uint32_t m_pauseRequest;            // 0x3b4
    uint32_t m_3b8;                     // 0x3b8
    uint16_t m_3bc;                     // 0x3bc 0xffff
    uint8_t m_lodMaskBytes;             // 0x3be bytes in each LOD's bone mask
    uint8_t m_boneDoneBytes;            // 0x3bf bytes in the bone-done mask
    uint32_t m_3c0;                     // 0x3c0
    uint32_t m_pad3c4;
    CM2Model** m_waitPrev;              // 0x3c8 the link pointing here on the file's waiting list
    CM2Model* m_waitNext;               // 0x3d0
    CM2Shared* m_shared;                // 0x3d8
    CM2Shared* m_shared2;               // 0x3e0
    CM2Model* m_donor;                  // 0x3e8 a model this one copies its state from
    M2TrackState<uint8_t>* m_attachmentStates;  // 0x3f0 (0xc each)
    uint32_t* m_globalTimes;            // 0x3f8 per global sequence (M2Data loops)
    uint32_t m_initTime;                // 0x400
    float m_404;                        // 0x404 1
    void* m_408;                        // 0x408
    void* m_410;                        // 0x410
    void* m_418;                        // 0x418
    void* m_callback420;                // 0x420
    void* m_callback420Closure[2];      // 0x428
    float m_screenSize;                 // 0x438 the model's size on screen, for throttling
    float m_43c;                        // 0x43c -10000
    uint32_t m_lastAnimateTime;         // 0x440
    uint32_t m_pad444;
    void* m_block;                      // 0x448 the per-instance arrays
    C44Matrix* m_matrixBlock;           // 0x450 aligned: bone matrices then texture matrices
    M2BoneState* m_boneStates;          // 0x458
    C44Matrix* m_boneMatrices;          // 0x460
    void* m_468;                        // 0x468
    uint8_t* m_boneDone;                // 0x470 a bit per bone animated this frame
    uint8_t* m_lodBoneMasks[8];         // 0x478 the bones each LOD uses
    void* m_physicsBones;               // 0x4b8 (0xc each)
    uint8_t m_4c0;                      // 0x4c0
    uint8_t m_pad4c1[7];
    void* m_windPhase;                  // 0x4c8 the "M2 Model Wind Phase Pool"
    M2ColorState* m_colors;             // 0x4d0
    void** m_textures;                  // 0x4d8 HTEXTURE per M2Texture
    void* m_4e0;                        // 0x4e0 (0x28)
    M2TrackState<float>* m_textureWeights;  // 0x4e8 (0xc each)
    M2TextureTransformState* m_textureTransforms;  // 0x4f0
    C44Matrix* m_textureMatrices;       // 0x4f8
    void (*m_effectMatrixCallback)(const C44Matrix* matrix, void* param);  // 0x500
    void* m_effectMatrixParam;          // 0x508
    void* m_510;                        // 0x510
    C44Matrix m_attachWorld;            // 0x518
    C44Matrix* m_558;                   // 0x558 the attachment matrix set from outside
    void* m_effect2;                    // 0x560 the "M2 Model Effect Pool"
    C3Vector m_color;                   // 0x568
    C3Vector m_colorAdd;                // 0x574
    float m_580;                        // 0x580 4
    uint32_t m_debugColors[3];          // 0x584
    void* m_590;                        // 0x590
    void* m_events;                     // 0x598
    void* m_5a0;                        // 0x5a0
    void* m_5a8;                        // 0x5a8
    M2LightState* m_lights;             // 0x5b0
    M2CameraState* m_cameras;           // 0x5b8
    M2RibbonState* m_ribbonStates;      // 0x5c0
    CRibbonEmitter** m_ribbons;         // 0x5c8
    M2ParticleState* m_particleStates;  // 0x5d0
    CParticleEmitter2** m_particles;    // 0x5d8
    uint32_t m_physicsBoneCount;        // 0x5e0
    uint32_t m_pad5e4;
    void* m_physicsBoneStates;          // 0x5e8 (0xe0 each)
    uint32_t m_5f0;                     // 0x5f0
    uint32_t m_pad5f4;
    void** m_5f8;                       // 0x5f8
    void* m_600;                        // 0x600
    uint32_t m_608;                     // 0x608
    uint32_t m_pad60c;
    M2AccumBounds* m_accumBounds;       // 0x610 the "M2 Accum Bounds Pool", with model flag 0x800000
    int32_t m_618;                      // 0x618 the bone file index, -1
    int32_t m_rtEntry[2];               // 0x61c the ray tracing entry: its index and state (2 registered)
    uint32_t m_pad624;
    void* m_skeleton;                   // 0x628 FUN_1411f9c00
    M2AnimState* m_animState;           // 0x630
    void* m_638;                        // 0x638
    void* m_effect;                     // 0x640 the "M2 Model Effect Pool"
    uint32_t m_648;                     // 0x648
    uint32_t m_pad64c;
    CM2Model* m_parentModel;            // 0x650
    CM2Model* m_attachedModels;         // 0x658 the models attached below this one
    CM2Model** m_attachedLink;          // 0x660
    CM2Model* m_attachedNext;           // 0x668
    void* m_lock670;                    // 0x670
    int32_t m_handle;                   // 0x678 -1
    uint32_t m_67c;                     // 0x67c
    uint32_t m_680;                     // 0x680
    uint16_t m_684;                     // 0x684 0xff00
    uint8_t m_686;                      // 0x686 1
    uint8_t m_687;
    // The last 4 bytes of the slot hold its ObjectAlloc index.
    uint32_t m_allocIndex;              // 0x688
    uint32_t m_pad68c;

    // ref: FUN_1411ad030
    CM2Model();
    // ref: FUN_1411ad380 (FUN_1411ad860 is the deleting form)
    ~CM2Model() override;
    // The per-instance block freed (FUN_1411b8320) and the GPU batches (FUN_1411cdb90).
    void FreeInstanceBlock();
    void ReleaseGpuBatches();

    // ref: FUN_1411cecd0
    void WaitForLoad(int32_t a2) override;
    // ref: FUN_1411b9730
    float* GetBounds(float* out) override;
    // ref: FUN_1411c61f0
    bool AreTexturesReady() override;
    // ref: FUN_1411b9820
    int32_t GetLoadState() override;
    // ref: FUN_1411ced60
    void WaitForAll() override;
    // ref: FUN_1411cece0
    void WaitForTextures() override;
    // ref: FUN_1411c9b60
    bool SetPriority(int32_t priority, bool textures) override;
    // ref: FUN_1411bbf90
    float* GetAnimatedSphere(float* out) override;
    // ref: FUN_1411bbb80
    float* GetAnimatedBox(float* out, bool effects) override;
    // ref: FUN_1411bbdf0
    float* GetBoundingBox(float* out) override;
    // ref: FUN_1411c0320
    float GetBoundsRadius() override;
    // ref: FUN_1411b7040
    void DetachChildrenAt(int32_t id) override;
    // ref: FUN_1411c7bb0
    void MarkRayTracingDirty() override;
    // ref: FUN_1411b60b0
    void SetAttachMatrix(C44Matrix* matrix) override;
    // ref: FUN_1411b77d0
    void DisableLights() override;
    // ref: FUN_1411b8e80
    void CollectEvents(CM2Scene* scene, void* callbacks) override;
    // ref: FUN_1411bd840
    int32_t GetFileDataId() override;
    // ref: FUN_1411bbb60
    bool GetContentKey(uint8_t* key) override;
    // ref: FUN_1411c6270
    bool IsSameFile(CM2BaseModel* other) override;
    // ref: FUN_1411c62a0
    bool IsFileDataId(int32_t fileDataId) override;
    // ref: FUN_1411c62c0
    bool IsFileName(const char* name) override;
    // ref: FUN_1411b6590
    void CountFiles(int64_t* loaded, int64_t* total) override;
    // ref: FUN_1411ce0d0
    bool AnimateAttachment(int32_t index, bool none) override;
    // ref: FUN_1411b98e0
    void GetAttachmentWorld(int32_t index, bool none, C44Matrix* out) override;
    // ref: FUN_1411b9a80
    uint32_t GetAttachmentIndex(uint32_t id) override;
    // ref: FUN_1411ba010
    void GetAttachmentMatrix(uint32_t index, C44Matrix* out) override;
    // ref: FUN_1411c0480
    CM2Shared* GetShared() override;
    // ref: FUN_1411b70d0
    void Detach(bool notify) override;
    // ref: FUN_1411c77b0
    void OnChildrenChanged() override;
    // ref: FUN_1411ce300
    void UpdateMatrices(const C44Matrix* parent, const CM2ModelParams* params, uint32_t a4, uint32_t a5,
        uint32_t a6) override;
    // ref: FUN_1411c7a90
    void PrepareLighting(int32_t pass, uint32_t propagate) override;
    // ref: FUN_1411b6990
    void UpdateCulling() override;
    // ref: FUN_1411bdef0
    float* GetBox(float* out) override;
    // ref: FUN_1411bdfe0
    float* GetSphere(float* out) override;
    // ref: FUN_1411bdf70
    float* GetBox2(float* out) override;
    // ref: FUN_1411be0a0
    float* GetSphere2(float* out) override;
    // ref: FUN_1411b9f10
    bool GetAttachmentMatrixByIndex(uint32_t index, C44Matrix* out) override;
    // ref: FUN_1411b9cf0
    bool IsAttachmentVisible(uint32_t index) override;
    // ref: FUN_1411c7b80
    void ClearLoaded() override;
    // ref: FUN_1411b6b50
    void OnRemove() override;
    // ref: FUN_1411ce880
    void SetWorldMatrix(const C44Matrix* world) override;
    // ref: FUN_1411b6f80
    void ReleaseGpuData() override;
    // ref: FUN_1411b6910
    void RefreshBatchEffects() override;
    // ref: FUN_1411c6a80
    void OnMatricesUpdated() override;
    // ref: FUN_1411cd550
    bool AreTexturesSettled() override;
    // ref: FUN_1411c99e0
    void SetTimeScale(float scale) override;
    // ref: FUN_1411bb3d0
    void GetFacets(const float* box, const float* matrix, void* facets) override;
    // ref: FUN_1411be860
    const char* GetName() override;
    // ref: FUN_1411bd850
    const char* GetName2(char* buffer, size_t size) override;
    // ref: FUN_1411bd8a0
    const char* GetName3(char* buffer, size_t size) override;
    // ref: FUN_1411b72b0
    void RemoveFromScene() override;
    // ref: FUN_1411be680
    uint8_t GetMaxLod() override;
    // ref: FUN_1411be670
    const void* GetLodData() override;
    // ref: FUN_1411c0d60
    bool HasGpuBatches() override;
    // ref: FUN_1411b6520
    void OnLodChanged() override;
    // ref: FUN_1411ce170
    void UpdateLodEffect() override;
    // ref: FUN_1411bdad0
    float GetScaledRadius(C3Vector* center, float* radius) override;
    // A key bone's sequence speed changed in place, its clock kept (FUN_1411cab20).
    void SetSequenceSpeed(uint32_t keyBone, float speed);
    // The model effect dropped, here and on the models attached below (FUN_1411b6240,
    // FUN_1411b6380).
    void ClearEffect();
    void ClearEffectFrom(void* effect);
    // The per-instance data released: override matrices, textures, lights, cameras, ribbons,
    // particle emitters and the skeleton's (FUN_1411b80b0).
    void ReleaseInstanceData();
    // Every playback this model waits on dropped or marked dead (FUN_1411b5cb0).
    void CancelAllPlaybacks();
    // An attachment point's matrix in view space when attached, else in local space
    // (FUN_1411c0860), and the root's (FUN_1411c0d80).
    void GetAttachmentPointMatrix(const C3Vector* position, uint32_t bone, float* out);
    void GetRootMatrix(float* out);
    // The box of the sequence a key bone plays, with the effects' (FUN_1411c0490).
    float* GetKeyBoneBounds(float* out, uint16_t keyBone);
    // What a key bone plays (FUN_1411bac00): false when the model has no such bone.
    bool GetSequenceInfo(uint32_t keyBone, struct M2SequenceInfo* out);
    // One bone animated now for something attached to it (FUN_1411ae3a0), and the transform for a
    // new frame without clearing the bone-done bits (FUN_1411ce120).
    bool AnimateBoneFor(uint32_t bone);
    void UpdateFrameTransform(const C44Matrix* parent, const CM2ModelParams* params, uint32_t frame, int32_t time);
    // The particle emitters and ribbons follow a new attachment matrix (FUN_1411b5fe0).
    void SetEffectMatrix(const C44Matrix* matrix);
    // The particle emitters' and ribbons' spheres around the model's (FUN_1411bc940) and their
    // boxes (FUN_1411bc710).
    float* GetEffectSphere(float* out);
    float* GetEffectBox(float* out);
    // The animation events between the last frame and this one, queued as callbacks
    // (FUN_1411b8670).
    bool QueueEvents(CM2Scene* scene, void* callbacks);

    // Bone animation (M2ModelAnimate.cpp). A slot shifted by this frame's pause.
    void ShiftSlotForPause(M2SequenceTime* slot, const M2AnimateContext* context);
    // One bone's matrix from its tracks and its parent's (FUN_1411ae6a0).
    void AnimateBone(const M2CompBone* bone, M2BoneState* state, uint32_t index, M2AnimateContext* context);
    // The scene's wind on a bone of a tinted file (FUN_1411ae6a0's tail).
    void AnimateBoneWind(uint32_t index, M2AnimateContext* context);
    // A bone after its parents (FUN_1411b0a30).
    void AnimateBoneTree(uint32_t index, M2AnimateContext* context);
    // The root slot only (FUN_1411b0af0).
    void AnimateRootOnly(M2BoneState* state);
    // The blended-from sequence released (FUN_1411b6cf0).
    void DropBlend(M2BoneState* state);
    // A camera's vectors from its state (FUN_1411b0d10).
    void UpdateCamera(uint32_t index);
    // The cameras' tracks (FUN_1411b0f00).
    void AnimateCameras();
    // The cameras held elsewhere (FUN_1411b1140).
    void UpdateHeldCameras();

    // The per-frame update (M2ModelUpdate.cpp): lights (FUN_1411b13a0), light placement
    // (FUN_1411b1a40), the whole pass (FUN_1411b1fe0), particle tracks (FUN_1411b2640) and
    // ribbon tracks (FUN_1411b3490).
    void AnimateLights(M2AnimateContext* context);
    void UpdateLights();
    // FUN_1411b3870, FUN_1411b3fc0, FUN_1411b41e0, FUN_1411b45e0, FUN_1411b2c90
    void UpdateRibbons(float delta);
    void UpdateDrawables(int32_t pass, float delta, uint32_t a4);
    void UpdateTransform(const C44Matrix* parent, const CM2ModelParams* params, int32_t time);
    void AnimateTextureTransform(uint32_t index, M2BoneState* state);
    void UpdateParticles(float delta, uint32_t pass, uint32_t a4);
    void Animate(const C44Matrix* parentMatrix, const CM2ModelParams* params, uint32_t frame, int32_t time,
        int32_t throttle);
    void AnimateParticleTracks(M2AnimateContext* context);
    void AnimateRibbonTracks(M2AnimateContext* context);
    // FUN_1411ce630, FUN_1411b73f0, FUN_1411b4980, FUN_1411b4830, FUN_1411ada40
    void UpdateMatricesFrom(const CM2ModelParams* parent);
    void InitializePhysicsBones();
    void AnimateTextureWeights();
    void AnimateTextureTransforms(M2AnimateContext* context);
    void UpdateChildren(M2AnimateContext* context);
    // The bounds through the render matrix into the accumulated bounds (FUN_1411b1fe0's tail).
    void UpdateAccumulatedBounds();

    // Attaching (FUN_1411b4b00, FUN_1411c98d0) and joining a scene (FUN_1411b4f10).
    bool AttachTo(CM2BaseModel* parent, uint32_t id, const float* position, bool flag) override;
    bool ResolveAttachment(CM2BaseModel* parent, uint32_t id, bool force);
    void AddToScene(CM2Scene* scene) override;
    // ref: FUN_1411bd740
    uint32_t MapAttachmentId(uint32_t id) override;
    // Sequences (M2Sequence.cpp). Plays sequence `id` on a key bone (-1 the root) from
    // `offset` ms at `speed`: as the primary sequence, or as one blended over it fading out;
    // `check` first asks whether the model has it (FUN_1411ca300).
    void SetSequence(uint32_t keyBone, uint32_t id, uint32_t variation, int32_t offset, float speed,
        int32_t primary, int32_t repeats, int32_t kind, int32_t fixedBlend, int32_t check, float weight);
    // The bone's own sequence dropped: it follows its parent's again (FUN_1411cdc50).
    void SetBoneSequenceNone(uint32_t keyBone, uint8_t kind, int32_t blend, int32_t check, uint32_t id,
        uint16_t fixedBlend);
    // ref: FUN_1411cde00
    void ReleaseBoneSequence(uint32_t bone, uint8_t kind, int32_t blend, uint16_t fixedBlend,
        M2Array<M2CompBone>* bones, M2BoneState* states);
    // The primary slot released (FUN_1411b6bc0) or made the blended-from one (FUN_1411c65e0).
    void DropSequence(M2BoneState* state);
    void BlendFromCurrent(M2BoneState* state);
    // The bone-done bits cleared and the transform brought up for a new frame (FUN_1411b61c0).
    void ResetBoneState(const C44Matrix* parent, const CM2ModelParams* params, uint32_t frame, int32_t time);
    // ref: FUN_1411c6c00
    bool AllowSequenceChange(uint32_t keyBone, uint16_t bone);
    // The .anim playbacks waiting for a bone (FUN_1411b5e20), and one queued (FUN_1411ca7d0).
    void CancelPlaybacks(uint32_t bone, bool blend);
    void QueuePlayback(uint16_t sequence, M2Data* data, uint16_t bone, uint32_t offset, float speed,
        const M2SequenceResolve* resolve, int32_t blend, int32_t endless, uint32_t repeats, uint8_t kind,
        uint16_t fixedBlend, uint32_t weight);
    // A slot started (FUN_1411cccd0); a bone's primary sequence started (FUN_1411cbfa0) and a
    // sequence blended over it (FUN_1411cc860).
    void StartSlot(uint16_t index, const M2SequenceResolve* resolve, uint32_t offset, float speed,
        M2SequenceTime* slot, int32_t repeats, M2Array<M2Sequence>* sequences);
    void StartSequence(uint16_t index, uint16_t bone, const M2SequenceResolve* resolve, uint32_t offset,
        float speed, int32_t repeats, uint8_t kind, uint16_t fixedBlend, float weight);
    void StartBlendSequence(uint16_t index, uint16_t bone, const M2SequenceResolve* resolve, uint32_t offset,
        float speed, int32_t repeats);
    // A bone's matrix set from outside, by key bone (FUN_1411ca130) or bone (FUN_1411ca270).
    void SetKeyBoneMatrix(uint32_t keyBone, const C44Matrix* matrix);
    void SetBoneMatrix(uint32_t bone, const C44Matrix* matrix);
    // The .anim files of a sequence's variations asked for (FUN_1411c6320).
    void PreloadSequence(uint32_t id);
    // A file flagged 0x20 registers its physics shape with the scene's ray tracing (FUN_1411c5ad0).
    void RegisterRayTracing();

    // The textures of a replaceable type (FUN_1411c8e40) and the physics bones' textures
    // released (FUN_141201d60).
    void SetReplaceableTexture(int32_t type, CTexture* texture, CTexture** extra);
    void ReleasePhysicsBoneTextures();
    // InitializeLoaded's parts: the no-particles flag, the anim state's arrays (FUN_1411b7d80),
    // the queued calls' replay, the donor copy and its LOD masks (FUN_1411b7970).
    void UpdateNoParticlesFlag();
    void SetupAnimState();
    void RunQueuedCalls();
    void CopyFromDonor(uint32_t* sequence);
    void CopyLodMasks(CM2Model* other);
    // The public setters (M2ModelApi.cpp), each queued until the file is in.
    void SetTextureAt(uint32_t index, CTexture* texture);
    bool SetGeosets(uint32_t lo, uint32_t hi, uint32_t show);
    void SetSkinSections(uint32_t lo, uint32_t hi, uint32_t show);
    void SetBoneFlags(uint32_t keyBone, uint32_t value, uint32_t mask);
    void SetSequenceTime(uint32_t keyBone, int32_t time);
    void SetLightEnabled(uint32_t index, uint32_t enabled);
    void SetParticleColors(uint32_t colorIndex, uint32_t start, uint32_t middle, uint32_t end);
    void SetParticlesEnabled(int32_t enabled);
    void SetRibbonsEnabled(uint32_t enabled);
    void StopParticles();
    void PrefetchTextures();
    void SetKeyBoneWeight(uint32_t keyBone, float weight);
    void SetBoneWeight(uint32_t bone, float weight);
    void SetBoneFile(int32_t fileDataId);
    void ShareBonesWithAttached();
    void SetRibbonQuality(uint32_t quality);
    void OnChildLoaded(CM2BaseModel* child, uint32_t id);
    void SetEffect(int32_t type, const void* params);
    bool ComputeEffectProjection(struct M2ModelEffect* effect);
    void SetShownSectionCount(int32_t count);
    int32_t CountShownSections();
    C3Vector* GetEventPosition(C3Vector* out, int32_t id);
    void AttachChildAtEvent(CM2BaseModel* child, float scale, int32_t eventId, bool clamp);
    void CreateGpuBatches();
    void ShareParentBones();

    // Set up from the loaded file (FUN_1411c1360): `donor` lends its textures and state.
    bool Initialize(CM2Scene* scene, CM2Shared* shared, CM2Model* donor, uint8_t lod, uint32_t createFlags);
    // ref: FUN_1411c24f0
    bool InitializeLoaded();
};
static_assert(offsetof(CM2Model, m_waitPrev) == 0x3c8, "CM2Model waiting link at 0x3c8");
static_assert(offsetof(CM2Model, m_animState) == 0x630, "CM2Model anim state at 0x630");
static_assert(sizeof(CM2Model) == 0x690, "CM2Model fills its 0x690 slot");

// Each bone's slots from its parent's where it keeps none of its own (FUN_1411ff490).
void M2BoneStatesPropagateSlots(M2BoneState* states, const M2Array<M2CompBone>* bones);
// A skeleton file's bone (FUN_141201f40), its sequence source by kind (+0x38 or +0x40) and a
// sequence it holds released (FUN_1411f8a30).
void M2SkeletonAnimateBone(void* skeleton, const M2CompBone* bone, M2BoneState* state, uint32_t index,
    M2AnimateContext* context, M2AnimState* anim);
void* M2SkeletonGetSource(void* skeleton, bool second);
void M2SkeletonReleaseSequence(void* source, uint32_t sequence);
void M2ResolveSequence(uint32_t id, M2SequenceResolve* out, M2AnimState* anim, M2Array<M2Sequence>* sequences,
    M2Array<uint16_t>* hash, M2Array<uint16_t>* blocked);
// A skeleton's per-model data released (FUN_141203770) and its playbacks for a model dropped
// (FUN_141203470).
void M2SkeletonReleaseModel(void* skeleton);
// A skeleton instance's reference dropped (FUN_141204240).
void M2SkeletonInstanceRelease(void* skeleton);
void M2SkeletonCancelModelPlaybacks(void* skeleton, CM2Model* model);
// animFrameSkipLOD's value (DAT_1430a0eb0 + 0x14).
int32_t M2AnimFrameSkipLod();
// The skeleton's scene (FUN_141203400).
void M2SkeletonSetScene(void* skeleton, CM2Scene* scene);
// The skeleton's side of a sequence: its clock (FUN_141203330), a reference (FUN_1411f8a40),
// its .anim asked for (FUN_1412041f0), the playbacks waiting on it (FUN_1412034c0,
// FUN_1412045f0).
void M2SkeletonSetFrame(void* skeleton, uint32_t frame, int32_t time);
void M2SkeletonAddSequenceRef(void* source, uint32_t sequence);
void M2SkeletonRequestSequence(void* skeleton, uint32_t sequence, bool parent);
void M2SkeletonCancelPlaybacks(void* skeleton, CM2Model* model, uint32_t bone, bool blend);
void M2SkeletonQueuePlayback(void* skeleton, CM2Model* model, uint16_t sequence, M2AnimState* anim, uint16_t bone,
    uint32_t offset, float speed, const M2SequenceResolve* resolve, int32_t blend, int32_t endless,
    uint32_t repeats, uint8_t kind, uint16_t fixedBlend, uint32_t weight);

// Sequence lookup (M2Animate.cpp): AnimationData's flags (FUN_1411febe0), the blend time
// between two sequences (FUN_1411feca0), a variation (FUN_1411fee80), whether a sequence is
// there (FUN_1411feef0, FUN_1411fef70), its repeat count (FUN_1411ff000), a variation by
// frequency (FUN_1411ff040), a slot's clock (FUN_1411ff380) and its source (FUN_1412006d0,
// FUN_141200740).
uint32_t M2AnimationDataFlags(const void* record, uint32_t index);
uint32_t M2SequenceBlendTime(uint16_t to, uint16_t from, const M2Array<M2Sequence>* fromSequences, uint8_t kind,
    uint16_t fixed, const M2Array<M2Sequence>* toSequences);
uint16_t M2FindSequenceVariation(uint32_t id, int32_t variation, const M2Array<M2Sequence>* sequences,
    const M2Array<uint16_t>* hash);
bool M2HasSequence(int16_t id, const M2Array<M2Sequence>* sequences, const M2Array<uint16_t>* hash,
    const M2Array<uint16_t>* blocked);
bool M2AnimStateHasSequence(uint32_t id, const M2AnimState* anim);
int32_t M2SequenceRepeats(const M2Sequence* sequence);
void M2PickVariation(int32_t* variation, uint16_t* index, const M2Array<M2Sequence>* sequences);
void M2SequenceTimeStart(const M2SequenceResolve* resolve, uint32_t offset, float speed, M2SequenceTime* slot,
    const M2Sequence* sequence, CM2Scene* scene);
void M2AnimStateCopySource(const M2AnimState* anim, M2SequenceSource* out, bool parent);
void M2SequenceSourceClear(M2SequenceSource* source);
// A slot from the "M2 Bone Seq Pool" (FUN_1411c6950), and a primary one freed without its
// sequence reference (FUN_1411b6c90).
M2SequenceTime* M2SequenceTimeAlloc();
void M2BoneStateFreeSequence(M2BoneState* state);
// A context started (FUN_1412005a0): the root identity, the clock and the pass's switches.
void M2AnimateContextInit(M2AnimateContext* context, uint32_t time, uint32_t frame, uint32_t compute,
    uint32_t throttle, uint32_t far, uint32_t a7);
// The "M2 Bone Seq Pool" (DAT_1430a0f60).
TObjectAllocHeap* M2GetBoneSeqPool();

// The file's waiting list (FUN_1411c62e0 joins at the tail, FUN_1411cdb30 leaves).
void M2ModelLinkWaiting(CM2Model* model);
void M2ModelUnlinkWaiting(CM2Model* model);
// A skeleton instance for a model (FUN_1411f9c00).
void* M2SkeletonCreateInstance(void* skeleton, CM2Scene* scene, uint32_t flags);
// A physics bone's state cleared (FUN_141200760).
void M2PhysicsBoneInit(void* state);
// A wind phase from its pool (DAT_1430a0f58).
void* M2ModelWindPhaseAlloc();
// A new model joins its file: initialized now, or waiting for the file (FUN_1411a5230).
bool M2SharedAttachModel(CM2Shared* shared, CM2Model* model);
// The file is in: the model sets itself up from it (FUN_1411c24f0).
bool M2ModelInitializeLoaded(CM2Model* model);
// CM2Model's pools (FUN_1411c5d90).
void M2ModelInitializePools();
// FUN_1411b7d60
void M2ModelSetFlag400(bool set);
// The module's own scene (DAT_1430a0ed0, FUN_141196e40(9, ...)).
void M2CreateModuleScene();

#endif
