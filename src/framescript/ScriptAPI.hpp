#ifndef FRAMESCRIPT_SCRIPT_API_HPP
#define FRAMESCRIPT_SCRIPT_API_HPP

// The helpers the generated script-API registrations call: namespace tables made on demand,
// functions put into them, and the Enum tables' values (Engine/Source/ScriptAPI, 0x1407cb000..
// 0x1407d3000).

#include <cstdint>

struct lua_State;
typedef int (*lua_CFunction)(lua_State* L);

// One Enum value (generated code pushes each as a number).
struct SCRIPTENUMVALUE {
    const char* m_name;
    double m_value;
};

// One function of a registration table ({name, function}, 0x10).
struct SCRIPTFUNCTION {
    const char* m_name;
    lua_CFunction m_function;
};

// Pushes the table field `name` of the table at `index`, making it first when it is missing
// (FUN_1407d2e10).
void ScriptOpenTable(lua_State* L, const char* name, int32_t index);
// A function in a namespace table, made when missing (FUN_1407d2f20).
void ScriptRegisterFunction(lua_State* L, const char* space, const char* name, lua_CFunction function);
// Enum.<name>[field] = value for each value, as the generated code does one value at a time.
void ScriptRegisterEnum(lua_State* L, const char* name, const SCRIPTENUMVALUE* values, uint64_t count);
// Enum.<name>[field] = value (FUN_1407cb7e0, the Meta tables' fields), and the setters for an
// unsigned value (FUN_1407cb890), a 64-bit one pushed as "0x%016llX" (FUN_1407cb940), a byte
// (FUN_140bb3ea0) and a short (FUN_140bb3f50).
void ScriptSetEnumValue(lua_State* L, const char* name, const char* field, int32_t value);
void ScriptSetEnumValueU32(lua_State* L, const char* name, const char* field, uint32_t value);
void ScriptSetEnumValueU64(lua_State* L, const char* name, const char* field, uint64_t value);
void ScriptSetEnumValue8(lua_State* L, const char* name, const char* field, uint8_t value);
void ScriptSetEnumValue16(lua_State* L, const char* name, const char* field, uint16_t value);
// <table>.<name>[field] = value, the table named by the caller (FUN_140c09480).
void ScriptSetTableValue(lua_State* L, const char* table, const char* name, int32_t value, const char* field);
// "0x%016llX" into a 0x14-byte buffer (FUN_14065f4e0), and a 64-bit value pushed that way
// (FUN_14065f770).
uint64_t ScriptFormatUInt64(char* out, const char* format, ...);
void ScriptPushUInt64(lua_State* L, uint64_t value);

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

// What the generated registrations inline for each value they write themselves: the stack
// check, then the number (or the 64-bit string).
inline void ScriptPushEnumNumber(lua_State* L, double value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return;
    }
    lua_pushnumber(L, value);
}

inline void ScriptPushEnumUInt64(lua_State* L, uint64_t value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return;
    }
    ScriptPushUInt64(L, value);
}

#endif
