#include "client/ConsoleDevice.hpp"
#include "client/CommandLine.hpp"
#include "client/GraphicsSettings.hpp"
#include "prism/Instance.hpp"
#include "client/GxWindow.hpp"
#include "client/Startup.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "framescript/EventCallbacks.hpp"
#include "glue/GlueMgr.hpp"
#include "gx/GxDevice.hpp"
#include "os/Os.hpp"
#include "os/Window.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/String.hpp"
#include "util/Fnv.hpp"
#include "util/Unimplemented.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>

namespace {

// What the client starts from (DAT_142ccbf80, DAT_142ccbfe0) and what detection found
// (DAT_142ccbf70).
GXDEVICEPARAMS s_defaultDevice = { 1, 0, 0, 3, 0, { 0, 0, 0 }, {} };
GXWINDOWPARAMS s_defaultWindow = { { 0, 0 }, { 0, 0 }, 0, 1, 1, 1, 1, 1, { 0, 0, 0 }, 0 };
CONSOLEDETECTED s_detected = { 0, 0, 0, 0, { 0, 0 }, 1.0f };

CVar* s_newResolutionCVar;          // DAT_142e30228: gxNewResolution
CVar* s_fullscreenResolutionCVar;   // DAT_142e30230: gxFullscreenResolution
CVar* s_windowedResolutionCVar;     // DAT_142e30238: gxWindowedResolution
CVar* s_apiCVar;                    // DAT_142e30240: gxApi
CVar* s_monitorCVar;                // DAT_142e30248: gxMonitor
CVar* s_vsyncCVar;                  // DAT_142e30250: vsync
CVar* s_cachelessCVar;              // DAT_142e30258: GxAllowCachelessShaderMode
CVar* s_maxFrameLatencyCVar;        // DAT_142e30260: gxMaxFrameLatency
CVar* s_hwDetectCVar;               // DAT_142e30268: hwDetect
CVar* s_aspectCVar;                 // DAT_142e30270: gxAspect
CVar* s_maximizeCVar;               // DAT_142e30278: gxMaximize
CVar* s_videoOptionsVersionCVar;    // DAT_142e30280: videoOptionsVersion
CVar* s_windowResizeLockCVar;       // DAT_142e30288: windowResizeLock
CVar* s_adapterCVar;                // DAT_142e30290: gxAdapter (looked up)
CVar* s_forceMaxSizeCVar;           // DAT_142e30298: ForceResolutionDefaultToMaxSize
CVar* s_aftermathCVar;              // DAT_142e302a0: gxAftermathEnabled
CVar* s_notchedDisplayCVar;         // DAT_142e302a8: NotchedDisplayMode
CVar* s_graphicsQualityCVar;        // DAT_142e302b0: graphicsQuality
CVar* s_raidGraphicsQualityCVar;    // DAT_142e302b8: RAIDgraphicsQuality
uint8_t s_restarting;               // DAT_142e30204
uint8_t s_hwDetect;                 // DAT_142e30205
uint8_t s_apiReset;                 // DAT_142e30206: videoOptionsVersion was old, gxApi went back to auto
uint32_t s_restartCount;            // DAT_142e30220
char s_windowTitle[0x100];          // DAT_142e302c0
void (*s_afterHwDetect)();          // DAT_142e303c0


// The localized glue strings' DB2 (DAT_142e94e90), and its record lookup.
// (WowClientDB2::GetRecord, FUN_14057b640, on that table)
const char* GlobalStringsGet(uint32_t id, const char* fallback) {
    (void)id;
    FROST_UNIMPLEMENTED();  // WowClientDB2::GetRecord: the strings table is not loaded this early yet
    return fallback;
}

bool StringEqualI(const char* a, const char* b) {
    for (;; a++, b++) {
        int32_t x = *a;
        int32_t y = *b;
        if (static_cast<uint32_t>(x - 'A') < 26) {
            x += 0x20;
        }
        if (static_cast<uint32_t>(y - 'A') < 26) {
            y += 0x20;
        }
        if (x != y) {
            return false;
        }
        if (x == 0) {
            return true;
        }
    }
}

long ParseInt(const char* value) {
    int radix = 10;
    if (value != nullptr && value[0] == '0' && ((value[1] + 0xa8u) & 0xdf) == 0) {
        radix = 0x10;
    }
    return std::strtol(value, nullptr, radix);
}

// ref: FUN_1402282f0
void FormatCVarDefault(char* dest, const char* format, ...) {
    va_list args;
    va_start(args, format);
    SStrVPrintfCount(dest, 0x104, format, args);
    va_end(args);
}

// Writes the parameters back into their CVars.
// ref: FUN_14032a380
void SaveSettings(const GXDEVICEPARAMS* device, const GXWINDOWPARAMS* window) {
    char text[0x400];
    SStrPrintf(text, sizeof(text), "%d", device->m_monitor);
    CVarSet(s_monitorCVar, text, true, false, false, true);
    SStrPrintf(text, sizeof(text), "%d", device->m_vsync);
    CVarSet(s_vsyncCVar, text, true, false, false, true);
    SStrPrintf(text, sizeof(text), "%d", device->m_cachelessShaders);
    CVarSet(s_cachelessCVar, text, true, false, false, true);
    SStrPrintf(text, sizeof(text), "%d", window->m_aspect);
    CVarSet(s_aspectCVar, text, true, false, false, true);
    SStrPrintf(text, sizeof(text), "%d", window->m_maximize);
    CVarSet(s_maximizeCVar, text, true, false, false, true);
    SStrPrintf(text, sizeof(text), "%d", device->m_maxFrameLatency);
    CVarSet(s_maxFrameLatencyCVar, text, true, false, false, true);
}

// A windowed window remembers its size.
// ref: FUN_14032a310
void SaveWindowedResolution() {
    if (s_maximizeCVar->m_intValue == 0) {
        GXSIZE size;
        WindowGetSize(&size);
        char text[0x118];
        std::snprintf(text, sizeof(text), "%hux%hu", size.m_width, size.m_height);  // FUN_140326f90
        CVarSet(s_windowedResolutionCVar, text, true, false, false, true);
    }
}

// ref: FUN_14032a260
void ReportDeviceCreationFailure() {
    GxLog("ReportDeviceCreationFailure(): no output device available!");
    const char* caption = GlobalStringsGet(1, "World of Warcraft");
    const char* text = GlobalStringsGet(3, "World of Warcraft was unable to start up 3D acceleration.");
    OsMessageBox(nullptr, 1, text, caption);
}

// The handler Gx calls when no GPU can run the game.
// ref: FUN_14032a530
void OnNoGpuFound(void* storage) {
    (void)storage;
    const char* caption = GlobalStringsGet(1, "World of Warcraft");
    const char* text = GlobalStringsGet(0x12,
        "World of Warcraft was unable to find a suitable graphics card, please check that your graphics driver is up to "
        "date and that you are not running in safe mode. For more information please see "
        "http://us.battle.net/support/en/article/driver-update-information");
    OsMessageBox(nullptr, 1, text, caption);
    GxLogClose();
}

// AMD's D3D driver crashed: remember the driver so later runs refuse it.
// ref: FUN_140329680
void OnDriverCrash(const char* module) {
    GxLog("Crash detected in driver module %s", module);
    GxAdapterInfo* adapter = GxSelectGpuToUse();
    GxLog("Marking driver as not supported for future client runs. Delete \"%s\" to undo this marking.",
        "UnsupportedDriver.dat");
    OSFILE file;
    OsFileInit(&file);
    if (OsFileOpen(&file, "UnsupportedDriver.dat", 4) == 0) {
        struct {
            uint32_t m_magic;
            uint32_t m_version;
            uint16_t m_vendor;
            uint16_t m_device;
            uint32_t m_pad;
            uint64_t m_driverVersion;
        } record = { 0x57445652, 1, adapter->m_gpu->m_vendorId, adapter->m_gpu->m_deviceId, 0,
            adapter->m_gpu->m_driverVersion };  // 'RVDW'
        uint32_t written;
        OsFileWrite(&file, &record, 0x18, &written);
        OsFileClose(&file);
    }
    OsFileClose(&file);
}

// The display changed: follow the monitor.
// ref: FUN_140329740
void OnDisplayChange(uint32_t monitor, void* param) {
    (void)param;
    if (s_restarting == 0) {
        CVarSetInt(s_monitorCVar, static_cast<int32_t>(monitor), true, false, false, true);
    }
}

// ref: FUN_140328140
bool OnWindowedResolutionChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    uint32_t width;
    uint32_t height;
    if (sscanf_s(newValue, "%ux%u", &width, &height) == 2 && width > 200 && height > 200) {  // FUN_14024cf80
        ConsoleWriteText("Resolution set pending. Call UpdateWindow to finalize");
        return true;
    }
    return false;
}

// ref: FUN_140328190
bool OnWindowResizeLockChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    OsSetWindowResizeLock(static_cast<uint32_t>(ParseInt(newValue)));
    return true;
}

// ref: FUN_140329410
bool OnGraphicsQualityChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    return static_cast<uint32_t>(ParseInt(newValue)) < 10;
}

// ref: FUN_140327ce0
bool OnClipCursorChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    WindowSetClipCursor(ParseInt(newValue) != 0);
    return true;
}

// ref: FUN_140326e80
bool OnNotchedDisplayModeChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    return static_cast<uint32_t>(std::atoi(newValue)) < 3;
}

// ref: FUN_140326fc0
bool OnAfrDevicesCountChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    uint32_t count = static_cast<uint32_t>(ParseInt(newValue));
    if (count > 8) {
        count = 0;
    }
    GxSetAfrDeviceCount(count);
    return true;
}

// gxApi: "auto", or an API the hardware runs.
// ref: FUN_140327d30
bool OnApiChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    if (StringEqualI(newValue, "auto")) {
        ConsoleWriteText("Setting GxApi to autodetect, pending GxRestart.");
        return true;
    }
    for (uint8_t api = 0; api < GXAPI_COUNT; api++) {
        if (GxApiSupported(api) && StringEqualI(newValue, GxApiName(api))) {
            ConsoleWriteText("GxApi set pending GxRestart");
            return true;
        }
    }
    char text[0x400] = "unsupported api, must be one of ";
    bool first = true;
    for (uint8_t api = 0; api < GXAPI_COUNT; api++) {
        if (!GxApiSupported(api)) {
            continue;
        }
        if (!first) {
            strncat_s(text, ", ", _TRUNCATE);
        }
        strncat_s(text, "'", _TRUNCATE);
        strncat_s(text, GxApiName(api), _TRUNCATE);
        strncat_s(text, "'", _TRUNCATE);
        first = false;
    }
    ConsoleWriteText(text);
    return false;
}

// GxRestart: tear the device down and bring it back with the current CVars, recreating the
// window when asked or when the API changes into D3D12 on Windows 10+.
// ref: FUN_140329450
void ConsoleDeviceRestart(uint8_t recreateWindow) {
    s_restartCount++;
    s_restarting = 1;
    GxLog("GxRestart");
    uint8_t current = static_cast<CGxDevice*>(g_gxDevice)->m_api;  // FUN_142289c30
    if (s_apiCVar != nullptr) {
        const char* wanted = s_apiCVar->m_stringValue.GetString();
        uint8_t api = current;
        if (StringEqualI(wanted, "auto")) {
            api = GxGetBestApi();
        } else {
            for (uint8_t i = 0; i < GXAPI_COUNT; i++) {
                if (GxApiSupported(i) && _stricmp(wanted, GxApiName(i)) == 0) {
                    api = i;
                    break;
                }
            }
        }
        if (api != current) {
            // out of D3D12 on Windows 10 and later the window goes with it
            if (current == GXAPI_D3D12 && OsGetVersion()->m_major > 9) {
                recreateWindow = 1;
            }
            GxSetApi(api);
        }
    }
    GxDevDestroy();
    GXDEVICEPARAMS device = { 1, 0, 0, 3, 0, { 0, 0, 0 }, {} };
    GXWINDOWPARAMS window = { { 0, 0 }, { 0, 0 }, 0, 1, 1, 1, 1, 1, { 0, 0, 0 }, 0 };
    ConsoleDeviceGetParams(&device, &window);
    if (recreateWindow == 0) {
        WindowUpdate(&window);
    } else {
        GxLog("Recreating Window");
        WindowDestroy();
        WindowCreate(&window);
    }
    if (!ConsoleDeviceCreate(&device, &window)) {
        ReportDeviceCreationFailure();
        OsTerminateProcess();  // FUN_1402651b0
    }
    WindowSetTitle(WindowGetHandle(), s_windowTitle);
    GlueMgrNoteGxRestart();  // DAT_14311c52a
    s_restarting = 0;
    UiSignalGxRestarted();
}

// The display-changed hook: the UI is told (FrameScript event 0x1105d6abf63fd94e).
// ref: FUN_14116bd70
void OnDisplaySizeChanged(void* storage) {
    (void)storage;
    FROST_UNIMPLEMENTED();  // the UI is not up yet
}

}  // namespace

// The window brought to the current CVars without a restart: size, mode, monitor; the UI is
// told (NOTCHED_DISPLAY_MODE_CHANGED). What UpdateWindow (console and Lua) runs.
// ref: FUN_140328c10
void ConsoleDeviceUpdateWindow() {
    GXDEVICEPARAMS device = { 1, 0, 0, 3, 0, { 0, 0, 0 }, {} };
    GXWINDOWPARAMS window = { { 0, 0 }, { 0, 0 }, 0, 1, 1, 1, 1, 1, { 0, 0, 0 }, 0 };
    ConsoleDeviceGetParams(&device, &window);
    WindowUpdate(&window);
    UiSignalNotchedDisplayModeChanged();
}

namespace {

// ref: FUN_140327c50
int32_t CommandGxRestart(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    GxRequestRestart();
    return 1;
}

// ref: FUN_140327cc0
int32_t CommandUpdateWindow(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    ConsoleDeviceUpdateWindow();
    return 1;
}

// Windowed <-> fullscreen window, not while a loading screen is up.
// ref: FUN_140327c70
int32_t CommandToggleWindowMode(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    if (!LoadingScreenIsActive()) {
        SaveWindowedResolution();
        CVarSetInt(s_maximizeCVar, s_maximizeCVar->m_intValue == 0, true, false, false, true);
        ConsoleDeviceUpdateWindow();
    }
    return 1;
}

#define CONSOLE_DEVICE_COMMAND_STUB(name, address)                     \
    int32_t name(const char* command, const char* arguments) {        \
        (void)command;                                                 \
        (void)arguments;                                               \
        FROST_UNIMPLEMENTED();                                         \
        return 1;                                                      \
    }

// ref: FUN_140327b60
CONSOLE_DEVICE_COMMAND_STUB(CommandGxGetVideoMemory, 0x140327b60)
// ref: FUN_140327bd0
CONSOLE_DEVICE_COMMAND_STUB(CommandGxListGPUs, 0x140327bd0)
// ref: FUN_140327900
CONSOLE_DEVICE_COMMAND_STUB(CommandGxCurrentGPU, 0x140327900)
// ref: FUN_1403278b0
CONSOLE_DEVICE_COMMAND_STUB(CommandGxCurrentAPI, 0x1403278b0)
// ref: FUN_140327c30
CONSOLE_DEVICE_COMMAND_STUB(CommandGxLogFullGPUInfo, 0x140327c30)
// ref: FUN_1403277b0
CONSOLE_DEVICE_COMMAND_STUB(CommandGetSwapChainSize, 0x1403277b0)
// ref: FUN_140327810
CONSOLE_DEVICE_COMMAND_STUB(CommandGetWindowSize, 0x140327810)
// ref: FUN_140327870
CONSOLE_DEVICE_COMMAND_STUB(CommandDefragmentGPU, 0x140327870)
// ref: FUN_140327ba0
CONSOLE_DEVICE_COMMAND_STUB(CommandGxIsPrism, 0x140327ba0)
// ref: FUN_140326e60
CONSOLE_DEVICE_COMMAND_STUB(CommandGxLogFlush, 0x140326e60)
// ref: FUN_140326ea0
CONSOLE_DEVICE_COMMAND_STUB(CommandD3D11Legacy, 0x140326ea0)
// ref: FUN_140326ee0
CONSOLE_DEVICE_COMMAND_STUB(CommandD3D11, 0x140326ee0)
// ref: FUN_140326f20
CONSOLE_DEVICE_COMMAND_STUB(CommandD3D12, 0x140326f20)

// The blz::function invokers the hooks are stored with: call the function pointer kept in
// the storage (LAB_1402282e0 with a flag, FUN_14032a520 without).
// The flag comes first and the storage second (0x1402282e0 is `jmp [rdx]`).
void InvokeStoredWithFlag(uint8_t flag, void* storage) {
    (*reinterpret_cast<void (**)(uint8_t)>(storage))(flag);
}

// ref: FUN_14032a520
void InvokeStored(void* storage) {
    (*reinterpret_cast<void (**)()>(storage))();
}

// The device CVars.
// ref: FUN_1403299d0
void RegisterDeviceCVars() {
    char text[0x110];
    FormatCVarDefault(text, "%d", s_defaultDevice.m_vsync);
    s_vsyncCVar = CVarRegister("vsync", "vsync on or off", 1, text, ConsoleDeviceRequestRestart, 1, false, nullptr, false);
    FormatCVarDefault(text, "%d", s_defaultDevice.m_cachelessShaders);
    s_cachelessCVar = CVarRegister("GxAllowCachelessShaderMode",
        "CPU memory saving mode, if supported by backend. When enabled, shaders are fetched from disk as needed instead "
        "of being kept resident. This mode may slightly increase the time objects take to appear the first time they "
        "are encountered. Computers without solid state drives may want to disable this feature",
        1, text, ConsoleDeviceRequestRestart, 1, false, nullptr, false);
    FormatCVarDefault(text, "%d", s_defaultWindow.m_aspect);
    s_aspectCVar = CVarRegister("gxAspect", "constrain window aspect", 1, text, ConsoleDeviceRequestRestart, 1, false,
        nullptr, false);
    FormatCVarDefault(text, "%d", s_defaultDevice.m_maxFrameLatency);
    s_maxFrameLatencyCVar = CVarRegister("gxMaxFrameLatency", "maximum number of frames ahead of GPU the CPU can be", 1,
        text, ConsoleDeviceRequestRestart, 1, false, nullptr, false);
    FormatCVarDefault(text, "%d", s_defaultDevice.m_aftermath);  // formatted, then "1" is registered
    s_aftermathCVar = CVarRegister("gxAftermathEnabled", "Enable frame crash debugging", 1, "1",
        ConsoleDeviceRequestRestart, 1, false, nullptr, false);
    s_windowResizeLockCVar = CVarRegister("windowResizeLock", "prevent resizing in windowed mode", 1, "0",
        OnWindowResizeLockChanged, 1, false, nullptr, false);
    s_graphicsQualityCVar = CVarRegister("graphicsQuality", "save for Graphics Quality Selection", 0x800, "6",
        OnGraphicsQualityChanged, 1, false, nullptr, false);
    s_raidGraphicsQualityCVar = CVarRegister("RAIDgraphicsQuality", "save for Raid Graphics Quality Selection", 0x800,
        "6", OnGraphicsQualityChanged, 1, false, nullptr, false);
    CVarRegister("ClipCursor", "Lock the cursor to the game window", 1, "0", OnClipCursorChanged, 1, false, nullptr,
        false);
    s_notchedDisplayCVar = CVarRegister("NotchedDisplayMode",
        "Do nothing = 0. Shift UI down = 1. Shift everything down = 2.", 1, "1", OnNotchedDisplayModeChanged, 1, false,
        nullptr, false);
}

// The window CVars, then the API to use: gxApi's, else the best the hardware runs, and the
// command line over both.
// ref: FUN_140329d10
void RegisterWindowCVars() {
    s_apiCVar = CVarRegister("gxApi", "graphics api", 1, "auto", OnApiChanged, 1, false, nullptr, false);
    s_adapterCVar = CVarLookup(t2util::FnvHashStringI("xAdapter", 0xaf63da4c8601e926ull));  // "gxAdapter"
    s_forceMaxSizeCVar = CVarRegister("ForceResolutionDefaultToMaxSize",
        "Force default resolution to the maximum supported size rather than the auto-detected size", 1, "0",
        static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    CVarRegister("gxAFRDevicesCount", "Force to set number of AFR devices", 1, "0", OnAfrDevicesCountChanged, 1, false,
        nullptr, false);
    char text[0x110];
    FormatCVarDefault(text, "%d", 1);
    s_maximizeCVar = CVarRegister("gxMaximize", "toggle fullscreen/window", 0x800, text, ConsoleDeviceRequestRestart,
        1, false, nullptr, false);
    s_monitorCVar = CVarRegister("gxMonitor", "monitor", 1, "0", static_cast<CVARCALLBACK>(nullptr), 1, false,
        nullptr, false);
    uint32_t monitor = static_cast<uint32_t>(s_monitorCVar->m_intValue);
    if (GxMonitorCount() <= monitor) {
        CVarSetInt(s_monitorCVar, 0, true, false, false, true);
    }
    GXSIZE desktop = { 0, 0 };
    uint32_t bitsPerPixel;
    uint8_t index = static_cast<uint8_t>(s_monitorCVar->m_intValue);
    if (GxMonitorGetDesktopSize(index, &desktop, &bitsPerPixel)) {
        s_defaultWindow.m_size = desktop;
        s_defaultWindow.m_resolution = desktop;
        s_defaultWindow.m_monitor = index;
    }
    s_fullscreenResolutionCVar = CVarRegister("gxFullscreenResolution", "resolution", 1, "auto",
        ConsoleDeviceRequestRestart, 1, false, nullptr, false);
    FormatCVarDefault(text, "%dx%d", 0x780, 0x438);
    s_windowedResolutionCVar = CVarRegister("gxWindowedResolution", "windowed resolution", 1, text,
        OnWindowedResolutionChanged, 1, false, nullptr, false);
    s_newResolutionCVar = CVarRegister("gxNewResolution", "resolution to be set", 0x400, "0x0",
        ConsoleDeviceRequestRestart, 1, false, nullptr, false);
    s_videoOptionsVersionCVar = CVarRegister("videoOptionsVersion", "Video options version", 1, "0",
        static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    CVarRegister("reloadUIOnAspectChange", "Reload the UI on aspect change", 1, "0", static_cast<CVARCALLBACK>(nullptr), 1,
        false, nullptr, false);
    int32_t version = s_videoOptionsVersionCVar->m_intValue;
    if (version < 0x22) {
        CVarSet(s_apiCVar, "auto", true, false, false, true);
        s_apiReset = 1;
    } else if (version == 0x22 || version == 0x23) {
        CVar* cacheless = CVarLookup(t2util::FnvHashStringI("xAllowCachelessShaderMode", 0xaf63da4c8601e926ull));
        if (cacheless != nullptr) {
            CVarSetInt(cacheless, 0, true, false, false, true);
        }
    }
    uint8_t best = GxGetBestApi();
    const char* wanted = s_apiCVar->m_stringValue.GetString();
    uint8_t api = best;
    if (!StringEqualI(wanted, "auto")) {
        for (uint8_t candidate = 0; candidate < GXAPI_COUNT; candidate++) {
            if (GxApiSupported(candidate) && _stricmp(wanted, GxApiName(candidate)) == 0) {
                api = candidate;
                break;
            }
        }
    }
    if (CommandLineHasOption(2) && GxApiSupported(GXAPI_D3D12)) {
        api = GXAPI_D3D12;
    }
    if (CommandLineHasOption(1) && GxApiSupported(GXAPI_D3D11)) {
        api = GXAPI_D3D11;
    }
    if (CommandLineHasOption(4) && GxApiSupported(GXAPI_D3D11_LEGACY)) {
        api = GXAPI_D3D11_LEGACY;
    }
    GxSetApi(api);
}

// The device commands, and the hooks Gx calls back into the console through.
// ref: FUN_140329770
void RegisterDeviceCommands() {
    blz::function_storage restart = { reinterpret_cast<void*>(&InvokeStoredWithFlag), blz::function_trivial_manager, {} };
    auto restartTarget = &ConsoleDeviceRestart;
    std::memcpy(restart.m_storage, &restartTarget, sizeof(restartTarget));
    GxSetRestartHandler(&restart);
    blz::function_storage displayChanged = { reinterpret_cast<void*>(&InvokeStored), blz::function_trivial_manager, {} };
    auto displayTarget = &OnDisplaySizeChanged;
    std::memcpy(displayChanged.m_storage, &displayTarget, sizeof(displayTarget));
    GxSetDisplayChangedHandler(&displayChanged);
    ConsoleCommandRegister("GxRestart", CommandGxRestart, 1, nullptr, 0);
    ConsoleCommandRegister("UpdateWindow", CommandUpdateWindow, 1, nullptr, 0);
    ConsoleCommandRegister("ToggleWindowMode", CommandToggleWindowMode, 1, nullptr, 0);
    ConsoleCommandRegister("GxGetVideoMemory", CommandGxGetVideoMemory, 1, nullptr, 0);
    ConsoleCommandRegister("GxListGPUs", CommandGxListGPUs, 1, nullptr, 0);
    ConsoleCommandRegister("GxCurrentGPU", CommandGxCurrentGPU, 1, nullptr, 0);
    ConsoleCommandRegister("GxCurrentAPI", CommandGxCurrentAPI, 1, nullptr, 0);
    ConsoleCommandRegister("GxLogFullGPUInfo", CommandGxLogFullGPUInfo, 1, nullptr, 0);
    ConsoleCommandRegister("GetSwapChainSize", CommandGetSwapChainSize, 1, nullptr, 0);
    ConsoleCommandRegister("GetWindowSize", CommandGetWindowSize, 1, nullptr, 0);
    ConsoleCommandRegister("DefragmentGPU", CommandDefragmentGPU, 1, "Defragment suballocated GPU resources", 0);
    ConsoleCommandRegister("GxIsPrism", CommandGxIsPrism, 1, nullptr, 0);
    ConsoleCommandRegister("GxLogFlush", CommandGxLogFlush, 1, nullptr, 0);
    ConsoleCommandRegister("D3D11_LEGACY", CommandD3D11Legacy, 1, nullptr, 0);
    ConsoleCommandRegister("D3D11", CommandD3D11, 1, nullptr, 0);
    ConsoleCommandRegister("D3D12", CommandD3D12, 1, nullptr, 0);
}

}  // namespace

// The fullscreen sizes the monitor offers that suit the client: its modes of 16 bits or more,
// matching the desktop's aspect when asked; landscape ones above 640x480 at the minimum aspect,
// and on a portrait desktop narrow ones turned on their side; sorted largest first without
// repeats.
// ref: FUN_14032abf0
void GetFullscreenSizes(blz::vector<GXSIZE, s_gxSizeTag>* sizes, uint32_t monitor, bool matchAspect) {
    sizes->clear();
    GXSIZE desktop = { 0, 0 };
    uint32_t bitsPerPixel = 0;
    // FUN_142290bc0
    GxMonitorGetDesktopSize(monitor, &desktop, &bitsPerPixel);
    double desktopAspect = 1.7777777777777777;  // DAT_142637038
    bool portrait = false;
    if (desktop.m_height != 0) {
        desktopAspect = static_cast<double>(desktop.m_width) / static_cast<double>(desktop.m_height);
        portrait = static_cast<float>(desktop.m_width) / static_cast<float>(desktop.m_height) < 1.0f;
    }
    blz::vector<GXMONITORMODE, s_gxMonitorModeTag> modes;
    modes.reserve(0x12c);
    GxMonitorGetModes(monitor, &modes);
    uint32_t maxWidth = 0;
    for (uint64_t i = 0; i < modes.size(); i++) {
        uint32_t width = modes[i].m_size.m_width;
        if (width > maxWidth) {
            maxWidth = width;
        }
    }
    sizes->reserve(0);
    const float minimumAspect = 1.248f;      // DAT_142637034
    const double epsilon = 1.0e-6;            // DAT_14261bf68
    uint16_t lastWidth = 0;
    uint16_t lastHeight = 0;
    for (uint64_t i = 0; i < modes.size(); i++) {
        GXSIZE size = modes[i].m_size;
        float aspect = static_cast<float>(size.m_width) / static_cast<float>(size.m_height);
        if (size.m_width == lastWidth && size.m_height == lastHeight) {
            continue;
        }
        if (modes[i].m_bitsPerPixel < 0x10) {
            continue;
        }
        if (matchAspect
            && std::fabs(desktopAspect - static_cast<double>(size.m_width) / static_cast<double>(size.m_height))
                >= epsilon) {
            continue;
        }
        if (aspect >= minimumAspect && size.m_width > 0x280 && size.m_height > 0x1e0) {
            // FUN_14032a5e0
            sizes->push_back(size);
        } else if (portrait && aspect < 1.0f && maxWidth >= size.m_height && size.m_height > 0x280
            && size.m_width > 0x1e0) {
            GXSIZE turned = { size.m_height, size.m_width };
            sizes->push_back(turned);
        }
        lastWidth = size.m_width;
        lastHeight = size.m_height;
    }
    // FUN_14032a7b0, FUN_14032a980: stable, taller first, then wider first
    std::stable_sort(sizes->begin(), sizes->end(),
        [](const GXSIZE& a, const GXSIZE& b) { return a.m_height > b.m_height; });
    std::stable_sort(sizes->begin(), sizes->end(),
        [](const GXSIZE& a, const GXSIZE& b) { return a.m_width > b.m_width; });
    uint64_t count = 0;
    for (uint64_t i = 0; i < sizes->size(); i++) {
        GXSIZE size = (*sizes)[i];
        if (count != 0 && (*sizes)[count - 1].m_width == size.m_width && (*sizes)[count - 1].m_height == size.m_height) {
            continue;
        }
        (*sizes)[count++] = size;
    }
    sizes->resize(count);
}

// The default fullscreen size for the adapter: the desktop, except on parts that run better
// at 720p or 1080p.
// ref: FUN_1403281e0
GXSIZE DefaultFullscreenSize(GxAdapterInfo* adapter, uint32_t monitor) {
    uint16_t height = 0xffff;
    pm::GPU* gpu = adapter->m_gpu;
    if (gpu->m_type == pm::GPU_INTEGRATED && gpu->m_vendorId == 0x8086) {
        auto data = reinterpret_cast<const uint8_t*>(adapter->m_data);
        if (static_cast<uint8_t>(data[0x78] - 1) > 5) {
            uint32_t frequency;
            uint32_t units;
            std::memcpy(&frequency, data + 0x7c, 4);
            std::memcpy(&units, data + 0x84, 4);
            double power = static_cast<double>(frequency) * 0.008 * static_cast<double>(units);  // _DAT_142636f78
            uint32_t score = static_cast<uint32_t>(static_cast<int64_t>(power + power));
            if (score > 0x9c3) {
                height = 0xf00;
            } else if (score > 899) {
                height = 0x438;
            } else {
                height = 0x2d0;
            }
        } else {
            height = 0x2d0;
        }
    } else if (gpu->m_vendorId == 0x4351) {
        height = 0x2d0;
    }
    if (s_forceMaxSizeCVar->m_intValue != 0) {
        height = 0xffff;
    }
    GXSIZE size = { 0x780, 0x438 };
    if (height == 0xffff) {
        GXSIZE desktop = { 0, 0 };
        uint32_t bitsPerPixel;
        GxMonitorGetDesktopSize(monitor, &desktop, &bitsPerPixel);
        size = desktop;
    } else {
        blz::vector<GXSIZE, s_gxSizeTag> sizes;
        GetFullscreenSizes(&sizes, monitor, true);
        uint32_t best = 0xffffffff;
        for (uint32_t i = 0; i < sizes.size(); i++) {
            int32_t difference = static_cast<int32_t>(sizes[i].m_height) - static_cast<int32_t>(height);
            uint32_t distance = static_cast<uint32_t>(difference < 0 ? -difference : difference);
            if (distance < best) {
                size = sizes[i];
                best = distance;
            }
        }
    }
    return size;
}

namespace {

// The lowest of the three tiers.
// ref: FUN_14032b830
int8_t DetectedTier(const CONSOLEDETECTED* detected) {
    if (detected->m_gpu < detected->m_cpu) {
        return detected->m_memory < detected->m_gpu ? detected->m_memory : detected->m_gpu;
    }
    return detected->m_memory < detected->m_cpu ? detected->m_memory : detected->m_cpu;
}

// renderScale's coded default: the detected scale.
// ref: FUN_14032b860
float DetectedRenderScale() {
    return ConsoleGetDetected()->m_renderScale;
}

// The CPU tier for a processor family the tiers overrate: older Intel cores and AMD's
// Bulldozer and early Zen parts are held to 4.
bool IsHeldBackCpu(const OSCPUINFO* cpu) {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    uint32_t version = static_cast<uint32_t>(cpuInfo[0]);
    uint32_t family = version >> 8 & 0xf;
    uint32_t model = version >> 4 & 0xf;
    uint32_t extModel = version >> 16 & 0xf;
    uint32_t extFamily = version >> 20 & 0xff;
    const char* vendor = reinterpret_cast<const char*>(cpu) + 0x2b;
    if (std::strcmp(vendor, "GenuineIntel") == 0) {
        if (extFamily != 0 || family != 6) {
            return false;
        }
        if (extModel == 3) {
            return model == 0xc || model == 0xf || model == 0xd;
        }
        if (extModel == 4) {
            return model - 5 < 2 || ((model - 7) & ~8u) == 0 || model == 0xe;
        }
        if (extModel == 5) {
            return model == 6 || model == 0xe || model == 5;
        }
        return false;
    }
    if (std::strcmp(vendor, "AuthenticAMD") == 0) {
        return family == 0xf && (extFamily == 9 || (extFamily == 8 && (extModel << 4 | model) < 0x31));
    }
    return false;
}

// The graphics defaults for the machine: a tier (0 lowest to 6) each for the CPU (cores,
// clock, family), the GPU (vendor family, or for an integrated part its generation; capped by
// video memory) and the memory; the window size the adapter wants; and for a GPU below the top
// tiers a render scale bringing the world down to 1080 (1440 for tiers 4 and 5) lines. The
// lowest tier becomes graphicsQuality's coded default, and the scale renderScale's. Fails, with
// a message, on a system older than Windows Vista (or macOS 10.14).
// ref: FUN_14032b0a0
bool DetectDefaults(CONSOLEDETECTED* detected, uint32_t monitor) {
    const OSVERSION* os = OsGetVersion();
    const OSCPUINFO* cpu = OsGetCpuInfo();
    bool tooOld = false;
    if (os->m_platform == 10) {
        tooOld = os->m_major < 6 || (os->m_major == 6 && os->m_minor == 0);
    } else if (os->m_platform == 6 && os->m_major == 10 && os->m_minor < 0xe) {
        tooOld = true;
    }
    if (tooOld) {
        const char* caption = GlobalStringsGet(1, "World of Warcraft");
        const char* text = GlobalStringsGet(0x1b, "World of Warcraft was unable to start in compatibility mode.");
        OsMessageBox(nullptr, 1, text, caption);
        return false;
    }

    // the CPU
    uint32_t threads = cpu->m_logical;
    int8_t coreTier = threads == 1 ? 0 : threads < 4 ? 2 : threads < 6 ? 4 : 6;
    int8_t cpuTier = coreTier;
    uint64_t clock = cpu->m_maxFrequency;
    if (clock != 0) {
        cpuTier = clock < 2000000000 ? 0 : clock < 3000000000 ? 2 : clock < 3500000000 ? 4 : 6;
    }
    int8_t capTier = 6;
    if ((reinterpret_cast<const uint8_t*>(cpu)[0x21] & 1) == 0) {
        capTier = 2;
    } else if (IsHeldBackCpu(cpu)) {
        cpuTier = 4;
    }
    if (coreTier < cpuTier) {
        cpuTier = coreTier;
    }
    if (capTier < cpuTier) {
        cpuTier = capTier;
    }

    // the memory
    uint64_t memory = OsGetPhysicalMemory();
    int8_t memoryTier = memory < 0x73800000 ? 0
        : memory < 0xf3800000                ? 2
        : memory < 0x1f3800000               ? 3
        : memory < 0x3f3800000               ? 4
                                             : 6;

    // the GPU
    GxAdapterInfo* adapter = GxSelectGpuToUse();
    pm::GPU* gpu = adapter->m_gpu;
    pm::APIDEVICEDATA* data = adapter->m_data;
    int8_t gpuTier;
    if ((gpu->m_type & 0xfd) == 0) {
        // unknown or integrated
        if (gpu->m_vendorId == 0x8086) {
            gpuTier = static_cast<uint8_t>(data->m_intelGeneration - 1) > 0xb ? 2 : 1;
        } else if (gpu->m_vendorId == 0x106b) {
            uint32_t cores = data->m_appleGpuCores;
            gpuTier = cores > 0x17 ? 6 : cores > 0xd ? 4 : cores > 7 ? 3 : 2;
        } else {
            gpuTier = 1;
        }
    } else {
        uint32_t dedicated = 0;
        uint32_t shared = 0;
        GxDeviceVideoMemory(&dedicated, &shared);
        if (gpu->m_vendorId == 0x1002) {
            uint8_t family = data->m_amdFamily;
            if (static_cast<uint8_t>(family - 1) >= 7) {
                gpuTier = 6;
            } else if (static_cast<uint8_t>(family - 6) < 2) {
                gpuTier = 5;
            } else if (family == 5) {
                gpuTier = 4;
            } else {
                gpuTier = family == 4 ? 2 : 1;
            }
        } else if (gpu->m_vendorId == 0x10de) {
            uint8_t family = data->m_nvidiaFamily;
            if (static_cast<uint8_t>(family - 1) > 5) {
                gpuTier = 6;
            } else if (static_cast<uint8_t>(family - 5) < 2) {
                gpuTier = 5;
            } else if (family == 4) {
                gpuTier = 4;
            } else {
                gpuTier = family > 1 ? 2 : 1;
            }
        } else {
            gpuTier = gpu->m_vendorId == 0x8086 ? 6 : 4;
        }
        if (gpu->m_virtualization == 3) {
            dedicated += shared;
        }
        int8_t memoryCap = dedicated < 0x401 ? 1 : dedicated < 0xb38 ? 2 : dedicated < 0x1f38 ? 4 : 6;
        if (memoryCap < gpuTier) {
            gpuTier = memoryCap;
        }
        if (data->m_d9 < 4 && 1 < gpuTier) {
            gpuTier = 1;
        }
    }

    detected->m_cpu = cpuTier;
    detected->m_memory = memoryTier;
    detected->m_gpu = gpuTier;
    detected->m_windowSize = DefaultFullscreenSize(GxSelectGpuToUse(), monitor);
    detected->m_renderScale = 1.0f;

    // the world's render size
    uint16_t shorter = detected->m_windowSize.m_width < detected->m_windowSize.m_height ? detected->m_windowSize.m_width
                                                                                       : detected->m_windowSize.m_height;
    float target = 0.0f;
    if (gpuTier < 4) {
        target = shorter < 0x439 ? 0.0f : 1080.0f;  // DAT_142637040
    } else if (gpuTier <= 5) {
        target = shorter < 0x5a1 ? 0.0f : 1440.0f;  // DAT_142637044
    }
    if (target != 0.0f) {
        float scale = target / static_cast<float>(shorter);
        if (scale <= 1.0f / 3.0f) {  // DAT_14261bf60
            scale = 1.0f / 3.0f;
        }
        detected->m_renderScale = scale;
    }

    GraphicsSetRenderScaleDefault(&DetectedRenderScale);
    if (CVar* renderScale = CVarLookup(t2util::ConstFnvHashStringI("renderScale"))) {
        CVarSetFloat(renderScale, detected->m_renderScale, false, false, true, true);
    }

    blz::inline_string<0x400> text;
    int8_t tier = DetectedTier(detected);
    blz::string_format(text.header(), "Detected Graphics Defaults: %d (CPU = %d, GPU = %d, MEM = %d) WindowSize: %ux%u",
        tier + 1, cpuTier + 1, gpuTier + 1, memoryTier + 1, detected->m_windowSize.m_width,
        detected->m_windowSize.m_height);
    float scale = detected->m_renderScale;
    if (scale != 1.0f) {
        blz::string_append_format(text.header(), " WorldRenderSize: %ux%u (%.1f%%)",
            static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(detected->m_windowSize.m_width) * scale)),
            static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(detected->m_windowSize.m_height) * scale)),
            static_cast<double>(scale));
    }
    GxLog("%s", text.m_data);
    for (const char* name : { "graphicsQuality", "RAIDgraphicsQuality" }) {
        CVarSetDefaultInt(CVarLookup(t2util::ConstFnvHashStringI(name)), DetectedTier(detected));
    }
    return true;
}

}

// ref: FUN_14032a510
void ConsoleDeviceSetHardwareDetectHook(void (*hook)()) {
    s_afterHwDetect = hook;
}

// ref: FUN_140328120
bool ConsoleDeviceRequestRestart(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)newValue;
    (void)userData;
    ConsoleWriteText("set pending gxRestart");
    return true;
}

// ref: FUN_1403271d0
void ConsoleDeviceGetParams(GXDEVICEPARAMS* device, GXWINDOWPARAMS* window) {
    device->m_vsync = 1;
    device->m_cachelessShaders = 0;
    device->m_monitor = 0;
    device->m_maxFrameLatency = 3;
    device->m_aftermath = 0;
    device->m_adapterName.m_size = 0;
    device->m_adapterName.m_data[0] = '\0';
    uint8_t monitorWanted = static_cast<uint8_t>(s_monitorCVar->m_intValue);
    uint8_t monitor = monitorWanted < GxMonitorCount() ? monitorWanted : 0;
    device->m_monitor = monitor;
    window->m_monitor = monitor;
    window->m_14 = 0;
    window->m_maximize = s_maximizeCVar->m_intValue != 0;
    window->m_aspect = s_aspectCVar->m_intValue != 0;
    uint8_t notched = s_notchedDisplayCVar != nullptr ? static_cast<uint8_t>(s_notchedDisplayCVar->m_intValue) : 0;
    window->m_keepUiPosition = notched != 2;
    uint8_t latency = static_cast<uint8_t>(s_maxFrameLatencyCVar->m_intValue);
    if (latency == 0) {
        latency = 1;
    } else if (latency > 3) {
        latency = 3;
    }
    device->m_maxFrameLatency = latency;
    bool vsync = s_vsyncCVar->m_intValue != 0;
    window->m_vsync = vsync;
    device->m_vsync = vsync;
    device->m_cachelessShaders = s_cachelessCVar->m_intValue != 0;
    device->m_aftermath = s_aftermathCVar->m_intValue != 0;
    blz::string_reserve(device->m_adapterName.header(), 0);
    device->m_adapterName.m_data[0] = '\0';
    device->m_adapterName.m_size = 0;
    const char* adapterName = s_adapterCVar->m_stringValue.GetString();
    if (std::strlen(adapterName) > 2) {
        GXADAPTERLIST* adapters = GxGetAdapterList();
        bool found = false;
        for (uint64_t i = 0; i < adapters->m_size; i++) {
            GxAdapterInfo* info = &adapters->m_data[i];
            if (std::strcmp(info->m_name.m_data, adapterName) == 0) {
                uint64_t length = std::strlen(adapterName);
                blz::string_reserve(device->m_adapterName.header(), length);
                std::memcpy(device->m_adapterName.m_data, adapterName, length);
                device->m_adapterName.m_data[length] = '\0';
                device->m_adapterName.m_size = length;
                found = true;
                break;
            }
        }
        if (!found) {
            GxLog("Requested GPU named \"%s\" not found. Using default GPU instead", adapterName);
            CVarSet(s_adapterCVar, "", true, false, false, true);
        }
    }
    CVar* resolutionCVar = window->m_maximize != 0 ? s_fullscreenResolutionCVar : s_windowedResolutionCVar;
    const char* pending = s_newResolutionCVar->m_stringValue.GetString();
    if (std::strcmp(pending, "0x0") != 0) {
        uint16_t size[2] = { 0, 0 };
        if (window->m_maximize != 0 && std::strcmp(pending, "auto") == 0) {
            CVarSet(resolutionCVar, "auto", true, false, false, true);
        } else {
            sscanf_s(pending, "%hux%hu", &size[0], &size[1]);
            if (size[1] > 0x77) {
                CVarSet(resolutionCVar, pending, true, false, false, true);
            }
        }
        CVarSet(s_newResolutionCVar, "0x0", true, false, false, true);
    }
    bool automatic = false;
    GXSIZE size = { 0, 0 };
    const char* resolution = resolutionCVar->m_stringValue.GetString();
    if (window->m_maximize != 0 && std::strcmp(resolution, "auto") == 0) {
        size = DefaultFullscreenSize(GxSelectGpuToUse(), static_cast<uint32_t>(s_monitorCVar->m_intValue));
        automatic = true;
    } else {
        int32_t parsed = sscanf_s(resolution, "%hux%hu", &size.m_width, &size.m_height);
        if (parsed != 2 || size.m_width < 0x78 || size.m_height < 0x78) {
            blz::vector<GXSIZE, s_gxSizeTag> sizes;
            GetFullscreenSizes(&sizes, window->m_monitor, window->m_maximize != 0);
            if (sizes.size() == 0) {
                size = { 0x780, 0x438 };
            } else {
                size = sizes[0];
            }
            char text[0x20];
            SStrPrintf(text, sizeof(text), "%hux%hu", size.m_width, size.m_height);  // FUN_140326f60
            CVarSet(resolutionCVar, text, true, false, false, true);
        }
    }
    if (window->m_maximize == 0) {
        window->m_size = size;
        window->m_resolution = size;
        window->m_sizeMatches = 1;
    } else {
        GXSIZE desktop = { 0, 0 };
        uint32_t bitsPerPixel;
        GxMonitorGetDesktopSize(monitor, &desktop, &bitsPerPixel);
        window->m_size = desktop;
        window->m_resolution = size;
        window->m_sizeMatches = automatic && desktop.m_width == size.m_width && desktop.m_height == size.m_height;
    }
}

// ref: FUN_140329230
bool ConsoleDeviceCreate(GXDEVICEPARAMS* device, GXWINDOWPARAMS* window) {
    uint8_t api = GxGetApi();
    for (;;) {
        if (GxDevCreate(api, device)) {
            GxAdapterInfo* adapter = GxSelectGpuToUse();
            const char* wanted = s_adapterCVar->m_stringValue.GetString();
            uint64_t wantedLength = std::strlen(wanted);
            uint64_t length = adapter->m_name.m_size;
            uint64_t compared = length <= wantedLength ? length : wantedLength;
            if ((compared != 0 && std::memcmp(adapter->m_name.m_data, wanted, compared) != 0)
                || static_cast<int32_t>(length) != static_cast<int32_t>(wantedLength)) {
                CVarSet(s_adapterCVar, "", true, false, false, true);
            }
            return true;
        }
        if (std::strlen(s_adapterCVar->m_stringValue.GetString()) > 2) {
            GxLog("Trying default GPU");
            CVarSet(s_adapterCVar, "", true, false, false, true);
            GxLogFlushNow();
            continue;
        }
        if (api == GXAPI_D3D12) {
            GxLog("Trying DX11");
            if (OsGetVersion()->m_major > 9) {
                WindowDestroy();
                WindowCreate(WindowGetParams());
            }
            api = GXAPI_D3D11;
            GxSetApi(GXAPI_D3D11);
            GxLogFlushNow();
            continue;
        }
        if (api == GXAPI_D3D11) {
            GxLog("Trying DX11 Legacy");
            api = GXAPI_D3D11_LEGACY;
            GxSetApi(GXAPI_D3D11_LEGACY);
            GxLogFlushNow();
            continue;
        }
        if (window->m_maximize == 0) {
            break;
        }
        GxLog("Trying windowed mode");
        window->m_maximize = 0;
        GxLogFlushNow();
    }
    GxLog("Giving up");
    GxLogFlushNow();
    return false;
}

// ref: FUN_140328770
bool ConsoleDeviceInitialize(const char* title) {
    ErrorReportAddModuleCrashHandler("atidxx64", OnDriverCrash);
    GxLogOpen();
    RegisterDeviceCVars();
    uint8_t forceApi = GXAPI_COUNT;
    if (CommandLineHasOption(2)) {
        forceApi = GXAPI_D3D12;
    } else if (CommandLineHasOption(1)) {
        forceApi = GXAPI_D3D11;
    } else if (CommandLineHasOption(4)) {
        forceApi = GXAPI_D3D11_LEGACY;
    }
    if (!GxInitialize(forceApi)) {
        const char* caption = GlobalStringsGet(1, "World of Warcraft");
        const char* text = GlobalStringsGet(4,
            "Your 3D accelerator card is not supported by World of Warcraft. For more information please see "
            "https://us.battle.net/support/en/article/world-of-warcraft-system-requirements");
        OsMessageBox(nullptr, 1, text, caption);
        GxLogClose();
        return false;
    }
    blz::function_storage noGpu = { reinterpret_cast<void*>(&OnNoGpuFound), blz::function_trivial_manager, {} };
    GxSetNoGpuHandler(&noGpu);
    s_hwDetectCVar = CVarRegister("hwDetect", "do hardware detection", 1, "1", static_cast<CVARCALLBACK>(nullptr), 1,
        false, nullptr, false);
    s_hwDetect = s_hwDetectCVar->m_intValue != 0;
    if (s_hwDetect != 0) {
        CVarSet(s_hwDetectCVar, "0", true, false, false, true);
    }
    RegisterWindowCVars();
    RegisterDeviceCommands();
    if (s_hwDetect != 0) {
        GxLog("AutoDetect: hwDetect = %d", s_hwDetect);
        GXWINDOWPARAMS window = s_defaultWindow;
        window.m_maximize = s_maximizeCVar->m_intValue != 0;
        SaveSettings(&s_defaultDevice, &window);
    }
    if (s_afterHwDetect != nullptr) {
        s_afterHwDetect();
    }
    if (CommandLineHasOption(8)) {
        CVarSet(s_maximizeCVar, "1", true, false, false, true);
    } else if (CommandLineHasOption(0x10)) {
        CVarSet(s_maximizeCVar, "0", true, false, false, true);
    }
    GXDEVICEPARAMS device = { 1, 0, 0, 3, 0, { 0, 0, 0 }, {} };
    GXWINDOWPARAMS window = { { 0, 0 }, { 0, 0 }, 0, 1, 1, 1, 1, 1, { 0, 0, 0 }, 0 };
    ConsoleDeviceGetParams(&device, &window);
    WindowCreate(&window);
    if (!ConsoleDeviceCreate(&device, &window)) {
        ReportDeviceCreationFailure();
        WindowDestroy();
        GxShutdown();
        GxLogClose();
        return false;
    }
    if (!DetectDefaults(&s_detected, static_cast<uint32_t>(s_monitorCVar->m_intValue))) {
        GxDevDestroy();
        GxShutdown();
        GxLogClose();
        return false;
    }
    SaveSettings(&device, &window);
    SaveWindowedResolution();
    const char* name = title != nullptr ? title : "";
    strncpy_s(s_windowTitle, sizeof(s_windowTitle), name, _TRUNCATE);
    void* handle = WindowGetHandle();
    if (handle != nullptr) {
        WindowSetTitle(handle, s_windowTitle);
    }
    char version[0x20];
    SStrPrintf(version, sizeof(version), "%d", 0x25);
    CVarSet(s_videoOptionsVersionCVar, version, true, false, false, true);
    GxAddDisplayChangeCallback(OnDisplayChange, nullptr);
    return true;
}

// Whether the hardware detection ran this start (hwDetect was set).
// ref: FUN_140328760
bool ConsoleDeviceHwDetected() {
    return s_hwDetect != 0;
}

// Whether an old videoOptionsVersion put gxApi back to auto this start.
// ref: FUN_140329220
bool ConsoleDeviceApiWasReset() {
    return s_apiReset != 0;
}

// ref: FUN_140328750
CONSOLEDETECTED* ConsoleGetDetected() {
    return &s_detected;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14024cf80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1402651b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140326f60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140326f90=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142289c30=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_14032a310 = (void*)&SaveWindowedResolution;
extern "C" void* const gen_f_14032a380 = (void*)&SaveSettings;
extern "C" void* const gen_f_14032b0a0 = (void*)&DetectDefaults;
extern "C" void* const gen_p_142e30204 = (void*)&s_restarting;
extern "C" void* const gen_p_142e30220 = (void*)&s_restartCount;
extern "C" void* const gen_p_142e30240 = (void*)&s_apiCVar;
extern "C" void* const gen_p_142e30248 = (void*)&s_monitorCVar;
extern "C" void* const gen_p_142e30278 = (void*)&s_maximizeCVar;
extern "C" void* const gen_p_142e30298 = (void*)&s_forceMaxSizeCVar;
extern "C" void* const gen_p_142e302a8 = (void*)&s_notchedDisplayCVar;
// ---- end of generated globals ----
