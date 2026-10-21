// The Model frame's script methods (0x142cf9020, 0x3f of them).

#include "ui/CSimpleModel.hpp"

#include <cstring>

#include "cas/CAS.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/UIScriptArgs.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

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


struct OPTIONALVECTOR {
    float m_value[3];
    uint8_t m_present;
};

struct OPTIONALFLOAT {
    float m_value;
    uint8_t m_present;
};

CSimpleModel* ModelSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleModel*>(Self(L, CSimpleModel::GetObjectType(), index, usage));
}

// A table made from a named mixin, with room for `fields` (FUN_1407d0a90).
int32_t PushMixin(lua_State* L, const char* mixin, int32_t fields) {
    lua_createtable(L, 0, fields);
    lua_getfield(L, LUA_GLOBALSINDEX, mixin);
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, -5);
        }
    }
    lua_settop(L, -2);
    return lua_gettop(L);
}

// A Vector3DMixin {x, y, z} (FUN_140d131e0).
int32_t PushVector3D(lua_State* L, const float* v) {
    CheckStack(L);
    int32_t table = PushMixin(L, "Vector3DMixin", 3);
    lua_pushnumber(L, v[0]);
    lua_setfield(L, table, "x");
    lua_pushnumber(L, v[1]);
    lua_setfield(L, table, "y");
    lua_pushnumber(L, v[2]);
    lua_setfield(L, table, "z");
    return 1;
}

// A ColorMixin {r, g, b} or nil (FUN_140c59200).
int32_t PushOptionalColor(lua_State* L, uint32_t color, bool present) {
    CheckStack(L);
    if (!present) {
        lua_pushnil(L);
        return 1;
    }
    int32_t table = PushMixin(L, "ColorMixin", 3);
    lua_pushnumber(L, static_cast<float>((color >> 16) & 0xff) / 255.0f);
    lua_setfield(L, table, "r");
    lua_pushnumber(L, static_cast<float>((color >> 8) & 0xff) / 255.0f);
    lua_setfield(L, table, "g");
    lua_pushnumber(L, static_cast<float>(color & 0xff) / 255.0f);
    lua_setfield(L, table, "b");
    return 1;
}

// Whether a table's field is a finite number (FUN_1407cad50).
bool HasNumberField(lua_State* L, int32_t index, const char* key) {
    lua_getfield(L, index, key);
    bool ok = lua_isnumber(L, -1) != 0;
    if (ok) {
        double value = lua_tonumber(L, -1);
        ok = value == value && -3.4028234663852886e38 <= value && value <= 3.4028234663852886e38;
    }
    lua_settop(L, -2);
    return ok;
}

float NumberField(lua_State* L, int32_t index, const char* key) {
    lua_getfield(L, index, key);
    float value = static_cast<float>(lua_tonumber(L, -1));
    lua_settop(L, -2);
    return value;
}

// Whether a value is a {x, y, z} table (FUN_140a50d90), and the vector (FUN_140a509c0).
bool IsVector3D(lua_State* L, int32_t index) {
    return lua_type(L, index) == LUA_TTABLE && HasNumberField(L, index, "x") && HasNumberField(L, index, "y")
        && HasNumberField(L, index, "z");
}

// ref: FUN_140a509c0
void ReadVector3D(lua_State* L, int32_t index, float* out) {
    out[0] = NumberField(L, index, "x");
    out[1] = NumberField(L, index, "y");
    out[2] = NumberField(L, index, "z");
}

// Three numbers (FUN_1408c6aa0).
void RequiredXYZ(lua_State* L, int32_t* index, const char* usage, float* out) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
        return;
    }
    int32_t i = *index;
    if (!lua_isnumber(L, i) || !lua_isnumber(L, i + 1) || !lua_isnumber(L, i + 2)) {
        luaL_argerror(L, i, usage);
        return;
    }
    out[0] = static_cast<float>(lua_tonumber(L, i));
    out[1] = static_cast<float>(lua_tonumber(L, i + 1));
    out[2] = static_cast<float>(lua_tonumber(L, i + 2));
    *index += 3;
}

// A file by FileDataID or by path; a path with no file is tried again as a .blp
// (FUN_1408c68a0 over FUN_1406380d0 / FUN_140638160).
int32_t RequiredFileDataId(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
        return 0;
    }
    int32_t i = *index;
    int32_t id = 0;
    if (lua_type(L, i) == LUA_TNUMBER) {
        id = static_cast<int32_t>(lua_tonumber(L, i));
    } else if (lua_type(L, i) == LUA_TSTRING) {
        const char* path = lua_tolstring(L, i, nullptr);
        if (path != nullptr) {
            id = cas::CASFileGetIdByName(path, 0);
            if (id == 0) {
                // FUN_1402622c0: the path with ".blp" appended
                char withExtension[0x30c];
                std::strncpy(withExtension, path, sizeof(withExtension) - 5);
                withExtension[sizeof(withExtension) - 5] = '\0';
                std::strcat(withExtension, ".blp");
                id = cas::CASFileGetIdByName(withExtension, 0);
            }
        }
    } else {
        luaL_argerror(L, i, usage);
        return 0;
    }
    CheckStack(L);
    (*index)++;
    return id;
}

// A ColorMixin into a packed colour (FUN_140c589d0); absent leaves it unset.
void OptionalColorTable(lua_State* L, int32_t index, const char* usage, uint32_t* color, uint8_t* present) {
    if (!Present(L, index)) {
        return;
    }
    if (!(lua_type(L, index) == LUA_TTABLE && HasNumberField(L, index, "r") && HasNumberField(L, index, "g")
            && HasNumberField(L, index, "b"))) {
        luaL_argerror(L, index, usage);
        return;
    }
    auto channel = [](float value) {
        if (!(0.0f <= value)) {
            value = 0.0f;
        } else if (1.0f <= value) {
            value = 1.0f;
        }
        return static_cast<uint32_t>(static_cast<int64_t>(value * 255.0f + 0.5f)) & 0xff;
    };
    uint32_t b = channel(NumberField(L, index, "b"));
    uint32_t g = channel(NumberField(L, index, "g"));
    uint32_t r = channel(NumberField(L, index, "r"));
    *color = 0xff000000u | r << 16 | g << 8 | b;
    *present = 1;
}

// A light table (FUN_140d46ba0).
void ReadLight(lua_State* L, int32_t index, const char* usage, SCRIPTLIGHT* light) {
    CheckStack(L);
    lua_getfield(L, index, "omnidirectional");
    if (Present(L, -1)) {
        light->m_omnidirectional = lua_toboolean(L, -1) != 0;
    }
    lua_settop(L, -2);
    lua_getfield(L, index, "point");
    int32_t top = lua_gettop(L);
    if (!Present(L, top)) {
        luaL_error(L, usage);
    } else if (!IsVector3D(L, top)) {
        luaL_argerror(L, top, usage);
    } else {
        ReadVector3D(L, top, light->m_point);
    }
    lua_settop(L, -2);
    lua_getfield(L, index, "ambientIntensity");
    if (Present(L, -1)) {
        light->m_ambientIntensity = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_settop(L, -2);
    lua_getfield(L, index, "ambientColor");
    OptionalColorTable(L, lua_gettop(L), usage, &light->m_ambientColor, &light->m_hasAmbient);
    lua_settop(L, -2);
    lua_getfield(L, index, "diffuseIntensity");
    if (Present(L, -1)) {
        light->m_diffuseIntensity = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_settop(L, -2);
    lua_getfield(L, index, "diffuseColor");
    OptionalColorTable(L, lua_gettop(L), usage, &light->m_diffuseColor, &light->m_hasDiffuse);
    lua_settop(L, -2);
}

// A light as a table (FUN_140d46a30).
int32_t PushLight(lua_State* L, const SCRIPTLIGHT* light) {
    lua_createtable(L, 0, 6);
    int32_t table = lua_gettop(L);
    lua_pushboolean(L, light->m_omnidirectional);
    lua_setfield(L, table, "omnidirectional");
    PushVector3D(L, light->m_point);
    lua_setfield(L, table, "point");
    lua_pushnumber(L, light->m_ambientIntensity);
    lua_setfield(L, table, "ambientIntensity");
    PushOptionalColor(L, light->m_ambientColor, light->m_hasAmbient != 0);
    lua_setfield(L, table, "ambientColor");
    lua_pushnumber(L, light->m_diffuseIntensity);
    lua_setfield(L, table, "diffuseIntensity");
    PushOptionalColor(L, light->m_diffuseColor, light->m_hasDiffuse != 0);
    lua_setfield(L, table, "diffuseColor");
    return 1;
}

int32_t Push3(lua_State* L, const float* v) {
    PushFloat(L, v[0]);
    PushFloat(L, v[1]);
    PushFloat(L, v[2]);
    return 3;
}

// ---- the methods -------------------------------------------------------------------------------

// ref: FUN_140a0b460
int AdvanceTime(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: self:AdvanceTime()");
    RaisePendingError(L, model, "AdvanceTime");
    return 0;
}

// ref: FUN_140a0c1f0
int ClearFog(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: self:ClearFog()");
    model->m_modelFlags &= ~CSimpleModel::MODEL_FOG_COLOR;
    RaisePendingError(L, model, "ClearFog");
    return 0;
}

// ref: FUN_140a0cf60
int ClearModel(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: self:ClearModel()");
    model->ClearModel();
    RaisePendingError(L, model, "ClearModel");
    return 0;
}

// ref: FUN_140a0dcf0
int ClearTransform(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: self:ClearTransform()");
    model->m_modelFlags &= ~CSimpleModel::MODEL_TRANSFORM;
    RaisePendingError(L, model, "ClearTransform");
    return 0;
}

// ref: FUN_140a0ea60
int GetCameraDistance(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local distance = self:GetCameraDistance()");
    float distance = model->m_cameraDistance;
    RaisePendingError(L, model, "GetCameraDistance");
    return PushFloat(L, distance);
}

// ref: FUN_140a0f800
int GetCameraFacing(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local radians = self:GetCameraFacing()");
    float facing = model->GetCameraFacing();
    RaisePendingError(L, model, "GetCameraFacing");
    return PushFloat(L, facing);
}

// ref: FUN_140a105a0
int GetCameraPosition(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local x, y, z = self:GetCameraPosition()");
    float v[3];
    model->GetCameraPosition(v);
    RaisePendingError(L, model, "GetCameraPosition");
    return Push3(L, v);
}

// ref: FUN_140a11340
int GetCameraRoll(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local radians = self:GetCameraRoll()");
    float roll = model->GetCameraRoll();
    RaisePendingError(L, model, "GetCameraRoll");
    return PushFloat(L, roll);
}

// ref: FUN_140a120e0
int GetCameraTarget(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local x, y, z = self:GetCameraTarget()");
    float v[3];
    model->GetCameraTarget(v);
    RaisePendingError(L, model, "GetCameraTarget");
    return Push3(L, v);
}

// ref: FUN_140a12e80
int GetDesaturation(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local strength = self:GetDesaturation()");
    float strength = model->GetDesaturation();
    RaisePendingError(L, model, "GetDesaturation");
    return PushFloat(L, strength);
}

// ref: FUN_140a13c20
int GetFacing(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local facing = self:GetFacing()");
    float facing = model->m_facing;
    RaisePendingError(L, model, "GetFacing");
    return PushFloat(L, facing);
}

// ref: FUN_140a149c0
int GetFogColor(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local r, g, b, a = self:GetFogColor()");
    RaisePendingError(L, model, "GetFogColor");
    // FUN_1408c6c60
    return PushColor(L, model->m_fog.GetColor());
}

// ref: FUN_140a15750
int GetFogFar(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local fogFar = self:GetFogFar()");
    float value = model->m_fog.Get(2);
    RaisePendingError(L, model, "GetFogFar");
    return PushFloat(L, value);
}

// ref: FUN_140a164f0
int GetFogNear(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local fogNear = self:GetFogNear()");
    float value = model->m_fog.Get(1);
    RaisePendingError(L, model, "GetFogNear");
    return PushFloat(L, value);
}

struct LIGHTARGS {
    CSimpleModel* m_model;
    uint8_t m_enabled;
    uint8_t m_pad09[3];
    SCRIPTLIGHT m_light;
};

// ref: FUN_1408e60e0
int32_t GetLightBody(lua_State* L, void* context) {
    auto args = static_cast<LIGHTARGS*>(context);
    int32_t index = 1;
    args->m_model = ModelSelf(L, &index, "Usage: local enabled, light = self:GetLight()");
    bool enabled = false;
    args->m_model->GetLight(&enabled, &args->m_light);
    args->m_enabled = enabled;
    RaisePendingError(L, args->m_model, "GetLight");
    PushBool(L, args->m_enabled != 0);
    return 1 + PushLight(L, &args->m_light);
}

// ref: FUN_140a17290
int GetLight(lua_State* L) {
    LIGHTARGS args = {};
    return ProtectedCall(L, GetLightBody, &args);
}

// ref: FUN_140a17fe0
int GetModelAlpha(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local alpha = self:GetModelAlpha()");
    float alpha = model->GetModelAlpha();
    RaisePendingError(L, model, "GetModelAlpha");
    return PushFloat(L, alpha);
}

// ref: FUN_140a18d80
int GetModelDrawLayer(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local layer, sublayer = self:GetModelDrawLayer()");
    uint32_t layer = model->m_drawLayer;
    RaisePendingError(L, model, "GetModelDrawLayer");
    PushStringOrNil(L, DrawLayerToString(layer & 0xf));
    PushFloat(L, 0.0f);
    return 2;
}

// ref: FUN_140a19b90
int GetModelFileID(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local modelFileID = self:GetModelFileID()");
    int32_t id = model->GetModelFileId();
    RaisePendingError(L, model, "GetModelFileID");
    CheckStack(L);
    if (id == 0) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, id);
    }
    return 1;
}

// ref: FUN_140a1a970
int GetModelScale(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local scale = self:GetModelScale()");
    float scale = model->GetModelScale();
    RaisePendingError(L, model, "GetModelScale");
    return PushFloat(L, scale);
}

// ref: FUN_140a1b710
int GetPaused(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local paused = self:GetPaused()");
    bool paused = model->GetPaused();
    RaisePendingError(L, model, "GetPaused");
    return PushBool(L, paused);
}

// ref: FUN_140a1c4b0
int GetPitch(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local pitch = self:GetPitch()");
    float pitch = model->m_pitch;
    RaisePendingError(L, model, "GetPitch");
    return PushFloat(L, pitch);
}

// ref: FUN_140a1d250
int GetPosition(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local x, y, z = self:GetPosition()");
    float v[3] = { model->m_position[0], model->m_position[1], model->m_position[2] };
    RaisePendingError(L, model, "GetPosition");
    return Push3(L, v);
}

// ref: FUN_140a1e010
int GetRoll(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local roll = self:GetRoll()");
    float roll = model->m_roll;
    RaisePendingError(L, model, "GetRoll");
    return PushFloat(L, roll);
}

// ref: FUN_140a1edb0
int GetShadowEffect(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local strength = self:GetShadowEffect()");
    float strength = model->GetShadowEffect();
    RaisePendingError(L, model, "GetShadowEffect");
    return PushFloat(L, strength);
}

// ref: FUN_140a1fb50
int GetViewInsets(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local left, right, top, bottom = self:GetViewInsets()");
    float left = model->m_viewInsets[1];
    float right = model->m_viewInsets[3];
    float top = model->m_viewInsets[0];
    float bottom = model->m_viewInsets[2];
    RaisePendingError(L, model, "GetViewInsets");
    PushUIUnits(L, left);
    PushUIUnits(L, right);
    PushUIUnits(L, top);
    PushUIUnits(L, bottom);
    return 4;
}

// ref: FUN_140a20a80
int GetViewTranslation(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local x, y = self:GetViewTranslation()");
    float x = model->m_viewTranslation[0];
    float y = model->m_viewTranslation[1];
    RaisePendingError(L, model, "GetViewTranslation");
    PushUIUnits(L, x);
    PushUIUnits(L, y);
    return 2;
}

// ref: FUN_140a218e0
int GetWorldScale(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local worldScale = self:GetWorldScale()");
    float scale = model->GetWorldScale();
    RaisePendingError(L, model, "GetWorldScale");
    return PushFloat(L, scale);
}

// ref: FUN_140a22680
int HasAttachmentPoints(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local hasAttachmentPoints = self:HasAttachmentPoints()");
    bool has = model->HasAttachmentPoints();
    RaisePendingError(L, model, "HasAttachmentPoints");
    return PushBool(L, has);
}

// ref: FUN_140a23420
int HasCustomCamera(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local hasCustomCamera = self:HasCustomCamera()");
    bool custom = (model->m_modelFlags & CSimpleModel::MODEL_CUSTOM_CAMERA) != 0;
    RaisePendingError(L, model, "HasCustomCamera");
    return PushBool(L, custom);
}

// ref: FUN_140a241c0
int IsUsingModelCenterToTransform(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: local useCenter = self:IsUsingModelCenterToTransform()");
    bool center = (model->m_modelFlags & CSimpleModel::MODEL_USE_CENTER) != 0;
    RaisePendingError(L, model, "IsUsingModelCenterToTransform");
    return PushBool(L, center);
}

// ref: FUN_140a24f60
int MakeCurrentCameraCustom(lua_State* L) {
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, "Usage: self:MakeCurrentCameraCustom()");
    model->MakeCurrentCameraCustom();
    RaisePendingError(L, model, "MakeCurrentCameraCustom");
    return 0;
}

struct ASSETARGS {
    CSimpleModel* m_model;
    int32_t m_fileDataId;
    uint8_t m_noMip;
};

// ref: FUN_1408e6de0
int32_t ReplaceIconTextureBody(lua_State* L, void* context) {
    auto args = static_cast<ASSETARGS*>(context);
    const char* usage = "Usage: self:ReplaceIconTexture(asset)";
    int32_t index = 1;
    args->m_model = ModelSelf(L, &index, usage);
    // FUN_1408c6520
    args->m_fileDataId = RequiredFileDataId(L, &index, usage);
    args->m_model->ReplaceIconTexture(&args->m_fileDataId);
    RaisePendingError(L, args->m_model, "ReplaceIconTexture");
    return 0;
}

// ref: FUN_140a25cf0
int ReplaceIconTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, ReplaceIconTextureBody, &args);
}

// ref: FUN_140a26a10
int SetCamera(lua_State* L) {
    const char* usage = "Usage: self:SetCamera(cameraIndex)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    uint32_t camera = RequiredUInt32(L, &index, usage);
    model->SetCamera(camera);
    RaisePendingError(L, model, "SetCamera");
    return 0;
}

// ref: FUN_140a27810
int SetCameraDistance(lua_State* L) {
    const char* usage = "Usage: self:SetCameraDistance(distance)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float distance = RequiredFinite(L, &index, usage);
    model->SetCameraDistance(distance);
    RaisePendingError(L, model, "SetCameraDistance");
    return 0;
}

// ref: FUN_140a28620
int SetCameraFacing(lua_State* L) {
    const char* usage = "Usage: self:SetCameraFacing(radians)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float radians = RequiredFinite(L, &index, usage);
    model->SetCameraFacing(radians);
    RaisePendingError(L, model, "SetCameraFacing");
    return 0;
}

// ref: FUN_140a29430
int SetCameraPosition(lua_State* L) {
    const char* usage = "Usage: self:SetCameraPosition(position)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float v[3] = {};
    RequiredXYZ(L, &index, usage, v);
    model->SetCameraPosition(v);
    RaisePendingError(L, model, "SetCameraPosition");
    return 0;
}

// ref: FUN_140a2a230
int SetCameraRoll(lua_State* L) {
    const char* usage = "Usage: self:SetCameraRoll(radians)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float radians = RequiredFinite(L, &index, usage);
    model->SetCameraRoll(radians);
    RaisePendingError(L, model, "SetCameraRoll");
    return 0;
}

// ref: FUN_140a2b040
int SetCameraTarget(lua_State* L) {
    const char* usage = "Usage: self:SetCameraTarget(target)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float v[3] = {};
    RequiredXYZ(L, &index, usage, v);
    model->SetCameraTarget(v);
    RaisePendingError(L, model, "SetCameraTarget");
    return 0;
}

// ref: FUN_140a2be40
int SetCustomCamera(lua_State* L) {
    const char* usage = "Usage: self:SetCustomCamera(cameraIndex)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    uint32_t camera = RequiredUInt32(L, &index, usage);
    model->SetCustomCamera(camera);
    RaisePendingError(L, model, "SetCustomCamera");
    return 0;
}

// ref: FUN_140a2cc40
int SetDesaturation(lua_State* L) {
    const char* usage = "Usage: self:SetDesaturation(strength)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float strength = RequiredFinite(L, &index, usage);
    model->SetDesaturation(strength);
    RaisePendingError(L, model, "SetDesaturation");
    return 0;
}

// ref: FUN_140a2da50
int SetFacing(lua_State* L) {
    const char* usage = "Usage: self:SetFacing(facing)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    model->m_facing = RequiredFinite(L, &index, usage);
    RaisePendingError(L, model, "SetFacing");
    return 0;
}

struct FOGCOLORARGS {
    CSimpleModel* m_model;
    uint32_t m_rgb;
    uint8_t m_alpha[2];     // the byte, then whether it was given
};

// ref: FUN_1408e7ab0
int32_t SetFogColorBody(lua_State* L, void* context) {
    auto args = static_cast<FOGCOLORARGS*>(context);
    const char* usage = "Usage: self:SetFogColor(color [, a])";
    int32_t index = 1;
    args->m_model = ModelSelf(L, &index, usage);
    args->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, args->m_alpha);
    args->m_model->SetFogColor(args->m_rgb, args->m_alpha);
    RaisePendingError(L, args->m_model, "SetFogColor");
    return 0;
}

// ref: FUN_140a2e860
int SetFogColor(lua_State* L) {
    FOGCOLORARGS args = {};
    return ProtectedCall(L, SetFogColorBody, &args);
}

// ref: FUN_140a2f590
int SetFogFar(lua_State* L) {
    const char* usage = "Usage: self:SetFogFar(fogFar)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    model->m_fog.Set(2, RequiredFinite(L, &index, usage));
    RaisePendingError(L, model, "SetFogFar");
    return 0;
}

// ref: FUN_140a303a0
int SetFogNear(lua_State* L) {
    const char* usage = "Usage: self:SetFogNear(fogNear)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    model->m_fog.Set(1, RequiredFinite(L, &index, usage));
    RaisePendingError(L, model, "SetFogNear");
    return 0;
}

// ref: FUN_140a311b0
int SetGlow(lua_State* L) {
    const char* usage = "Usage: self:SetGlow(glow)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float glow = RequiredFinite(L, &index, usage);
    model->SetGlow(glow);
    RaisePendingError(L, model, "SetGlow");
    return 0;
}

// ref: FUN_1408e8920
int32_t SetLightBody(lua_State* L, void* context) {
    auto args = static_cast<LIGHTARGS*>(context);
    const char* usage = "Usage: self:SetLight(enabled, light)";
    int32_t index = 1;
    args->m_model = ModelSelf(L, &index, usage);
    args->m_enabled = RequiredBool(L, &index, usage);
    ReadLight(L, index, usage, &args->m_light);
    args->m_model->SetLight(args->m_enabled != 0, &args->m_light);
    RaisePendingError(L, args->m_model, "SetLight");
    return 0;
}

// ref: FUN_140a31fc0
int SetLight(lua_State* L) {
    LIGHTARGS args = {};
    return ProtectedCall(L, SetLightBody, &args);
}

// ref: FUN_1408e9650
int32_t SetModelBody(lua_State* L, void* context) {
    auto args = static_cast<ASSETARGS*>(context);
    const char* usage = "Usage: self:SetModel(asset [, noMip])";
    int32_t index = 1;
    args->m_model = ModelSelf(L, &index, usage);
    args->m_fileDataId = RequiredFileDataId(L, &index, usage);
    if (Present(L, index)) {
        CheckStack(L);
        args->m_noMip = lua_toboolean(L, index) != 0;
    }
    index++;
    args->m_model->SetModelFile(args->m_fileDataId, args->m_noMip != 0);
    RaisePendingError(L, args->m_model, "SetModel");
    return 0;
}

// ref: FUN_140a32d00
int SetModel(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetModelBody, &args);
}

// ref: FUN_140a33a30
int SetModelAlpha(lua_State* L) {
    const char* usage = "Usage: self:SetModelAlpha(alpha)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float alpha = RequiredFinite(L, &index, usage);
    model->SetModelAlpha(alpha);
    RaisePendingError(L, model, "SetModelAlpha");
    return 0;
}

// ref: FUN_140a34840
int SetModelDrawLayer(lua_State* L) {
    const char* usage = "Usage: self:SetModelDrawLayer(layer)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    uint32_t layer = RequiredDrawLayer(L, &index, usage);
    model->SetModelDrawLayer(layer);
    RaisePendingError(L, model, "SetModelDrawLayer");
    return 0;
}

// ref: FUN_140a356b0
int SetModelScale(lua_State* L) {
    const char* usage = "Usage: self:SetModelScale(scale)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    model->m_modelScale = RequiredFinite(L, &index, usage);
    RaisePendingError(L, model, "SetModelScale");
    return 0;
}

// ref: FUN_140a364c0
int SetParticlesEnabled(lua_State* L) {
    const char* usage = "Usage: self:SetParticlesEnabled(enabled)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    bool enabled = RequiredBool(L, &index, usage);
    model->SetParticlesEnabled(enabled);
    RaisePendingError(L, model, "SetParticlesEnabled");
    return 0;
}

// ref: FUN_140a37290
int SetPaused(lua_State* L) {
    const char* usage = "Usage: self:SetPaused(paused)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    bool paused = RequiredBool(L, &index, usage);
    model->SetPaused(paused);
    RaisePendingError(L, model, "SetPaused");
    return 0;
}

// ref: FUN_140a38060
int SetPitch(lua_State* L) {
    const char* usage = "Usage: self:SetPitch(pitch)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    model->m_pitch = RequiredFinite(L, &index, usage);
    RaisePendingError(L, model, "SetPitch");
    return 0;
}

// ref: FUN_140a38e70
int SetPosition(lua_State* L) {
    const char* usage = "Usage: self:SetPosition(position)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float v[3] = {};
    RequiredXYZ(L, &index, usage, v);
    model->m_position[0] = v[0];
    model->m_position[1] = v[1];
    model->m_position[2] = v[2];
    RaisePendingError(L, model, "SetPosition");
    return 0;
}

// ref: FUN_140a39c50
int SetRoll(lua_State* L) {
    const char* usage = "Usage: self:SetRoll(roll)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    model->m_roll = RequiredFinite(L, &index, usage);
    RaisePendingError(L, model, "SetRoll");
    return 0;
}

// ref: FUN_140a3aa60
int SetSequence(lua_State* L) {
    const char* usage = "Usage: self:SetSequence(sequence)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    uint32_t sequence = RequiredUInt32(L, &index, usage);
    model->SetSequence(sequence);
    RaisePendingError(L, model, "SetSequence");
    return 0;
}

// ref: FUN_140a3b860
int SetSequenceTime(lua_State* L) {
    const char* usage = "Usage: self:SetSequenceTime(sequence, timeOffset)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    uint32_t sequence = RequiredUInt32(L, &index, usage);
    int32_t offset = RequiredInt32(L, &index, usage);
    model->SetSequenceTime(sequence, offset);
    RaisePendingError(L, model, "SetSequenceTime");
    return 0;
}

// ref: FUN_140a3c6a0
int SetShadowEffect(lua_State* L) {
    const char* usage = "Usage: self:SetShadowEffect(strength)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float strength = RequiredFinite(L, &index, usage);
    model->SetShadowEffect(strength);
    RaisePendingError(L, model, "SetShadowEffect");
    return 0;
}

struct TRANSFORMARGS {
    CSimpleModel* m_model;
    OPTIONALVECTOR m_translation;
    OPTIONALVECTOR m_rotation;
    OPTIONALFLOAT m_scale;
};

// ref: FUN_1408ea370
int32_t SetTransformBody(lua_State* L, void* context) {
    auto args = static_cast<TRANSFORMARGS*>(context);
    const char* usage = "Usage: self:SetTransform([translation, rotation, scale])";
    int32_t index = 1;
    args->m_model = ModelSelf(L, &index, usage);
    for (OPTIONALVECTOR* vector : { &args->m_translation, &args->m_rotation }) {
        if (Present(L, index)) {
            if (!IsVector3D(L, index)) {
                luaL_argerror(L, index, usage);
            } else {
                CheckStack(L);
                ReadVector3D(L, index, vector->m_value);
                vector->m_present = 1;
            }
        }
        index++;
    }
    if (Present(L, index)) {
        args->m_scale.m_value = RequiredFinite(L, &index, usage);
        args->m_scale.m_present = 1;
    } else {
        index++;
    }
    args->m_model->SetTransform(&args->m_translation, &args->m_rotation, &args->m_scale);
    RaisePendingError(L, args->m_model, "SetTransform");
    return 0;
}

// ref: FUN_140a3d4b0
int SetTransform(lua_State* L) {
    TRANSFORMARGS args = {};
    return ProtectedCall(L, SetTransformBody, &args);
}

// The four numbers are UI units: (FUN_142ccb9fc's aspect compensation) times 1024 times
// the value over the diagonal width (DAT_1426e6324 / DAT_142ccb9fc * DAT_142ccb9f4).
// ref: FUN_140a3e1e0
int SetViewInsets(lua_State* L) {
    const char* usage = "Usage: self:SetViewInsets(left, right, top, bottom)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float left = RequiredUIUnits(L, &index, usage);
    float right = RequiredUIUnits(L, &index, usage);
    float top = RequiredUIUnits(L, &index, usage);
    float bottom = RequiredUIUnits(L, &index, usage);
    model->m_viewInsets[0] = top;
    model->m_viewInsets[1] = left;
    model->m_viewInsets[2] = bottom;
    model->m_viewInsets[3] = right;
    RaisePendingError(L, model, "SetViewInsets");
    return 0;
}

// ref: FUN_140a3f230
int SetViewTranslation(lua_State* L) {
    const char* usage = "Usage: self:SetViewTranslation(x, y)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float x = RequiredUIUnits(L, &index, usage);
    float y = RequiredUIUnits(L, &index, usage);
    model->m_viewTranslation[0] = x;
    model->m_viewTranslation[1] = y;
    RaisePendingError(L, model, "SetViewTranslation");
    return 0;
}

// ref: FUN_140a40140
int TransformCameraSpaceToModelSpace(lua_State* L) {
    const char* usage = "Usage: local modelPosition = self:TransformCameraSpaceToModelSpace(cameraPosition)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    float position[3] = {};
    if (!Present(L, index)) {
        luaL_error(L, usage);
    } else if (!IsVector3D(L, index)) {
        luaL_argerror(L, index, usage);
    } else {
        CheckStack(L);
        ReadVector3D(L, index, position);
    }
    float out[3];
    model->TransformCameraSpaceToModelSpace(out, position);
    RaisePendingError(L, model, "TransformCameraSpaceToModelSpace");
    return PushVector3D(L, out);
}

// ref: FUN_140a41060
int UseModelCenterToTransform(lua_State* L) {
    const char* usage = "Usage: self:UseModelCenterToTransform(useCenter)";
    int32_t index = 1;
    CSimpleModel* model = ModelSelf(L, &index, usage);
    bool center = RequiredBool(L, &index, usage);
    if (center) {
        model->m_modelFlags |= CSimpleModel::MODEL_USE_CENTER;
    } else {
        model->m_modelFlags &= ~CSimpleModel::MODEL_USE_CENTER;
    }
    RaisePendingError(L, model, "UseModelCenterToTransform");
    return 0;
}

// DAT_142cf9020
const luaL_Reg s_modelMethods[] = {
    { "AdvanceTime", AdvanceTime },
    { "ClearFog", ClearFog },
    { "ClearModel", ClearModel },
    { "ClearTransform", ClearTransform },
    { "GetCameraDistance", GetCameraDistance },
    { "GetCameraFacing", GetCameraFacing },
    { "GetCameraPosition", GetCameraPosition },
    { "GetCameraRoll", GetCameraRoll },
    { "GetCameraTarget", GetCameraTarget },
    { "GetDesaturation", GetDesaturation },
    { "GetFacing", GetFacing },
    { "GetFogColor", GetFogColor },
    { "GetFogFar", GetFogFar },
    { "GetFogNear", GetFogNear },
    { "GetLight", GetLight },
    { "GetModelAlpha", GetModelAlpha },
    { "GetModelDrawLayer", GetModelDrawLayer },
    { "GetModelFileID", GetModelFileID },
    { "GetModelScale", GetModelScale },
    { "GetPaused", GetPaused },
    { "GetPitch", GetPitch },
    { "GetPosition", GetPosition },
    { "GetRoll", GetRoll },
    { "GetShadowEffect", GetShadowEffect },
    { "GetViewInsets", GetViewInsets },
    { "GetViewTranslation", GetViewTranslation },
    { "GetWorldScale", GetWorldScale },
    { "HasAttachmentPoints", HasAttachmentPoints },
    { "HasCustomCamera", HasCustomCamera },
    { "IsUsingModelCenterToTransform", IsUsingModelCenterToTransform },
    { "MakeCurrentCameraCustom", MakeCurrentCameraCustom },
    { "ReplaceIconTexture", ReplaceIconTexture },
    { "SetCamera", SetCamera },
    { "SetCameraDistance", SetCameraDistance },
    { "SetCameraFacing", SetCameraFacing },
    { "SetCameraPosition", SetCameraPosition },
    { "SetCameraRoll", SetCameraRoll },
    { "SetCameraTarget", SetCameraTarget },
    { "SetCustomCamera", SetCustomCamera },
    { "SetDesaturation", SetDesaturation },
    { "SetFacing", SetFacing },
    { "SetFogColor", SetFogColor },
    { "SetFogFar", SetFogFar },
    { "SetFogNear", SetFogNear },
    { "SetGlow", SetGlow },
    { "SetLight", SetLight },
    { "SetModel", SetModel },
    { "SetModelAlpha", SetModelAlpha },
    { "SetModelDrawLayer", SetModelDrawLayer },
    { "SetModelScale", SetModelScale },
    { "SetParticlesEnabled", SetParticlesEnabled },
    { "SetPaused", SetPaused },
    { "SetPitch", SetPitch },
    { "SetPosition", SetPosition },
    { "SetRoll", SetRoll },
    { "SetSequence", SetSequence },
    { "SetSequenceTime", SetSequenceTime },
    { "SetShadowEffect", SetShadowEffect },
    { "SetTransform", SetTransform },
    { "SetViewInsets", SetViewInsets },
    { "SetViewTranslation", SetViewTranslation },
    { "TransformCameraSpaceToModelSpace", TransformCameraSpaceToModelSpace },
    { "UseModelCenterToTransform", UseModelCenterToTransform },
};
static_assert(sizeof(s_modelMethods) / sizeof(s_modelMethods[0]) == 0x3f, "the model table has 0x3f methods");

}

void ReadScriptLight(lua_State* L, int32_t index, const char* usage, SCRIPTLIGHT* light) {
    ReadLight(L, index, usage, light);
}

// ref: FUN_140a51120
void CSimpleModel_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_modelMethods, 0x3f);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_140a509c0 = (void*)&ReadVector3D;
// ---- end of generated globals ----
