#ifndef UI_C_SIMPLE_MODEL_HPP
#define UI_C_SIMPLE_MODEL_HPP

// Engine/Source/Frame/CSimpleModel ("Model", 0x4d0 bytes; vtable 0x14297e430, CLayoutFrame
// vtable 0x14297e780 at +0x18, CAnimatableObject vtable 0x14297e878 at +0x1d8): a frame that
// shows one M2 model in a world scene of its own. The model is placed by a transform built from
// the frame's position, facing, pitch, roll and scale, lit by the frame's light, seen through
// one of the model's cameras (or a custom one) and drawn with the frame's fog by a render
// callback the frame adds to its batch.

#include <cstdint>

#include "model2/CM2Lighting.hpp"
#include "ui/CSimpleFrame.hpp"

class CWorldScene;
class CM2Model;
class CM2Camera;
struct lua_State;

// The fog a model frame draws with (0xa0, initialised by FUN_1402394a0): the colour at +0, the
// near and far distances at +4 and +8, the colour again at +0x30..+0x38.
struct MODELFOG {
    uint32_t m_words[40];

    // ref: FUN_1402394a0
    MODELFOG();

    uint32_t GetColor() const {
        return this->m_words[0];
    }
    float Get(int32_t word) const;
    void Set(int32_t word, float value);
    // The colour in its four slots (FUN_1424bed40's stores).
    void SetColor(uint32_t argb);
};
static_assert(sizeof(MODELFOG) == 0xa0, "MODELFOG is 0xa0 bytes");

// The Lua light (FUN_140d46a30 pushes it, FUN_140d46ba0 reads it).
struct SCRIPTLIGHT {
    uint8_t m_omnidirectional;      // 0x00
    uint8_t m_pad01[3];
    float m_point[3];               // 0x04
    float m_ambientIntensity;       // 0x10
    uint32_t m_ambientColor;        // 0x14
    uint8_t m_hasAmbient;           // 0x18
    uint8_t m_pad19[3];
    float m_diffuseIntensity;       // 0x1c
    uint32_t m_diffuseColor;        // 0x20
    uint8_t m_hasDiffuse;           // 0x24
    uint8_t m_pad25[3];
};

// What the render callback hands the scene for a frame (the stack block FUN_1424bcd80 builds).
struct MODELFRAMEVIEW {
    float m_rect[4];                // 0x00 the frame's rectangle (CRect order)
    float m_translation[2];         // 0x10 scaled by the layout scale
    float m_insets[4];              // 0x18 top, left, bottom, right, scaled
    MODELFOG m_fog;                 // 0x28
    uint8_t m_hasTransform;         // 0xc8
};

class CSimpleModel : public CSimpleFrame {
    public:
    // m_modelFlags.
    enum : uint8_t {
        MODEL_FOG_COLOR = 0x1,
        MODEL_CUSTOM_CAMERA = 0x2,
        MODEL_VISIBLE = 0x4,            // the last IsOnScreen seen on a size change
        MODEL_TRANSFORM = 0x8,
        MODEL_USE_CENTER = 0x10,
    };

    CWorldScene* m_scene;               // 0x300
    CM2Model* m_model;                  // 0x308
    CM2Camera* m_overrideCamera;        // 0x310
    CM2Camera* m_camera;                // 0x318
    int32_t m_pendingCameraId;          // 0x320 -1 none
    int32_t m_pendingCameraIndex;       // 0x324 -1 none
    CM2Lighting m_lighting;             // 0x328
    float m_position[3];                // 0x3c0
    float m_facing;                     // 0x3cc
    float m_pitch;                      // 0x3d0
    float m_roll;                       // 0x3d4
    float m_modelScale;                 // 0x3d8
    float m_elapsed;                    // 0x3dc the last update's
    float m_cameraDistance;             // 0x3e0
    uint32_t m_drawFlags;               // 0x3e4
    uint32_t m_drawLayer;               // 0x3e8 low 4 bits
    uint8_t m_modelFlags;               // 0x3ec
    uint8_t m_pad3ed[3];
    MODELFOG m_fog;                     // 0x3f0
    float m_bounds[6];                  // 0x490 min, max
    float m_center[3];                  // 0x4a8
    float m_viewTranslation[2];         // 0x4b4
    float m_viewInsets[4];              // 0x4bc top, left, bottom, right
    uint8_t m_pad4cc[4];

    // ref: FUN_1424bcfa0
    explicit CSimpleModel(CSimpleFrame* parent);
    // ref: FUN_1424bd140
    ~CSimpleModel() override;
    // Model frames come from their own pool (0x1447aee28, 5 per block).
    static void* operator new(size_t size);
    static void operator delete(void* frame);

    // This class's type id (DAT_14309fedc).
    static int32_t GetObjectType();
    // ref: FUN_1424bd400
    static void CreateScriptMetaTables();
    // ref: FUN_1424bd450
    static void DestroyScriptMetaTables();
    static int32_t s_metaTable;             // DAT_142d5e500
    static int32_t s_restrictedMetaTable;   // DAT_142d5e504
    // Whether models draw their first draw flag (DAT_142d1b684).
    static bool s_drawFlagAll;

    int32_t GetScriptMetaTable() override;
    bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) override;
    void RunOnUpdate(float elapsed) override;
    bool WantsUpdate() override;
    void AddRegionsToBatch(CRenderBatch* batch) override;
    void UpdateEffectiveAlpha() override;
    void ScaleBy(float* scale) override;
    float GetWidth() override;
    float GetHeight() override;
    void OnFrameSizeChanged(const CRect* rect, const CRect* previous) override;

    // The new virtuals (0x318..0x348).
    // ref: FUN_1424bf000
    virtual void SetModel(CM2Model* model);
    // ref: FUN_1424bef80
    virtual void SetModelById(int32_t fileDataId, uint32_t noMip);
    // ref: FUN_1424bdb60
    virtual bool IsModelReady();
    // ref: FUN_1416977e0
    virtual float GetModelScale();
    // ref: FUN_1402112e0 (the base has no glow)
    virtual void SetGlow(float glow);
    // ref: FUN_1424bf530
    virtual void UpdateModelTransform();
    // ref: FUN_1424be110
    virtual void OnModelLoaded(CM2Model* model);

    // ref: FUN_1424bd8b0: the frame's world scene, made on first use.
    CWorldScene* GetScene();
    // ref: FUN_1424bd2c0
    void ClearModel();
    // ref: FUN_1424bf150
    void SetModelFile(int32_t fileDataId, bool noMip);
    // ref: FUN_1424be470
    void SetCameraIndex(uint32_t index);
    // ref: FUN_1424be560
    void SetCamera(uint32_t id);
    // ref: FUN_1424be650
    void SetCustomCamera(uint32_t id);
    // ref: FUN_1424be400
    void SetCameraObject(CM2Camera* camera);
    // ref: FUN_1424bda60
    void UseCustomCamera(CM2Camera* camera);
    // ref: FUN_1424bde50
    void MakeCurrentCameraCustom();
    // ref: FUN_1424bd4a0 / FUN_1424bd5c0
    float* GetCameraPosition(float* out);
    float* GetCameraTarget(float* out);
    // ref: FUN_1424bd4e0
    float GetCameraFacing();
    // ref: FUN_1424bd5a0
    float GetCameraRoll();
    // ref: FUN_1424be6b0
    void SetCameraDistance(float distance);
    // ref: FUN_1424be7b0
    void SetCameraFacing(float radians);
    // ref: FUN_1424be8a0
    void SetCameraPosition(float* position);
    // ref: FUN_1424bea10
    void SetCameraTarget(float* target);
    // ref: FUN_1424be9e0
    void SetCameraRoll(float radians);
    // ref: FUN_1424bd600 / FUN_1424bd960
    float GetDesaturation();
    float GetShadowEffect();
    // ref: FUN_1424becb0 / FUN_1424bf3b0
    void SetDesaturation(float strength);
    void SetShadowEffect(float strength);
    // ref: FUN_1424bd640
    uint32_t GetDrawFlags() const;
    // ref: FUN_1424bd820
    float GetModelAlpha();
    // ref: FUN_1424bf130
    void SetModelAlpha(float alpha);
    // ref: FUN_1424bd840
    int32_t GetModelFileId();
    // ref: FUN_1424bd870
    bool GetPaused();
    // ref: FUN_1424bf270 / FUN_1424bf230
    void SetPaused(bool paused);
    void SetParticlesEnabled(bool enabled);
    // ref: FUN_1424bda00
    float GetWorldScale();
    // ref: FUN_1424bda30
    bool HasAttachmentPoints();
    // ref: FUN_1424bed10
    void SetModelDrawLayer(uint32_t layer);
    // ref: FUN_1424bed40
    void SetFogColor(uint32_t rgb, const uint8_t* alpha);
    // ref: FUN_1424bd6c0
    void GetLight(bool* enabled, void* light);
    // ref: FUN_1424bed90
    void SetLight(bool enabled, const void* light);
    // ref: FUN_1424bf2b0
    void SetSequence(uint32_t sequence);
    // ref: FUN_1424bf330
    bool SetSequenceTime(uint32_t sequence, int32_t timeOffset);
    // ref: FUN_1424beb50
    void SetTransform(const void* translation, const void* rotation, const void* scale);
    // ref: FUN_1424bf430
    float* TransformCameraSpaceToModelSpace(float* out, const float* position);
    // ref: FUN_1424be330
    void ReplaceIconTexture(const int32_t* fileDataId);
    // ref: FUN_1424be340
    void ReplaceTexture(int32_t slot, int32_t fileDataId);
    // ref: FUN_1424bf220
    void SetOverrideCamera(CM2Camera* camera);
};

static_assert(sizeof(CSimpleModel) == 0x4d0, "CSimpleModel is 0x4d0 bytes");

void CSimpleModel_FillScriptMethods(lua_State* L);
// A light table at `index` (FUN_140d46ba0).
void ReadScriptLight(lua_State* L, int32_t index, const char* usage, SCRIPTLIGHT* light);
// A CM2Lighting from a script light (FUN_1424bd2d0).
void LightingFromScript(CM2Lighting* lighting, const void* light);

#endif
