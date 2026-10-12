#include "glue/GlueMgr.hpp"
#include "ui/CSimpleModelFFX.hpp"
#include "framescript/EventCallbacks.hpp"
#include "db/DB2Tables.hpp"
#include "db/DBClient.hpp"
#include "db/DynamicLifeTimeDB.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include <cctype>
#include <cstring>
#include "console/Command.hpp"
#include "glue/GlueScriptAPI.hpp"
#include "glue/ScriptAPIRegistry.hpp"

#include <cstdint>

#include "base/Coordinate.hpp"
#include "client/GxWindow.hpp"
#include "framescript/FrameScript.hpp"
#include "os/Timer.hpp"
#include "storm/String.hpp"
#include "storm/Memory.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "client/Locale.hpp"
#include "ui/CGTooltip.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lua.h"
}

namespace {

const char s_glueFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Classic\\Source\\Glue\\CGlueMgr.cpp";

// CGlueMgr's state (0x14311c510..).
uint32_t s_windowWidth;         // 0x14311c510
uint32_t s_windowHeight;        // 0x14311c514
float s_aspect;                 // 0x14311c518
void* s_uiRoot;                 // 0x14311c520 the glue's CSimpleTop
bool s_initialized;             // 0x14311c51e
bool s_initializing;            // 0x14311c51f
bool s_destroying;              // 0x14311c528 set by CGlueMgr::Destroy (FUN_14167c3c0)
bool s_reloadPending;           // 0x14311c529 a UI reload asked for after an aspect change
bool s_unknown52b;              // 0x14311c52b
bool s_unknown52c;              // 0x14311c52c
uint32_t s_startTime;           // 0x14311c530
uint64_t s_unknown5f0;          // 0x14311c5f0

// The GlueParent frame (the glue's top frame), looked up in the globals.
// ref: FUN_14167b2c0
CSimpleFrame* GetGlueParent() {
    lua_State* L = FrameScript_GetContext();
    lua_pushstring(L, "GlueParent");
    lua_rawget(L, LUA_GLOBALSINDEX);
    auto parent = static_cast<CSimpleFrame*>(
        FrameScript_Object::GetObjectFromLua(nullptr, L, CSimpleFrame::GetObjectType(), -1, false, false));
    lua_settop(L, -2);
    return parent;
}

// ref: FUN_141174300
void Sub141174300() {
    FROST_UNIMPLEMENTED();
}

// Releases textures the UI no longer uses (CSimpleTop.cpp, "WUNNEEDEDTEXTURE").
// ref: FUN_1405eef20
void TopReleaseUnneededTextures() {
    FROST_UNIMPLEMENTED();
}

// The glue-only frame types.

// ref: FUN_141697530
CSimpleFrame* CreateModelFFX(CSimpleFrame* parent) {
    if (!ClientUI_IsGlue()) {
        return nullptr;
    }
    return new CSimpleModelFFX(parent);
}

// ref: FUN_14167b1d0
CSimpleFrame* CreatePlayerModel(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14167b220
CSimpleFrame* CreateGameTooltip(CSimpleFrame* parent) {
    return new CGTooltip(parent);
}

// ref: FUN_14167aee0
CSimpleFrame* CreateModelScene(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14167ae90
CSimpleFrame* CreateCheckout(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// The kiosk settings block, and its CVars.
// ref: FUN_14192fa70
void* GetKioskSettings() {
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14192fac0
void KioskRegisterCVars(void* settings) {
    (void)settings;
    FROST_UNIMPLEMENTED();
}

// Script-function implementations the glue installs into the client's function slots.

// ref: FUN_141b4a6e0
void Sub141B4A6E0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141696aa0
void Sub141696AA0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14169bf50
void Sub14169BF50() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141a255a0
void Sub141A255A0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141a3cc80
void Sub141A3CC80() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1419ee9b0
void Sub1419EE9B0() {
    FROST_UNIMPLEMENTED();
}

// The account data log and the synchronizeSettings CVar.
// ref: FUN_14156e300
void Sub14156E300() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1416a7bb0
void Sub1416A7BB0() {
    FROST_UNIMPLEMENTED();
}

// Walks the glue model-info list (0x142e3fdc0) and preloads each entry.
void PreloadGlueModels() {
    FROST_UNIMPLEMENTED();
}

}

// Whether the UI up is the glue's: it is initialized and not going away, or (with no game
// UI, whose test FUN_141925900 is) still initializing.
// ref: FUN_14196fe80
bool ClientUI_IsGlue() {
    if (s_initialized && !s_destroying) {
        return true;
    }
    return s_initializing;
}

// The game UI (CGGameUI, 0x1431e7d26 and FUN_141925900) does not exist before the world, so
// only the glue's state decides.
// ref: FUN_14196fed0
bool ClientUI_IsTearingDown() {
    if (!s_initialized || s_destroying) {
        return true;
    }
    if (!s_reloadPending && s_initialized && !s_destroying && !s_initializing) {
        return false;
    }
    return true;
}

namespace {

// ---- CGlueMgr::Initialize's callees -----------------------------------------------------

// The enableSourceLocationLookup CVar (source file names for script positions).
// ref: FUN_141b4b720
void Sub141B4B720() {
    FROST_UNIMPLEMENTED();
}

// The AddOns directory scan (Interface/AddOns/).
// ref: FUN_141c99c50
void Sub141C99C50() {
    FROST_UNIMPLEMENTED();
}

// The timers whose time has come run (C_Timer's queue, 0x1432521a8): the UI root's
// pre-update hook.
// ref: FUN_141cf9a60
void UiTimersRun() {
    FROST_UNIMPLEMENTED();
}

// The glue follows a new window size: the GlueParent rescales (the root's +0xf38 hook).
// ref: FUN_14167b3a0
void GlueOnWindowSize() {
    FROST_UNIMPLEMENTED();
}

// The cursor follows the cursorSizePreferred CVar (the root's +0xf40 hook).
// ref: FUN_14167b330
bool GlueOnCursorSize() {
    FROST_UNIMPLEMENTED();
    return true;
}

// A gamepad event reaches the glue's gamepad handling (the root's +0xf58 hook).
// ref: FUN_14167b4b0
void GlueOnGamePad() {
    FROST_UNIMPLEMENTED();
}

// Whether the cursor textures are loaded, and loading them (Interface\Cursor\*.blp).
// ref: FUN_141cb2810
bool CursorIsLoaded() {
    FROST_UNIMPLEMENTED();
    return true;
}

// ref: FUN_141cb1440
void CursorLoad() {
    FROST_UNIMPLEMENTED();
}

// The cursorSizePreferred CVar.
// ref: FUN_14167b4d0
void Sub14167B4D0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141cb20e0
void CursorSetMode(uint32_t mode) {
    (void)mode;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141527450
void Sub141527450(uint32_t value) {
    (void)value;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141cfe3e0
void Sub141CFE3E0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141b46e90
void Sub141B46E90() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141695dd0
void Sub141695DD0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141695d80
void Sub141695D80() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141c9a070
void Sub141C9A070(uint32_t value) {
    (void)value;
    FROST_UNIMPLEMENTED();
}

DynamicLifeTimeDB s_globalStrings(&g_globalStringsDB);  // DAT_1431e9ee8

// The C-style escapes a global string's text carries: \n (and a backslash before a line break)
// is a newline, \a \b \f \r \t \v their controls, up to three decimal digits a byte, and any
// other character itself. False, with `out` emptied, when the text does not fit or a byte
// is 0xff or above.
// ref: FUN_141d06830
bool GlobalStringUnescape(char* out, uint32_t size, const char* text) {
    uint32_t length = static_cast<uint32_t>(std::strlen(text));
    if (length >= size) {
        out[0] = '\0';
        return false;
    }
    int32_t written = 0;
    uint32_t i = 0;
    while (i < length) {
        uint32_t c = static_cast<uint8_t>(text[i]);
        if (c == 0xff) {
            out[0] = '\0';
            return false;
        }
        if (c != '\n' && c != '\r' && c == '\\') {
            i++;
            uint8_t next = static_cast<uint8_t>(text[i]);
            switch (next) {
            case '\n':
            case '\r':
            case 'n':
                c = '\n';
                break;
            case 'a':
                c = 7;
                break;
            case 'b':
                c = 8;
                break;
            case 'f':
                c = 0xc;
                break;
            case 'r':
                c = 0xd;
                break;
            case 't':
                c = 9;
                break;
            case 'v':
                c = 0xb;
                break;
            default:
                if (!std::isdigit(next)) {
                    c = next;
                } else {
                    // The reference leaves `i` on the character after the digits, which the step
                    // below then skips.
                    int32_t digits = 0;
                    c = 0;
                    do {
                        c = c * 10 + (static_cast<uint8_t>(text[i]) - '0');
                        i++;
                        digits++;
                    } while (digits <= 2 && std::isdigit(static_cast<uint8_t>(text[i])));
                    if (static_cast<int32_t>(c) > 0xff) {
                        out[0] = '\0';
                        return false;
                    }
                }
                break;
            }
        }
        out[written] = static_cast<char>(c);
        written++;
        i++;
    }
    out[written] = '\0';
    return true;
}

}

void Sub141970330();

// Sets every GlobalStrings row whose flags include all of `flags` as a script global, its tag
// the name and its unescaped text the value. Loads the table first when its file is on disk,
// and asks for the file when it is not.
// ref: FUN_14196ff50
void Sub14196FF50(uint32_t flags) {
    s_globalStrings.AcquireIfUnloaded();

    lua_State* L = FrameScript_GetContext();
    WowDB2Lookup* lookup = g_globalStringsDB.m_lookup;
    if (lookup != nullptr) {
        static char s_text[4000];  // the reference's is on its stack
        DB2LOOKUPITER it;
        WowDB2LookupBegin(lookup, &it);
        while (it.m_index != -2) {
            auto record = static_cast<const int32_t*>(it.m_record);
            uint8_t rowFlags = DB2FieldU8(reinterpret_cast<const uint8_t*>(record + 2), 2, 8, &g_globalStringsDB);
            if ((rowFlags & flags) == flags) {
                const char* text = record[1] != 0
                    ? reinterpret_cast<const char*>(record) + record[1] + 4 : "";
                if (GlobalStringUnescape(s_text, sizeof(s_text), text)) {
                    lua_pushstring(L, s_text);
                    const char* tag = record[0] != 0
                        ? reinterpret_cast<const char*>(record) + record[0] : "";
                    lua_pushstring(L, tag);
                    lua_insert(L, -2);
                    lua_rawset(L, LUA_GLOBALSINDEX);
                }
            }
            lookup->Next(&it);
        }
    }

    Sub141970330();
}

// Drops this caller's hold on the GlobalStrings table, unloading it after the last.
// ref: FUN_141970330
void Sub141970330() {
    s_globalStrings.ReleaseIfLoaded();
}

namespace {

// Glue script globals and API tables, in registration order.

// ref: FUN_1416a4070
void Sub1416A4070() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14063d240
void Sub14063D240() {
    FROST_UNIMPLEMENTED();
}

// The FullUserdataObject metatable.
// ref: FUN_1424cd500
void Sub1424CD500() {
    FROST_UNIMPLEMENTED();
}

// LuaFunctionContainer and C_FunctionContainers.
// ref: FUN_141c43e60
void Sub141C43E60() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141a49240
void Sub141A49240() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141a49200
void Sub141A49200() {
    FROST_UNIMPLEMENTED();
}

namespace {

// The interface files written out of the archives ('art' and/or 'code'). Not ported.
// ref: FUN_141cf3c70
bool InterfaceExportFiles(const char* arguments) {
    (void)arguments;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1416a7810
int32_t CommandExportInterfaceFiles(const char* command, const char* arguments) {
    (void)command;
    if (!InterfaceExportFiles(arguments)) {
        ConsoleWrite(4, "NO FILES EXPORTED! Need to define either 'art' or 'code'");
    }
    return 1;
}

// The region the mouse is over (the last the motion list holds), or nil.
// ref: FUN_141b87860
int Lua_GetMouseFocus(lua_State* L) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_mouseMotion.size() == 0) {
        lua_pushnil(L);
        return 1;
    }
    return FrameScript_Object::PushObject(L, top->m_mouseMotion.back());
}

}  // namespace

// The ExportInterfaceFiles command and the global GetMouseFocus.
// ref: FUN_1416a7840
void Sub1416A7840() {
    ConsoleCommandRegister("ExportInterfaceFiles", CommandExportInterfaceFiles, 5, nullptr, 0);
    lua_State* L = FrameScript_GetContext();
    lua_pushcclosure(L, Lua_GetMouseFocus, 0);
    lua_pushstring(L, "GetMouseFocus");
    lua_insert(L, -2);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

// ref: FUN_141d06200
void SetVisitableUrl(uint32_t index, const char* url) {
    (void)index;
    (void)url;
    FROST_UNIMPLEMENTED();
}

// The glue's frames have loaded: FRAMES_LOADED.
// ref: FUN_1411749f0
void Sub1411749F0() {
    uint8_t args[0x20];
    EventCallbacks_Fire(0x10e8, 0x295344b1926539ccull, args);
}

// ref: FUN_14148c1d0
void Sub14148C1D0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1416a7d00
void Sub1416A7D00() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141b37f60
void Sub141B37F60() {
    FROST_UNIMPLEMENTED();
}

// The name of the glue's starting screen.
// ref: FUN_14156c0a0
const char* Sub14156C0A0() {
    FROST_UNIMPLEMENTED();
    return "";
}

// ref: FUN_1416a3860
void Sub1416A3860(const char* screen) {
    (void)screen;
    FROST_UNIMPLEMENTED();
}

// The glue's world-scene camera (position, look and up vectors).
// ref: FUN_1412d3440
void Sub1412D3440(const float* position, const float* look, const float* up, uint32_t flags) {
    (void)position;
    (void)look;
    (void)up;
    (void)flags;
    FROST_UNIMPLEMENTED();
}

}  // namespace

// ref: FUN_14167c1c0
void GlueMgrStartup() {
    s_initializing = true;
    GXSIZE size;
    WindowGetResolution(&size);
    if (s_windowWidth == 0 || s_windowWidth != size.m_width || s_windowHeight == 0 || s_windowHeight != size.m_height) {
        s_windowWidth = size.m_width;
        s_windowHeight = size.m_height;
        float aspect = WindowGetAspect();
        if (CSimpleFrame* parent = GetGlueParent()) {
            if (parent->SetScale(1.0f, true)) {
                static_cast<CSimpleTop*>(s_uiRoot)->InvalidateLayout();
            }
            CLayoutFrame::ResizePending();
        }
        s_aspect = aspect;
        Sub141174300();
    }
    TopReleaseUnneededTextures();
    FrameXML_RegisterFactory("ModelFFX", CreateModelFFX, false);
    FrameXML_RegisterFactory("PlayerModel", CreatePlayerModel, false);
    FrameXML_RegisterFactory("GameTooltip", CreateGameTooltip, false);
    FrameXML_RegisterFactory("ModelScene", CreateModelScene, false);
    FrameXML_RegisterFactory("Checkout", CreateCheckout, false);
    KioskRegisterCVars(GetKioskSettings());
    Sub141B4A6E0();
    // FUN_14026be80 returns 1 and its result is unused
    Sub141696AA0();
    Sub14169BF50();
    Sub141A255A0();
    Sub141A3CC80();
    Sub1419EE9B0();
    Sub14156E300();
    Sub1416A7BB0();
    PreloadGlueModels();
    s_initialized = true;
    s_initializing = false;
    s_unknown52b = false;
    CGlueMgr::Initialize();
    s_unknown5f0 = 0;
}

// ref: FUN_14167bac0
void CGlueMgr::Initialize() {
    s_initializing = true;
    s_unknown52c = false;
    s_startTime = OsGetAsyncTimeMs();
    GXSIZE size;
    WindowGetResolution(&size);
    s_windowWidth = size.m_width;
    s_windowHeight = size.m_height;
    s_aspect = WindowGetAspect();
    CoordinateSetAspectRatio(&g_coordinateSpace, s_aspect);
    Sub141B4B720();
    Sub141C99C50();
    UiSetDatabaseReady();
    void* memory = SMemAlloc(sizeof(CSimpleTop), 3, s_glueFile, SStrHashFnv1a(s_glueFile), 0x117, 0);
    CSimpleTop* top = new (memory) CSimpleTop(UiTimersRun);
    s_uiRoot = top;
    top->m_unknownF38 = reinterpret_cast<void*>(GlueOnWindowSize);
    top->m_keyFilterHook = reinterpret_cast<void*>(GlueOnCursorSize);
    top->m_padHook = reinterpret_cast<void*>(GlueOnGamePad);
    if (!CursorIsLoaded()) {
        CursorLoad();
    }
    Sub14167B4D0();
    CursorSetMode(1);
    Sub141527450(1);
    Sub141CFE3E0();
    Sub141B46E90();
    Sub141695DD0();
    Sub141695D80();
    Sub141C9A070(0);
    FrameScript_Reinitialize(false);
    Sub14196FF50(2);
    if (LocaleGetText() == 4) {
        // the CONVERT_TITLE / CONVERT_DESCRIPTION / CONVERT_LEFTOVER_MINUTES globals
        FROST_UNIMPLEMENTED();
    }
    Sub1416A4070();
    Sub14063D240();
    Sub14191C2A0();
    Sub141DB0F60();
    Sub1424DB660();
    Sub141D06170();
    Sub1424CD500();
    Sub141C43E60();
    CGTooltip::CreateScriptMetaTables();
    Sub1416A70B0();
    Sub14168ED20();
    Sub14169C190();
    Sub14169C220();
    Sub14130C2E0();
    Sub1419F63A0();
    Sub141B47390();
    Sub141A49240();
    Sub141A49200();
    Sub141B65C50();
    Sub1416A0D20();
    Sub1416A8880();
    Sub141DB1990();
    GlueRegisterScriptAPI(FrameScript_GetContext());
    FrameXML_ClearScopedMask();
    // FUN_1402146d0: a blz string at 0x142617f88 reset
    CScriptStatus status("Logs\\GlueXML.log", false, "Interface\\GlueXML\\GlueXML_Wrath.toc");
    FrameXML_FreeVirtuals();
    FrameXML_FreeIntrinsics();
    Sub1416A7840();
    // the alphabet font families take their settings from (0x142e99c24)
    switch (LocaleGetText()) {
    case 1:
        CSimpleFont::s_alphabet = CSimpleFont::ALPHABET_KOREAN;
        break;
    case 4:
        CSimpleFont::s_alphabet = CSimpleFont::ALPHABET_SIMPLIFIED_CHINESE;
        break;
    case 5:
        CSimpleFont::s_alphabet = CSimpleFont::ALPHABET_TRADITIONAL_CHINESE;
        break;
    case 8:
        CSimpleFont::s_alphabet = CSimpleFont::ALPHABET_RUSSIAN;
        break;
    default:
        break;
    }
    FrameXML_LoadToc("Interface\\GlueXML\\GlueXML_Wrath.toc", nullptr, &status, false, 7);
    SetVisitableUrl(0, FrameScript_GetText("VISITABLE_URL_GENERIC", -1, 0));
    for (uint32_t i = 1; i < 0x3D; i++) {
        char name[0x100];
        SStrPrintf(name, 0x100, "%s%d", "VISITABLE_URL", i);
        const char* url = FrameScript_GetText(name, -1, 0);
        if (url == nullptr || *url == '\0') {
            url = FrameScript_GetText("VISITABLE_URL_GENERIC", -1, 0);
        }
        SetVisitableUrl(i, url);
    }
    Sub1411749F0();
    // nine script-function slots get the glue's implementations (0x14309dc70..)
    Sub14148C1D0();
    Sub1416A7D00();
    Sub141B37F60();
    Sub1416A3860(FrameScript_GetText(Sub14156C0A0(), -1, 0));
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float look[3] = { 1.0f, 0.0f, 0.0f };
    float up[3] = { 0.0f, 1.0f, 0.0f };
    Sub1412D3440(position, look, up, 0);
    s_unknown52b = false;
    s_initializing = false;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_14311c51e = (void*)&s_initialized;
extern "C" void* const gen_p_14311c528 = (void*)&s_destroying;
extern "C" void* const gen_p_14311c529 = (void*)&s_reloadPending;
extern "C" void* const gen_p_14311c52b = (void*)&s_unknown52b;
extern "C" void* const gen_p_14311c5f0 = (void*)&s_unknown5f0;
// ---- end of generated globals ----
