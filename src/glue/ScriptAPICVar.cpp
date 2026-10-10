// C_CVar: the generated bindings to the console variables. A variable flagged 0x40 is
// hidden from script and reads as absent.

#include "console/Var.hpp"
#include "framescript/ScriptMethod.hpp"
#include "glue/ScriptAPIHandlers.hpp"
#include "framescript/EventCallbacks.hpp"
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

// The script API's view of a CVar by name: whether scripts see it (FUN_141c2cbc0), may change
// it (FUN_141c2cab0: not read-only, flags 0x104), and are trusted to when it is protected
// (FUN_141c2cb30: flag 8 needs secure code, action 0x23).
bool ScriptCVarVisible(const char* name) {
    CVar* cvar = CVarLookup(name == nullptr ? 0 : t2util::ConstFnvHashStringI(name));
    return cvar != nullptr && (cvar->m_flags & CVAR_FLAG_SCRIPT_HIDDEN) == 0;
}

bool ScriptCVarWritable(const char* name) {
    CVar* cvar = CVarLookup(name == nullptr ? 0 : t2util::ConstFnvHashStringI(name));
    return cvar != nullptr && (cvar->m_flags & 0x104) == 0;
}

bool ScriptCVarPermitted(const char* name) {
    CVar* cvar = CVarLookup(name == nullptr ? 0 : t2util::ConstFnvHashStringI(name));
    if (cvar == nullptr) {
        return false;
    }
    return (cvar->m_flags >> 3 & 1) == 0 || ScriptMethod::IsActionPermitted(0x23);
}

// The value set (absent reads as "0"), and CVAR_UPDATE for a script CVar name.
// ref: FUN_141c2cc90
bool ScriptCVarSet(const char* name, const char* value, const char* scriptCVar) {
    CVar* cvar = CVarLookup(name == nullptr ? 0 : t2util::ConstFnvHashStringI(name));
    const char* text = value != nullptr ? value : "0";
    bool result = CVarSet(cvar, text, true, false, false, true);
    if (scriptCVar != nullptr) {
        UiSignalCVarUpdate(scriptCVar, value != nullptr ? value : "0");
    }
    return result;
}

// A bit of a visible CVar, converting an old-form value first; nothing for an unknown one.
// ref: FUN_141c2c780
bool ScriptCVarGetBit(const char* name, uint32_t index, bool* bit) {
    CVar* cvar = CVarLookup(name == nullptr ? 0 : t2util::ConstFnvHashStringI(name));
    if (cvar == nullptr || (cvar->m_flags & CVAR_FLAG_SCRIPT_HIDDEN) != 0) {
        return false;
    }
    const char* value = cvar->m_stringValue.GetString();
    if (value[0] != 2) {
        CVarConvertBitfield(cvar, value[0]);
    }
    *bit = CVarGetBit(cvar, index);
    return true;
}

// ref: FUN_141c2cd60
bool ScriptCVarSetBit(const char* name, uint32_t index, bool on, const char* scriptCVar) {
    CVar* cvar = CVarLookup(name == nullptr ? 0 : t2util::ConstFnvHashStringI(name));
    int32_t marker = cvar != nullptr ? cvar->m_stringValue.GetString()[0] : 0;
    if (cvar == nullptr || marker != 2) {
        CVarConvertBitfield(cvar, marker);
    }
    CVarSetBit(cvar, index, on);
    if (scriptCVar != nullptr) {
        UiSignalCVarUpdate(scriptCVar, on ? "1" : "0");
    }
    return true;
}

// One string argument at `index`, optional or not; the generated checks raise the usage.
const char* StringArgument(lua_State* L, int32_t index, bool optional, const char* usage, bool* present) {
    *present = false;
    if (lua_type(L, index) < 1) {
        if (!optional) {
            luaL_error(L, usage);
        }
        return nullptr;
    }
    if (!lua_isstring(L, index)) {
        luaL_argerror(L, index, usage);
        return nullptr;
    }
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return nullptr;
    }
    *present = true;
    return lua_tolstring(L, index, nullptr);
}

uint32_t IndexArgument(lua_State* L, int32_t index, const char* usage) {
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
        return 0;
    }
    if (!lua_isnumber(L, index)) {
        luaL_argerror(L, index, usage);
        return 0;
    }
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    return static_cast<uint32_t>(static_cast<int32_t>(lua_tonumber(L, index) - 1.0));  // DAT_14261c978
}

void PushBool(lua_State* L, bool value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return;
    }
    lua_pushboolean(L, value);
}

// ref: FUN_140c5e1b0
int32_t SetCVarBody(lua_State* L, void* context) {
    (void)context;
    const char* usage = "Usage: local success = C_CVar.SetCVar(name [, value, scriptCVar])";
    bool present;
    const char* name = StringArgument(L, 1, false, usage, &present);
    int32_t index = present ? 2 : 1;
    bool hasValue;
    const char* value = StringArgument(L, index, true, usage, &hasValue);
    index++;
    bool hasScript;
    const char* scriptCVar = StringArgument(L, index, true, usage, &hasScript);
    if (!ScriptCVarVisible(name) || !ScriptCVarWritable(name) || !ScriptCVarPermitted(name)) {
        return 0;
    }
    bool result = ScriptCVarSet(name, hasValue ? value : nullptr, hasScript ? scriptCVar : nullptr);
    PushBool(L, result);
    return 1;
}

// ref: FUN_140c5c360
int32_t GetCVarBitfieldBody(lua_State* L, void* context) {
    (void)context;
    const char* usage = "Usage: local value = C_CVar.GetCVarBitfield(name, index)";
    bool present;
    const char* name = StringArgument(L, 1, false, usage, &present);
    uint32_t index = IndexArgument(L, present ? 2 : 1, usage);
    bool bit = false;
    if (!ScriptCVarGetBit(name, index, &bit)) {
        if (lua_checkstack(L, 1)) {
            lua_pushnil(L);
        }
        return 1;
    }
    PushBool(L, bit);
    return 1;
}

// ref: FUN_140c5e9d0
int32_t SetCVarBitfieldBody(lua_State* L, void* context) {
    (void)context;
    const char* usage = "Usage: local success = C_CVar.SetCVarBitfield(name, index, value [, scriptCVar])";
    bool present;
    const char* name = StringArgument(L, 1, false, usage, &present);
    int32_t argument = present ? 2 : 1;
    uint32_t index = IndexArgument(L, argument, usage);
    argument++;
    bool on = false;
    if (lua_type(L, argument) < 1) {
        luaL_error(L, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        on = lua_toboolean(L, argument) != 0;
        argument++;
    }
    bool hasScript;
    const char* scriptCVar = StringArgument(L, argument, true, usage, &hasScript);
    if (!ScriptCVarVisible(name) || !ScriptCVarWritable(name) || !ScriptCVarPermitted(name)
        || !CVarBitIndexValid(index)) {
        return 0;
    }
    bool result = ScriptCVarSetBit(name, index, on, hasScript ? scriptCVar : nullptr);
    PushBool(L, result);
    return 1;
}

}

// ref: FUN_140c9aa00
int Lua_C_CVar_SetCVar(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, SetCVarBody, nullptr);
}

// ref: FUN_140c96ee0
int Lua_C_CVar_GetCVarBitfield(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetCVarBitfieldBody, nullptr);
}

// ref: FUN_140c9b730
int Lua_C_CVar_SetCVarBitfield(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, SetCVarBitfieldBody, nullptr);
}

namespace {
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
