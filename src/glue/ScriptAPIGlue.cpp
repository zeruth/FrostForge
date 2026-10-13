// The glue's plain script functions that read client state: the saved account, the build,
// the screen and render scale limits, and the event and login queries.

#include "client/ClientCVars.hpp"
#include "client/CommandLine.hpp"
#include "client/ConsoleDevice.hpp"
#include "client/Locale.hpp"
#include "client/GraphicsSettings.hpp"
#include "client/GxWindow.hpp"
#include "client/RenderSettings.hpp"
#include "console/Var.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptEvents.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "glue/ScriptAPIHandlers.hpp"
#include "gx/Gx.hpp"
#include "gx/GxDevice.hpp"
#include "ui/CScriptObject.hpp"
#include "ui/CSimpleModelFFX.hpp"
#include "world/WorldFrame.hpp"
#include <cstring>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

// The character list's size (DAT_143122480), set by the character list when it arrives.
int32_t g_numCharacters;
// The glue screen's display state (DAT_14311c534): 2 once the first screen has been shown.
int32_t g_glueScreenDisplayState;
// The live region's character services (DAT_1431227b3..5), from the realm list.
uint8_t g_liveRegionCharacterList;
uint8_t g_liveRegionCharacterCopy;
uint8_t g_liveRegionAccountCopy;
// Set when the launcher's login has been used up (DAT_1430af424).
uint8_t g_launcherLoginUsed;
// Whether the realm connection can be resumed without a password (DAT_1430af4c8).
uint8_t g_reconnectLoginPossible;
// The character select screen's model frame (DAT_143122460).
CSimpleModelFFX* g_charSelectModelFrame;

namespace {

void PushOptionalString(lua_State* L, const char* text) {
    if (text == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, text);
    }
}

// The body C_Debug.PrintToDebugWindow runs (inline in FUN_140fa2560).
int32_t PrintToDebugWindowBody(lua_State* L, void*) {
    if (lua_gettop(L) < 1) {
        luaL_error(L, "Usage: C_Debug.PrintToDebugWindow(printMsg)");
    } else if (!lua_isstring(L, 1)) {
        luaL_argerror(L, 1, "Usage: C_Debug.PrintToDebugWindow(printMsg)");
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        lua_tolstring(L, 1, nullptr);
    }
    // the debug window is compiled out of this build
    return 0;
}

}

// ref: FUN_1416a40f0
int Lua_GetSavedAccountName(lua_State* L) {
    PushOptionalString(L, g_accountNameCVar->m_stringValue.GetString());
    return 1;
}

// ref: FUN_1416a4230
int Lua_GetSavedAccountList(lua_State* L) {
    PushOptionalString(L, g_accountListCVar->m_stringValue.GetString());
    return 1;
}

// ref: FUN_1416a4140
int Lua_SetSavedAccountName(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: SetSavedAccountName(\"accountName\")");
        return 0;
    }
    CVarSet(g_accountNameCVar, lua_tolstring(L, 1, nullptr), true, false, false, true);
    return 0;
}

// ref: FUN_1416a4280
int Lua_ClearSavedAccountList(lua_State* L) {
    (void)L;
    CVarSet(g_accountListCVar, "", true, false, false, true);
    return 0;
}

extern "C" unsigned long long FUN_141c191f0(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_141c191f0

// The locales the client offers, as their names: those of the region's (FUN_141c191f0), or every
// locale when the argument asks and the region is 1 or 3. One result per locale; a locale
// without a name is a nil. The decompilation calls this void, but the count of results is what
// it leaves in eax, and the translated copy returned garbage there.
// ref: FUN_141d01d00
int Lua_GetAvailableLocales(lua_State* L) {
    int32_t count = 12;
    uint32_t region = RegionGet();
    bool all = lua_toboolean(L, 1) != 0 && ((region - 1) & 0xfffffffd) == 0;
    uint32_t locales[12];
    FUN_141c191f0(reinterpret_cast<unsigned long long>(locales), reinterpret_cast<unsigned long long>(&count), all);
    for (int32_t i = 0; i < count; i++) {
        const char* name = LocaleGetName(locales[i]);
        if (name == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, name, strlen(name));
        }
    }
    return count;
}

// ref: FUN_141d01f00
int Lua_IsOnGlueScreen(lua_State* L) {
    lua_pushboolean(L, ClientUI_IsGlue());
    return 1;
}

// The reference folds this onto the function that always answers false.
// ref: FUN_14168ea50
int Lua_IsMacClient(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

// ref: FUN_140cec430
int Lua_GetNumCharacters(lua_State* L) {
    // FUN_14168fbf0
    int32_t count = g_numCharacters;
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(count));
    return 1;
}

// ref: FUN_1416a65b0
int Lua_WasScreenFirstDisplayed(lua_State* L) {
    lua_pushboolean(L, g_glueScreenDisplayState == 2);
    return 1;
}

// version, build, date, interface version, localized version label, build type, and the
// interface version again as an integer.
// ref: FUN_141d05b40
int Lua_GetBuildInfo(lua_State* L) {
    lua_pushlstring(L, "3.4.3", 5);
    lua_pushlstring(L, "54261", 5);
    lua_pushlstring(L, "Apr 12 2024", 0xb);
    // FUN_141c95ad0
    lua_pushnumber(L, static_cast<double>(0x76c3));
    const char* version = FrameScript_GetText("VERSION", -1, 0, false);
    PushOptionalString(L, version);
    std::string type;
    // FUN_141db5e70: this is a release build (6)
    const char* typeText = FrameScript_GetText("RELEASE_BUILD", -1, 0, false);
    type += typeText;
    type += " ";
    type += FrameScript_GetText("X64_BUILD", -1, 0, false);
    lua_pushlstring(L, type.c_str(), type.size());
    lua_pushinteger(L, 0x76c3);
    return 7;
}

// ref: FUN_141081ed0
int Lua_GetClientDisplayExpansionLevel(lua_State* L) {
    // FUN_141557f40
    uint32_t level = 2;
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(level));
    return 1;
}

// ref: FUN_1419f5fa0
int Lua_GetGraphicsCVarOffsetForUI(lua_State* L) {
    double offset = lua_tonumber(L, 1);
    lua_pushnumber(L, static_cast<double>(static_cast<int32_t>(static_cast<int64_t>(offset)) + 1));
    return 1;
}

// ref: FUN_141698da0
int Lua_C_CharacterServices_IsLiveRegionCharacterListEnabled(lua_State* L) {
    lua_pushboolean(L, g_liveRegionCharacterList);
    return 1;
}

// ref: FUN_141698dc0
int Lua_C_CharacterServices_IsLiveRegionCharacterCopyEnabled(lua_State* L) {
    lua_pushboolean(L, g_liveRegionCharacterCopy);
    return 1;
}

// ref: FUN_141698de0
int Lua_C_CharacterServices_IsLiveRegionAccountCopyEnabled(lua_State* L) {
    lua_pushboolean(L, g_liveRegionAccountCopy);
    return 1;
}

// ref: FUN_1419f5ef0
int Lua_GetMinRenderScale(lua_State* L) {
    lua_pushnumber(L, 0.3333333432674408);  // DAT_142821330
    return 1;
}

// ref: FUN_1419f5ec0
int Lua_GetMaxRenderScale(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(RenderSettingsMaxScale()));
    return 1;
}

// Whether the device takes a sample count or quality at all (FUN_14228c060 over the caps at
// +0x80 and +0x84).
// ref: FUN_1419f5c90
int Lua_AntiAliasingSupported(lua_State* L) {
    lua_pushboolean(L, 1);
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    // FUN_1405e5ae0: FUN_14228c060(0xb000, 4)
    bool supported = device->Field<uint32_t>(0x80) >= 0xb000 || device->Field<uint8_t>(0x84) >= 4;
    lua_pushboolean(L, supported);
    return 2;
}

// ref: FUN_140fa2560
int Lua_C_Debug_PrintToDebugWindow(lua_State* L) {
    return PrintToDebugWindowBody(L, nullptr);
}

// ref: FUN_14107f0a0
int Lua_C_EventUtils_IsEventValid(lua_State* L) {
    const char* name = nullptr;
    const char* usage = "Usage: local valid = C_EventUtils.IsEventValid(eventName)";
    if (lua_gettop(L) < 1) {
        luaL_error(L, usage);
    } else if (!lua_isstring(L, 1)) {
        luaL_argerror(L, 1, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        name = lua_tolstring(L, 1, nullptr);
    }
    // FUN_141cfc440: FUN_1407bc570 then FUN_140647e10, the three event tables
    uint64_t id = FrameScriptEvents::GetEventID(name, *name == '\0' ? 0 : std::strlen(name));
    bool valid = FrameScriptEvents::IsKnownEvent(id) || FrameScriptEvents::IsKnownEvent(id) ||
        FrameScriptEvents::IsKnownEvent(id);
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushboolean(L, valid);
    return 1;
}

// The launcher's login applies while its command line option (0x20) is there and unused.
// ref: FUN_14169df70
int Lua_C_Login_IsLauncherLogin(lua_State* L) {
    // FUN_14148ba60
    lua_pushboolean(L, CommandLineHasOption(0x20) && g_launcherLoginUsed == 0);
    return 1;
}

// The APIs this machine can run, by name.
// ref: FUN_1419f5bc0
int Lua_GetGraphicsAPIs(lua_State* L) {
    int count = 0;
    for (uint8_t api = 0; api < 5; api++) {
        if (GxApiAvailable(api)) {
            lua_pushstring(L, GxApiName(api));
            count++;
        }
    }
    return count;
}

// ref: FUN_1419f5c10
int Lua_GetCurrentGraphicsAPI(lua_State* L) {
    lua_pushstring(L, GxApiName(GxGetApi()));
    return 1;
}

// No display has a notch on this platform (FUN_1402ca290 answers false).
// ref: FUN_140d42c80
int Lua_C_UI_DoesAnyDisplayHaveNotch(lua_State* L) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushboolean(L, 0);
    return 1;
}

// The ModelFFX frame the character select screen draws the characters in.
// ref: FUN_14168dd10
int Lua_SetCharSelectModelFrame(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: SetCharSelectModelFrame(\"frameName\")");
        return 0;
    }
    const char* name = lua_tolstring(L, 1, nullptr);
    lua_State* context = FrameScript_GetContext();
    lua_pushstring(context, name);
    lua_rawget(context, LUA_GLOBALSINDEX);
    FrameScript_Object* frame =
        FrameScript_Object::GetObjectFromLua(nullptr, context, CSimpleModelFFX::GetObjectType(), -1, false, false);
    lua_settop(context, -2);
    if (frame != nullptr) {
        g_charSelectModelFrame = static_cast<CSimpleModelFFX*>(frame);
    }
    return 0;
}

// ref: FUN_14169d380
int Lua_C_Login_IsReconnectLoginPossible(lua_State* L) {
    // FUN_14148ba50
    lua_pushboolean(L, g_reconnectLoginPossible);
    return 1;
}

// For each sample count and quality the device takes: "<samples>,<quality>" and the colour
// and coverage sample counts.
// ref: FUN_1419f5cc0
int Lua_MultiSampleAntiAliasingSupported(lua_State* L) {
    if (!lua_checkstack(L, 0x2d)) {
        return 0;
    }
    int count = 0;
    for (uint32_t samples = 1; samples < 6; samples++) {
        for (uint32_t quality = 0; quality < 4; quality++) {
            if (RenderSettingsCheckMsaa(static_cast<uint8_t>(samples), static_cast<uint8_t>(quality)) == 0) {
                lua_pushfstring(L, "%d,%d", samples, quality);
                lua_pushnumber(L, static_cast<double>(1ull << (samples & 0x3f)));
                lua_pushnumber(L, static_cast<double>(1ull << ((quality + samples) & 0x3f)));
                count++;
            }
        }
    }
    return count * 3;
}

// For each of a quality group's settings, why this machine cannot take it (0 when it can).
// ref: FUN_1419f55b0
int Lua_GetCVarSettingValidity(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isnumber(L, 2)) {
        luaL_error(L, "Usage: GetCVarSettingValidity(cvar, settingsCount [, isRaid])");
        return 0;
    }
    const char* name = lua_tolstring(L, 1, nullptr);
    double count = lua_tonumber(L, 2);
    bool raid = lua_toboolean(L, 3) != 0;
    uint32_t settings = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int64_t>(count)));
    uint32_t setting = 0;
    do {
        lua_pushnumber(L, static_cast<double>(GraphicsSettingValidity(name, static_cast<int32_t>(setting), raid)));
        setting++;
    } while (setting <= settings - 1);
    return static_cast<int>(settings);
}

// The CVar values of the quality level the machine was detected at: the lowest of its CPU,
// GPU and memory tiers.
// ref: FUN_1419f5820
int Lua_GetDefaultVideoOptions(lua_State* L) {
    bool raid = lua_toboolean(L, 1) != 0;
    const int8_t* tiers = &ConsoleGetDetected()->m_cpu;
    if (tiers[1] < tiers[0]) {
        tiers = tiers + (tiers[2] < tiers[1] ? 1 : 0) + 1;
    } else if (tiers[2] < tiers[0]) {
        tiers = tiers + 2;
    }
    const GRAPHICSLEVELVALUE* entry = GraphicsLevelValues(static_cast<uint32_t>(*tiers), raid);
    lua_checkstack(L, 0x46);
    int count = 0;
    for (; entry->m_name != nullptr; entry++) {
        lua_pushstring(L, entry->m_name);
        lua_pushnumber(L, static_cast<double>(entry->m_value));
        count += 2;
    }
    return count;
}

// The default UI scale: 768 / the window height for a landscape window taller than 768
// lines, 1 otherwise.
// ref: FUN_141d05500
int Lua_GetDefaultScale(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(WindowGetDefaultUiScale()));
    return 1;
}

// The high resolution background when its model is ready, the low one otherwise.
// ref: FUN_1416a4df0
int Lua_GetLoginScreenBackground(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) {
        luaL_error(L, "Usage: GetLoginScreenBackground(\"highresbg\", \"lowresbg\")");
        return 0;
    }
    const char* high = lua_tolstring(L, 1, nullptr);
    const char* low = lua_tolstring(L, 2, nullptr);
    const char* background = WorldSceneModelReady(nullptr, high, 3) ? high : low;
    if (background == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, background);
    return 1;
}

namespace {

// What GetExpansionDisplayInfo returns: the logo and banner files and the feature list (none
// are filled by the lookup).
struct EXPANSIONDISPLAYINFO {
    int32_t m_logo;
    int32_t m_banner;
};

// The UIExpansionDisplayInfo row of an expansion and release type: its logo (field 0).
// ref: FUN_141c28d00
bool ExpansionDisplayInfo(EXPANSIONDISPLAYINFO* info, int32_t expansion, int32_t releaseType) {
    WowDB2Lookup* lookup = g_uIExpansionDisplayInfoDB.m_lookup;
    if (lookup == nullptr) {
        return false;
    }
    DB2LOOKUPITER it;
    WowDB2LookupBegin(lookup, &it);
    while (it.m_index != -2) {
        auto record = static_cast<const uint8_t*>(it.m_record);
        // FUN_14051c400
        if (static_cast<int32_t>(DB2FieldU32(record, 2, 0, &g_uIExpansionDisplayInfoDB)) == expansion
            && static_cast<int32_t>(DB2FieldU32(record, 3, 0, &g_uIExpansionDisplayInfoDB)) == releaseType) {
            info->m_logo = DB2FieldI32(record, 0, 0, &g_uIExpansionDisplayInfoDB);
            info->m_banner = 0;
            return true;
        }
        lookup->Next(&it);
    }
    return false;
}

// A file id, or nil for none (FUN_140aefd80).
void PushOptionalFile(lua_State* L, int32_t file) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    }
    if (file != 0) {
        lua_pushnumber(L, static_cast<double>(file));
    } else {
        lua_pushnil(L);
    }
}

// ref: FUN_140fcb9a0
int32_t GetExpansionDisplayInfoBody(lua_State* L, void*) {
    const char* usage = "Usage: local info = GetExpansionDisplayInfo(expansionLevel, desiredReleaseType)";
    int32_t args[2] = {0, 0};
    int32_t index = 1;
    for (int32_t i = 0; i < 2; i++) {
        if (lua_type(L, index) < 1) {
            luaL_error(L, usage);
        } else if (!lua_isnumber(L, index)) {
            luaL_argerror(L, index, usage);
        } else if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else {
            args[i] = static_cast<int32_t>(static_cast<int64_t>(lua_tonumber(L, index)));
            index++;
        }
    }
    EXPANSIONDISPLAYINFO info = {};
    if (!ExpansionDisplayInfo(&info, args[0], args[1])) {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
            return 0;
        }
        lua_pushnil(L);
        return 1;
    }
    // FUN_1407d0e40
    lua_createtable(L, 0, 3);
    int32_t table = lua_gettop(L);
    PushOptionalFile(L, info.m_logo);
    lua_setfield(L, table, "logo");
    PushOptionalFile(L, info.m_banner);
    lua_setfield(L, table, "banner");
    // FUN_140fb6bf0: the features, none
    lua_createtable(L, 0, 0);
    lua_setfield(L, table, "features");
    return 1;
}

}  // namespace

// ref: FUN_141082560
int Lua_GetExpansionDisplayInfo(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetExpansionDisplayInfoBody, nullptr);
}
