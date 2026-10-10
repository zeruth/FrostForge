// The UI's named colours: GlobalColor read once into a map by constant name, and handed to
// scripts by C_UIColor.GetColors as ColorMixin tables.

#include "db/DB2Tables.hpp"
#include "db/DynamicLifeTimeDB.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "framescript/ScriptMethod.hpp"
#include "glue/ScriptAPIHandlers.hpp"
#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {

// ColorInfo: the colour, and its hex spelling.
struct COLORINFO {
    uint32_t m_color;       // 0x48 in the tree node
    char m_hex[12];         // 0x4c
};

// One row of what C_UIColor.GetColors returns (DBColorExport, 0x10).
struct DBCOLOREXPORT {
    const char* m_baseTag;
    uint32_t m_color;
};

DynamicLifeTimeDB s_globalColor(&g_globalColorDB);     // DAT_14311bf98
std::map<std::string, COLORINFO> s_colors;              // DAT_142d1ad50
bool s_colorsBuilt;                                     // DAT_14311bf80

constexpr float COLOR_SCALE = 1.0f / 255.0f;            // DAT_142632528

// Reads GlobalColor into the map once, holding the table only while it does.
// ref: FUN_1416728e0
void GlobalColorBuild() {
    if (s_colorsBuilt) {
        return;
    }
    s_globalColor.Acquire();
    s_globalColor.Wait();
    if (WowDB2Lookup* lookup = g_globalColorDB.m_lookup) {
        DB2LOOKUPITER it;
        WowDB2LookupBegin(lookup, &it);
        while (it.m_index != -2) {
            auto record = static_cast<const int32_t*>(it.m_record);
            const char* name = record[0] != 0 ? reinterpret_cast<const char*>(record) + record[0] : "";
            COLORINFO info;
            info.m_color = static_cast<uint32_t>(
                DB2FieldI32(reinterpret_cast<const uint8_t*>(record + 1), 1, 4, &g_globalColorDB));
            std::snprintf(info.m_hex, 9, "%x", info.m_color);
            s_colors.emplace(name, info);
            lookup->Next(&it);
        }
    }
    s_globalColor.Release();
    s_colorsBuilt = true;
}

// Every named colour, in name order.
// ref: FUN_1416725c0
std::vector<DBCOLOREXPORT> GlobalColorExports() {
    if (!s_colorsBuilt) {
        GlobalColorBuild();
    }
    std::vector<DBCOLOREXPORT> exports;
    for (const auto& entry : s_colors) {
        exports.push_back(DBCOLOREXPORT { entry.first.c_str(), entry.second.m_color });
    }
    return exports;
}

// A colour as a ColorMixin table: r, g, b and a from its ARGB bytes.
// ref: FUN_140d13080
int32_t PushColorMixin(lua_State* L, uint32_t color) {
    if (!lua_checkstack(L, 1)) {
        return luaL_error(L, "stack overflow");
    }
    lua_createtable(L, 0, 4);
    lua_getfield(L, LUA_GLOBALSINDEX, "ColorMixin");
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, -5);
        }
    }
    lua_settop(L, -2);
    int32_t table = lua_gettop(L);
    lua_pushnumber(L, static_cast<float>((color >> 16) & 0xff) * COLOR_SCALE);
    lua_setfield(L, table, "r");
    lua_pushnumber(L, static_cast<float>((color >> 8) & 0xff) * COLOR_SCALE);
    lua_setfield(L, table, "g");
    lua_pushnumber(L, static_cast<float>(color & 0xff) * COLOR_SCALE);
    lua_setfield(L, table, "b");
    lua_pushnumber(L, static_cast<float>(color >> 24) * COLOR_SCALE);
    lua_setfield(L, table, "a");
    return 1;
}

// ref: FUN_140d1b7c0
int32_t GetColorsBody(lua_State* L, void* context) {
    auto exports = static_cast<std::vector<DBCOLOREXPORT>*>(context);
    *exports = GlobalColorExports();
    if (!lua_checkstack(L, 1)) {
        return luaL_error(L, "stack overflow");
    }
    lua_createtable(L, static_cast<int32_t>(exports->size()), 0);
    int32_t list = lua_gettop(L);
    int32_t index = 0;
    for (const DBCOLOREXPORT& entry : *exports) {
        index++;
        lua_pushinteger(L, index);
        lua_createtable(L, 0, 2);
        int32_t row = lua_gettop(L);
        if (entry.m_baseTag == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushstring(L, entry.m_baseTag);
        }
        lua_setfield(L, row, "baseTag");
        PushColorMixin(L, entry.m_color);
        lua_setfield(L, row, "color");
        lua_settable(L, list);
    }
    return 1;
}

}

// ref: FUN_140d302e0
int Lua_C_UIColor_GetColors(lua_State* L) {
    std::vector<DBCOLOREXPORT> exports;
    return ScriptMethod::ProtectedCall(L, GetColorsBody, &exports);
}
