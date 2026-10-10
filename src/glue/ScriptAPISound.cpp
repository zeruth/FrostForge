// Sound_GameSystem / Sound_ChatSystem: the sound device bindings the audio options read. The
// chat system has no devices in this client and answers "None".

#include "framescript/FrameScript.hpp"
#include "glue/ScriptAPIHandlers.hpp"
#include "sound/SE3.hpp"
#include "sound/SI3.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {

// The bounded copy the handlers inline: at most size - 1 characters, always terminated.
void CopyText(char* dest, const char* source, uint64_t size) {
    uint64_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

}

// ref: FUN_141309ff0
int Lua_Sound_ChatSystem_GetInputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        char name[0x800] = "None";
        lua_pushstring(L, name);
        return 1;
    }
    luaL_error(L, "Usage: Sound_ChatSystem_GetInputDriverNameByIndex(InputDriverIndex)");
    return 0;
}

// ref: FUN_14130add0
int Lua_Sound_ChatSystem_GetOutputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        char name[0x800] = "None";
        lua_pushstring(L, name);
        return 1;
    }
    luaL_error(L, "Usage: Sound_ChatSystem_GetOutputDriverNameByIndex(OutputDriverIndex)");
    return 0;
}

// ref: FUN_141307e00
int Lua_Sound_GameSystem_GetInputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        lua_pushstring(L, "None");
        return 1;
    }
    luaL_error(L, "Usage: Sound_GameSystem_GetInputDriverNameByIndex(InputDriverIndex)");
    return 0;
}

// ref: FUN_1413077a0
int Lua_Sound_GameSystem_GetNumInputDrivers(lua_State* L) {
    lua_pushnumber(L, 0.0);
    return 1;
}

// ref: FUN_141308ad0
int Lua_Sound_GameSystem_GetNumOutputDrivers(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(SE3GetNumOutputDrivers()));
    return 1;
}

// The driver's name; the system default shows as the localized SYSTEM_DEFAULT, and a driver
// that is not there as NONE.
// ref: FUN_141309140
int Lua_Sound_GameSystem_GetOutputDriverNameByIndex(lua_State* L) {
    if (lua_isnumber(L, 1) != 0) {
        int32_t index = static_cast<int32_t>(lua_tonumber(L, 1));
        char name[0x800];
        CopyText(name, FrameScript_GetText("NONE", -1, 0), 0x800);
        bool isDefault = false;
        SE3GetOutputDriverName(index, name, 0x800, &isDefault);
        if (isDefault) {
            CopyText(name, FrameScript_GetText("SYSTEM_DEFAULT", -1, 0), 0x800);
        }
        lua_pushstring(L, name);
        return 1;
    }
    luaL_error(L, "Usage: Sound_GetOutputDriverNameByIndex(OutputDriverIndex)");
    return 0;
}

// ref: FUN_14130bbb0
int Lua_Sound_GameSystem_RestartSoundSystem(lua_State* L) {
    (void)L;
    SI3RequestRestart();
    return 0;
}
