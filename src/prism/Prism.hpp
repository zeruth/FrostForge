#ifndef PRISM_PRISM_HPP
#define PRISM_PRISM_HPP

// Prism (pm_api): Blizzard's GPU abstraction under Gx. An instance owns the platform's GPU list
// (pm_dxgi on Windows) and one backend per API (pm_dx11, pm_dx12); devices, swap chains and
// command lists come from the backends. Every call is virtual; the slot order of each class
// below is the reference's.

#include "blz/Allocator.hpp"
#include <cstddef>
#include <cstdint>

namespace pm {

// A call's outcome (8 bytes): a code (0 success, 1 failed or incomplete, 2 out of memory,
// 3 device lost), its category (4: from an HRESULT), and the HRESULT.
struct RESULT {
    uint8_t m_code;                 // 0x00
    uint8_t m_category;             // 0x01
    uint16_t m_pad;
    int32_t m_hresult;              // 0x04
};
static_assert(sizeof(RESULT) == 8, "pm::RESULT is 8 bytes");

inline RESULT Ok() {
    return RESULT { 0, 0, 0, 0 };
}

// The allocator the instance is created with is a blz::allocator.
typedef blz::allocator Allocator;

// Where the instance reports: slot 0x08 receives (severity, function, result, flags, text).
class Messenger {
    public:
    virtual ~Messenger() {}
    virtual void Report(uint8_t severity, const char* function, RESULT result, uint32_t flags, const char* text) = 0;
};

// pmCreateInstance's parameters (0x110 bytes).
struct INSTANCEINFO {
    Allocator* m_allocator;         // 0x00: null for the default
    Messenger* m_messenger;         // 0x08: null for the default
    uint8_t m_10;                   // 0x10: 2
    uint8_t m_pad11[7];
    uint32_t m_18;                  // 0x18: the settings block (GetSettings) starts here
    uint32_t m_cpuValidation;       // 0x1c
    uint32_t m_gpuValidation;       // 0x20
    uint32_t m_24;                  // 0x24
    uint32_t m_28;                  // 0x28: 0xf
    uint8_t m_2c[0x54];             // 0x2c
    const char* m_appName;          // 0x80
    uint32_t m_appVersion;          // 0x88
    uint32_t m_pad8c;
    const char* m_engineName;       // 0x90: "Prism" when null
    uint32_t m_engineVersion;       // 0x98
    uint32_t m_9c;
    uint32_t m_a0;                  // 0xa0: 8
    uint8_t m_a4;                   // 0xa4: 2
    uint8_t m_pada5[3];
    void* m_a8;                     // 0xa8
    uint8_t m_b0[0x18];             // 0xb0
    void* m_presentExceptionHandler;  // 0xc8: (code, info)
    uint8_t m_d0[0x20];             // 0xd0
    uint32_t m_f0;                  // 0xf0: 0x800000
    uint8_t m_f4[4];
    uint8_t m_f8;                   // 0xf8: 1
    uint8_t m_f9[0x13];
    uint32_t m_10c;                 // 0x10c: 0x20000
};
static_assert(offsetof(INSTANCEINFO, m_appName) == 0x80, "INSTANCEINFO layout");
static_assert(offsetof(INSTANCEINFO, m_presentExceptionHandler) == 0xc8, "INSTANCEINFO layout");
static_assert(sizeof(INSTANCEINFO) == 0x110, "INSTANCEINFO is 0x110 bytes");

// The APIs a backend can be (pmApi): 2 D3D11, 3 D3D12.
enum : uint8_t {
    API_D3D11 = 2,
    API_D3D12 = 3,
    API_COUNT = 9,
};

// The GPU types (pmGpuType): bit n of an enumeration mask admits type n.
enum : uint8_t {
    GPU_UNKNOWN = 0,
    GPU_SOFTWARE = 1,
    GPU_INTEGRATED = 2,
    GPU_DISCRETE = 3,
    GPU_EXTERNAL = 4,
};

class Instance;
struct MONITOR;

// One GPU (0x150 bytes, pm_api::Gpu with pm_dxgi's data); m_size marks the layout.
struct GPU {
    const char* m_name;             // 0x000: points at m_nameBuffer
    uint64_t m_driverVersion;       // 0x008: the user-mode driver version (CheckInterfaceSupport)
    uint16_t m_vendorId;            // 0x010
    uint16_t m_deviceId;            // 0x012
    uint32_t m_subSysId;            // 0x014
    uint16_t m_revision;            // 0x018
    uint8_t m_type;                 // 0x01a: GPU_*
    uint8_t m_virtualization;       // 0x01b
    uint8_t m_1c[0x14];             // 0x01c
    uint64_t m_luid;                // 0x030
    uint32_t m_index;               // 0x038
    uint32_t m_location;            // 0x03c: the PCI location (SetupAPI)
    uint8_t m_40[0xc];              // 0x040
    uint8_t m_flags;                // 0x04c: 1 an AMD integrated part ("/ AMD Graphics")
    uint8_t m_4d[3];
    uint32_t m_size;                // 0x050: 0x150
    char m_nameBuffer[0x80];        // 0x054
    uint8_t m_d4[4];
    void* m_adapter1;               // 0x0d8: IDXGIAdapter1
    void* m_e0[4];                  // 0x0e0: further adapter interfaces
    Allocator* m_allocator;         // 0x100
    MONITOR** m_monitors;           // 0x108
    uint32_t m_monitorCount;        // 0x110
    uint32_t m_monitorCapacity;     // 0x114
    uint64_t m_118;                 // 0x118
    void* m_vtable120;              // 0x120: 0x1429244e8
    void* m_extension[2];           // 0x128: 0x1429267c8 over the instance's AGS context
    void* m_vtable138;              // 0x138: 0x142924508
    void* m_vtable140;              // 0x140: 0x142924528
    uint64_t m_148;                 // 0x148
};
static_assert(offsetof(GPU, m_size) == 0x50, "GPU layout");
static_assert(offsetof(GPU, m_adapter1) == 0xd8, "GPU layout");
static_assert(offsetof(GPU, m_monitors) == 0x108, "GPU layout");
static_assert(sizeof(GPU) == 0x150, "GPU is 0x150 bytes");

// Creates the platform instance and initializes it (FUN_1423109f0, pmCreateInstance).
RESULT CreateInstance(const INSTANCEINFO* info, Instance** instance);
void DestroyInstance(Instance* instance);

// Formats and reports through the messenger, and stores `result` (FUN_142310800).
class Object;
RESULT* ReportError(Object* object, RESULT* out, uint8_t severity, const char* function, RESULT result,
    const char* format, ...);

// A Win32 error or HRESULT as a result (pm_dxgi's FUN_142353760, pm_dx12's FUN_1423459a0).
RESULT ResultFromHResult(int32_t hresult);
RESULT ResultFromHResultDx12(int32_t hresult);

const char* ApiName(uint8_t api);           // FUN_142359190
const char* GpuTypeName(uint8_t type);      // FUN_142359210
const char* AmdFamilyName(uint8_t family);  // FUN_142359170
const char* NvidiaFamilyName(uint8_t family);  // FUN_142359290
const char* IntelFamilyName(uint8_t family);   // FUN_142359250
const char* ResultCodeName(uint8_t code);      // FUN_142359150
const char* ResultSourceName(uint8_t category);  // FUN_1423591d0

}

#endif
