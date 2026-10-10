#ifndef GX_GX_HPP
#define GX_GX_HPP

// Gx: the engine's graphics layer over Prism (Engine/Source/Gx). GxInitialize brings up the
// Prism instance and picks the API each GPU can run; devices come later.

#include "blz/Function.hpp"
#include "blz/String.hpp"
#include <cstdarg>
#include <cstdint>

#include "prism/Instance.hpp"

struct GxBuf;
class GxRenderContext;

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
bool GxApiAvailable(uint8_t api);
uint8_t GxGetBestApi();
GXADAPTERLIST* GxGetAdapterList();
GxAdapterInfo* GxSelectGpuToUse();
void GxSetAfrDeviceCount(uint32_t count);
bool GxGetAllowSoftwareGpu();
void GxSetAllowSoftwareGpu(bool allow);
void GxLogFlushNow();

// What a frame's end hands its callback (0x50 bytes, nanoseconds).
struct GXFRAMESTATS {
    int64_t m_frameTime;            // 0x00: end to end
    int64_t m_beginWait;            // 0x08: from the latency wait to GxFrameBegin
    int64_t m_cpuTime;              // 0x10: GxFrameBegin to the end, less the GPU wait
    uint64_t m_gpuWait;             // 0x18: what the device's FrameEnd waited
    uint64_t m_gpuTime;             // 0x20: from the frame's timer query
    uint64_t m_gpuBegin;            // 0x28: the query's timestamps
    uint64_t m_gpuEnd;              // 0x30
    int64_t m_presentTime;          // 0x38: the device's (+0x150)
    int64_t m_waited;               // 0x40: the latency wait's sleep
    uint32_t m_latencyFrames;       // 0x48: the device's (+0x14e)
    uint32_t m_pad4c;
};
static_assert(sizeof(GXFRAMESTATS) == 0x50, "GXFRAMESTATS is 0x50 bytes");

// The frame (GxApi.cpp): the phase flag (FUN_14228b8c0), the device's begin
// (FUN_142289f20), the latency wait (FUN_14228b3a0), arming an overlapped end
// (FUN_14228a950), completing the last end (FUN_14228c1a0), the end with its stats
// callback (FUN_142289f30), whether a frame cap applies (FUN_14228aa60) and the cap's sleep
// (FUN_14228c9f0).
uint8_t GxFrameSetPhase(uint8_t phase);
bool GxFrameBegin();
// Whether a frame has begun and not yet ended (FUN_14228a040).
bool GxFrameBegan();
void GxFrameLatencyWait();
bool GxFrameArm(bool arm);
void GxFrameWorkFlush(uint8_t mode);
void GxFrameEnd(blz::function_storage* callback, bool overlapped);
bool GxFrameLimited(uint32_t fps);
void GxFrameLimit(uint32_t fps);
uint32_t GxDeviceLostCount();
void GxRequestRestart();

// Textures and texture heaps through the device (FUN_14228c1d0, FUN_14228c390, FUN_14228c490,
// FUN_14228c4c0), what a description would take (FUN_142289a20) and a texture's description
// (FUN_1422884b0).
struct GXTEXALLOCINFO;
bool GxTexCreate(const void* desc, void** tex);
// What a texture update reads: for each level, its pixels and their row pitch
// (param, width, height, face or slice, level, pitch out, data out).
typedef void (*GXTEXUPDATEPROC)(void* param, uint32_t width, uint32_t height, uint32_t slice, uint32_t mip,
    uint32_t* pitch, const void** data);
// A buffer update's writer (param, the buffer's memory).
typedef void (*GXBUFUPDATEPROC)(void* param, void* data);

// The device's upload interface (DAT_14348b648): contents written outside the frame.
class GxUploadInterface {
    public:
    virtual void BufUpdate(GxBuf* buf, uint32_t size, GXBUFUPDATEPROC callback, void* param) = 0;   // 0x00
    virtual void BufWrite(GxBuf* buf, const void* data, uint32_t size) = 0;                          // 0x08
    virtual void TexUpdate(void* tex, const int32_t* rect, bool shaderResource, GXTEXUPDATEPROC callback,
        void* param) = 0;                                                                             // 0x10
};
extern GxUploadInterface* g_gxUploadInterface;

// A texture's levels from `callback`, on the upload interface. `rect` is {top, left, bottom,
// right}; with `shaderResource` the texture is left ready to sample.
void GxTexUpdate(GxUploadInterface* upload, void* tex, const int32_t* rect, bool shaderResource,
    GXTEXUPDATEPROC callback, void* param);
// The same in the frame, on a render context.
void GxTexUpdateInFrame(GxRenderContext* context, void* tex, const int32_t* rect, bool shaderResource,
    GXTEXUPDATEPROC callback, void* param);
void GxTexDestroy(void** tex);
// A texture's filter and anisotropy (FUN_142288460).
void GxTexSetFilter(void* tex, uint8_t filter, uint32_t anisotropy);
// The device drained before every texture is made again, and refilled after
// (FUN_142289ef0, FUN_14228b950).
void GxDeviceBeginTextureReload();
void GxDeviceEndTextureReload();
bool GxTexHeapCreate(const void* desc, void** heap);
void GxTexHeapDestroy(void* heap);
GXTEXALLOCINFO* GxTexAllocationInfo(GXTEXALLOCINFO* out, const void* desc);
void* GxTexGetDesc(const void* tex, void* out);

// Once-a-frame Gx work (FUN_14228c670) and the callbacks it runs (FUN_14228e190, FUN_14228e8f0).
void GxFrameUpdate();
void GxAddFrameStartCallback(void (*callback)(void*), void* param);
void GxRemoveFrameStartCallback(void (*callback)(void*), void* param);
void GxRemoveEngineDeviceCreatedCallback(void (*callback)(void*), void* param);
void GxRemoveLowMemoryCallback(void (*callback)(void*), void* param);

// Device queries (GxApi.cpp): the low-latency mode (FUN_14228b810), video memory in MB
// (FUN_14228aa40), multisampling (FUN_142289e20), variable-rate shading (FUN_14228ac40), the
// legacy and D3D11 APIs (FUN_142291a50, FUN_1422919f0), the capability block
// (FUN_14228a0b0) and the frame number (FUN_14228a030).
void GxSetLowLatencyMode(uint8_t mode);
void GxDeviceVideoMemory(uint32_t* dedicated, uint32_t* shared);
bool GxFormatSupportsMultisample(uint8_t format, uint8_t samples, int32_t quality);
bool GxShadingRateSupported(uint32_t rate, int32_t a2, uint8_t a3);
bool GxApiIsLegacy();
bool GxApiIsD3d11();
bool GxVariableRateShadingSupported(uint8_t tier);
uint8_t GxCurrentApi();
uint8_t* GxDeviceCaps();
uint32_t GxDeviceFrame();

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
// A vertex format's layouts and how many there are, 0 to 2 (FUN_14228a980).
struct GXVERTEXBUFFERFORMAT;
uint32_t GxVertexFormatLayouts(uint8_t format, GXVERTEXBUFFERFORMAT*** layouts);
// Whether the device reaches a feature level (its +0x80) or shader model (+0x84); 0 asks
// nothing (FUN_14228c060).
bool GxDeviceSupports(uint32_t featureLevel, uint8_t shaderModel);
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

// Releases of device objects through three of the device's slots (0x98, 0xe0, 0xf8), what
// the world renderer holds its streamed buffers and captures in. The holder is passed along
// with the object; the slots are not ported, so a live object reports itself.
// ref: FUN_142288440
void GxDeviceRelease098(void* holder, void* object);
// ref: FUN_1422883e0
void GxDeviceRelease0e0(void* holder, void* object);
// ref: FUN_142288400
void GxDeviceRelease0f8(void* holder, void* object);

// A texture from its parameters (the description FUN_14228c330 builds on the stack): kind,
// size, format and mip count, creation flags. The name is for the debugger and is dropped.
// ref: FUN_14228c330
bool GxTexCreateParams(uint8_t kind, uint32_t width, uint32_t height, uint32_t depth, uint8_t format,
    uint8_t mips, uint64_t flags, const char* name, void** tex);

// The device's queries (slots 0x278..0x288).
// ref: FUN_14228b670
void* GxQueryCreate(uint8_t type);
// ref: FUN_14228b690
void GxQueryDestroy(void* query);
// ref: FUN_14228b6e0
bool GxQueryInfo(void* query, uint8_t what, uint32_t* out);

// How many frames the device buffers (+0x144), and which of them is being recorded (+0x13c).
// ref: FUN_142288550
uint32_t GxDeviceBufferedFrames();
// ref: FUN_142288530
uint32_t GxDeviceFrameSlot();

// Whether the frame's phase leaves the frame open (false only between a frame's start and the
// scene's draw).
// ref: FUN_14228aa10
bool GxFramePhaseOpen();
// The device's last frame time (+0x158), 0 without a device.
// ref: FUN_14228a9b0
uint32_t GxDeviceFrameTime();
// The viewport the scene draws in, as fractions of the target (0..1 each by default).
// ref: FUN_14228cbd0
void GxXformGetViewport(float* minX, float* maxX, float* minY, float* maxY, float* minZ, float* maxZ);
// The viewport into the render context (clamped to the target), and the frame's phase reset.
// ref: FUN_14228cb00
void GxXformApplyViewport();

#endif
