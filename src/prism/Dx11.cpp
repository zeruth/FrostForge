#include "prism/Instance.hpp"
#include "prism/Intel.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <cstring>
#include <new>

namespace pm {

namespace {

template <class T>
T& At(void* base, uint64_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// How a device is created for a vendor (vtables 0x14291e980 default, 0x142924c68 AMD,
// 0x142924ce0 Intel, 0x14291e9f8 NVIDIA): slot 0x08 creates, 0x10 releases, 0x18 runs after
// a device is made, 0x20 fills the vendor's part of the device data.
class DeviceCreator {
    public:
    virtual ~DeviceCreator() {}
    // ref: FUN_14234dd00 (default and Intel)
    virtual RESULT* Create(RESULT* out, PFN_D3D11_CREATE_DEVICE create, void* a3, IDXGIAdapter* adapter, UINT flags,
        const D3D_FEATURE_LEVEL* levels, UINT levelCount, ID3D11Device** device, D3D_FEATURE_LEVEL* level,
        ID3D11DeviceContext** context) {
        (void)a3;
        HRESULT hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, levelCount, 7, device, level, context);
        *out = ResultFromHResult(hr);
        return out;
    }
    // ref: FUN_14234de60 (default)
    virtual void Release(IUnknown* device, IUnknown* context) {
        if (device != nullptr) {
            device->Release();
        }
        if (context != nullptr) {
            context->Release();
        }
    }
    // ref: _guard_check_icall (default)
    virtual void AfterCreate(ID3D11Device* device, D3D_FEATURE_LEVEL level, ID3D11DeviceContext* context) {
        (void)device;
        (void)level;
        (void)context;
    }
    // ref: _guard_check_icall (default: nothing to fill)
    virtual void FillDeviceData(uint8_t* data) {
        (void)data;
    }
};

// Intel keeps the device and context it was handed, and reads its generation and the
// driver's device information from them (0x142924ce0, FUN_1423551d0).
class IntelDeviceCreator : public DeviceCreator {
    public:
    Allocator* m_allocator;
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;

    explicit IntelDeviceCreator(Allocator* allocator = nullptr)
        : m_allocator(allocator), m_device(nullptr), m_context(nullptr) {}

    // ref: FUN_142355240
    void Release(IUnknown* device, IUnknown* context) override {
        DeviceCreator::Release(device, context);
        this->m_device = nullptr;
    }
    // ref: 0x142355230
    void AfterCreate(ID3D11Device* device, D3D_FEATURE_LEVEL level, ID3D11DeviceContext* context) override {
        (void)level;
        this->m_device = device;
        this->m_context = context;
    }
    // ref: FUN_1423551f0
    void FillDeviceData(uint8_t* data) override {
        data[0x78] = IntelGeneration(reinterpret_cast<const uint16_t*>(data + 2));
        IntelQueryDeviceInfo(this->m_allocator, data + 0x78, this->m_device, this->m_context);
    }
};

// What a capability query holds while it runs (0xd8 bytes, FUN_142313720 / FUN_142314700).
struct PROBE {
    IUnknown* m_adapter1;           // 0x00
    IUnknown* m_adapter2;           // 0x08
    IDXGIAdapter3* m_adapter3;      // 0x10
    IUnknown* m_adapter4;           // 0x18
    ID3D11Device* m_device;         // 0x20
    ID3D11Device1* m_device1;       // 0x28
    ID3D11Device2* m_device2;       // 0x30
    ID3D11Device3* m_device3;       // 0x38
    ID3D11Device4* m_device4;       // 0x40
    ID3D11DeviceContext* m_context; // 0x48
    ID3D11DeviceContext1* m_context1;  // 0x50
    D3D_FEATURE_LEVEL m_level;      // 0x58
    DeviceCreator* m_creator;       // 0x60
    DeviceCreator m_default;        // 0x68
    DeviceCreator m_amd;            // 0x70
    IntelDeviceCreator m_intel;     // 0x90
    DeviceCreator m_nvidia;         // 0xb0
};

// pm_dx11::Instance (vtable 0x14291ea70, 0xd8 bytes).
class Dx11Backend : public Backend {
    public:
    Instance* m_instance;           // 0x20
    DxgiInstance* m_dxgi;           // 0x28
    uint8_t m_info[0x88];           // 0x30: the instance info from +0x18
    uint32_t m_b8;                  // 0xb8: info +0xa0
    uint32_t m_featureLevel;        // 0xbc: info +0xd0, a forced feature level
    HMODULE m_module;               // 0xc0
    PFN_D3D11_CREATE_DEVICE m_create;  // 0xc8
    void* m_d0;                     // 0xd0: info +0xa8

    explicit Dx11Backend(Instance* instance) {
        this->m_owner = instance->m_owner;
        this->m_allocator = instance->m_allocator;
        this->m_messenger = instance->m_messenger;
        this->m_instance = instance;
        this->m_dxgi = &instance->m_dxgi;
        std::memset(this->m_info, 0, sizeof(this->m_info));
        this->m_b8 = 0;
        this->m_featureLevel = 0;
        this->m_module = nullptr;
        this->m_create = nullptr;
        this->m_d0 = nullptr;
    }

    // ref: FUN_142315a20
    ~Dx11Backend() override {
        if (this->m_module != nullptr) {
            FreeLibrary(this->m_module);
        }
    }

    // ref: FUN_1423246b0
    RESULT* Init(RESULT* out, const INSTANCEINFO* info) override {
        auto bytes = reinterpret_cast<const uint8_t*>(info);
        std::memcpy(this->m_info, bytes + 0x18, 0x88);
        std::memcpy(&this->m_b8, bytes + 0xa0, 4);
        std::memcpy(&this->m_featureLevel, bytes + 0xd0, 4);
        std::memcpy(&this->m_d0, bytes + 0xa8, 8);
        this->m_module = LoadLibraryW(L"d3d11.dll");
        if (this->m_module == nullptr) {
            return ReportError(this, out, 2, "pm_dx11::Instance::Init", ResultFromHResult(static_cast<int32_t>(GetLastError())),
                "LoadLibrary(%s) Failed", "d3d11.dll");
        }
        this->m_create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(this->m_module, "D3D11CreateDevice"));
        if (this->m_create == nullptr) {
            return ReportError(this, out, 2, "pm_dx11::Instance::Init", ResultFromHResult(static_cast<int32_t>(GetLastError())),
                "GetProcAddress(%s) Failed", "D3D11CreateDevice");
        }
        *out = Ok();
        return out;
    }

    // ref: FUN_142318a70
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

    // ref: FUN_142326460
    void FillDeviceData(GPU* gpu, DEVICEVALUE* value) override {
        if (gpu->m_size != sizeof(GPU)) {
            return;
        }
        uint8_t options[0x10] = {};
        uint64_t timeout = 1000000;
        std::memcpy(options + 8, &timeout, 8);
        PROBE probe;
        this->ProbeConstruct(&probe, gpu);
        RESULT result;
        this->ProbeDevice(&result, gpu, options, value, &probe);
        this->ProbeDestroy(&probe);
    }

    // ref: FUN_142310860
    RESULT* BuildDeviceList(RESULT* out) override {
        *out = Ok();
        return out;
    }

    // ref: FUN_142322590
    void RebindDevice(void* data, GPU* gpu) override {
        (void)data;
        (void)gpu;
        FROST_UNIMPLEMENTED();
    }

    // ref: FUN_1423224a0
    void Slot38(void* a2) override {
        (void)a2;
        FROST_UNIMPLEMENTED();
    }

    // ref: FUN_142313720
    void ProbeConstruct(PROBE* probe, GPU* gpu) {
        std::memset(probe, 0, sizeof(*probe));
        new (&probe->m_default) DeviceCreator();
        new (&probe->m_amd) DeviceCreator();      // FUN_142354d60 (0x142924c68)
        new (&probe->m_intel) IntelDeviceCreator(this->m_allocator);  // FUN_1423551d0 (0x142924ce0)
        new (&probe->m_nvidia) DeviceCreator();   // 0x14291e9f8
        probe->m_level = static_cast<D3D_FEATURE_LEVEL>(0x1000);
        if (gpu->m_vendorId == 0x1002) {
            if (this->m_instance->m_ags.m_context != nullptr) {
                probe->m_creator = &probe->m_amd;
            }
        } else if (gpu->m_vendorId == 0x10de) {
            if (this->m_instance->m_nvapi != 0) {
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

    // ref: FUN_142314700
    void ProbeDestroy(PROBE* probe) {
        IUnknown** owned[] = { reinterpret_cast<IUnknown**>(&probe->m_context1), &probe->m_adapter1, &probe->m_adapter2,
            reinterpret_cast<IUnknown**>(&probe->m_adapter3), &probe->m_adapter4,
            reinterpret_cast<IUnknown**>(&probe->m_device1), reinterpret_cast<IUnknown**>(&probe->m_device2),
            reinterpret_cast<IUnknown**>(&probe->m_device3), reinterpret_cast<IUnknown**>(&probe->m_device4) };
        for (IUnknown** slot : owned) {
            if (*slot != nullptr) {
                (*slot)->Release();
                *slot = nullptr;
            }
        }
        if (probe->m_creator != nullptr) {
            probe->m_creator->Release(probe->m_device, probe->m_context);
        }
    }

    // ref: FUN_1423187f0
    RESULT* ProbeDevice(RESULT* out, GPU* gpu, const uint8_t* options, DEVICEVALUE* value, PROBE* probe) {
        uint32_t cpuValidation;
        std::memcpy(&cpuValidation, this->m_info + 4, 4);
        UINT flags = cpuValidation != 0 ? D3D11_CREATE_DEVICE_DEBUG : 0;
        if (options[0] != 0) {
            flags |= D3D11_CREATE_DEVICE_SINGLETHREADED;
        }
        flags |= D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY;
        auto adapter = static_cast<IDXGIAdapter*>(gpu->m_adapter1);
        RESULT result;
        if (this->m_featureLevel == 0) {
            D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
            probe->m_creator->Create(&result, this->m_create, &this->m_b8, adapter, flags, &level, 1, &probe->m_device,
                &probe->m_level, &probe->m_context);
            if (result.m_code != 0) {
                static const D3D_FEATURE_LEVEL s_levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                    D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };
                probe->m_creator->Create(&result, this->m_create, &this->m_b8, adapter, flags, s_levels, 6,
                    &probe->m_device, &probe->m_level, &probe->m_context);
            }
        } else {
            auto level = static_cast<D3D_FEATURE_LEVEL>(this->m_featureLevel);
            probe->m_creator->Create(&result, this->m_create, &this->m_b8, adapter, flags, &level, 1, &probe->m_device,
                &probe->m_level, &probe->m_context);
        }
        if (result.m_code == 0 && probe->m_device != nullptr) {
            probe->m_creator->AfterCreate(probe->m_device, probe->m_level, probe->m_context);
            probe->m_device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&probe->m_device1));
            probe->m_device->QueryInterface(__uuidof(ID3D11Device2), reinterpret_cast<void**>(&probe->m_device2));
            probe->m_device->QueryInterface(__uuidof(ID3D11Device3), reinterpret_cast<void**>(&probe->m_device3));
            probe->m_device->QueryInterface(__uuidof(ID3D11Device4), reinterpret_cast<void**>(&probe->m_device4));
            probe->m_context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&probe->m_context1));
            if (value->m_data.m_ready == 0) {
                this->FillCapabilities(gpu, probe, value);
            }
            *out = Ok();
            return out;
        }
        return ReportError(this, out, 2, "pm_dx11::Instance::CreateDevice", result, "%s", "D3D11CreateDevice Failed");
    }

    // The device data for D3D11: the API and its limits from the feature level, then what the
    // device reports through CheckFeatureSupport and the adapter's memory.
    // ref: FUN_142326a50
    void FillCapabilities(GPU* gpu, PROBE* probe, DEVICEVALUE* value) {
        uint8_t* data = reinterpret_cast<uint8_t*>(value);
        At<uint16_t>(data, 0x10) = 0x102;  // the API (D3D11), supported
        std::memcpy(data + 0x12, &gpu->m_vendorId, 8);
        At<uint16_t>(data, 0x1a) = gpu->m_revision;
        this->m_instance->ReportOsVersion(data + 0x10);
        int32_t level = static_cast<int32_t>(probe->m_level);
        At<int32_t>(data, 0xcc) = level;
        At<uint16_t>(data, 0xe9) = 0x100;
        if (level > 0x92ff) {
            At<uint8_t>(data, 0xeb) = 1;
            At<uint8_t>(data, 0xe9) = 1;
        }
        if (level > 0x9fff) {
            At<uint8_t>(data, 0xec) = 1;
            At<uint8_t>(data, 0xe9) = 2;
        }
        if (level > 0xa0ff) {
            At<uint8_t>(data, 0xed) = 1;
            At<uint8_t>(data, 0xe9) = 3;
        }
        if (level > 0xafff) {
            At<uint8_t>(data, 0xee) = 1;
            At<uint8_t>(data, 0xe9) = 4;
        }
        At<uint8_t>(data, 0x121) = 1;
        At<uint32_t>(data, 0x128) = 0;
        uint32_t uav;
        if (level < 0xb000) {
            At<uint32_t>(data, 0x14c) = 0x8000000;
            if (level < 0xa000) {
                At<uint32_t>(data, 0x15c) = 0x100;
                uav = 0;
                uint32_t size = level < 0x9300 ? 0x800 : 0x1000;
                At<uint32_t>(data, 0x150) = size;
                At<uint32_t>(data, 0x154) = size;
                At<uint32_t>(data, 0x158) = level < 0x9300 ? 0x200 : 0x1000;
            } else {
                At<uint32_t>(data, 0x150) = 0x2000;
                At<uint32_t>(data, 0x154) = 0x2000;
                At<uint32_t>(data, 0x158) = 0x2000;
                At<uint32_t>(data, 0x15c) = 0x800;
                uav = 0x200;
            }
        } else {
            At<uint32_t>(data, 0x150) = 0x4000;
            At<uint32_t>(data, 0x154) = 0x4000;
            At<uint32_t>(data, 0x158) = 0x4000;
            At<uint32_t>(data, 0x15c) = 0x800;
            uav = 0x800;
        }
        At<uint32_t>(data, 0x160) = uav;
        At<uint32_t>(data, 0x188) = 0x10;
        At<uint32_t>(data, 0x18c) = 0x10;
        At<uint32_t>(data, 0x178) = 4;
        At<uint32_t>(data, 0x17c) = 4;
        At<uint64_t>(data, 0x194) = 0x400;
        At<uint32_t>(data, 0x1a0) = 0x20;
        At<uint64_t>(data, 0x1b0) = 0x10;
        At<uint64_t>(data, 0x1b8) = 8;
        At<uint32_t>(data, 0x164) = 0;
        At<uint32_t>(data, 0x16c) = 0x10000;
        At<uint32_t>(data, 0x180) = 0x100;
        At<uint32_t>(data, 0x184) = 0x100;
        At<uint32_t>(data, 0x190) = 0x100;
        At<uint32_t>(data, 0x19c) = 0x40;
        At<uint32_t>(data, 0x1a4) = 0x100;
        At<uint64_t>(data, 0x1a8) = 0x100;
        At<uint32_t>(data, 0x1f0) = level > 0x91ff ? 0x10 : 2;
        At<uint32_t>(data, 0x1f4) = level >= 0xa000 ? 8 : (level > 0x92ff ? 4 : 1);
        At<uint32_t>(data, 0x1f8) = 0x20;
        At<uint32_t>(data, 0x208) = 0xffff;
        At<uint32_t>(data, 0x20c) = 0xffff;
        At<uint32_t>(data, 0x210) = 0xc7000000;
        At<uint32_t>(data, 0x214) = 0x46fffe00;
        At<uint32_t>(data, 0x218) = 0xc1800000;
        At<uint64_t>(data, 0x21c) = 0x417fd70a;
        At<uint8_t>(data, 0x224) = level >= 0xb100 ? 5 : (level > 0xafff ? 4 : 0);
        At<uint32_t>(data, 0x226) = 0;
        At<uint16_t>(data, 0x22a) = 0;
        At<uint8_t>(data, 0x22c) = 0;
        At<uint64_t>(data, 0x130) = level > 0x91ff ? 7 : 4;
        if (this->m_instance->m_osClass > 2) {
            At<uint64_t>(data, 0x140) = 0x48;
        }
        uint32_t bits = At<uint32_t>(data, 0x230);
        bits = (bits & 0xa111794d) | 0xc | 0x140 | 0x17800 | 0x1000000 | 0x80000000;
        if (level > 0x91ff) {
            bits |= 2 | 0x200;
        }
        if (level > 0x9fff) {
            bits |= 0x10 | 0x20 | 0x400;
        }
        if ((this->m_b8 & 1) != 0 && level >= 0xa000) {
            bits |= 0x20000;
        }
        if ((this->m_b8 & 2) != 0 && level >= 0xb000) {
            bits |= 0x40000;
        }
        if (level > 0xafff) {
            bits |= 0x80000 | 0x4000000 | 0x8000000;
        }
        if (level > 0x92ff) {
            bits |= 0x800000 | 0x2000000;
        }
        At<uint32_t>(data, 0x230) = bits;
        uint32_t more = (At<uint32_t>(data, 0x234) & 0xfffefffe) | 4;
        if (GetSystemMetrics(SM_REMOTESESSION) != 0) {
            more &= ~4u;
        }
        more = (more & 0xffffff9f) | 0x10 | 0x40000 | (level > 0xafff ? 0x20 : 0);
        At<uint32_t>(data, 0x234) = more;
        ID3D11Device* device = probe->m_device;
        if (level < 0xb000) {
            D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS options = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &options, 4))
                && options.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x) {
                At<uint32_t>(data, 0x230) |= 0x80000;
                At<uint8_t>(data, 0x224) = 1;
            }
        }
        uint32_t precision[2] = {};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, precision, 8))) {
            uint32_t half = precision[0] == 2 && precision[1] == 2 ? 1 : 0;
            At<uint32_t>(data, 0x234) = (At<uint32_t>(data, 0x234) & 0xfffffffe) | half;
        }
        At<uint32_t>(data, 0xd4) &= 0xfffffffe;
        uint32_t threading[2] = {};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_THREADING, threading, 8))) {
            At<uint32_t>(data, 0xd4) = (At<uint32_t>(data, 0xd4) & 0xfffffffe) | (threading[1] & 1);
        }
        if (probe->m_device1 != nullptr) {
            uint32_t options[14] = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, options, sizeof(options)))) {
                At<uint32_t>(data, 0x230) = (At<uint32_t>(data, 0x230) & 0xdfffffff) | ((options[7] & 1) << 0x1d);
            }
        }
        if (probe->m_device2 != nullptr) {
            uint32_t options[4] = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, options, sizeof(options)))) {
                At<uint32_t>(data, 0x230) = (At<uint32_t>(data, 0x230) & 0xfffffffe) | (options[1] != 0 ? 1 : 0);
            }
        }
        uint32_t integrated = gpu->m_type == 2 ? 8 : 0;
        At<uint32_t>(data, 0x234) = integrated | (At<uint32_t>(data, 0x234) & 0xfffffff7);
        if (probe->m_device3 != nullptr) {
            uint32_t options[8] = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, options, sizeof(options)))) {
                At<uint32_t>(data, 0xd0) = (options[1] != 0 ? 1u : 0u) | (At<uint32_t>(data, 0xd0) & 0xfffffffe);
                At<uint32_t>(data, 0x234) = ((options[7] != 0) ? 8u : 0u) | (At<uint32_t>(data, 0x234) & 0xfffffff5)
                    | ((options[2] != 0) ? 2u : 0u);
            }
        }
        if (level > 0xafff) {
            bool shared = ((At<uint32_t>(data, 0x234) >> 3) & 1) != 0;
            DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal = {};
            if (probe->m_adapter3 != nullptr) {
                probe->m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
                probe->m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
            }
            uint64_t memory[6];
            DxgiQueryVideoMemory(this->m_dxgi, memory, static_cast<IDXGIAdapter1*>(probe->m_adapter1), &local, &nonLocal, shared);
            float megabytes = static_cast<float>(static_cast<uint32_t>(memory[0] >> 0x14)) * 0.25f;  // DAT_1426193c4
            uint64_t budget = static_cast<uint64_t>(megabytes) * 0x100000;
            if (budget < 0x8000000) {
                At<uint32_t>(data, 0x14c) = 0x8000000;
            } else if (budget < 0x100000000ull) {
                At<uint32_t>(data, 0x14c) = static_cast<uint32_t>(budget);
            } else {
                At<uint32_t>(data, 0x14c) = 0xffffffff;
            }
        }
        At<uint32_t>(data, 0x170) = At<uint32_t>(data, 0x14c);
        At<uint32_t>(data, 0x174) = At<uint32_t>(data, 0x14c);
        uint32_t flag = At<uint8_t>(data, 0x227) != 0 ? 0x80 : 0;
        At<uint32_t>(data, 0x234) = flag | (At<uint32_t>(data, 0x234) & 0xffffff7f);
        probe->m_creator->FillDeviceData(data + 0x10);
    }
};

}
static_assert(offsetof(Dx11Backend, m_module) == 0xc0, "Instance::GetPlatformHandles reads the module at +0xc0");

// ref: FUN_142312eb0
Backend* CreateDx11Backend(Instance* instance) {
    void* memory = instance->m_allocator->allocate(sizeof(Dx11Backend), 8);
    return memory != nullptr ? new (memory) Dx11Backend(instance) : nullptr;
}

}
