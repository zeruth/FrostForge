// The client's own console variables (FUN_14024c220) and the full-screen effect switches
// (FUN_1414725c0), with the callbacks that carry a new value to the system it configures.

#include "client/ClientCVars.hpp"
#include "base/Matrix.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gxu/GxuFont.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "gxu/TextBlock.hpp"
#include "ui/FrameTime.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "framescript/EventCallbacks.hpp"
#include "os/Window.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

CVar* g_accountNameCVar;            // DAT_142de29a0
CVar* g_accountListCVar;            // DAT_142de29b0

namespace {

CVar* s_accountNameDebugToggleCVar;  // DAT_142de29a8
CVar* s_accountUsesTokenCVar;       // DAT_142de29b8
CVar* s_engineSurveyCVar;           // DAT_142de0cb8
CVar* s_engineSurveyPatchCVar;      // DAT_142de0cc0
CVar* s_checkAddonVersionCVar;      // DAT_142de29c0
CVar* s_lastAddonVersionCVar;       // DAT_142de29c8
CVar* s_enableMouseSpeedCVar;       // DAT_142de2a18
CVar* s_mouseSpeedCVar;             // DAT_142de2a20
CVar* s_lastCharacterIndexCVar;     // DAT_142de29d0
CVar* s_lastCharacterGuidCVar;      // DAT_142de29d8
CVar* s_seenCharacterUpgradePopupCVar;  // DAT_142de29e0
CVar* s_seenExpansionTrialPopupCVar;    // DAT_142de29e8
CVar* s_seenLevelSquishPopupCVar;   // DAT_142de29f0
CVar* s_seenConfigurationWarningsCVar;  // DAT_142de29f8
CVar* s_advancedCombatLoggingCVar;  // DAT_142de2a00
CVar* s_skipStartGearCVar;          // DAT_142de2a08
CVar* s_debugGameEventsCVar;        // DAT_142de2a10
CVar* s_enablePingsCVar;            // DAT_142de2a28
CVar* s_showPingsInChatCVar;        // DAT_142de2a30
CVar* s_pingModeCVar;               // DAT_142de2a38

CVar* s_ffxGlowCVar;                // DAT_1430ae8a8
CVar* s_ffxDeathCVar;               // DAT_1430ae8b0
CVar* s_ffxNetherCVar;              // DAT_1430ae8b8
CVar* s_ffxVenariCVar;              // DAT_1430ae8c0
CVar* s_ffxLingeringVenariCVar;     // DAT_1430ae8c8

// The system message display (FUN_141283xxx) and the error display's state.
uint32_t s_messageFilter;           // DAT_142cfbe80: the Errors / showErrors switch
int32_t s_messageLevelMin;          // DAT_1430a5600
int32_t s_messageLevelMax;          // DAT_142cfbe84
uint32_t s_messageCategories;       // DAT_142cfbe88: a bit per filter name
uint8_t s_errorDisplayReady;        // DAT_142de1930
uint8_t s_errorDisplayEnabled;      // DAT_142de1931
uint8_t s_errorDisplayHidden;       // DAT_142de1932
uint8_t s_fullDump;                 // DAT_142e1c528
uint8_t s_screenshotQuality = 3;    // DAT_142cc87b0
uint8_t s_ffxGlow;                  // DAT_142d074e5
uint8_t s_ffxDeath;                 // DAT_142d074e6
uint8_t s_ffxNether;                // DAT_142d074e7

// The filter names, in bit order (PTR_s_general_14261c9a0).
const char* const s_messageFilterNames[10] = {
    "general", "world", "ui", "animation", "models", "objects", "sound", "movement", "network", "spell",
};

// strtol with the reference's "0x" check: hexadecimal when the text starts with 0x or 0X.
long ParseLong(const char* text) {
    int32_t radix = 10;
    if (text != nullptr && text[0] == '0' && ((text[1] + 0xa8u) & 0xdf) == 0) {
        radix = 16;
    }
    return strtol(text, nullptr, radix);
}

// ref: FUN_141283bf0
void MessageSetFilter(uint32_t value) {
    s_messageFilter = value;
}

// ref: FUN_141283d30
void MessageSetLevelMin(int32_t level) {
    s_messageLevelMin = level;
    if (s_messageLevelMax < level) {
        s_messageLevelMax = level;
    }
}

// ref: FUN_141283d10
void MessageSetLevelMax(int32_t level) {
    s_messageLevelMax = level;
    if (level < s_messageLevelMin) {
        s_messageLevelMin = level;
    }
}

// ref: FUN_141283d00
void MessageSetCategories(uint32_t categories) {
    s_messageCategories = categories;
}

// ref: FUN_140238b60
void ErrorDisplayDisable() {
    if (s_errorDisplayEnabled != 0) {
        s_errorDisplayEnabled = 0;
    }
}

// ref: FUN_140238b80
void ErrorDisplayEnable() {
    if (s_errorDisplayEnabled == 0 && s_errorDisplayReady != 0) {
        s_errorDisplayEnabled = 1;
    }
}

// ref: FUN_140238ba0
void ErrorDisplayHide() {
    if (s_errorDisplayHidden == 0) {
        s_errorDisplayHidden = 1;
    }
}

// ref: FUN_1402390f0
void ErrorDisplayShow() {
    if (s_errorDisplayHidden != 0) {
        s_errorDisplayHidden = 0;
    }
}

}  // namespace

// ---- the error display -----------------------------------------------------------------------
//
// The last ten messages drawn over the screen in the debug font, each fading over its last
// three seconds and gone thirteen seconds after it came. (Nothing feeds it in this build: the
// init hands ErrorDisplayAdd to a registration compiled down to an empty function.)

namespace {

// One line (0x118).
struct ERRORLINE {
    uint32_t m_time;                // 0x000 when it came
    char m_text[0x104];             // 0x004 set until the line's string is made
    CGxString* m_string;            // 0x108
    uint32_t m_color;               // 0x110 b, g, r, a
    uint32_t m_pad114;
};
static_assert(sizeof(ERRORLINE) == 0x118, "ERRORLINE is 0x118 bytes");

constexpr int32_t ERROR_LINES = 10;
constexpr float ERROR_LINE_STEP = 0.025f;   // DAT_14261b480
constexpr float ERROR_LINE_DEPTH = -0.45f;  // DAT_14261b488
constexpr float ERROR_ORTHO_FAR = 500.0f;   // DAT_14261a9bc
constexpr float ERROR_COLOR_SCALE = 255.0f; // DAT_14261b484

ERRORLINE s_errorLines[ERROR_LINES];        // DAT_142de0e40
uint8_t s_errorLinesMoved;                  // DAT_142de1933: draw every line at its place again
float s_errorFontHeight;                    // DAT_142de1934
GxuStringBatch* s_errorBatch;               // DAT_142de1938
CGxuFont* s_errorFont;                      // DAT_142de1940
uint32_t s_errorHead;                       // DAT_142de1948: the next line written

}  // namespace

extern "C" unsigned long long FUN_14126ece0(unsigned long long);
extern "C" unsigned long long FUN_14126ed00(unsigned long long);
extern "C" unsigned long long FUN_141283c30(unsigned long long, unsigned long long, unsigned long long,
    unsigned long long);
// translated: FUN_14126ece0
// translated: FUN_14126ed00
// translated: FUN_141283c30

// A message line: its text, its level's colour, at the next place.
// ref: FUN_140238c30
void ErrorDisplayAdd(const char* text, int32_t level) {
    if (s_errorDisplayEnabled == 0) {
        return;
    }
    ERRORLINE& line = s_errorLines[s_errorHead];
    line.m_time = FrameTime_GetTime();
    char* out = line.m_text;
    const char* in = text;
    int32_t left = 0xff;
    for (;;) {
        char c = *in++;
        *out++ = c;
        if (c == '\0') {
            break;
        }
        if (--left == 0) {
            *out = '\0';
            break;
        }
    }
    float r;
    float g;
    float b;
    FUN_141283c30(static_cast<uint32_t>(level), reinterpret_cast<uintptr_t>(&r), reinterpret_cast<uintptr_t>(&g),
        reinterpret_cast<uintptr_t>(&b));
    auto color = reinterpret_cast<uint8_t*>(&line.m_color);
    color[3] = 0xff;
    s_errorLinesMoved = 1;
    color[2] = static_cast<uint8_t>(static_cast<int32_t>(r * ERROR_COLOR_SCALE));
    color[1] = static_cast<uint8_t>(static_cast<int32_t>(g * ERROR_COLOR_SCALE));
    color[0] = static_cast<uint8_t>(static_cast<int32_t>(b * ERROR_COLOR_SCALE));
    s_errorHead = (s_errorHead + 1) % ERROR_LINES;
    ConsoleWrite(static_cast<uint32_t>((level < 2) + 3), text);
}

// The display's batch and the debug font.
// ref: FUN_140238bc0
void ErrorDisplayInitialize() {
    if (s_errorDisplayReady != 0) {
        return;
    }
    s_errorBatch = GxuStringBatchCreate();
    if (s_errorBatch == nullptr) {
        return;
    }
    s_errorFont = TextBlockGetFontPtr(*reinterpret_cast<const HTEXTFONT*>(FUN_14126ece0(1)));
    if (s_errorFont == nullptr) {
        return;
    }
    uint32_t height = static_cast<uint32_t>(FUN_14126ed00(1));
    std::memcpy(&s_errorFontHeight, &height, sizeof(height));
    if (s_errorFontHeight == 0.0f) {
        return;
    }
    // the registration of ErrorDisplayAdd: a call to an empty function (0x1402112e0)
    s_errorDisplayReady = 1;
}

// Once a frame: each line's string made when it is new, faded, moved up when lines came, and
// drawn; lines past their time let go.
// ref: FUN_140238d50
void ErrorDisplayRender() {
    if (s_errorDisplayHidden != 0) {
        return;
    }
    float projection[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    Orthographic(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, ERROR_ORTHO_FAR, projection);
    uint32_t now = FrameTime_GetTime();
    for (uint32_t i = 0; i < ERROR_LINES; i++) {
        ERRORLINE& line = s_errorLines[(s_errorHead + i) % ERROR_LINES];
        float y = 1.0f - static_cast<float>(i + 12) * ERROR_LINE_STEP;
        float position[3] = {0.0f, y, 1.0f};
        bool made = false;
        if (line.m_text[0] != '\0') {
            if (line.m_string != nullptr) {
                GxuFontDestroyString(&line.m_string);
            }
            GxuFontCreateString(s_errorFont, line.m_text, 0.0f, position, 1.0f, s_errorFontHeight, 0.0f,
                &line.m_string, 1, 0, 0x1f44, &line.m_color, ERROR_LINE_DEPTH, 1.0f, 1.0f, 0.0f);
            made = true;
            line.m_text[0] = '\0';
        }
        if (line.m_string == nullptr) {
            continue;
        }
        uint32_t age = now - line.m_time;
        if (age > 13000) {
            GxuFontDestroyString(&line.m_string);
            line.m_string = nullptr;
            continue;
        }
        if (age > 10000) {
            reinterpret_cast<uint8_t*>(&line.m_color)[3] = static_cast<uint8_t>((13000 - age) / 12);
            GxuFontStringSetColor(line.m_string, &line.m_color);
        }
        if (s_errorLinesMoved != 0 && !made) {
            float moved[3] = {0.0f, y, 0.0f};
            line.m_string->SetPosition(moved);
        }
        GxuStringBatchAddString(s_errorBatch, line.m_string);
    }
    float view[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    GxuStringBatchRender(g_gxRenderContext, s_errorBatch, view, projection, nullptr);
    GxuStringBatchClear(s_errorBatch);
    s_errorLinesMoved = 0;
}

namespace {

// "Displaying [only ]<min>[ through <max>]".
// ref: FUN_14024b530
void PrintMessageLevels() {
    int32_t low = s_messageLevelMin;
    int32_t high = s_messageLevelMax;
    if (low == 0 && high == 3) {
        ConsoleWriteText("Displaying all system messages");
        return;
    }
    static const char* const lowNames[4] = { "informational messages", "warnings", "errors", "fatal errors" };
    static const char* const highNames[4] = { nullptr, " through warnings", " through errors", " through fatal errors" };
    char text[80];
    strcpy_s(text, sizeof(text), "Displaying ");
    if (low == high) {
        strcat_s(text, sizeof(text), "only ");
    }
    if (low >= 0 && low < 4) {
        strcat_s(text, sizeof(text), lowNames[low]);
    }
    if (low != high && high >= 1 && high < 4) {
        strcat_s(text, sizeof(text), highNames[high]);
    }
    ConsoleWriteText(text);
}

// "Now filtering: <names>".
// ref: FUN_14024bda0
void PrintMessageFilter() {
    uint32_t categories = s_messageCategories;
    if (categories == 0xffffffff) {
        ConsoleWriteText("Now filtering: all messages");
        return;
    }
    char text[80] = {};
    for (uint32_t i = 0; i < 10; i++) {
        if ((categories & (1u << i)) != 0) {
            strcat_s(text, sizeof(text), s_messageFilterNames[i]);
            strcat_s(text, sizeof(text), " ");
        }
    }
    ConsoleWriteFormat(0, "Now filtering: %s", text);
}

// The CVar change hook: every change outside the hidden and unsaved ones is reported to the
// UI as a CVar update.
// ref: FUN_14024add0
void ClientCVarChanged(CVar* cvar, const char* oldValue, const char* newValue) {
    if ((cvar->m_flags & 0x4040) != 0) {
        return;
    }
    // FUN_140255c60: a debug print compiled out of this build
    // ("CVarChangedHandler: Changed '%s' from '%s' to '%s'\n")
    if (oldValue != nullptr) {
        UiSignalCVarUpdate(cvar->m_name.m_data, newValue);
    }
}

// Shows the account, realm or character in the title bar.
// ref: FUN_14024ab40
bool AccountNameDebugToggleCallback(CVar*, const char*, const char* value, void*) {
    // The login account (FUN_14156b750 +0x771 / FUN_14156bc70), the realm (FUN_14156baa0)
    // and the window title they make are not ported.
    FROST_UNIMPLEMENTED();
    ParseLong(value);
    return true;
}

// ref: FUN_14024b8f0
bool EnableMouseSpeedCallback(CVar*, const char*, const char* value, void*) {
    OsSetUseSystemMouseSpeed(ParseLong(value) == 0);
    return true;
}

// ref: FUN_14024bd60
bool MouseSpeedCallback(CVar*, const char*, const char* value, void*) {
    OsSetMouseSpeed(SStrToFloat(value));
    return true;
}

// ref: FUN_14024b890
bool FullDumpCallback(CVar*, const char*, const char* value, void*) {
    long full = ParseLong(value);
    ConsoleWriteText(full == 0 ? "Full memory crash dump disabled" : "Full memory crash dump enabled");
    // FUN_140259770
    s_fullDump = full != 0;
    return true;
}

// ref: FUN_14024b980
bool ErrorsCallback(CVar*, const char*, const char* value, void*) {
    long errors = ParseLong(value);
    if (errors == 0) {
        ErrorDisplayDisable();
        ConsoleWriteText("Error display disabled");
    } else {
        ErrorDisplayEnable();
        ConsoleWriteText("Error display enabled");
    }
    MessageSetFilter(static_cast<uint32_t>(errors));
    return true;
}

// ref: FUN_14024bc50
bool ShowErrorsCallback(CVar*, const char*, const char* value, void*) {
    long show = ParseLong(value);
    if (show == 0) {
        ErrorDisplayHide();
        ConsoleWriteText("Error display hidden");
    } else {
        ErrorDisplayShow();
        ConsoleWriteText("Error display shown");
    }
    MessageSetFilter(static_cast<uint32_t>(show));
    return true;
}

// ref: FUN_14024bbe0
bool ErrorLevelMinCallback(CVar*, const char*, const char* value, void*) {
    uint32_t level = static_cast<uint32_t>(ParseLong(value));
    if (level < 4) {
        MessageSetLevelMin(static_cast<int32_t>(level));
        PrintMessageLevels();
        return true;
    }
    ConsoleWriteFormat(0, "%i is not valid, valid values are 0 - %i", level, 3);
    return false;
}

// ref: FUN_14024bb70
bool ErrorLevelMaxCallback(CVar*, const char*, const char* value, void*) {
    uint32_t level = static_cast<uint32_t>(ParseLong(value));
    if (level < 4) {
        MessageSetLevelMax(static_cast<int32_t>(level));
        PrintMessageLevels();
        return true;
    }
    ConsoleWriteFormat(0, "%i is not valid, valid values are 0 - %i", level, 3);
    return false;
}

// A list of filter names, "all", and "except" to invert what follows.
// ref: FUN_14024b9f0
bool ErrorFilterCallback(CVar*, const char*, const char* value, void*) {
    const char* cursor = value;
    char token[64];
    bool except = false;
    uint32_t categories = 0;
    SStrTokenize(&cursor, token, sizeof(token), "\t\r\n\" ", nullptr);
    while (token[0] != '\0') {
        bool known = false;
        for (uint32_t i = 0; i < 10; i++) {
            if (_stricmp(token, s_messageFilterNames[i]) == 0) {
                uint32_t bit = 1u << i;
                categories = except ? (categories & ~bit) : (categories | bit);
                known = true;
                break;
            }
        }
        if (!known) {
            if (_stricmp(token, "all") == 0) {
                categories = except ? 0 : 0xffffffff;
            } else if (_stricmp(token, "except") == 0) {
                ConsoleWriteFormat(0, "Unknown filter %s", token);
                ConsoleWriteText("Filters: general world ui animation models objects sound movement all");
                ConsoleWriteText("         use \"except\" to invert mask");
                ConsoleWriteText("         i.e.: all except objects");
                return false;
            } else {
                except = true;
            }
        }
        SStrTokenize(&cursor, token, sizeof(token), "\t\r\n\" ", nullptr);
    }
    MessageSetCategories(categories);
    PrintMessageFilter();
    return true;
}

// ref: LAB_14024a9c0
bool ScreenshotFormatCallback(CVar*, const char*, const char*, void*) {
    return true;
}

// ref: FUN_14024a9d0
bool ScreenshotQualityCallback(CVar*, const char*, const char* value, void*) {
    // FUN_14023a3c0: 1..10, anything else is refused
    uint32_t quality = static_cast<uint32_t>(ParseLong(value));
    if (quality != 0 && quality <= 10) {
        s_screenshotQuality = static_cast<uint8_t>(quality);
    }
    return true;
}

// ref: FUN_14024aa10
bool ScreenshotSizeOverrideCallback(CVar*, const char*, const char* value, void*) {
    uint16_t size[2] = { 0, 0 };
    if (sscanf_s(value, "%hux%hu", &size[0], &size[1]) != 2) {
        size[0] = 0;
        size[1] = 0;
    }
    ClientSetScreenshotSize(size);
    return true;
}

// ref: FUN_14024aa60
bool FrontendMatchingModesCallback(CVar*, const char*, const char*, void*) {
    // FUN_1419701d0: the WowLabs front end's matching modes, not ported
    FROST_UNIMPLEMENTED();
    return true;
}

// ref: FUN_1419050d0
bool AdvancedCombatLoggingCallback(CVar*, const char*, const char* value, void*) {
    // FUN_141915c40: the combat log's version header, not ported
    if (ParseLong(value) != 0) {
        FROST_UNIMPLEMENTED();
    }
    return true;
}

// The key that opens the console: a letter or digit, F<n>, NUMPAD<x>, or a named key.
// ref: FUN_14024ae40
bool ConsoleKeyCallback(CVar*, const char*, const char* value, void*) {
    char first = value[0];
    int32_t key = 0x136;
    if (std::strlen(value) == 1) {
        key = first;
        if (static_cast<uint8_t>(first - '0') > 9 && static_cast<uint8_t>(first - 'A') > 0x19 &&
            static_cast<uint8_t>(first - 'a') < 0x1a) {
            ConsoleSetHotKey(toupper(first));
            return true;
        }
        ConsoleSetHotKey(key);
        return true;
    }
    if (toupper(first) == 'F' && isdigit(static_cast<uint8_t>(value[1]))) {
        ConsoleSetHotKey(ParseLong(value + 1) + 0x122);
        return true;
    }
    if (_strnicmp(value, "NUMPAD", 6) == 0) {
        const char* rest = value + 6;
        while (*rest == ' ') {
            rest++;
        }
        if (isdigit(static_cast<uint8_t>(*rest))) {
            ConsoleSetHotKey(*rest + 0xd0);
            return true;
        }
        struct { const char* name; char symbol; int32_t key; } const pad[] = {
            { "PLUS", '+', 0x10a }, { "MINUS", '-', 0x10b }, { "MULTIPLY", '*', 0x10c },
            { "DIVIDE", '/', 0x10d }, { "DECIMAL", '.', 0x10e }, { "EQUALS", '=', 0x12f },
        };
        for (const auto& entry : pad) {
            if (_stricmp(rest, entry.name) == 0 || *rest == entry.symbol) {
                ConsoleSetHotKey(entry.key);
                return true;
            }
        }
        ConsoleSetHotKey(key);
        return true;
    }
    struct { const char* name; int32_t key; } const named[] = {
        { "ESCAPE", 0x10f }, { "ENTER", 0x110 }, { "BACKSPACE", 0x111 }, { "TAB", 0x112 },
        { "LEFT", 0x113 }, { "UP", 0x114 }, { "RIGHT", 0x115 }, { "DOWN", 0x116 },
        { "INSERT", 0x117 }, { "DELETE", 0x118 }, { "HOME", 0x119 }, { "END", 0x11a },
        { "PAGEUP", 0x11b }, { "PAGEDOWN", 0x11c }, { "NUMLOCK", 0x11e }, { "PRINTSCREEN", 0x121 },
        { "CAPSLOCK", 0x11d },
    };
    for (const auto& entry : named) {
        if (_stricmp(value, entry.name) == 0) {
            ConsoleSetHotKey(entry.key);
            return true;
        }
    }
    return false;
}

// ref: FUN_14024b960
bool RawMouseEnableCallback(CVar*, const char*, const char* value, void*) {
    OsSetRawMouse(atoi(value) != 0);
    return true;
}

// ref: FUN_14024b940
bool RawMouseAccelerationEnableCallback(CVar*, const char*, const char* value, void*) {
    OsSetRawMouseAcceleration(atoi(value) != 0);
    return true;
}

// 125 .. 1000.
// ref: FUN_14024c1a0
bool RawMouseRateCallback(CVar*, const char*, const char* value, void*) {
    int32_t rate = atoi(value);
    if (static_cast<uint32_t>(rate - 0x7d) > 0x36b) {
        rate = rate < 0x7d ? 0x7d : 0x3e8;
    }
    OsSetRawMouseRate(static_cast<uint32_t>(rate));
    return true;
}

// 1 .. 32000.
// ref: FUN_14024c1e0
bool RawMouseResolutionCallback(CVar*, const char*, const char* value, void*) {
    int32_t resolution = atoi(value);
    if (static_cast<uint32_t>(resolution - 1) > 31999) {
        resolution = resolution < 1 ? 1 : 32000;
    }
    OsSetRawMouseResolution(static_cast<uint32_t>(resolution));
    return true;
}

// The reference build's handler is an empty CFG check (_guard_check_icall).
// ref: FUN_14024bd80
bool MouseUseLazyRepositioningCallback(CVar*, const char*, const char* value, void*) {
    (void)atoi(value);
    return true;
}

// ref: FUN_14024bd40
bool MouseNoRepositioningCallback(CVar*, const char*, const char* value, void*) {
    OsSetMouseNoRepositioning(atoi(value) != 0);
    return true;
}

// translated: FUN_14167f5c0
extern "C" unsigned long long FUN_14167f5c0(unsigned long long);
// translated: FUN_141cb1cf0
extern "C" unsigned long long FUN_141cb1cf0();
extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
// -1 .. 4.
// ref: FUN_14024b440
bool CursorSizePreferredCallback(CVar*, const char*, const char* value, void*) {
    long size = ParseLong(value);
    if (static_cast<uint32_t>(size + 1) > 5) {
        size = size < -1 ? -1 : 4;
    }
    // the cursor size (-1 from the monitor's DPI) and the cursor textures rebuilt at it
    __try {
        FUN_14167f5c0(static_cast<unsigned long long>(size));
        FUN_141cb1cf0();
    } __except (GenFault(0x14024b440ull, GetExceptionInformation())) {
    }
    return true;
}

// ref: FUN_14024aa70
bool HardwareCursorCallback(CVar*, const char*, const char* value, void*) {
    ClientSetHardwareCursor(ParseLong(value) == 0);
    return true;
}

// 10 .. 100.
// ref: FUN_14024bcc0
bool MaxFPSLoadingCallback(CVar*, const char*, const char* value, void*) {
    long fps = ParseLong(value);
    if (static_cast<unsigned long>(fps - 10) < 0x5b) {
        return true;
    }
    char text[264];
    SStrPrintf(text, 0x100, "MaxFPSLoading must be between %d and %d", 10, 100);
    ConsoleWriteText(text);
    return false;
}

// ref: FUN_14024b4c0
bool DebugGameEventsCallback(CVar*, const char*, const char*, void*) {
    // FUN_1406d3700 / FUN_1406d3720 build a debug-event message and FUN_14156dc60 sends it to
    // the realm connection (DAT_1430d2b38); not ported.
    FROST_UNIMPLEMENTED();
    return true;
}

// ref: FUN_14024aac0
bool PreventOsIdleSleepCallback(CVar*, const char*, const char* value, void*) {
    // FUN_140267290
    uint32_t mode = ParseLong(value) != 0 ? 3 : 0;
    EXECUTION_STATE state = ES_CONTINUOUS | ((mode & 1) * ES_DISPLAY_REQUIRED);
    if ((mode & 2) != 0) {
        state |= ES_SYSTEM_REQUIRED;
    }
    SetThreadExecutionState(state);
    return true;
}

// ref: FUN_14146b360
bool FfxGlowCallback(CVar*, const char*, const char* value, void*) {
    s_ffxGlow = ParseLong(value) != 0;
    return true;
}

// ref: FUN_14146b300
bool FfxDeathCallback(CVar*, const char*, const char* value, void*) {
    s_ffxDeath = ParseLong(value) != 0;
    // FUN_141476bf0 (the effect rebuilt while one is up, DAT_1430ae8a0) is not ported
    return true;
}

// ref: FUN_14146b3b0
bool FfxNetherCallback(CVar*, const char*, const char* value, void*) {
    s_ffxNether = ParseLong(value) != 0;
    return true;
}

}

// ref: FUN_1414725c0
void FullScreenEffectRegisterCVars() {
    s_ffxGlowCVar = CVarRegister("ffxGlow", "full screen glow effect", 1, "1", FfxGlowCallback, 1, false, nullptr, false);
    s_ffxDeathCVar = CVarRegister("ffxDeath", "full screen death desat effect", 1, "1", FfxDeathCallback, 1, false, nullptr, false);
    s_ffxNetherCVar = CVarRegister("ffxNether", "full screen nether effect", 1, "1", FfxNetherCallback, 1, false, nullptr, false);
    s_ffxVenariCVar = CVarRegister("ffxVenari", "full screen Cloak of Ven'ari effect", 1, "1", nullptr, 1, false, nullptr, false);
    s_ffxLingeringVenariCVar = CVarRegister("ffxLingeringVenari", "full screen Lingering Cloak of Ven'ari effect", 1, "1", nullptr, 1, false, nullptr, false);
}

// ref: FUN_14024c220
void ClientRegisterCVars() {
    CVarSetChangedHandler(ClientCVarChanged);
    CVarRegister("luaErrorExceptions", "Enable exceptions for non-tainted lua errors", 0, "1", nullptr, 4, false, nullptr, false);
    g_accountNameCVar = CVarRegister("accountName", "Saved account name", 0x40, "", nullptr, 4, false, nullptr, false);
    s_accountNameDebugToggleCVar = CVarRegister("accountNameDebugToggle", "Toggle showing the username and server in the title bar. 0 is off, 1 for account name, 2 for realm connected to, 3 for character name, -1 for all", 0x40, "0", AccountNameDebugToggleCallback, 4, false, nullptr, false);
    g_accountListCVar = CVarRegister("accountList", "List of wow accounts for saved Blizzard account", 0x40, "", nullptr, 4, false, nullptr, false);
    s_accountUsesTokenCVar = CVarRegister("g_accountUsesToken", "Saved whether uses authenticator", 0x40, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("playIntroMovie", "Starting expansion movie to play on startup", 0, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("movieSubtitle", "Show movie subtitles", 0, "1", nullptr, 4, false, nullptr, false);
    CVarRegister("showLoadingScreenTips", "Show loading screen tooltips", 0, "1", nullptr, 4, false, nullptr, false);
    s_engineSurveyCVar = CVarRegister("engineSurvey", "Engine Survey Index", 0, "0", nullptr, 4, false, nullptr, false);
    s_engineSurveyPatchCVar = CVarRegister("engineSurveyPatch", "Engine Survey Patch", 0, "0", nullptr, 4, false, nullptr, false);
    s_checkAddonVersionCVar = CVarRegister("checkAddonVersion", "Check interface addon version number", 0, "1", nullptr, 4, false, nullptr, false);
    s_lastAddonVersionCVar = CVarRegister("lastAddonVersion", "Addon interface version number from previous build", 0, "0", nullptr, 4, false, nullptr, false);
    s_enableMouseSpeedCVar = CVarRegister("enableMouseSpeed", "Enables setting a custom mouse sensitivity to override the setting from the operating system.", 0, "0", EnableMouseSpeedCallback, 4, false, nullptr, false);
    char speed[0x20];
    SStrPrintf(speed, sizeof(speed), "%1.1f", static_cast<double>(OsGetMouseSpeed()));
    s_mouseSpeedCVar = CVarRegister("mouseSpeed", nullptr, 0, speed, MouseSpeedCallback, 4, false, nullptr, false);
    CVarRegister("fullDump", "When you crash, generate a full memory dump", 0, "0", FullDumpCallback, 0, false, nullptr, false);
    CVarRegister("Errors", nullptr, 0x400, "0", ErrorsCallback, 0, false, nullptr, false);
    CVarRegister("showErrors", nullptr, 0, "1", ShowErrorsCallback, 0, false, nullptr, false);
    CVarRegister("ErrorLevelMin", nullptr, 0, "2", ErrorLevelMinCallback, 0, false, nullptr, false);
    CVarRegister("ErrorLevelMax", nullptr, 0, "3", ErrorLevelMaxCallback, 0, false, nullptr, false);
    CVarRegister("ErrorFilter", nullptr, 0, "all", ErrorFilterCallback, 0, false, nullptr, false);
    s_lastCharacterIndexCVar = CVarRegister("lastCharacterIndex", "Last character selected", 0, "0", nullptr, 4, false, nullptr, false);
    s_lastCharacterGuidCVar = CVarRegister("lastCharacterGuid", "Last character selected's guid", 0, "0", nullptr, 4, false, nullptr, false);
    s_seenCharacterUpgradePopupCVar = CVarRegister("seenCharacterUpgradePopup", "Seen the free character upgrade popup", 0, "6", nullptr, 4, false, nullptr, false);
    s_seenExpansionTrialPopupCVar = CVarRegister("seenExpansionTrialPopup", "Seen the expansion trial popup", 0, "6", nullptr, 4, false, nullptr, false);
    s_seenLevelSquishPopupCVar = CVarRegister("seenLevelSquishPopup", "Seen the level squish popup", 0, "0", nullptr, 4, false, nullptr, false);
    s_seenConfigurationWarningsCVar = CVarRegister("seenConfigurationWarnings", "A bitfield to track which configuration warnings have been seen", 0, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("screenshotFormat", "Set the format of screenshots", 1, "jpeg", ScreenshotFormatCallback, 1, false, nullptr, false);
    CVarRegister("screenshotQuality", "Set the quality of screenshots (1 - 10)", 1, "3", ScreenshotQualityCallback, 1, false, nullptr, false);
    CVarRegister("screenshotSizeOverride", "Set the size of screenshots to a specific resolution (e.g. 7680x4320). 0x0 means use the window size", 1, "0x0", ScreenshotSizeOverrideCallback, 1, false, nullptr, false);
    CVarRegister("expandUpgradePanel", "Controls whether the upgrade panel is expanded or collapsed.", 0, "1", nullptr, 4, false, nullptr, false);
    CVarRegister("seenAlliedRaceUnlocks", "Bit array for which allied race unlocks have been seen", 0, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("pendingInviteInfoShown_WowLabs", "The info for pending invites has been shown", 0, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("friendInvitesCollapsed_WowLabs", "The info for pending invites has been shown", 0, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("partyInvitesCollapsed_WowLabs", "The info for pending invites has been shown", 0, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("frontendMatchingModes_WowLabs", "0=All,1=DuoOnly,2=TrioOnly,3=DuoAndTrio", 0, "1", FrontendMatchingModesCallback, 4, false, nullptr, false);
    s_advancedCombatLoggingCVar = CVarRegister("advancedCombatLogging", "Whether we want advanced combat log data sent from the server", 1, "0", AdvancedCombatLoggingCallback, 4, false, nullptr, false);
    s_skipStartGearCVar = CVarRegister("skipStartGear", "Whether we should show starter gear on character create", 1, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("preloadPlayerModels", "Preload all local racial models into memory", 0, "1", nullptr, 4, false, nullptr, false);
    CVarRegister("ConsoleKey", "Set key that opens the console", 1, "`", ConsoleKeyCallback, 2, false, nullptr, false);
    CVarRegister("rawMouseEnable", "Enable raw mouse input", 1, "0", RawMouseEnableCallback, 4, false, nullptr, false);
    CVarRegister("rawMouseAccelerationEnable", "Enable acceleration for raw mouse input", 1, "1", RawMouseAccelerationEnableCallback, 4, false, nullptr, false);
    CVarRegister("rawMouseRate", "Raw mouse update rate", 1, "125", RawMouseRateCallback, 4, false, nullptr, false);
    CVarRegister("rawMouseResolution", "Raw mouse resolution", 1, "400", RawMouseResolutionCallback, 4, false, nullptr, false);
    CVarRegister("MouseUseLazyRepositioning", "During camera control, only reposition mouse cursor when nearing edge of window, rather than every input event", 1, "1", MouseUseLazyRepositioningCallback, 4, false, nullptr, false);
    CVarRegister("MouseNoRepositioning", "Disables repositioning of mouse cursor during relative movement to keep it in the game's window frame", 1, "0", MouseNoRepositioningCallback, 4, false, nullptr, false);
    CVarRegister("cursorSizePreferred", "Size of cursor: -1=determine based on system/monitor dpi, 0=32x32, 1=48x48, 2=64x64, 3=96x96, 4=128x128", 1, "-1", CursorSizePreferredCallback, 1, false, nullptr, false);
    CVarRegister("HardwareCursor", "", 1, "1", HardwareCursorCallback, 1, false, nullptr, false);
    CVarRegister("maxFPSLoading", "Set loading screen max FPS", 1, "10", MaxFPSLoadingCallback, 1, false, nullptr, false);
    s_debugGameEventsCVar = CVarRegister("debugGameEvents", "Show additional information about game events", 1, "0", DebugGameEventsCallback, 0, false, nullptr, false);
    CVarRegister("PreventOsIdleSleep", "Enable this to prevent the computer from idle sleeping while the game is running", 1, "0", PreventOsIdleSleepCallback, 5, false, nullptr, false);
    CVarRegister("heardChoiceSFX", "Whether or not the user has heard the special SFX for the first transition choice selection.", 1, "0", nullptr, 4, false, nullptr, false);
    CVarRegister("seenSoMNotification", "Whether or not the user has seen the Season of Mastery notification at least once.", 1, "0", nullptr, 4, false, nullptr, false);
    s_enablePingsCVar = CVarRegister("enablePings", "Enables ping system.", 0, "1", nullptr, 4, false, nullptr, false);
    s_showPingsInChatCVar = CVarRegister("showPingsInChat", "Enables ping details being shown in chat.", 0, "1", nullptr, 4, false, nullptr, false);
    s_pingModeCVar = CVarRegister("pingMode", "Determines which mode is used to use the ping system.", 0, "0", nullptr, 4, false, nullptr, false);
    FullScreenEffectRegisterCVars();
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cc87b0 = (void*)&s_screenshotQuality;
extern "C" void* const gen_p_142cfbe80 = (void*)&s_messageFilter;
extern "C" void* const gen_p_142cfbe84 = (void*)&s_messageLevelMax;
extern "C" void* const gen_p_142cfbe88 = (void*)&s_messageCategories;
extern "C" void* const gen_p_142de29b8 = (void*)&s_accountUsesTokenCVar;
extern "C" void* const gen_p_142de29c0 = (void*)&s_checkAddonVersionCVar;
extern "C" void* const gen_p_142de29c8 = (void*)&s_lastAddonVersionCVar;
extern "C" void* const gen_p_142de29d0 = (void*)&s_lastCharacterIndexCVar;
extern "C" void* const gen_p_142de29d8 = (void*)&s_lastCharacterGuidCVar;
extern "C" void* const gen_p_142de29e0 = (void*)&s_seenCharacterUpgradePopupCVar;
extern "C" void* const gen_p_142de29e8 = (void*)&s_seenExpansionTrialPopupCVar;
extern "C" void* const gen_p_1430a5600 = (void*)&s_messageLevelMin;
// ---- end of generated globals ----
