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

struct C44Matrix {
    float m[16];
};

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

// An attachment made later in the cache's update (0x18).
struct M2PendingAttach {
    CM2BaseModel* m_model;              // 0x00
    CM2BaseModel* m_parent;             // 0x08
    uint32_t m_id;                      // 0x10
    uint32_t m_pad14;
};

// A box and a sphere with flags (0x38 at CM2BaseModel+0x348, FUN_1411acff0).
struct CM2ModelBounds {
    uint64_t m_00;                      // 0x00
    uint64_t m_08;                      // 0x08
    float m_10;                         // 0x10 1
    float m_14;                         // 0x14 1
    float m_18;                         // 0x18 1
    uint32_t m_flags;                   // 0x1c bit 0 cleared
    float m_20;                         // 0x20 1
    float m_24;                         // 0x24 1
    float m_28;                         // 0x28 1
    uint32_t m_2c;                      // 0x2c
    uint64_t m_30;                      // 0x30
};
static_assert(sizeof(CM2ModelBounds) == 0x38, "CM2ModelBounds is 0x38 bytes");

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
    CM2ModelBounds m_modelBounds;       // 0x348
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
    virtual float* GetLocalBounds(float* out) = 0;                                  // 0x010
    virtual bool AreTexturesReady() = 0;                                            // 0x018
    virtual int32_t GetLoadState() = 0;                                             // 0x020 2 ready
    virtual void Virtual028() = 0;                                                  // 0x028
    virtual void Virtual030() = 0;                                                  // 0x030
    virtual void Virtual038() = 0;                                                  // 0x038
    virtual void Virtual040() = 0;                                                  // 0x040
    virtual void Virtual048() = 0;                                                  // 0x048
    virtual void Virtual050() = 0;                                                  // 0x050
    virtual void Virtual058() = 0;                                                  // 0x058
    virtual void Virtual060() = 0;                                                  // 0x060
    virtual void Virtual068() = 0;                                                  // 0x068
    virtual void SetAttachMatrix(const C44Matrix* matrix) = 0;                      // 0x070
    virtual void Virtual078() = 0;                                                  // 0x078
    virtual void Virtual080() = 0;                                                  // 0x080
    virtual int32_t GetFileDataId() = 0;                                            // 0x088
    virtual void Virtual090() = 0;                                                  // 0x090
    virtual bool IsSameFile(CM2BaseModel* other) = 0;                               // 0x098
    virtual bool IsFileDataId(int32_t fileDataId) = 0;                              // 0x0a0
    virtual bool IsFileName(const char* name) = 0;                                  // 0x0a8
    virtual void Virtual0b0() = 0;                                                  // 0x0b0
    virtual bool AnimateAttachment(int32_t index, bool none) = 0;                   // 0x0b8
    virtual void GetAttachmentWorld(int32_t index, bool none, C44Matrix* out) = 0;  // 0x0c0
    virtual void Virtual0c8() = 0;                                                  // 0x0c8
    virtual void GetAttachmentMatrix(uint32_t index, C44Matrix* out) = 0;           // 0x0d0
    virtual void Virtual0d8() = 0;                                                  // 0x0d8
    // ref: FUN_1411e68e0
    virtual void Detach(bool notify);                                               // 0x0e0
    // ref: FUN_1411e7630
    virtual void OnChildrenChanged();                                               // 0x0e8
    // ref: FUN_1411e6220
    virtual bool AttachTo(CM2BaseModel* parent, uint32_t id, const float* position, bool flag);  // 0x0f0
    // ref: FUN_1411e6cd0
    virtual uint32_t Virtual0f8(uint32_t value);                                    // 0x0f8
    // ref: FUN_1411e7eb0
    virtual void SetShown(bool shown);                                              // 0x100
    // ref: FUN_1411e5fa0
    virtual void QueueAttachChild(CM2BaseModel* child, uint32_t id, bool flag);     // 0x108
    // ref: FUN_1411e6080
    virtual void QueueCallback(CM2BaseModel* target, void* callback);               // 0x110
    // ref: FUN_1411e6140
    virtual void QueueAttachChildAt(CM2BaseModel* child, uint32_t a3, uint32_t a4, bool flag);  // 0x118
    // ref: FUN_1411e8210
    virtual void UpdateMatrices(const C44Matrix* parent, void* a3, uint32_t a4, uint32_t a5, uint32_t a6);  // 0x120
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
    virtual bool IsAttachmentVisible();                                             // 0x160
    // ref: FUN_1411e77d0
    virtual void ClearLoaded();                                                     // 0x168
    virtual void OnRemove();                                                        // 0x170
    // ref: FUN_1411e8c80
    virtual void SetWorldMatrix(const C44Matrix* world);                            // 0x178
    virtual void Virtual180();                                                      // 0x180
    virtual void Virtual188();                                                      // 0x188
    virtual void Virtual190();                                                      // 0x190
    virtual void OnMatricesUpdated();                                               // 0x198
    virtual bool Virtual1a0();                                                      // 0x1a0
    virtual void Virtual1a8();                                                      // 0x1a8
    virtual void Virtual1b0();                                                      // 0x1b0
    // ref: FUN_1411e6ce0
    virtual const char* GetName();                                                  // 0x1b8
    virtual const char* GetName2();                                                 // 0x1c0
    virtual const char* GetName3();                                                 // 0x1c8
    // ref: FUN_1411e6a10
    virtual void RemoveFromScene();                                                 // 0x1d0
    // ref: FUN_1411e6430
    virtual void AddToScene(CM2Scene* scene);                                       // 0x1d8
    // ref: FUN_1402ca290
    virtual uint8_t GetMaxLod();                                                    // 0x1e0
    virtual bool Virtual1e8();                                                      // 0x1e8
    virtual uint8_t Virtual1f0();                                                   // 0x1f0
    // ref: FUN_1411e6600
    virtual void OnLodChanged();                                                    // 0x1f8
    virtual void Virtual200();                                                      // 0x200
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
static_assert(offsetof(CM2BaseModel, m_modelBounds) == 0x348, "CM2BaseModel bounds at 0x348");
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
    uint32_t m_5c;                          // 0x5c -1
    uint32_t m_60;                          // 0x60 0xffff0000
    uint32_t m_64;
    uint64_t m_68;                          // 0x68
    int32_t m_blendEnd;                     // 0x70 the clock the blend ends at
    float m_blendRate;                      // 0x74 per ms
    float m_blendMax;                       // 0x78 1
    float m_blendWeight;                    // 0x7c
    float m_weight;                         // 0x80 1
    float m_inheritedWeight;                // 0x84 1, times the parent's
    uint8_t m_88;                           // 0x88 5
    uint8_t m_89;
    uint16_t m_8a;                          // 0x8a
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
    void* m_08;                             // 0x08
    M2Array<M2Sequence>* m_sequences;       // 0x10
    M2Array<uint16_t>* m_sequenceHash;      // 0x18
    M2Array<M2Attachment>* m_attachments;   // 0x20
    M2Array<uint16_t>* m_attachmentLookup;  // 0x28 attachment index by id
    M2BoneState* m_boneStates;              // 0x30
    uint32_t* m_globalTimes;                // 0x38
    M2TrackState<uint8_t>* m_attachmentStates;  // 0x40
    void* m_48;                             // 0x48
    C44Matrix* m_boneMatrices;              // 0x50
    // A parent skeleton's source.
    M2Array<M2CompBone>* m_parentBones;     // 0x58
    void* m_60;                             // 0x60
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
    void* m_500;                        // 0x500
    void* m_508;                        // 0x508
    void* m_510;                        // 0x510
    C44Matrix m_attachWorld;            // 0x518
    C44Matrix* m_558;                   // 0x558
    void* m_effect2;                    // 0x560 the "M2 Model Effect Pool"
    float m_color[6];                   // 0x568
    float m_580;                        // 0x580 4
    uint32_t m_debugColors[3];          // 0x584
    void* m_590;                        // 0x590
    void* m_events;                     // 0x598
    void* m_5a0;                        // 0x5a0
    void* m_5a8;                        // 0x5a8
    M2LightState* m_lights;             // 0x5b0
    M2CameraState* m_cameras;           // 0x5b8
    M2RibbonState* m_ribbonStates;      // 0x5c0
    void** m_ribbons;                   // 0x5c8 CRibbonEmitter*
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
    void* m_610;                        // 0x610
    uint64_t m_618;                     // 0x618 -1
    uint32_t m_620;                     // 0x620
    uint32_t m_pad624;
    void* m_skeleton;                   // 0x628 FUN_1411f9c00
    M2AnimState* m_animState;           // 0x630
    void* m_638;                        // 0x638
    void* m_effect;                     // 0x640 the "M2 Model Effect Pool"
    uint32_t m_648;                     // 0x648
    uint32_t m_pad64c;
    CM2Model* m_parentModel;            // 0x650
    void* m_658;                        // 0x658
    void* m_660;                        // 0x660
    void* m_668;                        // 0x668
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
    // ref: FUN_1411ad860
    ~CM2Model() override;

    void WaitForLoad(int32_t a2) override;
    float* GetLocalBounds(float* out) override;
    // ref: FUN_1411c61f0
    bool AreTexturesReady() override;
    int32_t GetLoadState() override;
    void Virtual028() override;
    void Virtual030() override;
    void Virtual038() override;
    void Virtual040() override;
    void Virtual048() override;
    void Virtual050() override;
    void Virtual058() override;
    void Virtual060() override;
    void Virtual068() override;
    void SetAttachMatrix(const C44Matrix* matrix) override;
    void Virtual078() override;
    void Virtual080() override;
    int32_t GetFileDataId() override;
    void Virtual090() override;
    // ref: FUN_1411c6270
    bool IsSameFile(CM2BaseModel* other) override;
    // ref: FUN_1411c62a0
    bool IsFileDataId(int32_t fileDataId) override;
    // ref: FUN_1411c62c0
    bool IsFileName(const char* name) override;
    void Virtual0b0() override;
    bool AnimateAttachment(int32_t index, bool none) override;
    void GetAttachmentWorld(int32_t index, bool none, C44Matrix* out) override;
    void Virtual0c8() override;
    void GetAttachmentMatrix(uint32_t index, C44Matrix* out) override;
    void Virtual0d8() override;

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
    void Animate(void* a2, const C44Matrix* parent, uint32_t frame, int32_t time, int32_t throttle);
    void AnimateParticleTracks(M2AnimateContext* context);
    void AnimateRibbonTracks(M2AnimateContext* context);
    // FUN_1411ce630, FUN_1411b61c0, FUN_1411b73f0, FUN_1411b4980, FUN_1411b4830, FUN_1411ada40
    void UpdateMatricesFrom(const C44Matrix* parent);
    void ResetBoneState();
    void InitializePhysicsBones();
    void AnimateTextureWeights();
    void AnimateTextureTransforms(M2AnimateContext* context);
    void UpdateChildren(M2AnimateContext* context);
    // The bounds through the render matrix into the accumulated bounds (FUN_1411b1fe0's tail).
    void UpdateAccumulatedBounds();

    // Set up from the loaded file (FUN_1411c1360): `donor` lends its textures and state.
    void Initialize(CM2Scene* scene, CM2Shared* shared, CM2Model* donor, uint8_t lod, uint32_t createFlags);
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
// A context started (FUN_1412005a0): the root identity, the clock and the pass's switches.
void M2AnimateContextInit(M2AnimateContext* context, uint32_t time, uint32_t frame, uint32_t compute,
    uint32_t throttle, uint32_t far, uint32_t a7);
// The "M2 Bone Seq Pool" (DAT_1430a0f60).
TObjectAllocHeap* M2GetBoneSeqPool();

// The file's waiting list (FUN_1411c62e0 joins at the tail, FUN_1411cdb30 leaves).
void M2ModelLinkWaiting(CM2Model* model);
void M2ModelUnlinkWaiting(CM2Model* model);
// The file is in: the model sets itself up from it (FUN_1411c24f0).
bool M2ModelInitializeLoaded(CM2Model* model);
// CM2Model's pools (FUN_1411c5d90).
void M2ModelInitializePools();
// FUN_1411b7d60
void M2ModelSetFlag400(bool set);
// The module's own scene (DAT_1430a0ed0, FUN_141196e40(9, ...)).
void M2CreateModuleScene();

#endif
