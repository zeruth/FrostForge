#include "prism/Instance.hpp"
#include "prism/Intel.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <malloc.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstring>
#include <new>

// D3D12 on Windows 7 (D3D12On7) answers memory queries through this interface; its header is
// not in the Windows SDK.
MIDL_INTERFACE("74eaee3f-2f4b-476d-82ba-2b85cb49e310")
ID3D12DeviceDownlevel : public IUnknown {
    public:
    virtual HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT nodeIndex, DXGI_MEMORY_SEGMENT_GROUP group,
        DXGI_QUERY_VIDEO_MEMORY_INFO* info) = 0;
};

namespace pm {

namespace {

template <class T>
T& At(void* base, uint64_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// How a D3D12 device is created for a vendor (vtables 0x142924d90 default, 0x142926818 AMD,
// 0x142926850 Intel, 0x142924dc8 NVIDIA): slot 0x08 creates, 0x10 releases, 0x18 fills the
// vendor's part of the device data, 0x20 succeeds.
class DeviceCreator12 {
    public:
    virtual ~DeviceCreator12() {}
    // ref: FUN_1423588a0
    virtual RESULT* Create(RESULT* out, PFN_D3D12_CREATE_DEVICE create, void* a3, GPU* gpu, D3D_FEATURE_LEVEL level,
        ID3D12Device** device) {
        (void)a3;
        HRESULT hr = create(static_cast<IUnknown*>(gpu->m_adapter1), level, __uuidof(ID3D12Device),
            reinterpret_cast<void**>(device));
        *out = ResultFromHResult(hr);
        return out;
    }
    // ref: FUN_1423588e0
    virtual void Release(IUnknown* device) {
        if (device != nullptr) {
            device->Release();
        }
    }
    // ref: _guard_check_icall (default: nothing to fill)
    virtual void FillDeviceData(uint8_t* data) {
        (void)data;
    }
    // ref: FUN_142310860
    virtual RESULT* Slot20(RESULT* out) {
        *out = Ok();
        return out;
    }
};

// Intel keeps the device it made and the adapter it made it on; its device data comes from a
// D3D11 device on the same adapter, as D3D12 has no counters (0x142926850, FUN_142358b40).
class IntelDeviceCreator12 : public DeviceCreator12 {
    public:
    Allocator* m_allocator;         // 0x08
    ID3D12Device* m_device;         // 0x10
    IDXGIAdapter1* m_adapter;       // 0x18

    explicit IntelDeviceCreator12(Allocator* allocator = nullptr)
        : m_allocator(allocator), m_device(nullptr), m_adapter(nullptr) {}

    // ref: FUN_142358b90
    RESULT* Create(RESULT* out, PFN_D3D12_CREATE_DEVICE create, void* a3, GPU* gpu, D3D_FEATURE_LEVEL level,
        ID3D12Device** device) override {
        this->m_adapter = static_cast<IDXGIAdapter1*>(gpu->m_adapter1);
        DeviceCreator12::Create(out, create, a3, gpu, level, &this->m_device);
        *device = this->m_device;
        return out;
    }

    // ref: FUN_142358bf0
    void FillDeviceData(uint8_t* data) override {
        data[0x78] = IntelGeneration(reinterpret_cast<const uint16_t*>(data + 2));
        IDXGIAdapter1* adapter = this->m_adapter;
        Allocator* allocator = this->m_allocator;
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        if (d3d11 == nullptr) {
            return;
        }
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice"));
        if (create != nullptr) {
            HRESULT hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, 7, &device, nullptr, &context);
            if (SUCCEEDED(hr)) {
                IntelQueryDeviceInfo(allocator, data + 0x78, device, context);
            }
            if (device != nullptr) {
                device->Release();
            }
            if (context != nullptr) {
                context->Release();
            }
        }
        FreeLibrary(d3d11);
    }
};

// What a capability query holds while it runs (0xe0 bytes, FUN_142356270 / FUN_142356410).
struct PROBE12 {
    IUnknown* m_adapter1;           // 0x00
    IUnknown* m_adapter2;           // 0x08
    IDXGIAdapter3* m_adapter3;      // 0x10
    IUnknown* m_adapter4;           // 0x18
    ID3D12Device* m_device;         // 0x20
    ID3D12DeviceDownlevel* m_downlevel;  // 0x28
    ID3D12Device1* m_device1;       // 0x30
    ID3D12Device2* m_device2;       // 0x38
    ID3D12Device3* m_device3;       // 0x40
    ID3D12Device4* m_device4;       // 0x48
    ID3D12Device5* m_device5;       // 0x50
    ID3D12Device6* m_device6;       // 0x58
    ID3D12Device7* m_device7;       // 0x60
    ID3D12Device8* m_device8;       // 0x68
    ID3D12Device9* m_device9;       // 0x70
    DeviceCreator12* m_creator;     // 0x78
    DeviceCreator12 m_default;      // 0x80
    DeviceCreator12 m_amd;          // 0x88: FUN_1423588f0 over the AGS context
    uint64_t m_amdContext;
    IntelDeviceCreator12 m_intel;   // 0x98
    DeviceCreator12 m_nvidia;       // 0xb8: 0x142924dc8
    uint64_t m_c0;
    uint64_t m_c8;
    uint16_t m_d0;
    uint32_t m_d4;
    uint64_t m_d8;
};
static_assert(offsetof(PROBE12, m_creator) == 0x78, "PROBE12 layout");
static_assert(offsetof(PROBE12, m_intel) == 0x98, "PROBE12 layout");
static_assert(sizeof(PROBE12) == 0xe0, "PROBE12 is 0xe0 bytes");

// The shader models a device is asked for, newest first (0x142925048).
const D3D_SHADER_MODEL s_shaderModels[] = { static_cast<D3D_SHADER_MODEL>(0x67), static_cast<D3D_SHADER_MODEL>(0x66),
    static_cast<D3D_SHADER_MODEL>(0x65), static_cast<D3D_SHADER_MODEL>(0x64), static_cast<D3D_SHADER_MODEL>(0x63),
    static_cast<D3D_SHADER_MODEL>(0x62), static_cast<D3D_SHADER_MODEL>(0x61), static_cast<D3D_SHADER_MODEL>(0x60) };

// pm_dx12::Instance (vtable 0x142920a00, 0x128 bytes) over its base (0x142924e10, 0x70 bytes).
class Dx12Backend : public Backend {
    public:
    DxgiInstance* m_dxgi;           // 0x20
    HMODULE m_module;               // 0x28
    PFN_D3D12_GET_DEBUG_INTERFACE m_getDebugInterface;  // 0x30
    PFN_D3D12_CREATE_DEVICE m_create;  // 0x38
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE m_serializeRootSignature;  // 0x40
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE m_serializeVersionedRootSignature;  // 0x48
    decltype(&D3D12EnableExperimentalFeatures) m_enableExperimentalFeatures;  // 0x50
    Allocator* m_bufferAllocator;   // 0x58: the instance's allocator
    void* m_buffer;                 // 0x60
    uint64_t m_68;                  // 0x68
    Instance* m_instance;           // 0x70
    uint8_t m_info[0x88];           // 0x78: the instance info from +0x18
    uint32_t m_flags;               // 0x100: info +0xa0
    uint32_t m_pad104;
    uint32_t m_featureLevel;        // 0x108: info +0xd8, a forced feature level
    uint32_t m_10c;                 // 0x10c: info +0xdc
    const char* m_library;          // 0x110: info +0xe0, the D3D12 runtime to load
    uint64_t m_experimental;        // 0x118: info +0xe8, its low byte enables experimental shader models
    void* m_120;                    // 0x120: info +0xa8

    // ref: FUN_1423565d0 (the base), FUN_14232ba50
    explicit Dx12Backend(Instance* instance) {
        this->m_owner = instance->m_owner;
        this->m_allocator = instance->m_allocator;
        this->m_messenger = instance->m_messenger;
        this->m_dxgi = &instance->m_dxgi;
        this->m_module = nullptr;
        this->m_getDebugInterface = nullptr;
        this->m_create = nullptr;
        this->m_serializeRootSignature = nullptr;
        this->m_serializeVersionedRootSignature = nullptr;
        this->m_enableExperimentalFeatures = nullptr;
        this->m_bufferAllocator = instance->m_allocator;
        this->m_buffer = nullptr;
        this->m_68 = 0;
        this->m_instance = instance;
        std::memset(this->m_info, 0, sizeof(this->m_info));
        At<uint32_t>(this->m_info, 0x10) = 0xf;
        this->m_flags = 0;
        this->m_pad104 = 0;
        this->m_featureLevel = 0;
        this->m_10c = 0;
        this->m_library = nullptr;
        this->m_experimental = 0;
        this->m_120 = nullptr;
    }

    // ref: FUN_14232f090, FUN_142356690, FUN_142356640
    ~Dx12Backend() override {
        if (this->m_module != nullptr) {
            FreeLibrary(this->m_module);
        }
        if (this->m_buffer != nullptr) {
            this->m_bufferAllocator->deallocate(this->m_buffer);
        }
    }

    // ref: FUN_142356940
    RESULT* Init(RESULT* out, const INSTANCEINFO* info) override {
        auto bytes = reinterpret_cast<const uint8_t*>(info);
        std::memcpy(this->m_info, bytes + 0x18, 0x88);
        std::memcpy(&this->m_flags, bytes + 0xa0, 4);
        std::memcpy(&this->m_featureLevel, bytes + 0xd8, 0x10);
        std::memcpy(&this->m_experimental, bytes + 0xe8, 8);
        std::memcpy(&this->m_120, bytes + 0xa8, 8);
        if (this->m_instance->m_osClass - 3 < 2) {
            // Vista and 7 have no D3D12
            *out = Ok();
            out->m_code = 1;
            return out;
        }
        const char* library = this->m_library != nullptr ? this->m_library : "d3d12.dll";
        size_t length = std::strlen(library);
        auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 0x11) & ~static_cast<size_t>(0xf)));
        MultiByteToWideChar(CP_UTF8, 0, library, -1, wide, 0x3fffffff);
        this->m_module = LoadLibraryW(wide);
        if (this->m_module == nullptr) {
            this->m_module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (this->m_module == nullptr) {
                return ReportError(this, out, 2, "pm_dx12::Instance::Init",
                    ResultFromHResultDx12(static_cast<int32_t>(GetLastError())), "LoadLibrary(%s) Failed", "d3d12.dll");
            }
        }
        const char* missing = "D3D12CreateDevice";
        this->m_create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(this->m_module, missing));
        if (this->m_create != nullptr) {
            missing = "D3D12SerializeRootSignature";
            this->m_serializeRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
                GetProcAddress(this->m_module, "D3D12SerializeRootSignature"));
            this->m_serializeVersionedRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(
                GetProcAddress(this->m_module, "D3D12SerializeVersionedRootSignature"));
            if (this->m_serializeRootSignature != nullptr || this->m_serializeVersionedRootSignature != nullptr) {
                this->m_enableExperimentalFeatures = reinterpret_cast<decltype(&D3D12EnableExperimentalFeatures)>(
                    GetProcAddress(this->m_module, "D3D12EnableExperimentalFeatures"));
                this->m_getDebugInterface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
                    GetProcAddress(this->m_module, "D3D12GetDebugInterface"));
                if (this->m_getDebugInterface != nullptr && (info->m_cpuValidation != 0 || info->m_gpuValidation != 0)) {
                    this->EnableDebugLayer(info);
                }
                if (static_cast<uint8_t>(this->m_experimental) != 0) {
                    this->m_enableExperimentalFeatures(1, &D3D12ExperimentalShaderModels, nullptr, nullptr);
                }
                *out = Ok();
                return out;
            }
        }
        return ReportError(this, out, 2, "pm_dx12::Instance::Init",
            ResultFromHResultDx12(static_cast<int32_t>(GetLastError())), "GetProcAddress(%s) Failed", missing);
    }

    // The debug layer for CPU validation, and GPU-based validation on top of it (inlined in
    // FUN_142356940).
    void EnableDebugLayer(const INSTANCEINFO* info) {
        ID3D12Debug* debug = nullptr;
        RESULT ignored;
        HRESULT hr = this->m_getDebugInterface(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&debug));
        if (FAILED(hr)) {
            ReportError(this, &ignored, 1, "pm_dx12::Instance::Init", ResultFromHResultDx12(hr),
                "CPU/GPU validation requested but D3D12 Debug Interface is unavailable");
            return;
        }
        if (info->m_cpuValidation != 0) {
            debug->EnableDebugLayer();
        }
        if (info->m_gpuValidation != 0) {
            if ((info->m_a0 & 8) != 0) {
                RESULT warning = Ok();
                warning.m_code = 1;
                ReportError(this, &ignored, 1, "pm_dx12::Instance::Init", warning,
                    "Enabling GPU Validation with raytracing may display rendering issues on certain versions of Windows");
            }
            ID3D12Debug1* debug1 = nullptr;
            if (SUCCEEDED(debug->QueryInterface(__uuidof(ID3D12Debug1), reinterpret_cast<void**>(&debug1)))) {
                debug1->SetEnableGPUBasedValidation(TRUE);
                debug1->Release();
            }
        }
        debug->Release();
    }

    // ref: FUN_142335a00
    RESULT* CreateDevice(RESULT* out, GPU* gpu, const void* params, void* device, DEVICEVALUE* value) override {
        (void)gpu;
        (void)params;
        (void)device;
        (void)value;
        FROST_UNIMPLEMENTED();
        *out = Ok();
        out->m_code = 1;
        return out;
    }

    // ref: FUN_14234eb10
    void Slot18() override {
        FROST_UNIMPLEMENTED();
    }

    // ref: FUN_1423466f0
    void FillDeviceData(GPU* gpu, DEVICEVALUE* value) override {
        if (gpu->m_size != sizeof(GPU)) {
            return;
        }
        PROBE12 probe;
        this->ProbeConstruct(&probe, gpu);
        RESULT result;
        this->ProbeDevice(&result, gpu, value, &probe);
        this->ProbeDestroy(&probe);
    }

    // ref: FUN_142310860
    RESULT* BuildDeviceList(RESULT* out) override {
        *out = Ok();
        return out;
    }

    // ref: FUN_142341100
    void RebindDevice(void* data, GPU* gpu) override {
        (void)data;
        (void)gpu;
        FROST_UNIMPLEMENTED();
    }

    // ref: FUN_142356850 (forwards to the DXGI instance's FUN_142351410)
    void Slot38(void* a2) override {
        (void)a2;
        FROST_UNIMPLEMENTED();
    }

    // ref: FUN_142356270
    void ProbeConstruct(PROBE12* probe, GPU* gpu) {
        std::memset(probe, 0, sizeof(*probe));
        new (&probe->m_default) DeviceCreator12();
        new (&probe->m_amd) DeviceCreator12();       // FUN_1423588f0 (0x142926818)
        new (&probe->m_intel) IntelDeviceCreator12(this->m_allocator);  // FUN_142358b40 (0x142926850)
        new (&probe->m_nvidia) DeviceCreator12();    // 0x142924dc8
        Instance* instance = this->m_instance;
        if (gpu->m_vendorId == 0x1002) {
            if (instance->m_ags.m_context != nullptr && instance->m_osClass != 2) {
                probe->m_creator = &probe->m_amd;
            }
        } else if (gpu->m_vendorId == 0x10de) {
            if (instance->m_nvapi != 0) {
                probe->m_creator = &probe->m_nvidia;
            }
        } else if (gpu->m_vendorId == 0x8086) {
            probe->m_creator = &probe->m_intel;
        }
        if (probe->m_creator == nullptr || (gpu->m_flags & 1) != 0) {
            probe->m_creator = &probe->m_default;
        }
        IUnknown* adapters[] = { static_cast<IUnknown*>(gpu->m_adapter1), static_cast<IUnknown*>(gpu->m_e0[0]),
            static_cast<IUnknown*>(gpu->m_e0[1]), static_cast<IUnknown*>(gpu->m_e0[2]) };
        if (adapters[0] != nullptr) {
            probe->m_adapter1 = adapters[0];
            adapters[0]->AddRef();
        }
        if (adapters[1] != nullptr) {
            probe->m_adapter2 = adapters[1];
            adapters[1]->AddRef();
        }
        if (adapters[2] != nullptr) {
            probe->m_adapter3 = static_cast<IDXGIAdapter3*>(adapters[2]);
            adapters[2]->AddRef();
        }
        if (adapters[3] != nullptr) {
            probe->m_adapter4 = adapters[3];
            adapters[3]->AddRef();
        }
    }

    // ref: FUN_142356410
    void ProbeDestroy(PROBE12* probe) {
        IUnknown** owned[] = { reinterpret_cast<IUnknown**>(&probe->m_downlevel), &probe->m_adapter1,
            &probe->m_adapter2, reinterpret_cast<IUnknown**>(&probe->m_adapter3), &probe->m_adapter4,
            reinterpret_cast<IUnknown**>(&probe->m_device1), reinterpret_cast<IUnknown**>(&probe->m_device2),
            reinterpret_cast<IUnknown**>(&probe->m_device3), reinterpret_cast<IUnknown**>(&probe->m_device4),
            reinterpret_cast<IUnknown**>(&probe->m_device5), reinterpret_cast<IUnknown**>(&probe->m_device6),
            reinterpret_cast<IUnknown**>(&probe->m_device7), reinterpret_cast<IUnknown**>(&probe->m_device8),
            reinterpret_cast<IUnknown**>(&probe->m_device9) };
        for (IUnknown** slot : owned) {
            if (*slot != nullptr) {
                (*slot)->Release();
                *slot = nullptr;
            }
        }
        probe->m_creator->Release(probe->m_device);
    }

    // ref: FUN_1423566b0
    RESULT* ProbeDevice(RESULT* out, GPU* gpu, DEVICEVALUE* value, PROBE12* probe) {
        auto level = static_cast<D3D_FEATURE_LEVEL>(this->m_featureLevel != 0 ? this->m_featureLevel : 0xb000);
        RESULT result;
        probe->m_creator->Create(&result, this->m_create, this->m_info + 0x68, gpu, level, &probe->m_device);
        if (result.m_code != 0) {
            return ReportError(this, out, 2, "pm_dx12::Instance::CreateDevice", result, "D3D12CreateDevice Failed");
        }
        ID3D12Device* device = probe->m_device;
        device->QueryInterface(__uuidof(ID3D12DeviceDownlevel), reinterpret_cast<void**>(&probe->m_downlevel));
        device->QueryInterface(__uuidof(ID3D12Device1), reinterpret_cast<void**>(&probe->m_device1));
        device->QueryInterface(__uuidof(ID3D12Device2), reinterpret_cast<void**>(&probe->m_device2));
        device->QueryInterface(__uuidof(ID3D12Device3), reinterpret_cast<void**>(&probe->m_device3));
        device->QueryInterface(__uuidof(ID3D12Device4), reinterpret_cast<void**>(&probe->m_device4));
        device->QueryInterface(__uuidof(ID3D12Device5), reinterpret_cast<void**>(&probe->m_device5));
        device->QueryInterface(__uuidof(ID3D12Device6), reinterpret_cast<void**>(&probe->m_device6));
        device->QueryInterface(__uuidof(ID3D12Device7), reinterpret_cast<void**>(&probe->m_device7));
        device->QueryInterface(__uuidof(ID3D12Device8), reinterpret_cast<void**>(&probe->m_device8));
        device->QueryInterface(__uuidof(ID3D12Device9), reinterpret_cast<void**>(&probe->m_device9));
        if (value->m_data.m_ready == 0) {
            this->FillCapabilities(gpu, probe, value);
        }
        *out = Ok();
        return out;
    }

    // The device data for D3D12: fixed limits, then what the device reports through
    // CheckFeatureSupport and the adapter's memory.
    // ref: FUN_142356d40
    void FillCapabilities(GPU* gpu, PROBE12* probe, DEVICEVALUE* value) {
        uint8_t* data = reinterpret_cast<uint8_t*>(value);
        ID3D12Device* device = probe->m_device;
        At<uint16_t>(data, 0x10) = 0x103;  // the API (D3D12), supported
        std::memcpy(data + 0x12, &gpu->m_vendorId, 8);
        At<uint16_t>(data, 0x1a) = gpu->m_revision;
        At<uint32_t>(data, 0x234) = (At<uint32_t>(data, 0x234) & ~0x2000u) | (((this->m_flags >> 4) & 1) << 0xd);
        this->m_instance->ReportOsVersion(data + 0x10);
        static const D3D_FEATURE_LEVEL s_levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1 };  // 0x142925038
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels = { 4, s_levels, static_cast<D3D_FEATURE_LEVEL>(0) };
        HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels));
        At<uint8_t>(data, 0x121) = 1;
        At<uint32_t>(data, 0x128) = 0x3fffff;
        At<uint32_t>(data, 0x150) = 0x4000;
        int32_t level = SUCCEEDED(hr) ? static_cast<int32_t>(levels.MaxSupportedFeatureLevel) : 0xb000;
        At<uint64_t>(data, 0x1b8) = 8;
        At<uint32_t>(data, 0x1f4) = 8;
        At<int32_t>(data, 0xcc) = level;
        At<uint32_t>(data, 0x154) = 0x4000;
        At<uint32_t>(data, 0x158) = 0x4000;
        At<uint64_t>(data, 0x140) = 0;
        At<uint32_t>(data, 0x15c) = 0x800;
        At<uint64_t>(data, 0x160) = 0x800;
        At<uint32_t>(data, 0x16c) = 0x10000;
        At<uint32_t>(data, 0x180) = 0x100;
        At<uint32_t>(data, 0x184) = 0x100;
        At<uint32_t>(data, 0x188) = 0x10;
        At<uint32_t>(data, 0x18c) = 0x10;
        At<uint32_t>(data, 0x178) = 4;
        At<uint32_t>(data, 0x17c) = 4;
        At<uint32_t>(data, 0x190) = 0x100;
        At<uint32_t>(data, 0x194) = 0x200;
        At<uint32_t>(data, 0x198) = 0x20;
        At<uint32_t>(data, 0x19c) = 0x40;
        At<uint32_t>(data, 0x1a0) = 0x20;
        At<uint32_t>(data, 0x1a4) = 0x100;
        At<uint32_t>(data, 0x1a8) = 0x10;
        At<uint32_t>(data, 0x1ac) = 0x40;
        At<uint32_t>(data, 0x1b0) = 0x10;
        At<uint32_t>(data, 0x1b4) = 0x30;
        At<uint32_t>(data, 0x1f0) = 0x10;
        At<uint32_t>(data, 0x1f8) = 0x20;
        At<uint32_t>(data, 0x204) = 0x1f;
        At<uint32_t>(data, 0x208) = 0xffff;
        At<uint32_t>(data, 0x20c) = 0xffff;
        At<uint32_t>(data, 0x210) = 0xc7000000;
        At<uint32_t>(data, 0x214) = 0x46fffe00;
        At<uint32_t>(data, 0x218) = 0xc1800000;
        At<uint64_t>(data, 0x21c) = 0x417fd70a;
        At<uint8_t>(data, 0x224) = level > 0xb0ff ? 5 : 4;
        At<uint16_t>(data, 0x22b) = 0x101;
        At<uint64_t>(data, 0x130) = 0x3fff;
        uint64_t descriptorFlags = 0;
        if (this->m_instance->m_osClass > 2) {
            descriptorFlags = 0x48;
            At<uint64_t>(data, 0x140) = 0x48;
        }
        if (probe->m_downlevel == nullptr) {
            At<uint8_t>(data, 0x148) = 1;
            At<uint64_t>(data, 0x140) = descriptorFlags | 0x3c;
        }
        uint32_t bits = (At<uint32_t>(data, 0x230) & 0xffffff7f) | 0x17f7f;
        bits = (bits & ~0x20000u) | ((this->m_flags & 1) << 0x11);
        bits = (bits & ~0x40000u) | (((this->m_flags >> 1) & 1) << 0x12);
        At<uint32_t>(data, 0x230) = (bits & 0xffdfffff) | 0xff880000;
        uint32_t more = At<uint32_t>(data, 0x234);
        more = (probe->m_downlevel == nullptr ? 4u : 0u) | (more & 0xfffffffa) | 0x800;
        if (GetSystemMetrics(SM_REMOTESESSION) != 0) {  // FUN_140267270
            more &= ~4u;
        }
        At<uint32_t>(data, 0x234) = more | 0x30030;

        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
            At<uint32_t>(data, 0xd0) = (options.TypedUAVLoadAdditionalFormats != 0 ? 1u : 0u)
                | (At<uint32_t>(data, 0xd0) & 0xfffffffe);
            int32_t binding = options.ResourceBindingTier;
            At<uint8_t>(data, 0x226) = binding >= 3 ? 2 : (binding >= 2 ? 1 : 0);
            int32_t heap = options.ResourceHeapTier;
            uint8_t heapTier = heap >= 2 ? 2 : (heap > 0 ? 1 : 0);
            At<uint8_t>(data, 0x227) = heapTier;
            uint32_t heapFlag = heapTier != 0 ? 0x80 : 0;
            if (binding == 3) {
                At<uint8_t>(data, 0x224) = 6;
            }
            int32_t tiled = options.TiledResourcesTier;
            At<uint8_t>(data, 0x225) = tiled >= 3 ? 3 : (tiled >= 2 ? 2 : (tiled > 0 ? 1 : 0));
            At<uint32_t>(data, 0x168) = 0x10000;
            At<uint32_t>(data, 0x234) = (options.MinPrecisionSupport == D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT ? 1u : 0u)
                | heapFlag | (At<uint32_t>(data, 0x234) & 0xffffff7c) | (options.ROVsSupported != 0 ? 2u : 0u);
        }
        D3D12_FEATURE_DATA_ARCHITECTURE architecture = {};
        uint32_t uma = 0;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)))) {
            if (gpu->m_type == GPU_INTEGRATED) {
                uma = 8;
            }
        } else {
            uma = architecture.UMA != 0 ? 8 : 0;
        }
        At<uint32_t>(data, 0x234) = uma | (At<uint32_t>(data, 0x234) & 0xfffffff7);
        At<uint16_t>(data, 0xee) = 0x101;
        At<uint8_t>(data, 0xe9) = 5;
        if (probe->m_device1 != nullptr) {
            for (D3D_SHADER_MODEL model : s_shaderModels) {
                D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { model };
                if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, 4))) {
                    int32_t highest = shaderModel.HighestShaderModel;
                    for (int32_t step = 0; step < 8; step++) {
                        if (highest > 0x5f + step) {
                            At<uint8_t>(data, 0xf0 + step) = 1;
                            At<uint8_t>(data, 0xe9) = static_cast<uint8_t>(6 + step);
                        }
                    }
                    break;
                }
            }
        }
        if (probe->m_device1 != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)))) {
                At<uint32_t>(data, 0x1fc) = options1.WaveLaneCountMin;
                At<uint32_t>(data, 0x200) = options1.WaveLaneCountMax;
                At<uint32_t>(data, 0x234) = ((options1.WaveOps & 1) << 10) | (At<uint32_t>(data, 0x234) & 0xfffffbff);
            }
        }
        if (probe->m_device2 != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2 = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &options2, sizeof(options2)))) {
                uint32_t programmable = options2.ProgrammableSamplePositionsTier > 0 ? 0x400000 : 0;
                At<uint32_t>(data, 0x230) = programmable | (options2.DepthBoundsTestSupported != 0 ? 0x8000u : 0u)
                    | (At<uint32_t>(data, 0x230) & 0xffbf7fff);
            }
            if (probe->m_device2 != nullptr) {
                D3D12_FEATURE_DATA_SHADER_CACHE cache = {};
                device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache));
            }
        }
        if (probe->m_device3 != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3 = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3)))) {
                At<uint32_t>(data, 0x234) = ((options3.BarycentricsSupported & 1) << 6)
                    | (At<uint32_t>(data, 0x234) & 0xffffffbf);
            }
            if (probe->m_device3 != nullptr) {
                D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 = {};
                device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4));
            }
        }
        if (probe->m_device5 != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
                uint8_t raytracing = 0;
                if ((this->m_flags & 8) != 0) {
                    if (options5.RaytracingTier > 9) {
                        raytracing = 2;
                    }
                    if (options5.RaytracingTier > 10) {
                        raytracing = 3;
                    }
                }
                At<uint8_t>(data, 0x228) = raytracing;
            }
        }
        if (probe->m_device6 != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)))) {
                At<uint8_t>(data, 0x229) = static_cast<uint8_t>(options6.VariableShadingRateTier);
                At<uint32_t>(data, 0x1c0) = options6.ShadingRateImageTileSize;
            }
            if (At<uint8_t>(data, 0x229) != 0) {
                uint64_t rates = At<uint64_t>(data, 0x1c8);
                uint64_t combiners = At<uint64_t>(data, 0x1d0);
                At<uint64_t>(data, 0x1d8) |= 7;
                At<uint64_t>(data, 0x1c8) = rates | 7;
                At<uint64_t>(data, 0x1d0) = combiners | 7;
                if (options6.AdditionalShadingRatesSupported) {
                    At<uint64_t>(data, 0x1c8) = rates | 0x3f;
                    At<uint64_t>(data, 0x1d0) = combiners | 0xf;
                }
            }
        }
        if (probe->m_device7 != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
                At<uint8_t>(data, 0x22a) = options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_1 ? 1 : 0;
            }
        }
        if (probe->m_device9 != nullptr) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS8 options8 = {};
            device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS8, &options8, sizeof(options8));
            if (probe->m_device9 != nullptr) {
                D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9 = {};
                device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9));
                if (probe->m_device9 != nullptr) {
                    D3D12_FEATURE_DATA_D3D12_OPTIONS10 options10 = {};
                    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS10, &options10, sizeof(options10));
                    if (probe->m_device9 != nullptr) {
                        D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11 = {};
                        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11, sizeof(options11));
                    }
                }
            }
        }
        bool shared = ((At<uint32_t>(data, 0x234) >> 3) & 1) != 0;
        DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal = {};
        if (probe->m_downlevel != nullptr) {
            probe->m_downlevel->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            probe->m_downlevel->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        } else if (probe->m_adapter3 != nullptr) {
            probe->m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            probe->m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
        }
        uint64_t memory[6];
        DxgiQueryVideoMemory(this->m_dxgi, memory, static_cast<IDXGIAdapter1*>(probe->m_adapter1), &local, &nonLocal,
            shared);
        float megabytes = static_cast<float>(static_cast<uint32_t>(memory[0] >> 0x14)) * 0.25f;  // DAT_1426193c4
        uint64_t budget = static_cast<uint64_t>(megabytes) * 0x100000;
        uint32_t limit;
        if (budget < 0x8000000) {
            limit = 0x8000000;
        } else if (budget < 0x100000000ull) {
            limit = static_cast<uint32_t>(budget);
        } else {
            limit = 0xffffffff;
        }
        At<uint32_t>(data, 0x14c) = limit;
        At<uint32_t>(data, 0x170) = limit;
        At<uint32_t>(data, 0x174) = limit;
        if (At<uint16_t>(data, 0x12) == 0x1414 && At<uint16_t>(data, 0x14) == 0x8c) {
            // the Microsoft Basic Render Driver (WARP)
            At<uint32_t>(data, 0x230) &= 0xffbfffff;
        }
        probe->m_creator->FillDeviceData(data + 0x10);
    }
};

}
static_assert(offsetof(Dx12Backend, m_module) == 0x28, "Instance::GetPlatformHandles reads the module at +0x28");

// ref: FUN_14232ba50
Backend* CreateDx12Backend(Instance* instance) {
    void* memory = instance->m_allocator->allocate(sizeof(Dx12Backend), 8);
    return memory != nullptr ? new (memory) Dx12Backend(instance) : nullptr;
}

}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140267270=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1423588f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142358b40=GenForwardMissing")
// ---- end of link fallbacks ----
