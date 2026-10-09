#ifndef PRISM_DXGI_HPP
#define PRISM_DXGI_HPP

// pm_dxgi::Instance: the DXGI factory, and the GPU list built from it (with SetupAPI's view
// of each display device for the bus location and type).

#include "prism/Prism.hpp"
#include <cstddef>
#include <cstdint>

struct IDXGIAdapter1;
struct DXGI_QUERY_VIDEO_MEMORY_INFO;

namespace pm {

// A display mode (0x30 bytes): the summary Prism hands out, then DXGI's own description.
struct MODE {
    uint32_t m_width;               // 0x00
    uint32_t m_height;              // 0x04
    uint32_t m_refreshNumerator;    // 0x08
    uint32_t m_refreshDenominator;  // 0x0c: 1 when DXGI gave 0
    uint8_t m_format;               // 0x10: pmFormat
    uint8_t m_pad11[3];
    uint8_t m_desc[0x1c];           // 0x14: DXGI_MODE_DESC
};
static_assert(sizeof(MODE) == 0x30, "MODE is 0x30 bytes");

// A monitor (0xc0 bytes).
struct MONITOR {
    const char* m_name;             // 0x00: points at m_nameBuffer
    int32_t m_left;                 // 0x08
    int32_t m_top;                  // 0x0c
    int32_t m_width;                // 0x10
    int32_t m_height;               // 0x14
    void* m_monitor;                // 0x18: HMONITOR
    uint8_t m_flags;                // 0x20: 2 HDR (10-bit modes listed)
    uint8_t m_pad21[7];
    char m_nameBuffer[0x80];        // 0x28
    Allocator* m_modeAllocator;     // 0xa8
    MODE** m_modes;                 // 0xb0
    uint32_t m_modeCount;           // 0xb8
    uint32_t m_modeCapacity;        // 0xbc
};
static_assert(sizeof(MONITOR) == 0xc0, "MONITOR is 0xc0 bytes");

class Instance;

// vtable 0x142924548; 0x110 bytes at instance +0x338.
struct DxgiInstance {
    void* m_vtable;                 // 0x00
    void* m_owner;                  // 0x08
    Allocator* m_allocator;         // 0x10
    Messenger* m_messenger;         // 0x18
    Instance* m_instance;           // 0x20
    Allocator* m_gpuAllocator;      // 0x28
    GPU** m_gpus;                   // 0x30
    uint32_t m_gpuCount;            // 0x38
    uint32_t m_gpuCapacity;         // 0x3c
    uint8_t m_initialized;          // 0x40
    uint8_t m_pad41[7];
    void* m_factory1;               // 0x48: IDXGIFactory1
    void* m_factory2;               // 0x50
    void* m_factory3;               // 0x58
    void* m_factory4;               // 0x60
    void* m_factory5;               // 0x68
    void* m_factory6;               // 0x70
    uint32_t m_78;                  // 0x78
    uint32_t m_factoryFlags;        // 0x7c: DXGI_CREATE_FACTORY_DEBUG with CPU validation
    uint8_t m_allowTearing;         // 0x80
    uint8_t m_pad81[7];
    void* m_dxgiModule;             // 0x88
    void* m_createFactory1;         // 0x90
    void* m_createFactory2;         // 0x98
    void* m_getDebugInterface1;     // 0xa0
    void* m_declareAdapterRemovalSupport;  // 0xa8
    void* m_setupApi;               // 0xb0
    void* m_setupDiGetClassDevsW;   // 0xb8
    void* m_setupDiDestroyDeviceInfoList;  // 0xc0
    void* m_setupDiEnumDeviceInfo;  // 0xc8
    void* m_setupDiGetDevicePropertyW;     // 0xd0
    void* m_presentExceptionHandler;  // 0xd8
    uint8_t m_e0;                   // 0xe0
    uint8_t m_pade1[0x27];
    void* m_infoQueue;              // 0x108: IDXGIInfoQueue
};
static_assert(offsetof(DxgiInstance, m_factory1) == 0x48, "DxgiInstance layout");
static_assert(offsetof(DxgiInstance, m_dxgiModule) == 0x88, "DxgiInstance layout");
static_assert(offsetof(DxgiInstance, m_infoQueue) == 0x108, "DxgiInstance layout");
static_assert(sizeof(DxgiInstance) == 0x110, "DxgiInstance is 0x110 bytes");

void DxgiConstruct(DxgiInstance* dxgi, Instance* instance);                // FUN_142350c20
RESULT* DxgiInit(DxgiInstance* dxgi, RESULT* out, const INSTANCEINFO* info);  // FUN_142352080
RESULT* DxgiInitGpus(DxgiInstance* dxgi, RESULT* out, uint8_t preference);    // FUN_142352390
void DxgiShutdown(DxgiInstance* dxgi);                                     // FUN_142350e40
void DxgiSetupInfoQueue(DxgiInstance* dxgi);                               // FUN_142351110
void DxgiFillGpu(DxgiInstance* dxgi, GPU* gpu);                            // FUN_142353a10
// The GPU's memory (local and non-local: size, budget, usage), from DXGI's figures when it
// gave them and from the adapter description otherwise (FUN_142351980).
void DxgiQueryVideoMemory(DxgiInstance* dxgi, uint64_t out[6], IDXGIAdapter1* adapter,
    const DXGI_QUERY_VIDEO_MEMORY_INFO* local, const DXGI_QUERY_VIDEO_MEMORY_INFO* nonLocal, bool integrated);

}

#endif
