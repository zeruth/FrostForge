#include "ui/CSimpleModelFFX.hpp"

#include <cstring>
#include <new>

#include "base/DataAllocator.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "model2/M2Api.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CRenderBatch.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

int32_t CSimpleModelFFX::s_metaTable = -2;
int32_t CSimpleModelFFX::s_restrictedMetaTable = -2;

namespace {

// The ModelFFX frames' pool (0x143122788).
CDataAllocator s_modelFfxHeap(0x13d8, 5);

// A model event: '$DSO' plays the sound the event names (FUN_1412d25a0, FUN_1412ce0f0).
// ref: FUN_1416973f0
void ModelEvent(void* param, uint32_t event, void* data) {
    (void)param;
    (void)data;
    if (event == 0x4f534424) {
        FROST_UNIMPLEMENTED();  // the sound kit of the event, played (the sound engine)
    }
}

// The full-screen-effect draw: the glow (FUN_1405dd5d0 with the frame's glow as a byte), the
// render targets of the global renderer (DAT_14348b658), the scene drawn through the frame's
// camera (FUN_1424d0a40) when it has a model and a camera, and the effect passes.
// ref: FUN_141697e50
void RenderModelFFX(void* param, const float* view, const float* projection, void* target) {
    (void)view;
    (void)projection;
    (void)target;
    auto frame = static_cast<CSimpleModelFFX*>(param);
    (void)frame;
    FROST_UNIMPLEMENTED();  // the FFX render path (WorldClient2's renderer)
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
        M2ModelSetEventCallback(this->m_model, ModelEvent, closure);
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

// [index], light: FUN_1408c5ad0 reads the optional index.
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
