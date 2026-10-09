// The Lua methods every UI object has (FrameScript_Object's table 0x142cf8be0) and those of
// every named object (CScriptObject's table 0x142cf9490).

#include "ui/CScriptObjectScript.hpp"

#include "blz/String.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/CScriptObject.hpp"
#include "ui/CSimpleTop.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

namespace {

using namespace ScriptMethod;

// ---- FrameScript_Object ----------------------------------------------------------------

// ref: FUN_140a4c610
int GetName(lua_State* L) {
    int32_t index = 1;
    auto object = Self(L, FrameScript_Object::GetObjectType(), &index, "Usage: local name = self:GetName()");
    const char* name = object->GetName();
    RaisePendingError(L, object, "GetName");
    return PushStringOrNil(L, name);
}

// ref: FUN_140a4d3b0
int GetObjectType(lua_State* L) {
    int32_t index = 1;
    auto object = Self(L, FrameScript_Object::GetObjectType(), &index, "Usage: local objectType = self:GetObjectType()");
    const char* type = object->GetObjectTypeName();
    RaisePendingError(L, object, "GetObjectType");
    return PushStringOrNil(L, type);
}

// Forbidden: forbidden, protected-metatable, locked down, or not accessible.
// ref: FUN_1424cc6e0
int IsForbiddenBody(lua_State* L) {
    const char* error = nullptr;
    FrameScript_Object* object = FrameScript_Object::GetObjectFromLua(&error, L, FrameScript_Object::GetObjectType(), 1, true, true);
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    if (object == nullptr) {
        return 0;
    }
    uint16_t flags = object->m_flags;
    bool forbidden = (flags & FSOBJ_FORBIDDEN) || (flags & FSOBJ_PROTECTED_METATABLE) || (flags & FSOBJ_LOCKDOWN)
        || !(flags & FSOBJ_ACCESSIBLE);
    lua_pushboolean(L, forbidden);
    return 1;
}

// ref: FUN_140a4e150
int IsForbidden(lua_State* L) {
    int results = IsForbiddenBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_140a4ee00
int IsObjectType(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local isType = self:IsObjectType(objectType)";
    auto object = Self(L, FrameScript_Object::GetObjectType(), &index, usage);
    const char* type = RequiredString(L, &index, usage);
    bool is = object->IsObjectTypeName(type);
    RaisePendingError(L, object, "IsObjectType");
    return PushBool(L, is);
}

// Untainted code forbids the object; tainted code is reported to the blocked-action hook.
// ref: FUN_1424cc780
int SetForbiddenBody(lua_State* L) {
    const char* error = nullptr;
    FrameScript_Object* object = FrameScript_Object::GetObjectFromLua(&error, L, FrameScript_Object::GetObjectType(), 1, true, true);
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    if (object != nullptr) {
        if (lua_taint == nullptr) {
            object->SetProtected();
        } else if (CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
            CSimpleTop::s_instance->m_actionBlockedHook(reinterpret_cast<CSimpleFrame*>(object));
        }
    }
    return 0;
}

// ref: FUN_140a4fc10
int SetForbidden(lua_State* L) {
    int results = SetForbiddenBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ---- CScriptObject ---------------------------------------------------------------------

struct DEBUGNAMECONTEXT {
    CScriptObject* m_object = nullptr;
    bool m_preferParentKey = false;
    blz::string m_name;
};

// ref: FUN_1408c7240
int32_t GetDebugNameBody(lua_State* L, void* context) {
    auto ctx = static_cast<DEBUGNAMECONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local debugName = self:GetDebugName([preferParentKey])";
    ctx->m_object = static_cast<CScriptObject*>(Self(L, CScriptObject::GetObjectType(), &index, usage));
    ctx->m_preferParentKey = OptionalBool(L, &index);
    alignas(blz::string) uint8_t storage[sizeof(blz::string)];
    blz::string* name = ctx->m_object->GetDebugName(reinterpret_cast<blz::string*>(storage), ctx->m_preferParentKey);
    blz::string_assign(reinterpret_cast<blz::string_header*>(&ctx->m_name), name->m_data, name->m_size);
    name->~string();
    RaisePendingError(L, ctx->m_object, "GetDebugName");
    return PushLString(L, ctx->m_name.m_data, ctx->m_name.m_size);
}

// ref: FUN_1408eccd0
int GetDebugName(lua_State* L) {
    DEBUGNAMECONTEXT context;
    return ProtectedCall(L, GetDebugNameBody, &context);
}

// ref: FUN_1408eda30
int GetParent(lua_State* L) {
    int32_t index = 1;
    auto object = static_cast<CScriptObject*>(Self(L, CScriptObject::GetObjectType(), &index, "Usage: local parent = self:GetParent()"));
    CScriptObject* parent = object->GetParent();
    RaisePendingError(L, object, "GetParent");
    return PushObject(L, parent);
}

// ref: FUN_1408ee820
int GetParentKey(lua_State* L) {
    int32_t index = 1;
    auto object = static_cast<CScriptObject*>(
        Self(L, CScriptObject::GetObjectType(), &index, "Usage: local parentKey = self:GetParentKey()"));
    const char* key = object->GetParentKey();
    RaisePendingError(L, object, "GetParentKey");
    return PushStringOrNil(L, key);
}

// ref: FUN_1408ef5c0
int SetParentKey(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetParentKey(parentKey)";
    auto object = static_cast<CScriptObject*>(Self(L, CScriptObject::GetObjectType(), &index, usage));
    const char* key = RequiredString(L, &index, usage);
    object->SetParentKey(key);
    RaisePendingError(L, object, "SetParentKey");
    return 0;
}


// 0x142cf8be0
const luaL_Reg s_objectMethods[] = {
    {"GetName", GetName},
    {"GetObjectType", GetObjectType},
    {"IsForbidden", IsForbidden},
    {"IsObjectType", IsObjectType},
    {"SetForbidden", SetForbidden},
};

// 0x142cf9490
const luaL_Reg s_scriptObjectMethods[] = {
    {"GetDebugName", GetDebugName},
    {"GetParent", GetParent},
    {"GetParentKey", GetParentKey},
    {"SetParentKey", SetParentKey},
};

}

// The object methods alone, for objects that are not script regions (fonts).
// ref: FUN_140a512b0
void CScriptObject_FillObjectScriptMethods(lua_State* L) {
    FrameScript_Object::FillScriptMethodTable(L, s_objectMethods, 5);
}

// ref: FUN_140a51270
void CScriptObject_FillScriptMethods(lua_State* L) {
    FrameScript_Object::FillScriptMethodTable(L, s_objectMethods, 5);
    FrameScript_Object::FillScriptMethodTable(L, s_scriptObjectMethods, 4);
}
