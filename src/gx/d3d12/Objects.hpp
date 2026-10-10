#ifndef GX_D3D12_OBJECTS_HPP
#define GX_D3D12_OBJECTS_HPP

// gxDx12Backend's resource objects (Engine/Source/Gx/src/D3d12): textures and their views,
// the placed-resource heaps, descriptor heaps, queues and the per-frame data.

#include "blz/Vector.hpp"
#include "gx/d3d12/Residency.hpp"
#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <map>

namespace gxDx12Backend {

extern const char s_d3d12ObjectTag[];       // ".?AUD3d12Object@gxDx12Backend@@"
extern const char s_d3d12ObjectPtrTag[];    // ".PEBUD3d12Object@gxDx12Backend@@"
extern const char s_texPtrTag[];            // ".PEAUTex@gxDx12Backend@@"
extern const char s_texTag[];               // ".?AUTex@gxDx12Backend@@"
extern const char s_freeListMapTag[];       // ".PEAI"
extern const char s_freeListBlockTag[];     // ".I"
extern const char s_comPtrTag[];            // ".?AV?$ComPtr@UIUnknown@@@WRL@Microsoft@@"
extern const char s_queryPtrTag[];          // ".PEAUQuery@gxDx12Backend@@"
extern const char s_callbackPageTag[];      // ".?AUCallbackPage@GxCallbackListBase@@"
extern const char s_smemPtrTag[];           // ".?AV?$unique_ptr@XUsmem_free_deleter@@@blz@@"

// One resource (or one subresource of it) as the state tracker sees it (0x18 bytes).
struct D3d12Object {
    ID3D12Resource* m_resource;     // 0x00 (ComPtr)
    D3DX12Residency::ManagedObject* m_residency;  // 0x08: the texture's own, or its heap's
    uint32_t m_subresource;         // 0x10: 0xFFFFFFFF for all of them
    uint8_t m_untracked;            // 0x14: its state is not tracked (depth formats without SRV)
    uint8_t m_15[3];
};
static_assert(sizeof(D3d12Object) == 0x18, "D3d12Object is 0x18 bytes");

struct Tex;

// What a view covers, as Tex views are created with it (8 bytes).
struct VIEWRANGE {
    uint8_t m_mip;                  // 0x00
    uint8_t m_mipCount;             // 0x01: 0xFF for the rest of the chain
    uint16_t m_slice;               // 0x02
    uint16_t m_sliceCount;          // 0x04: 0xFFFF for the rest of the array
    uint16_t m_flags;               // 0x06: 1 read-only depth, 2 the stencil plane
};

// A render target or depth view (0x58 bytes).
struct TargetView {
    Tex* m_tex = nullptr;           // 0x00
    uint32_t m_id = 0;              // 0x08
    uint32_t m_pad0c = 0;
    uint8_t m_mip = 0;                  // 0x10
    uint8_t m_pad11[7] = {};
    uint32_t m_state = 0;           // 0x18: D3D12_RESOURCE_STATES while bound
    uint32_t m_pad1c = 0;
    blz::embedded_vector<D3d12Object*, 1, s_d3d12ObjectPtrTag> m_objects;  // 0x20
    uint32_t m_mipBegin = 0;        // 0x40
    uint32_t m_mipEnd = 0;          // 0x44
    uint32_t m_sliceBegin = 0;      // 0x48
    uint32_t m_sliceEnd = 0;        // 0x4c
    D3D12_CPU_DESCRIPTOR_HANDLE m_handle = {};  // 0x50
};
static_assert(offsetof(TargetView, m_objects) == 0x20, "TargetView layout");
static_assert(sizeof(TargetView) == 0x58, "TargetView is 0x58 bytes");

// A shader resource or unordered access view (0x78 bytes).
struct ShaderView {
    Tex* m_tex = nullptr;           // 0x00
    uint32_t m_id = 0;              // 0x08
    uint32_t m_pad0c = 0;
    uint32_t m_state = 0;           // 0x10
    uint32_t m_pad14 = 0;
    blz::embedded_vector<D3d12Object*, 1, s_d3d12ObjectPtrTag> m_objects;  // 0x18
    uint32_t m_mipBegin = 0;        // 0x38
    uint32_t m_mipEnd = 0;          // 0x3c
    uint32_t m_sliceBegin = 0;      // 0x40
    uint32_t m_sliceEnd = 0;        // 0x44
    uint8_t m_desc[0x28] = {};      // 0x48: the D3D12 view description
    D3D12_CPU_DESCRIPTOR_HANDLE m_handle = {};  // 0x70
};
static_assert(offsetof(ShaderView, m_objects) == 0x18, "ShaderView layout");
static_assert(sizeof(ShaderView) == 0x78, "ShaderView is 0x78 bytes");

// VariableSizeAllocationsManager: the free ranges of a heap page, by offset and by size.
// DIVERGED: the reference keeps them in two blz rb_trees laid out in the page (0x58 bytes);
// std::map and std::multimap are the same ordered containers, so the page is larger here.
struct VariableSizeAllocationsManager {
    struct FreeBlockInfo {
        uint32_t m_size;
        std::multimap<uint32_t, uint32_t>::iterator m_bySize;
    };

    std::map<uint32_t, FreeBlockInfo> m_byOffset;
    std::multimap<uint32_t, uint32_t> m_bySize;
    uint32_t m_maxSize = 0;
    uint32_t m_freeSize = 0;

    void AddNewBlock(uint32_t offset, uint32_t size);   // FUN_1422c2350
    uint32_t Allocate(uint32_t size);                   // FUN_1422c27d0
    void Free(uint32_t offset, uint32_t size);          // FUN_1422c2d00
};

// An ID3D12Heap placed textures are carved from (0xb8 bytes in the reference).
struct HeapPage {
    ID3D12Heap* m_heap;             // 0x00
    D3DX12Residency::ManagedObject m_residency;  // 0x08
    VariableSizeAllocationsManager m_allocations;  // 0x48
    blz::vector<Tex*, s_texPtrTag> m_textures;  // 0xa0: Tex::m_heapIndex indexes this

    explicit HeapPage(uint32_t size);   // FUN_1422cb610
    ~HeapPage();                        // FUN_1422cc560
};

// A 32 MB committed buffer that static and stream GxBufs are carved from (BufferAllocator,
// 0xa8 bytes in the reference). Each usage has its own list of pages, in the state that usage
// is read in.
struct BufferPage {
    ID3D12Resource* m_resource;     // 0x00 (ComPtr)
    VariableSizeAllocationsManager m_allocations;  // 0x08
    D3DX12Residency::ManagedObject m_residency;    // 0x60
    uint8_t m_copyPending;          // 0xa0: a buffer in it was written this frame

    explicit BufferPage(uint32_t size);     // inline in FUN_1422d5a60
    ~BufferPage();                          // inline in FUN_1422d5a60 and FUN_1422c7e80
};

// A texture or render target (0x260 bytes).
struct Tex {
    uint16_t m_width;               // 0x000
    uint16_t m_height;              // 0x002
    uint16_t m_depth;               // 0x004
    uint8_t m_mipLevels;            // 0x006
    uint8_t m_dimension;            // 0x007: GXTEXDIM_*
    uint8_t m_format;               // 0x008: GXTEXFMT_*
    uint8_t m_09;                   // 0x009
    uint8_t m_pad0a[2];
    uint32_t m_id;                  // 0x00c
    uint32_t m_flags;               // 0x010: 1 created, 0x40 views without objects
    uint32_t m_usage;               // 0x014: the description's usage flags
    uint32_t m_usage2;              // 0x018: its high word (bit 0: no state object)
    uint32_t m_1c;                  // 0x01c
    blz::embedded_vector<D3d12Object, 1, s_d3d12ObjectTag> m_objects;  // 0x020
    ShaderView m_srv;               // 0x050
    ShaderView m_stencilSrv;        // 0x0c8
    ShaderView m_uav;               // 0x140
    TargetView m_rtv;               // 0x1b8: the render target or depth view
    HeapPage* m_heap;               // 0x210: when placed
    uint32_t m_heapOffset;          // 0x218
    uint32_t m_size;                // 0x21c
    D3DX12Residency::ManagedObject m_residency;  // 0x220: when committed

    Tex(const struct TEXDESC* desc, uint32_t id);  // FUN_1422cb6b0
    Tex(Tex&& other);               // FUN_1422ea7c0
    ~Tex();                         // FUN_1422cc600

    // A placed texture has no residency object of its own; its size field (+0x230) holds the
    // texture's index in the heap page's list instead.
    uint64_t& HeapIndex() {
        return this->m_residency.m_size;
    }
};
static_assert(offsetof(Tex, m_objects) == 0x20, "Tex layout");
static_assert(offsetof(Tex, m_srv) == 0x50, "Tex layout");
static_assert(offsetof(Tex, m_uav) == 0x140, "Tex layout");
static_assert(offsetof(Tex, m_rtv) == 0x1b8, "Tex layout");
static_assert(offsetof(Tex, m_heap) == 0x210, "Tex layout");
static_assert(offsetof(Tex, m_residency) == 0x220, "Tex layout");
static_assert(sizeof(Tex) == 0x260, "Tex is 0x260 bytes");

// What a texture is made from (GxTexDesc as the device sees it).
struct TEXDESC {
    uint8_t m_dimension;            // 0x00: 1 cube, 2 2D, 3 3D, 4 1D, 5 2D array
    uint8_t m_pad01[3];
    uint32_t m_width;               // 0x04
    uint32_t m_height;              // 0x08
    uint32_t m_depth;               // 0x0c
    uint8_t m_format;               // 0x10
    uint8_t m_11;                   // 0x11
    uint8_t m_pad12[2];
    uint32_t m_usage;               // 0x14: bits 0-2 mip mode, 18 render target, 19 shader
                                    //       resource, 20 unordered access, 21-23 samples,
                                    //       27 placed elsewhere, 28-31 mip count
    uint32_t m_usage2;              // 0x18: bit 0 (the usage's bit 32) no state object
    uint32_t m_pad1c;
    void* m_heap;                   // 0x20: a TEXHEAP to place the texture in, or null
    uint64_t m_28;                  // 0x28
};
static_assert(sizeof(TEXDESC) == 0x30, "TEXDESC is 0x30 bytes");

// A descriptor heap and its free list (0x88 bytes).
struct DescriptorHeap {
    ID3D12DescriptorHeap* m_heap;   // 0x00
    uint32_t m_type;                // 0x08: D3D12_DESCRIPTOR_HEAP_TYPE
    uint32_t m_pad0c;
    uint64_t m_cpuStart;            // 0x10
    uint64_t m_gpuStart;            // 0x18
    uint32_t m_increment;           // 0x20
    uint32_t m_next;                // 0x24: the bump offset
    uint32_t m_end;                 // 0x28: increment x count
    uint32_t m_pad2c;
    blz::deque<uint32_t, 16, s_freeListMapTag, s_freeListBlockTag> m_free;  // 0x30: offsets
    SRWLOCK m_lock;                 // 0x50
    blz::inline_string<0x11> m_name;  // 0x58

    DescriptorHeap();                                       // FUN_1422cab90
    ~DescriptorHeap();
    bool Allocate(D3D12_CPU_DESCRIPTOR_HANDLE* handle);     // FUN_1422d4500
    void Free(uint64_t handle);                             // the inline frees in FUN_1422da380
};
static_assert(offsetof(DescriptorHeap, m_free) == 0x30, "DescriptorHeap layout");
static_assert(offsetof(DescriptorHeap, m_lock) == 0x50, "DescriptorHeap layout");
static_assert(sizeof(DescriptorHeap) == 0x88, "DescriptorHeap is 0x88 bytes");

extern const char s_queueFrameTag[];        // ".?AUFrameData@Queue@Device@gxDx12Backend@@"

// A command queue and its fence (0x58 bytes).
struct Queue {
    ID3D12CommandQueue* m_queue;    // 0x00
    ID3D12Fence* m_fence;           // 0x08
    HANDLE m_event;                 // 0x10
    uint64_t m_fenceValue;          // 0x18
    uint32_t m_type;                // 0x20: D3D12_COMMAND_LIST_TYPE
    uint32_t m_pad24;
    blz::embedded_vector<uint64_t, 3, s_queueFrameTag> m_frameFences;  // 0x28: per frame

    ~Queue();
    bool Create();                                  // FUN_1422d0020
    void Flush();                                   // FUN_1422e6450
    void WaitForValue(uint64_t value);              // FUN_1422e64b0
    uint64_t Signal();                              // FUN_1422ccf40
};
static_assert(offsetof(Queue, m_frameFences) == 0x28, "Queue layout");
static_assert(sizeof(Queue) == 0x58, "Queue is 0x58 bytes");

// GxCallbackListBase (0x48 bytes): callbacks deferred until the GPU is done with a frame,
// written into pages of packed records.
struct CallbackPage {
    void* m_begin;
    void* m_end;
};

struct GxCallbackList {
    uint32_t m_pageSize;            // 0x00: 0x10000
    uint32_t m_pad04;
    void (*m_08)(void*);            // 0x08: _guard_check_icall
    blz::vector<CallbackPage, s_callbackPageTag> m_pages;  // 0x10: {begin, end}
    blz::vector<void*, s_smemPtrTag> m_freePages;  // 0x28
    void* m_lock;                   // 0x40: the device lock

    GxCallbackList();
    GxCallbackList(GxCallbackList&&) = default;
    ~GxCallbackList();
    void* Add(void (*callback)(void*), uint64_t size, uint64_t alignment);  // FUN_1422911b0
    void Run();                     // FUN_14229bdd0
    void SwapPages(GxCallbackList* other);  // FUN_1422a7b70, FUN_1422a7ee0
};
static_assert(sizeof(GxCallbackList) == 0x48, "GxCallbackList is 0x48 bytes");

// A Gx format as a resource format (DAT_142919aa0).
DXGI_FORMAT ResourceFormat(uint8_t format);
// As a shader resource (DAT_142919b30).
DXGI_FORMAT ShaderFormat(uint8_t format);

extern const char s_frameDataTag[];         // ".?AUFrameData@Device@gxDx12Backend@@"

// One frame in flight (0x90 bytes).
struct FrameData {
    blz::vector<IUnknown*, s_comPtrTag> m_releases;  // 0x00: released when the frame is done
    blz::vector<void*, s_queryPtrTag> m_queries;     // 0x18
    ID3D12Resource* m_queryReadback[2];              // 0x30: occlusion, timestamps
    uint8_t m_queryPending[2];                       // 0x40
    uint8_t m_pad42[6];
    GxCallbackList m_callbacks;                      // 0x48

    FrameData();
    FrameData(FrameData&&) = default;
    ~FrameData();
    void ReleaseObjects();                           // FUN_1422a5810
};
static_assert(offsetof(FrameData, m_queryReadback) == 0x30, "FrameData layout");
static_assert(offsetof(FrameData, m_callbacks) == 0x48, "FrameData layout");
static_assert(sizeof(FrameData) == 0x90, "FrameData is 0x90 bytes");

}

#endif
