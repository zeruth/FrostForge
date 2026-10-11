#include "framescript/ScriptAPI.hpp"

#include <cstdarg>

#include "storm/String.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

// ref: FUN_1407d2e10
void ScriptOpenTable(lua_State* L, const char* name, int32_t index) {
    lua_getfield(L, index, name);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        lua_createtable(L, 0, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, index, name);
    }
}

// ref: FUN_1407d2f20
void ScriptRegisterFunction(lua_State* L, const char* space, const char* name, lua_CFunction function) {
    ScriptOpenTable(L, space, LUA_GLOBALSINDEX);
    int32_t top = lua_gettop(L);
    // FUN_1407e92b0: the closure stored under its name
    lua_pushcclosure(L, function, 0);
    lua_setfield(L, top, name);
    lua_settop(L, -2);
}

namespace {

void PushNumberChecked(lua_State* L, double value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return;
    }
    lua_pushnumber(L, value);
}

}

void ScriptRegisterEnum(lua_State* L, const char* name, const SCRIPTENUMVALUE* values, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        ScriptOpenTable(L, "Enum", LUA_GLOBALSINDEX);
        ScriptOpenTable(L, name, lua_gettop(L));
        PushNumberChecked(L, values[i].m_value);
        lua_setfield(L, -2, values[i].m_name);
        lua_settop(L, -3);
    }
}

// ref: FUN_1407cb7e0
void ScriptSetEnumValue(lua_State* L, const char* name, const char* field, int32_t value) {
    ScriptOpenTable(L, "Enum", LUA_GLOBALSINDEX);
    ScriptOpenTable(L, name, lua_gettop(L));
    PushNumberChecked(L, static_cast<double>(value));
    lua_setfield(L, -2, field);
    lua_settop(L, -3);
}

// ref: FUN_1407cb890
void ScriptSetEnumValueU32(lua_State* L, const char* name, const char* field, uint32_t value) {
    ScriptOpenTable(L, "Enum", LUA_GLOBALSINDEX);
    ScriptOpenTable(L, name, lua_gettop(L));
    PushNumberChecked(L, static_cast<double>(value));
    lua_setfield(L, -2, field);
    lua_settop(L, -3);
}

// ref: FUN_14065f4e0
uint64_t ScriptFormatUInt64(char* out, const char* format, ...) {
    va_list args;
    va_start(args, format);
    uint64_t count = SStrVPrintfCount(out, 0x14, format, args);
    va_end(args);
    return count;
}

// ref: FUN_14065f770
void ScriptPushUInt64(lua_State* L, uint64_t value) {
    char text[0x14];
    uint64_t count = ScriptFormatUInt64(text, "0x%016llX", value);
    lua_pushlstring(L, text, count);
}

// ref: FUN_1407cb940
void ScriptSetEnumValueU64(lua_State* L, const char* name, const char* field, uint64_t value) {
    ScriptOpenTable(L, "Enum", LUA_GLOBALSINDEX);
    ScriptOpenTable(L, name, lua_gettop(L));
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        ScriptPushUInt64(L, value);
    }
    lua_setfield(L, -2, field);
    lua_settop(L, -3);
}

// ref: FUN_140bb3ea0
void ScriptSetEnumValue8(lua_State* L, const char* name, const char* field, uint8_t value) {
    ScriptOpenTable(L, "Enum", LUA_GLOBALSINDEX);
    ScriptOpenTable(L, name, lua_gettop(L));
    PushNumberChecked(L, static_cast<double>(value));
    lua_setfield(L, -2, field);
    lua_settop(L, -3);
}

// ref: FUN_140bb3f50
void ScriptSetEnumValue16(lua_State* L, const char* name, const char* field, uint16_t value) {
    ScriptOpenTable(L, "Enum", LUA_GLOBALSINDEX);
    ScriptOpenTable(L, name, lua_gettop(L));
    PushNumberChecked(L, static_cast<double>(value));
    lua_setfield(L, -2, field);
    lua_settop(L, -3);
}

// ref: FUN_140c09480
void ScriptSetTableValue(lua_State* L, const char* table, const char* name, int32_t value, const char* field) {
    ScriptOpenTable(L, table, LUA_GLOBALSINDEX);
    ScriptOpenTable(L, name, lua_gettop(L));
    PushNumberChecked(L, static_cast<double>(value));
    lua_setfield(L, -2, field);
    lua_settop(L, -3);
}
