#include "ui/CSimpleModelFFX.hpp"

#include <cstring>
#include <new>

#include "base/DataAllocator.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "model2/M2Api.hpp"
#include "sound/SE3Kit.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CRenderBatch.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/GxWindow.hpp"
#include "client/RenderSettings.hpp"
#include "client/Resample.hpp"
#include "gx/Gx.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldFrame.hpp"
#include "world/WorldScene.hpp"
#include <cmath>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

int32_t CSimpleModelFFX::s_metaTable = -2;
int32_t CSimpleModelFFX::s_restrictedMetaTable = -2;

namespace {

// The ModelFFX frames' pool (0x143122788).
CDataAllocator s_modelFfxHeap(0x13d8, 5);  // DAT_143122788

// A model event, as CM2Model::RunCallback hands it on: the model, the key bone, the sequence,
// the event's id and data, where it happened, how late, and the closure. '$DSO' plays the sound
// kit its data names, at the event's position, one-shot (FUN_1412d25a0, FUN_1412ce0f0).
// ref: FUN_1416973f0
void ModelEvent(CM2Model* model, uint32_t keyBone, uint32_t sequence, uint32_t event, uint32_t data,
    const float* position, uint32_t delay, const void* closure) {
    (void)model;
    (void)keyBone;
    (void)sequence;
    (void)delay;
    (void)closure;
    if (event != 0x4f534424) {
        return;
    }
    SE3PLAYPARAMS params;
    SE3PlayParamsConstruct(&params);
    params.m_3dMode = 2;
    SE3PlayKit(data, position, &params, nullptr, nullptr, false, nullptr);
    SE3PlayParamsDestroy(&params);
}

// The full-screen effects (Engine/Source/FFX): their slots (DAT_142e97120), the glow pass
// (DAT_142e97788), the anti-aliasing mode (DAT_142e97028) and the other passes' switches. The
// FFX module is not ported, so every switch reads its initial 0.
uint8_t s_ffxGlow;                  // DAT_142e97788
uint8_t s_ffxAntiAliasing;          // DAT_142e97028
uint8_t s_ffxPassSlot;              // DAT_142e96fd0
void* s_ffxSlots[8];                // DAT_142e97120
uint8_t s_ffxDeathEffect;           // DAT_1447aec40
uint8_t s_ffxNetherEffect;          // DAT_142e9769b
// DAT_142e96fc8: the targets sharing the scene's depth, while an FFX frame draws.
GXRENDERTARGETS* s_ffxSharedTargets;

// ref: FUN_1405dd5d0
void FfxEffectBegin(uint8_t slot, uint32_t pass, const uint32_t* config) {
    (void)pass;
    (void)config;
    if (slot != 0) {
        FROST_UNIMPLEMENTED();  // the slot's effect (vtable +0x28)
    }
}

// ref: FUN_1405e7e00
uint8_t FfxGlowEnabled() {
    return s_ffxGlow;
}

// ref: FUN_1405dc960
bool FfxAntiAliasingEnabled() {
    return s_ffxAntiAliasing != 0;
}

// CMAA (and CMAA2) over the buffer's colour. Not ported.
// ref: FUN_1405dd540
void FfxAntiAlias(FRAMEBUFFER* buffer) {
    (void)buffer;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405dcf00
bool FfxPassActive() {
    return s_ffxSlots[s_ffxPassSlot] != nullptr;
}

// ref: FUN_1424a59d0
uint8_t FfxDeathEffectEnabled() {
    return s_ffxDeathEffect;
}

// ref: FUN_1405e4e70
uint8_t FfxNetherEffectEnabled() {
    return s_ffxNetherEffect;
}

// The passes run on the frame's colour through a borrowed target (FUN_141696f50 the glow,
// FUN_141696fd0 the slot's pass, FUN_141697050 the death effect, FUN_1413c1110 the nether
// effect). Their bodies (FUN_1405e8060, FUN_1405dca30, FUN_1424a57e0, FUN_1405e4e80) are not
// ported.
// ref: FUN_141696f50
void FfxGlowPass(void* param, GxRenderContext* context, void* source, void* dest) {
    (void)param;
    (void)context;
    (void)source;
    (void)dest;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141696fd0
void FfxSlotPass(void* param, GxRenderContext* context, void* source, void* dest) {
    (void)param;
    (void)context;
    (void)source;
    (void)dest;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141697050
void FfxDeathPass(void* param, GxRenderContext* context, void* source, void* dest) {
    (void)param;
    (void)context;
    (void)source;
    (void)dest;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413c1110
void FfxNetherPass(void* param, GxRenderContext* context, void* source, void* dest) {
    (void)param;
    (void)context;
    (void)source;
    (void)dest;
    FROST_UNIMPLEMENTED();
}

// The character screens' camera (UIUtil/CameraManager.cpp, DAT_14323f220) stepped from the
// frame's camera and made the frame's override camera (DAT_143122530). Not ported: the frame
// draws through its own camera.
// ref: FUN_141696200
void ModelFfxSyncCamera(CSimpleModelFFX* frame) {
    (void)frame;
    FROST_UNIMPLEMENTED();
}

// The scene drawn through a buffer of the scaled window size (cleared to opaque black), with
// targets sharing its depth for frames that draw their first flag, through the frame's camera
// into its rectangle; then resolved or moved, given the glow and anti-aliasing, resampled into
// the target as the settings ask, and given the remaining effect passes.
// ref: FUN_141697e50
void RenderModelFFX(void* param, const float* view, const float* projection, void* target) {
    (void)view;
    (void)projection;
    auto frame = static_cast<CSimpleModelFFX*>(param);
    uint32_t glow[3] = {0, static_cast<uint32_t>(std::nearbyint(frame->m_glow * 255.0f)) & 0xff, 0};
    FfxEffectBegin(1, 3, glow);
    GXSIZE resolution;
    WindowGetResolution(&resolution);
    int32_t windowSize[2] = {resolution.m_width, resolution.m_height};
    int32_t size[2];
    RenderSettingsScaledSize(size, windowSize);
    GxRenderContext* context = g_gxRenderContext;
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
    context->Clear(sceneBuffer.m_targets.m_count != 0 ? &sceneBuffer.m_targets : nullptr, 0x3f, 0xff000000, 0.0f, 0, 0);
    FRAMEBUFFER sharedBuffer;
    FrameBufferInit(&sharedBuffer);
    GxRenderTargetsClear(&sharedBuffer.m_targets);
    if ((frame->GetDrawFlags() & 1) != 0 && CSimpleModel::s_drawFlagAll) {
        FrameBufferSetupSharedDepth(&sharedBuffer, size,
            sceneBuffer.m_targets.m_count != 0 ? &sceneBuffer.m_targets : nullptr);
        FrameBufferBind(&sharedBuffer);
        s_ffxSharedTargets = sharedBuffer.m_targets.m_count != 0 ? &sharedBuffer.m_targets : nullptr;
    }
    if (frame->m_model != nullptr && frame->m_camera != nullptr) {
        ModelFfxSyncCamera(frame);
        float scissor[4] = {};
        context->GetScissor(scissor);
        frame->UpdateModelTransform();
        MODELFRAMEVIEW block;
        std::memset(&block, 0, sizeof(block));
        block.m_fog = frame->m_fog;
        if ((frame->m_modelFlags & CSimpleModel::MODEL_FOG_COLOR) == 0) {
            block.m_fog.m_words[1] = 0x4b189680;    // 1e7
            block.m_fog.m_words[2] = 0x4cbebc20;    // 1e8
            block.m_fog.m_words[4] = 0;
        }
        std::memcpy(block.m_rect, &frame->m_rect, sizeof(block.m_rect));
        block.m_hasTransform = (frame->m_modelFlags & CSimpleModel::MODEL_TRANSFORM) != 0 ? 1 : 0;
        block.m_translation[0] = frame->m_viewTranslation[0];
        block.m_translation[1] = frame->m_viewTranslation[1];
        std::memcpy(block.m_insets, frame->m_viewInsets, sizeof(block.m_insets));
        uint32_t flags = frame->GetDrawFlags();
        float elapsed = frame->m_elapsed;
        CM2Camera* camera = frame->m_overrideCamera != nullptr ? frame->m_overrideCamera : frame->m_camera;
        uint8_t cameraView[0x64];
        void* cameraBlock = WorldSceneViewFromCamera(cameraView, camera);
        CWorldScene* scene = frame->GetScene();
        uint32_t* params = WorldSceneGetParams(scene);
        WorldSceneRenderView(frame->GetScene(), params, static_cast<const float*>(cameraBlock), &block, &sceneBuffer,
            &sharedBuffer, elapsed, flags);
        context->SetScissor(scissor);
        frame->SetOverrideCamera(nullptr);
    }
    FRAMEBUFFER resolved;
    FrameBufferInit(&resolved);
    if (samples != 0) {
        FrameBufferResolve(context, &sceneBuffer, &resolved);
    } else {
        FrameBufferMove(&resolved, &sceneBuffer);
    }
    if (FfxGlowEnabled()) {
        FRAMEBUFFERFLIP flip = {context, &resolved, nullptr, 0};
        FrameBufferExecuteFlipDiscard(&flip, FfxGlowPass, nullptr);
    }
    if (FfxAntiAliasingEnabled()) {
        FfxAntiAlias(&resolved);
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
    CWorldScene* scene = frame->GetScene();
    float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};      // DAT_1426c8890
    WorldSceneSetRect(full, scene);
    void* environment = WorldSceneGetEnv(scene);
    auto destination = static_cast<FRAMEBUFFER*>(target);
    if (destination == nullptr) {
        destination = WindowGetFrameBuffer();
    }
    if (destination->m_bound == 0) {
        return;
    }
    RESAMPLEOPTIONS options;
    RenderSettingsResampleOptions(&options);
    // The reference reads the destination's colour texture at a null +0x48 when it has none.
    ResampleToTarget(context, environment, resolved.m_targets.m_texs[1], destination->m_targets.m_texs[1], &options,
        false);
    FrameBufferRelease(&resolved);
    if (FfxPassActive()) {
        FRAMEBUFFERFLIP flip = {context, destination, nullptr, 1};
        FrameBufferExecuteFlipDiscard(&flip, FfxSlotPass, &environment);
    }
    if (FfxDeathEffectEnabled()) {
        FRAMEBUFFERFLIP flip = {context, destination, nullptr, 1};
        FrameBufferExecuteFlipDiscard(&flip, FfxDeathPass, &environment);
    }
    if (FfxNetherEffectEnabled()) {
        FRAMEBUFFERFLIP flip = {context, destination, nullptr, 1};
        FrameBufferExecuteFlipDiscard(&flip, FfxNetherPass, nullptr);
    }
    context->SetRenderTargets(destination->m_targets.m_count != 0 ? &destination->m_targets : nullptr);
    if (sharedBuffer.m_targets.m_count != 0) {
        FrameBufferRelease(&sharedBuffer);
    }
}

void InitBank(MODELFFXLIGHTS* bank, void* owner) {
    bank->m_count = 0;
    bank->m_pad004 = 0;
    new (&bank->m_lights[0]) CM2Lighting();
    new (&bank->m_lights[1]) CM2Lighting();
    new (&bank->m_lights[2]) CM2Lighting();
    new (&bank->m_lights[3]) CM2Lighting();
    bank->m_dirty = 0;
    std::memset(bank->m_pad269, 0, sizeof(bank->m_pad269));
    bank->m_owner = owner;
    bank->m_278 = 0;
}

void DestroyBank(MODELFFXLIGHTS* bank) {
    for (CM2Lighting& light : bank->m_lights) {
        light.~CM2Lighting();
    }
}

}

// ---- construction ------------------------------------------------------------------------------

// ref: FUN_141697530
CSimpleModelFFX::CSimpleModelFFX(CSimpleFrame* parent) : CSimpleModel(parent) {
    for (int32_t i = 0; i < 2; i++) {
        InitBank(&this->m_sceneLights[i], this);
        InitBank(&this->m_characterLights[i], this);
        InitBank(&this->m_petLights[i], this);
    }
    this->m_glow = 0.3f;
}

// ref: FUN_1416970f0
CSimpleModelFFX::~CSimpleModelFFX() {
    for (int32_t i = 1; i >= 0; i--) {
        DestroyBank(&this->m_petLights[i]);
        DestroyBank(&this->m_characterLights[i]);
        DestroyBank(&this->m_sceneLights[i]);
    }
}

void* CSimpleModelFFX::operator new(size_t size) {
    (void)size;
    return s_modelFfxHeap.GetData(false, ".?AVCSimpleModelFFX@@", -2, 0x1f);
}

void CSimpleModelFFX::operator delete(void* frame) {
    if (frame != nullptr) {
        s_modelFfxHeap.PutData(frame);
    }
}

// DAT_14309fee4
int32_t CSimpleModelFFX::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_141697780
void CSimpleModelFFX::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleModelFFX_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleModelFFX_FillScriptMethods, true);
}

// ---- FrameScript_Object ------------------------------------------------------------------------

// ref: FUN_141697800
int32_t CSimpleModelFFX::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_141697ce0
bool CSimpleModelFFX::IsObjectTypeName(const char* name) {
    return _stricmp(name, "ModelFFX") == 0 || this->CSimpleModel::IsObjectTypeName(name);
}

// ref: FUN_141697aa0
bool CSimpleModelFFX::IsObjectType(int32_t type) {
    return type == CSimpleModelFFX::GetObjectType() || this->CSimpleModel::IsObjectType(type);
}

// ref: FUN_1416977f0
const char* CSimpleModelFFX::GetObjectTypeName() {
    return "ModelFFX";
}

// ref: FUN_1416977d0
uint32_t CSimpleModelFFX::GetObjectSize() {
    return sizeof(CSimpleModelFFX);
}

// ---- CSimpleFrame ------------------------------------------------------------------------------

// ref: FUN_141697d70
void CSimpleModelFFX::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleModel::LoadXML(node, status, intrinsic);
    if ((node->m_flags & 0x1000000) && (static_cast<XMLMODELEX*>(node)->m_modelExFlags & 1)) {
        this->SetGlow(static_cast<XMLMODELEX*>(node)->m_glow);
    }
}

// The glue's full-screen effects are switched on with the frame (FUN_1405dd650 with
// FUN_14146edb0's setting).
// ref: FUN_141697e20
void CSimpleModelFFX::QueueOnShow() {
    FROST_UNIMPLEMENTED();  // FUN_1405dd650(FUN_14146edb0()): the FFX enable
    this->CSimpleFrame::QueueOnShow();
}

// Every ModelFFX draws on top of its strata, behind its regions.
// ref: FUN_141697dc0
void CSimpleModelFFX::AddRegionsToBatch(CRenderBatch* batch) {
    this->CSimpleFrame::AddRegionsToBatch(batch);
    uint64_t drawOrder = this->GetDrawOrderKey(2, 0);
    batch->AddCallback(this, drawOrder, RenderModelFFX, this, true);
}

// ref: FUN_141698820
void CSimpleModelFFX::SetModel(CM2Model* model) {
    this->CSimpleModel::SetModel(model);
    if (this->m_model != nullptr) {
        void* closure[2] = { this, nullptr };
        M2ModelSetEventCallback(this->m_model,
            reinterpret_cast<void (*)(void* param, uint32_t event, void* data)>(&ModelEvent), closure);
    }
}

// ref: FUN_141698810
void CSimpleModelFFX::SetGlow(float glow) {
    this->m_glow = glow;
}

// ---- the lights --------------------------------------------------------------------------------

// ref: FUN_141697290
void CSimpleModelFFX::AddLight(MODELFFXLIGHTS* set, const uint32_t* index, const void* light) {
    // `index` is {value, present}; anything but 0 picks the second bank.
    auto bytes = reinterpret_cast<const uint8_t*>(index);
    uint32_t value = bytes[4] != 0 ? index[0] : 0;
    if (2 <= static_cast<uint32_t>(value != 0)) {
        FrameScript_ActionError("Invalid light index");
        return;
    }
    CM2Lighting lighting;
    lighting.SetEnabled(1);
    LightingFromScript(&lighting, light);
    MODELFFXLIGHTS* bank = &set[value != 0 ? 1 : 0];
    if (bank->m_count < 4) {
        std::memcpy(static_cast<void*>(&bank->m_lights[bank->m_count]), static_cast<const void*>(&lighting),
            sizeof(CM2Lighting));
        bank->m_count++;
        bank->m_dirty = 1;
    }
    lighting.m_link = nullptr;
    lighting.m_next = nullptr;
}

// ref: FUN_141697230
void CSimpleModelFFX::AddSceneLight(const uint32_t* index, const void* light) {
    this->AddLight(this->m_sceneLights, index, light);
}

// ref: FUN_141697260
void CSimpleModelFFX::AddCharacterLight(const uint32_t* index, const void* light) {
    this->AddLight(this->m_characterLights, index, light);
}

// ref: FUN_1416973c0
void CSimpleModelFFX::AddPetLight(const uint32_t* index, const void* light) {
    this->AddLight(this->m_petLights, index, light);
}

void CSimpleModelFFX::ResetLights() {
    for (int32_t i = 0; i < 2; i++) {
        this->m_sceneLights[i].m_count = 0;
        this->m_sceneLights[i].m_dirty = 0;
        this->m_characterLights[i].m_count = 0;
        this->m_characterLights[i].m_dirty = 0;
        this->m_petLights[i].m_count = 0;
        this->m_petLights[i].m_dirty = 0;
    }
}

// ---- script methods ----------------------------------------------------------------------------

namespace {

using namespace ScriptMethod;

// FUN_1402b4a30: anything but none and nil.
bool Present(lua_State* L, int32_t index) {
    return lua_type(L, index) >= 1;
}

void CheckStack(lua_State* L) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    }
}

CSimpleModelFFX* FfxSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleModelFFX*>(Self(L, CSimpleModelFFX::GetObjectType(), index, usage));
}

struct ADDLIGHTARGS {
    CSimpleModelFFX* m_frame;
    uint32_t m_index[2];            // the value, then whether it was given (byte 4)
    SCRIPTLIGHT m_light;
};

}  // namespace

// [index], light: FUN_1408c5ad0 reads the optional index.
// ref: FUN_1407d22e0
void ReadAddLight(lua_State* L, const char* usage, ADDLIGHTARGS* args) {
    int32_t index = 1;
    args->m_frame = FfxSelf(L, &index, usage);
    if (lua_type(L, index) == LUA_TNUMBER) {
        args->m_index[0] = RequiredUInt32(L, &index, usage);
        reinterpret_cast<uint8_t*>(args->m_index)[4] = 1;
    } else if (!Present(L, index)) {
        index++;
    }
    ReadScriptLight(L, index, usage, &args->m_light);
}

namespace {

// ref: FUN_1408e38f0
int32_t AddCharacterLightBody(lua_State* L, void* context) {
    auto args = static_cast<ADDLIGHTARGS*>(context);
    ReadAddLight(L, "Usage: self:AddCharacterLight([index], light)", args);
    args->m_frame->AddCharacterLight(args->m_index, &args->m_light);
    RaisePendingError(L, args->m_frame, "AddCharacterLight");
    return 0;
}

// ref: FUN_140a07eb0
int AddCharacterLight(lua_State* L) {
    ADDLIGHTARGS args = {};
    return ProtectedCall(L, AddCharacterLightBody, &args);
}

// ref: FUN_1408e4640
int32_t AddLightBody(lua_State* L, void* context) {
    auto args = static_cast<ADDLIGHTARGS*>(context);
    ReadAddLight(L, "Usage: self:AddLight([index], light)", args);
    args->m_frame->AddSceneLight(args->m_index, &args->m_light);
    RaisePendingError(L, args->m_frame, "AddLight");
    return 0;
}

// ref: FUN_140a08c00
int AddLight(lua_State* L) {
    ADDLIGHTARGS args = {};
    return ProtectedCall(L, AddLightBody, &args);
}

// ref: FUN_1408e5390
int32_t AddPetLightBody(lua_State* L, void* context) {
    auto args = static_cast<ADDLIGHTARGS*>(context);
    ReadAddLight(L, "Usage: self:AddPetLight([index], light)", args);
    args->m_frame->AddPetLight(args->m_index, &args->m_light);
    RaisePendingError(L, args->m_frame, "AddPetLight");
    return 0;
}

// ref: FUN_140a09950
int AddPetLight(lua_State* L) {
    ADDLIGHTARGS args = {};
    return ProtectedCall(L, AddPetLightBody, &args);
}

// ref: FUN_140a0a6a0
int ResetLights(lua_State* L) {
    int32_t index = 1;
    CSimpleModelFFX* frame = FfxSelf(L, &index, "Usage: self:ResetLights()");
    frame->ResetLights();
    RaisePendingError(L, frame, "ResetLights");
    return 0;
}

// DAT_142cf9410
const luaL_Reg s_modelFfxMethods[] = {
    { "AddCharacterLight", AddCharacterLight },
    { "AddLight", AddLight },
    { "AddPetLight", AddPetLight },
    { "ResetLights", ResetLights },
};

}

// ref: FUN_140a51170
void CSimpleModelFFX_FillScriptMethods(lua_State* L) {
    CSimpleModel_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_modelFfxMethods, 4);
}
