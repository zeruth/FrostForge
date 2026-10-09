#include "glue/GlueMgr.hpp"

#include <cstdint>

#include "base/Coordinate.hpp"
#include "client/GxWindow.hpp"
#include "framescript/FrameScript.hpp"
#include "os/Timer.hpp"
#include "storm/String.hpp"
#include "ui/FrameXML.hpp"
#include "util/Unimplemented.hpp"

namespace {

// CGlueMgr's state (0x14311c510..).
uint32_t s_windowWidth;         // 0x14311c510
uint32_t s_windowHeight;        // 0x14311c514
float s_aspect;                 // 0x14311c518
void* s_uiRoot;                 // 0x14311c520 the glue's CSimpleTop
bool s_unknown51e;              // 0x14311c51e
bool s_initializing;            // 0x14311c51f
bool s_unknown52b;              // 0x14311c52b
bool s_unknown52c;              // 0x14311c52c
uint32_t s_startTime;           // 0x14311c530
uint64_t s_unknown5f0;          // 0x14311c5f0

// The window's aspect ratio from the last size set (0x142d08178 / 0x142d0817a), else 1.
// ref: FUN_1414fcc60
float WindowGetAspect() {
    FROST_UNIMPLEMENTED();
    return 1.0f;
}

// The GlueParent frame (the glue's top frame), looked up in the globals.
// ref: FUN_14167b2c0
void* GetGlueParent() {
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// CSimpleFrame::SetScale: false (and an error) for a scale <= 0.
// ref: FUN_14061af60
bool FrameSetScale(void* frame, float scale, bool force) {
    (void)frame;
    (void)scale;
    (void)force;
    FROST_UNIMPLEMENTED();
    return false;
}

// Marks the UI root's layout dirty (+0x1024).
// ref: FUN_1405efb60
void TopInvalidateLayout(void* top) {
    (void)top;
    FROST_UNIMPLEMENTED();
}

// Resolves every pending layout frame.
// ref: FUN_1405f9890
void LayoutFlush() {
    FROST_UNIMPLEMENTED();
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
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14167b1d0
CSimpleFrame* CreatePlayerModel(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14167b220
CSimpleFrame* CreateGameTooltip(CSimpleFrame* parent) {
    (void)parent;
    FROST_UNIMPLEMENTED();
    return nullptr;
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

// ref: FUN_1406031e0
void Sub1406031E0() {
    FROST_UNIMPLEMENTED();
}

// CSimpleTop's constructor (0x1070 bytes; CSimpleTop.cpp), with the frame-render callback.
// ref: FUN_1405ebe10
void* CSimpleTopConstruct(void* memory) {
    (void)memory;
    FROST_UNIMPLEMENTED();
    return nullptr;
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

// ref: FUN_14196ff50
void Sub14196FF50(uint32_t value) {
    (void)value;
    FROST_UNIMPLEMENTED();
}

// The account's expansion level as the glue sees it.
// ref: FUN_1405d5790
int32_t GetAccountExpansion() {
    FROST_UNIMPLEMENTED();
    return 0;
}

// Glue script globals and API tables, in registration order.

// ref: FUN_1416a4070
void Sub1416A4070() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14063d240
void Sub14063D240() {
    FROST_UNIMPLEMENTED();
}

// The LE_* enum constants.
// ref: FUN_14191c2a0
void Sub14191C2A0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141db0f60
void Sub141DB0F60() {
    FROST_UNIMPLEMENTED();
}

// GetText and the other localized-string functions.
// ref: FUN_1424db660
void Sub1424DB660() {
    FROST_UNIMPLEMENTED();
}

// GetBuildInfo and the other general glue functions.
// ref: FUN_141d06170
void Sub141D06170() {
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

// ref: FUN_141cd13c0
void Sub141CD13C0() {
    FROST_UNIMPLEMENTED();
}

// GetLocale and the other glue functions.
// ref: FUN_1416a70b0
void Sub1416A70B0() {
    FROST_UNIMPLEMENTED();
}

// The character-select functions (SetCharSelectModelFrame, ...).
// ref: FUN_14168ed20
void Sub14168ED20() {
    FROST_UNIMPLEMENTED();
}

// C_CharacterServices.
// ref: FUN_14169c190
void Sub14169C190() {
    FROST_UNIMPLEMENTED();
}

// C_SharedCharacterServices / C_CharacterServicesPublic.
// ref: FUN_14169c220
void Sub14169C220() {
    FROST_UNIMPLEMENTED();
}

// PlaySound and the other sound functions.
// ref: FUN_14130c2e0
void Sub14130C2E0() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1419f63a0
void Sub1419F63A0() {
    FROST_UNIMPLEMENTED();
}

// C_AuthChallenge.
// ref: FUN_141b47390
void Sub141B47390() {
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

// C_WowTokenSecure / C_WowTokenPublic / C_WowTokenGlue.
// ref: FUN_141b65c50
void Sub141B65C50() {
    FROST_UNIMPLEMENTED();
}

// C_Login.
// ref: FUN_1416a0d20
void Sub1416A0D20() {
    FROST_UNIMPLEMENTED();
}

// C_RealmList.
// ref: FUN_1416a8880
void Sub1416A8880() {
    FROST_UNIMPLEMENTED();
}

// C_Widget.
// ref: FUN_141db1990
void Sub141DB1990() {
    FROST_UNIMPLEMENTED();
}

// C_Seasons and the other generated API namespaces.
// ref: FUN_140afd040
void Sub140AFD040(lua_State* L) {
    (void)L;
    FROST_UNIMPLEMENTED();
}

// ExportInterfaceFiles / GetMouseFocus and the glue's protected functions.
// ref: FUN_1416a7840
void Sub1416A7840() {
    FROST_UNIMPLEMENTED();
}

// A localized string from the Lua string tables (GlobalStrings), with gender variants.
// ref: FUN_140641860
const char* FrameScript_GetText(const char* name, int32_t count, int32_t gender) {
    (void)count;
    (void)gender;
    FROST_UNIMPLEMENTED();
    return name;
}

// ref: FUN_141d06200
void SetVisitableUrl(uint32_t index, const char* url) {
    (void)index;
    (void)url;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1411749f0
void Sub1411749F0() {
    FROST_UNIMPLEMENTED();
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
        if (void* parent = GetGlueParent()) {
            if (FrameSetScale(parent, 1.0f, true)) {
                TopInvalidateLayout(s_uiRoot);
            }
            LayoutFlush();
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
    s_unknown51e = true;
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
    Sub1406031E0();
    s_uiRoot = CSimpleTopConstruct(nullptr);
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
    if (GetAccountExpansion() == 4) {
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
    Sub141CD13C0();
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
    Sub140AFD040(FrameScript_GetContext());
    FrameXML_ClearScopedMask();
    // FUN_1402146d0: a blz string at 0x142617f88 reset
    CScriptStatus status("Logs\\GlueXML.log", false, "Interface\\GlueXML\\GlueXML_Wrath.toc");
    FrameXML_FreeVirtuals();
    FrameXML_FreeIntrinsics();
    Sub1416A7840();
    // the expansion level as FrameXML's version selector (0x142e99c24)
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
