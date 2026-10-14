#ifndef PRISM_INSTANCE_HPP
#define PRISM_INSTANCE_HPP

// pm_api::Instance and pm_api::PlatformInstance (Windows): the GPU list, one backend per API,
// and the per-GPU device data each backend fills.

#include "prism/Dxgi.hpp"
#include "prism/Prism.hpp"
#include <cstddef>
#include <cstdint>

namespace pm {

class Backend;

// Every Prism object starts with this (0x20 bytes, vtable 0x142821338): its owner, and the
// allocator and messenger it was made with.
class Object {
    public:
    void* m_owner;                  // 0x08
    Allocator* m_allocator;         // 0x10
    Messenger* m_messenger;         // 0x18

    virtual ~Object() {}
};

// A GPU's data for one API (pm_api::DeviceData, 0x228 bytes): what slot 0x78 returns. The
// backend fills it the first time it is asked for. Not DEVICEDATA: wingdi.h defines that name.
struct APIDEVICEDATA {
    uint8_t m_api;                  // 0x00
    uint8_t m_ready;                // 0x01: the backend made a device and filled the rest
    uint8_t m_ids[8];               // 0x02: the GPU's vendor, device and subsystem ids
    uint16_t m_revision;            // 0x0a
    uint8_t m_0c[0x34];
    uint8_t m_amdFamily;            // 0x40
    uint8_t m_41[0x37];
    uint8_t m_intelGeneration;      // 0x78: then the driver's device information
    uint8_t m_79[0x17];
    uint8_t m_nvidiaFamily;         // 0x90
    uint8_t m_91[0xf];
    uint32_t m_appleGpuCores;       // 0xa0: Apple silicon's GPU core count
    uint8_t m_a4[8];
    uint32_t m_osClass;             // 0xac
    uint32_t m_osBuild;             // 0xb0
    uint8_t m_b4[8];
    uint32_t m_featureLevel;        // 0xbc: D3D_FEATURE_LEVEL
    uint8_t m_c0[0x19];
    uint8_t m_d9;                   // 0xd9: below 4 caps a discrete part's tier at 1
    uint8_t m_da[0x13c];
    uint8_t m_216;                  // 0x216
    uint8_t m_217[0x11];
};
static_assert(offsetof(APIDEVICEDATA, m_osClass) == 0xac, "APIDEVICEDATA layout");
static_assert(offsetof(APIDEVICEDATA, m_appleGpuCores) == 0xa0, "APIDEVICEDATA layout");
static_assert(offsetof(APIDEVICEDATA, m_d9) == 0xd9, "APIDEVICEDATA layout");
static_assert(offsetof(APIDEVICEDATA, m_featureLevel) == 0xbc, "APIDEVICEDATA layout");
static_assert(sizeof(APIDEVICEDATA) == 0x228, "APIDEVICEDATA is 0x228 bytes");

// The value the device map keeps per GPU (0x238 bytes): the GPU's type and LUID, so the data
// can be matched to a new GPU object after the list is rebuilt, then the data. The backends
// fill through this, so their offsets are 0x10 past APIDEVICEDATA's.
struct DEVICEVALUE {
    uint8_t m_gpuType;              // 0x00
    uint8_t m_01[7];
    uint64_t m_luid;                // 0x08
    APIDEVICEDATA m_data;           // 0x10
};
static_assert(sizeof(DEVICEVALUE) == 0x238, "DEVICEVALUE is 0x238 bytes");

// blz::chained_hash_node<pair<Gpu const* const, DeviceData>> (0x268 bytes).
struct DEVICENODE {
    DEVICENODE* m_next;             // 0x00
    GPU* m_key;                     // 0x08
    DEVICEVALUE m_value;            // 0x10
    uint64_t m_devicesCapacity;     // 0x248: blz::chained_hash_set<pmDevice*>
    void** m_devices;               // 0x250
    uint64_t m_deviceCount;         // 0x258
    float m_devicesMaxLoad;         // 0x260
};
static_assert(offsetof(DEVICENODE, m_devicesCapacity) == 0x248, "DEVICENODE layout");
static_assert(sizeof(DEVICENODE) == 0x268, "DEVICENODE is 0x268 bytes");

// The platform objects behind the instance, for the engine's own use (slot 0x30, 0x60 bytes):
// the DXGI and D3D modules and factory, and the vendor libraries' contexts.
struct PLATFORMHANDLES {
    void* m_dxgiModule;             // 0x00
    void* m_factory;                // 0x08: IDXGIFactory1
    void* m_d3d11Module;            // 0x10: when the D3D11 backend is up
    void* m_d3d12Module;            // 0x18: when the D3D12 backend is up
    void* m_20[3];
    void* m_agsContext;             // 0x38
    void* m_agsGpuInfo;             // 0x40: points at the AGS context's GPU info
    uint8_t m_nvapi;                // 0x48
    uint8_t m_49[7];
    void* m_50;
    void* m_renderDoc;              // 0x58: the RenderDoc API, when loaded
};
static_assert(sizeof(PLATFORMHANDLES) == 0x60, "PLATFORMHANDLES is 0x60 bytes");

// One API's backend and its device data (0x30 bytes, nine from +0x130).
struct BACKENDSLOT {
    RESULT m_state;                 // 0x00: the backend's Init result
    Backend* m_backend;             // 0x08
    uint64_t m_capacity;            // 0x10: the device-data map
    DEVICENODE** m_buckets;         // 0x18
    uint64_t m_count;               // 0x20
    float m_maxLoad;                // 0x28
};
static_assert(sizeof(BACKENDSLOT) == 0x30, "BACKENDSLOT is 0x30 bytes");

// The AGS driver extension context (0x30 bytes at +0x300).
struct AGSCONTEXT {
    Allocator* m_allocator;
    void* m_context;
    void* m_gpuInfo;
    void* m_18[3];
};

// vtable 0x142924298 (pm_api::Instance), 0x1429241e0 (PlatformInstance); 0x448 bytes.
class Instance : public Object {
    public:
    INSTANCEINFO m_info;            // 0x020
    BACKENDSLOT m_backends[API_COUNT];  // 0x130
    Allocator* m_gpuAllocator;      // 0x2e0
    GPU** m_gpus;                   // 0x2e8
    uint32_t m_gpuCount;            // 0x2f0
    uint32_t m_gpuCapacity;         // 0x2f4
    uint32_t m_osClass;             // 0x2f8: 1 pre-Vista .. 5 Windows 8+/10, 6 newer
    uint32_t m_osBuild;             // 0x2fc
    AGSCONTEXT m_ags;               // 0x300
    uint8_t m_nvapi;                // 0x330
    uint8_t m_pad331[7];
    DxgiInstance m_dxgi;            // 0x338

    Instance(const INSTANCEINFO* info);
    virtual ~Instance();                                                        // 0x00
    virtual RESULT* Init(RESULT* out, const INSTANCEINFO* info);                // 0x08
    virtual RESULT* CreateDevice(RESULT* out, const void* params, void* device);  // 0x10
    virtual void DestroyDevice(void* device);                                   // 0x18
    virtual RESULT* Rebuild(RESULT* out, uint8_t a2, uint8_t a3);              // 0x20
    virtual bool IsCurrent();                                                   // 0x28: the GPUs are unchanged
    virtual PLATFORMHANDLES* GetPlatformHandles(PLATFORMHANDLES* out);          // 0x30
    virtual void* GetSlot38(void* out, GPU* gpu);                               // 0x38
    virtual RESULT* EnumerateGpus(RESULT* out, uint32_t* count, GPU** gpus, uint16_t typeMask);  // 0x40
    virtual RESULT* EnumerateMonitors(RESULT* out, GPU* gpu, uint32_t* count, void** monitors);  // 0x48
    virtual RESULT* EnumerateModes(RESULT* out, void* monitor, uint32_t* count, void** modes);    // 0x50
    virtual void Slot58(void* a2, uint8_t api);                                 // 0x58
    virtual void* GetMode(void* mode);                                          // 0x60
    virtual void* GetMonitor(void* monitor);                                    // 0x68
    virtual GPU* GetGpu(GPU* gpu);                                              // 0x70
    virtual APIDEVICEDATA* GetDeviceData(GPU* gpu, uint8_t api);                   // 0x78
    virtual void* Slot80(void* out, void* monitor);                            // 0x80
    virtual void* GetSettings();                                                // 0x88
    virtual RESULT* InitPlatform(RESULT* out, const INSTANCEINFO* info);        // 0x90
    virtual void ShutdownPlatform();                                            // 0x98
    virtual RESULT* InitGpus(RESULT* out, uint8_t a2, uint8_t a3, uint8_t a4);  // 0xa0
    virtual void ReportOsVersion(void* out);                                    // 0xa8

    // The API's backend, initialized on first use; null when the API is not available.
    Backend* GetBackend(uint8_t api);                                           // FUN_14234ed50
    DEVICEVALUE* FindDeviceNode(GPU* gpu, uint8_t api);                         // FUN_14234ee90
    DEVICEVALUE* AddDeviceNode(GPU* gpu, uint8_t api);                          // FUN_14234e930
    RESULT* RebuildDeviceList(RESULT* out);                                     // FUN_14234f180
};

Backend* CreateDx11Backend(Instance* instance);   // FUN_142312eb0
Backend* CreateDx12Backend(Instance* instance);   // FUN_14232ba50
static_assert(offsetof(Instance, m_info) == 0x20, "Instance layout");
static_assert(offsetof(Instance, m_backends) == 0x130, "Instance layout");
static_assert(offsetof(Instance, m_gpus) == 0x2e8, "Instance layout");
static_assert(offsetof(Instance, m_ags) == 0x300, "Instance layout");
static_assert(offsetof(Instance, m_dxgi) == 0x338, "Instance layout");

// A backend (pm_dx11::Instance, pm_dx12::Instance); slot 0x08 initializes it, 0x20 fills a
// GPU's device data, 0x28 rebuilds its device list.
class Backend : public Object {
    public:
    virtual RESULT* Init(RESULT* out, const INSTANCEINFO* info) = 0;            // 0x08
    virtual RESULT* CreateDevice(RESULT* out, GPU* gpu, const void* params, void* device, DEVICEVALUE* value) = 0;  // 0x10
    virtual void Slot18() = 0;                                                  // 0x18
    virtual void FillDeviceData(GPU* gpu, DEVICEVALUE* value) = 0;              // 0x20
    virtual RESULT* BuildDeviceList(RESULT* out) = 0;                           // 0x28
    virtual void RebindDevice(void* data, GPU* gpu) = 0;                        // 0x30
    virtual void Slot38(void* a2) = 0;                                          // 0x38
};

}

#endif
