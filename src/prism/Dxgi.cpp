#include "prism/Dxgi.hpp"
#include "prism/Instance.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <setupapi.h>
#include <devpropdef.h>
#include <algorithm>
#include <cstring>
#include <cwchar>

namespace pm {

namespace {

// The vendor extension objects every GPU carries (+0x120 default, +0x128 AMD, +0x138 Intel,
// +0x140 NVIDIA); +0x118 points at the one in use and its slot 0x08 fills the GPU.
struct GPUEXTENSION {
    void* m_vtable;
    void* m_context;
};

const GUID s_umdVersionIid = { 0x9b7e4c0f, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };  // 0x142915688: ID3D10Device
const GUID s_displayClass = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };  // 0x142924450
const DEVPROPKEY s_adapterLuid = { { 0x60b193cb, 0x5276, 0x4d0f, { 0x96, 0xfc, 0xf1, 0x73, 0xab, 0xad, 0x3e, 0xc6 } }, 2 };
const DEVPROPKEY s_matchingDeviceId = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 8 };
const DEVPROPKEY s_providerName = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 9 };
const DEVPROPKEY s_busNumber = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 0x17 };
const DEVPROPKEY s_removalPolicy = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 0x21 };

// DXGI_ENUM_MODES_SCALING over the 8-bit and 10-bit formats.
const UINT s_modeFlags = DXGI_ENUM_MODES_SCALING;

// DXGI_GPU_PREFERENCE by the instance's preference (DAT_142924798).
const DXGI_GPU_PREFERENCE s_gpuPreference[] = { DXGI_GPU_PREFERENCE_UNSPECIFIED, DXGI_GPU_PREFERENCE_MINIMUM_POWER,
    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE };

typedef HRESULT(WINAPI* CREATEFACTORY1)(REFIID, void**);
typedef HRESULT(WINAPI* CREATEFACTORY2)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* GETDEBUGINTERFACE1)(UINT, REFIID, void**);
typedef HRESULT(WINAPI* DECLAREREMOVAL)();
typedef HDEVINFO(WINAPI* GETCLASSDEVS)(const GUID*, PCWSTR, HWND, DWORD);
typedef BOOL(WINAPI* DESTROYINFOLIST)(HDEVINFO);
typedef BOOL(WINAPI* ENUMDEVICEINFO)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL(WINAPI* GETDEVICEPROPERTY)(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY*, DEVPROPTYPE*, PBYTE, DWORD,
    PDWORD, DWORD);

// A DXGI error as a reported result (the inline copy in Init/InitGpus of FUN_142353760).
RESULT* Fail(DxgiInstance* dxgi, RESULT* out, const char* function, int32_t hresult, const char* format, const char* arg) {
    return ReportError(reinterpret_cast<Object*>(dxgi), out, 2, function, ResultFromHResult(hresult), format, arg);
}

// The pmFormat a DXGI format maps to (the switch in FUN_142353a10).
uint8_t PrismFormat(uint32_t format) {
    switch (format) {
        case 2: return 0x38;
        case 3: return 0x36;
        case 4: return 0x37;
        case 6: return 0x35;
        case 7: return 0x33;
        case 8: return 0x34;
        case 10: return 0x32;
        case 0xb: return 0x2e;
        case 0xc: return 0x30;
        case 0xd: return 0x2f;
        case 0xe: return 0x31;
        case 0x10: return 0x2d;
        case 0x11: return 0x2b;
        case 0x12: return 0x2c;
        case 0x14: return 0x87;
        case 0x18: return 0x26;
        case 0x19: return 0x27;
        case 0x1a: return 0x29;
        case 0x1c: return 0x1f;
        case 0x1d: return 0x20;
        case 0x1e: return 0x22;
        case 0x1f: return 0x21;
        case 0x20: return 0x23;
        case 0x22: return 0x1e;
        case 0x23: return 0x1a;
        case 0x24: return 0x1c;
        case 0x25: return 0x1b;
        case 0x26: return 0x1d;
        case 0x28: return 0x84;
        case 0x29: return 0x19;
        case 0x2a: return 0x17;
        case 0x2b: return 0x18;
        case 0x2d: return 0x86;
        case 0x31: return 0xc;
        case 0x32: return 0xf;
        case 0x33: return 0xe;
        case 0x34: return 0x10;
        case 0x36: return 0xb;
        case 0x37: return 0x83;
        case 0x38: return 7;
        case 0x39: return 9;
        case 0x3a: return 8;
        case 0x3b: return 10;
        case 0x3d: return 1;
        case 0x3e: return 4;
        case 0x3f: return 3;
        case 0x40: return 5;
        case 0x41: return 6;
        case 0x43: return 0x2a;
        case 0x47: return 0x39;
        case 0x48: return 0x3a;
        case 0x4a: return 0x3b;
        case 0x4b: return 0x3c;
        case 0x4d: return 0x3d;
        case 0x4e: return 0x3e;
        case 0x50: return 0x3f;
        case 0x51: return 0x40;
        case 0x53: return 0x41;
        case 0x54: return 0x42;
        case 0x55: return 0x14;
        case 0x56: return 0x16;
        case 0x57: return 0x24;
        case 0x5b: return 0x25;
        case 0x5f: return 0x44;
        case 0x60: return 0x43;
        case 0x62: return 0x45;
        case 99: return 0x46;
        case 0x73: return 0x12;
        default: return 0;
    }
}

// The order display modes are kept in.
// ref: 0x1423536b0
bool ModeLess(const DXGI_MODE_DESC& a, const DXGI_MODE_DESC& b) {
    static const uint32_t s_scalingOrder[] = { 1, 2, 0 };  // DAT_142d43cc0
    if (static_cast<int32_t>(a.Format) != static_cast<int32_t>(b.Format)) {
        return static_cast<int32_t>(a.Format) < static_cast<int32_t>(b.Format);
    }
    if (a.Width != b.Width) {
        return a.Width < b.Width;
    }
    if (a.Height != b.Height) {
        return a.Height < b.Height;
    }
    float rateA = static_cast<float>(a.RefreshRate.Numerator);
    if (a.RefreshRate.Denominator != 0) {
        rateA /= static_cast<float>(a.RefreshRate.Denominator);
    }
    float rateB = static_cast<float>(b.RefreshRate.Numerator);
    if (b.RefreshRate.Denominator != 0) {
        rateB /= static_cast<float>(b.RefreshRate.Denominator);
    }
    if (rateA != rateB) {
        return rateA < rateB;
    }
    if (static_cast<int32_t>(a.Scaling) < 3 && static_cast<int32_t>(b.Scaling) < 3) {
        uint32_t orderA = s_scalingOrder[a.Scaling];
        uint32_t orderB = s_scalingOrder[b.Scaling];
        if (orderA != orderB) {
            return orderA < orderB;
        }
    }
    return static_cast<int32_t>(a.ScanlineOrdering) < static_cast<int32_t>(b.ScanlineOrdering);
}

// The order GPUs are listed in: by the preference table for the GPU's type, then the one with
// more video memory, then by index.
// ref: FUN_142310700
bool GpuBefore(Instance* instance, GPU* a, GPU* b, uint8_t preference) {
    static const uint32_t s_powerOrder[] = { 4, 5, 1, 2, 3, 0 };    // DAT_14291e280
    static const uint32_t s_performanceOrder[] = { 4, 5, 3, 2, 1 };  // DAT_14291e298
    GPU* gpuA = instance->GetGpu(a);
    GPU* gpuB = instance->GetGpu(b);
    if (preference == 1 || preference == 2) {
        const uint32_t* order = preference == 1 ? s_powerOrder : s_performanceOrder;
        if (order[gpuA->m_type] != order[gpuB->m_type]) {
            return order[gpuA->m_type] < order[gpuB->m_type];
        }
    }
    // The reference counts the first GPU's monitors twice, so this step never decides.
    RESULT result;
    uint32_t monitorsA = 0;
    uint32_t monitorsB = 0;
    instance->EnumerateMonitors(&result, a, &monitorsA, nullptr);
    instance->EnumerateMonitors(&result, a, &monitorsB, nullptr);
    if (monitorsB < monitorsA) {
        return true;
    }
    if (monitorsB > monitorsA) {
        return false;
    }
    uint64_t memoryA = 0;
    uint64_t memoryB = 0;
    std::memcpy(&memoryA, reinterpret_cast<uint8_t*>(gpuA) + 0x20, 8);
    std::memcpy(&memoryB, reinterpret_cast<uint8_t*>(gpuB) + 0x20, 8);
    if (memoryB < memoryA) {
        return true;
    }
    if (memoryB > memoryA) {
        return false;
    }
    return gpuA->m_index < gpuB->m_index;
}

// FUN_1402d3f30's growth for the arrays of pointers the GPU and monitor records keep.
template <class T>
void PushPointer(Allocator* allocator, T*** data, uint32_t* count, uint32_t* capacity, T* value) {
    if (*count == *capacity) {
        uint32_t grown = *capacity + (*capacity >> 1);
        if (grown < *count + 1) {
            grown = *count + 1;
        }
        auto block = static_cast<T**>(allocator->allocate(uint64_t(grown) * 8, 8));
        if (*data != nullptr) {
            std::memcpy(block, *data, uint64_t(*count) * 8);
            allocator->deallocate(*data);
        }
        *data = block;
        *capacity = grown;
    }
    (*data)[(*count)++] = value;
}

// Intel's extension: integrated or discrete from the dedicated memory and from D3D11's
// unified memory flag.
// ref: FUN_142357e40
void IntelFillGpu(GPU* gpu) {
    auto adapter = static_cast<IDXGIAdapter1*>(gpu->m_adapter1);
    uint8_t type = 2;
    DXGI_ADAPTER_DESC1 desc;
    std::memset(&desc, 0, sizeof(desc));
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.DedicatedVideoMemory > 0x20000000) {
        type = 3;
    }
    ID3D11Device* device = nullptr;
    ID3D11Device3* device3 = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (d3d11 != nullptr) {
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
        if (create != nullptr) {
            HRESULT hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, 7, &device, nullptr, &context);
            if (SUCCEEDED(hr)) {
                if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&device3)))) {
                    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options;
                    std::memset(&options, 0, sizeof(options));
                    if (SUCCEEDED(device3->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &options, sizeof(options)))
                        && !options.UnifiedMemoryArchitecture) {
                        type = 3;
                    }
                }
                device->Release();
                if (device3 != nullptr) {
                    device3->Release();
                }
            }
            if (context != nullptr) {
                context->Release();
            }
        }
        FreeLibrary(d3d11);
    }
    gpu->m_type = type;
}

void FillWithExtension(GPU* gpu) {
    auto bytes = reinterpret_cast<uint8_t*>(gpu);
    auto extension = *reinterpret_cast<uint8_t**>(bytes + 0x118);
    if (extension == bytes + 0x138) {
        IntelFillGpu(gpu);
    } else if (extension == bytes + 0x128) {
        FROST_UNIMPLEMENTED();  // AMD (AGS gpu info, 0x142357ce0): AGS is not vendored
    } else if (extension == bytes + 0x140) {
        FROST_UNIMPLEMENTED();  // NVIDIA (NVAPI, FUN_1423581c0): NVAPI is not vendored
    }
    // the default extension's slot 0x08 does nothing
}

}

// ref: FUN_142350c20
void DxgiConstruct(DxgiInstance* dxgi, Instance* instance) {
    std::memset(dxgi, 0, sizeof(*dxgi));
    dxgi->m_owner = instance->m_owner;  // FUN_142310460
    dxgi->m_allocator = instance->m_allocator;
    dxgi->m_messenger = instance->m_messenger;
    dxgi->m_instance = instance;
    dxgi->m_gpuAllocator = instance->m_allocator;
}

// ref: FUN_142352080
RESULT* DxgiInit(DxgiInstance* dxgi, RESULT* out, const INSTANCEINFO* info) {
    dxgi->m_dxgiModule = LoadLibraryW(L"dxgi.dll");
    if (dxgi->m_dxgiModule == nullptr) {
        return Fail(dxgi, out, "pm_dxgi::Instance::Init", static_cast<int32_t>(GetLastError()), "LoadLibrary(%s) Failed",
            "dxgi.dll");
    }
    auto module = static_cast<HMODULE>(dxgi->m_dxgiModule);
    dxgi->m_createFactory1 = reinterpret_cast<void*>(GetProcAddress(module, "CreateDXGIFactory1"));
    if (dxgi->m_createFactory1 == nullptr) {
        return Fail(dxgi, out, "pm_dxgi::Instance::Init", static_cast<int32_t>(GetLastError()), "GetProcAddress(%s) Failed",
            "CreateDXGIFactory1");
    }
    dxgi->m_createFactory2 = reinterpret_cast<void*>(GetProcAddress(module, "CreateDXGIFactory2"));
    dxgi->m_getDebugInterface1 = reinterpret_cast<void*>(GetProcAddress(module, "DXGIGetDebugInterface1"));
    dxgi->m_declareAdapterRemovalSupport = reinterpret_cast<void*>(GetProcAddress(module, "DXGIDeclareAdapterRemovalSupport"));
    if (dxgi->m_declareAdapterRemovalSupport != nullptr) {
        reinterpret_cast<DECLAREREMOVAL>(dxgi->m_declareAdapterRemovalSupport)();
    }
    dxgi->m_setupApi = LoadLibraryW(L"SetupAPI.dll");
    if (dxgi->m_setupApi != nullptr) {
        auto setupApi = static_cast<HMODULE>(dxgi->m_setupApi);
        dxgi->m_setupDiGetClassDevsW = reinterpret_cast<void*>(GetProcAddress(setupApi, "SetupDiGetClassDevsW"));
        dxgi->m_setupDiDestroyDeviceInfoList = reinterpret_cast<void*>(GetProcAddress(setupApi, "SetupDiDestroyDeviceInfoList"));
        dxgi->m_setupDiEnumDeviceInfo = reinterpret_cast<void*>(GetProcAddress(setupApi, "SetupDiEnumDeviceInfo"));
        dxgi->m_setupDiGetDevicePropertyW = reinterpret_cast<void*>(GetProcAddress(setupApi, "SetupDiGetDevicePropertyW"));
        if (dxgi->m_setupDiGetClassDevsW == nullptr || dxgi->m_setupDiDestroyDeviceInfoList == nullptr
            || dxgi->m_setupDiEnumDeviceInfo == nullptr || dxgi->m_setupDiGetDevicePropertyW == nullptr) {
            FreeLibrary(setupApi);
            dxgi->m_setupApi = nullptr;
        }
    }
    dxgi->m_presentExceptionHandler = info->m_presentExceptionHandler;
    DxgiSetupInfoQueue(dxgi);
    *out = Ok();
    return out;
}

// With CPU validation, DXGI's info queue breaks on errors (and on warnings and messages as the
// break level rises).
// ref: FUN_142351110
void DxgiSetupInfoQueue(DxgiInstance* dxgi) {
    auto settings = static_cast<uint8_t*>(dxgi->m_instance->GetSettings());
    uint32_t cpuValidation;
    std::memcpy(&cpuValidation, settings + 4, 4);
    if (cpuValidation == 0 || dxgi->m_getDebugInterface1 == nullptr) {
        return;
    }
    FROST_UNIMPLEMENTED();  // the IDXGIInfoQueue filter and break setup
}

// ref: FUN_142352390
RESULT* DxgiInitGpus(DxgiInstance* dxgi, RESULT* out, uint8_t preference) {
    if (dxgi->m_createFactory2 == nullptr) {
        HRESULT hr = reinterpret_cast<CREATEFACTORY1>(dxgi->m_createFactory1)(__uuidof(IDXGIFactory1), &dxgi->m_factory1);
        if (FAILED(hr)) {
            return Fail(dxgi, out, "pm_dxgi::Instance::InitGpus", hr, "%s", "CreateDXGIFactory1 Failed");
        }
        static_cast<IUnknown*>(dxgi->m_factory1)->QueryInterface(__uuidof(IDXGIFactory2), &dxgi->m_factory2);
    } else {
        auto settings = static_cast<uint8_t*>(dxgi->m_instance->GetSettings());
        dxgi->m_78 = 0;
        dxgi->m_factoryFlags = 0;
        uint32_t cpuValidation;
        std::memcpy(&cpuValidation, settings + 4, 4);
        if (dxgi->m_getDebugInterface1 != nullptr && cpuValidation != 0) {
            IUnknown* debug = nullptr;
            HRESULT hr = reinterpret_cast<GETDEBUGINTERFACE1>(dxgi->m_getDebugInterface1)(0, __uuidof(IDXGIDebug1),
                reinterpret_cast<void**>(&debug));
            if (FAILED(hr)) {
                RESULT ignored;
                ReportError(reinterpret_cast<Object*>(dxgi), &ignored, 1, "pm_dxgi::Instance::InitGpus", ResultFromHResult(hr),
                    "%s", "CPU validation requested but the DXGI Debug Interface is unavailable");
            } else {
                dxgi->m_factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
                debug->Release();
            }
        }
        HRESULT hr = reinterpret_cast<CREATEFACTORY2>(dxgi->m_createFactory2)(dxgi->m_factoryFlags, __uuidof(IDXGIFactory2),
            &dxgi->m_factory2);
        if (FAILED(hr)) {
            return Fail(dxgi, out, "pm_dxgi::Instance::InitGpus", hr, "%s", "CreateDXGIFactory2 Failed");
        }
        dxgi->m_factory1 = dxgi->m_factory2;
        static_cast<IUnknown*>(dxgi->m_factory2)->AddRef();
    }
    auto factory = static_cast<IUnknown*>(dxgi->m_factory1);
    factory->QueryInterface(__uuidof(IDXGIFactory3), &dxgi->m_factory3);
    factory->QueryInterface(__uuidof(IDXGIFactory4), &dxgi->m_factory4);
    factory->QueryInterface(__uuidof(IDXGIFactory5), &dxgi->m_factory5);
    factory->QueryInterface(__uuidof(IDXGIFactory6), &dxgi->m_factory6);
    if (dxgi->m_factory5 != nullptr) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(static_cast<IDXGIFactory5*>(dxgi->m_factory5)->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
            dxgi->m_allowTearing = tearing != FALSE;
        }
    }

    // The adapters, by GPU preference when the factory can order them.
    auto addGpu = [&](IDXGIAdapter1* adapter) {
        LARGE_INTEGER driverVersion = {};
        adapter->CheckInterfaceSupport(s_umdVersionIid, &driverVersion);
        Instance* instance = dxgi->m_instance;
        auto gpu = static_cast<GPU*>(dxgi->m_allocator->allocate(sizeof(GPU), 8));
        if (gpu != nullptr) {
            std::memset(gpu, 0, sizeof(GPU));
            gpu->m_allocator = instance->m_allocator;
            // The extension objects (vtables 0x1429244e8 default, 0x1429267c8 AMD over the AGS
            // context, 0x142924508 Intel, 0x142924528 NVIDIA) are told apart by where +0x118
            // points; only the AMD one carries data.
            auto bytes = reinterpret_cast<uint8_t*>(gpu);
            reinterpret_cast<GPUEXTENSION*>(bytes + 0x128)->m_context = &instance->m_ags;
        }
        PushPointer(dxgi->m_gpuAllocator, &dxgi->m_gpus, &dxgi->m_gpuCount, &dxgi->m_gpuCapacity, gpu);
        gpu->m_size = sizeof(GPU);
        gpu->m_index = dxgi->m_gpuCount - 1;
        gpu->m_adapter1 = adapter;
        gpu->m_driverVersion = static_cast<uint64_t>(driverVersion.QuadPart);
        DxgiFillGpu(dxgi, gpu);
    };
    if (dxgi->m_factory6 == nullptr) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; static_cast<IDXGIFactory1*>(dxgi->m_factory1)->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            addGpu(adapter);
        }
        // FUN_14234feb0: introsort by GpuBefore
        Instance* instance = dxgi->m_instance;
        std::sort(dxgi->m_gpus, dxgi->m_gpus + dxgi->m_gpuCount,
            [&](GPU* a, GPU* b) { return GpuBefore(instance, a, b, preference); });
    } else {
        auto factory6 = static_cast<IDXGIFactory6*>(dxgi->m_factory6);
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory6->EnumAdapterByGpuPreference(i, s_gpuPreference[preference], __uuidof(IDXGIAdapter1),
                             reinterpret_cast<void**>(&adapter)) != DXGI_ERROR_NOT_FOUND;
             i++) {
            addGpu(adapter);
        }
    }
    for (uint32_t i = 0; i < dxgi->m_gpuCount; i++) {
        GPU* gpu = dxgi->m_gpus[i];
        gpu->m_name = gpu->m_nameBuffer;
        for (uint32_t m = 0; m < gpu->m_monitorCount; m++) {
            auto monitor = static_cast<MONITOR*>(gpu->m_monitors[m]);
            monitor->m_name = monitor->m_nameBuffer;
        }
    }

    // SetupAPI's view of the display devices: the bus number, the type when DXGI did not say,
    // and AMD graphics behind an Intel device id.
    if (dxgi->m_setupApi != nullptr) {
        auto getClassDevs = reinterpret_cast<GETCLASSDEVS>(dxgi->m_setupDiGetClassDevsW);
        auto destroyList = reinterpret_cast<DESTROYINFOLIST>(dxgi->m_setupDiDestroyDeviceInfoList);
        auto enumInfo = reinterpret_cast<ENUMDEVICEINFO>(dxgi->m_setupDiEnumDeviceInfo);
        auto getProperty = reinterpret_cast<GETDEVICEPROPERTY>(dxgi->m_setupDiGetDevicePropertyW);
        HDEVINFO devices = getClassDevs(&s_displayClass, nullptr, nullptr, DIGCF_PRESENT);
        SP_DEVINFO_DATA info;
        std::memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        for (DWORD index = 0; enumInfo(devices, index, &info); index++) {
            DEVPROPTYPE type;
            DWORD size;
            uint64_t luid = 0;
            if (!getProperty(devices, &info, &s_adapterLuid, &type, reinterpret_cast<PBYTE>(&luid), 8, &size, 0)
                || type != DEVPROP_TYPE_UINT64) {
                luid = 0;
            }
            uint16_t vendor = 0;
            uint16_t deviceId = 0;
            wchar_t ids[0x40];
            if (getProperty(devices, &info, &s_matchingDeviceId, &type, reinterpret_cast<PBYTE>(ids), 0x80, &size, 0)
                && type == DEVPROP_TYPE_STRING) {
                wchar_t* end = ids + std::wcslen(ids);
                auto parse = [&](const wchar_t* key) -> uint16_t {
                    wchar_t* found = std::wcsstr(ids, key);
                    if (found == nullptr || found + 8 > end) {
                        return 0;
                    }
                    wchar_t* stop;
                    return static_cast<uint16_t>(std::wcstol(found + 4, &stop, 16));
                };
                vendor = parse(L"ven_");
                deviceId = parse(L"dev_");
                if (vendor == 0) {
                    vendor = parse(L"VEN_");
                }
                if (deviceId == 0) {
                    deviceId = parse(L"DEV_");
                }
            }
            bool amdGraphics = false;
            if (deviceId != 0 && vendor == 0x8086) {
                wchar_t provider[0x100];
                if (getProperty(devices, &info, &s_providerName, &type, reinterpret_cast<PBYTE>(provider), 0x200, &size, 0)
                    && type == DEVPROP_TYPE_STRING) {
                    amdGraphics = std::wcscmp(provider, L"Advanced Micro Devices, Inc.") == 0
                        || std::wcscmp(provider, L"ATI Technologies Inc.") == 0;
                }
            }
            for (uint32_t i = 0; i < dxgi->m_gpuCount; i++) {
                GPU* gpu = dxgi->m_gpus[i];
                bool match;
                if (luid != 0) {
                    // A LUID match whose ids disagree is a GPU DXGI cannot type (without NVAPI).
                    if (gpu->m_luid == luid && (gpu->m_deviceId != deviceId || gpu->m_vendorId != vendor)) {
                        gpu->m_type = 0;
                    }
                    match = gpu->m_luid == luid;
                } else {
                    match = gpu->m_deviceId == deviceId && gpu->m_vendorId == vendor;
                }
                if (!match) {
                    continue;
                }
                uint32_t value = 0;
                if (getProperty(devices, &info, &s_busNumber, &type, reinterpret_cast<PBYTE>(&value), 4, &size, 0)
                    && type == DEVPROP_TYPE_UINT32) {
                    gpu->m_location = value;
                    if (gpu->m_type == 0) {
                        gpu->m_type = (value != 0) + 2;
                    }
                }
                uint32_t policy = 0;
                if (getProperty(devices, &info, &s_removalPolicy, &type, reinterpret_cast<PBYTE>(&policy), 4, &size, 0)
                    && type == DEVPROP_TYPE_UINT32 && policy == 3) {
                    gpu->m_type = 4;
                }
                if (amdGraphics) {
                    gpu->m_flags |= 1;
                    uint64_t length = strnlen(gpu->m_nameBuffer, 0x80);
                    if (length < 0x80) {
                        uint64_t room = 0x7f - length < 0xf ? 0x7f - length : 0xf;
                        std::memcpy(gpu->m_nameBuffer + length, " / AMD Graphics", room);
                        gpu->m_nameBuffer[length + room] = '\0';
                    }
                }
                break;
            }
        }
        destroyList(devices);
    }
    for (uint32_t i = 0; i < dxgi->m_gpuCount; i++) {
        GPU* gpu = dxgi->m_gpus[i];
        if (gpu->m_type == 0 && gpu->m_vendorId == 0x1414) {
            gpu->m_type = 1;
        }
    }
    dxgi->m_initialized = 1;
    *out = Ok();
    return out;
}

// ref: FUN_142353a10
void DxgiFillGpu(DxgiInstance* dxgi, GPU* gpu) {
    auto adapter = static_cast<IDXGIAdapter1*>(gpu->m_adapter1);
    adapter->QueryInterface(__uuidof(IDXGIAdapter2), &gpu->m_e0[0]);
    adapter->QueryInterface(__uuidof(IDXGIAdapter3), &gpu->m_e0[1]);
    adapter->QueryInterface(__uuidof(IDXGIAdapter4), &gpu->m_e0[2]);
    DXGI_ADAPTER_DESC2 desc;
    std::memset(&desc, 0, sizeof(desc));
    if (gpu->m_e0[0] == nullptr) {
        DXGI_ADAPTER_DESC1 desc1;
        adapter->GetDesc1(&desc1);
        std::memcpy(&desc, &desc1, sizeof(desc1));
    } else {
        static_cast<IDXGIAdapter2*>(gpu->m_e0[0])->GetDesc2(&desc);
    }
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu->m_nameBuffer, 0x80, nullptr, nullptr);
    auto bytes = reinterpret_cast<uint8_t*>(gpu);
    gpu->m_deviceId = static_cast<uint16_t>(desc.DeviceId);
    std::memcpy(bytes + 0x14, &desc.SubSysId, 4);
    gpu->m_revision = static_cast<uint8_t>(desc.Revision);
    std::memcpy(&gpu->m_luid, &desc.AdapterLuid, 8);
    gpu->m_vendorId = static_cast<uint16_t>(desc.VendorId);
    gpu->m_virtualization = 0;
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
        gpu->m_type = 1;
    }
    uint8_t* extension = nullptr;
    if (gpu->m_vendorId == 0x1002) {
        extension = bytes + 0x128;
    } else if (gpu->m_vendorId == 0x10de) {
        if (dxgi->m_instance->m_nvapi != 0) {
            extension = bytes + 0x140;
        }
    } else if (gpu->m_vendorId == 0x15ad) {
        gpu->m_type = 2;
        gpu->m_virtualization = 3;
    } else if (gpu->m_vendorId == 0x8086) {
        extension = bytes + 0x138;
    }
    if (extension != nullptr) {
        *reinterpret_cast<uint8_t**>(bytes + 0x118) = extension;
    }
    if (*reinterpret_cast<uint8_t**>(bytes + 0x118) == nullptr) {
        *reinterpret_cast<uint8_t**>(bytes + 0x118) = bytes + 0x120;
    }
    FillWithExtension(gpu);
    uint64_t dedicatedVideo = desc.DedicatedVideoMemory;
    uint64_t dedicatedSystem = desc.DedicatedSystemMemory;
    uint64_t shared = desc.SharedSystemMemory;
    if (gpu->m_type == 2) {
        std::memcpy(bytes + 0x20, &shared, 8);
        std::memset(bytes + 0x28, 0, 8);
    } else {
        std::memcpy(bytes + 0x20, &dedicatedVideo, 8);
        std::memcpy(bytes + 0x28, &shared, 8);
    }
    (void)dedicatedSystem;
    uint32_t one = 1;
    std::memcpy(bytes + 0x40, &one, 4);
    std::memcpy(bytes + 0x44, &one, 4);
    std::memcpy(bytes + 0x48, &one, 4);

    // The outputs: a monitor record each, with its display modes.
    IDXGIOutput* output = nullptr;
    UINT index = 0;
    HRESULT hr = adapter->EnumOutputs(index, &output);
    while (hr != DXGI_ERROR_NOT_FOUND && hr != static_cast<HRESULT>(0x887a0022)) {
        if (output != nullptr) {
            DXGI_OUTPUT_DESC outputDesc;
            output->GetDesc(&outputDesc);
            DISPLAY_DEVICEW display;
            std::memset(&display, 0, sizeof(display));
            display.cb = sizeof(display);
            EnumDisplayDevicesW(outputDesc.DeviceName, 0, &display, 0);
            Allocator* allocator = dxgi->m_allocator;
            auto monitor = static_cast<MONITOR*>(allocator->allocate(sizeof(MONITOR), 8));
            if (monitor != nullptr) {
                std::memset(monitor, 0, 0x28);
                monitor->m_modeAllocator = allocator;
                monitor->m_modes = nullptr;
                monitor->m_modeCount = 0;
                monitor->m_modeCapacity = 0;
            }
            PushPointer(gpu->m_allocator, reinterpret_cast<MONITOR***>(&gpu->m_monitors), &gpu->m_monitorCount,
                &gpu->m_monitorCapacity, monitor);
            WideCharToMultiByte(CP_UTF8, 0, display.DeviceString, -1, monitor->m_nameBuffer, 0x80, nullptr, nullptr);
            monitor->m_left = outputDesc.DesktopCoordinates.left;
            monitor->m_top = outputDesc.DesktopCoordinates.top;
            monitor->m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
            monitor->m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
            monitor->m_monitor = outputDesc.Monitor;
            bool hdr = false;
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6)))) {
                DXGI_OUTPUT_DESC1 desc1;
                if (SUCCEEDED(output6->GetDesc1(&desc1))
                    && (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                        || desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)) {
                    hdr = true;
                }
                output6->Release();
            }
            if (GetSystemMetrics(SM_REMOTESESSION) == 0) {
                UINT count8 = 0;
                output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, s_modeFlags, &count8, nullptr);
                UINT count10 = 0;
                if (hdr) {
                    output->GetDisplayModeList(DXGI_FORMAT_R10G10B10A2_UNORM, s_modeFlags, &count10, nullptr);
                }
                uint32_t total = count8 + count10;
                DXGI_MODE_DESC* modes = nullptr;
                if (total != 0) {
                    uint64_t capacity = total < 8 ? 8 : total;
                    uint64_t allocated;
                    modes = static_cast<DXGI_MODE_DESC*>(dxgi->m_allocator->allocate_sized(&allocated,
                        capacity * sizeof(DXGI_MODE_DESC), 4));
                    dxgi->m_allocator->deallocate(nullptr);
                    std::memset(modes, 0, uint64_t(total) * sizeof(DXGI_MODE_DESC));
                }
                output->GetDisplayModeList(DXGI_FORMAT_R8G8B8A8_UNORM, s_modeFlags, &count8, modes);
                if (count10 != 0) {
                    output->GetDisplayModeList(DXGI_FORMAT_R10G10B10A2_UNORM, s_modeFlags, &count10, modes + count8);
                    monitor->m_flags |= 2;
                }
                // FUN_142350330: introsort by ModeLess, then the duplicates dropped
                std::sort(modes, modes + total, ModeLess);
                uint32_t kept = total;
                if (total != 0) {
                    uint32_t last = 0;
                    for (uint32_t i = 1; i < total; i++) {
                        const DXGI_MODE_DESC& a = modes[last];
                        const DXGI_MODE_DESC& b = modes[i];
                        if (a.Width != b.Width || a.Height != b.Height || a.RefreshRate.Numerator != b.RefreshRate.Numerator
                            || a.RefreshRate.Denominator != b.RefreshRate.Denominator || a.Format != b.Format
                            || a.ScanlineOrdering != b.ScanlineOrdering) {
                            last++;
                            if (last != i) {
                                modes[last] = modes[i];
                            }
                        }
                    }
                    kept = last + 1;
                }
                for (uint32_t i = 0; i < kept; i++) {
                    auto mode = static_cast<MODE*>(dxgi->m_allocator->allocate(sizeof(MODE), 4));
                    if (mode != nullptr) {
                        std::memset(mode, 0, sizeof(MODE));
                    }
                    PushPointer(monitor->m_modeAllocator, &monitor->m_modes, &monitor->m_modeCount, &monitor->m_modeCapacity,
                        mode);
                    std::memcpy(mode->m_desc, &modes[i], sizeof(DXGI_MODE_DESC));
                    mode->m_width = modes[i].Width;
                    mode->m_height = modes[i].Height;
                    mode->m_refreshNumerator = modes[i].RefreshRate.Numerator;
                    mode->m_refreshDenominator = modes[i].RefreshRate.Denominator != 0 ? modes[i].RefreshRate.Denominator : 1;
                    mode->m_format = PrismFormat(modes[i].Format);
                }
                if (modes != nullptr) {
                    dxgi->m_allocator->deallocate(modes);
                }
            }
            output->Release();
        }
        index++;
        hr = adapter->EnumOutputs(index, &output);
    }
}

// The video memory a GPU offers (local and non-local, budget and reservation),
// from the adapter's description unless DXGI reported it.
// ref: FUN_142351980
void DxgiQueryVideoMemory(DxgiInstance* dxgi, uint64_t out[6], IDXGIAdapter1* adapter, const DXGI_QUERY_VIDEO_MEMORY_INFO* local,
    const DXGI_QUERY_VIDEO_MEMORY_INFO* nonLocal, bool integrated) {
    (void)dxgi;
    std::memset(out, 0, 6 * 8);
    DXGI_ADAPTER_DESC1 desc;
    adapter->GetDesc1(&desc);
    uint64_t dedicated = desc.DedicatedVideoMemory;
    uint64_t shared = desc.SharedSystemMemory;
    if (integrated) {
        dedicated = shared;
        shared = 0;
    }
    out[3] = shared;
    out[0] = dedicated;
    if (local->Budget == 0) {
        out[1] = dedicated * 9 / 10;
    } else {
        out[1] = local->Budget;
        out[2] = local->CurrentUsage;
    }
    if (nonLocal->Budget == 0) {
        out[4] = shared * 9 / 10;
    } else {
        out[4] = nonLocal->Budget;
        out[5] = nonLocal->CurrentUsage;
    }
}

// ref: FUN_142350e40
void DxgiShutdown(DxgiInstance* dxgi) {
    for (uint32_t i = 0; i < dxgi->m_gpuCount; i++) {
        GPU* gpu = dxgi->m_gpus[i];
        if (gpu->m_adapter1 != nullptr) {
            static_cast<IUnknown*>(gpu->m_adapter1)->Release();
            gpu->m_adapter1 = nullptr;
        }
        for (int32_t k = 0; k < 3; k++) {
            if (gpu->m_e0[k] != nullptr) {
                static_cast<IUnknown*>(gpu->m_e0[k])->Release();
                gpu->m_e0[k] = nullptr;
            }
        }
        for (uint32_t m = 0; m < gpu->m_monitorCount; m++) {
            auto monitor = static_cast<MONITOR*>(gpu->m_monitors[m]);
            for (uint32_t k = 0; k < monitor->m_modeCount; k++) {
                dxgi->m_allocator->deallocate(monitor->m_modes[k]);
            }
            if (monitor->m_modes != nullptr) {
                monitor->m_modeAllocator->deallocate(monitor->m_modes);
            }
            dxgi->m_allocator->deallocate(monitor);
        }
        if (gpu->m_monitors != nullptr) {
            gpu->m_allocator->deallocate(gpu->m_monitors);
        }
        dxgi->m_allocator->deallocate(gpu);
    }
    dxgi->m_gpuCount = 0;
    for (void** factory : { &dxgi->m_factory1, &dxgi->m_factory2, &dxgi->m_factory3, &dxgi->m_factory4,
             &dxgi->m_factory5, &dxgi->m_factory6 }) {
        if (*factory != nullptr) {
            static_cast<IUnknown*>(*factory)->Release();
            *factory = nullptr;
        }
    }
}

}
