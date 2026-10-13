#include "ui/CSimpleModel.hpp"

#include <cfloat>
#include <cmath>
#include <cstring>

#include "base/DataAllocator.hpp"
#include "base/Matrix.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "gx/GxShader.hpp"
#include "model2/M2Api.hpp"
#include "texture/Texture.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CRenderBatch.hpp"
#include "ui/CSimpleTop.hpp"
#include "util/Unimplemented.hpp"
#include "base/Coordinate.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

int32_t CSimpleModel::s_metaTable = -2;
int32_t CSimpleModel::s_restrictedMetaTable = -2;
bool CSimpleModel::s_drawFlagAll = true;

namespace {

// The model frames' pool (0x1447aee28).
CDataAllocator s_modelHeap(0x4d0, 5);  // DAT_1447aee28

// The scripts of a model frame.
enum : uint8_t {
    SCRIPT_ONMODELLOADED = 0x2d,
    SCRIPT_ONMODELCLEARED = 0x44,
    SCRIPT_ONMODELLOADING = 0x45,
};

constexpr float BYTE_SCALE = 1.0f / 255.0f;     // DAT_142632528
constexpr float MIN_DISTANCE = 0.1f;            // DAT_14261a97c
constexpr float MAX_FOV = 0.3f;                 // DAT_142618d5c
constexpr float NEAR_TARGET = 0.01f;            // DAT_1427fb104
constexpr float UNITS_PER_HEIGHT = 1.6666666f;  // DAT_142737e34
constexpr uint32_t FRAME2_STRATA = 0x780;
constexpr uint32_t FRAME2_STRATA_TOP = 0x480;


// An optional vector or number of SetTransform (the value, then whether it was given).
struct OPTIONALVECTOR {
    float m_value[3];
    uint8_t m_present;
};
struct OPTIONALFLOAT {
    float m_value;
    uint8_t m_present;
};

void ScaledColor(uint32_t color, bool present, float intensity, float* out) {
    uint32_t c = present ? color : 0;
    out[0] = static_cast<float>((c >> 16) & 0xff) * BYTE_SCALE * intensity;
    out[1] = static_cast<float>((c >> 8) & 0xff) * BYTE_SCALE * intensity;
    out[2] = static_cast<float>(c & 0xff) * BYTE_SCALE * intensity;
}

// The batch's draw: the scene through the frame's camera into the frame's rectangle.
// ref: FUN_1424bcd80
void RenderModel(void* param, const float* view, const float* projection, void* target) {
    (void)view;
    (void)projection;
    auto frame = static_cast<CSimpleModel*>(param);
    CM2Camera* camera = frame->m_overrideCamera != nullptr ? frame->m_overrideCamera : frame->m_camera;
    if (camera == nullptr || frame->m_scene == nullptr || !frame->IsOnScreen()) {
        return;
    }
    float fov = M2CameraGetFloat(camera, 3);
    if (MIN_DISTANCE < fov && fov < MAX_FOV) {
        // the reference resets a near-degenerate field of view (the value is set by FUN_141296160)
        M2CameraSetFloat(camera, 3, fov);
    }
    frame->UpdateModelTransform();
    MODELFRAMEVIEW block;
    block.m_fog = frame->m_fog;
    if (!(frame->m_modelFlags & CSimpleModel::MODEL_FOG_COLOR)) {
        block.m_fog.m_words[1] = 0x4b189680;    // 1e7
        block.m_fog.m_words[2] = 0x4cbebc20;    // 1e8
        block.m_fog.m_words[4] = 0;
    }
    float scale = frame->m_layoutScale;
    block.m_hasTransform = (frame->m_modelFlags & CSimpleModel::MODEL_TRANSFORM) != 0;
    std::memcpy(block.m_rect, &frame->m_rect, sizeof(block.m_rect));
    block.m_translation[0] = scale * frame->m_viewTranslation[0];
    block.m_translation[1] = scale * frame->m_viewTranslation[1];
    for (int32_t i = 0; i < 4; i++) {
        block.m_insets[i] = frame->m_viewInsets[i] * scale;
    }
    uint8_t cameraView[0x64];
    void* cameraBlock = WorldSceneViewFromCamera(cameraView, camera);
    WorldSceneRenderFrame(frame->m_scene, WorldSceneGetParams(frame->m_scene), cameraBlock, &block, target,
        frame->m_elapsed, frame->GetDrawFlags());
}

// The model's notifications.
// ref: LAB_1424bd2b0
void ModelCleared(void* model, void* param) {
    (void)model;
    static_cast<CSimpleModel*>(param)->RunScript(SCRIPT_ONMODELCLEARED, nullptr);
}

// ref: LAB_141a92e70
void ModelLoading(void* model, void* param) {
    (void)model;
    static_cast<CSimpleModel*>(param)->RunScript(SCRIPT_ONMODELLOADING, nullptr);
}

// The frame's light onto the model, when it has one (FUN_1411ff860).
// ref: LAB_1424bdb90
void ModelLighting(void* model, CM2ModelLighting* lighting, void* param) {
    (void)model;
    auto frame = static_cast<CSimpleModel*>(param);
    if (frame->m_lighting.m_enabled != 0) {
        M2ModelLightingAdd(lighting, &frame->m_lighting);
    }
}

// ref: FUN_1424bdea0
void ModelLoaded(CM2Model* model, void* param) {
    auto frame = static_cast<CSimpleModel*>(param);
    frame->OnModelLoaded(model);
    frame->RunScript(SCRIPT_ONMODELLOADED, nullptr);
}

}

// The light's kind, then its point or direction, then the ambient and diffuse colours
// scaled by their intensities (the three ambient slots alike).
// ref: FUN_1424bd2d0
void LightingFromScript(CM2Lighting* lighting, const void* in) {
    auto light = static_cast<const SCRIPTLIGHT*>(in);
    lighting->SetLightType(light->m_omnidirectional != 0 ? CM2Lighting::LIGHT_POINT : CM2Lighting::LIGHT_DIRECTIONAL);
    if (lighting->m_type == CM2Lighting::LIGHT_POINT) {
        lighting->SetPosition(light->m_point);
    } else {
        lighting->SetDirection(light->m_point);
    }
    float ambient[3];
    ScaledColor(light->m_ambientColor, light->m_hasAmbient != 0, light->m_ambientIntensity, ambient);
    for (int32_t i = 0; i < 3; i++) {
        lighting->m_ambient[i][0] = ambient[0];
        lighting->m_ambient[i][1] = ambient[1];
        lighting->m_ambient[i][2] = ambient[2];
    }
    ScaledColor(light->m_diffuseColor, light->m_hasDiffuse != 0, light->m_diffuseIntensity, lighting->m_diffuse);
}

// ---- construction ------------------------------------------------------------------------------

// ref: FUN_1424bcfa0
CSimpleModel::CSimpleModel(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_scene = nullptr;
    this->m_model = nullptr;
    this->m_overrideCamera = nullptr;
    this->m_camera = nullptr;
    this->m_pendingCameraId = 0;
    this->m_pendingCameraIndex = 0;
    this->m_position[0] = 0.0f;
    this->m_position[1] = 0.0f;
    this->m_position[2] = 0.0f;
    this->m_facing = 0.0f;
    this->m_pitch = 0.0f;
    this->m_roll = 0.0f;
    this->m_modelScale = 1.0f;
    this->m_elapsed = 0.0f;
    this->m_drawLayer = (this->m_drawLayer & 0xfffffff2) | 2;
    this->m_drawFlags = 0;
    this->m_modelFlags = 0;
    std::memset(this->m_bounds, 0, sizeof(this->m_bounds));
    std::memset(this->m_center, 0, sizeof(this->m_center));
    std::memset(this->m_viewTranslation, 0, sizeof(this->m_viewTranslation));
    std::memset(this->m_viewInsets, 0, sizeof(this->m_viewInsets));
    this->m_lighting.SetEnabled(0);
    this->m_lighting.SetLightType(CM2Lighting::LIGHT_POINT);
    for (int32_t i = 0; i < 3; i++) {
        for (int32_t c = 0; c < 3; c++) {
            this->m_lighting.m_ambient[i][c] = 1.0f;
        }
        this->m_lighting.m_diffuse[i] = 1.0f;
    }
}

// ref: FUN_1424bd140
CSimpleModel::~CSimpleModel() {
    if (this->m_camera != nullptr) {
        M2CameraRelease(&this->m_camera);
    }
    if (this->m_model != nullptr) {
        M2ModelRelease(this->m_model);
    }
    if (this->m_scene != nullptr) {
        WorldSceneRelease(this->m_scene);
    }
}

void* CSimpleModel::operator new(size_t size) {
    (void)size;
    return s_modelHeap.GetData(false, ".?AVCSimpleModel@@", -2, 0x1f);
}

void CSimpleModel::operator delete(void* frame) {
    if (frame != nullptr) {
        s_modelHeap.PutData(frame);
    }
}

// DAT_14309fedc
int32_t CSimpleModel::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_1424bd400
void CSimpleModel::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleModel_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleModel_FillScriptMethods, true);
}

// ref: FUN_1424bd450
void CSimpleModel::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object ------------------------------------------------------------------------

// ref: FUN_1424bd930
int32_t CSimpleModel::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424bf410
bool CSimpleModel::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (index == SCRIPT_ONMODELLOADED || index == SCRIPT_ONMODELCLEARED || index == SCRIPT_ONMODELLOADING) {
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_141697a20
bool CSimpleModel::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Model") == 0 || _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_141697830
bool CSimpleModel::IsObjectType(int32_t type) {
    if (type == CSimpleModel::GetObjectType() || type == CSimpleFrame::GetObjectType()
        || type == CScriptRegion::GetObjectType()) {
        return true;
    }
    return this->CScriptObject::IsObjectType(type);
}

// ref: FUN_1424bd860
const char* CSimpleModel::GetObjectTypeName() {
    return "Model";
}

// ref: FUN_141a968e0
uint32_t CSimpleModel::GetObjectSize() {
    return sizeof(CSimpleModel);
}

// ---- CSimpleFrame ------------------------------------------------------------------------------

// ref: FUN_1424bdbb0
void CSimpleModel::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x2000)) {
        return;
    }
    auto xml = static_cast<XMLMODEL*>(node);
    if (xml->m_modelFlags & 1) {
        const char* file = xml->m_file.m_text;
        CM2Model* model = nullptr;
        if (file != nullptr && file[0] != '\0') {
            // FUN_1413555c0
            model = WorldSceneCreateModelByName(this->GetScene(), file, 0, 1, 0);
        }
        this->SetModel(model);
        if (model != nullptr) {
            M2ModelRelease(model);
        }
        if (this->m_model == nullptr) {
            const char* name = this->GetName();
            if (name == nullptr) {
                name = this->GetObjectTypeName();
            }
            status->Add(1, "Frame %s: Bad model file: %s", name, xml->m_file.m_text);
        }
    }
    if (xml->m_modelFlags & 2) {
        float scale = xml->m_modelScale;
        if (scale <= 0.0f) {
            const char* name = this->GetName();
            if (name == nullptr) {
                name = this->GetObjectTypeName();
            }
            status->Add(1, "Frame %s: Invalid model scale: %f", name, static_cast<double>(scale));
        } else {
            this->m_modelScale = scale;
        }
    }
    if (xml->m_modelFlags & 4) {
        this->m_fog.Set(1, xml->m_fogNear > 0.0f ? xml->m_fogNear : 0.0f);
    }
    if (xml->m_modelFlags & 8) {
        this->m_fog.Set(2, xml->m_fogFar > 0.0f ? xml->m_fogFar : 0.0f);
    }
    if (XMLOBJECT* color = XMLObjectFindChild(node, XMLTYPE_FOGCOLOR)) {
        uint32_t argb = XmlColorPack(static_cast<XMLCOLOR*>(color));
        this->m_fog.SetColor(argb);
        this->m_modelFlags |= MODEL_FOG_COLOR;
    }
    if (XMLOBJECT* insets = XMLObjectFindChild(node, XMLTYPE_VIEWINSETS)) {
        float left = 0.0f;
        float right = 0.0f;
        float top = 0.0f;
        float bottom = 0.0f;
        XMLInsetGet(static_cast<XMLINSET*>(insets), &left, &right, &top, &bottom, status);
        this->m_viewInsets[0] = top;
        this->m_viewInsets[1] = left;
        this->m_viewInsets[2] = bottom;
        this->m_viewInsets[3] = right;
    }
    if ((xml->m_frameFlags & 0x100000) && (this->m_drawLayer & 0xf) != xml->m_drawLayer) {
        this->m_drawLayer = ((this->m_drawLayer ^ xml->m_drawLayer) & 0xf) ^ this->m_drawLayer;
        this->MarkBatchDirty();
    }
}

// ref: FUN_1424be040
void CSimpleModel::RunOnUpdate(float elapsed) {
    this->CSimpleFrame::RunOnUpdate(elapsed);
    if (this->m_model != nullptr && (this->m_layoutState & LAYOUT_VISIBLE) && this->m_pendingCameraId == -1) {
        M2ModelSetBucketed(this->m_model, true);
        M2ModelSetShown(this->m_model, true);
    }
    this->m_elapsed = elapsed;
    uint32_t flags = this->GetDrawFlags();
    CWorldScene* scene = this->GetScene();
    WorldSceneAnimate(scene, WorldSceneGetParams(scene), elapsed, 0, 0, flags, 1);
}

// ref: FUN_14026bea0
bool CSimpleModel::WantsUpdate() {
    return true;
}

// ref: FUN_1424bdf50
void CSimpleModel::AddRegionsToBatch(CRenderBatch* batch) {
    this->CSimpleFrame::AddRegionsToBatch(batch);
    if (this->m_model != nullptr && M2ModelIsLoaded(this->m_model)) {
        uint64_t drawOrder = this->GetDrawOrderKey(static_cast<int32_t>(this->m_drawLayer & 0xf), 0);
        batch->AddCallback(this, drawOrder, RenderModel, this, (this->m_flags2 & FRAME2_STRATA) == FRAME2_STRATA_TOP);
    }
}

// ref: FUN_1424bf4d0
void CSimpleModel::UpdateEffectiveAlpha() {
    this->CSimpleFrame::UpdateEffectiveAlpha();
    if (this->m_model != nullptr) {
        M2ModelSetAlpha(this->m_model, static_cast<float>(this->GetAlpha()) * BYTE_SCALE);
    }
}

// ref: FUN_1424bded0
void CSimpleModel::ScaleBy(float* scale) {
    this->m_viewTranslation[0] *= *scale;
    this->m_viewTranslation[1] *= *scale;
    for (int32_t i = 0; i < 4; i++) {
        this->m_viewInsets[i] *= *scale;
    }
    this->CSimpleFrame::ScaleBy(scale);
}

// ref: FUN_1424bd9a0
float CSimpleModel::GetWidth() {
    float width = this->CLayoutFrame::GetWidth();
    if (width == 0.0f && this->m_model != nullptr && M2ModelIsLoaded(this->m_model)) {
        return this->m_bounds[3] - this->m_bounds[0];
    }
    return width;
}

// ref: FUN_1424bd660
float CSimpleModel::GetHeight() {
    float height = this->CLayoutFrame::GetHeight();
    if (height == 0.0f && this->m_model != nullptr && M2ModelIsLoaded(this->m_model)) {
        return this->m_bounds[4] - this->m_bounds[1];
    }
    return height;
}

// ref: FUN_1424bdfd0
void CSimpleModel::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    this->CSimpleFrame::OnFrameSizeChanged(rect, previous);
    bool visible = this->IsOnScreen();
    if (((this->m_modelFlags & MODEL_VISIBLE) != 0) != visible) {
        this->MarkBatchDirty();
        if (visible) {
            this->m_modelFlags |= MODEL_VISIBLE;
        } else {
            this->m_modelFlags &= ~MODEL_VISIBLE;
        }
    }
}

// ---- the model -------------------------------------------------------------------------------

// ref: FUN_1424bd8b0
CWorldScene* CSimpleModel::GetScene() {
    if (this->m_scene == nullptr) {
        this->m_scene = WorldSceneCreate(0, 5, true);
        WorldSceneParamsSetFlags(WorldSceneGetParams(this->m_scene), 0x3fbf640a, false);
        WorldSceneParamsSetFlags(WorldSceneGetParams(this->m_scene), 0x1000, true);
    }
    return this->m_scene;
}

// ref: FUN_1424bf000
void CSimpleModel::SetModel(CM2Model* model) {
    if (model != nullptr) {
        M2ModelAddRef(model);
    }
    if (this->m_model != nullptr) {
        void* none[2] = { nullptr, nullptr };
        M2ModelSetCallbackCC990(this->m_model, nullptr, none);
        M2ModelSetCallback318(this->m_model, nullptr, nullptr);
        M2ModelSetLoadCallback(this->m_model, nullptr, nullptr);
        M2ModelRelease(this->m_model);
    }
    this->m_model = model;
    if (model != nullptr) {
        M2ModelSetAlpha(model, static_cast<float>(this->GetAlpha()) * BYTE_SCALE);
        void* closure[2] = { this, nullptr };
        M2ModelSetCallback420(model, reinterpret_cast<void*>(ModelCleared), closure);
        M2ModelSetCallbackCC990(model, reinterpret_cast<void*>(ModelLoading), closure);
        M2ModelSetCallback318(model, reinterpret_cast<void*>(ModelLighting), this);
        M2ModelSetLoadCallback(model, ModelLoaded, this);
    }
    this->MarkBatchDirty();
}

// ref: FUN_1424bef80
void CSimpleModel::SetModelById(int32_t fileDataId, uint32_t noMip) {
    CM2Model* model = nullptr;
    if (fileDataId != 0) {
        model = WorldSceneCreateModelById(this->GetScene(), fileDataId, (noMip & 0xff) << 8, 1, 0);
    }
    this->SetModel(model);
    if (model != nullptr) {
        M2ModelRelease(model);
    }
}

// ref: FUN_1424bdb60
bool CSimpleModel::IsModelReady() {
    return this->m_model != nullptr && M2ModelIsReady(this->m_model);
}

// ref: FUN_1416977e0
float CSimpleModel::GetModelScale() {
    return this->m_modelScale;
}

// ref: FUN_1402112e0
void CSimpleModel::SetGlow(float glow) {
    (void)glow;
}

// ref: FUN_1424bf530
void CSimpleModel::UpdateModelTransform() {
    if (this->m_model == nullptr || !M2ModelIsLoaded(this->m_model) || (this->m_modelFlags & MODEL_TRANSFORM)) {
        return;
    }
    float scale = this->GetModelScale() * this->m_layoutScale * (g_coordinateSpace.m_diagonalHeight * UNITS_PER_HEIGHT);
    float matrix[16];
    MatrixIdentity(matrix);
    float center[3] = { scale * this->m_center[0], scale * this->m_center[1], scale * this->m_center[2] };
    float step[16];
    MatrixIdentity(step);
    float back[3] = { -center[0], -center[1], -center[2] };
    MatrixTranslate(step, back);
    MatrixMultiply(matrix, matrix, step);
    float rotation[4];
    QuaternionFromEuler(rotation, this->m_facing, this->m_pitch, this->m_roll);
    MatrixFromQuaternion(step, rotation);
    MatrixMultiply(matrix, matrix, step);
    if (!(this->m_modelFlags & MODEL_USE_CENTER)) {
        MatrixIdentity(step);
        MatrixTranslate(step, center);
        MatrixMultiply(matrix, matrix, step);
    }
    MatrixIdentity(step);
    float position[3] = { scale * this->m_position[0], scale * this->m_position[1], scale * this->m_position[2] };
    MatrixTranslate(step, position);
    MatrixMultiply(matrix, matrix, step);
    MatrixScaleUniform(matrix, scale);
    // FUN_141356ea0
    M2ModelSetWorldTransform(this->m_model, matrix);
}

// ref: FUN_1424be110
void CSimpleModel::OnModelLoaded(CM2Model* model) {
    int32_t id = this->m_pendingCameraId;
    bool apply = true;
    CM2Camera* camera = nullptr;
    bool clear = false;
    if (id == -1) {
        int32_t index = this->m_pendingCameraIndex;
        if (index == -1) {
            apply = false;
        } else if (this->m_model != nullptr && M2ModelIsLoaded(this->m_model)) {
            if (static_cast<uint32_t>(index) < 2) {
                camera = M2ModelGetCameraByIndex(this->m_model, static_cast<uint32_t>(index));
            } else {
                clear = true;
            }
        } else {
            this->m_pendingCameraIndex = index;
            apply = false;
        }
    } else if (this->m_model != nullptr && M2ModelIsLoaded(this->m_model)) {
        if (static_cast<uint32_t>(id) < M2ModelGetCameraCount(this->m_model)) {
            camera = M2ModelGetCameraById(this->m_model, static_cast<uint32_t>(id));
        } else {
            clear = true;
        }
    } else {
        this->m_pendingCameraId = id;
        apply = false;
    }
    if (apply) {
        if (clear) {
            camera = nullptr;
        } else if (camera != nullptr) {
            M2CameraAddRef(camera);
        }
        if (this->m_camera != nullptr) {
            M2CameraRelease(&this->m_camera);
        }
        this->m_camera = camera;
        this->m_pendingCameraId = -1;
        this->m_pendingCameraIndex = -1;
        this->m_modelFlags &= ~MODEL_CUSTOM_CAMERA;
    }
    if (!M2ModelHasExternalTransform(this->m_model)) {
        this->UpdateModelTransform();
    }
    M2ModelResetEmitters(this->m_model);
    M2ModelSetBucketed(model, true);
    M2ModelSetSceneFlagDAE8(model, false);
    CWorldScene* scene = this->GetScene();
    WorldSceneUpdate(scene, WorldSceneGetParams(scene), 1, 0);
    M2ModelSetSceneFlagDAE8(model, true);
    M2ModelGetBounds(this->m_bounds, model);
    M2ModelGetCenter(this->m_center, this->m_model, -1, 0);
    if (this->m_center[0] == FLT_MAX && this->m_center[1] == FLT_MAX && this->m_center[2] == FLT_MAX) {
        this->m_center[0] = 0.0f;
        this->m_center[1] = 0.0f;
        this->m_center[2] = 0.0f;
    }
    this->Resize(false);
    this->MarkBatchDirty();
}

void CSimpleModel::ClearModel() {
    this->SetModel(nullptr);
}

// ref: FUN_1424bf150
void CSimpleModel::SetModelFile(int32_t fileDataId, bool noMip) {
    this->SetModelById(fileDataId, noMip ? 1 : 0);
    if (this->m_model == nullptr) {
        char name[0x104];
        GxShaderName(fileDataId, name, sizeof(name));
        FrameScript_ActionError("Invalid model file: %s", name);
    }
}

// ref: FUN_1424be470
void CSimpleModel::SetCameraIndex(uint32_t index) {
    if (this->m_model != nullptr && M2ModelIsLoaded(this->m_model)) {
        CM2Camera* camera = nullptr;
        if (index < 2) {
            camera = M2ModelGetCameraByIndex(this->m_model, index);
        }
        this->SetCameraObject(camera);
        return;
    }
    this->m_pendingCameraIndex = static_cast<int32_t>(index);
}

// ref: FUN_1424be560
void CSimpleModel::SetCamera(uint32_t id) {
    if (this->m_model != nullptr && M2ModelIsLoaded(this->m_model)) {
        CM2Camera* camera = nullptr;
        if (id < M2ModelGetCameraCount(this->m_model)) {
            camera = M2ModelGetCameraById(this->m_model, id);
        }
        this->SetCameraObject(camera);
        return;
    }
    this->m_pendingCameraId = static_cast<int32_t>(id);
}

// ref: FUN_1424be650
void CSimpleModel::SetCustomCamera(uint32_t id) {
    if (this->m_model == nullptr || id >= M2ModelGetCameraCount(this->m_model)) {
        return;
    }
    CM2Camera* camera = M2ModelGetCameraById(this->m_model, id);
    if (camera != nullptr) {
        camera = M2CameraClone(camera);
        if (camera != nullptr) {
            this->UseCustomCamera(camera);
        }
    }
}

// ref: FUN_1424be400
void CSimpleModel::SetCameraObject(CM2Camera* camera) {
    if (camera != nullptr) {
        M2CameraAddRef(camera);
    }
    if (this->m_camera != nullptr) {
        M2CameraRelease(&this->m_camera);
    }
    this->m_camera = camera;
    this->m_pendingCameraId = -1;
    this->m_pendingCameraIndex = -1;
    this->m_modelFlags &= ~MODEL_CUSTOM_CAMERA;
}

// ref: FUN_1424bda60
void CSimpleModel::UseCustomCamera(CM2Camera* camera) {
    if (camera == nullptr) {
        return;
    }
    CM2Camera* held = camera;
    this->SetCameraObject(camera);
    M2CameraRelease(&held);
    this->m_modelFlags |= MODEL_CUSTOM_CAMERA;
    float position[3] = {};
    float target[3] = {};
    M2CameraGetVector(this->m_camera, 7, position);
    M2CameraGetVector(this->m_camera, 8, target);
    float dx = position[0] - target[0];
    float dy = position[1] - target[1];
    this->m_cameraDistance = std::sqrt(dy * dy + dx * dx);
}

// ref: FUN_1424bde50
void CSimpleModel::MakeCurrentCameraCustom() {
    if (this->m_modelFlags & MODEL_CUSTOM_CAMERA) {
        return;
    }
    CM2Camera* camera = this->m_camera == nullptr ? M2CameraCreateDefault() : M2CameraClone(this->m_camera);
    if (camera != nullptr) {
        this->UseCustomCamera(camera);
    }
}

// ref: FUN_1424bd4a0
float* CSimpleModel::GetCameraPosition(float* out) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    if (this->m_camera != nullptr) {
        M2CameraGetVector(this->m_camera, 7, out);
    }
    return out;
}

// ref: FUN_1424bd5c0
float* CSimpleModel::GetCameraTarget(float* out) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    if (this->m_camera != nullptr) {
        M2CameraGetVector(this->m_camera, 8, out);
    }
    return out;
}

// ref: FUN_1424bd4e0
float CSimpleModel::GetCameraFacing() {
    if (this->m_camera == nullptr) {
        return 0.0f;
    }
    float position[3] = {};
    float target[3] = {};
    M2CameraGetVector(this->m_camera, 7, position);
    M2CameraGetVector(this->m_camera, 8, target);
    float dx = position[0] - target[0];
    float dy = position[1] - target[1];
    if (0.0f < dy * dy + dx * dx) {
        // FUN_142412a68
        return std::atan2(dy, dx);
    }
    return 0.0f;
}

// ref: FUN_1424bd5a0
float CSimpleModel::GetCameraRoll() {
    if (this->m_camera == nullptr) {
        return 0.0f;
    }
    return M2CameraGetFloat(this->m_camera, 5);
}

// ref: FUN_1424be6b0
void CSimpleModel::SetCameraDistance(float distance) {
    if (!(this->m_modelFlags & MODEL_CUSTOM_CAMERA)) {
        FrameScript_ActionError("Not using a custom camera.");
        return;
    }
    if (this->m_camera == nullptr) {
        return;
    }
    if (distance <= MIN_DISTANCE) {
        distance = MIN_DISTANCE;
    }
    float position[3] = {};
    float target[3] = {};
    M2CameraGetVector(this->m_camera, 7, position);
    M2CameraGetVector(this->m_camera, 8, target);
    float facing = this->GetCameraFacing();
    position[0] = std::cos(facing) * distance + target[0];
    position[1] = std::sin(facing) * distance + target[1];
    M2CameraSetVector(this->m_camera, 7, position);
    this->m_cameraDistance = distance;
}

// ref: FUN_1424be7b0
void CSimpleModel::SetCameraFacing(float radians) {
    if (!(this->m_modelFlags & MODEL_CUSTOM_CAMERA)) {
        FrameScript_ActionError("Not using a custom camera.");
        return;
    }
    if (this->m_camera == nullptr) {
        return;
    }
    float position[3] = {};
    float target[3] = {};
    M2CameraGetVector(this->m_camera, 7, position);
    M2CameraGetVector(this->m_camera, 8, target);
    position[0] = std::cos(radians) * this->m_cameraDistance + target[0];
    position[1] = std::sin(radians) * this->m_cameraDistance + target[1];
    M2CameraSetVector(this->m_camera, 7, position);
}

// ref: FUN_1424be8a0
void CSimpleModel::SetCameraPosition(float* position) {
    if (!(this->m_modelFlags & MODEL_CUSTOM_CAMERA)) {
        FrameScript_ActionError("Not using a custom camera.");
        return;
    }
    if (this->m_camera == nullptr) {
        return;
    }
    float target[3] = {};
    M2CameraGetVector(this->m_camera, 8, target);
    float dx = position[0] - target[0];
    float dy = position[1] - target[1];
    float squared = dy * dy + dx * dx;
    if (squared < NEAR_TARGET) {
        float facing = this->GetCameraFacing();
        position[0] = std::cos(facing) * MIN_DISTANCE + target[0];
        position[1] = std::sin(facing) * MIN_DISTANCE + target[1];
        M2CameraSetVector(this->m_camera, 7, position);
        this->m_cameraDistance = MIN_DISTANCE;
        return;
    }
    M2CameraSetVector(this->m_camera, 7, position);
    this->m_cameraDistance = std::sqrt(squared);
}

// ref: FUN_1424bea10
void CSimpleModel::SetCameraTarget(float* target) {
    if (!(this->m_modelFlags & MODEL_CUSTOM_CAMERA)) {
        FrameScript_ActionError("Not using a custom camera.");
        return;
    }
    if (this->m_camera == nullptr) {
        return;
    }
    float position[3] = {};
    M2CameraGetVector(this->m_camera, 7, position);
    float dx = position[0] - target[0];
    float dy = position[1] - target[1];
    float squared = dy * dy + dx * dx;
    if (squared < NEAR_TARGET) {
        float facing = this->GetCameraFacing();
        target[0] = position[0] - std::cos(facing) * MIN_DISTANCE;
        target[1] = position[1] - std::sin(facing) * MIN_DISTANCE;
        M2CameraSetVector(this->m_camera, 8, target);
        this->m_cameraDistance = MIN_DISTANCE;
        return;
    }
    M2CameraSetVector(this->m_camera, 8, target);
    this->m_cameraDistance = std::sqrt(squared);
}

// ref: FUN_1424be9e0
void CSimpleModel::SetCameraRoll(float radians) {
    if (!(this->m_modelFlags & MODEL_CUSTOM_CAMERA)) {
        FrameScript_ActionError("Not using a custom camera.");
        return;
    }
    if (this->m_camera != nullptr) {
        M2CameraSetFloat(this->m_camera, 5, radians);
    }
}

// ref: FUN_1424bd600
float CSimpleModel::GetDesaturation() {
    if (this->m_model != nullptr && M2ModelGetEffectType(this->m_model) == 4) {
        return M2ModelGetEffectStrength(this->m_model);
    }
    return 0.0f;
}

// ref: FUN_1424bd960
float CSimpleModel::GetShadowEffect() {
    if (this->m_model != nullptr && M2ModelGetEffectType(this->m_model) == 2) {
        return M2ModelGetEffectStrength(this->m_model);
    }
    return 0.0f;
}

// ref: FUN_1424becb0
void CSimpleModel::SetDesaturation(float strength) {
    if (this->m_model == nullptr) {
        return;
    }
    if (strength <= 0.0f) {
        M2ModelClearEffect(this->m_model);
        return;
    }
    M2ModelSetEffect(this->m_model, 4);
    M2ModelSetEffectStrength(this->m_model, 1.0f <= strength ? 1.0f : strength);
}

// ref: FUN_1424bf3b0
void CSimpleModel::SetShadowEffect(float strength) {
    if (this->m_model == nullptr) {
        return;
    }
    if (strength <= 0.0f) {
        M2ModelClearEffect(this->m_model);
        return;
    }
    M2ModelSetEffect(this->m_model, 2);
    M2ModelSetEffectStrength(this->m_model, 1.0f <= strength ? 1.0f : strength);
}

// ref: FUN_1424bd640
uint32_t CSimpleModel::GetDrawFlags() const {
    return s_drawFlagAll ? this->m_drawFlags : this->m_drawFlags & ~1u;
}

// ref: FUN_1424bd820
float CSimpleModel::GetModelAlpha() {
    return this->m_model == nullptr ? 1.0f : M2ModelGetAlpha(this->m_model);
}

// ref: FUN_1424bf130
void CSimpleModel::SetModelAlpha(float alpha) {
    if (this->m_model != nullptr) {
        M2ModelSetAlpha(this->m_model, alpha);
    }
}

// ref: FUN_1424bd840
int32_t CSimpleModel::GetModelFileId() {
    return this->m_model == nullptr ? 0 : M2ModelGetFileDataId(this->m_model);
}

// ref: FUN_1424bd870
bool CSimpleModel::GetPaused() {
    return this->m_model != nullptr && M2ModelHasPauseRequest(this->m_model) && M2ModelIsAnimationPaused(this->m_model);
}

// ref: FUN_1424bf270
void CSimpleModel::SetPaused(bool paused) {
    if (this->m_model != nullptr) {
        M2ModelSetPausedA(this->m_model, paused);
        M2ModelSetPausedB(this->m_model, paused);
    }
}

// ref: FUN_1424bf230
void CSimpleModel::SetParticlesEnabled(bool enabled) {
    if (this->m_model != nullptr) {
        M2ModelSetParticlesA(this->m_model, enabled);
        M2ModelSetParticlesB(this->m_model, enabled);
    }
}

// ref: FUN_1424bda00
float CSimpleModel::GetWorldScale() {
    return this->m_model == nullptr ? 1.0f : *M2ModelGetLocalMatrix(this->m_model);
}

// ref: FUN_1424bda30
bool CSimpleModel::HasAttachmentPoints() {
    return this->m_model != nullptr && M2ModelHasAttachments(this->m_model);
}

// ref: FUN_1424bed10
void CSimpleModel::SetModelDrawLayer(uint32_t layer) {
    if ((this->m_drawLayer & 0xf) != layer) {
        this->m_drawLayer = ((this->m_drawLayer ^ layer) & 0xf) ^ this->m_drawLayer;
        this->MarkBatchDirty();
    }
}

// The alpha byte follows its presence byte (an optional byte, FUN_1424bed40's third argument).
// ref: FUN_1424bed40
void CSimpleModel::SetFogColor(uint32_t rgb, const uint8_t* alpha) {
    uint8_t a = alpha[1] == 0 ? 0xff : alpha[0];
    this->m_fog.SetColor((rgb & 0x00ffffff) | static_cast<uint32_t>(a) << 24);
    this->m_modelFlags |= MODEL_FOG_COLOR;
}

// ref: FUN_1424bd6c0
void CSimpleModel::GetLight(bool* enabled, void* out) {
    auto light = static_cast<SCRIPTLIGHT*>(out);
    *enabled = this->m_lighting.m_enabled != 0;
    bool point = this->m_lighting.m_type == CM2Lighting::LIGHT_POINT;
    light->m_omnidirectional = point;
    const float* vector = point ? this->m_lighting.m_position : this->m_lighting.m_direction;
    light->m_point[0] = vector[0];
    light->m_point[1] = vector[1];
    light->m_point[2] = vector[2];
    const float* ambient = this->m_lighting.m_ambient[0];
    if (2.3841858e-07f <= std::fabs(ambient[0] * ambient[0] + ambient[1] * ambient[1] + ambient[2] * ambient[2])) {
        light->m_ambientIntensity = 1.0f;
        light->m_hasAmbient = 0;
        light->m_ambientColor = ColorFromVector(ambient);
        light->m_hasAmbient = 1;
    }
    float diffuse[3];
    this->m_lighting.GetDiffuse(diffuse);
    if (2.3841858e-07f <= std::fabs(diffuse[0] * diffuse[0] + diffuse[1] * diffuse[1] + diffuse[2] * diffuse[2])) {
        light->m_diffuseIntensity = 1.0f;
        light->m_diffuseColor = ColorFromVector(diffuse);
        light->m_hasDiffuse = 1;
    }
}

// ref: FUN_1424bed90
void CSimpleModel::SetLight(bool enabled, const void* in) {
    auto light = static_cast<const SCRIPTLIGHT*>(in);
    CM2Lighting lighting;
    lighting.SetEnabled(enabled ? 1 : 0);
    if (lighting.m_enabled != 0) {
        lighting.SetLightType(light->m_omnidirectional != 0 ? CM2Lighting::LIGHT_POINT : CM2Lighting::LIGHT_DIRECTIONAL);
        if (lighting.m_type == CM2Lighting::LIGHT_POINT) {
            lighting.SetPosition(light->m_point);
        } else {
            lighting.SetDirection(light->m_point);
        }
        float ambient[3];
        ScaledColor(light->m_ambientColor, light->m_hasAmbient != 0, light->m_ambientIntensity, ambient);
        for (int32_t i = 0; i < 3; i++) {
            lighting.m_ambient[i][0] = ambient[0];
            lighting.m_ambient[i][1] = ambient[1];
            lighting.m_ambient[i][2] = ambient[2];
        }
        ScaledColor(light->m_diffuseColor, light->m_hasDiffuse != 0, light->m_diffuseIntensity, lighting.m_diffuse);
    }
    std::memcpy(static_cast<void*>(&this->m_lighting), static_cast<const void*>(&lighting), sizeof(CM2Lighting));
    lighting.m_link = nullptr;
    lighting.m_next = nullptr;
}

// ref: FUN_1424bf2b0
void CSimpleModel::SetSequence(uint32_t sequence) {
    if (0x6eb < sequence) {
        FrameScript_ActionError("Sequence exceeds valid range of 0 - %d", 0x6ec);
        return;
    }
    if (this->m_model != nullptr) {
        M2ModelSetSequence(this->m_model, -1, sequence, -1, 0, 1.0f, 1, 0, 5, 0, 0, 1.0f);
    }
}

// ref: FUN_1424bf330
bool CSimpleModel::SetSequenceTime(uint32_t sequence, int32_t timeOffset) {
    if (0x6eb < sequence) {
        FrameScript_ActionError("Sequence exceeds valid range of 0 - %d", 0x6ec);
        return false;
    }
    if (this->m_model != nullptr) {
        M2ModelSetSequence(this->m_model, -1, sequence, -1, timeOffset, 1.0f, 1, 0, 5, 0, 0, 1.0f);
    }
    return true;
}

// The matrix starts as the identity with the translation in its fourth row (DAT_14261a9d0..
// DAT_14261aa1c), turned about X, Y and Z, scaled; the translation is filled in last.
// ref: FUN_1424beb50
void CSimpleModel::SetTransform(const void* translation, const void* rotation, const void* scaleArg) {
    auto move = static_cast<const OPTIONALVECTOR*>(translation);
    auto turn = static_cast<const OPTIONALVECTOR*>(rotation);
    auto scale = static_cast<const OPTIONALFLOAT*>(scaleArg);
    float t[3] = { 0.0f, 0.0f, 0.0f };
    if (move->m_present) {
        t[0] = move->m_value[0];
        t[1] = move->m_value[1];
        t[2] = move->m_value[2];
    }
    float r[3] = { 0.0f, 0.0f, 0.0f };
    if (turn->m_present) {
        r[0] = turn->m_value[0];
        r[1] = turn->m_value[1];
        r[2] = turn->m_value[2];
    }
    float s = scale->m_present ? scale->m_value : 0.0f;
    float matrix[16];
    MatrixIdentity(matrix);
    MatrixRotateX(matrix, r[0]);
    MatrixRotateZ(matrix, r[2]);
    MatrixRotateY(matrix, r[1]);
    MatrixScaleUniform(matrix, s);
    if (this->m_model != nullptr) {
        this->m_modelFlags |= MODEL_TRANSFORM;
        matrix[12] = t[0];
        matrix[13] = t[1];
        matrix[14] = t[2];
        M2ModelSetWorldTransform(this->m_model, matrix);
    }
}

// ref: FUN_1424bf430
float* CSimpleModel::TransformCameraSpaceToModelSpace(float* out, const float* position) {
    float units = g_coordinateSpace.m_diagonalHeight * UNITS_PER_HEIGHT;
    float scale = this->m_layoutScale;
    float model = this->GetModelScale();
    out[0] = model * position[0] * scale * units;
    out[1] = scale * units * model * position[1];
    out[2] = scale * units * position[2] * model;
    return out;
}

// ref: FUN_1424be330
void CSimpleModel::ReplaceIconTexture(const int32_t* fileDataId) {
    this->ReplaceTexture(0xe, *fileDataId);
}

// ref: FUN_1424be340
void CSimpleModel::ReplaceTexture(int32_t slot, int32_t fileDataId) {
    if (this->m_model == nullptr) {
        return;
    }
    CStatus status;
    HTEXTURE texture = TextureCreate(fileDataId, 0x82003, &status, 2, 1, 0);
    SysMessageReportStatus(&status);
    M2ModelReplaceTexture(this->m_model, slot, texture);
    TextureFree(&texture);
}

// ref: FUN_1424bf220
void CSimpleModel::SetOverrideCamera(CM2Camera* camera) {
    this->m_overrideCamera = camera;
}
