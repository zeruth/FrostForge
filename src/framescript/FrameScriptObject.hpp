#ifndef FRAMESCRIPT_FRAME_SCRIPT_OBJECT_HPP
#define FRAMESCRIPT_FRAME_SCRIPT_OBJECT_HPP

// FrameScript objects (Engine/Source/FrameScript/FrameScript_Object.cpp): the script handlers
// a scriptable object can carry.

#include <cstdint>

// The handler names, lower case: "onclick", "preclick", ..., "onuimapchanged".
constexpr uint32_t FRAMESCRIPT_NUM_SCRIPTS = 0x4f;
// The index of a handler name (any case), or FRAMESCRIPT_NUM_SCRIPTS + 1 if unknown.
uint32_t FrameScript_GetScriptIndex(const char* name);

struct lua_State;
class CStatus;

// Turns each named global mixin table (a list separated by spaces or commas) into a read-only
// proxy: the global becomes an empty table whose metatable reads through to the original and
// is locked with the secure-mixin tag. A name that is not a table is reported against `owner`.
void FrameScript_CreateSecureMixins(lua_State* L, const char* mixins, const char* owner, CStatus* status);

#endif
