#include "prism/Instance.hpp"
#include "blz/Murmur.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
#include <dxgi.h>

namespace pm {

namespace {

const char s_deviceNodeTag[] = ".?AU?$chained_hash_node@U?$pair@QEBUGpu@pm_api@@UDeviceData@2@@blz@@@blz@@";
const char s_deviceBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@QEBUGpu@pm_api@@UDeviceData@2@@blz@@@blz@@";
const char s_deviceSetTag[] = ".?AU?$chained_hash_node@PEAVpmDevice@@@blz@@";
const char s_deviceSetBucketTag[] = ".PEAU?$chained_hash_node@PEAVpmDevice@@@blz@@";

// The severity names and the default messenger (vtable 0x14291c3c8).
const char* SeverityName(uint8_t severity) {  // FUN_142359270
    static const char* const s_names[] = { "Info", "Warning", "Error" };
    return severity < 3 ? s_names[severity] : "Unknown";
}

class DefaultMessenger : public Messenger {
    public:
    // ref: FUN_142310150
    void Report(uint8_t severity, const char* function, RESULT result, uint32_t flags, const char* text) override {
        char code[0x20];
        if (severity == 0 && result.m_hresult == 0) {
            code[0] = '\0';
        } else {
            SStrPrintf(code, sizeof(code), " %s (0x%08x)", ResultCodeName(result.m_code), static_cast<uint32_t>(result.m_hresult));
        }
        char where[0x80];
        if (function == nullptr) {
            where[0] = '\0';
        } else {
            SStrPrintf(where, sizeof(where), "%s: ", function);
        }
        char line[0x400];
        SStrPrintf(line, sizeof(line), "prism: %s%s: %s%s%s", SeverityName(severity), code, where, text, "\n");
        if (!IsDebuggerPresent() || (flags & 1) == 0) {
            std::fputs(line, stdout);
            std::fflush(stdout);
        } else {
            OutputDebugStringA(line);
        }
        if (IsDebuggerPresent() && (flags & 2) != 0) {
            __debugbreak();
        }
    }

};

// ref: FUN_142310290
Messenger* DefaultMessengerGet() {
    static DefaultMessenger s_messenger;
    return &s_messenger;
}

// ref: FUN_142359530
bool ShouldBreak(uint8_t level, uint8_t severity) {
    if (level == 1) {
        return severity > 1;
    }
    if (level == 2) {
        return severity != 0;
    }
    return level == 3;
}

uint64_t ChainedCapacity(uint64_t count, float maxLoad, uint64_t minimum) {
    float load = static_cast<float>(count);
    uint64_t need = static_cast<uint64_t>(load / maxLoad);
    if (load > static_cast<float>(need) * maxLoad) {
        need++;
    }
    if (need < minimum) {
        need = minimum;
    }
    uint64_t capacity = 8;
    if (need > 8) {
        do {
            capacity *= 2;
        } while (capacity < need);
    }
    return capacity;
}

// The device-data map's rehash.
// ref: FUN_14234fa50
void RehashDeviceMap(BACKENDSLOT* slot, uint64_t minimum) {
    uint64_t capacity = ChainedCapacity(slot->m_count, slot->m_maxLoad, minimum);
    if (capacity == slot->m_capacity && slot->m_buckets != nullptr) {
        return;
    }
    auto buckets = static_cast<DEVICENODE**>(
        SMemAlloc(capacity * 8, 0xB, s_deviceBucketTag, SStrHashFnv1a(s_deviceBucketTag), -6, 0));
    std::memset(buckets, 0, capacity * 8);
    if (slot->m_buckets != nullptr) {
        for (uint64_t i = 0; i < slot->m_capacity; i++) {
            for (DEVICENODE* node = slot->m_buckets[i]; node != nullptr;) {
                DEVICENODE* next = node->m_next;
                uint64_t index = (capacity - 1) & blz::fmix64(reinterpret_cast<uint64_t>(node->m_key));
                node->m_next = buckets[index];
                buckets[index] = node;
                node = next;
            }
        }
    }
    SMemFree(slot->m_buckets, s_deviceBucketTag, -6, 0);
    slot->m_buckets = buckets;
    slot->m_capacity = capacity;
}

// ref: FUN_14234e2d0
void FreeDeviceSet(DEVICENODE* node) {
    if (node->m_devices == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < node->m_devicesCapacity; i++) {
        for (void** entry = static_cast<void**>(node->m_devices[i]); entry != nullptr;) {
            void** next = static_cast<void**>(*entry);
            SMemFree(entry, s_deviceSetTag, -6, 0);
            entry = next;
        }
    }
    SMemFree(node->m_devices, s_deviceSetBucketTag, -6, 0);
    node->m_devices = nullptr;
}

// ref: FUN_14234e220
void FreeDeviceMap(BACKENDSLOT* slot) {
    if (slot->m_buckets == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < slot->m_capacity; i++) {
        for (DEVICENODE* node = slot->m_buckets[i]; node != nullptr;) {
            DEVICENODE* next = node->m_next;
            FreeDeviceSet(node);
            SMemFree(node, s_deviceNodeTag, -6, 0);
            node = next;
        }
    }
    SMemFree(slot->m_buckets, s_deviceBucketTag, -6, 0);
    slot->m_buckets = nullptr;
}

// NVIDIA Reflex's PC latency stats provider (PCLStats, an ETW provider and a ping thread).
// DIVERGED: the Reflex SDK is not vendored; the provider is not registered.
// ref: FUN_14230d720
void LatencyStatsInit(uint16_t vendor, uint32_t a2) {
    (void)vendor;
    (void)a2;
}

}

// ref: FUN_142310800
RESULT* ReportError(Object* object, RESULT* out, uint8_t severity, const char* function, RESULT result,
    const char* format, ...) {
    va_list args;
    va_start(args, format);
    char text[0x400];
    SStrVPrintf(text, sizeof(text), format, args);  // FUN_140265ea0
    va_end(args);
    // FUN_142310300: through the owner instance's messenger and settings
    auto instance = static_cast<Instance*>(object->m_owner);
    Messenger* messenger = instance->m_messenger;
    auto settings = static_cast<uint8_t*>(instance->GetSettings());
    uint32_t flags = ShouldBreak(settings[0xc], severity) ? 2u : 0u;
    messenger->Report(severity, function, result, flags, text);
    *out = result;
    return out;
}

// ref: FUN_142353760
RESULT ResultFromHResult(int32_t hresult) {
    RESULT result = {};
    uint32_t code = 0x401;
    uint32_t value = static_cast<uint32_t>(hresult);
    if (hresult < static_cast<int32_t>(0x887a0007)) {
        if (value == 0x887a0006 || value == 0x80070005) {
            code = 0x403;
        } else if (value == 0x8007000e) {
            code = 0x402;
            hresult = static_cast<int32_t>(0x8007000e);
        } else if (value == 0x887a0005) {
            code = 0x403;
        }
    } else {
        uint32_t index = value + 0x7785fff9u;
        if (index < 0x25 && ((0x1002000001ull >> (index & 0x3f)) & 1) != 0) {
            code = 0x403;
        } else if (hresult == 0) {
            code = 0x400;
        }
    }
    result.m_code = static_cast<uint8_t>(code & 0xff);
    result.m_category = static_cast<uint8_t>(code >> 8);
    result.m_hresult = hresult;
    return result;
}

// ref: FUN_1423459a0
RESULT ResultFromHResultDx12(int32_t hresult) {
    RESULT result = {};
    uint32_t value = static_cast<uint32_t>(hresult);
    uint32_t code;
    if (hresult < static_cast<int32_t>(0x887a0007)) {
        if (value == 0x887a0006) {
            code = 0x403;
        } else if (value == 0x8007000e) {
            code = 0x402;
            hresult = static_cast<int32_t>(0x8007000e);
        } else if (((value + 0x7785ffffu) & 0xfffffffbu) == 0) {
            code = 0x403;
        } else {
            code = 0x401;
        }
    } else if (value == 0x887a0007 || value == 0x887a0020) {
        code = 0x403;
    } else if (hresult == 0) {
        code = 0x400;
        hresult = 0;
    } else {
        code = 0x401;
    }
    result.m_code = static_cast<uint8_t>(code & 0xff);
    result.m_category = static_cast<uint8_t>(code >> 8);
    result.m_hresult = hresult;
    return result;
}

// ref: FUN_142359190
const char* ApiName(uint8_t api) {
    static const char* const s_names[] = { "Null", "Agc", "Dx11", "Dx12", "Gles", "Gnm", "Metal", "Nvn", "Vulkan" };
    return api < 9 ? s_names[api] : "Unknown";  // PTR_DAT_142d4afe0
}

// ref: FUN_142359210
const char* GpuTypeName(uint8_t type) {
    static const char* const s_names[] = { "Unknown", "Software", "Integrated", "Discrete", "External" };
    return type < 5 ? s_names[type] : "Unknown";  // PTR_s_Unknown_142d4b470
}

// ref: FUN_142359170
const char* AmdFamilyName(uint8_t family) {
    static const char* const s_names[] = { "Unknown", "PreGCN", "GCN1", "GCN2", "GCN3", "GCN4", "Vega", "RDNA",
        "RDNA2" };
    return family < 9 ? s_names[family] : "Unknown";  // PTR_s_Unknown_142d4b4c0
}

// ref: FUN_142359290
const char* NvidiaFamilyName(uint8_t family) {
    static const char* const s_names[] = { "Unknown", "Fermi_Or_Earlier", "Kepler", "Maxwell_Gen1", "Maxwell_Gen2",
        "Pascal", "Volta", "Turing", "Ampere" };
    return family < 9 ? s_names[family] : "Unknown";  // PTR_s_Unknown_142d4b580
}

// ref: FUN_142359250
const char* IntelFamilyName(uint8_t family) {
    static const char* const s_names[] = { "Unknown", "Gen6", "Gen7", "Gen7_5", "Gen8LP", "Gen8", "Gen9LP", "Gen9",
        "Gen9_5", "Gen10", "Gen11LP", "Gen11", "Gen12LP", "Gen12" };
    return family < 0xe ? s_names[family] : "Unknown";  // PTR_s_Unknown_142d4b510
}

// ref: FUN_142359150
const char* ResultCodeName(uint8_t code) {
    static const char* const s_names[] = { "Success", "Invalid", "OutOfMemory", "DeviceLost", "StructuredException" };
    return code < 5 ? s_names[code] : "Unknown";  // PTR_s_Success_142d4ba40
}

// ref: FUN_1423591d0
const char* ResultSourceName(uint8_t category) {
    static const char* const s_names[] = { "Prism", "AGSReturnCode", "bcResult", "GLError", "Hresult", "NSError",
        "NvAPI_Status", "nvn_ErrorResult", "PsrStatus", "PsslStatus", "SceError", "TPError", "VkResult" };
    return category < 0xd ? s_names[category] : "UnknownSource";  // PTR_s_Prism_142d4ba70
}

// The base construction (FUN_14234e0a0) with the platform's (FUN_14234d660).
Instance::Instance(const INSTANCEINFO* info) {
    // FUN_142310620: the root object is its own owner
    this->m_owner = this;
    this->m_allocator = info->m_allocator;
    this->m_messenger = info->m_messenger;
    std::memset(&this->m_info, 0, sizeof(this->m_info));
    this->m_info.m_10 = 2;
    this->m_info.m_28 = 0xf;
    this->m_info.m_a4 = 2;
    this->m_info.m_f0 = 0x800000;
    this->m_info.m_f8 = 1;
    this->m_info.m_10c = 0x20000;
    uint16_t one = 1;
    std::memcpy(reinterpret_cast<uint8_t*>(&this->m_info) + 0xb0, &one, 2);
    for (BACKENDSLOT& slot : this->m_backends) {
        slot.m_state = Ok();
        slot.m_backend = nullptr;
        slot.m_capacity = 8;
        slot.m_buckets = nullptr;
        slot.m_count = 0;
        slot.m_maxLoad = 1.0f;
    }
    this->m_gpuAllocator = this->m_allocator;
    this->m_gpus = nullptr;
    this->m_gpuCount = 0;
    this->m_gpuCapacity = 0;
    this->m_osClass = 1;
    // FUN_142357940: the AMD AGS context. DIVERGED: the AGS SDK is not vendored; the context
    // stays empty, as on a machine where agsInitialize fails.
    std::memset(&this->m_ags, 0, sizeof(this->m_ags));
    this->m_ags.m_allocator = this->m_allocator;
    // FUN_142357d70: NVAPI. DIVERGED: the NVAPI SDK is not vendored (NvAPI_Initialize fails).
    this->m_nvapi = 0;
    DxgiConstruct(&this->m_dxgi, this);
    this->m_osClass = 1;
    typedef LONG(WINAPI * RTLGETVERSION)(RTL_OSVERSIONINFOW*);
    HMODULE ntdll = LoadLibraryW(L"ntdll.dll");
    RTLGETVERSION getVersion = nullptr;
    if (ntdll != nullptr) {
        getVersion = reinterpret_cast<RTLGETVERSION>(GetProcAddress(ntdll, "RtlGetVersion"));
    }
    RTL_OSVERSIONINFOEXW version;
    std::memset(&version, 0, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);
    if (getVersion != nullptr) {
        getVersion(reinterpret_cast<RTL_OSVERSIONINFOW*>(&version));
    } else {
#pragma warning(suppress : 4996)
        GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&version));  // FUN_14234dce0
    }
    if (version.dwPlatformId == 2 && version.dwMajorVersion >= 6) {
        if (version.dwMajorVersion == 6) {
            if (version.dwMinorVersion == 1) {
                this->m_osClass = 2;
            } else if (version.dwMinorVersion == 2) {
                this->m_osClass = 3;
            } else if (version.dwMinorVersion == 3) {
                this->m_osClass = 4;
            } else if (version.dwMinorVersion == 4) {
                this->m_osClass = 5;
            } else {
                this->m_osClass = 6;
            }
        } else if (version.dwMajorVersion == 10 && version.dwMinorVersion == 0) {
            this->m_osClass = 5;
        } else {
            this->m_osClass = 6;
        }
    }
    this->m_osBuild = version.dwBuildNumber;
    if (ntdll != nullptr) {
        FreeLibrary(ntdll);
    }
}

// ref: FUN_14234d800
Instance::~Instance() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14234f080
RESULT* Instance::Init(RESULT* out, const INSTANCEINFO* info) {
    this->m_info = *info;
    RESULT result;
    this->InitPlatform(&result, info);
    if (result.m_code == 0) {
        this->InitGpus(&result, info->m_10, info->m_a4, 1);
        if (result.m_code == 0) {
            *out = Ok();
            return out;
        }
    }
    *out = result;
    return out;
}

// ref: FUN_14234e730
RESULT* Instance::CreateDevice(RESULT* out, const void* params, void* device) {
    (void)params;
    (void)device;
    FROST_UNIMPLEMENTED();
    *out = Ok();
    out->m_code = 2;
    return out;
}

// ref: FUN_14234eb50
void Instance::DestroyDevice(void* device) {
    (void)device;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14234ec90
RESULT* Instance::Rebuild(RESULT* out, uint8_t a2, uint8_t a3) {
    this->ShutdownPlatform();
    RESULT result;
    this->InitGpus(&result, a2, a3, 0);
    if (result.m_code == 0) {
        return this->RebuildDeviceList(out);
    }
    *out = result;
    return out;
}

// ref: FUN_14234db90
// ref: FUN_14234db90 (FUN_142353650 on the DXGI instance)
bool Instance::IsCurrent() {
    DxgiInstance* dxgi = &this->m_dxgi;
    if (dxgi->m_initialized == 0) {
        return false;
    }
    bool current = static_cast<IDXGIFactory1*>(dxgi->m_factory1)->IsCurrent() != FALSE;
    dxgi->m_initialized = current;
    return current;
}

// ref: FUN_14234da50
PLATFORMHANDLES* Instance::GetPlatformHandles(PLATFORMHANDLES* out) {
    std::memset(out, 0, sizeof(*out));
    out->m_dxgiModule = this->m_dxgi.m_dxgiModule;
    out->m_factory = this->m_dxgi.m_factory1;
    // The backends' modules, read where each backend keeps it (pm_dx11 +0xc0, pm_dx12 +0x28).
    if (this->m_backends[API_D3D11].m_backend != nullptr) {
        std::memcpy(&out->m_d3d11Module, reinterpret_cast<uint8_t*>(this->m_backends[API_D3D11].m_backend) + 0xc0, 8);
    }
    if (this->m_backends[API_D3D12].m_backend != nullptr) {
        std::memcpy(&out->m_d3d12Module, reinterpret_cast<uint8_t*>(this->m_backends[API_D3D12].m_backend) + 0x28, 8);
    }
    out->m_agsContext = this->m_ags.m_context;
    out->m_agsGpuInfo = &this->m_ags.m_gpuInfo;
    out->m_nvapi = this->m_nvapi;
    return out;
}

// ref: FUN_14234da00
void* Instance::GetSlot38(void* out, GPU* gpu) {
    auto fields = static_cast<uint64_t*>(out);
    auto bytes = reinterpret_cast<uint8_t*>(gpu);
    std::memcpy(&fields[0], bytes + 0xd8, 4 * 8);
    fields[4] = 0;
    if (gpu->m_vendorId == 0x10de) {
        std::memcpy(&fields[4], bytes + 0x148, 8);
    }
    return out;
}

// ref: FUN_14234ef30
RESULT* Instance::EnumerateGpus(RESULT* out, uint32_t* count, GPU** gpus, uint16_t typeMask) {
    GPU** list = this->m_gpus;
    uint32_t total = this->m_gpuCount;
    if (typeMask == 0x1f) {
        if (gpus == nullptr) {
            *count = total;
            out->m_code = 0;
            out->m_category = 0;
        } else {
            uint32_t want = *count;
            uint32_t n = want < total ? want : total;
            *count = n;
            for (uint32_t i = 0; i < n; i++) {
                gpus[i] = list[i];
            }
            out->m_code = want < total ? 1 : 0;
            out->m_category = 0;
        }
    } else if (gpus == nullptr) {
        *count = 0;
        for (uint32_t i = 0; i < total; i++) {
            if (((typeMask >> (list[i]->m_type & 0xf)) & 1) != 0) {
                (*count)++;
            }
        }
        out->m_code = 0;
        out->m_category = 0;
    } else {
        uint32_t found = 0;
        for (uint32_t i = 0; i < total; i++) {
            if (((typeMask >> (list[i]->m_type & 0xf)) & 1) != 0) {
                if (*count <= found) {
                    *count = found;
                    out->m_code = 1;
                    out->m_category = 0;
                    out->m_hresult = 0;
                    return out;
                }
                gpus[found++] = list[i];
            }
        }
        *count = found;
        out->m_code = 0;
        out->m_category = 0;
    }
    out->m_hresult = 0;
    return out;
}

// ref: FUN_14234d950
RESULT* Instance::EnumerateMonitors(RESULT* out, GPU* gpu, uint32_t* count, void** monitors) {
    if (gpu->m_size != sizeof(GPU)) {
        *count = 0;
        *out = Ok();
        return out;
    }
    uint32_t total = gpu->m_monitorCount;
    bool more = false;
    if (monitors == nullptr) {
        *count = total;
    } else {
        uint32_t want = *count;
        uint32_t n = want < total ? want : total;
        *count = n;
        for (uint32_t i = 0; i < n; i++) {
            monitors[i] = gpu->m_monitors[i];
        }
        more = want < total;
    }
    *out = Ok();
    out->m_code = more ? 1 : 0;
    return out;
}

// ref: FUN_14234d8d0
RESULT* Instance::EnumerateModes(RESULT* out, void* monitor, uint32_t* count, void** modes) {
    auto record = static_cast<MONITOR*>(monitor);
    uint32_t total = record->m_modeCount;
    bool more = false;
    if (modes == nullptr) {
        *count = total;
    } else {
        uint32_t want = *count;
        uint32_t n = want < total ? want : total;
        *count = n;
        for (uint32_t i = 0; i < n; i++) {
            modes[i] = record->m_modes[i];
        }
        more = want < total;
    }
    *out = Ok();
    out->m_code = more ? 1 : 0;
    return out;
}

// ref: FUN_14234ed10
void Instance::Slot58(void* a2, uint8_t api) {
    Backend* backend = this->GetBackend(api);
    if (backend != nullptr) {
        backend->Slot38(a2);
    }
}

// ref: FUN_1406b0410 (shared by the three identity slots)
void* Instance::GetMode(void* mode) {
    return mode;
}

void* Instance::GetMonitor(void* monitor) {
    return monitor;
}

GPU* Instance::GetGpu(GPU* gpu) {
    return gpu;
}

// ref: FUN_14234ee00
APIDEVICEDATA* Instance::GetDeviceData(GPU* gpu, uint8_t api) {
    DEVICEVALUE* value = this->FindDeviceNode(gpu, api);
    if (value == nullptr) {
        value = this->AddDeviceNode(gpu, api);
        Backend* backend = this->GetBackend(api);
        if (backend != nullptr) {
            backend->FillDeviceData(gpu, value);
        }
    }
    return &value->m_data;
}

// ref: FUN_14234dad0
void* Instance::Slot80(void* out, void* monitor) {
    (void)monitor;
    auto fields = static_cast<uint64_t*>(out);
    fields[0] = 0;
    fields[1] = 0;
    FROST_UNIMPLEMENTED();  // FUN_142351a60 on the DXGI instance for a monitor record
    return out;
}

// ref: FUN_140318060
void* Instance::GetSettings() {
    return reinterpret_cast<uint8_t*>(this) + 0x38;
}

// ref: FUN_14234db10
RESULT* Instance::InitPlatform(RESULT* out, const INSTANCEINFO* info) {
    LatencyStatsInit(static_cast<uint16_t>(info->m_a0 & 0xffff), 0);
    RESULT result;
    DxgiInit(&this->m_dxgi, &result, info);
    if (result.m_code != 0) {
        *out = result;
        return out;
    }
    *out = Ok();
    return out;
}

// ref: FUN_14234d8a0
void Instance::ShutdownPlatform() {
    DxgiShutdown(&this->m_dxgi);
    this->m_gpuCount = 0;  // FUN_14234e5f0
}

// ref: FUN_14234dbc0
RESULT* Instance::InitGpus(RESULT* out, uint8_t a2, uint8_t a3, uint8_t a4) {
    (void)a3;
    (void)a4;
    RESULT result;
    DxgiInitGpus(&this->m_dxgi, &result, a2);
    if (result.m_code != 0) {
        *out = result;
        return out;
    }
    // FUN_1402d3f30: reserve, then append the DXGI list
    for (uint32_t i = 0; i < this->m_dxgi.m_gpuCount; i++) {
        if (this->m_gpuCount == this->m_gpuCapacity) {
            uint32_t capacity = this->m_gpuCapacity + (this->m_gpuCapacity >> 1);
            if (capacity < this->m_gpuCount + 1) {
                capacity = this->m_gpuCount + 1;
            }
            auto grown = static_cast<GPU**>(this->m_gpuAllocator->allocate(uint64_t(capacity) * 8, 8));
            if (this->m_gpus != nullptr) {
                std::memcpy(grown, this->m_gpus, uint64_t(this->m_gpuCount) * 8);
                this->m_gpuAllocator->deallocate(this->m_gpus);
            }
            this->m_gpus = grown;
            this->m_gpuCapacity = capacity;
        }
        this->m_gpus[this->m_gpuCount++] = this->m_dxgi.m_gpus[i];
    }
    *out = Ok();  // FUN_142310860
    return out;
}

// ref: FUN_14234dba0
void Instance::ReportOsVersion(void* out) {
    auto bytes = static_cast<uint8_t*>(out);
    std::memcpy(bytes + 0xac, &this->m_osClass, 4);
    std::memcpy(bytes + 0xb0, &this->m_osBuild, 4);
}

// ref: FUN_14234ed50
Backend* Instance::GetBackend(uint8_t api) {
    BACKENDSLOT* slot = &this->m_backends[api];
    if ((api != API_D3D11 && api != API_D3D12) || slot->m_state.m_code != 0) {  // FUN_14190cb10
        return nullptr;
    }
    if (slot->m_backend == nullptr) {
        slot->m_backend = api == API_D3D11 ? CreateDx11Backend(this) : CreateDx12Backend(this);
        RESULT result;
        slot->m_backend->Init(&result, &this->m_info);
        slot->m_state = result;
        if (result.m_code != 0) {
            return nullptr;
        }
    }
    return slot->m_backend;
}

// ref: FUN_14234ee90
DEVICEVALUE* Instance::FindDeviceNode(GPU* gpu, uint8_t api) {
    BACKENDSLOT* slot = &this->m_backends[api];
    if (slot->m_buckets == nullptr) {
        return nullptr;
    }
    uint64_t index = (slot->m_capacity - 1) & blz::fmix64(reinterpret_cast<uint64_t>(gpu));
    for (DEVICENODE* node = slot->m_buckets[index]; node != nullptr; node = node->m_next) {
        if (node->m_key == gpu) {
            return &node->m_value;
        }
    }
    return nullptr;
}

// ref: FUN_14234e930
DEVICEVALUE* Instance::AddDeviceNode(GPU* gpu, uint8_t api) {
    BACKENDSLOT* slot = &this->m_backends[api];
    if (slot->m_buckets == nullptr) {
        RehashDeviceMap(slot, slot->m_capacity);
    }
    uint64_t index = (slot->m_capacity - 1) & blz::fmix64(reinterpret_cast<uint64_t>(gpu));
    DEVICENODE** link = &slot->m_buckets[index];
    DEVICENODE* node = *link;
    for (; node != nullptr && node->m_key != gpu; node = node->m_next) {
    }
    if (node == nullptr) {
        node = static_cast<DEVICENODE*>(SMemAlloc(sizeof(DEVICENODE), 0xB, s_deviceNodeTag, SStrHashFnv1a(s_deviceNodeTag), -6, 0));
        if (node != nullptr) {
            node->m_next = *link;
            node->m_key = gpu;
            std::memset(&node->m_value.m_data, 0, sizeof(node->m_value.m_data));
            node->m_devicesCapacity = 8;
            node->m_devices = nullptr;
            node->m_deviceCount = 0;
            node->m_devicesMaxLoad = 1.0f;
        }
        *link = node;
        slot->m_count++;
        // FUN_14234f950: grow when over the load
        if (slot->m_maxLoad < static_cast<float>(slot->m_count) / static_cast<float>(slot->m_capacity)) {
            RehashDeviceMap(slot, slot->m_capacity * 2);
        }
    }
    node->m_value.m_luid = gpu->m_luid;
    node->m_value.m_data.m_api = api;
    node->m_value.m_gpuType = gpu->m_type;
    std::memcpy(node->m_value.m_data.m_ids, &gpu->m_vendorId, 8);
    node->m_value.m_data.m_revision = gpu->m_revision;
    node->m_value.m_data.m_ready = 0;
    return &node->m_value;
}

// Asks each backend to rebuild its device list, then rekeys the device data by the new GPU
// objects (matched by type and LUID); data whose GPU went away is dropped.
// ref: FUN_14234f180
RESULT* Instance::RebuildDeviceList(RESULT* out) {
    for (uint32_t api = 0; api < API_COUNT; api++) {
        BACKENDSLOT* slot = &this->m_backends[api];
        Backend* backend = slot->m_backend;
        if (backend == nullptr) {
            continue;
        }
        RESULT result;
        backend->BuildDeviceList(&result);
        if (result.m_code != 0) {
            RESULT failed = {};
            failed.m_code = 2;
            return ReportError(this, out, 2, "pm_api::PlatformInstance::RebuildDeviceList", failed,
                "BuildDeviceList failed for %s", ApiName(static_cast<uint8_t>(api)));
        }
        if (slot->m_count == 0) {
            continue;
        }
        BACKENDSLOT rebuilt = {};
        rebuilt.m_capacity = 8;
        rebuilt.m_maxLoad = 1.0f;
        RehashDeviceMap(&rebuilt, static_cast<uint64_t>(static_cast<float>(slot->m_count) + 0.5f));
        for (uint64_t i = 0; slot->m_buckets != nullptr && i < slot->m_capacity; i++) {
            for (DEVICENODE* node = slot->m_buckets[i]; node != nullptr; node = node->m_next) {
                GPU* match = nullptr;
                for (uint32_t g = 0; g < this->m_gpuCount; g++) {
                    GPU* gpu = this->m_gpus[g];
                    if (gpu->m_type == node->m_value.m_gpuType && gpu->m_luid == node->m_value.m_luid) {
                        match = gpu;
                        break;
                    }
                }
                if (node->m_deviceCount != 0) {
                    backend->RebindDevice(&node->m_value, match);
                }
                if (match == nullptr) {
                    continue;
                }
                // Moved over whole, its device set with it (the reference copies the set).
                FROST_UNIMPLEMENTED();
            }
        }
        BACKENDSLOT old = *slot;
        slot->m_capacity = rebuilt.m_capacity;
        slot->m_buckets = rebuilt.m_buckets;
        slot->m_count = rebuilt.m_count;
        slot->m_maxLoad = rebuilt.m_maxLoad;
        FreeDeviceMap(&old);
    }
    *out = Ok();
    return out;
}

// ref: FUN_1423109f0
RESULT CreateInstance(const INSTANCEINFO* info, Instance** instance) {
    INSTANCEINFO copy = *info;
    if (copy.m_allocator == nullptr) {
        copy.m_allocator = blz::default_allocator();  // FUN_142363a50
    }
    if (copy.m_messenger == nullptr) {
        copy.m_messenger = DefaultMessengerGet();
    }
    if (copy.m_engineName == nullptr) {
        copy.m_engineVersion = 1;
        copy.m_engineName = "Prism";
    }
    void* memory = copy.m_allocator->allocate(sizeof(Instance), 8);  // FUN_14234d660
    Instance* created = memory != nullptr ? new (memory) Instance(&copy) : nullptr;
    RESULT result;
    created->Init(&result, &copy);
    if (result.m_code == 0) {
        *instance = created;
        return Ok();
    }
    if (created != nullptr) {
        Allocator* allocator = created->m_allocator;
        created->~Instance();
        allocator->deallocate(created);
    }
    return result;
}

// ref: FUN_142310b30
void DestroyInstance(Instance* instance) {
    if (instance == nullptr) {
        return;
    }
    Allocator* allocator = instance->m_allocator;
    instance->~Instance();
    allocator->deallocate(instance);
}

}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140265ea0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14190cb10=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142310860=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14234d660=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14234dce0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14234e5f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142351a60=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142359270=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142363a50=GenForwardMissing")
// ---- end of link fallbacks ----
