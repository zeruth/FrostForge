// A development aid, not the reference's: with FROST_LUA naming a file, the main thread runs
// that file in the UI's Lua state whenever it changes (checked once a second), so a running
// client's UI state can be read without a console. `frost_log(...)` writes its arguments to
// Logs/FrostLite.log under "lua-dev"; an error goes there too. Nothing happens without the
// variable.
#include "ui/DevLua.hpp"

#include "framescript/FrameScript.hpp"
#include "util/FrostLog.hpp"

#include <cstdio>
#include <string>
#include <windows.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {

int LogArgs(lua_State* L) {
    std::string line;
    int count = lua_gettop(L);
    for (int i = 1; i <= count; i++) {
        if (i > 1) {
            line += "  ";
        }
        switch (lua_type(L, i)) {
            case LUA_TNIL:
                line += "nil";
                break;
            case LUA_TBOOLEAN:
                line += lua_toboolean(L, i) ? "true" : "false";
                break;
            case LUA_TNUMBER:
            case LUA_TSTRING:
                line += lua_tostring(L, i);
                break;
            default: {
                char text[64];
                std::snprintf(text, sizeof(text), "%s: %p", lua_typename(L, lua_type(L, i)), lua_topointer(L, i));
                line += text;
                break;
            }
        }
    }
    FrostLog("lua-dev", "%s", line.c_str());
    return 0;
}

}  // namespace

void DevLuaPoll() {
    static int s_state = 0;   // 0 unread, 1 set, 2 not set
    static char s_path[MAX_PATH];
    static FILETIME s_written = {};
    static DWORD s_next = 0;
    if (s_state == 0) {
        DWORD length = GetEnvironmentVariableA("FROST_LUA", s_path, sizeof(s_path));
        s_state = (length != 0 && length < sizeof(s_path)) ? 1 : 2;
    }
    if (s_state != 1) {
        return;
    }
    DWORD now = GetTickCount();
    if (static_cast<int32_t>(now - s_next) < 0) {
        return;
    }
    s_next = now + 1000;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExA(s_path, GetFileExInfoStandard, &attributes)
        || CompareFileTime(&attributes.ftLastWriteTime, &s_written) == 0) {
        return;
    }
    s_written = attributes.ftLastWriteTime;
    FILE* file = std::fopen(s_path, "rb");
    if (file == nullptr) {
        return;
    }
    std::string source;
    char buffer[4096];
    for (size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) {
        source.append(buffer, read);
    }
    std::fclose(file);
    lua_State* L = FrameScript_GetContext();
    if (L == nullptr) {
        return;
    }
    int top = lua_gettop(L);
    lua_pushcfunction(L, LogArgs);
    lua_setfield(L, LUA_GLOBALSINDEX, "frost_log");
    FrostLog("lua-dev", "running %s", s_path);
    if (luaL_loadbuffer(L, source.data(), source.size(), "=FROST_LUA") != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        const char* error = lua_tostring(L, -1);
        FrostLog("lua-dev", "error: %s", error != nullptr ? error : "?");
    }
    lua_settop(L, top);
}
