#include "glue/GlueMgr.hpp"
#include "blz/Function.hpp"
#include "blz/String.hpp"
#include "texture/Texture.hpp"
#include "console/Var.hpp"
#include "images/Images.hpp"
#include "util/Fnv.hpp"
#include <cstddef>
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
#include "ui/ModelSceneFrame.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lua.h"
}

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

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
bool s_gxRestarted;             // 0x14311c52a the device came back from a restart
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

extern "C" unsigned long long FUN_141174300();
// translated: FUN_141174300
void Sub141174300() {
    __try {
        FUN_141174300();
    } __except (GenFault(0x141174300ull, GetExceptionInformation())) {
    }
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
    return new ModelSceneFrame(parent);
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

extern "C" unsigned long long FUN_141b4a6e0();
// translated: FUN_141b4a6e0
void Sub141B4A6E0() {
    __try {
        FUN_141b4a6e0();
    } __except (GenFault(0x141b4a6e0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141696aa0();
// translated: FUN_141696aa0
void Sub141696AA0() {
    __try {
        FUN_141696aa0();
    } __except (GenFault(0x141696aa0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14169bf50();
// translated: FUN_14169bf50
void Sub14169BF50() {
    __try {
        FUN_14169bf50();
    } __except (GenFault(0x14169bf50ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141a255a0();
// translated: FUN_141a255a0
void Sub141A255A0() {
    __try {
        FUN_141a255a0();
    } __except (GenFault(0x141a255a0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141a3cc80();
// translated: FUN_141a3cc80
void Sub141A3CC80() {
    __try {
        FUN_141a3cc80();
    } __except (GenFault(0x141a3cc80ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_1419ee9b0();
// translated: FUN_1419ee9b0
void Sub1419EE9B0() {
    __try {
        FUN_1419ee9b0();
    } __except (GenFault(0x1419ee9b0ull, GetExceptionInformation())) {
    }
}

// The account data log and the synchronizeSettings CVar.
extern "C" unsigned long long FUN_14156e300();
// translated: FUN_14156e300
void Sub14156E300() {
    __try {
        FUN_14156e300();
    } __except (GenFault(0x14156e300ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_1416a7bb0();
// translated: FUN_1416a7bb0
void Sub1416A7BB0() {
    __try {
        FUN_1416a7bb0();
    } __except (GenFault(0x1416a7bb0ull, GetExceptionInformation())) {
    }
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
extern "C" unsigned long long FUN_141b4b720();
// translated: FUN_141b4b720
void Sub141B4B720() {
    __try {
        FUN_141b4b720();
    } __except (GenFault(0x141b4b720ull, GetExceptionInformation())) {
    }
}

// The AddOns directory scan (Interface/AddOns/).
extern "C" unsigned long long FUN_141c99c50();
// translated: FUN_141c99c50
void Sub141C99C50() {
    __try {
        FUN_141c99c50();
    } __except (GenFault(0x141c99c50ull, GetExceptionInformation())) {
    }
}

// The timers whose time has come run (C_Timer's queue, 0x1432521a8): the UI root's
// pre-update hook.
extern "C" unsigned long long FUN_141cf9a60();
// translated: FUN_141cf9a60
void UiTimersRun() {
    __try {
        FUN_141cf9a60();
    } __except (GenFault(0x141cf9a60ull, GetExceptionInformation())) {
    }
}

// The glue follows a new window size: the GlueParent rescales (the root's +0xf38 hook).
// ref: FUN_14167b3a0
void GlueOnWindowSize() {
    FROST_UNIMPLEMENTED();
}

// The cursor follows the cursorSizePreferred CVar (the root's +0xf40 hook): the size (-1, or no
// CVar, meaning from the monitor's DPI: the larger size once its scale passes 1.3), then the
// cursor textures rebuilt at it.
// ref: FUN_14167b330
bool GlueOnCursorSize() {
    CVar* cvar = CVarLookup(t2util::FnvHashStringI("ursorSizePreferred", 0xaf63de4c8601eff2ull));
    uint32_t index;
    if (cvar == nullptr) {
        index = 0xffffffff;
    } else {
        index = static_cast<uint32_t>(cvar->m_intValue);
        if (index + 1 > 5) {
            index = (~(static_cast<int32_t>(index + 1) >> 31) & 5u) - 1;
        }
    }
    if (index == 0xffffffff) {
        index = 1.3f < WindowGetDpiScale();
    }
    WindowSetCursorSize(static_cast<int32_t>(index));
    CursorUnload();
    return true;
}

// A gamepad event reaches the glue's gamepad handling (the root's +0xf58 hook).
// ref: FUN_14167b4b0
void GlueOnGamePad() {
    FROST_UNIMPLEMENTED();
}

// The cursor module (Interface\Cursor\*.blp, DAT_1432515e0..): its textures, the mode, and the
// image it hands the window's hardware cursor run as the reference's own code, translated.
}  // namespace
// translated: FUN_141cb2190
extern "C" unsigned long long FUN_141cb1440();
extern "C" unsigned long long FUN_141cb2190();
// The cursor textures' loaded flag stays the image's (the loader is translated).
static uint8_t& s_cursorLoaded = *reinterpret_cast<uint8_t*>(0x1432515edull);
namespace {

// Whether the cursor textures are loaded, and loading them.
// ref: FUN_141cb2810
bool CursorIsLoaded() {
    return s_cursorLoaded != 0;
}

// translated: FUN_141cb1440
void CursorLoad() {
    __try {
        FUN_141cb1440();
    } __except (GenFault(0x141cb1440ull, GetExceptionInformation())) {
    }
}

// The cursor's size from cursorSizePreferred (-1, or no CVar, meaning from the monitor's DPI),
// then the cursor again.
// ref: FUN_14167b4d0
void GlueResetCursorSize() {
    CVar* cvar = CVarLookup(t2util::FnvHashStringI("ursorSizePreferred", 0xaf63de4c8601eff2ull));
    uint32_t index;
    if (cvar == nullptr) {
        index = 0xffffffff;
    } else {
        index = static_cast<uint32_t>(cvar->m_intValue);
        if (index + 1 > 5) {
            index = (~(static_cast<int32_t>(index + 1) >> 31) & 5u) - 1;
        }
    }
    if (index == 0xffffffff) {
        index = 1.3f < WindowGetDpiScale();
    }
    WindowSetCursorSize(static_cast<int32_t>(index));
    CursorSetMode(1);
}

}  // namespace

// The cursor size index (-1 .. 4; out of range goes to -1 or 4), -1 meaning from the monitor's
// DPI: the larger size once its scale passes 1.3.
// ref: FUN_14167f5c0
void CursorSetSize(int32_t index) {
    uint32_t size = static_cast<uint32_t>(index);
    if (size + 1 > 5) {
        size = (~(static_cast<int32_t>(size + 1) >> 31) & 5u) - 1;
    }
    if (size == 0xffffffff) {
        size = 1.3f < WindowGetDpiScale();
    }
    WindowSetCursorSize(static_cast<int32_t>(size));
}

// The cursor slots: 0x48 cursors by two sets, each a CURSORIMAGE and its loaded flag.
struct CURSORSLOT {
    uint32_t m_sizeIndex;
    HTEXTURE m_texture;      // +0x08
    uint8_t* m_pixels;       // +0x10
    uint64_t m_pixelCount;   // +0x18
    uint64_t m_capacity;     // +0x20
    uint8_t m_loaded;        // +0x28
};
static_assert(sizeof(CURSORSLOT) == 0x30, "CURSORSLOT layout");
static CURSORSLOT (&s_cursorSlots)[0x48][2] = *reinterpret_cast<CURSORSLOT(*)[0x48][2]>(0x14324f930ull);
// The three fixed cursors (the default and Item64 among them) and the source image.
static CURSORSLOT (&s_fixedCursors)[3] = *reinterpret_cast<CURSORSLOT(*)[3]>(0x143251430ull);
static MIPBITS*& s_cursorSourceBits = *reinterpret_cast<MIPBITS**>(0x1432515f8ull);
static uint32_t& s_cursorSourceWidth = *reinterpret_cast<uint32_t*>(0x1432515f4ull);
static uint32_t& s_cursorSourceHeight = *reinterpret_cast<uint32_t*>(0x143251600ull);
static uint32_t& s_cursorSource604 = *reinterpret_cast<uint32_t*>(0x143251604ull);

// The cursor textures dropped and loaded again (at a new size), when they are loaded: the
// source image freed, the resize callback taken off, the first set's slots and the fixed
// cursors released, then the load and the source image's cursor again.
// ref: FUN_141cb1cf0
void CursorUnload() {
    if (s_cursorLoaded == 0) {
        return;
    }
    s_cursorLoaded = 0;
    if (s_cursorSourceBits != nullptr) {
        MipBitsFree(&s_cursorSourceBits);
        s_cursorSourceBits = nullptr;
        s_cursorSourceWidth = 0;
        s_cursorSourceHeight = 0;
        s_cursorSource604 = 0;
    }
    WindowRemoveResizeCallback(reinterpret_cast<void (*)(void*)>(0x141cb2b20ull), nullptr);
    // The reference walks both sets and clears only the first (a `set == 0` test in the loop).
    for (uint32_t set = 0; set < 2; set++) {
        if (set != 0) {
            continue;
        }
        for (uint32_t i = 0; i < 0x48; i++) {
            CURSORSLOT& slot = s_cursorSlots[i][set];
            if (slot.m_texture != nullptr) {
                slot.m_texture->m_mipBits = nullptr;
                TextureFree(&slot.m_texture);
            }
            slot.m_sizeIndex = 0;
            slot.m_pixelCount = 0;
            slot.m_loaded = 0;
        }
    }
    // the three fixed cursors, unrolled as the reference has them
    if (s_fixedCursors[0].m_texture != nullptr) {
        s_fixedCursors[0].m_texture->m_mipBits = nullptr;
        TextureFree(&s_fixedCursors[0].m_texture);
    }
    s_fixedCursors[0].m_sizeIndex = 0;
    s_fixedCursors[0].m_pixelCount = 0;
    if (s_fixedCursors[1].m_texture != nullptr) {
        s_fixedCursors[1].m_texture->m_mipBits = nullptr;
        TextureFree(&s_fixedCursors[1].m_texture);
    }
    s_fixedCursors[1].m_sizeIndex = 0;
    s_fixedCursors[1].m_pixelCount = 0;
    if (s_fixedCursors[2].m_texture != nullptr) {
        s_fixedCursors[2].m_texture->m_mipBits = nullptr;
        TextureFree(&s_fixedCursors[2].m_texture);
    }
    s_fixedCursors[2].m_sizeIndex = 0;
    s_fixedCursors[2].m_pixelCount = 0;
    CursorLoad();
    FUN_141cb2190();
}

extern "C" unsigned long long FUN_141cb0380(unsigned long long, unsigned long long, unsigned long long);
// blz::vector<uint8_t>::resize (a template the translator runs)
extern "C" unsigned long long FUN_14078f3c0(unsigned long long, unsigned long long);

namespace {

// A cursor image: its size index, its texture and its RGBA pixels (a blz::vector).
struct CURSORIMAGE {
    uint32_t m_sizeIndex;
    HTEXTURE m_texture;      // +0x08
    uint8_t* m_pixels;       // +0x10
    uint64_t m_pixelCount;   // +0x18
    uint64_t m_capacity;     // +0x20
};
static_assert(offsetof(CURSORIMAGE, m_pixels) == 0x10, "CURSORIMAGE layout");

// The cursor texture's writer: the pixels are tightly packed rows (the blz::function's
// storage holds the pixel pointer).
// ref: LAB_141cb2d60
void CursorFill(uint32_t width, uint32_t height, uint32_t slice, uint32_t mip, uint32_t* pitch, const void** data,
    void* storage) {
    (void)height;
    (void)slice;
    (void)mip;
    *pitch = width * 4;
    *data = *static_cast<void**>(storage);
}

}  // namespace

// Frees the cursor's texture and makes room for width x height pixels, returning them.
// ref: FUN_141cafa20
uint8_t* CursorAllocPixels(uint32_t* cursor, int32_t width, int32_t height) {
    auto image = reinterpret_cast<CURSORIMAGE*>(cursor);
    if (image->m_texture != nullptr) {
        image->m_texture->m_mipBits = nullptr;
        TextureFree(&image->m_texture);
    }
    image->m_sizeIndex = 0;
    image->m_pixelCount = 0;
    FUN_14078f3c0(reinterpret_cast<unsigned long long>(&image->m_pixels), static_cast<unsigned long long>(width * height));
    return image->m_pixels;
}

// The cursor's texture ("Cursor <name>") over its pixels; one that is not a power of two on
// either side is made as type 2.
// ref: FUN_141cb25b0
void CursorCreateTexture(const char* name, uint32_t* cursor, uint32_t width, uint32_t height) {
    auto image = reinterpret_cast<CURSORIMAGE*>(cursor);
    blz::inline_string<0x80> label;
    blz::string_format(label.header(), "Cursor %s", name);
    blz::function_storage fill;
    *reinterpret_cast<uint8_t**>(fill.m_storage) = image->m_pixels;
    uint8_t type = ((width & (width - 1)) == 0 && (height & (height - 1)) == 0) ? 0 : 2;
    fill.m_invoke = reinterpret_cast<void*>(&CursorFill);
    fill.m_manager = blz::function_trivial_manager;
    image->m_texture = TextureCreateUnique(type, width, height, 1, 2, 2, 0x10082001, &fill, label.m_data, true);
}

// The cursor mode (0x47 is not one); the image is rebuilt for it, and the hardware cursor
// redrawn when it is shown.
extern "C" unsigned long long FUN_1414fc2f0();
extern "C" unsigned long long FUN_141cb26d0(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
extern "C" unsigned long long FUN_141cb2b30();
extern "C" unsigned long long FUN_1419272e0();
// The cursor module's state stays the image's (its code is translated), so these read it there;
// no trailing address tags, which would make the translator tie them.
namespace {
uint32_t& s_cursorMode = *reinterpret_cast<uint32_t*>(0x1432515e0ull);
uint32_t& s_cursorSource = *reinterpret_cast<uint32_t*>(0x1432515e4ull);
uint8_t& s_cursorFromSource = *reinterpret_cast<uint8_t*>(0x1432515ecull);
uint32_t& s_cursorState = *reinterpret_cast<uint32_t*>(0x1432515f0ull);
uint8_t& s_cursorShown = *reinterpret_cast<uint8_t*>(0x14311c528ull);
}

// ref: FUN_141cb20e0
void CursorSetMode(uint32_t mode) {
    if (mode == 0x47 || s_cursorMode == mode) {
        return;
    }
    s_cursorFromSource = s_cursorSource == 1;
    if (s_cursorState == 4) {
        s_cursorState = 0;
    }
    s_cursorMode = mode;
    auto size = FUN_1414fc2f0();
    FUN_141cb26d0(mode, s_cursorSource, size & 0xffffffff, 0);
    if ((FUN_141cb2b30() & 0xff) != 0 && s_cursorShown != 0) {
        FUN_1419272e0();
    }
}

// A loaded image's cursor at a size (32, 48, 64, 96 or 128): the rectangle `rect` names (row,
// column, then their ends; the whole image when it is empty or already that size) copied out
// row by row into the cursor's pixels, which are then registered; an image too small for it, or
// none, leaves the cursor cleared at the smallest size. (The decompiler lost the rectangle's
// third value and the clear's pixels argument; the instructions give both.)
// ref: FUN_141cb01c0
bool CursorImageFromMips(const char* name, uint32_t* cursor, uint8_t** mips, uint32_t width, uint32_t height,
    uint32_t sizeIndex, const float* rect) {
    static const int32_t s_sizes[5] = { 0x20, 0x30, 0x40, 0x60, 0x80 };
    if (mips != nullptr) {
        float row = rect[0];
        float column = rect[1];
        float rowEnd = rect[2];
        float columnEnd = rect[3];
        int32_t size = s_sizes[sizeIndex];
        bool whole = rowEnd <= row || columnEnd <= column
            || (static_cast<int32_t>(static_cast<int64_t>(column)) + size == static_cast<int32_t>(static_cast<int64_t>(columnEnd))
                && static_cast<int32_t>(static_cast<int64_t>(row)) + size == static_cast<int32_t>(static_cast<int64_t>(rowEnd)));
        if (whole && static_cast<uint32_t>(static_cast<int64_t>(columnEnd)) <= width
            && static_cast<uint32_t>(static_cast<int64_t>(rowEnd)) <= height) {
            __try {
                uint8_t* out = CursorAllocPixels(cursor, size, size);
                for (int32_t i = 0; i < size; i++) {
                    uint64_t offset = static_cast<uint64_t>((static_cast<int32_t>(static_cast<int64_t>(row)) + i) * width)
                        + static_cast<uint32_t>(static_cast<int64_t>(column));
                    std::memcpy(out, mips[0] + offset * 4, static_cast<size_t>(size) * 4);
                    out += static_cast<size_t>(size) * 4;
                }
                *cursor = sizeIndex;
                CursorCreateTexture(name, cursor, size, size);
            } __except (GenFault(0x141cb01c0ull, GetExceptionInformation())) {
            }
            return true;
        }
    }
    __try {
        FUN_141cb0380(reinterpret_cast<unsigned long long>(name), reinterpret_cast<unsigned long long>(cursor), 0);
    } __except (GenFault(0x141cb0380ull, GetExceptionInformation())) {
    }
    return false;
}

namespace {

// ref: FUN_141527450
void Sub141527450(uint32_t value) {
    (void)value;
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_141cfe3e0();
// translated: FUN_141cfe3e0
void Sub141CFE3E0() {
    __try {
        FUN_141cfe3e0();
    } __except (GenFault(0x141cfe3e0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141b46e90();
// translated: FUN_141b46e90
void Sub141B46E90() {
    __try {
        FUN_141b46e90();
    } __except (GenFault(0x141b46e90ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141695dd0();
// translated: FUN_141695dd0
void Sub141695DD0() {
    __try {
        FUN_141695dd0();
    } __except (GenFault(0x141695dd0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141695d80();
// translated: FUN_141695d80
void Sub141695D80() {
    __try {
        FUN_141695d80();
    } __except (GenFault(0x141695d80ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141c9a070(unsigned long long);
// translated: FUN_141c9a070
void Sub141C9A070(uint32_t value) {
    __try {
        FUN_141c9a070((unsigned long long)value);
    } __except (GenFault(0x141c9a070ull, GetExceptionInformation())) {
    }
}

DynamicLifeTimeDB::STATE s_globalStringsState;  // DAT_1431e9ee8
DynamicLifeTimeDB s_globalStrings(&s_globalStringsState, &g_globalStringsDB);

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

extern "C" unsigned long long FUN_1416a4070();
// translated: FUN_1416a4070
void Sub1416A4070() {
    __try {
        FUN_1416a4070();
    } __except (GenFault(0x1416a4070ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14063d240();
// translated: FUN_14063d240
void Sub14063D240() {
    __try {
        FUN_14063d240();
    } __except (GenFault(0x14063d240ull, GetExceptionInformation())) {
    }
}

// The FullUserdataObject metatable.
extern "C" unsigned long long FUN_1424cd500();
// translated: FUN_1424cd500
void Sub1424CD500() {
    __try {
        FUN_1424cd500();
    } __except (GenFault(0x1424cd500ull, GetExceptionInformation())) {
    }
}

// LuaFunctionContainer and C_FunctionContainers.
// Forwarded with tools/forward_stubs.py --all, the translation faulted inside Lua's string
// table (lstring.c, touching -1) registering these (2026-10-18): a stub again until that is
// understood.
// ref: FUN_141c43e60
void Sub141C43E60() {
    FROST_UNIMPLEMENTED();
}

extern "C" unsigned long long FUN_141a49240();
// translated: FUN_141a49240
void Sub141A49240() {
    __try {
        FUN_141a49240();
    } __except (GenFault(0x141a49240ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141a49200();
// translated: FUN_141a49200
void Sub141A49200() {
    __try {
        FUN_141a49200();
    } __except (GenFault(0x141a49200ull, GetExceptionInformation())) {
    }
}

namespace {

// The interface files written out of the archives ('art' and/or 'code'). Not ported.
extern "C" unsigned long long FUN_141cf3c70(unsigned long long);
// translated: FUN_141cf3c70
bool InterfaceExportFiles(const char* arguments) {
    __try {
        return (FUN_141cf3c70((unsigned long long)arguments) & 0xff) != 0;
    } __except (GenFault(0x141cf3c70ull, GetExceptionInformation())) {
        return false;
    }
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

extern "C" unsigned long long FUN_141d06200(unsigned long long, unsigned long long);
// translated: FUN_141d06200
void SetVisitableUrl(uint32_t index, const char* url) {
    __try {
        FUN_141d06200((unsigned long long)index, (unsigned long long)url);
    } __except (GenFault(0x141d06200ull, GetExceptionInformation())) {
    }
}

// The glue's frames have loaded: FRAMES_LOADED.
// ref: FUN_1411749f0
void Sub1411749F0() {
    uint8_t args[0x20];
    EventCallbacks_Fire(0x10e8, 0x295344b1926539ccull, args);
}

extern "C" unsigned long long FUN_14148c1d0();
// translated: FUN_14148c1d0
void Sub14148C1D0() {
    __try {
        FUN_14148c1d0();
    } __except (GenFault(0x14148c1d0ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_1416a7d00();
// translated: FUN_1416a7d00
void Sub1416A7D00() {
    __try {
        FUN_1416a7d00();
    } __except (GenFault(0x1416a7d00ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_141b37f60();
// translated: FUN_141b37f60
void Sub141B37F60() {
    __try {
        FUN_141b37f60();
    } __except (GenFault(0x141b37f60ull, GetExceptionInformation())) {
    }
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
extern "C" unsigned long long FUN_1412d3440(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_1412d3440
void Sub1412D3440(const float* position, const float* look, const float* up, uint32_t flags) {
    __try {
        FUN_1412d3440((unsigned long long)position, (unsigned long long)look, (unsigned long long)up, (unsigned long long)flags);
    } __except (GenFault(0x1412d3440ull, GetExceptionInformation())) {
    }
}

}  // namespace

void GlueMgrNoteGxRestart() {
    s_gxRestarted = true;
}

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
    UiUnneededTexturesInitialize();
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
    GlueResetCursorSize();
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

extern "C" unsigned long long FUN_1416a0db0(unsigned long long);
// What the Login button starts (Classic/Source/Glue/GlueLogin.cpp): "Glue Start Login", the
// login portal from the portal CVar, the Battle.net login. Not ported: it runs translated
// (docs/login-roadmap.md, Phases 2-3). Translated code calls it; this forwarder makes it a root.
// translated: FUN_1416a0db0
void GlueLoginStart(int32_t a1) {
    __try {
        FUN_1416a0db0(static_cast<uint32_t>(a1));
    } __except (GenFault(0x1416a0db0ull, GetExceptionInformation())) {
    }
}

// The listener GlueLoginStart hands the Battle.net client: a static object (0x142d1b850) whose
// vtable (0x1427e6d00) the image's data already holds. Its slots are GlueLogin.cpp's reactions
// to the logon (the fatal error, "Waiting for server response.", logged on, the error states),
// which set the login state and fire LOGIN_STATE_CHANGED. Nothing calls them by name; they run
// translated, reached through the image's vtable.
// translated: FUN_14169f2e0
// translated: FUN_14169e1c0
// translated: FUN_14169e440
// translated: FUN_14169e560
// translated: FUN_14169e8c0
// translated: FUN_14169e940
// translated: FUN_14169e9d0
// translated: FUN_14169ea60
// translated: FUN_14169eaf0
// translated: FUN_14169eb00
// translated: FUN_14169eb90
// translated: FUN_14169ee60
// translated: FUN_14169ef40
// translated: FUN_14169f2a0
// translated: FUN_14169f2b0

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_14063d240=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14078f3c0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141174300=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412d3440=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14148c1d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414fc2f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156e300=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14167c3c0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141695d80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141695dd0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141696aa0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14169bf50=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1416a0db0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1416a4070=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1416a7bb0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1416a7d00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1419272e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1419ee9b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a255a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a3cc80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a49200=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141a49240=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141b37f60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141b46e90=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141b4a6e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141b4b720=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141c99c50=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141c9a070=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cb0380=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cb1440=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cb2190=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cb26d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cb2b30=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cf3c70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cf9a60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141cfe3e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141d06200=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1424cd500=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_14167b220 = (void*)&CreateGameTooltip;
extern "C" void* const gen_f_14167b2c0 = (void*)&GetGlueParent;
extern "C" void* const gen_f_14167b4d0 = (void*)&GlueResetCursorSize;
extern "C" void* const gen_f_141d06830 = (void*)&GlobalStringUnescape;
extern "C" void* const gen_p_14311c510 = (void*)&s_windowWidth;
extern "C" void* const gen_p_14311c514 = (void*)&s_windowHeight;
extern "C" void* const gen_p_14311c518 = (void*)&s_aspect;
extern "C" void* const gen_p_14311c51e = (void*)&s_initialized;
extern "C" void* const gen_p_14311c51f = (void*)&s_initializing;
extern "C" void* const gen_p_14311c520 = (void*)&s_uiRoot;
extern "C" void* const gen_p_14311c528 = (void*)&s_destroying;
extern "C" void* const gen_p_14311c529 = (void*)&s_reloadPending;
extern "C" void* const gen_p_14311c52a = (void*)&s_gxRestarted;
extern "C" void* const gen_p_14311c52b = (void*)&s_unknown52b;
extern "C" void* const gen_p_14311c52c = (void*)&s_unknown52c;
extern "C" void* const gen_p_14311c530 = (void*)&s_startTime;
extern "C" void* const gen_p_14311c5f0 = (void*)&s_unknown5f0;
extern "C" void* const gen_p_1431e9ee8 = (void*)&s_globalStringsState;
// ---- end of generated globals ----
