#include "gx/Gx.hpp"
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

// The frame-end helper (DAT_142d41b30).
struct GXFRAMEEND {
    uint8_t m_00[8];
    void* m_threadPool;             // 0x08
    uint8_t m_10[0x79];
    uint8_t m_overlap;              // 0x89: GxFrameEndMode (always 0: the feature is not available)
    uint8_t m_8a[6];
    uint8_t m_pending;              // 0x90
    uint8_t m_91[0x57];
};
GXFRAMEEND s_frameEnd;

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
GxPrismAllocator s_prismAllocator;

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

// ref: FUN_14235a070
void* ThreadPoolCreate(uint32_t priority) {
    (void)priority;
    FROST_UNIMPLEMENTED();  // Engine/Source/ThreadPool is not ported
    return nullptr;
}

// ref: FUN_142291c50
void GxFrameEndInitialize(GXFRAMEEND* frameEnd) {
    InterlockedExchange8(reinterpret_cast<volatile char*>(&frameEnd->m_pending), 0);
    if (frameEnd->m_threadPool == nullptr) {
        frameEnd->m_threadPool = ThreadPoolCreate(2);
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

// ref: FUN_1422918f0
void GxOnDeviceCreated(void* param) {
    (void)param;
    FROST_UNIMPLEMENTED();  // the device's per-frame objects: needs CGxDevice
}

// ref: FUN_142291980
void GxOnDeviceDestroyed(void* param) {
    (void)param;
    FROST_UNIMPLEMENTED();  // releases what GxOnDeviceCreated made
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

// Creates the device for `api` with the console's settings, returning whether it is up.
// ref: FUN_142289c40
bool GxDevCreate(uint8_t api, const GXDEVICEPARAMS* params) {
    (void)api;
    (void)params;
    // The CGxDevice and its Prism device and swap chain are the next port. Until then the
    // device "succeeds" without existing, so the window and main loop can come up.
    FROST_UNIMPLEMENTED();
    return true;
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
    (void)frameEnd;
    FROST_UNIMPLEMENTED();  // waits out and releases the frame-end thread pool work
}

// ref: FUN_142292a10
void GxFrameEndWait(GXFRAMEEND* frameEnd, uint32_t a2, uint32_t a3) {
    (void)frameEnd;
    (void)a2;
    (void)a3;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14235a710
void ThreadPoolFlush(uint32_t priority) {
    (void)priority;
    FROST_UNIMPLEMENTED();  // Engine/Source/ThreadPool is not ported
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
        FROST_UNIMPLEMENTED();  // CGxDevice slots 0x38 (shutdown) and 0x18 (destroy)
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
