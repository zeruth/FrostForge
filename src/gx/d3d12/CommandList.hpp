#ifndef GX_D3D12_COMMAND_LIST_HPP
#define GX_D3D12_COMMAND_LIST_HPP

// gxDx12Backend's command lists: CopyCommandList records into a ring of D3D12 lists with their
// residency sets and resource state tracking; GraphicsCommandList adds the descriptor staging
// and the engine's render context (GxRenderContext) on top.

#include "gx/d3d12/Objects.hpp"
#include <cstddef>
#include <cstdint>
#include <d3d12.h>

struct GXRENDERTARGETS;
struct GxBuf;

namespace gxDx12Backend {

extern const char s_listTag[];              // ".?AV?$ComPtr@UID3D12GraphicsCommandList@@@WRL@Microsoft@@"
extern const char s_allocatorTag[];         // ".?AV?$ComPtr@UID3D12CommandAllocator@@@WRL@Microsoft@@"
extern const char s_residencySetPtrTag[];   // ".PEAVResidencySet@D3DX12Residency@@"
extern const char s_barrierTag[];           // ".?AUD3D12_RESOURCE_BARRIER@@"
extern const char s_stateTrackerTag[];      // ".?AU?$pair@QEBUD3d12Object@gxDx12Backend@@UStateTracker@2@@blz@@"
extern const char s_stateTrackerMapTag[];   // ".?AV?$probed_hash_map@PEBUD3d12Object@..."
extern const char s_perFrameTag[];          // ".?AUPerFrameResources@CopyCommandList@gxDx12Backend@@"
extern const char s_uploadPageTag[];        // ".?AUDescriptorUploadPage@gxDx12Backend@@"
extern const char s_bufPairTag[];           // ".?AU?$pair@PEAUGxBuf@@I@blz@@"
extern const char s_uintVectorTag[];        // ".?AV?$embedded_vector@I$04V?$allocator@I@blz@@@blz@@"
extern const char s_uintTag[];              // ".I"
extern const char s_arrayTag[];             // ".?AU?$array@_K$04@blz@@"

// What a list knows of a tracked object's state: the state it is in at the end of what has
// been recorded, and the state it must be in at the start (8 bytes).
struct StateTracker {
    uint32_t m_first;               // 0x00: what the object must be in when the list starts
    uint32_t m_current;             // 0x04
};

typedef blz::pointer_map<StateTracker, s_stateTrackerTag> StateTrackerMap;
static_assert(sizeof(StateTrackerMap) == 0x28, "StateTrackerMap is 0x28 bytes");

// A page of the descriptor upload ring (0x30 bytes).
struct DescriptorUploadPage {
    ID3D12DescriptorHeap* m_heap;   // 0x00 (ComPtr)
    uint8_t m_08[0x28];
};

extern const char s_dataUploadPageTag[];    // ".?AUDataUploadPage@gxDx12Backend@@"

// A mapped upload buffer constants and dynamic data are written into (0x28 bytes).
struct DataUploadPage {
    ID3D12Resource* m_resource;     // 0x00 (ComPtr)
    uint8_t* m_cpu;                 // 0x08: mapped
    uint64_t m_gpu;                 // 0x10: GPU virtual address
    uint32_t m_begin;               // 0x18: the last allocation's offset
    uint32_t m_cursor;              // 0x1c
    uint32_t m_size;                // 0x20
    uint32_t m_pad24;
};

// An allocation from the upload pages (0x20 bytes).
struct UPLOADALLOC {
    ID3D12Resource* m_resource;
    uint8_t* m_cpu;
    uint64_t m_gpu;
    uint32_t m_offset;
    uint32_t m_size;
};

// The allocators and transient pages one frame of a list owns (0xe8 bytes).
struct PerFrameResources {
    blz::embedded_vector<ID3D12CommandAllocator*, 1, s_allocatorTag> m_allocators;  // 0x00
    blz::vector<DataUploadPage, s_dataUploadPageTag> m_pages;      // 0x20: reused each frame
    blz::vector<DataUploadPage, s_dataUploadPageTag> m_largePages; // 0x38: one per large upload
    uint64_t m_uploadOffsets[4];    // 0x50
    blz::vector<DescriptorUploadPage, s_uploadPageTag> m_uploadPages[5];  // 0x70
};
static_assert(offsetof(PerFrameResources, m_uploadOffsets) == 0x50, "PerFrameResources layout");
static_assert(sizeof(PerFrameResources) == 0xe8, "PerFrameResources is 0xe8 bytes");

// One recorded barrier set's bookkeeping (0x30 bytes: the barriers a list needed before it).
struct BufPair {
    void* m_buf;
    uint32_t m_size;    // the bytes written, to copy into its heap
    uint32_t m_pad;
};

struct Array5 {
    uint64_t m_values[5];
};

struct UintVector5 {
    blz::embedded_vector<uint32_t, 5, s_uintTag> m_values;
};

// The copy (and compute) command list (0xb98 bytes).
class CopyCommandList {
    public:
    ID3D12GraphicsCommandList* m_list;      // 0x000: the one being recorded
    ID3D12GraphicsCommandList5* m_list5;    // 0x008 (ComPtr)
    uint8_t m_empty;                        // 0x010: nothing recorded since the reset
    uint8_t m_current;                      // 0x011: index of m_list in m_lists
    uint8_t m_pad12[2];
    uint32_t m_type;                        // 0x014: D3D12_COMMAND_LIST_TYPE
    uint32_t m_18;                          // 0x018
    uint32_t m_pad1c;
    blz::embedded_vector<ID3D12GraphicsCommandList*, 1, s_listTag> m_lists;  // 0x020
    blz::embedded_vector<D3DX12Residency::ResidencySet*, 1, s_residencySetPtrTag> m_residencySets;  // 0x040
    blz::vector<D3D12_RESOURCE_BARRIER, s_barrierTag> m_barriers;  // 0x060
    blz::embedded_vector<StateTrackerMap, 1, s_stateTrackerMapTag> m_states;  // 0x078: per list
    blz::embedded_vector<PerFrameResources, 3, s_perFrameTag> m_frames;  // 0x0b8
    PerFrameResources* m_frame;             // 0x388
    DataUploadPage* m_page;                 // 0x390: the page being filled
    uint8_t m_398[0x700];                   // 0x398: the constant bindings (the graphics list's)
    blz::vector<BufPair, s_bufPairTag> m_bufs;  // 0xa98: buffers written this frame, and how much
    blz::embedded_vector<UintVector5, 1, s_uintVectorTag> m_signals;  // 0xab0: per list
    blz::embedded_vector<uint32_t, 5, s_uintTag> m_af8;  // 0xaf8
    blz::embedded_vector<Array5, 1, s_arrayTag> m_b28;  // 0xb28
    void** m_listStates;                    // 0xb68: CommandListState pointers
    uint64_t m_listStateCount;              // 0xb70
    uint8_t m_b78[0x10];                    // 0xb78
    void* m_b88;                            // 0xb88
    uint64_t m_b90;                         // 0xb90

    CopyCommandList(uint32_t type);         // FUN_1422ca100
    ~CopyCommandList();                     // FUN_1422cc120

    void BeginFrame();                      // FUN_1422cf9a0
    void Reset();                           // FUN_1422cf000
    void FlushBarriers();                   // the inline flush before each command
    bool Transition(blz::vector<D3d12Object*, s_d3d12ObjectPtrTag>* objects, uint32_t state);  // FUN_1422e49c0
    bool TransitionObjects(D3d12Object* objects, uint64_t count, uint32_t state);  // FUN_1422e4c40
    void CloseList(uint32_t index);         // FUN_1422de6d0
    void CloseLists();                      // FUN_1422ccce0

    D3DX12Residency::ResidencySet* ResidencySet() {
        return this->m_residencySets[this->m_current];
    }
};
static_assert(offsetof(CopyCommandList, m_lists) == 0x20, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_barriers) == 0x60, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_states) == 0x78, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_frames) == 0xb8, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_frame) == 0x388, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_bufs) == 0xa98, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_signals) == 0xab0, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_af8) == 0xaf8, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_b28) == 0xb28, "CopyCommandList layout");
static_assert(offsetof(CopyCommandList, m_listStates) == 0xb68, "CopyCommandList layout");
static_assert(sizeof(CopyCommandList) == 0xb98, "CopyCommandList is 0xb98 bytes");

}

// The engine's render context (GxRenderContext, Engine/Source/Gx, vtable 0x142912178, 0x300
// bytes): what drawing code calls; the device's command list implements it.
class GxRenderContext {
    public:
    GxRenderContext();                      // FUN_14228f2a0
    virtual ~GxRenderContext();             // 0x000 (FUN_14228f3e0)
    virtual void Slot008();
    virtual void Slot010(uint8_t stage, uint32_t slot, void* texture, uint8_t a5);
    virtual void Slot018(uint8_t stage, uint32_t first, uint32_t count, void** textures);  // FUN_14228fa20
    virtual void Slot020(uint32_t a2, void* a3);
    virtual void Slot028();
    virtual void Slot030(uint8_t stage, uint32_t slot, void* a4);
    virtual void Slot038();                 // FUN_14228fa90
    virtual void Slot040();
    virtual void Slot048();
    virtual void Slot050();
    virtual void Slot058();
    virtual void Slot060();
    virtual void Slot068();
    virtual void Slot070();
    virtual void Slot078();
    virtual void Slot080();
    virtual void Slot088();
    virtual void SetRenderTargets(GXRENDERTARGETS* targets);  // 0x090
    virtual void Slot098();
    virtual void Slot0a0();
    virtual void Clear(GXRENDERTARGETS* targets, uint16_t mask, uint32_t color, float depth, uint32_t stencil,
        uint8_t a7);                        // 0x0a8
    virtual void Slot0b0();
    virtual void Slot0b8();
    virtual void Slot0c0();
    virtual void Slot0c8();
    virtual void Slot0d0();
    virtual void Slot0d8();
    virtual void Slot0e0();
    virtual void Slot0e8();
    virtual void Slot0f0();
    virtual void Slot0f8();
    virtual void Slot100();
    virtual void Slot108();
    virtual void Slot110();
    virtual void Slot118();
    virtual void GetDepthRange(float* a2, float* a3, float* a4, float* a5, float* a6, float* a7);  // 0x120
    virtual void Slot128();
    virtual void Slot130();
    virtual void Slot138();
    virtual void Slot140();
    virtual void Slot148();
    virtual void Slot150();
    virtual void Slot158();
    virtual void Slot160();
    virtual float* LockConstants(uint8_t stage, uint32_t slot, uint32_t size);  // 0x168
    virtual void UnlockConstants(uint8_t stage, uint32_t slot);   // 0x170
    virtual void Slot178();
    // Space to write a buffer's contents into, and its release to the GPU (`size` bytes, all
    // when 0).
    virtual void* BufLock(GxBuf* buf);                      // 0x180
    virtual bool BufUnlock(GxBuf* buf, uint32_t size);      // 0x188
    virtual void Slot190();
    virtual bool CopyTex(gxDx12Backend::Tex* source, gxDx12Backend::Tex* dest, int32_t* sourceOrigin,
        int32_t* destOrigin, uint32_t* size, uint32_t slice, int32_t mip);  // 0x198
    virtual void Slot1a0();
    virtual bool ResolveTex(gxDx12Backend::Tex* source, gxDx12Backend::Tex* dest);  // 0x1a8
    virtual void Slot1b0();
    virtual void Slot1b8();
    virtual void Slot1c0();
    virtual void Slot1c8();
    virtual void Slot1d0();
    virtual void Slot1d8();
    virtual void Slot1e0();
    virtual void Slot1e8();
    virtual void Slot1f0();
    virtual void Slot1f8();
    // The context's transient buffer for vertices (0), indices (1) or constants (2).
    virtual GxBuf* GetUploadBuf(uint8_t type);              // 0x200

    // A member at its reference offset (the state is 0x2f8 bytes of mostly unported fields:
    // +0x08 the viewport, +0x18 and +0x74 its dirty flags, +0x88 the bound GXRENDERTARGETS).
    template <class T>
    T& Field(size_t offset) {
        return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(this) + offset);
    }

    // Sets the viewport, flagging it when it changed (FUN_14228f680).
    void SetViewport(const float* viewport);

    uint8_t m_state[0x2f8];                 // 0x008
};
static_assert(sizeof(GxRenderContext) == 0x300, "GxRenderContext is 0x300 bytes");

#endif
