// C_CVar: the generated bindings to the console variables. A variable flagged 0x40 is
// hidden from script and reads as absent.

#include "console/Var.hpp"
#include "framescript/ScriptMethod.hpp"
#include "glue/ScriptAPIHandlers.hpp"
#include "util/Fnv.hpp"
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

bool StringToBOOL(const char* string);

namespace {

constexpr uint32_t CVAR_FLAG_SCRIPT_HIDDEN = 0x40;

CVar* ScriptCVar(const char* name) {
    uint64_t hash = name == nullptr ? 0 : t2util::ConstFnvHashStringI(name);
    CVar* cvar = CVarLookup(hash);
    if (cvar == nullptr || (cvar->m_flags & CVAR_FLAG_SCRIPT_HIDDEN) != 0) {
        return nullptr;
    }
    return cvar;
}

// The generated argument check: one string argument, named in the usage on failure.
const char* NameArgument(lua_State* L, const char* usage) {
    if (lua_gettop(L) < 1) {
        luaL_error(L, usage);
    } else if (!lua_isstring(L, 1)) {
        luaL_argerror(L, 1, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        return lua_tolstring(L, 1, nullptr);
    }
    return nullptr;
}

void PushOptionalString(lua_State* L, const char* value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return;
    }
    if (value == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, value);
    }
}

// ref: FUN_140c5bc10
int32_t GetCVarBody(lua_State* L, void* context) {
    (void)context;
    const char* name = NameArgument(L, "Usage: local value = C_CVar.GetCVar(name)");
    // FUN_141c2c5b0: the value as an optional string
    CVar* cvar = ScriptCVar(name);
    std::string value;
    bool has = false;
    if (cvar != nullptr) {
        const char* text = cvar->m_stringValue.GetString();
        value = text != nullptr ? text : "";
        has = true;
    }
    PushOptionalString(L, has ? value.c_str() : nullptr);
    return 1;
}

// ref: FUN_140c5cb60
int32_t GetCVarBoolBody(lua_State* L, void* context) {
    (void)context;
    const char* name = NameArgument(L, "Usage: local value = C_CVar.GetCVarBool(name)");
    // FUN_141c2c850: the value read as a boolean, absent for an unknown variable
    CVar* cvar = ScriptCVar(name);
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    if (cvar == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushboolean(L, StringToBOOL(cvar->m_stringValue.GetString()));
    }
    return 1;
}

// ref: FUN_140c5d2e0
int32_t GetCVarDefaultBody(lua_State* L, void* context) {
    (void)context;
    const char* name = NameArgument(L, "Usage: local defaultValue = C_CVar.GetCVarDefault(name)");
    // FUN_141c2cbc0: nothing is returned for an unknown variable
    CVar* cvar = ScriptCVar(name);
    if (cvar == nullptr) {
        return 0;
    }
    // FUN_141c2c8e0
    const char* text = cvar->m_defaultValue.GetString();
    std::string value = text != nullptr ? text : "";
    PushOptionalString(L, value.c_str());
    return 1;
}

}

// ref: FUN_140c96190
int Lua_C_CVar_GetCVar(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetCVarBody, nullptr);
}

// ref: FUN_140c97c10
int Lua_C_CVar_GetCVarBool(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetCVarBoolBody, nullptr);
}

// ref: FUN_140c98940
int Lua_C_CVar_GetCVarDefault(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetCVarDefaultBody, nullptr);
}
