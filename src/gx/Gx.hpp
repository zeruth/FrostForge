#ifndef GX_GX_HPP
#define GX_GX_HPP

// Gx: the engine's graphics layer over Prism (Engine/Source/Gx). GxInitialize brings up the
// Prism instance and picks the API each GPU can run; devices come later.

#include "blz/Function.hpp"
#include "blz/String.hpp"
#include <cstdarg>
#include <cstdint>

#include "prism/Instance.hpp"

// The APIs Gx can drive (the names at 0x142910f90); GXAPI_COUNT also means "none".
enum : uint8_t {
    GXAPI_D3D11_LEGACY = 0,
    GXAPI_D3D11 = 1,
    GXAPI_D3D12 = 2,
    GXAPI_MTL_LEGACY = 3,
    GXAPI_MTL = 4,
    GXAPI_COUNT = 5,
};

// One usable GPU (GxAdapterInfo, 0x78 bytes): its display name ("<name> [n]" when several
// share a name), the Prism GPU and the device data of the API chosen for it.
struct GxAdapterInfo {
    blz::inline_string<0x40> m_name;  // 0x00
    pm::GPU* m_gpu;                 // 0x58
    pm::APIDEVICEDATA* m_data;      // 0x60
    pm::GPU* m_handle;              // 0x68: as enumerated
    uint8_t m_api;                  // 0x70: GXAPI_*, GXAPI_COUNT when none
    uint8_t m_supported[GXAPI_COUNT];  // 0x71
    uint8_t m_pad76[2];
};
static_assert(sizeof(GxAdapterInfo) == 0x78, "GxAdapterInfo is 0x78 bytes");

// blz::vector<GxAdapterInfo> (DAT_143490680).
struct GXADAPTERLIST {
    GxAdapterInfo* m_data;
    uint64_t m_size;
    uint64_t m_capacity;            // bit 63: the storage is not owned
};
extern GXADAPTERLIST g_gxAdapters;

extern pm::Instance* g_gxPrismInstance;  // DAT_143490868
extern pm::PLATFORMHANDLES g_gxPlatform;  // DAT_143490a70: what the platform loaded (Prism slot 0x30)
extern void* g_gxDevice;                  // DAT_14348b650: the CGxDevice

// A size in pixels (GxSize): width, then height.
struct GXSIZE {
    uint16_t m_width;
    uint16_t m_height;
};

// What the console hands the device (0x60 bytes): sync, shader residency, the monitor, how far
// the CPU may run ahead, crash capture, and the GPU by name (empty: let Gx choose).
struct GXDEVICEPARAMS {
    uint8_t m_vsync;                // 0x00
    uint8_t m_cachelessShaders;     // 0x01
    uint8_t m_monitor;              // 0x02
    uint8_t m_maxFrameLatency;      // 0x03: 1..3
    uint8_t m_aftermath;            // 0x04
    uint8_t m_pad05[3];
    blz::inline_string<0x40> m_adapterName;  // 0x08
};
static_assert(sizeof(GXDEVICEPARAMS) == 0x60, "GXDEVICEPARAMS is 0x60 bytes");

// The window the console asks for (0x18 bytes).
struct GXWINDOWPARAMS {
    GXSIZE m_size;                  // 0x00: the window (the desktop when maximized)
    GXSIZE m_resolution;            // 0x04: the back buffer
    uint32_t m_monitor;             // 0x08
    uint8_t m_aspect;               // 0x0c: gxAspect
    uint8_t m_vsync;                // 0x0d
    uint8_t m_maximize;             // 0x0e: gxMaximize (fullscreen window)
    uint8_t m_sizeMatches;          // 0x0f: the back buffer is the window's size
    uint8_t m_keepUiPosition;       // 0x10: NotchedDisplayMode is not 2
    uint8_t m_pad11[3];
    uint32_t m_14;                  // 0x14
};
static_assert(sizeof(GXWINDOWPARAMS) == 0x18, "GXWINDOWPARAMS is 0x18 bytes");

// gx.log (Logs\gx.log), capped at 8 MB (GxLog.cpp).
void GxLogOpen();
void GxLogClose();
void GxLogFlush();
void GxLog(const char* format, ...);
void GxLogV(const char* format, va_list args);

// Registers the CVars, logs the machine, creates the Prism instance and builds the adapter
// list. `forceApi` is a GXAPI_*, GXAPI_COUNT to let Gx choose. False when no GPU can run
// any API.
bool GxInitialize(uint8_t forceApi);

bool GxBuildAdapterList();

uint8_t GxGetApi();
void GxSetApi(uint8_t api);
bool GxApiSupported(uint8_t api);
const char* GxApiName(uint8_t api);
uint8_t GxGetBestApi();
GXADAPTERLIST* GxGetAdapterList();
GxAdapterInfo* GxSelectGpuToUse();
void GxSetAfrDeviceCount(uint32_t count);
bool GxGetAllowSoftwareGpu();
void GxSetAllowSoftwareGpu(bool allow);
void GxLogFlushNow();

// The console's handlers (blz::function, moved in).
void GxSetRestartHandler(blz::function_storage* handler);
void GxSetDisplayChangedHandler(blz::function_storage* handler);
void GxSetNoGpuHandler(blz::function_storage* handler);

bool GxDevCreate(uint8_t api, const GXDEVICEPARAMS* params);
void GxDevDestroy();

// Undoes GxInitialize: the hooks, the platform handles, the Prism instance.
void GxShutdown();

// The monitors (GxDeviceW32.cpp): DXGI's outputs, the primary first.
uint32_t GxMonitorCount();
void* GxMonitorGet(uint32_t index);
bool GxMonitorGetDesktopSize(uint32_t index, GXSIZE* size, uint32_t* bitsPerPixel);
// Forgets the monitors, to be enumerated again on next use.
void GxMonitorsReset();

// Whether a device exists and is set up (FUN_142289a70).
bool GxDevExists();
// Whether there is a device at all (FUN_142289e10).
bool GxDevicePresent();

// Callbacks for device creation (run at once when a device exists) and destruction.
typedef void (*GXDEVICECALLBACK)(void* param);
void GxAddDeviceCreatedCallback(GXDEVICECALLBACK callback, void* param);
void GxAddDeviceDestroyedCallback(GXDEVICECALLBACK callback, void* param);
void GxRemoveDeviceCreatedCallback(GXDEVICECALLBACK callback, void* param);
void GxRemoveDeviceDestroyedCallback(GXDEVICECALLBACK callback, void* param);

// Callbacks told when the displays change, with the monitor in use.
typedef void (*GXDISPLAYCALLBACK)(uint32_t monitor, void* param);
void GxAddDisplayChangeCallback(GXDISPLAYCALLBACK callback, void* param);
void GxNotifyOnDisplayChange(uint32_t monitor);

#endif
