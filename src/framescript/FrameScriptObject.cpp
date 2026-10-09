#include "framescript/FrameScriptObject.hpp"

#include <cstring>

#include "base/Status.hpp"
#include "storm/String.hpp"

extern "C" {
#include "lua.h"
}

namespace {

// The value a secure mixin proxy's metatable is locked with.
constexpr lua_Integer SECURE_MIXIN_TAG = 0xa16d;

// 0x142cf5510
const char* const s_scriptNames[FRAMESCRIPT_NUM_SCRIPTS] = {
    "onclick", "preclick", "postclick", "ondoubleclick", "onload", "onfinished", "onplay",
    "onpause", "onstop", "onsizechanged", "onupdate", "onloop", "onshow", "onhide", "onenter",
    "onleave", "onmousedown", "onmouseup", "onmousewheel", "ondragstart", "ondragstop",
    "onreceivedrag", "onchar", "onhyperlinkclick", "onhyperlinkenter", "onhyperlinkleave",
    "onevent", "onkeydown", "onkeyup", "ongamepadbuttondown", "ongamepadbuttonup",
    "ongamepadstick", "onattributechanged", "onenable", "ondisable", "onenterpressed",
    "onescapepressed", "onscrollrangechanged", "onverticalscroll", "onhorizontalscroll",
    "onvaluechanged", "ontabpressed", "oneditfocuslost", "oneditfocusgained", "ontextchanged",
    "onmodelloaded", "onmodelcleared", "onmodelloading", "ondressmodel", "onmovieshowsubtitle",
    "onmoviehidesubtitle", "onmoviefinished", "ontooltipsetdefaultanchor", "ontooltipcleared",
    "ontooltipaddmoney", "ontooltipsetunit", "ontooltipsetitem", "ontooltipsetspell",
    "ontooltipsetquest", "ontooltipsetachievement", "ontooltipsetequipmentset",
    "ontooltipsetframestack", "onspacepressed", "ontextset", "oninputlanguagechanged",
    "oncharcomposition", "onminmaxchanged", "oncursorchanged", "onanimstarted",
    "onanimfinished", "oncolorselect", "onexternallink", "onbuttonupdate", "onarrowpressed",
    "onerror", "onpanfinished", "oncooldowndone", "onrequestnewsize", "onuimapchanged",
};

}

// ref: FUN_140642a70
uint32_t FrameScript_GetScriptIndex(const char* name) {
    if (name != nullptr) {
        char lower[0x80];
        char* dest = lower;
        uint64_t left = 0x7f;
        while (true) {
            char c = *name++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *dest = '\0';
                break;
            }
        }
        _strlwr(lower);
        for (uint32_t i = 0; i < FRAMESCRIPT_NUM_SCRIPTS; i++) {
            if (strcmp(lower, s_scriptNames[i]) == 0) {
                return i;
            }
        }
    }
    return FRAMESCRIPT_NUM_SCRIPTS + 1;
}

// ref: FUN_140644e90
void FrameScript_CreateSecureMixins(lua_State* L, const char* mixins, const char* owner, CStatus* status) {
    if (L == nullptr || mixins == nullptr) {
        return;
    }
    char name[0x400];
    name[0] = '\0';
    const char* cursor = mixins;
    do {
        SStrTokenize(&cursor, name, sizeof(name), " ,", nullptr);
        if (name[0] == '\0') {
            return;
        }
        lua_checkstack(L, 4);
        lua_pushlstring(L, name, strlen(name));
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_type(L, -1) == LUA_TTABLE) {
            int32_t pushed = lua_getmetatable(L, -1);
            int32_t top;
            if (pushed != 0) {
                lua_pushlstring(L, "__metatable", 11);
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1) && lua_tointeger(L, -1) == SECURE_MIXIN_TAG) {
                    // already a secure proxy
                    lua_settop(L, -3 - pushed);
                    continue;
                }
                lua_settop(L, -2 - pushed);
                lua_pushnil(L);
                lua_setmetatable(L, -2);
            }
            // _G[name] = setmetatable({}, { __index = original, __metatable = tag })
            lua_createtable(L, 0, 0);
            lua_pushlstring(L, "__index", 7);
            lua_pushvalue(L, -3);
            lua_settable(L, -3);
            lua_pushlstring(L, "__metatable", 11);
            lua_pushinteger(L, SECURE_MIXIN_TAG);
            lua_settable(L, -3);
            lua_createtable(L, 0, 0);
            lua_pushvalue(L, -2);
            lua_setmetatable(L, -2);
            lua_pushlstring(L, name, strlen(name));
            lua_insert(L, -2);
            lua_rawset(L, LUA_GLOBALSINDEX);
            top = -3;
            lua_settop(L, top);
        } else {
            lua_settop(L, -2);
            status->Add(1, "%s: Has bad mixin: %s", owner != nullptr ? owner : "", name);
        }
    } while (name[0] != '\0');
}
