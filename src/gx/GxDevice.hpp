#ifndef GX_GX_DEVICE_HPP
#define GX_GX_DEVICE_HPP

// CGxDevice (Engine/Source/Gx/src/GxDevice.cpp, vtable 0x1429131e0): the engine's device. One
// lives at a time, built in place in Gx's state block (DAT_14348b680) by the API's factory.
// The virtual slots follow the reference vtable's order; slots not yet ported keep a
// placeholder name. Members are read and written at their reference offsets (Field<T>), as
// the object is 0x4750 bytes of mostly API-specific state.

#include "blz/Allocator.hpp"
#include "blz/Function.hpp"
#include "gx/Gx.hpp"
#include <windows.h>
#include <cstddef>
#include <cstdint>

// The size and alignment a texture takes in a heap (device slot 0x1f8).
struct GXTEXALLOCINFO {
    uint64_t m_size;
    uint32_t m_alignment;
    uint32_t m_pad;
};

// What the window asks a swap chain for.
struct GXSWAPCHAINPARAMS {
    HWND m_window;                  // 0x00
    uint16_t m_width;               // 0x08: 0 for the client size
    uint16_t m_height;              // 0x0a
    uint32_t m_format;              // 0x0c
    uint8_t m_vsync;                // 0x10
    uint8_t m_primary;              // 0x11
};

// What every backend's swap chain starts with.
struct GXSWAPCHAIN {
    void* m_window;                 // 0x00
    uint16_t m_width;               // 0x08
    uint16_t m_height;              // 0x0a
    uint16_t m_requestWidth;        // 0x0c
    uint16_t m_requestHeight;       // 0x0e
    uint8_t m_format;               // 0x10
};

// A swap chain's frame: begin (FUN_14228c090), its back buffer (FUN_1422885f0), present
// (FUN_14228c140) or skip the present (FUN_14228c100), and end (FUN_14228c120).
bool GxSwapChainBegin(void* chain);
void* GxSwapChainGetBackBuffer(void* chain);
void GxSwapChainPresent(void* chain);
void GxSwapChainSkip(void* chain);
void GxSwapChainEnd(void* chain);

// Creates a swap chain on the device (FUN_14228c0b0), and destroys one, clearing the
// pointer (FUN_14228c0d0).
void* GxSwapChainCreate(const GXSWAPCHAINPARAMS* params);
void GxSwapChainDestroy(void** chain);

struct GxBuf;
struct GxPipeline;
struct GXPIPELINEDESC;
struct GXVERTEXBUFFERFORMAT;
struct GXVERTEXBUFFERFORMATDESC;

class CGxDevice {
    public:
    explicit CGxDevice(uint8_t api);

    virtual uint8_t Slot000();                              // 0x00
    virtual void Slot008();                                 // 0x08
    virtual void Slot010();                                 // 0x10
    virtual ~CGxDevice();                                   // 0x18
    virtual void Slot020();                                 // 0x20
    virtual bool DeviceCreate(const GXDEVICEPARAMS* params);  // 0x28
    virtual void Slot030();                                 // 0x30
    virtual void DeviceShutdown();                          // 0x38
    virtual bool FormatSupportsMultisample(uint8_t format, uint8_t samples, int32_t quality);  // 0x40
    virtual bool RebindAdapter();                           // 0x48: after the adapter list is rebuilt
    virtual uint8_t Slot050();                              // 0x50
    // Runs `callback` once the GPU is done with the frame being recorded.
    virtual void DeferCallback(blz::function_storage* callback);  // 0x58
    virtual void Slot060();                                 // 0x60
    virtual void Slot068();                                 // 0x68
    virtual void Slot070();                                 // 0x70
    virtual void SetLowLatencyMode(uint8_t mode);           // 0x78
    virtual void FrameMarker();                             // 0x80: after the frame's submission
    virtual void LatencyMarker(uint32_t marker, uint32_t frame);  // 0x88
    virtual void* Slot090(void* out, uint64_t a2); // 0x90 (pure in the reference's base)
    virtual void Slot098();                                 // 0x98
    virtual void Slot0a0();                                 // 0xa0
    virtual void Slot0a8();                                 // 0xa8
    virtual void Slot0b0();                                 // 0xb0
    virtual void Slot0b8();                                 // 0xb8
    virtual void Slot0c0();                                 // 0xc0
    virtual void Slot0c8();                                 // 0xc8
    virtual void Slot0d0();                                 // 0xd0
    virtual void* Slot0d8(void* out, uint64_t a2); // 0xd8 (pure in the reference's base)
    virtual void Slot0e0();                                 // 0xe0
    virtual void Slot0e8();                                 // 0xe8
    virtual void* Slot0f0(void* out);            // 0xf0 (pure in the reference's base)
    virtual void Slot0f8();                                 // 0xf8
    virtual void* SwapChainCreate(const GXSWAPCHAINPARAMS* params);  // 0x100
    virtual void SwapChainDestroy(void* chain);             // 0x108
    virtual bool SwapChainBegin(void* chain);               // 0x110
    virtual void* SwapChainGetBackBuffer(void* chain);      // 0x118
    virtual void SwapChainPresent(void* chain);             // 0x120
    virtual void Slot128(void* chain);                      // 0x128
    virtual void SwapChainEnd(void* chain);                 // 0x130
    // Builds a pipeline (in the background when the device can); `hash` keys it.
    virtual GxPipeline* PipelineCreate(const GXPIPELINEDESC* desc, uint64_t hash, const char* name);  // 0x138
    virtual void PipelineDestroy(GxPipeline* pipeline);    // 0x140
    // A stream layout for the device; *result is nonzero on failure.
    virtual uint16_t* CreateVertexBufferFormat(uint16_t* result, const GXVERTEXBUFFERFORMATDESC* desc,
        GXVERTEXBUFFERFORMAT** format);                     // 0x148
    virtual void ObjectFree(void* object);                  // 0x150: back to Gx's allocator
    virtual void Slot158();                                 // 0x158
    // The swap chain the current frame draws into (between SwapChainBegin and SwapChainEnd).
    virtual void* CurrentSwapChain();                       // 0x160
    virtual void Slot168();                                 // 0x168
    virtual bool VariableRateShadingSupported(uint8_t tier);  // 0x170
    virtual bool ShadingRateSupported(uint32_t rate, int32_t a3, uint8_t a4);  // 0x178
    virtual bool FrameBegin(void* query);                   // 0x180
    virtual void Slot188();                                 // 0x188
    virtual void* FrameEnd(void* out, void* query);         // 0x190: submits; out is the wait
    virtual uint32_t FrameCount();                          // 0x198: frames in flight
    virtual bool BufCreate(GxBuf* buf);                     // 0x1a0: the API side of a buffer
    virtual void BufDestroy(GxBuf* buf);                    // 0x1a8
    // a 0x20-byte description copied to the heap, and given back (pure in the reference's base)
    virtual void* Slot1b0(const void* desc);                // 0x1b0
    virtual void Slot1b8(void* object);                     // 0x1b8
    virtual void* Slot1c0(const void* desc);     // 0x1c0 (pure in the reference's base)
    virtual void Slot1c8();                                 // 0x1c8
    virtual void Slot1d0();                                 // 0x1d0
    virtual void Slot1d8();                                 // 0x1d8
    virtual void Slot1e0();                                 // 0x1e0
    virtual bool TexHeapCreate(const void* desc, void** heap);  // 0x1e8
    virtual void TexHeapDestroy(void* heap);                // 0x1f0
    virtual GXTEXALLOCINFO* TexAllocationInfo(GXTEXALLOCINFO* out, const void* desc);  // 0x1f8
    virtual bool TexCreate(const void* desc, void** tex);   // 0x200
    virtual void TexDestroy(void* tex);                     // 0x208
    virtual void* DefaultTex();                             // 0x210
    virtual void Slot218();                                 // 0x218
    virtual void Slot220();                                 // 0x220
    virtual void Slot228();                                 // 0x228
    virtual void Slot230();                                 // 0x230
    virtual void Slot238();                                 // 0x238
    virtual void Slot240();                                 // 0x240
    virtual void Slot248();                                 // 0x248
    virtual void* TexShaderView(void* tex, uint32_t plane); // 0x250
    virtual void* TexUnorderedView(void* tex);              // 0x258
    virtual void* TexTargetView(void* tex);                 // 0x260
    virtual void Slot268();                                 // 0x268
    virtual void Slot270();                                 // 0x270
    virtual void* QueryCreate(uint8_t type);                // 0x278
    virtual void QueryDestroy(void* query);                 // 0x280
    virtual bool QueryInfo(void* query, uint8_t what, uint32_t* out);  // 0x288
    virtual bool QueryGetResult(void* query, void* out, uint32_t size);  // 0x290
    // a compute shader object (0x38) by kind and permutation, and its release (pure in the base)
    virtual void* CreateComputeShader(uint32_t kind, uint16_t permutation);  // 0x298
    virtual void DestroyComputeShader(void* shader);        // 0x2a0

    // Re-creates the vertex formats and the registered shaders on a new device (FUN_142290d00).
    void ReregisterObjects();
    // The two stream layouts of each of the 0x2b vertex formats (FUN_142291c90).
    void CreateVertexFormats();
    // A vertex format's layouts: {&layouts[format], 1 when slot 0 exists, 2 when both}
    // (FUN_142291880).
    struct VERTEXFORMATREF {
        GXVERTEXBUFFERFORMAT** m_layouts;
        uint32_t m_state;
    };
    VERTEXFORMATREF* GetVertexFormat(VERTEXFORMATREF* out, uint8_t format);

    // Releases the registered shaders and frees the per-device format objects (FUN_142290f60).
    void ReleaseRegisteredObjects();

    // Takes the console's device settings (FUN_1422910d0).
    bool SetParams(const GXDEVICEPARAMS* params);

    // A member at its reference offset.
    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offset);
    }

    // The common part (0x470 bytes) as named members.
    uint8_t m_api;                  // 0x08: GXAPI_*
    uint8_t m_ready;                // 0x09: created and usable (GxDevExists)
    uint8_t m_0a;                   // 0x0a
    uint8_t m_pad0b[5];
    pm::GPU* m_gpu;                 // 0x10
    pm::APIDEVICEDATA* m_data;      // 0x18
    GXDEVICEPARAMS m_params;        // 0x20
    uint8_t m_80[0x3f0];            // 0x80: the limits, capabilities and caches (Field<T>)
};
static_assert(offsetof(CGxDevice, m_api) == 0x08, "CGxDevice layout");
static_assert(offsetof(CGxDevice, m_params) == 0x20, "CGxDevice layout");
static_assert(sizeof(CGxDevice) == 0x470, "CGxDevice is 0x470 bytes");

// Gx's allocator: the one it hands Prism (FUN_14228a070).
blz::allocator* GxGetAllocator();

// The buffers registered for re-creation across devices (PTR_LOOP_142d43850): an intrusive
// ring whose links sit 0x10 bytes into each GxBuf, under DAT_143490a00.
struct GXREGISTRYLINK {
    GXREGISTRYLINK* m_next;
    GXREGISTRYLINK* m_prev;
};
extern GXREGISTRYLINK g_gxRegisteredBufs;
extern SRWLOCK g_gxRegisteredBufsLock;

// Builds the D3D12 device in place in `block` (FUN_1422a8d70).
CGxDevice* GxDeviceD3d12Create(void* block);

#endif
