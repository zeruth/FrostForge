#ifndef GX_D3D12_DEVICE_D3D12_HPP
#define GX_D3D12_DEVICE_D3D12_HPP

// CGxDeviceD3d12 (Engine/Source/Gx/src/D3d12/GxDeviceD3d12.cpp, vtable 0x142915fd0, 0x4750
// bytes): the native D3D12 device, built in place in Gx's state block. gxDx12Backend::Device
// (vtable 0x14291a3b8) is the same object before the API class's vtable goes in.

#include "gx/GxDevice.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "gx/d3d12/Objects.hpp"
#include "gx/d3d12/Residency.hpp"
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace gxDx12Backend {

extern const char s_swapChainPtrTag[];      // ".PEAUSwapChain@gxDx12Backend@@"
extern const char s_enqueuePairTag[];       // ".?AUEnqueuePair@Device@gxDx12Backend@@"
extern const char s_commandListPtrTag[];    // ".PEAUID3D12CommandList@@"
extern const char s_copyListPtrTag[];       // ".PEAUCopyCommandList@gxDx12Backend@@"
extern const char s_heapPageTag[];          // ".?AV?$unique_ptr@UHeapPage@gxDx12Backend@@..."
extern const char s_samplerMapTag[];        // ".?AU?$pair@$$CB_KV?$unique_ptr@Usampler@Device@gxDx12Backend@@..."
extern const char s_stateMapTag[];          // ".?AU?$pair@QEBUD3d12Object@gxDx12Backend@@W4D3D12_RESOURCE_STATES@@@blz@@"

// A window's swap chain and its back buffers (0x798 bytes).
struct SwapChain {
    HWND m_window;                  // 0x000
    uint16_t m_width;               // 0x008
    uint16_t m_height;              // 0x00a
    uint16_t m_requestWidth;        // 0x00c: 0 for the window's client size
    uint16_t m_requestHeight;       // 0x00e
    uint8_t m_texFormat;            // 0x010: 2 (B8G8R8A8), 0x14 for the other formats
    uint8_t m_vsync;                // 0x011
    uint8_t m_primary;              // 0x012: the device's main swap chain
    uint8_t m_pad13[5];
    IDXGISwapChain3* m_swapChain;   // 0x018
    blz::embedded_vector<Tex, 3, s_texTag> m_buffers;  // 0x020
    ID3D12Resource* m_758;          // 0x758
    blz::embedded_vector<IUnknown*, 3, s_comPtrTag> m_760;  // 0x760
    uint16_t m_current;             // 0x790: the buffer being drawn
    uint8_t m_pad792[2];
    uint32_t m_dxgiFormat;          // 0x794
};
static_assert(offsetof(SwapChain, m_swapChain) == 0x18, "SwapChain layout");
static_assert(offsetof(SwapChain, m_buffers) == 0x20, "SwapChain layout");
static_assert(offsetof(SwapChain, m_758) == 0x758, "SwapChain layout");
static_assert(offsetof(SwapChain, m_current) == 0x790, "SwapChain layout");
static_assert(sizeof(SwapChain) == 0x798, "SwapChain is 0x798 bytes");

using SWAPCHAINPARAMS = ::GXSWAPCHAINPARAMS;

// A list waiting to be submitted and which of its lists (0x10 bytes).
struct EnqueuePair {
    CopyCommandList* m_list;
    uint32_t m_index;
    uint32_t m_pad;
};

// The graphics command list: the copy list's recording plus the binding state and the render
// context (0x2100 bytes at CGxDeviceD3d12+0xe80).
class GraphicsCommandList;

// The D3D12 render context (vtable 0x14291a188): each slot forwards to the command list it is
// embedded in, 0x1d80 bytes before it.
class RenderContextD3d12 : public GxRenderContext {
    public:
    GraphicsCommandList* Owner();

    void SetRenderTargets(GXRENDERTARGETS* targets) override;                  // FUN_1422d3270
    void Clear(GXRENDERTARGETS* targets, uint16_t mask, uint32_t color, float depth, uint32_t stencil,
        uint8_t a7) override;                                                   // FUN_1422d3010
    float* LockConstants(uint8_t stage, uint32_t slot, uint32_t size) override;  // FUN_1422e0030
    void UnlockConstants(uint8_t stage, uint32_t slot) override;               // FUN_1422e00c0
    bool CopyTex(Tex* source, Tex* dest, int32_t* sourceOrigin, int32_t* destOrigin, uint32_t* size,
        uint32_t slice, int32_t mip) override;                                  // FUN_1422e1870
    bool ResolveTex(Tex* source, Tex* dest) override;                          // FUN_1422e2d40
    void* BufLock(GxBuf* buf) override;                                        // FUN_1422cd8e0
    bool BufUnlock(GxBuf* buf, uint32_t size) override;                        // FUN_1422cd9f0
    GxBuf* GetUploadBuf(uint8_t type) override;                                // FUN_1422d40c0
    void SetPipeline(GxPipeline* pipeline) override;                           // FUN_1422dc9e0
    void SetTexture(uint8_t stage, uint32_t slot, Tex* tex) override;          // FUN_1422e4620
    void SetVertexBuffers(uint32_t first, uint32_t count, const GXVERTEXSTREAM* streams) override;  // FUN_1422dfe60
    void SetIndexBuffer(GxBuf* buf, uint8_t format) override;                  // FUN_1422dca20
    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance,
        uint8_t shadingRate) override;                                         // FUN_1422d1120
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, int32_t baseVertex,
        uint32_t startInstance, uint8_t shadingRate) override;                 // FUN_1422d11f0
};

class GraphicsCommandList {
    public:
    virtual void BeginQueryScope();         // FUN_1422ce740
    virtual void EndQueryScope();           // FUN_1422cea80

    CopyCommandList m_copy;                 // 0x0008
    void* m_uploadBufs[3];                  // 0x0ba0: GxBufs for the upload rings
    uint32_t m_boundTextures[3][0x40];      // 0x0bb8: by stage and slot
    uint32_t m_bb8_unused[0x40];            // 0x0eb8
    void* m_computeVtable;                  // 0x0fb8: the compute context (vtable 0x14291a070)
    uint64_t m_fc0;                         // 0x0fc0
    uint8_t m_pipelineDirty;                // 0x0fc8
    uint8_t m_heapsDirty;                   // 0x0fc9: the ring pages changed
    uint8_t m_topologyDirty;                // 0x0fca
    uint8_t m_queryScope;                   // 0x0fcb
    uint32_t m_fcc;                         // 0x0fcc
    DescriptorStagingHeap m_staging;        // 0x0fd0
    DescriptorUploadPage* m_ringPages[8];   // 0x1270: the page last bound per heap type
    void* m_views[4][0x40];                 // 0x12b0: bound shader views by stage and slot
    uint64_t m_1ab0[4][8];                  // 0x1ab0
    uint8_t m_1bb0[0xa0];                   // 0x1bb0
    uint8_t m_1c50[0xa0];                   // 0x1c50
    uint64_t m_1cf0[0xe];                   // 0x1cf0
    uint64_t m_1d60;                        // 0x1d60
    int64_t m_1d68;                         // 0x1d68
    uint64_t m_1d70[2];                     // 0x1d70
    RenderContextD3d12 m_context;           // 0x1d80
    uint64_t m_2080[2];                     // 0x2080
    GxBuf* m_indexBuf;                      // 0x2090
    uint32_t m_indexFormat;                 // 0x2098: DXGI_FORMAT_R16_UINT
    uint8_t m_targetsDirty;                 // 0x209c
    uint8_t m_pad209d[3];
    uint32_t m_rtvCount;                    // 0x20a0
    uint32_t m_pad20a4;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvs[4];  // 0x20a8
    uint64_t m_20c8;                        // 0x20c8
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsv;      // 0x20d0
    D3D12_CPU_DESCRIPTOR_HANDLE* m_dsvPtr;  // 0x20d8
    GxPipeline* m_pipeline;                 // 0x20e0
    uint8_t m_vrs;                          // 0x20e8
    uint8_t m_combiners[2];                 // 0x20e9: the shading-rate combiners set
    uint8_t m_wantCombiners[2];             // 0x20eb
    uint8_t m_shadingRate;                  // 0x20ed
    uint8_t m_20ee[2];
    uint32_t m_stencilRef;                  // 0x20f0
    uint32_t m_topology;                    // 0x20f4: D3D12_PRIMITIVE_TOPOLOGY
    uint64_t m_boundIndexAddress;           // 0x20f8

    GraphicsCommandList(uint32_t type, uint8_t vrs);   // FUN_1422cb470 over FUN_1422c9f00
    ~GraphicsCommandList();                 // FUN_1422cc000

    void BeginFrame();                      // FUN_1422d4120 (FUN_1422cec20 inside)
    void ResetBindings();                   // FUN_1422e4630
    void FlushBarriers();                   // FUN_1422ce780
    void ClearTargets(GXRENDERTARGETS* targets, uint32_t mask, const float* color, float depth, uint8_t stencil);  // FUN_1422d81c0
    void SetTargets(GXRENDERTARGETS* targets);  // FUN_1422d3090
    float* LockConstants(uint8_t stage, uint32_t slot, uint32_t size);  // FUN_1422dff60
    void UnlockConstants(uint8_t stage, uint32_t slot);  // FUN_1422e0040
    bool CopyTex(Tex* source, Tex* dest, int32_t* sourceOrigin, int32_t* destOrigin, uint32_t* size,
        uint32_t slice, int32_t mip);       // FUN_1422e14e0
    bool ResolveTex(Tex* source, Tex* dest);  // FUN_1422e2b20
    void CreateUploadBufs();                // the end of FUN_1422cb470
    // Room for `count` descriptors of a heap type in this frame's ring; a new page when the
    // current one is full, unless `keep`.
    DescriptorUploadPage* AllocateDescriptors(int32_t type, uint32_t count, bool keep);  // FUN_1422d9540
    // What the bound pipeline reads goes to the GPU: tables, heaps, root constants.
    void CommitBindings();                  // FUN_1422d5460
    // Everything a draw needs set on the list before it.
    void CommitState(bool indexed, uint8_t shadingRate);  // FUN_1422d9140
    // A draw that cannot happen still flushes the pending barriers.
    void SkipDraw();
    void SetTexture(uint8_t stage, uint32_t slot, Tex* tex);  // FUN_1422e43f0
};
static_assert(offsetof(GraphicsCommandList, m_copy) == 0x8, "GraphicsCommandList layout");
static_assert(offsetof(GraphicsCommandList, m_uploadBufs) == 0xba0, "GraphicsCommandList layout");
static_assert(offsetof(GraphicsCommandList, m_computeVtable) == 0xfb8, "GraphicsCommandList layout");
static_assert(offsetof(GraphicsCommandList, m_staging) == 0xfd0, "GraphicsCommandList layout");
static_assert(offsetof(GraphicsCommandList, m_views) == 0x12b0, "GraphicsCommandList layout");
static_assert(offsetof(GraphicsCommandList, m_context) == 0x1d80, "GraphicsCommandList layout");
static_assert(offsetof(GraphicsCommandList, m_targetsDirty) == 0x209c, "GraphicsCommandList layout");
static_assert(offsetof(GraphicsCommandList, m_dsvPtr) == 0x20d8, "GraphicsCommandList layout");
static_assert(sizeof(GraphicsCommandList) == 0x2100, "GraphicsCommandList is 0x2100 bytes");

}

// The mip generation compute pipeline (0x38 bytes).
struct GENERATEMIPSPIPELINE {
    int32_t m_shader;               // 0x00
    uint32_t m_pad04;
    ID3D12PipelineState* m_pipeline;  // 0x08
    uint64_t m_20;                  // 0x10
    uint64_t m_18;                  // 0x18
    uint64_t m_38;                  // 0x20
    uint16_t m_30;                  // 0x28
    uint16_t m_pad2a;
    uint32_t m_08;                  // 0x2c
    uint16_t m_permute;             // 0x30
    uint8_t m_pad32[6];
};
static_assert(sizeof(GENERATEMIPSPIPELINE) == 0x38, "GENERATEMIPSPIPELINE is 0x38 bytes");

// What a texture heap is made from, and the heap (device slot 0x1e8).
struct TEXHEAPDESC {
    uint64_t m_size;
    uint32_t m_alignment;
    uint32_t m_pad0c;
    uint8_t m_targets;
};

struct TEXHEAP {
    uint64_t m_size;                // 0x00
    uint8_t m_targets;              // 0x08
    uint8_t m_pad09[7];
    gxDx12Backend::HeapPage m_page; // 0x10
};

class CGxDeviceD3d12 : public CGxDevice {
    public:
    CGxDeviceD3d12();                                   // FUN_1422cacf0
    ~CGxDeviceD3d12() override;

    bool DeviceCreate(const GXDEVICEPARAMS* params) override;           // FUN_1422d0f80
    void DeviceShutdown() override;                                      // FUN_1422d1020
    void* SwapChainCreate(const GXSWAPCHAINPARAMS* params) override;     // FUN_1422e1100
    void SwapChainDestroy(void* chain) override;                         // FUN_1422e1270
    bool SwapChainBegin(void* chain) override;                           // FUN_1422e0f80
    void* SwapChainGetBackBuffer(void* chain) override;                  // FUN_1422eb860
    void SwapChainPresent(void* chain) override;                         // FUN_1422e1490
    void SwapChainEnd(void* chain) override;                             // FUN_1422e1480

    // The D3D12 class's own slots (0x2a8..0x2e8).
    virtual void ReportError(HRESULT hr, const char* text);              // 0x2a8 FUN_1422e65e0
    virtual bool CreateD3dDevice(uint32_t* featureLevel);                 // 0x2b0 FUN_1422eb8a0
    virtual void ReleaseAdapter();                                        // 0x2b8 FUN_1422ebb30
    virtual void PresentAll();                                            // 0x2c0 FUN_1422ec1d0
    virtual void UpdateFrameStatistics();                                 // 0x2c8 FUN_1422ec360
    virtual void QueryVideoMemory();                                      // 0x2d0 FUN_1422ec2e0
    virtual bool CreateSwapChain(gxDx12Backend::SwapChain* chain, const gxDx12Backend::SWAPCHAINPARAMS* params);  // 0x2d8 FUN_1422ebf50
    virtual bool CreateBackBuffers(gxDx12Backend::SwapChain* chain);      // 0x2e0 FUN_1422ebb60
    virtual void ReleaseBackBuffers(gxDx12Backend::SwapChain* chain);     // 0x2e8 FUN_1422ebe80

    // Fails the device: clears m_ready and reports (FUN_142295750).
    void Fail(HRESULT hr, const char* text);

    bool DeviceCreateCore(const GXDEVICEPARAMS* params);                 // FUN_1422d5d50
    void DeviceDestroy();                                                 // FUN_1422d7c80
    bool FindAdapter();                                                   // FUN_1422cdfb0
    void WaitForGpu();                                                    // FUN_1422e61d0
    void ReleaseFrames();                                                 // FUN_1422dca40
    bool TexCreate(const gxDx12Backend::TEXDESC* desc, gxDx12Backend::Tex* tex, bool mayFail, ID3D12Resource* resource);  // FUN_1422d9810
    void TexReleaseViews(gxDx12Backend::Tex* tex);                        // FUN_1422da380
    void TexRelease(gxDx12Backend::Tex* tex);                             // FUN_1422da5d0
    bool CreatePlaced(const D3D12_RESOURCE_DESC* desc, uint32_t state, const D3D12_CLEAR_VALUE* clear, bool mayFail,
        gxDx12Backend::Tex* tex);                                         // FUN_1422cfa50
    bool CreateTargetView(gxDx12Backend::Tex* tex, const gxDx12Backend::VIEWRANGE* range, ID3D12Resource* resource,
        gxDx12Backend::TargetView* view);                                 // FUN_1422da750
    bool CreateDepthView(gxDx12Backend::Tex* tex, const gxDx12Backend::VIEWRANGE* range, ID3D12Resource* resource,
        gxDx12Backend::TargetView* view);                                 // FUN_1422da080
    bool CreateShaderView(gxDx12Backend::Tex* tex, const gxDx12Backend::VIEWRANGE* range, ID3D12Resource* resource,
        gxDx12Backend::ShaderView* view);                                 // FUN_1422daa60
    bool CreateUnorderedView(gxDx12Backend::Tex* tex, const gxDx12Backend::VIEWRANGE* range, ID3D12Resource* resource,
        gxDx12Backend::ShaderView* view);                                 // FUN_1422dadb0
    void FrameSubmit();                                                   // FUN_1422d1690
    void FrameEnd();                                                      // FUN_1422d3280 (slot 0x190)
    bool FlushCopies();                                                   // FUN_1422cf0e0
    ID3D12Resource* CreateReadbackBuffer(uint32_t size);                 // FUN_1422d78a0
    bool CreateGenerateMips(GENERATEMIPSPIPELINE* pipeline);              // FUN_1422d4d60
    void ReleaseSamplers();
    // The sampler descriptor for a sampler key, made the first time (FUN_1422c9640).
    D3D12_CPU_DESCRIPTOR_HANDLE SamplerFor(uint64_t key);
    void ReportMemory(blz::string_header* report);                        // FUN_1422dbce0
    bool FormatSupportsMultisample(uint8_t format, uint8_t samples, int32_t quality) override;  // 0x040 FUN_1422d1060
    void SetLowLatencyMode(uint8_t mode) override;                        // 0x078 FUN_1422df730
    bool ShadingRateSupported(uint32_t rate, int32_t a3, uint8_t a4) override;  // 0x178 FUN_1422db790
    void FrameMarker() override;                                          // 0x080 FUN_1422db7e0
    void LatencyMarker(uint32_t marker, uint32_t frame) override;         // 0x088 FUN_1422df780
    bool FrameBegin(void* query) override;                                // 0x180 FUN_1422d2c20
    void* FrameEnd(void* out, void* query) override;                      // 0x190 FUN_1422d3280
    uint32_t FrameCount() override;                                       // 0x198 FUN_1422a8d50
    bool TexHeapCreate(const void* desc, void** heap) override;           // 0x1e8 FUN_1422e1e60
    GXTEXALLOCINFO* TexAllocationInfo(GXTEXALLOCINFO* out, const void* desc) override;  // 0x1f8 FUN_1422cde90
    bool TexCreate(const void* desc, void** tex) override;                // 0x200 FUN_1422e1880
    void TexDestroy(void* tex) override;                                  // 0x208 FUN_1422e1b70
    void* DefaultTex() override;                                          // 0x210 FUN_1422e1cd0
    void* TexShaderView(void* tex, uint32_t plane) override;              // 0x250 FUN_1422e1c30
    void* TexUnorderedView(void* tex) override;                           // 0x258 FUN_1422e1c80
    void* TexTargetView(void* tex) override;                              // 0x260 FUN_1422e1be0
    void* QueryCreate(uint8_t type) override;                             // 0x278 FUN_1422dd7c0
    void QueryDestroy(void* query) override;                              // 0x280 FUN_1422dd830
    bool QueryInfo(void* query, uint8_t what, uint32_t* out) override;    // 0x288 FUN_1422dd930
    bool QueryGetResult(void* query, void* out, uint32_t size) override;  // 0x290 FUN_1422b8750
    void DeferCallback(blz::function_storage* callback) override;         // 0x058 FUN_1422a8e10
    bool BufCreate(GxBuf* buf) override;                                  // 0x1a0 FUN_1422cd720
    void BufDestroy(GxBuf* buf) override;                                 // 0x1a8 FUN_1422cd8f0
    bool BufAllocate(GxBuf* buf);                                         // FUN_1422d46a0
    GxPipeline* PipelineCreate(const GXPIPELINEDESC* desc, uint64_t hash, const char* name) override;  // 0x138 FUN_1422dc720
    void PipelineDestroy(GxPipeline* pipeline) override;                  // 0x140 FUN_1422dc920
    bool BuildPipeline(const GXPIPELINEDESC* desc, GxPipeline* pipeline);  // FUN_1422d86b0
    uint16_t* CreateVertexBufferFormat(uint16_t* result, const GXVERTEXBUFFERFORMATDESC* desc,
        GXVERTEXBUFFERFORMAT** format) override;                         // 0x148 FUN_14229b570
    gxDx12Backend::Tex* DefaultTexD3d12();
    void SubmitBegin();                                                   // FUN_1422db9f0
    void StartFrame();                                                    // FUN_1422de840
    void Flush();                                                         // FUN_1422d2a30
    void ReleaseOldSamplers();
    void ReleaseUploadBuffers();
    void* WaitForFrame(void* out);                                        // FUN_1422e62a0
    void TransitionList(gxDx12Backend::CopyCommandList* list, uint32_t index,
        gxDx12Backend::CopyCommandList* barrierList, uint32_t barrierIndex, uint32_t mask);  // FUN_1422d24d0

    // The D3D12 part (0x470..0x4750).
    gxDx12Backend::GxCallbackList m_callbacks;             // 0x0470
    ID3D12RootSignature* m_rootSignatures[2];              // 0x04b8: graphics, compute
    ID3D12CommandSignature* m_drawSignature;               // 0x04c8
    ID3D12CommandSignature* m_drawIndexedSignature;        // 0x04d0
    ID3D12CommandSignature* m_dispatchSignature;           // 0x04d8
    IDXGIAdapter1* m_adapter1;                             // 0x04e0
    IDXGIAdapter3* m_adapter3;                             // 0x04e8
    ID3D12Device* m_device;                                // 0x04f0
    ID3D12QueryHeap* m_queryHeaps[2];                      // 0x04f8: occlusion, timestamps
    DXGI_QUERY_VIDEO_MEMORY_INFO m_localMemory;            // 0x0508
    DXGI_QUERY_VIDEO_MEMORY_INFO m_nonLocalMemory;         // 0x0528
    uint64_t m_548;                                        // 0x0548
    gxDx12Backend::Queue m_directQueue;                    // 0x0550
    gxDx12Backend::Queue m_computeQueue;                   // 0x05a8
    gxDx12Backend::Queue m_copyQueue;                      // 0x0600
    D3DX12Residency::ResidencyManager m_residency;         // 0x0658
    D3D12_FEATURE_DATA_D3D12_OPTIONS m_options;            // 0x0828
    uint32_t m_rootSignatureVersion;                       // 0x0864
    uint8_t m_tearing;                                     // 0x0868
    uint8_t m_vrs;                                         // 0x0869
    uint8_t m_largeHeaps;                                  // 0x086a
    uint8_t m_rtvHeapLow;                                  // 0x086b: the RTV heap is nearly full
    uint8_t m_pad86c[4];
    gxDx12Backend::SwapChain* m_currentChain;              // 0x0870
    gxDx12Backend::SwapChain* m_primaryChain;              // 0x0878
    blz::embedded_vector<gxDx12Backend::FrameData, 3, gxDx12Backend::s_frameDataTag> m_frames;  // 0x0880
    gxDx12Backend::FrameData* m_frame;                     // 0x0a48
    uint8_t m_frameIndex;                                  // 0x0a50
    uint8_t m_queryFlags[2];                               // 0x0a51
    uint8_t m_copiesPending;                               // 0x0a53
    uint8_t m_pada54[4];
    blz::vector<gxDx12Backend::CopyCommandList*, gxDx12Backend::s_copyListPtrTag> m_activeLists;  // 0x0a58
    blz::vector<gxDx12Backend::EnqueuePair, gxDx12Backend::s_enqueuePairTag> m_graphicsSubmits;  // 0x0a70
    blz::vector<gxDx12Backend::CopyCommandList*, gxDx12Backend::s_copyListPtrTag> m_computeSubmits;  // 0x0a88
    blz::vector<ID3D12CommandList*, gxDx12Backend::s_commandListPtrTag> m_submitLists;  // 0x0aa0
    blz::vector<D3DX12Residency::ResidencySet*, gxDx12Backend::s_residencySetPtrTag> m_submitSets;  // 0x0ab8
    SRWLOCK m_submitLock;                                  // 0x0ad0
    CONDITION_VARIABLE m_submitCondition;                  // 0x0ad8
    uint32_t m_submitPending;                              // 0x0ae0
    uint32_t m_padae4;
    blz::embedded_vector<gxDx12Backend::SwapChain*, 1, gxDx12Backend::s_swapChainPtrTag> m_presents;  // 0x0ae8
    blz::vector<gxDx12Backend::HeapPage*, gxDx12Backend::s_heapPageTag> m_heapPages;  // 0x0b08
    blz::vector<void*, gxDx12Backend::s_d3d12ObjectPtrTag> m_uploadsC2;  // 0x0b20
    blz::vector<void*, gxDx12Backend::s_d3d12ObjectPtrTag> m_uploadsC1;  // 0x0b38
    blz::vector<void*, gxDx12Backend::s_d3d12ObjectPtrTag> m_uploadsC0;  // 0x0b50
    blz::vector<gxDx12Backend::DescriptorUploadPage, gxDx12Backend::s_uploadPageTag> m_freeUploadPages[4];  // 0x0b68
    gxDx12Backend::DescriptorHeap m_rtvHeap;               // 0x0bc8
    gxDx12Backend::DescriptorHeap m_dsvHeap;               // 0x0c50
    gxDx12Backend::DescriptorHeap m_samplerHeap;           // 0x0cd8
    gxDx12Backend::DescriptorHeap m_viewHeap;              // 0x0d60: CBV/SRV/UAV
    blz::embedded_vector<gxDx12Backend::SwapChain*, 1, gxDx12Backend::s_swapChainPtrTag> m_swapChains;  // 0x0de8
    blz::pointer_map<void*, gxDx12Backend::s_samplerMapTag> m_samplers;  // 0x0e08
    uint64_t m_samplerReleases[4];                         // 0x0e30: blz::deque of samplers to free
    SRWLOCK m_samplerLock;                                 // 0x0e50
    blz::pointer_map<uint32_t, gxDx12Backend::s_stateMapTag> m_states;  // 0x0e58: D3d12Object -> state
    // The three lists are built here in DeviceCreate, not by the constructor.
    alignas(8) uint8_t m_graphics[sizeof(gxDx12Backend::GraphicsCommandList)];  // 0x0e80
    alignas(8) uint8_t m_copyList[sizeof(gxDx12Backend::CopyCommandList)];      // 0x2f80
    alignas(8) uint8_t m_computeList[sizeof(gxDx12Backend::CopyCommandList)];   // 0x3b18
    gxDx12Backend::GraphicsCommandList* m_graphicsPtr;     // 0x46b0
    gxDx12Backend::CopyCommandList* m_copyPtr;             // 0x46b8
    gxDx12Backend::CopyCommandList* m_computePtr;          // 0x46c0
    uint8_t m_latency[0x48];                               // 0x46c8: the Reflex markers and fence ring
    void* m_generateMips;                                  // 0x4710
    uint32_t m_committedBytes;                             // 0x4718
    uint32_t m_471c[2];
    uint32_t m_placedBytes;                                // 0x4724
    uint32_t m_4728[2];
    uint32_t m_committedCount;                             // 0x4730
    uint32_t m_placedCount;                                // 0x4734
    uint32_t m_4738;                                       // 0x4738
    uint32_t m_pad473c;
    // The device's queue-sync interface (vtable 0x14291a3a0; DAT_14348b648 points here).
    class SyncInterface {
        public:
        virtual void Slot00();      // FUN_1422cf970 -> FUN_14230a350
        virtual void Slot08();      // FUN_1422cf980 -> FUN_14230a3d0
        virtual void Slot10();      // FUN_1422cf990 -> FUN_14230a460
    };
    SyncInterface m_syncInterface;                         // 0x4740
    uint64_t m_computeWaitValue;                           // 0x4748
};
static_assert(offsetof(CGxDeviceD3d12, m_callbacks) == 0x470, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_device) == 0x4f0, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_directQueue) == 0x550, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_residency) == 0x658, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_options) == 0x828, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_rootSignatureVersion) == 0x864, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_currentChain) == 0x870, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_frames) == 0x880, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_frame) == 0xa48, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_activeLists) == 0xa58, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_submitLock) == 0xad0, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_presents) == 0xae8, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_heapPages) == 0xb08, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_rtvHeap) == 0xbc8, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_viewHeap) == 0xd60, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_swapChains) == 0xde8, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_states) == 0xe58, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_graphics) == 0xe80, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_copyList) == 0x2f80, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_computeList) == 0x3b18, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_generateMips) == 0x4710, "CGxDeviceD3d12 layout");
static_assert(offsetof(CGxDeviceD3d12, m_syncInterface) == 0x4740, "CGxDeviceD3d12 layout");
static_assert(sizeof(CGxDeviceD3d12) == 0x4750, "CGxDeviceD3d12 is 0x4750 bytes");

// DAT_14348b648: the device's sync interface while it is up.
extern CGxDeviceD3d12::SyncInterface* g_gxSyncInterface;

// DAT_143490b08: cleared after each Present.
extern uint32_t g_gxPresentSpin;

#endif
