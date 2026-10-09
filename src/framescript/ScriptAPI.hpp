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
// Enum.<name>[field] = value (FUN_1407cb7e0, the Meta tables' fields).
void ScriptSetEnumValue(lua_State* L, const char* name, const char* field, int32_t value);

#endif
