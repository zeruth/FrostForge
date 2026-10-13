#include "gx/Gx.hpp"
#include "gx/GxDevice.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "storm/String.hpp"
#include "threadpool/ThreadPool.hpp"
#include <cstdarg>
#include <intrin.h>
#include "blz/Sync.hpp"
#include "client/ConsoleDevice.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "os/Os.hpp"
#include "os/Window.hpp"
#include "prism/Instance.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

GXADAPTERLIST g_gxAdapters;
pm::Instance* g_gxPrismInstance;
pm::PLATFORMHANDLES g_gxPlatform;
void* g_gxDevice;

namespace {

const char s_gxApiFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\GxApi.cpp";
const char s_adapterInfoTag[] = ".?AUGxAdapterInfo@@";
const char s_callbackTag[] = ".?AU?$pair@P6AXPEAX@ZPEAX@blz@@";
const char s_callbackMapTag[] = ".PEAU?$pair@P6AXPEAX@ZPEAX@blz@@";
const char s_stringTag[] = ".D";
const char s_displayCallbackTag[] = ".?AU?$pair@P6AXIPEAX@ZPEAX@blz@@";
const char s_displayCallbackMapTag[] = ".PEAU?$pair@P6AXIPEAX@ZPEAX@blz@@";

const char* const s_apiNames[GXAPI_COUNT] = { "D3D11_LEGACY", "D3D11", "D3D12", "MTL_LEGACY", "MTL" };  // 0x142910f90

// The Gx state the crash dump keeps (DAT_14348b680, up to the adapter list). Its fields are
// not broken out yet; the block stands where they live so the range registered is the same.
uint8_t s_gxStateBlock[0x5000];

uint8_t s_forcedApi = GXAPI_COUNT;  // DAT_142d41c19
uint8_t s_onAcPower;                // DAT_142d41c1b
uint8_t s_allowSoftwareGpu;         // DAT_14349069d
uint8_t s_useMaxFps;                // DAT_14349069f
uint8_t s_apiSupported[GXAPI_COUNT];  // DAT_143490808
uint8_t s_useMaxFpsBk;              // DAT_14349080d
uint8_t s_apiAvailable[GXAPI_COUNT];  // DAT_143490810
CVar* s_adapterCVar;                // DAT_143490818: gxAdapter
CVar* s_prismEnabledCVar;           // DAT_143490820: GxPrismEnabled
CVar* s_driverVersionCheckCVar;     // DAT_143490828: DriverVersionCheck
CVar* s_frameEndModeCVar;           // DAT_143490830: GxFrameEndMode
uint64_t s_amdDriverVersion;        // DAT_143490838
uint64_t s_intelDriverVersion;      // DAT_143490840
uint64_t s_nvidiaDriverVersion;     // DAT_143490848
uint8_t s_driverChanged;            // DAT_143490850: a vendor's driver differs from the last GPU's
int32_t s_maxFps;                   // DAT_14349085c
int32_t s_maxFpsBk;                 // DAT_143490860
uint32_t s_mainThreadId;            // DAT_143490870

// The entry points Gx takes from what the platform loaded.
void* s_dxgiGetDebugInterface1;     // DAT_143490a30
void* s_d3d11CreateDevice;          // DAT_143490a38
void* s_d3d12CreateDevice;          // DAT_143490a40
void* s_d3d12GetDebugInterface;     // DAT_143490a48
void* s_d3d12SerializeRootSignature;  // DAT_143490a50
void* s_d3d12SerializeVersionedRootSignature;  // DAT_143490a58
void* s_d3d12CreateVersionedRootSignatureDeserializer;  // DAT_143490a60
IDXGIFactory5* s_factory5;          // DAT_143490ad0
IDXGIFactory6* s_factory6;          // DAT_143490ad8

const char s_frameQueryTag[] = ".?AV?$unique_ptr@UGxQuery@@UGxResourceDelete@@@blz@@";
const char s_frameGroupTag[] = ".PEAUThreadPoolGroup@@";

// The frame-end helper (DAT_142d41b30): the frame's begin and end, its timings, and the
// optional overlapped end on a pool thread.
struct GXFRAMEEND {
    uint8_t m_began;                // 0x00: between GxFrameBegin and the end's completion
    uint8_t m_busy;                 // 0x01: inside GxFrameBegin
    uint8_t m_pad02[6];
    THREADPOOLGROUP* m_group;       // 0x08: the overlapped end's pool group
    GXFRAMESTATS m_stats;           // 0x10: handed to the end callback
    int64_t m_lastEnd;              // 0x60: counter at the last end
    int64_t m_beginStamp;           // 0x68: counter at GxFrameBegin
    int64_t m_waitEnd;              // 0x70: counter after the latency wait
    int64_t m_lastReport;           // 0x78: counter at the last GPU status report
    int64_t m_waited;               // 0x80: nanoseconds the latency wait slept
    uint8_t m_overlapActive;        // 0x88: this frame ends on a pool thread
    uint8_t m_overlap;              // 0x89: GxFrameEndMode (always 0: the feature is not available)
    uint8_t m_pad8a[2];
    uint32_t m_frame;               // 0x8c: the device's frame number at the end
    volatile uint8_t m_pending;     // 0x90: 1 ending, 2 overlapped, 4 ended, 8 stats taken
    uint8_t m_pad91[7];
    blz::function_storage m_callback = { nullptr, blz::function_empty_manager, {} };  // 0x98: run with the stats
    blz::vector<THREADPOOLGROUP*, s_frameGroupTag> m_waits;     // 0xb8: drained every wait
    blz::vector<THREADPOOLGROUP*, s_frameGroupTag> m_lateWaits; // 0xd0: drained on a full wait
};
static_assert(offsetof(GXFRAMEEND, m_stats) == 0x10, "GXFRAMEEND layout");
static_assert(offsetof(GXFRAMEEND, m_lastEnd) == 0x60, "GXFRAMEEND layout");
static_assert(offsetof(GXFRAMEEND, m_frame) == 0x8c, "GXFRAMEEND layout");
static_assert(offsetof(GXFRAMEEND, m_callback) == 0x98, "GXFRAMEEND layout");
static_assert(sizeof(GXFRAMEEND) == 0xe8, "GXFRAMEEND is 0xe8 bytes");
GXFRAMEEND s_frameEnd;

// The timer queries frames begin and end with, one per frame in flight (PTR_DAT_142d43860).
blz::vector<void*, s_frameQueryTag> s_frameQueries;

void* FrameQuery(CGxDevice* device) {
    return s_frameQueries.m_data[device->Field<uint32_t>(0x138) % s_frameQueries.m_size];
}

// blz::deque<blz::pair<void (*)(void*), void*>>: a map of blocks of eight.
struct CALLBACKPAIR {
    GXDEVICECALLBACK m_callback;
    void* m_param;
};
struct CALLBACKDEQUE {
    CALLBACKPAIR** m_map;           // 0x00
    uint64_t m_mapSize;             // 0x08: a power of two
    uint64_t m_offset;              // 0x10
    uint64_t m_size;                // 0x18
};
CALLBACKDEQUE g_gxDeviceCreatedCallbacks;    // DAT_1434908f8
CALLBACKDEQUE g_gxDeviceDestroyedCallbacks;  // DAT_143490918

// Gx's allocator for Prism (vtable 0x142910ef8, PTR_PTR_142d41c68): Storm under GxApi.cpp.
class GxPrismAllocator : public blz::allocator {
    public:
    // ref: FUN_1420dad20
    void* allocate_sized(uint64_t* allocated, uint64_t size, uint64_t alignment) override {
        *allocated = size;
        return this->allocate(size, alignment);
    }
    // ref: FUN_142287da0
    void* allocate(uint64_t size, uint64_t alignment) override {
        if (alignment > 0x10) {
            return SMemAllocAligned(size, alignment, 0xd, s_gxApiFile, SStrHashFnv1a(s_gxApiFile), 0x249, 0);
        }
        return SMemAlloc(size, 0xd, s_gxApiFile, SStrHashFnv1a(s_gxApiFile), 0x24b, 0);
    }
    // ref: FUN_142287e50
    void* reallocate(void* ptr, uint64_t size, uint64_t alignment) override {
        if (alignment < 0x11) {
            return SMemReAlloc(ptr, size, 0xd, s_gxApiFile, SStrHashFnv1a(s_gxApiFile), 0x252, 0);
        }
        return SMemReAllocAligned(ptr, size, alignment, 0xd, s_gxApiFile, SStrHashFnv1a(s_gxApiFile), 0x250, 0);
    }
    // ref: FUN_1420dae20 (a block that exists is only ever freed through here)
    void* reallocate_sized(void* ptr, uint64_t* allocated, uint64_t size, uint64_t alignment) override {
        if (ptr == nullptr) {
            return this->allocate_sized(allocated, size, alignment);
        }
        if (size == 0) {
            this->deallocate(ptr);
        }
        return nullptr;
    }
    // ref: FUN_142287f20
    void deallocate(void* ptr) override {
        SMemFree(ptr, s_gxApiFile, 0x25a, 0);
    }
    // ref: 0x142287f40
    uint64_t size(void* ptr) override {
        return SMemSize(ptr);
    }
};
GxPrismAllocator s_prismAllocator;  // PTR_PTR_142d41c68

// Writes Prism's reports to gx.log, with the system's text for an HRESULT.
// ref: FUN_14228d420
void GxPrismReport(pm::RESULT result, const char* text) {
    uint32_t hresult = static_cast<uint32_t>(result.m_hresult);
    if (result.m_category == 4) {
        char* message = nullptr;
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, hresult, LANG_USER_DEFAULT, reinterpret_cast<char*>(&message), 0, nullptr);
        if (message == nullptr) {
            message = static_cast<char*>(LocalAlloc(LMEM_FIXED, 0x20));
            if (message != nullptr) {
                uint16_t code = static_cast<uint16_t>(hresult) - 0x200;
                if (hresult + 0x7ffbfe00u < 0xfe00u && code != 0) {
                    _snprintf_s(message, 0x20, _TRUNCATE, "IDispatch error #%d", code);  // FUN_14025a140
                } else {
                    _snprintf_s(message, 0x20, _TRUNCATE, "Unknown error 0x%0lX", hresult);
                }
            }
        } else {
            size_t length = std::strlen(message);
            if (length > 1 && message[length - 1] == '\n') {
                message[length - 1] = '\0';
                if (message[length - 2] == '\r') {
                    message[length - 2] = '\0';
                }
            }
        }
        GxLog("%s: %s (HR:0x%08X) %s", pm::ResultCodeName(result.m_code), text, hresult, message);
        if (message != nullptr) {
            LocalFree(message);
        }
    } else {
        GxLog("%s: %s (%s:0x%08X)", pm::ResultCodeName(result.m_code), text, pm::ResultSourceName(result.m_category),
            hresult);
    }
}

// Gx's messenger for Prism (vtable 0x142910ee8, PTR_PTR_142d41c60).
class GxPrismMessenger : public pm::Messenger {
    public:
    // ref: 0x142287d90
    void Report(uint8_t severity, const char* function, pm::RESULT result, uint32_t flags, const char* text) override {
        (void)severity;
        (void)function;
        (void)flags;
        GxPrismReport(result, text);
    }
};
GxPrismMessenger s_prismMessenger;

// The pipeline work queue Prism is handed (DAT_1434906a0, built by the static initializer
// FUN_14020b610): the allocator, a dispatcher that hands jobs to the thread pool, a list, and
// a manual-reset event.
class GxPrismDispatcher {
    public:
    // ref: FUN_1405dc900
    virtual ~GxPrismDispatcher() {}
    // ref: FUN_142287f60
    virtual void Dispatch(int32_t priority, void* job, void* param) {
        (void)priority;
        (void)job;
        (void)param;
        FROST_UNIMPLEMENTED();  // FUN_142359cf0: the thread pool is not ported
    }
};
GxPrismDispatcher s_prismDispatcher;  // PTR_PTR_142d41c70

struct PRISMTASKQUEUE {
    blz::allocator* m_allocator;    // 0x00
    GxPrismDispatcher* m_dispatcher;  // 0x08
    void* m_listHead;               // 0x10: FUN_142369d80, an empty list pointing at +0x18
    void* m_list[4];                // 0x18
    void* m_listTail;               // 0x38
    uint8_t m_40[0x30];
    void* m_70;                     // 0x70
    HANDLE m_event;                 // 0x78
    uint32_t m_80;                  // 0x80
};

// ref: FUN_142369000
PRISMTASKQUEUE* PrismTaskQueueConstruct(PRISMTASKQUEUE* queue, blz::allocator* allocator, GxPrismDispatcher* dispatcher) {
    if (queue != nullptr) {
        queue->m_allocator = allocator;
        queue->m_dispatcher = dispatcher;
        queue->m_listHead = &queue->m_list[0];
        queue->m_list[0] = nullptr;
        queue->m_list[1] = nullptr;
        queue->m_list[2] = nullptr;
        queue->m_list[3] = nullptr;
        queue->m_listTail = &queue->m_list[0];
        queue->m_80 = 1;
        queue->m_70 = nullptr;
        // FUN_142366350: a manual-reset event, not signaled; the process dies without one
        queue->m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (queue->m_event == nullptr) {
            OsTerminateProcess();
        }
    }
    return queue;
}

// The static initializer FUN_14020b610 (its atexit teardown, LAB_142601d60, is not ported).
struct PRISMTASKQUEUESTATIC {
    PRISMTASKQUEUE m_queue;
    PRISMTASKQUEUESTATIC() {
        PrismTaskQueueConstruct(&this->m_queue, &s_prismAllocator, &s_prismDispatcher);
    }
};
PRISMTASKQUEUESTATIC s_prismTaskQueue;  // DAT_1434906a0

// ref: FUN_142291c50
void GxFrameEndInitialize(GXFRAMEEND* frameEnd) {
    InterlockedExchange8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), 0);
    if (frameEnd->m_group == nullptr) {
        frameEnd->m_group = ThreadPoolGroupCreate(2);
    }
}

// ref: FUN_1422927c0
void GxFrameEndSetMode(GXFRAMEEND* frameEnd) {
    frameEnd->m_overlap = 0;
}

// The blocks of a callback deque (FUN_14228ed20) and its map's growth (FUN_14228ede0).
CALLBACKPAIR* CallbackSlot(CALLBACKDEQUE* deque, uint64_t index, const char* blockTag = s_callbackTag) {
    CALLBACKPAIR** block = &deque->m_map[(deque->m_mapSize - 1) & (index >> 3)];
    if (*block == nullptr) {
        *block = static_cast<CALLBACKPAIR*>(SMemAlloc(0x80, 0xb, blockTag, SStrHashFnv1a(blockTag), -6, 0));
    }
    return &(*block)[index & 7];
}

// ref: FUN_14228ede0
void CallbackGrowMap(CALLBACKDEQUE* deque, const char* mapTag = s_callbackMapTag) {
    uint64_t size = deque->m_mapSize;
    if (size == 0) {
        size = 8;
    } else {
        uint64_t wanted = size + 1;
        while (size < wanted) {
            size *= 2;
        }
    }
    auto map = static_cast<CALLBACKPAIR**>(SMemAlloc(size * 8, 0xb, mapTag, SStrHashFnv1a(mapTag), -6, 0));
    uint64_t first = (deque->m_offset >> 3) & (deque->m_mapSize - 1);
    std::memcpy(map, deque->m_map + first, (deque->m_mapSize - first) * 8);
    std::memcpy(map + (deque->m_mapSize - first), deque->m_map, first * 8);
    std::memset(map + deque->m_mapSize, 0, (size - deque->m_mapSize) * 8);
    SMemFree(deque->m_map, mapTag, -6, 0);
    deque->m_offset &= 7;
    deque->m_map = map;
    deque->m_mapSize = size;
}

// Logs the AC and battery state (FUN_14228cf80).
void GxLogPowerStatus(const OSPOWERSTATUS* status, bool changed) {
    blz::inline_string<0x11> percent;
    if ((status->m_percentFlags & 2) != 0) {
        blz::string_format(percent.header(), " (%d%%)", status->m_batteryPercent);
    }
    const char* battery = "Unknown";
    if ((status->m_batteryFlags & 2) != 0) {
        battery = status->m_batteryPresent != 0 ? "Present" : "Not Present";
    }
    const char* power = "Power State Unknown";
    if ((status->m_acFlags & 2) != 0) {
        power = status->m_onAc != 0 ? "On AC Power" : "On Battery";
    }
    GxLog("%s%s - Battery %s%s", changed ? "Power State Changed: " : "", power, battery, percent.m_data);
}

// ref: FUN_14228d0c0
void GxOnSuspend() {
    GxLog("System will sleep notification");
    GxLogFlush();
}

// ref: FUN_14228d0e0
void GxOnResume() {
    GxLog("System did wake notification");
    GxLogFlush();
}

// ref: FUN_14228d080
void GxOnPowerSettingChange() {
    OSPOWERSTATUS status = {};
    OsGetPowerStatus(&status);
    if (s_onAcPower != status.m_onAc) {
        s_onAcPower = status.m_onAc;
        GxLogPowerStatus(&status, true);
        GxLogFlush();
    }
}

// One timer query per frame in flight, when the device has them.
// ref: FUN_1422918f0
void GxOnDeviceCreated(void* param) {
    (void)param;
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    s_frameEnd.m_lastReport = 0;
    uint32_t count = device->FrameCount();
    // FUN_1422935a0: blz::vector<unique_ptr<GxQuery>>::resize
    for (uint64_t i = count; i < s_frameQueries.m_size; i++) {
        if (s_frameQueries.m_data[i] != nullptr) {
            device->QueryDestroy(s_frameQueries.m_data[i]);
        }
    }
    s_frameQueries.resize(count);
    if (device->Field<uint8_t>(0x129) != 0) {
        for (uint64_t i = 0; i < s_frameQueries.m_size; i++) {
            void* query = device->QueryCreate(1);
            void* old = s_frameQueries.m_data[i];
            s_frameQueries.m_data[i] = query;
            if (old != nullptr) {
                device->QueryDestroy(old);
            }
        }
    }
}

// ref: FUN_142291980
void GxOnDeviceDestroyed(void* param) {
    (void)param;
    for (uint64_t i = 0; i < s_frameQueries.m_size; i++) {
        if (s_frameQueries.m_data[i] != nullptr) {
            static_cast<CGxDevice*>(g_gxDevice)->QueryDestroy(s_frameQueries.m_data[i]);
        }
    }
    s_frameQueries.m_size = 0;
}

// A CVar's integer the way the callbacks parse it: "0x" prefixes hex.
long ParseCVarInt(const char* value) {
    int radix = 10;
    if (value != nullptr && value[0] == '0' && ((value[1] + 0xa8u) & 0xdf) == 0) {
        radix = 0x10;
    }
    return std::strtol(value, nullptr, radix);
}

// ref: FUN_142287fd0
bool OnMaxFpsChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    long value = ParseCVarInt(newValue);
    s_maxFps = value > 7 ? static_cast<int32_t>(value) : 8;
    return true;
}

// ref: FUN_142288020
bool OnUseMaxFpsChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    s_useMaxFps = ParseCVarInt(newValue) != 0;
    return true;
}

// ref: FUN_142288070
bool OnMaxFpsBkChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    long value = ParseCVarInt(newValue);
    s_maxFpsBk = value > 7 ? static_cast<int32_t>(value) : 8;
    return true;
}

// ref: FUN_1422880c0
bool OnUseMaxFpsBkChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    s_useMaxFpsBk = ParseCVarInt(newValue) != 0;
    return true;
}

// ref: FUN_142289ff0
bool OnFrameEndModeChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    (void)userData;
    if (newValue != nullptr) {
        std::atoi(newValue);  // the mode (0..2) is read and then not used
    }
    GxFrameEndSetMode(&s_frameEnd);
    return true;
}


// The OS text with the architecture appended, within 0x400 (FUN_142288140 and the inlined
// single-character append before it).
void AppendText(char* text, const char* suffix) {
    size_t used = strnlen(text, 0x400);
    if (used >= 0x400) {
        return;
    }
    size_t length = std::strlen(suffix);
    if (length > 0x3ff - used) {
        length = 0x3ff - used;
    }
    std::memcpy(text + used, suffix, length);
    text[used + length] = '\0';
}

// ref: FUN_142295830
LONG GxOnPresentException(uint32_t code, EXCEPTION_POINTERS* info) {
    (void)info;
    GxLog("Exception caught in SwapChain::Present: code 0x%X", code);
    // FUN_140211ed0 walks the exception's stack and FUN_140213070 formats it; neither is
    // ported, so the trace is empty.
    FROST_UNIMPLEMENTED();
    GxLog("Exception trace:\n%s", "");
    return 1;
}

// Takes the platform's modules and entry points from Prism (FUN_142295a90).
bool GxLoadPlatformHandles(bool logDrivers) {
    g_gxPrismInstance->GetPlatformHandles(&g_gxPlatform);
    if (g_gxPlatform.m_renderDoc != nullptr) {
        // RENDERDOC_API_1_x::SetCaptureFilePathTemplate
        auto setTemplate = *reinterpret_cast<void (**)(const char*)>(static_cast<uint8_t*>(g_gxPlatform.m_renderDoc) + 0x58);
        setTemplate("Screenshots/gpucapture");
    }
    s_dxgiGetDebugInterface1 = nullptr;
    if (g_gxPlatform.m_dxgiModule == nullptr) {
        GxLog("Unable to load \"dxgi.dll\"");
    } else {
        s_dxgiGetDebugInterface1 = reinterpret_cast<void*>(
            GetProcAddress(static_cast<HMODULE>(g_gxPlatform.m_dxgiModule), "DXGIGetDebugInterface1"));
    }
    if (g_gxPlatform.m_d3d11Module == nullptr) {
        GxLog("Unable to load \"d3d11.dll\"");
        s_d3d11CreateDevice = nullptr;
    } else {
        s_d3d11CreateDevice = reinterpret_cast<void*>(
            GetProcAddress(static_cast<HMODULE>(g_gxPlatform.m_d3d11Module), "D3D11CreateDevice"));
        if (s_d3d11CreateDevice == nullptr) {
            GxLog("Unable to GetProcAddress(\"D3D11CreateDevice\")");
            s_d3d11CreateDevice = nullptr;
        }
    }
    const OSVERSION* version = OsGetVersion();
    if (version->m_major > 6 || version->m_minor == 1) {
        auto d3d12 = static_cast<HMODULE>(g_gxPlatform.m_d3d12Module);
        const char* failure = nullptr;
        if (d3d12 == nullptr) {
            failure = "Unable to load \"d3d12.dll\"";
        } else {
            s_d3d12GetDebugInterface = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12GetDebugInterface"));
            if (s_d3d12GetDebugInterface == nullptr) {
                failure = "Unable to GetProcAddress(\"D3D12GetDebugInterface\")";
            } else {
                s_d3d12CreateDevice = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
                if (s_d3d12CreateDevice == nullptr) {
                    failure = "Unable to GetProcAddress(\"D3D12CreateDevice\")";
                } else {
                    s_d3d12SerializeRootSignature = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"));
                    s_d3d12SerializeVersionedRootSignature = reinterpret_cast<void*>(
                        GetProcAddress(d3d12, "D3D12SerializeVersionedRootSignature"));
                    if (s_d3d12SerializeRootSignature != nullptr || s_d3d12SerializeVersionedRootSignature != nullptr) {
                        s_d3d12CreateVersionedRootSignatureDeserializer = reinterpret_cast<void*>(
                            GetProcAddress(d3d12, "D3D12CreateVersionedRootSignatureDeserializer"));
                    } else {
                        failure = "Unable to GetProcAddress(\"D3D12SerializeRootSignature\") or "
                                  "GetProcAddress(\"D3D12SerializeVersionedRootSignature\")";
                    }
                }
            }
        }
        if (failure != nullptr) {
            GxLog(failure);
            s_d3d12CreateDevice = nullptr;
            s_d3d12GetDebugInterface = nullptr;
            s_d3d12SerializeRootSignature = nullptr;
            s_d3d12SerializeVersionedRootSignature = nullptr;
        }
    }
    if (g_gxPlatform.m_dxgiModule == nullptr || g_gxPlatform.m_factory == nullptr) {
        return false;
    }
    auto factory = static_cast<IDXGIFactory1*>(g_gxPlatform.m_factory);
    factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&s_factory5));
    factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&s_factory6));
    if (logDrivers) {
        if (g_gxPlatform.m_nvapi != 0) {
            // FUN_1401a4fe0 (NvAPI_SYS_GetDriverAndBranchVersion). DIVERGED: NVAPI is not
            // vendored, and the instance never reports it loaded.
            FROST_UNIMPLEMENTED();
        }
        if (g_gxPlatform.m_agsContext != nullptr) {
            auto versions = static_cast<const char* const*>(g_gxPlatform.m_agsGpuInfo);
            if (versions[0] != nullptr && versions[0][0] != '\0') {
                GxLog("AMD driver version: %s (AGS %s)", versions[0], versions[1]);
            }
        }
    }
    return g_gxPlatform.m_d3d11Module != nullptr || g_gxPlatform.m_d3d12Module != nullptr;
}

// ref: FUN_142295a20
void GxReleasePlatformHandles() {
    if (s_factory5 != nullptr) {
        s_factory5->Release();
    }
    s_factory5 = nullptr;
    if (s_factory6 != nullptr) {
        s_factory6->Release();
    }
    s_factory6 = nullptr;
    s_d3d11CreateDevice = nullptr;
    s_d3d12CreateDevice = nullptr;
    s_d3d12GetDebugInterface = nullptr;
    s_d3d12SerializeRootSignature = nullptr;
    s_d3d12SerializeVersionedRootSignature = nullptr;
    s_dxgiGetDebugInterface1 = nullptr;
}

// Creates the Prism instance and builds the adapter list (FUN_14228aad0).
bool GxCreatePrismInstance() {
    pm::INSTANCEINFO info = {};
    info.m_allocator = &s_prismAllocator;
    info.m_messenger = &s_prismMessenger;
    info.m_10 = 2;
    info.m_28 = 0xf;
    info.m_appName = "World of Warcraft";
    info.m_appVersion = 0x76c3;
    info.m_engineName = "Gx";
    info.m_engineVersion = 0x76c3;
    info.m_a0 = 8;
    info.m_a4 = 2;
    info.m_a8 = &s_prismTaskQueue.m_queue;
    info.m_presentExceptionHandler = reinterpret_cast<void*>(&GxOnPresentException);
    info.m_f0 = 0x800000;
    info.m_f8 = 1;
    info.m_10c = 0x20000;
    pm::Instance* instance = nullptr;
    pm::RESULT result = pm::CreateInstance(&info, &instance);
    if (result.m_code != 0) {
        GxLog("pmCreateInstance failed");
        return false;
    }
    g_gxPrismInstance = instance;
    bool built = GxBuildAdapterList();
    if (built) {
        GxLoadPlatformHandles(true);
    }
    return built;
}

// GxAdapterInfo's vector growth (FUN_14228d660): the names move over, heap ones by pointer.
void GxAdapterReserve(GXADAPTERLIST* list, uint64_t count) {
    uint64_t capacity = list->m_capacity & ~blz::string::INLINE;
    if (capacity >= count) {
        return;
    }
    uint64_t grown = ((list->m_capacity >> 1) & 0x3FFFFFFFFFFFFFFFull) + capacity;
    if (grown < count) {
        grown = count;
    }
    list->m_capacity = ((list->m_capacity ^ grown) & blz::string::INLINE) ^ grown;
    auto data = static_cast<GxAdapterInfo*>(SMemAlloc(grown * sizeof(GxAdapterInfo), 0xb, s_adapterInfoTag,
        SStrHashFnv1a(s_adapterInfoTag), -6, 0));
    for (uint64_t i = 0; i < list->m_size; i++) {
        GxAdapterInfo* from = &list->m_data[i];
        GxAdapterInfo* to = &data[i];
        to->m_name.m_size = 0;
        to->m_name.m_data = to->m_name.m_inline;
        to->m_name.m_inline[0] = '\0';
        if (from->m_name.IsInline()) {
            to->m_name.m_capacity = blz::string::INLINE | 0x3f;
            blz::string_reserve(to->m_name.header(), from->m_name.m_size);
            std::memcpy(to->m_name.m_data, from->m_name.m_data, from->m_name.m_size);
            to->m_name.m_data[from->m_name.m_size] = '\0';
            to->m_name.m_size = from->m_name.m_size;
        } else {
            to->m_name.m_data = from->m_name.m_data;
            to->m_name.m_size = from->m_name.m_size;
            to->m_name.m_capacity = from->m_name.m_capacity & ~blz::string::INLINE;
            from->m_name.m_capacity = blz::string::INLINE | 0x3f;
        }
        to->m_gpu = from->m_gpu;
        to->m_data = from->m_data;
        to->m_handle = from->m_handle;
        to->m_api = from->m_api;
        std::memcpy(to->m_supported, from->m_supported, sizeof(to->m_supported));
    }
    for (uint64_t i = 0; i < list->m_size; i++) {
        blz::string_free(list->m_data[i].m_name.header());
    }
    if ((list->m_capacity & blz::string::INLINE) == 0 && list->m_data != nullptr) {
        SMemFree(list->m_data, s_adapterInfoTag, -6, 0);
    }
    list->m_capacity &= ~blz::string::INLINE;
    list->m_data = data;
}

// ref: FUN_14228d660
GxAdapterInfo* GxAdapterPush(pm::GPU* gpu, pm::APIDEVICEDATA* data, pm::GPU* handle) {
    if (g_gxAdapters.m_size == (g_gxAdapters.m_capacity & ~blz::string::INLINE)) {
        GxAdapterReserve(&g_gxAdapters, (g_gxAdapters.m_capacity & ~blz::string::INLINE) + 1);
    }
    GxAdapterInfo* info = &g_gxAdapters.m_data[g_gxAdapters.m_size];
    info->m_name.m_size = 0;
    info->m_name.m_data = info->m_name.m_inline;
    info->m_name.m_inline[0] = '\0';
    info->m_name.m_capacity = blz::string::INLINE | 0x3f;
    info->m_handle = handle;
    info->m_gpu = gpu;
    info->m_data = data;
    info->m_api = GXAPI_COUNT;
    std::memset(info->m_supported, 0, sizeof(info->m_supported));
    g_gxAdapters.m_size++;
    return info;
}

void GxAdapterSetName(GxAdapterInfo* info, const char* name) {
    uint64_t length = std::strlen(name);
    blz::string_reserve(info->m_name.header(), length);
    std::memcpy(info->m_name.m_data, name, length);
    info->m_name.m_data[length] = '\0';
    info->m_name.m_size = length;
}

// The crash report's view of the adapter being examined (FUN_142288110).
struct ADAPTERCONTEXT {
    ERRORCONTEXT m_base;
    const char* m_format;           // 0x10
    uint16_t m_device;              // 0x18
    uint16_t m_vendor;              // 0x1a
    uint32_t m_1c;
    const char* m_name;             // 0x20
};

void DescribeAdapter(ERRORCONTEXT* context, uint64_t size, char* buffer) {
    auto adapter = reinterpret_cast<ADAPTERCONTEXT*>(context);
    SStrPrintf(buffer, size, adapter->m_format, adapter->m_name, adapter->m_vendor, adapter->m_device);
}

}

// ref: FUN_142289e10
bool GxDevicePresent() {
    return g_gxDevice != nullptr;
}

// ref: FUN_14228a980
uint32_t GxVertexFormatLayouts(uint8_t format, GXVERTEXBUFFERFORMAT*** layouts) {
    CGxDevice::VERTEXFORMATREF ref;
    static_cast<CGxDevice*>(g_gxDevice)->GetVertexFormat(&ref, format);
    *layouts = ref.m_layouts;
    return ref.m_state;
}

// ref: FUN_14228c060
bool GxDeviceSupports(uint32_t featureLevel, uint8_t shaderModel) {
    auto device = static_cast<uint8_t*>(g_gxDevice);
    if ((featureLevel == 0 || *reinterpret_cast<uint32_t*>(device + 0x80) < featureLevel)
        && (shaderModel == 0 || device[0x84] < shaderModel)) {
        return false;
    }
    return true;
}

// ref: FUN_142289a70
bool GxDevExists() {
    if (g_gxDevice == nullptr) {
        return false;
    }
    return static_cast<uint8_t*>(g_gxDevice)[9] != 0;
}

// ref: FUN_14228dfb0
void GxAddDeviceCreatedCallback(GXDEVICECALLBACK callback, void* param) {
    CALLBACKDEQUE* deque = &g_gxDeviceCreatedCallbacks;
    if (((deque->m_offset + deque->m_size) & 7) == 0 && deque->m_mapSize <= (deque->m_size + 8) >> 3) {
        CallbackGrowMap(deque);
    }
    uint64_t index = deque->m_offset + deque->m_size;
    deque->m_size++;
    CALLBACKPAIR* slot = CallbackSlot(deque, index);
    if (slot != nullptr) {
        slot->m_callback = callback;
        slot->m_param = param;
    }
    if (GxDevExists()) {
        callback(param);
    }
}

// ref: FUN_14228e040
void GxAddDeviceDestroyedCallback(GXDEVICECALLBACK callback, void* param) {
    CALLBACKDEQUE* deque = &g_gxDeviceDestroyedCallbacks;
    if ((deque->m_offset & 7) == 0 && deque->m_mapSize <= (deque->m_size + 8) >> 3) {
        CallbackGrowMap(deque);
    }
    deque->m_size++;
    deque->m_offset--;
    CALLBACKPAIR* slot = CallbackSlot(deque, deque->m_offset);
    if (slot != nullptr) {
        slot->m_callback = callback;
        slot->m_param = param;
    }
}

// The usable GPUs and the API each runs: on Windows D3D11 at feature level 10_0 or better,
// and D3D12 at 11_0 with resource binding tier 3 unless D3D11 is forced (or the driver is a
// known-bad NVIDIA one on Windows 7). GPUs sharing a name are numbered by bus location.
// ref: FUN_142288c00
bool GxBuildAdapterList() {
    for (uint64_t i = 0; i < g_gxAdapters.m_size; i++) {
        blz::string_free(g_gxAdapters.m_data[i].m_name.header());
    }
    g_gxAdapters.m_size = 0;
    pm::Instance* instance = g_gxPrismInstance;
    uint32_t count = 0x80;
    const OSVERSION* os = OsGetVersion();
    pm::GPU* gpus[0x80];
    pm::RESULT result;
    instance->EnumerateGpus(&result, &count, gpus, s_allowSoftwareGpu != 0 ? 0x1e : 0x1c);
    if (count == 0) {
        GxLog("No adapters found. Retrying with pmGpuType::Unknown allowed.");
        count = 0x80;
        instance->EnumerateGpus(&result, &count, gpus, 0x1f);
    }
    bool found = false;
    for (uint32_t index = 0; index < count; index++) {
        pm::GPU* handle = gpus[index];
        pm::GPU* gpu = instance->GetGpu(handle);
        ADAPTERCONTEXT context;
        ErrorContextPush(&context.m_base, DescribeAdapter);
        context.m_format = "Build Adapter List -- GPU Name: %s, VendorID: %4x, DeviceID: %4x";
        context.m_device = gpu->m_deviceId;
        context.m_vendor = gpu->m_vendorId;
        context.m_name = gpu->m_name;
        GxAdapterInfo* info = nullptr;
        bool describe = false;
        if (os->m_platform == 10) {
            pm::APIDEVICEDATA* data = nullptr;
            bool dx11 = false;
            bool dx12 = false;
            bool tryDx12 = false;
            if (s_forcedApi == GXAPI_D3D12) {
                tryDx12 = true;
            } else {
                data = instance->GetDeviceData(handle, pm::API_D3D11);
                if (data->m_ready != 0) {
                    dx11 = data->m_featureLevel > 0x9fff;
                }
                bool badDriver = false;
                if (os->m_major == 6 && os->m_minor == 1 && os->m_wine == 0 && gpu->m_vendorId == 0x10de) {
                    uint32_t high = static_cast<uint32_t>(gpu->m_driverVersion >> 32);
                    uint32_t low = static_cast<uint32_t>(gpu->m_driverVersion);
                    badDriver = (high == 0x1a0015 && low >= 0xe1046) || (high == 0x1b0015 && low < 0xe17bf);
                }
                if (badDriver) {
                    GxLog("*****************************************************************************************************************************************");
                    if (s_driverVersionCheckCVar->m_intValue != 0) {
                        GxLog("* Disabling DirectX 12");
                        GxLog("* To ignore this check, add the line: 'SET DriverVersionCheck \"0\"' to your WTF/Config.wtf file");
                    }
                    GxLog("* Known Bad Dx12 on Win 7 driver version detected. Nvidia driver versions 441.66 through 457.51 have major stability issues");
                    GxLog("* Updating to version 460.79 or newer is highly recommended");
                    GxLog("* Info (0x%x, 0x%x)", static_cast<uint32_t>(gpu->m_driverVersion >> 32),
                        static_cast<uint32_t>(gpu->m_driverVersion));
                    GxLog("*****************************************************************************************************************************************");
                    tryDx12 = s_driverVersionCheckCVar->m_intValue == 0 && s_forcedApi > GXAPI_D3D11;
                } else {
                    tryDx12 = s_forcedApi > GXAPI_D3D11;
                }
            }
            if (tryDx12) {
                pm::APIDEVICEDATA* data12 = instance->GetDeviceData(handle, pm::API_D3D12);
                if (data12->m_ready != 0 && data12->m_featureLevel > 0xafff && data12->m_216 > 1) {
                    dx12 = true;
                    data = data12;
                }
            }
            if (data != nullptr) {
                info = GxAdapterPush(gpu, data, handle);
                if (dx11) {
                    for (uint8_t api : { GXAPI_D3D11, GXAPI_D3D11_LEGACY }) {
                        info->m_supported[api] = 1;
                        s_apiSupported[api] = 1;
                        s_apiAvailable[api] = 1;
                    }
                    info->m_api = GXAPI_D3D11;
                }
                if (dx12) {
                    info->m_supported[GXAPI_D3D12] = 1;
                    info->m_api = GXAPI_D3D12;
                    s_apiSupported[GXAPI_D3D12] = 1;
                    s_apiAvailable[GXAPI_D3D12] = 1;
                }
                if (dx11 || dx12) {
                    // Intel parts before Gen9.5 stay on D3D11
                    if (info->m_gpu->m_vendorId == 0x8086 && static_cast<uint8_t>(info->m_data->m_intelGeneration - 1) < 8
                        && dx11) {
                        info->m_api = GXAPI_D3D11;
                    }
                    found = true;
                }
                describe = true;
            }
        } else if (os->m_platform == 6) {
            pm::APIDEVICEDATA* data = instance->GetDeviceData(handle, 6);
            info = GxAdapterPush(gpu, data, handle);
            if (data->m_ready != 0) {
                info->m_supported[GXAPI_MTL_LEGACY] = 1;
                s_apiSupported[GXAPI_MTL_LEGACY] = 1;
                s_apiAvailable[GXAPI_MTL_LEGACY] = 1;
                if (os->m_major < 0xb) {
                    info->m_api = GXAPI_MTL_LEGACY;
                } else {
                    info->m_supported[GXAPI_MTL] = 1;
                    s_apiSupported[GXAPI_MTL] = 1;
                    s_apiAvailable[GXAPI_MTL] = 1;
                    uint16_t vendor = info->m_gpu->m_vendorId;
                    info->m_api = vendor == 0x1002 || vendor == 0x10de ? GXAPI_MTL_LEGACY : GXAPI_MTL;
                }
                found = true;
            }
            describe = true;
        } else {
            GxLog("Unknown Platform %d", os->m_platform);
            describe = true;  // as the reference: it then reads the missing entry
        }
        if (describe) {
            blz::inline_string<0x40> virtualization;
            uint8_t mode = gpu->m_virtualization;
            if (mode > 1) {
                static const char* const s_modes[] = { "Unknown", "None", "Passthrough", "Virtual", "HypervisorHost" };
                blz::string_format(virtualization.header(), " virtualizationMode:%s", mode < 5 ? s_modes[mode] : "Unknown");
            }
            const char* family = "Unknown";
            uint16_t vendor = info->m_gpu->m_vendorId;
            if (vendor == 0x1002) {
                family = pm::AmdFamilyName(info->m_data->m_amdFamily);
            } else if (vendor == 0x10de) {
                family = pm::NvidiaFamilyName(info->m_data->m_nvidiaFamily);
            } else if (vendor == 0x8086) {
                family = pm::IntelFamilyName(info->m_data->m_intelGeneration);
            }
            GxLog("Adapter %u: \"%s\" family:%s type:%s vendor:0x%04x device:0x%04x driver(0x%llx) location:%u%s dx11:%s dx12:%s",
                index, gpu->m_name, family, pm::GpuTypeName(gpu->m_type), gpu->m_vendorId, gpu->m_deviceId,
                static_cast<unsigned long long>(gpu->m_driverVersion), gpu->m_location, virtualization.m_data,
                info->m_supported[GXAPI_D3D11] != 0 ? "true" : "false",
                info->m_supported[GXAPI_D3D12] != 0 ? "true" : "false");
            if (info->m_api == GXAPI_COUNT) {
                g_gxAdapters.m_size--;
                blz::string_free(g_gxAdapters.m_data[g_gxAdapters.m_size].m_name.header());
            } else {
                GxAdapterSetName(info, info->m_gpu->m_name);
                for (uint64_t i = 0; static_cast<int32_t>(i) < static_cast<int32_t>(g_gxAdapters.m_size) - 1; i++) {
                    GxAdapterInfo* other = &g_gxAdapters.m_data[i];
                    if (std::strcmp(other->m_gpu->m_name, info->m_gpu->m_name) == 0) {
                        GxAdapterSetName(info, "~");
                        GxAdapterSetName(other, "~");
                    }
                }
                uint32_t monitorCount = 0x20;
                void* monitors[0x20];
                pm::RESULT listed;
                instance->EnumerateMonitors(&listed, handle, &monitorCount, monitors);
                if (listed.m_code == 0) {
                    for (uint32_t m = 0; m < monitorCount; m++) {
                        auto monitor = static_cast<pm::MONITOR*>(instance->GetMonitor(monitors[m]));
                        GxLog("\tMonitor %u \"%s\" Size(%dx%d) Pos(%d, %d)", m, monitor->m_name, monitor->m_width,
                            monitor->m_height, monitor->m_left, monitor->m_top);
                    }
                }
                uint64_t* lastVersion = nullptr;
                if (vendor == 0x1002) {
                    lastVersion = &s_amdDriverVersion;
                } else if (vendor == 0x10de) {
                    lastVersion = &s_nvidiaDriverVersion;
                } else if (vendor == 0x8086) {
                    lastVersion = &s_intelDriverVersion;
                }
                if (lastVersion != nullptr && info->m_gpu->m_driverVersion != *lastVersion) {
                    if (*lastVersion != 0) {
                        s_driverChanged = 1;
                    }
                    *lastVersion = info->m_gpu->m_driverVersion;
                }
            }
        }
        ErrorContextPop(&context.m_base);
    }
    // GPUs that share a name: "<name> [n]", in bus-location order (or enumeration order when
    // the first two share a location).
    uint32_t total = static_cast<uint32_t>(g_gxAdapters.m_size);
    for (uint32_t i = 0; i < total; i++) {
        GxAdapterInfo* entry = &g_gxAdapters.m_data[i];
        if (entry->m_name.m_size != 1 || entry->m_name.m_data[0] != '~') {
            continue;
        }
        struct PAIR {
            uint32_t m_index;
            uint32_t m_key;
        };
        PAIR pairs[0x80];
        uint32_t pairCount = 0;
        pairs[pairCount++] = { i, entry->m_gpu->m_location };
        for (uint32_t j = i + 1; j < total; j++) {
            GxAdapterInfo* other = &g_gxAdapters.m_data[j];
            if (std::strcmp(other->m_gpu->m_name, entry->m_gpu->m_name) == 0) {
                pairs[pairCount++] = { j, other->m_gpu->m_location };
            }
        }
        if (pairCount > 1) {
            if (pairs[0].m_key == pairs[1].m_key) {
                for (uint32_t k = 0; k < pairCount; k++) {
                    pairs[k].m_key = g_gxAdapters.m_data[pairs[k].m_index].m_gpu->m_index;
                }
            }
            // FUN_1421645f0: an introsort on the key
            std::sort(pairs, pairs + pairCount, [](const PAIR& a, const PAIR& b) { return a.m_key < b.m_key; });
        }
        for (uint32_t k = 0; k < pairCount; k++) {
            GxAdapterInfo* named = &g_gxAdapters.m_data[pairs[k].m_index];
            blz::string_format(named->m_name.header(), "%s [%zu]", named->m_gpu->m_name, static_cast<size_t>(k + 1));
        }
    }
    return found && g_gxAdapters.m_size != 0;
}

// ref: FUN_14228ba10
bool GxInitialize(uint8_t forceApi) {
    if (!ErrorReportAddMemoryRange(s_gxStateBlock, sizeof(s_gxStateBlock))) {
        GxLog("Range set failed");
    }
    CVarRegister("videoOptionsVersionDefault", "", 0, "0", static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    CVarRegister("maxFPS", "Set FPS limit", 1, "120", OnMaxFpsChanged, 1, false, nullptr, false);
    CVarRegister("useMaxFPS", "Enables or disables FPS limit", 1, "0", OnUseMaxFpsChanged, 1, false, nullptr, false);
    CVarRegister("maxFPSBk", "Set background FPS limit", 1, "30", OnMaxFpsBkChanged, 1, false, nullptr, false);
    CVarRegister("useMaxFPSBk", "Enables or disables background FPS limit", 1, "1", OnUseMaxFpsBkChanged, 1, false,
        nullptr, false);
    char frameEndDefault[0x104];
    char frameEndHelp[0x104];
    SStrPrintf260(frameEndDefault, "%u", 0);
    SStrPrintf260(frameEndHelp, "Allow GxFrameEnd to overlap next frame (feature not yet available)");
    s_frameEndModeCVar = CVarRegister("GxFrameEndMode", frameEndHelp, 1, frameEndDefault, OnFrameEndModeChanged, 1,
        false, nullptr, false);
    s_adapterCVar = CVarRegister("gxAdapter",
        "Set which GPU to use. See GxListGPUs for valid names (empty string to let client choose)", 1, "",
        ConsoleDeviceRequestRestart, 1, false, nullptr, false);
    CVarRegister("ForceAllowAero",
        "Force Direct X 12 on Windows 7 to not disable Aero theme. You are opting into crashing in some edge cases", 0,
        "0", static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    s_prismEnabledCVar = CVarRegister("GxPrismEnabled",
        "0: Prism backends Disabled. 1: Default Prism backends Enabled. 2: Experimental Prism backends Enabled.", 0,
        "1", static_cast<CVARCALLBACK>(nullptr), 1, false, nullptr, false);
    s_driverVersionCheckCVar = CVarRegister("DriverVersionCheck", "", 0, "1", static_cast<CVARCALLBACK>(nullptr), 1,
        false, nullptr, false);
    s_mainThreadId = OsGetCurrentThreadId();
    const OSCPUINFO* cpu = OsGetCpuInfo();
    GxLog("World of Warcraft Retail x86_64 3.4.3.54261");
    static const char* const s_architectures[] = { "Unknown", "x86_32", "x86_64", "arm32", "arm64", "WAsm_32" };
    char text[0x400];
    OsFormatVersion(text, sizeof(text), OsGetVersion());
    AppendText(text, " ");
    AppendText(text, s_architectures[cpu->m_architecture]);
    GxLog("%s", text);
    const char* brand = cpu->m_brand;
    while (*brand == ' ' || *brand == '\t') {
        brand++;
    }
    blz::inline_string<0x40> frequency;
    if (cpu->m_maxFrequency != 0) {
        blz::string_format(frequency.header(), " | Max Frequency: %.1fGHz",
            static_cast<double>(static_cast<float>(static_cast<double>(cpu->m_maxFrequency) * 1e-09)));
    }
    GxLog("%s | Sockets:%u Cores:%u Threads:%u%s", brand, cpu->m_packages, cpu->m_cores, cpu->m_logical,
        frequency.m_data);
    uint64_t memory = OsGetPhysicalMemory();
    GxLog("%.1f GB System Memory", static_cast<double>(static_cast<float>(memory >> 20) * 0.0009765625f));
    OSPOWERSTATUS power = {};
    OsGetPowerStatus(&power);
    s_onAcPower = power.m_onAc;
    GxLogPowerStatus(&power, false);
    if (OsIsRemoteSession()) {
        GxLog("Remote Desktop Session Detected");
    }
    s_forcedApi = forceApi;
    if (forceApi != GXAPI_COUNT) {
        GxLog("Force API %s", forceApi < GXAPI_COUNT ? s_apiNames[forceApi] : "Unknown");
    }
    bool initialized = GxCreatePrismInstance();
    if (!initialized && s_forcedApi == GXAPI_D3D12) {
        GxLog("DX12 was forced but no DX12 capable device was found. Falling back to DX11...");
        s_forcedApi = GXAPI_COUNT;
        GxReleasePlatformHandles();
        pm::DestroyInstance(g_gxPrismInstance);
        g_gxPrismInstance = nullptr;
        initialized = GxCreatePrismInstance();
    }
    if (initialized) {
        s_driverChanged = 0;
        OsAddSuspendHandler(GxOnSuspend);
        OsAddResumeHandler(GxOnResume);
        OsAddPowerSettingHandler(GxOnPowerSettingChange);
        GxAddDeviceCreatedCallback(GxOnDeviceCreated, nullptr);
        GxAddDeviceDestroyedCallback(GxOnDeviceDestroyed, nullptr);
        GxFrameEndInitialize(&s_frameEnd);
    } else {
        GxLog("No suitable gpu found. World of Warcraft cannot proceed. Exiting application...");
        GxReleasePlatformHandles();
        pm::DestroyInstance(g_gxPrismInstance);
        g_gxPrismInstance = nullptr;
    }
    GxLogFlush();
    return initialized;
}

namespace {

uint8_t s_api;                      // DAT_142d41c1a: the API the device is (to be) created with
uint32_t s_afrDeviceCount;          // DAT_143490864

// The handlers Gx calls out to (blz::function each): restart (GxRestart), the display-change
// hook, and what to do when no GPU is usable.
blz::function_storage s_restartHandler = { nullptr, blz::function_empty_manager, {} };        // 0x142d41d10
blz::function_storage s_displayChangedHandler = { nullptr, blz::function_empty_manager, {} }; // 0x142d41d30
blz::function_storage s_noGpuHandler = { nullptr, blz::function_empty_manager, {} };          // 0x142d41d50

// What an empty handler runs (_guard_check_icall: a bare return).
void FunctionNoop(void* storage) {
    (void)storage;
}

// The inlined blz::function move-assignment the three setters share: an empty source becomes a
// handler that does nothing.
void AssignHandler(blz::function_storage* dest, blz::function_storage* source) {
    blz::function_storage moved;
    if (source->m_invoke == nullptr) {
        moved.m_invoke = reinterpret_cast<void*>(&FunctionNoop);
        moved.m_manager = blz::function_trivial_manager;
    } else {
        source->m_manager(moved.m_storage, source->m_storage, 2);
        moved.m_invoke = source->m_invoke;
        moved.m_manager = source->m_manager;
        source->m_invoke = nullptr;
        source->m_manager = blz::function_empty_manager;
    }
    dest->m_manager(dest->m_storage, nullptr, 0);
    moved.m_manager(dest->m_storage, moved.m_storage, 2);
    dest->m_invoke = moved.m_invoke;
    dest->m_manager = moved.m_manager;
    if (source->m_manager != blz::function_empty_manager) {
        source->m_manager(source->m_storage, nullptr, 0);
    }
}

}

// ref: FUN_14228aa30
uint8_t GxGetApi() {
    return s_api;
}

// ref: FUN_14228b8d0
void GxSetApi(uint8_t api) {
    s_api = api;
}

// ref: FUN_142288680
bool GxApiSupported(uint8_t api) {
    return s_apiSupported[api] != 0;
}

// ref: FUN_1422886b0
bool GxApiAvailable(uint8_t api) {
    return s_apiAvailable[api] != 0;
}

// ref: FUN_142288690
const char* GxApiName(uint8_t api) {
    return api < GXAPI_COUNT ? s_apiNames[api] : "Unknown";
}

// The best API any hardware adapter runs (software ones do not count).
// ref: FUN_1422886c0
uint8_t GxGetBestApi() {
    uint8_t best = 0;
    for (uint64_t i = 0; i < g_gxAdapters.m_size; i++) {
        GxAdapterInfo* info = &g_gxAdapters.m_data[i];
        if (info->m_gpu->m_type != pm::GPU_SOFTWARE && info->m_api > best) {
            best = info->m_api;
        }
    }
    return best;
}

// ref: FUN_14228a060
GXADAPTERLIST* GxGetAdapterList() {
    return &g_gxAdapters;
}

// ref: FUN_14228b7e0
void GxSetAfrDeviceCount(uint32_t count) {
    s_afrDeviceCount = count;
}

// ref: FUN_14228a080
bool GxGetAllowSoftwareGpu() {
    return s_allowSoftwareGpu != 0;
}

// ref: FUN_14228b7f0
void GxSetAllowSoftwareGpu(bool allow) {
    s_allowSoftwareGpu = allow;
}

// The device's adapter: the entry whose GPU the device was made on, else the first.
// ref: FUN_142291780
GxAdapterInfo* GxDevAdapter(void* device) {
    GXADAPTERLIST* list = GxGetAdapterList();
    void* gpu = *reinterpret_cast<void**>(static_cast<uint8_t*>(device) + 0x10);
    for (uint64_t i = 0; i < list->m_size; i++) {
        if (list->m_data[i].m_handle == gpu) {
            return &list->m_data[i];
        }
    }
    return GxGetAdapterList()->m_data;
}

// The GPU to create the device on: the gxAdapter CVar's choice when it names one, else by
// preference an external GPU with no monitor, a discrete or external one driving a monitor,
// any external, any discrete, any integrated, then the first. Only GPUs running the best API
// are considered.
// ref: FUN_14228a100
GxAdapterInfo* GxSelectGpuToUse() {
    if (g_gxDevice != nullptr) {
        return GxDevAdapter(g_gxDevice);
    }
    blz::inline_string<0x40> wanted;
    const char* name = s_adapterCVar != nullptr ? s_adapterCVar->m_stringValue.GetString() : nullptr;
    uint64_t wantedLength = 0;
    if (name != nullptr && *name != '\0') {
        wantedLength = std::strlen(name);
    }
    blz::string_reserve(wanted.header(), wantedLength);
    if (wantedLength != 0) {
        std::memcpy(wanted.m_data, name, wantedLength);
    }
    wanted.m_data[wantedLength] = '\0';
    wanted.m_size = wantedLength;
    uint8_t best = GxGetBestApi();
    pm::GPU* chosen = nullptr;
    if (g_gxAdapters.m_size == 0) {
        GxLog("SelectGpuToUse failed");
    } else {
        GxAdapterInfo* externalWithoutMonitor = nullptr;
        GxAdapterInfo* withMonitor = nullptr;
        GxAdapterInfo* external = nullptr;
        GxAdapterInfo* discrete = nullptr;
        GxAdapterInfo* integrated = nullptr;
        bool named = false;
        for (uint64_t i = 0; i < g_gxAdapters.m_size; i++) {
            GxAdapterInfo* info = &g_gxAdapters.m_data[i];
            if (info->m_supported[best] == 0) {
                continue;
            }
            if (wantedLength > 2) {
                uint64_t length = info->m_name.m_size;
                uint64_t compared = length < wantedLength ? length : wantedLength;
                if ((compared == 0 || std::memcmp(info->m_name.m_data, wanted.m_data, compared) == 0)
                    && static_cast<int32_t>(length) == static_cast<int32_t>(wantedLength)) {
                    chosen = info->m_handle;
                    named = true;
                    break;
                }
            }
            uint32_t monitorCount = 8;
            void* monitors[8];
            pm::RESULT result;
            g_gxPrismInstance->EnumerateMonitors(&result, info->m_handle, &monitorCount, monitors);
            bool hasMonitor = result.m_code == 0 && monitorCount != 0;
            uint8_t type = info->m_gpu->m_type;
            if (type == pm::GPU_EXTERNAL) {
                if (!hasMonitor && externalWithoutMonitor == nullptr) {
                    externalWithoutMonitor = info;
                }
                if (external == nullptr) {
                    external = info;
                }
            }
            if ((type == pm::GPU_EXTERNAL || type == pm::GPU_DISCRETE) && hasMonitor && withMonitor == nullptr) {
                withMonitor = info;
            }
            if (type == pm::GPU_INTEGRATED) {
                if (integrated == nullptr) {
                    integrated = info;
                }
            } else if (type == pm::GPU_DISCRETE && discrete == nullptr) {
                discrete = info;
            }
        }
        if (!named) {
            GxAdapterInfo* pick = externalWithoutMonitor;
            if (pick == nullptr) {
                pick = withMonitor;
            }
            if (pick == nullptr) {
                pick = external;
            }
            if (pick == nullptr) {
                pick = discrete;
            }
            if (pick == nullptr) {
                pick = integrated;
            }
            if (pick == nullptr) {
                pick = g_gxAdapters.m_data;
            }
            chosen = pick->m_handle;
        }
    }
    for (uint64_t i = 0; i < g_gxAdapters.m_size; i++) {
        if (g_gxAdapters.m_data[i].m_handle == chosen) {
            return &g_gxAdapters.m_data[i];
        }
    }
    return g_gxAdapters.m_data;
}

// ref: FUN_14228b0d0
void GxLogFlushNow() {
    GxLogFlush();
}

// ref: FUN_14228ea90
void GxSetRestartHandler(blz::function_storage* handler) {
    AssignHandler(&s_restartHandler, handler);
}

// ref: FUN_14228e950
void GxSetDisplayChangedHandler(blz::function_storage* handler) {
    AssignHandler(&s_displayChangedHandler, handler);
}

// ref: FUN_14228ebe0
void GxSetNoGpuHandler(blz::function_storage* handler) {
    AssignHandler(&s_noGpuHandler, handler);
}

bool GxDriverOutOfDate(CGxDevice* device);

namespace {

uint8_t s_apiDisabled[GXAPI_COUNT];  // DAT_1434909b8: APIs turned off after a failure

// ref: FUN_14228e3a0
bool GxApiDisabled(uint8_t api) {
    if (api < GXAPI_COUNT) {
        return s_apiDisabled[api] != 0;
    }
    return true;
}

// The GPU to create `api`'s device on: the one named in the device parameters, else by the
// same preference as GxSelectGpuToUse among the GPUs that run `api`.
// ref: FUN_14228d100
pm::GPU* GxSelectGpu(uint8_t api, const blz::inline_string<0x40>* name, bool log) {
    if (g_gxAdapters.m_size == 0) {
        GxLog("SelectGpuToUse failed");
        return nullptr;
    }
    GxAdapterInfo* externalWithoutMonitor = nullptr;
    GxAdapterInfo* withMonitor = nullptr;
    GxAdapterInfo* external = nullptr;
    GxAdapterInfo* discrete = nullptr;
    GxAdapterInfo* integrated = nullptr;
    for (uint64_t i = 0; i < g_gxAdapters.m_size; i++) {
        GxAdapterInfo* info = &g_gxAdapters.m_data[i];
        if (info->m_supported[api] == 0) {
            continue;
        }
        if (name->m_size > 2) {
            uint64_t length = info->m_name.m_size;
            uint64_t compared = length < name->m_size ? length : name->m_size;
            if ((compared == 0 || std::memcmp(info->m_name.m_data, name->m_data, compared) == 0) &&
                static_cast<int32_t>(length) == static_cast<int32_t>(name->m_size)) {
                if (log) {
                    GxLog("Choosing gpu by name: \"%s\"", info->m_name.m_data);
                }
                return info->m_handle;
            }
        }
        uint32_t monitorCount = 8;
        void* monitors[8];
        pm::RESULT result;
        g_gxPrismInstance->EnumerateMonitors(&result, info->m_handle, &monitorCount, monitors);
        bool hasMonitor = result.m_code == 0 && monitorCount != 0;
        uint8_t type = info->m_gpu->m_type;
        if (type == pm::GPU_EXTERNAL) {
            if (!hasMonitor && externalWithoutMonitor == nullptr) {
                externalWithoutMonitor = info;
            }
            if (external == nullptr) {
                external = info;
            }
        }
        if ((type == pm::GPU_EXTERNAL || type == pm::GPU_DISCRETE) && hasMonitor && withMonitor == nullptr) {
            withMonitor = info;
        }
        if (type == pm::GPU_INTEGRATED) {
            if (integrated == nullptr) {
                integrated = info;
            }
        } else if (type == pm::GPU_DISCRETE && discrete == nullptr) {
            discrete = info;
        }
    }
    if (externalWithoutMonitor != nullptr) {
        if (log) {
            GxLog("Choosing headless egpu: \"%s\"", externalWithoutMonitor->m_name.m_data);
        }
        return externalWithoutMonitor->m_handle;
    }
    if (withMonitor != nullptr) {
        if (log) {
            GxLog("Choosing gpu with monitor attached: \"%s\"", withMonitor->m_name.m_data);
        }
        return withMonitor->m_handle;
    }
    if (external != nullptr) {
        if (log) {
            GxLog("Choosing egpu: \"%s\"", external->m_name.m_data);
        }
        return external->m_handle;
    }
    if (discrete != nullptr) {
        if (log) {
            GxLog("Choosing high power gpu: \"%s\"", discrete->m_name.m_data);
        }
        return discrete->m_handle;
    }
    if (integrated != nullptr) {
        if (log) {
            GxLog("Choosing low power gpu: \"%s\"", integrated->m_name.m_data);
        }
        return integrated->m_handle;
    }
    if (log) {
        GxLog("No suitable device found. Attempting to use default device:");
    }
    return g_gxAdapters.m_size != 0 ? g_gxAdapters.m_data[0].m_handle : nullptr;
}

// The ray tracing tier names (PTR_DAT_142910fe8).
const char* const s_rayTracingNames[3] = { "None", "1.0", "1.1" };

// The shader family names (FUN_14228b900).
const char* GxShaderFamilyName(int32_t family) {
    const char* names[4] = { "dx_4_0", "dx_5_0", "dx_6_0", "mtl_1_1" };
    if (family < 4) {
        return names[family];
    }
    return "Unknown";
}

// Builds the device for `api` in Gx's state block and fills in what it knows of the GPU.
// ref: FUN_14228cc30
void GxDeviceConstruct(uint8_t api, pm::GPU* gpu) {
    static const uint8_t s_prismApis[GXAPI_COUNT] = { pm::API_D3D11, pm::API_D3D11, pm::API_D3D12, 0, 0 };  // DAT_142910f88
    pm::APIDEVICEDATA* data = g_gxPrismInstance->GetDeviceData(gpu, s_prismApis[api]);
    std::memset(s_gxStateBlock, 0, sizeof(s_gxStateBlock));
    int32_t prism = s_prismEnabledCVar->m_intValue;
    int32_t prismMode = 0;
    CGxDevice* device = nullptr;
    bool native;
    if (api == GXAPI_D3D12) {
        native = prism != 2;
    } else {
        native = (prism == 0 || api == GXAPI_D3D11_LEGACY) && static_cast<uint8_t>(api - 3) > 1;
        prismMode = prism;
    }
    if (native) {
        prismMode = 0;
        if (api == GXAPI_D3D11_LEGACY || api == GXAPI_D3D11) {
            FROST_UNIMPLEMENTED();  // FUN_1422a0810: the native D3D11 device
        } else if (api == GXAPI_D3D12) {
            device = GxDeviceD3d12Create(s_gxStateBlock);
        }
    } else {
        FROST_UNIMPLEMENTED();  // FUN_1422b7df0: the Prism device
    }
    g_gxDevice = device;
    if (device == nullptr) {
        return;
    }
    auto bytes = reinterpret_cast<uint8_t*>(device);
    auto dataBytes = reinterpret_cast<const uint8_t*>(data);
    device->m_gpu = gpu;
    device->m_data = data;
    *reinterpret_cast<uint32_t*>(bytes + 0x80) = *reinterpret_cast<const uint32_t*>(dataBytes + 0xbc);
    bytes[0x84] = dataBytes[0xc9];
    bytes[0x8c] = dataBytes[0xd9];
    pm::GPU* info = g_gxPrismInstance->GetGpu(gpu);
    std::memcpy(bytes + 0x90, info, 0x50);
    int32_t& model = *reinterpret_cast<int32_t*>(bytes + 0x88);
    if (static_cast<uint8_t>(api - 3) < 2) {
        model = 3;
    } else if (api < 2) {
        model = dataBytes[0xd9] > 3 ? 1 : 0;
    } else if (api == GXAPI_D3D12) {
        if (dataBytes[0x218] < 3) {
            model = 1;
        } else {
            model = 2 - (dataBytes[0xd9] < 0xb ? 1 : 0);
        }
    }
    *reinterpret_cast<uint32_t*>(bytes + 0xe0) = 0x40;
    *reinterpret_cast<uint32_t*>(bytes + 0xe8) = 0x40;
    *reinterpret_cast<uint32_t*>(bytes + 0xec) = 0x10;
    *reinterpret_cast<uint32_t*>(bytes + 0xf0) = 0x10;
    *reinterpret_cast<uint32_t*>(bytes + 0xf8) = 0x10;
    *reinterpret_cast<uint32_t*>(bytes + 0xfc) = 0x10;
    *reinterpret_cast<uint32_t*>(bytes + 0x108) = 0x1000;
    *reinterpret_cast<uint32_t*>(bytes + 0x100) = 0x1000;
    *reinterpret_cast<uint32_t*>(bytes + 0x10c) = 0x1000;
    *reinterpret_cast<uint32_t*>(bytes + 0x110) = 0x4000;
    *reinterpret_cast<uint32_t*>(bytes + 0x114) = 0x4000;
    *reinterpret_cast<uint32_t*>(bytes + 0x118) = 0x4000;
    *reinterpret_cast<uint32_t*>(bytes + 0x11c) = 0x800;
    *reinterpret_cast<uint32_t*>(bytes + 0x120) = 0x4000;
    *reinterpret_cast<uint32_t*>(bytes + 0x124) = 0x4000;
    *reinterpret_cast<uint32_t*>(bytes + 0x128) = 1;
    *reinterpret_cast<uint16_t*>(bytes + 0x12c) = 0x100;
    bytes[0x12e] = 0;
    uint8_t rayTracing = 0;
    if (model == 2 && prismMode == 0 && dataBytes[0x218] >= 2) {
        rayTracing = static_cast<uint8_t>((dataBytes[0x218] != 2) + 1);
    }
    bytes[0x12f] = rayTracing;
    const char* rayName = bytes[0x12f] < 3 ? s_rayTracingNames[bytes[0x12f]] : "Unknown";
    const char* modelNames[4] = { "dx_4_0", "dx_5_0", "dx_6_0", "mtl_1_1" };
    const char* modelName = model < 4 ? modelNames[model] : "Unknown";
    auto deviceData = reinterpret_cast<const uint8_t*>(device->m_data);
    GxLog("GpuInfo: sm:%s, rt:%s, vrt:%u, bary:%d, mesh:%u pull:%u", modelName, rayName, deviceData[0x219],
        bytes[0x12e] != 0, deviceData[0x21a], (*reinterpret_cast<const uint32_t*>(deviceData + 0x224) >> 5) & 1);
}

}

// Creates the device for `api` with the console's settings, returning whether it is up.
// ref: FUN_142289c40
bool GxDevCreate(uint8_t api, const GXDEVICEPARAMS* params) {
    if (!GxApiDisabled(api)) {
        pm::GPU* gpu = GxSelectGpu(api, &params->m_adapterName, true);
        if (gpu == nullptr) {
            GxLog("No Gpu Found <");
        } else {
            GxDeviceConstruct(api, gpu);
            auto device = static_cast<CGxDevice*>(g_gxDevice);
            if (device != nullptr && device->DeviceCreate(params)) {
                bool outdated = GxDriverOutOfDate(device);
                GxLog("Using shader family %s", GxShaderFamilyName(*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(device) + 0x88)));
                GxLog("<IsGPUDriverOutOfDate> %s", outdated ? "true" : "false");
                GxLogFlush();
                return true;
            }
            if (device != nullptr) {
                device->~CGxDevice();
            }
            g_gxDevice = nullptr;
        }
    } else if (api < GXAPI_COUNT) {
        GxLog("%s was disabled", s_apiNames[api]);
    } else {
        GxLog("%s was disabled", "Unknown");
    }
    GxLogFlush();
    return false;
}

// Whether the driver is older than the vendor's minimum the game supports.
// ref: FUN_1422920e0
bool GxDriverOutOfDate(CGxDevice* device) {
    auto bytes = reinterpret_cast<uint8_t*>(device);
    if (device->m_ready == 0 || OsGetVersion()->m_wine != 0) {
        return false;
    }
    GXADAPTERLIST* list = GxGetAdapterList();
    GxAdapterInfo* info = list->m_data;
    for (uint64_t i = 0; i < list->m_size; i++) {
        if (list->m_data[i].m_handle == device->m_gpu) {
            info = &list->m_data[i];
            break;
        }
    }
    uint8_t api = device->m_api;
    if (api == GXAPI_MTL_LEGACY || api == GXAPI_MTL) {
        return reinterpret_cast<uint8_t*>(device->m_data)[0xc9] < 6;
    }
    if (api > GXAPI_D3D12) {
        return false;
    }
    if (info->m_supported[3] == 0 || GxApiDisabled(GXAPI_D3D12)) {
        return false;
    }
    pm::APIDEVICEDATA* data = g_gxPrismInstance->GetDeviceData(device->m_gpu, pm::API_D3D12);
    if (data->m_ready == 0) {
        return false;
    }
    uint16_t vendor = *reinterpret_cast<uint16_t*>(bytes + 0xa0);
    if (vendor == 0x10de) {
        // FUN_1401a4fe0 (NvAPI_SYS_GetDriverAndBranchVersion). DIVERGED: NVAPI is not vendored,
        // so the NVIDIA version is not known here and the check passes.
        return false;
    }
    if (vendor == 0x1002) {
        if (g_gxPlatform.m_agsContext == nullptr || g_gxPlatform.m_agsGpuInfo == nullptr) {
            return false;
        }
        auto versions = static_cast<const char* const*>(g_gxPlatform.m_agsGpuInfo);
        if (versions[0] == nullptr) {
            return false;
        }
        return std::atoi(versions[0]) <= 0x13;
    }
    if (vendor == 0x8086) {
        uint32_t version = *reinterpret_cast<uint32_t*>(bytes + 0x98);
        if ((version & 0xffff0000) > 0x63ffff) {
            return false;
        }
        return static_cast<uint16_t>(version) < 0x12c0;
    }
    if (vendor == 0x4351 && *reinterpret_cast<uint16_t*>(bytes + 0xa2) == 0x3430) {
        return *reinterpret_cast<uint64_t*>(bytes + 0x98) < 0x1a001204e20001ull;
    }
    return false;
}

namespace {

CALLBACKDEQUE s_displayChangeCallbacks;  // DAT_143490958

// Erases [first, last) from a callback deque, moving whichever side is shorter.
// ref: FUN_14228ef20
uint64_t CallbackErase(CALLBACKDEQUE* deque, uint64_t first, uint64_t last) {
    uint64_t before = first - deque->m_offset;
    uint64_t count = last - first;
    if (count != 0) {
        uint64_t end = deque->m_size + deque->m_offset;
        if (before < end - last) {
            uint64_t from = first;
            uint64_t to = last;
            while (from != deque->m_offset) {
                from--;
                to--;
                *CallbackSlot(deque, to) = *CallbackSlot(deque, from);
            }
            deque->m_offset += count;
        } else {
            uint64_t to = first;
            for (uint64_t from = last; from != end; from++, to++) {
                *CallbackSlot(deque, to) = *CallbackSlot(deque, from);
            }
        }
        deque->m_size -= count;
    }
    return deque->m_offset + before;
}

// Removes the first entry for (callback, param).
// ref: FUN_14228def0
void CallbackRemove(CALLBACKDEQUE* deque, GXDEVICECALLBACK callback, void* param) {
    uint64_t end = deque->m_size + deque->m_offset;
    uint64_t index = deque->m_offset;
    for (; index != end; index++) {
        CALLBACKPAIR* pair = CallbackSlot(deque, index);
        if (pair->m_callback == callback && pair->m_param == param) {
            break;
        }
    }
    if (index != end) {
        CallbackErase(deque, index, index + 1);
    }
}

}

// ref: FUN_14228e6d0
void GxRemoveDeviceCreatedCallback(GXDEVICECALLBACK callback, void* param) {
    CallbackRemove(&g_gxDeviceCreatedCallbacks, callback, param);
}

// ref: FUN_14228e6f0
void GxRemoveDeviceDestroyedCallback(GXDEVICECALLBACK callback, void* param) {
    CallbackRemove(&g_gxDeviceDestroyedCallbacks, callback, param);
}

// ref: FUN_14228e150
void GxAddDisplayChangeCallback(GXDISPLAYCALLBACK callback, void* param) {
    CALLBACKDEQUE* deque = &s_displayChangeCallbacks;
    // FUN_14228f090: push_back
    if (((deque->m_offset + deque->m_size) & 7) == 0 && deque->m_mapSize <= (deque->m_size + 8) >> 3) {
        CallbackGrowMap(deque, s_displayCallbackMapTag);
    }
    uint64_t index = deque->m_offset + deque->m_size;
    deque->m_size++;
    CALLBACKPAIR* slot = CallbackSlot(deque, index, s_displayCallbackTag);
    if (slot != nullptr) {
        slot->m_callback = reinterpret_cast<GXDEVICECALLBACK>(callback);
        slot->m_param = param;
    }
}

// ref: FUN_14228e550
void GxNotifyOnDisplayChange(uint32_t monitor) {
    GxLog("GxNotifyOnDisplayChange: %d", monitor);
    CALLBACKDEQUE* deque = &s_displayChangeCallbacks;
    uint64_t end = deque->m_size + deque->m_offset;
    for (uint64_t index = deque->m_offset; index != end; index++) {
        CALLBACKPAIR* pair = CallbackSlot(deque, index, s_displayCallbackTag);
        reinterpret_cast<GXDISPLAYCALLBACK>(pair->m_callback)(monitor, pair->m_param);
    }
}

namespace {

blz::shared_mutex s_deviceLock;     // DAT_1434908d8
uint8_t s_destroyingDevice;         // DAT_14349069c
uint32_t s_deviceGeneration;        // DAT_143490878

// ref: FUN_142291650
void GxFrameEndShutdown(GXFRAMEEND* frameEnd) {
    if (frameEnd->m_group != nullptr) {
        if ((frameEnd->m_pending & 2) != 0) {
            ThreadPoolGroupWait(frameEnd->m_group);
        }
        ThreadPoolGroupRelease(frameEnd->m_group);
    }
    frameEnd->m_group = nullptr;
    _InterlockedAnd8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), static_cast<char>(0xfd));
}

// Waits on the groups queued against the frame end; the late ones only when `all`.
// ref: FUN_142292bd0
void GxFrameEndWaitGroups(GXFRAMEEND* frameEnd, bool all) {
    for (uint64_t i = 0; i < frameEnd->m_waits.m_size; i++) {
        ThreadPoolGroupWait(frameEnd->m_waits.m_data[i]);
    }
    frameEnd->m_waits.m_size = 0;
    if (all) {
        for (uint64_t i = 0; i < frameEnd->m_lateWaits.m_size; i++) {
            ThreadPoolGroupWait(frameEnd->m_lateWaits.m_data[i]);
        }
        frameEnd->m_lateWaits.m_size = 0;
    }
}

// Completes an ended frame on the main thread: drains the waits (all of them from mode 2 on),
// finishes an overlapped end and runs its callback, and unbinds the context's textures.
// Mode 0 and 1 return early while the stats are not yet taken.
// ref: FUN_142292a10
void GxFrameEndWait(GXFRAMEEND* frameEnd, uint32_t mode, uint32_t unused) {
    (void)unused;
    if ((frameEnd->m_pending & 4) == 0 || !ThreadPoolIsMainThread()) {
        return;
    }
    if ((frameEnd->m_pending & 8) == 0) {
        GxFrameEndWaitGroups(frameEnd, mode != 0);
        if (mode < 2) {
            return;
        }
    } else {
        GxFrameEndWaitGroups(frameEnd, true);
    }
    if ((frameEnd->m_pending & 2) != 0) {
        ThreadPoolGroupWait(frameEnd->m_group);
        static_cast<CGxDevice*>(g_gxDevice)->LatencyMarker(3, frameEnd->m_frame);
        _InterlockedAnd8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), static_cast<char>(0xfd));
        if (frameEnd->m_callback.m_invoke != nullptr) {
            GXFRAMESTATS stats = frameEnd->m_stats;
            reinterpret_cast<void (*)(GXFRAMESTATS*, void*)>(frameEnd->m_callback.m_invoke)(&stats,
                frameEnd->m_callback.m_storage);
        }
        frameEnd->m_callback.m_manager(frameEnd->m_callback.m_storage, nullptr, 0);
        frameEnd->m_callback.m_invoke = nullptr;
        frameEnd->m_callback.m_manager = blz::function_empty_manager;
    }
    if (GxDevExists()) {
        g_gxRenderContext->UnbindTextures();
    }
    InterlockedExchange8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), 0);
    frameEnd->m_began = 0;
}

}

// Destroys the device: the frame-end work and the thread pool are drained first, and the
// device generation moves on so objects from the old device are recognized.
// ref: FUN_142289d70
void GxDevDestroy() {
    GxFrameEndWait(&s_frameEnd, 2, 1);
    ThreadPoolFlush(2);
    ThreadPoolFlush(1);
    ThreadPoolFlush(0);
    s_deviceLock.lock();
    s_destroyingDevice = 1;
    if (g_gxDevice != nullptr) {
        auto device = static_cast<CGxDevice*>(g_gxDevice);
        device->DeviceShutdown();
        device->~CGxDevice();
        g_gxDevice = nullptr;
    }
    s_deviceGeneration++;
    s_destroyingDevice = 0;
    s_deviceLock.unlock();
    GxLogFlush();
}

// ref: FUN_14228b960
void GxShutdown() {
    GxRemoveDeviceCreatedCallback(GxOnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(GxOnDeviceDestroyed, nullptr);
    GxOnDeviceDestroyed(nullptr);
    GxFrameEndShutdown(&s_frameEnd);
    OsRemovePowerSettingHandler(GxOnPowerSettingChange);
    OsRemoveSuspendHandler(GxOnSuspend);
    OsRemoveResumeHandler(GxOnResume);
    GxReleasePlatformHandles();
    pm::DestroyInstance(g_gxPrismInstance);
    g_gxPrismInstance = nullptr;
    GxLog("GxShutdown");
    GxLogFlush();
}

// ---- what the device backends take from GxApi ---------------------------------------------

SCritSect g_gxDeviceLock;  // DAT_143490880

namespace {

// ref: FUN_14020b5a0 (the static initializer; the matching atexit is left to the quit path)
struct GXDEVICELOCKINIT {
    GXDEVICELOCKINIT() {
        g_gxDeviceLock.Initialize();
    }
} s_deviceLockInit;

volatile LONG s_deviceRemovedCount;            // DAT_143490698
CALLBACKDEQUE s_engineDeviceCreatedCallbacks;  // DAT_143490978
CALLBACKDEQUE s_lowMemoryCallbacks;            // DAT_143490998

// FUN_14228f7a0's function-local static (DAT_1434909c0).
SCritSect* DeviceCallCritSect() {
    struct LOCK {
        SCritSect m_lock;
        LOCK() {
            this->m_lock.Initialize();
        }
    };
    static LOCK s_lock;
    return &s_lock.m_lock;
}

void RunCallbacks(CALLBACKDEQUE* deque) {
    uint64_t end = deque->m_size + deque->m_offset;
    for (uint64_t index = deque->m_offset; index != end; index++) {
        CALLBACKPAIR* pair = CallbackSlot(deque, index);
        pair->m_callback(pair->m_param);
    }
}

// ref: FUN_14228ed20
void PushCallback(CALLBACKDEQUE* deque, GXDEVICECALLBACK callback, void* param) {
    if (((deque->m_offset + deque->m_size) & 7) == 0 && deque->m_mapSize <= (deque->m_size + 8) >> 3) {
        CallbackGrowMap(deque);
    }
    uint64_t index = deque->m_offset + deque->m_size;
    deque->m_size++;
    CALLBACKPAIR* slot = CallbackSlot(deque, index);
    if (slot != nullptr) {
        slot->m_callback = callback;
        slot->m_param = param;
    }
}

}

// ref: FUN_14228f7a0
GxDeviceCallLock::GxDeviceCallLock() {
    DeviceCallCritSect()->Enter();
}

GxDeviceCallLock::~GxDeviceCallLock() {
    DeviceCallCritSect()->Leave();
}

// ref: FUN_142295780
IDXGIFactory1* GxPlatformFactory1() {
    return static_cast<IDXGIFactory1*>(g_gxPlatform.m_factory);
}

// ref: FUN_142295790
IDXGIFactory5* GxPlatformFactory5() {
    return s_factory5;
}

// ref: FUN_1422957a0
IDXGIFactory6* GxPlatformFactory6() {
    return s_factory6;
}

// ref: FUN_142295920
bool GxPlatformHasD3d12() {
    return g_gxPlatform.m_d3d12Module != nullptr;
}

// ref: FUN_1422958a0
bool GxPlatformHasNvapi() {
    return g_gxPlatform.m_nvapi != 0;
}

// The entry points DeviceCreateCore reads straight from GxApi's globals.
PFN_GXD3D12CREATEDEVICE GxPlatformD3d12CreateDevice() {
    return reinterpret_cast<PFN_GXD3D12CREATEDEVICE>(s_d3d12CreateDevice);
}

PFN_D3D12_SERIALIZE_ROOT_SIGNATURE GxPlatformSerializeRootSignature() {
    return reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(s_d3d12SerializeRootSignature);
}

PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE GxPlatformSerializeVersionedRootSignature() {
    return reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(s_d3d12SerializeVersionedRootSignature);
}

// ref: FUN_14228aab0
void GxCountDeviceRemoved() {
    InterlockedIncrement(&s_deviceRemovedCount);
}

// ref: FUN_14228e400
void GxNotifyOnDeviceCreate() {
    GxLog("NotifyOnDeviceCreate");
    RunCallbacks(&s_engineDeviceCreatedCallbacks);
    RunCallbacks(&g_gxDeviceCreatedCallbacks);
}

// ref: FUN_14228e4d0
void GxNotifyOnDeviceDestroy() {
    GxLog("NotifyOnDeviceDestroy");
    RunCallbacks(&g_gxDeviceDestroyedCallbacks);
}

// ref: FUN_14228e650
void GxNotifyOnLowMemory() {
    GxLog("GxNotifyOnLowMemory");
    RunCallbacks(&s_lowMemoryCallbacks);
}

// ref: FUN_14228e0c0
void GxAddEngineDeviceCreatedCallback(GXDEVICECALLBACK callback, void* param) {
    PushCallback(&s_engineDeviceCreatedCallbacks, callback, param);
    if (GxDevExists()) {
        callback(param);
    }
}

// ref: FUN_14228e220
void GxAddLowMemoryCallback(GXDEVICECALLBACK callback, void* param) {
    PushCallback(&s_lowMemoryCallbacks, callback, param);
}

// ref: FUN_14228a070
blz::allocator* GxGetAllocator() {
    return &s_prismAllocator;
}

// ---- the frame ---------------------------------------------------------------------------

namespace {

uint8_t s_framePhase;               // DAT_14348b640
uint8_t s_frameSceneOpen;           // DAT_14348b641: a scene draw is under way (FUN_1413c5250)
uint32_t s_deviceLostCount;         // DAT_143490858
uint8_t s_deviceRestartPending;     // DAT_14349069e
uint32_t s_lastMarkedFrame;         // DAT_142d438bc
uint8_t s_foregroundFps = 1;        // DAT_142d41c18: the window has the focus (GxSetForeground)
uint32_t s_frameLimitFps;           // DAT_1434908c0
uint64_t s_frameLimitTicks;         // DAT_1434908c8: counter ticks per frame
uint64_t s_frameLimitNext;          // DAT_1434908d0: when the next frame may start

// The frame-rate cap in force: maxFPS in the foreground, maxFPSBk behind, at least 8.
uint32_t FrameRateCap() {
    uint32_t foreground = s_useMaxFps != 0 ? static_cast<uint32_t>(s_maxFps) : 0xffffffff;
    uint32_t cap = s_useMaxFpsBk != 0 ? static_cast<uint32_t>(s_maxFpsBk) : 0xffffffff;
    if (s_foregroundFps != 0) {
        cap = foreground;
    }
    return cap > 7 ? cap : 8;
}

void StringAppend(blz::string_header* text, const char* append) {
    uint64_t length = std::strlen(append);
    uint64_t size = text->m_size + length;
    blz::string_reserve(text, size);
    std::memcpy(text->m_data + text->m_size, append, length);
    text->m_data[size] = '\0';
    text->m_size = size;
}

// Records the frame's end on the device and its GPU time from the frame's timer query.
// ref: FUN_142291390
void GxFrameEndRecord(GXFRAMEEND* frameEnd) {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    frameEnd->m_stats.m_waited = frameEnd->m_waited;
    uint64_t wait;
    frameEnd->m_stats.m_gpuWait = *static_cast<uint64_t*>(device->FrameEnd(&wait, FrameQuery(device)));
    frameEnd->m_stats.m_presentTime = device->Field<int64_t>(0x150);
    frameEnd->m_stats.m_latencyFrames = device->Field<uint8_t>(0x14e);
    frameEnd->m_stats.m_gpuTime = 0;
    uint64_t stamps[2];
    void* query = FrameQuery(device);
    if (query != nullptr && device->QueryGetResult(query, stamps, 0x10) && stamps[0] < stamps[1]) {
        uint64_t frequency = device->m_ready != 0 ? device->Field<uint64_t>(0x130) : 1;
        frameEnd->m_stats.m_gpuBegin = stamps[0];
        frameEnd->m_stats.m_gpuEnd = stamps[1];
        frameEnd->m_stats.m_gpuTime = (stamps[1] - stamps[0]) * 1000000000 / frequency;
    }
}

// ref: FUN_142292750
void GxFrameEndTimes(GXFRAMEEND* frameEnd) {
    int64_t now = OsGetPerformanceCounter();
    frameEnd->m_stats.m_frameTime = OsTicksToNanoseconds(now - frameEnd->m_lastEnd);
    frameEnd->m_stats.m_beginWait = OsTicksToNanoseconds(frameEnd->m_beginStamp - frameEnd->m_waitEnd);
    int64_t cpu = OsTicksToNanoseconds(now - frameEnd->m_beginStamp);
    frameEnd->m_lastEnd = now;
    frameEnd->m_stats.m_cpuTime = cpu - static_cast<int64_t>(frameEnd->m_stats.m_gpuWait);
}

// ref: FUN_142292640
void GxFrameEndMark(GXFRAMEEND* frameEnd) {
    frameEnd->m_frame = static_cast<CGxDevice*>(g_gxDevice)->Field<uint32_t>(0x138);
    _InterlockedOr8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), 1);
}

// Ends the frame on this thread.
// ref: FUN_142292680
void GxFrameEndNow(GXFRAMEEND* frameEnd) {
    frameEnd->m_callback.m_manager(frameEnd->m_callback.m_storage, nullptr, 0);
    frameEnd->m_callback.m_invoke = nullptr;
    frameEnd->m_callback.m_manager = blz::function_empty_manager;
    GxFrameEndRecord(frameEnd);
    static_cast<CGxDevice*>(g_gxDevice)->LatencyMarker(3, frameEnd->m_frame);
    GxFrameEndTimes(frameEnd);
    _InterlockedOr8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), 4);
}

// The overlapped end's pool task.
// ref: FUN_142292820
void GxFrameEndTask(void* storage) {
    (void)storage;
    if (g_gxDevice != nullptr) {
        GxFrameEndRecord(&s_frameEnd);
        _InterlockedOr8(reinterpret_cast<volatile char*>(&s_frameEnd.m_pending), 8);
    }
}

// Ends the frame on a pool thread, keeping `callback` for when it completes.
// ref: FUN_1422922d0
void GxFrameEndOverlapped(GXFRAMEEND* frameEnd, blz::function_storage* callback) {
    frameEnd->m_callback.m_manager(frameEnd->m_callback.m_storage, nullptr, 0);
    callback->m_manager(frameEnd->m_callback.m_storage, callback->m_storage, 2);
    frameEnd->m_callback.m_invoke = callback->m_invoke;
    frameEnd->m_callback.m_manager = callback->m_manager;
    callback->m_manager = blz::function_empty_manager;
    callback->m_invoke = nullptr;
    _InterlockedAnd8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), static_cast<char>(0xf7));
    _InterlockedOr8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), 2);
    ThreadPoolGroupSetCount(frameEnd->m_group, 1);
    // FUN_14032a520 invokes the function pointer held in the storage.
    blz::function_storage task = { reinterpret_cast<void*>(&GxFrameEndTask), blz::function_trivial_manager, {} };
    ThreadPoolSubmit(1, &task, 2, frameEnd->m_group);
    task.m_manager(task.m_storage, nullptr, 0);
    GxFrameEndTimes(frameEnd);
    _InterlockedOr8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), 4);
}

// The GPU's clock, temperature and throttling, for the periodic report.
// ref: FUN_14228a490
void GxGpuStatusReport(blz::string_header* report) {
    struct GPUSTATUS {
        uint32_t m_frequency;       // kHz
        uint32_t m_temperature;     // C
        uint8_t m_throttled;
        uint8_t m_pad[3];
        uint32_t m_reasons;         // NV_GPU_PERF_DECREASE_REASON_* bits
    } status = {};
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    if (device != nullptr) {
        uint8_t out[0x20];
        status = *static_cast<GPUSTATUS*>(g_gxPrismInstance->Slot80(out, device->m_gpu));
    }
    if (status.m_frequency != 0) {
        blz::string_append_format(report, "Current Frequency:%.2fGHz ", static_cast<double>(static_cast<float>(status.m_frequency) * 1e-6f));
    }
    if (status.m_temperature != 0) {
        blz::string_append_format(report, "Temperature:%uC ", status.m_temperature);
    }
    if (status.m_throttled == 0) {
        return;
    }
    StringAppend(report, "GPU is throttled. ");
    if (!GxPlatformHasNvapi() || device->Field<uint16_t>(0xa0) != 0x10de) {
        return;
    }
    StringAppend(report, "Reason: ");
    if ((status.m_reasons & 1) != 0) {
        StringAppend(report, "NV_GPU_PERF_DECREASE_REASON_THERMAL_PROTECTION ");
    }
    if ((status.m_reasons & 2) != 0) {
        StringAppend(report, "NV_GPU_PERF_DECREASE_REASON_POWER_CONTROL ");
    }
    if ((status.m_reasons & 4) != 0) {
        StringAppend(report, "NV_GPU_PERF_DECREASE_REASON_AC_BATT ");
    }
    if ((status.m_reasons & 8) != 0) {
        StringAppend(report, "NV_GPU_PERF_DECREASE_REASON_API_TRIGGERED ");
    }
    if ((status.m_reasons & 0x10) != 0) {
        StringAppend(report, "NV_GPU_PERF_DECREASE_REASON_INSUFFICIENT_POWER ");
    }
    if (static_cast<int32_t>(status.m_reasons) < 0) {
        StringAppend(report, "NV_GPU_PERF_DECREASE_REASON_UNKNOWN ");
    }
}

// Starts the frame on the device; a failure is a lost device, which asks for a restart.
// ref: FUN_1422914a0
bool GxFrameEndBegin(GXFRAMEEND* frameEnd) {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    device->LatencyMarker(1, device->Field<uint32_t>(0x138));
    device->LatencyMarker(2, device->Field<uint32_t>(0x138));
    if (frameEnd->m_busy != 0) {
        return false;
    }
    frameEnd->m_busy = 1;
    if (frameEnd->m_began == 0) {
        frameEnd->m_beginStamp = OsGetPerformanceCounter();
        if (g_gxDevice != nullptr) {
            if (device->FrameBegin(FrameQuery(device))) {
                device->Slot188();
                if (OsTicksToNanoseconds(frameEnd->m_beginStamp - frameEnd->m_lastReport) > 300000000000ll) {
                    frameEnd->m_lastReport = frameEnd->m_beginStamp;
                    blz::inline_string<0x100> report;
                    GxGpuStatusReport(report.header());
                    if (report.m_size != 0) {
                        GxLog("Periodic Gpu Status Report: %s", report.m_data);
                    }
                }
                frameEnd->m_began = 1;
                frameEnd->m_busy = 0;
                return true;
            }
            s_deviceLostCount++;
            GxLog("Device context was lost. Attempting recovery. Occurrence: %d", s_deviceLostCount);
            s_deviceRestartPending = 1;
            GxLogFlushNow();
        }
    }
    frameEnd->m_busy = 0;
    return false;
}

// Marks the frame on the device and, under low-latency mode, sleeps so the CPU does not run
// further ahead of the GPU than the frames in flight allow.
// ref: FUN_142292520
void GxFrameEndLatencyWait(GXFRAMEEND* frameEnd) {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    if (device == nullptr) {
        return;
    }
    uint32_t frame = device->Field<uint32_t>(0x138);
    bool changed = frame != s_lastMarkedFrame;
    s_lastMarkedFrame = frame;
    if (!changed) {
        return;
    }
    uint64_t start = OsTicksToNanoseconds(OsGetPerformanceCounter());
    device->FrameMarker();
    int64_t busy = frameEnd->m_stats.m_beginWait + frameEnd->m_stats.m_cpuTime;
    if (device->Field<uint8_t>(0x14d) == 1 && device->Field<uint32_t>(0x138) > 10) {
        uint32_t latency = frameEnd->m_stats.m_latencyFrames;
        int64_t gpu = static_cast<int64_t>(frameEnd->m_stats.m_gpuTime);
        if (latency - 2 < 3 && busy < gpu && static_cast<uint64_t>(gpu - 1000001) < 248999999) {
            int64_t sleep = gpu - busy;
            if (latency > 2) {
                sleep += static_cast<int64_t>(latency - 2) * gpu;
            }
            OsSleepPrecise(sleep);
        }
    }
    frameEnd->m_waitEnd = OsGetPerformanceCounter();
    uint64_t end = OsTicksToNanoseconds(frameEnd->m_waitEnd);
    frameEnd->m_waited = static_cast<int64_t>(end - start);
    if (end <= start || static_cast<int64_t>(end - start) < 10000) {
        frameEnd->m_waited = 0;
    }
}

// ref: FUN_1422917d0
bool GxFrameEndArm(GXFRAMEEND* frameEnd, bool arm) {
    if (g_gxDevice == nullptr) {
        GxGetApi();
    }
    if (arm) {
        frameEnd->m_overlapActive = frameEnd->m_overlap;
    }
    return false;
}

}

// ref: FUN_14228b8c0
uint8_t GxFrameSetPhase(uint8_t phase) {
    uint8_t previous = s_framePhase;
    s_framePhase = phase;
    return previous;
}

// ref: FUN_142289f20
bool GxFrameBegin() {
    return GxFrameEndBegin(&s_frameEnd);
}

// ref: FUN_14228b3a0
void GxFrameLatencyWait() {
    GxFrameEndLatencyWait(&s_frameEnd);
}

// Whether a frame has begun and not yet ended.
// ref: FUN_14228a040
bool GxFrameBegan() {
    return s_frameEnd.m_began != 0;
}

// ref: FUN_14228a950
bool GxFrameArm(bool arm) {
    return GxFrameEndArm(&s_frameEnd, arm);
}

// ref: FUN_14228c1a0
void GxFrameWorkFlush(uint8_t mode) {
    GxFrameEndWait(&s_frameEnd, mode, 1);
}

// Ends the frame: overlapped on a pool thread, or here with `callback` run on the stats.
// ref: FUN_142289f30
void GxFrameEnd(blz::function_storage* callback, bool overlapped) {
    GxFrameEndMark(&s_frameEnd);
    if (overlapped) {
        GxFrameEndOverlapped(&s_frameEnd, callback);
        GxFrameEndWait(&s_frameEnd, 0, 1);
        return;
    }
    GxFrameEndNow(&s_frameEnd);
    GxFrameEndWait(&s_frameEnd, 2, 0);
    if (callback->m_invoke != nullptr) {
        GXFRAMESTATS stats = s_frameEnd.m_stats;
        reinterpret_cast<void (*)(GXFRAMESTATS*, void*)>(callback->m_invoke)(&stats, callback->m_storage);
    }
}

// Whether a frame-rate cap applies (any cap, or `background` asked for).
// ref: FUN_14228aa60
bool GxFrameLimited(uint32_t fps) {
    uint32_t cap = FrameRateCap();
    return fps != 0 || ((cap + 1) & 0xfffffffe) != 0;
}

// Sleeps out the rest of the frame under the cap (`fps`, when lower, overrides it).
// ref: FUN_14228c9f0
void GxFrameLimit(uint32_t fps) {
    uint32_t cap = FrameRateCap();
    uint32_t limit = cap;
    if (fps != 0) {
        limit = fps;
        if (cap != 0 && cap < fps) {
            limit = cap;
        }
    }
    uint64_t next = s_frameLimitNext;
    if (limit != s_frameLimitFps) {
        uint64_t ticks = 0;
        s_frameLimitFps = limit;
        if (limit != 0) {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);  // thunk_FUN_140266b90
            ticks = static_cast<uint64_t>(frequency.QuadPart) / limit;
        }
        s_frameLimitTicks = ticks;
        next = 0;
        if (s_frameLimitFps != 0) {
            next = static_cast<uint64_t>(OsGetPerformanceCounter()) + s_frameLimitTicks;
        }
    }
    s_frameLimitNext = next;
    if (s_frameLimitFps == 0) {
        return;
    }
    uint64_t now = static_cast<uint64_t>(OsGetPerformanceCounter());
    if (s_frameLimitNext <= now) {
        s_frameLimitNext = now + s_frameLimitTicks;
        return;
    }
    OsSleepPrecise(OsTicksToNanoseconds(s_frameLimitNext - now));
    s_frameLimitNext += s_frameLimitTicks;
}

// ref: FUN_14228a940
uint32_t GxDeviceLostCount() {
    return s_deviceLostCount;
}

// ref: FUN_14228b7a0
void GxRequestRestart() {
    s_deviceRestartPending = 1;
}

// ---- the per-frame update ------------------------------------------------------------------

namespace {

CALLBACKDEQUE s_frameStartCallbacks;  // DAT_143490938

// ref: FUN_14228e930
void GxRunRestartHandler(uint8_t api) {
    if (s_restartHandler.m_invoke != nullptr) {
        reinterpret_cast<void (*)(uint8_t, void*)>(s_restartHandler.m_invoke)(api, s_restartHandler.m_storage);
    }
}

// ref: FUN_14228e3c0
void GxRunDisplayChangedHandler() {
    if (s_displayChangedHandler.m_invoke != nullptr) {
        reinterpret_cast<void (*)(void*)>(s_displayChangedHandler.m_invoke)(s_displayChangedHandler.m_storage);
    }
}

// ref: FUN_14228e3e0
void GxRunNoGpuHandler() {
    if (s_noGpuHandler.m_invoke != nullptr) {
        reinterpret_cast<void (*)(void*)>(s_noGpuHandler.m_invoke)(s_noGpuHandler.m_storage);
    }
}

}

// ref: FUN_14228e190
void GxAddFrameStartCallback(GXDEVICECALLBACK callback, void* param) {
    PushCallback(&s_frameStartCallbacks, callback, param);
}

// ref: FUN_14228e8f0
void GxRemoveFrameStartCallback(GXDEVICECALLBACK callback, void* param) {
    CallbackRemove(&s_frameStartCallbacks, callback, param);
}

// ref: FUN_14228e710
void GxRemoveEngineDeviceCreatedCallback(GXDEVICECALLBACK callback, void* param) {
    CallbackRemove(&s_engineDeviceCreatedCallbacks, callback, param);
}

// ref: FUN_14228e910
void GxRemoveLowMemoryCallback(GXDEVICECALLBACK callback, void* param) {
    CallbackRemove(&s_lowMemoryCallbacks, callback, param);
}

// Whether the window has the focus, which picks maxFPS over maxFPSBk (set each frame by
// WindowUpdateForeground).
// ref: FUN_14228b800
void GxSetForeground(bool foreground) {
    s_foregroundFps = foreground;
}

// Once a frame: notices a changed GPU set (re-enumerating, and restarting the device when the
// GPU went away or a better one arrived), completes the last frame's end, runs a pending
// restart, marks the frame's start and runs the frame-start callbacks.
// ref: FUN_14228c670
void GxFrameUpdate() {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    bool current = g_gxPrismInstance->IsCurrent();
    if (!current || s_deviceRestartPending != 0) {
        GxFrameEndWait(&s_frameEnd, 2, 1);
        if (!current) {
            GxLog("Gpu state has changed. re-enumerating");
            uint8_t vendor = device->Field<uint8_t>(0xaa);
            for (int32_t attempt = 0;; attempt++) {
                pm::RESULT result;
                g_gxPrismInstance->Rebuild(&result, 2, 3);
                if (result.m_code != 0) {
                    GxLog("Failure on Gpu Enumerate call: %s (0x%08X). Retrying 1 second...", pm::ResultCodeName(result.m_code),
                        result.m_hresult);
                } else if (!GxBuildAdapterList()) {
                    GxLog("No GPU found, Retrying in 1 second...");
                } else {
                    bool found = vendor == 4 || attempt > 0x19;
                    for (uint64_t i = 0; !found && i < g_gxAdapters.m_size; i++) {
                        pm::GPU* gpu = g_gxAdapters.m_data[i].m_gpu;
                        found = gpu->m_deviceId == device->Field<uint16_t>(0xa2) && gpu->m_vendorId == device->Field<uint16_t>(0xa0);
                    }
                    if (found) {
                        if (s_driverChanged != 0) {
                            GxLog("Driver Update Detected");
                        }
                        GxReleasePlatformHandles();
                        GxLoadPlatformHandles(false);
                        if (!device->RebindAdapter()) {
                            device->m_gpu = nullptr;
                            device->m_data = nullptr;
                            GxLog(s_driverChanged != 0 ? "Restarting..." : "Gpu Eject Detected. Restarting...");
                            s_deviceRestartPending = 1;
                        } else if (GxSelectGpu(device->m_api, &device->m_params.m_adapterName, false) != device->m_gpu) {
                            GxLog("More Suitable Gpu Found. Switching to it..");
                            s_deviceRestartPending = 1;
                        }
                        s_driverChanged = 0;
                        GxLogFlush();
                        GxRunDisplayChangedHandler();
                        break;
                    }
                    GxLog("Waiting for GPU to come back, Rechecking in 1 second... (0x%hu, 0x%hu)", device->Field<uint16_t>(0xa0),
                        device->Field<uint16_t>(0xa2));
                }
                GxLogFlush();
                OsSleepNanoseconds(1000000000);
                if (attempt + 1 > 0x1d) {
                    GxLog("No Gpu Found. Aborting.");
                    GxRunNoGpuHandler();
                    OsTerminateProcess();
                }
            }
        }
    } else if (!GxFrameEndArm(&s_frameEnd, false)) {
        GxFrameEndWait(&s_frameEnd, 2, 1);
    }
    if (s_deviceRestartPending != 0) {
        GxRunRestartHandler(device->m_0a);
        s_deviceRestartPending = 0;
    }
    device = static_cast<CGxDevice*>(g_gxDevice);
    device->LatencyMarker(0, device->Field<uint32_t>(0x138));
    uint64_t end = s_frameStartCallbacks.m_size + s_frameStartCallbacks.m_offset;
    for (uint64_t index = s_frameStartCallbacks.m_offset; index != end; index++) {
        CALLBACKPAIR* pair = CallbackSlot(&s_frameStartCallbacks, index);
        pair->m_callback(pair->m_param);
    }
    ThreadPoolReclaim();
}

// The device takes the low-latency mode, and logs it.
// ref: FUN_14228b810
void GxSetLowLatencyMode(uint8_t mode) {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    device->SetLowLatencyMode(mode);
    device->Field<uint8_t>(0x14d) = mode;
    static const char* const s_names[4] = { "None", "BuiltIn", "Reflex", "Reflex+Boost" };
    GxLog("GxLowLatencyMode: %s", mode < 4 ? s_names[mode] : "Unknown");
}

// The device's dedicated and shared video memory in MB (FUN_1422918d0).
// ref: FUN_14228aa40
void GxDeviceVideoMemory(uint32_t* dedicated, uint32_t* shared) {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    *dedicated = device->Field<uint32_t>(0x15c);
    *shared = device->Field<uint32_t>(0x160);
}

// Whether a format multisamples; vendor 3 below 1900 MB of video memory never does.
// ref: FUN_142289e20
bool GxFormatSupportsMultisample(uint8_t format, uint8_t samples, int32_t quality) {
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    if (device->Field<uint8_t>(0xaa) == 3) {
        uint32_t dedicated;
        uint32_t shared;
        GxDeviceVideoMemory(&dedicated, &shared);
        if (dedicated < 0x76c) {
            return false;
        }
    }
    return device->FormatSupportsMultisample(format, samples, quality);
}

// ref: FUN_14228ac40
bool GxShadingRateSupported(uint32_t rate, int32_t a2, uint8_t a3) {
    return static_cast<CGxDevice*>(g_gxDevice)->ShadingRateSupported(rate, a2, a3);
}

// The D3D11 APIs and legacy Metal (FUN_142291a50: 0 or 3) and the D3D11 ones (FUN_1422919f0).
// ref: FUN_142291a50
bool GxApiIsLegacy() {
    uint8_t api = g_gxDevice != nullptr ? static_cast<CGxDevice*>(g_gxDevice)->m_api : GxGetApi();
    return api == GXAPI_D3D11_LEGACY || api == GXAPI_MTL_LEGACY;
}

// ref: FUN_14228ac60
bool GxVariableRateShadingSupported(uint8_t tier) {
    return static_cast<CGxDevice*>(g_gxDevice)->VariableRateShadingSupported(tier);
}

// The device's API, or the one that would be chosen before there is a device.
// ref: FUN_1422919d0
uint8_t GxCurrentApi() {
    return g_gxDevice != nullptr ? static_cast<CGxDevice*>(g_gxDevice)->m_api : GxGetApi();
}

// ref: FUN_1422919f0
bool GxApiIsD3d11() {
    uint8_t api = g_gxDevice != nullptr ? static_cast<CGxDevice*>(g_gxDevice)->m_api : GxGetApi();
    return api < 2;
}

// The device's capabilities and caches (FUN_14228a0b0): the block from +0x80.
// ref: FUN_14228a0b0
uint8_t* GxDeviceCaps() {
    return static_cast<uint8_t*>(g_gxDevice) + 0x80;
}

// ref: FUN_14228a030
uint32_t GxDeviceFrame() {
    return static_cast<CGxDevice*>(g_gxDevice)->Field<uint32_t>(0x138);
}

// ---- textures through the device ------------------------------------------------------------

// The texture a description makes. Off the main thread it needs the device lock shared; a
// texture that fails becomes the default texture. Textures carry the device generation in
// bits 1-5 of their flags so one from an older device is recognized.
// ref: FUN_14228c1d0
bool GxTexCreate(const void* descHandle, void** tex) {
    *tex = nullptr;
    // Formats that cannot multisample or take a mip mode (local table in the reference).
    static const uint8_t s_compressed[0x23] = { 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 };
    auto desc = static_cast<const uint8_t*>(descHandle);
    if (s_compressed[desc[0x10]] != 0 && ((desc[4] & 3) != 0 || (desc[8] & 3) != 0)) {
        return false;
    }
    bool shared = false;
    if (*reinterpret_cast<void* const*>(desc + 0x20) == nullptr && OsGetCurrentThreadId() != s_mainThreadId) {
        shared = s_deviceLock.try_lock_shared();
        if (!shared) {
            return false;
        }
    }
    bool created = false;
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    if (device != nullptr) {
        if (device->m_ready != 0) {
            created = device->TexCreate(descHandle, tex);
            if (!created || *tex == nullptr) {
                if (*tex != nullptr) {
                    device->TexDestroy(*tex);
                }
                *tex = device->DefaultTex();
                created = false;
            } else {
                uint32_t& flags = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(*tex) + 0x10);
                flags = ((s_deviceGeneration & 0x1f) << 1) | (flags & 0xffffffc1);
            }
        } else {
            *tex = device->DefaultTex();
        }
    }
    if (shared) {
        s_deviceLock.unlock_shared();
    }
    return created;
}

// Destroys a texture unless it is the shared default; one from an older device is freed
// without the device.
// ref: FUN_14228f5d0
void GxTexUpdate(GxUploadInterface* upload, void* tex, const int32_t* rect, bool shaderResource,
    GXTEXUPDATEPROC callback, void* param) {
    if (tex == nullptr || !GxDevExists()) {
        return;
    }
    if (ThreadPoolIsMainThread()) {
        GxFrameWorkFlush(2);
    }
    upload->TexUpdate(tex, rect, shaderResource, callback, param);
}

// ref: FUN_14228f9a0
void GxTexUpdateInFrame(GxRenderContext* context, void* tex, const int32_t* rect, bool shaderResource,
    GXTEXUPDATEPROC callback, void* param) {
    if (tex == nullptr || !GxDevExists()) {
        return;
    }
    GxFrameWorkFlush(2);
    context->UpdateTex(static_cast<gxDx12Backend::Tex*>(tex), rect, shaderResource, callback, param);
}

// ref: FUN_142288460
void GxTexSetFilter(void* tex, uint8_t filter, uint32_t anisotropy) {
    auto usage = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(tex) + 0x14);
    uint32_t value = ((filter ^ *usage) & 7) ^ *usage;
    *usage = (((anisotropy << 0xd) ^ value) & 0x3e000) ^ value;
}

// ref: FUN_142289ef0
void GxDeviceBeginTextureReload() {
    GxFrameEndWait(&s_frameEnd, 2, 1);
    static_cast<CGxDevice*>(g_gxDevice)->Slot008();
}

// ref: FUN_14228b950
void GxDeviceEndTextureReload() {
    static_cast<CGxDevice*>(g_gxDevice)->Slot010();
}

// ref: FUN_14228c390
void GxTexDestroy(void** tex) {
    if (*tex == nullptr) {
        return;
    }
    uint32_t flags = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(*tex) + 0x10);
    if ((flags & 0x40) == 0) {
        bool other = OsGetCurrentThreadId() != s_mainThreadId;
        if (other) {
            s_deviceLock.lock_shared();
        }
        if (((flags >> 1 ^ s_deviceGeneration) & 0x1f) == 0) {
            static_cast<CGxDevice*>(g_gxDevice)->TexDestroy(*tex);
        } else {
            GxLog("Deleting texture found from previous device instance!");
            operator delete(*tex, 0x1c);
        }
        if (other) {
            s_deviceLock.unlock_shared();
        }
    }
    *tex = nullptr;
}

// ref: FUN_14228c490
bool GxTexHeapCreate(const void* desc, void** heap) {
    *heap = nullptr;
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    if (device->Field<uint8_t>(0x12a) == 0) {
        return false;
    }
    return device->TexHeapCreate(desc, heap);
}

// ref: FUN_14228c4c0
void GxTexHeapDestroy(void* heap) {
    if (heap != nullptr) {
        static_cast<CGxDevice*>(g_gxDevice)->TexHeapDestroy(heap);
    }
}

// ref: FUN_142289a20
GXTEXALLOCINFO* GxTexAllocationInfo(GXTEXALLOCINFO* out, const void* desc) {
    if (g_gxDevice != nullptr) {
        GXTEXALLOCINFO info;
        *out = *static_cast<CGxDevice*>(g_gxDevice)->TexAllocationInfo(&info, desc);
        return out;
    }
    out->m_size = 0;
    out->m_alignment = 0;
    out->m_pad = 0;
    return out;
}

// A texture's description back from the texture.
// ref: FUN_1422884b0
void* GxTexGetDesc(const void* tex, void* out) {
    auto bytes = static_cast<const uint8_t*>(tex);
    auto desc = static_cast<uint8_t*>(out);
    std::memset(desc, 0, 0x30);
    desc[0] = bytes[7];
    *reinterpret_cast<uint32_t*>(desc + 4) = *reinterpret_cast<const uint16_t*>(bytes);
    *reinterpret_cast<uint32_t*>(desc + 8) = *reinterpret_cast<const uint16_t*>(bytes + 2);
    *reinterpret_cast<uint32_t*>(desc + 0xc) = *reinterpret_cast<const uint16_t*>(bytes + 4);
    desc[0x10] = bytes[8];
    desc[0x11] = bytes[9];
    *reinterpret_cast<uint64_t*>(desc + 0x14) = *reinterpret_cast<const uint64_t*>(bytes + 0x14);
    return out;
}

// ref: FUN_142288440
void GxDeviceRelease098(void* holder, void* object) {
    (void)holder;
    if (object != nullptr) {
        FROST_UNIMPLEMENTED();  // the device's slot 0x98
    }
}

// ref: FUN_1422883e0
void GxDeviceRelease0e0(void* holder, void* object) {
    (void)holder;
    if (object != nullptr) {
        FROST_UNIMPLEMENTED();  // the device's slot 0xe0
    }
}

// ref: FUN_142288400
void GxDeviceRelease0f8(void* holder, void* object) {
    (void)holder;
    (void)object;
    FROST_UNIMPLEMENTED();  // the device's slot 0xf8
}

// ref: FUN_14228c330
bool GxTexCreateParams(uint8_t kind, uint32_t width, uint32_t height, uint32_t depth, uint8_t format,
    uint8_t mips, uint64_t flags, const char* name, void** tex) {
    (void)name;
    uint8_t desc[0x30] = {};
    desc[0] = kind;
    std::memcpy(desc + 4, &width, 4);
    std::memcpy(desc + 8, &height, 4);
    std::memcpy(desc + 0xc, &depth, 4);
    desc[0x10] = format;
    desc[0x11] = mips;
    std::memcpy(desc + 0x14, &flags, 8);
    return GxTexCreate(desc, tex);
}

// ref: FUN_14228b670
void* GxQueryCreate(uint8_t type) {
    return static_cast<CGxDevice*>(g_gxDevice)->QueryCreate(type);
}

// ref: FUN_14228b690
void GxQueryDestroy(void* query) {
    if (query != nullptr) {
        static_cast<CGxDevice*>(g_gxDevice)->QueryDestroy(query);
    }
}

// ref: FUN_14228b6e0
bool GxQueryInfo(void* query, uint8_t what, uint32_t* out) {
    if (query == nullptr) {
        return false;
    }
    return static_cast<CGxDevice*>(g_gxDevice)->QueryInfo(query, what, out);
}

// ref: FUN_142288550
uint32_t GxDeviceBufferedFrames() {
    return static_cast<CGxDevice*>(g_gxDevice)->Field<uint32_t>(0x144);
}

// ref: FUN_142288530
uint32_t GxDeviceFrameSlot() {
    return static_cast<CGxDevice*>(g_gxDevice)->Field<uint32_t>(0x13c);
}

namespace {

// The viewport (DAT_142d41c20..DAT_142d41c34): x, then y, then z, each from and to.
float s_viewport[6] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};

}  // namespace

// ref: FUN_14228aa10
bool GxFramePhaseOpen() {
    return s_framePhase != 0 || s_frameSceneOpen == 0;
}

// ref: FUN_14228a9b0
uint32_t GxDeviceFrameTime() {
    if (g_gxDevice == nullptr) {
        return 0;
    }
    return static_cast<CGxDevice*>(g_gxDevice)->Field<uint32_t>(0x158);
}

// ref: FUN_14228cbd0
void GxXformGetViewport(float* minX, float* maxX, float* minY, float* maxY, float* minZ, float* maxZ) {
    *minY = s_viewport[2];
    *maxY = s_viewport[3];
    *minX = s_viewport[0];
    *maxX = s_viewport[1];
    *minZ = s_viewport[4];
    *maxZ = s_viewport[5];
}

// ref: FUN_14228cb00
void GxXformApplyViewport() {
    float minY = s_viewport[2] <= 0.0f ? 0.0f : s_viewport[2];
    float maxX = 1.0f <= s_viewport[1] ? 1.0f : s_viewport[1];
    float maxY = 1.0f <= s_viewport[3] ? 1.0f : s_viewport[3];
    float minX = s_viewport[0] <= 0.0f ? 0.0f : s_viewport[0];
    GxRenderContext* context = g_gxRenderContext;
    if (minX != context->Field<float>(0x1c) || maxX != context->Field<float>(0x20)
        || minY != context->Field<float>(0x24) || maxY != context->Field<float>(0x28)
        || s_viewport[4] != context->Field<float>(0x2c) || s_viewport[5] != context->Field<float>(0x30)) {
        context->Field<float>(0x1c) = minX;
        context->Field<float>(0x20) = maxX;
        context->Field<float>(0x24) = minY;
        context->Field<float>(0x28) = maxY;
        context->Field<float>(0x2c) = s_viewport[4];
        context->Field<float>(0x30) = s_viewport[5];
        context->Field<uint8_t>(0x18) = 1;
    }
    s_framePhase = 0;
}
