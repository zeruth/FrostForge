#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxRenderTargets.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>

using namespace gxDx12Backend;

CGxDeviceD3d12* GxDeviceD3d12();

namespace {

const char s_deviceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\D3d12\\GxDeviceD3d12.cpp";
const char s_residencyFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\D3d12\\d3dx12Residency.h";

template <class T>
void ReleaseAndNull(T** object) {
    if (*object != nullptr) {
        (*object)->Release();
        *object = nullptr;
    }
}

// The residency object a texture is paged by: its own when committed, its heap page's when
// placed (the test inline at each use).
D3DX12Residency::ManagedObject* TexResidency(Tex* tex) {
    if (tex->m_residency.m_underlying != nullptr) {
        return &tex->m_residency;
    }
    if (tex->m_heap != nullptr) {
        return &tex->m_heap->m_residency;
    }
    return nullptr;
}

// The upload buffer a page or a large allocation lives in.
// ref: FUN_1422d7950
ID3D12Resource* CreateUploadBuffer(uint64_t size) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = static_cast<uint32_t>(size);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    CGxDeviceD3d12* device = GxDeviceD3d12();
    HRESULT hr = device->m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (FAILED(hr)) {
        device->Fail(hr, "CreateUploadBuffer CreateCommittedResource failed");
        ReleaseAndNull(&resource);
    }
    return resource;
}

}

// ---- CopyCommandList ---------------------------------------------------------------------

// ref: FUN_1422ca100
CopyCommandList::CopyCommandList(uint32_t type) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    this->m_empty = 1;
    this->m_current = 0;
    this->m_list = nullptr;
    this->m_list5 = nullptr;
    this->m_type = type;
    this->m_18 = 0;
    this->m_frame = nullptr;
    this->m_page = nullptr;
    std::memset(this->m_398, 0, sizeof(this->m_398));
    this->m_listStates = nullptr;
    this->m_listStateCount = 0;
    std::memset(this->m_b78, 0, sizeof(this->m_b78));
    this->m_b88 = nullptr;
    this->m_b90 = 0;

    // One allocator per frame in flight.
    this->m_frames.resize(device->m_frames.size());
    HRESULT hr = E_FAIL;
    for (PerFrameResources& frame : this->m_frames) {
        uint64_t count = frame.m_allocators.size();
        if (count < 2) {
            if (count == 0) {
                frame.m_allocators.emplace_back(nullptr);
            }
        } else {
            for (uint64_t i = 1; i < count; i++) {
                ReleaseAndNull(&frame.m_allocators[i]);
            }
            frame.m_allocators.m_size = 1;
        }
        ID3D12CommandAllocator** allocator = &frame.m_allocators.back();
        if (device->m_device != nullptr) {
            ReleaseAndNull(allocator);
            hr = device->m_device->CreateCommandAllocator(static_cast<D3D12_COMMAND_LIST_TYPE>(this->m_type),
                __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(allocator));
            if (SUCCEEDED(hr) && *allocator != nullptr) {
                continue;
            }
        } else if (*allocator != nullptr) {
            continue;
        }
        device->Fail(hr, "CommandListCreate CreateCommandAllocator failed");
    }

    // The list itself, closed until the first reset.
    uint64_t lists = this->m_lists.size();
    if (lists < 2) {
        if (lists == 0) {
            this->m_lists.emplace_back(nullptr);
        }
    } else {
        for (uint64_t i = 1; i < lists; i++) {
            ReleaseAndNull(&this->m_lists[i]);
        }
        this->m_lists.m_size = 1;
    }
    const char* failure = "CommandListCreate CreateCommandList failed";
    if (device->m_device != nullptr) {
        ID3D12GraphicsCommandList** list = &this->m_lists.back();
        ReleaseAndNull(list);
        hr = device->m_device->CreateCommandList(0, static_cast<D3D12_COMMAND_LIST_TYPE>(this->m_type),
            this->m_frames[0].m_allocators.back(), nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(list));
        if (SUCCEEDED(hr)) {
            ID3D12GraphicsCommandList* created = this->m_lists.back();
            if (created != nullptr && SUCCEEDED(hr = created->Close())) {
                failure = nullptr;
            } else {
                hr = created == nullptr ? E_FAIL : hr;
                failure = "CommandListCreate Close failed";
            }
        }
    }
    if (failure != nullptr) {
        device->Fail(hr, failure);
    }

    // The list's residency set.
    if (this->m_residencySets.size() == 0) {
        this->m_residencySets.emplace_back(nullptr);
    } else {
        this->m_residencySets.m_size = 1;
    }
    auto set = static_cast<D3DX12Residency::ResidencySet*>(
        operator new(sizeof(D3DX12Residency::ResidencySet), 0xd, s_residencyFile, SStrHashFnv1a(s_residencyFile), 0x6b1));
    set->m_set = nullptr;
    set->m_mutex = device->m_residency.m_setMutex;
    set->m_index = 0xffffffff;
    set->m_maxSize = 0;
    set->m_count = 0;
    set->m_isOpen = 0;
    set->m_outOfMemory = 0;
    this->m_residencySets[0] = set;

    // Its state tracker.
    this->m_states.resize(1);
    this->m_states.back().init(0.75f);
    this->m_states.back().m_maxLoad = 0.5f;
    if (this->m_b28.size() == 0) {
        this->m_b28.emplace_back();
    } else {
        this->m_b28.m_size = 1;
    }
    std::memset(&this->m_b28[0], 0, sizeof(Array5));

    device->m_activeLists.push_back(this);
    this->m_frame = &this->m_frames[device->m_frameIndex];
}

// ref: FUN_1422cc120
CopyCommandList::~CopyCommandList() {
    for (D3DX12Residency::ResidencySet* set : this->m_residencySets) {
        if (set != nullptr) {
            if (set->m_set != nullptr) {
                delete[] set->m_set;
            }
            operator delete(set, sizeof(D3DX12Residency::ResidencySet));
        }
    }
    for (PerFrameResources& frame : this->m_frames) {
        for (ID3D12CommandAllocator*& allocator : frame.m_allocators) {
            ReleaseAndNull(&allocator);
        }
        frame.m_allocators.m_size = 0;
        for (DataUploadPage& page : frame.m_pages) {
            ReleaseAndNull(&page.m_resource);
        }
        frame.m_pages.m_size = 0;
        for (DataUploadPage& page : frame.m_largePages) {
            ReleaseAndNull(&page.m_resource);
        }
        frame.m_largePages.m_size = 0;
        for (auto& pages : frame.m_uploadPages) {
            for (DescriptorUploadPage& page : pages) {
                ReleaseAndNull(&page.m_heap);
            }
            pages.m_size = 0;
        }
    }
    for (ID3D12GraphicsCommandList*& list : this->m_lists) {
        ReleaseAndNull(&list);
    }
    ReleaseAndNull(&this->m_list5);
    CGxDeviceD3d12* device = GxDeviceD3d12();
    auto& active = device->m_activeLists;
    for (uint64_t i = 0; i < active.size(); i++) {
        if (active[i] == this) {
            active.erase_unordered(i);
            break;
        }
    }
    if (this->m_b88 != nullptr) {
        SMemFree(this->m_b88, "D:\\BuildServer\\A\\work-git\\wow\\Storm\\H\\SMemNew.h", 0xa8, 0);
    }
}

// Starts a frame: the frame's allocators are reset for reuse.
// ref: FUN_1422cf9a0
void CopyCommandList::BeginFrame() {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0) {
        return;
    }
    this->m_empty = 1;
    this->m_frame = &this->m_frames[device->m_frameIndex];
    this->m_18 = 0;
    for (ID3D12CommandAllocator* allocator : this->m_frame->m_allocators) {
        if (allocator != nullptr) {
            HRESULT hr = allocator->Reset();
            if (FAILED(hr)) {
                device->Fail(hr, "Allocator Reset failed");
            }
        }
    }
}

// Opens the first list for recording on the frame's allocator.
// ref: FUN_1422cf000
void CopyCommandList::Reset() {
    this->m_current = 0;
    this->m_residencySets[0]->Open();
    this->m_states[0].clear();
    ID3D12GraphicsCommandList* list = this->m_lists[0];
    this->m_list = list;
    if (list == nullptr) {
        return;
    }
    ReleaseAndNull(&this->m_list5);
    list->QueryInterface(__uuidof(ID3D12GraphicsCommandList5), reinterpret_cast<void**>(&this->m_list5));
    ID3D12CommandAllocator* allocator = this->m_frame->m_allocators[0];
    if (this->m_list != nullptr && allocator != nullptr) {
        HRESULT hr = this->m_list->Reset(allocator, nullptr);
        if (FAILED(hr)) {
            GxDeviceD3d12()->Fail(hr, "CommandList Reset failed");
        }
    }
}

// Records the pending barriers before the next command.
void CopyCommandList::FlushBarriers() {
    if (this->m_barriers.size() != 0) {
        this->m_empty = 0;
        this->m_list->ResourceBarrier(static_cast<UINT>(this->m_barriers.size()), this->m_barriers.data());
        this->m_barriers.m_size = 0;
    }
}

// ref: FUN_1422de6d0
void CopyCommandList::CloseList(uint32_t index) {
    this->FlushBarriers();
    ID3D12GraphicsCommandList* list = this->m_lists[index];
    if (list != nullptr) {
        HRESULT hr = list->Close();
        if (FAILED(hr)) {
            GxDeviceD3d12()->Fail(hr, "RealCloseSingle failed");
        }
    }
    this->m_residencySets[index]->Close();
}

// ref: FUN_1422ccce0
void CopyCommandList::CloseLists() {
    uint8_t count = static_cast<uint8_t>(this->m_current + 1);
    if (static_cast<uint8_t>(this->m_lists.size()) < count) {
        count = static_cast<uint8_t>(this->m_lists.size());
    }
    for (uint32_t i = 0; i < count; i++) {
        this->CloseList(i);
    }
}

// Moves one object to `state` in this list: the first use records the state the list needs
// it in at the start; later uses add (or fold into) a transition barrier.
bool TrackState(CopyCommandList* list, D3d12Object* object, uint32_t state) {
    StateTrackerMap& map = list->m_states[list->m_current];
    bool inserted = false;
    StateTracker* tracker = map.insert(reinterpret_cast<uint64_t>(object), &inserted);
    if (inserted) {
        tracker->m_first = state;
        tracker->m_current = state;
        return false;
    }
    ID3D12Resource* resource = object->m_resource;
    for (uint64_t i = 0; i < list->m_barriers.size(); i++) {
        D3D12_RESOURCE_BARRIER& barrier = list->m_barriers[i];
        if (barrier.Transition.pResource == resource && barrier.Transition.Subresource == object->m_subresource &&
            barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            if (static_cast<uint32_t>(barrier.Transition.StateAfter) == state) {
                return true;
            }
            barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(state);
            tracker->m_current = state;
            if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
                return true;
            }
            // Back where it started: the barrier is dropped.
            barrier = list->m_barriers.back();
            list->m_barriers.m_size--;
            return false;
        }
    }
    if (tracker->m_current == state) {
        return false;
    }
    D3D12_RESOURCE_BARRIER& barrier = list->m_barriers.emplace_back();
    std::memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = object->m_subresource;
    barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(tracker->m_current);
    barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(state);
    tracker->m_current = state;
    list->m_empty = 0;
    return true;
}

// ref: FUN_1422e49c0
bool CopyCommandList::Transition(blz::vector<D3d12Object*, s_d3d12ObjectPtrTag>* objects, uint32_t state) {
    if (objects->size() == 0 || (*objects)[0]->m_untracked != 0) {
        return false;
    }
    bool changed = false;
    for (D3d12Object* object : *objects) {
        if (object->m_untracked == 0) {
            changed |= TrackState(this, object, state);
        }
    }
    return changed;
}

// ref: FUN_1422e4c40
bool CopyCommandList::TransitionObjects(D3d12Object* objects, uint64_t count, uint32_t state) {
    if (count == 0 || objects[0].m_untracked != 0) {
        return false;
    }
    bool changed = false;
    for (uint64_t i = 0; i < count; i++) {
        if (objects[i].m_untracked == 0) {
            changed |= TrackState(this, &objects[i], state);
        }
    }
    return changed;
}

// The next part of the upload ring, or a new 2 MB page.
// ref: FUN_1422d7a50
DataUploadPage* NextUploadPage(CopyCommandList* list) {
    PerFrameResources* frame = list->m_frame;
    DataUploadPage* page = nullptr;
    if (frame->m_pages.size() != 0) {
        if (list->m_page == nullptr) {
            page = &frame->m_pages[0];
        } else if (list->m_page != &frame->m_pages.back()) {
            page = list->m_page + 1;
            list->m_page = page;
        }
        if (page != nullptr) {
            page->m_cursor = 0;
            list->m_page = page;
            return page;
        }
    }
    page = &frame->m_pages.emplace_back();
    std::memset(page, 0, sizeof(DataUploadPage));
    page->m_resource = CreateUploadBuffer(0x200000);
    if (page->m_resource == nullptr) {
        frame->m_pages.m_size--;
        GxDeviceD3d12()->m_ready = 0;
        return nullptr;
    }
    D3D12_RANGE range = { 0, 0 };
    HRESULT hr = page->m_resource->Map(0, &range, reinterpret_cast<void**>(&page->m_cpu));
    if (FAILED(hr)) {
        GxDeviceD3d12()->Fail(hr, "CreateUploadPage Map failed");
    }
    page->m_gpu = page->m_resource->GetGPUVirtualAddress();
    page->m_size = 0x1fd000;
    page->m_cursor = 0;
    list->m_page = page;
    return page;
}

// Space in this frame's upload memory: from the ring when it fits a page, else a buffer of
// its own.
// ref: FUN_1422c2fd0
static UPLOADALLOC* UploadAllocate(CopyCommandList* list, UPLOADALLOC* out, uint32_t size, uint32_t alignment) {
    DataUploadPage* page;
    if (size <= 0x1fd000) {
        page = list->m_page;
        if (page != nullptr) {
            page->m_cursor = (page->m_cursor + alignment - 1) & static_cast<uint32_t>(-static_cast<int32_t>(alignment));
        }
        if (page == nullptr || page->m_size < page->m_cursor + size) {
            page = NextUploadPage(list);
            if (page == nullptr || page->m_cpu == nullptr) {
                GxDeviceD3d12()->m_ready = 0;
                std::memset(out, 0, sizeof(UPLOADALLOC));
                return out;
            }
        }
        page->m_begin = page->m_cursor;
        page->m_cursor += size;
    } else {
        PerFrameResources* frame = list->m_frame;
        page = &frame->m_largePages.emplace_back();
        std::memset(page, 0, sizeof(DataUploadPage));
        page->m_resource = CreateUploadBuffer(size);
        if (page->m_resource == nullptr) {
            frame->m_largePages.m_size--;
            GxDeviceD3d12()->m_ready = 0;
            std::memset(out, 0, sizeof(UPLOADALLOC));
            return out;
        }
        D3D12_RANGE range = { 0, 0 };
        page->m_resource->Map(0, &range, reinterpret_cast<void**>(&page->m_cpu));
        page->m_gpu = page->m_resource->GetGPUVirtualAddress();
        page->m_size = size;
    }
    out->m_resource = page->m_resource;
    out->m_cpu = page->m_cpu;
    out->m_gpu = page->m_gpu;
    out->m_offset = page->m_begin;
    out->m_size = page->m_cursor - page->m_begin;
    return out;
}

// CPU scratch memory for writes when the device has failed (FUN_1422d85a0).
static uint8_t* ScratchAllocate(CopyCommandList* list, uint64_t size, uint64_t alignment) {
    if (alignment < 0x11) {
        alignment = 0x10;
    }
    if (list->m_b90 < size - 0x10 + alignment) {
        uint64_t grown = (size + 0xffef + alignment) & 0xffffffffffff0000ull;
        if (grown < 0x100001) {
            grown = 0x100000;
        }
        list->m_b90 = grown;
        void* old = list->m_b88;
        list->m_b88 = nullptr;
        if (old != nullptr) {
            SMemFree(old, "D:\\BuildServer\\A\\work-git\\wow\\Storm\\H\\SMemNew.h", 0xa8, 0);
        }
        list->m_b88 = SMemAlloc(list->m_b90, 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0xdf4, 0);
    }
    return reinterpret_cast<uint8_t*>((reinterpret_cast<uint64_t>(list->m_b88) + alignment - 1) & static_cast<uint64_t>(-static_cast<int64_t>(alignment)));
}

// ---- GraphicsCommandList -----------------------------------------------------------------

// ref: FUN_1422c9f00, FUN_1422cb470
GraphicsCommandList::GraphicsCommandList(uint32_t type, uint8_t vrs) : m_copy(type) {
    this->m_uploadBufs[0] = nullptr;
    this->m_uploadBufs[1] = nullptr;
    this->m_uploadBufs[2] = nullptr;
    std::memset(this->m_boundTextures, 0, sizeof(this->m_boundTextures));
    std::memset(this->m_bb8_unused, 0, sizeof(this->m_bb8_unused));
    this->m_pipelineDirty = 1;
    this->m_heapsDirty = 0;
    this->m_topologyDirty = 0;
    this->m_queryScope = 0;
    this->m_fcc = 0;
    this->m_computeVtable = nullptr;  // FROST: the compute context (vtable 0x14291a070) is not ported
    this->m_fc0 = 0;
    std::memset(&this->m_staging, 0, sizeof(this->m_staging));
    std::memset(this->m_ringPages, 0, sizeof(this->m_ringPages));
    std::memset(this->m_views, 0, sizeof(this->m_views));
    std::memset(this->m_1ab0, 0, sizeof(this->m_1ab0));
    std::memset(this->m_1bb0, 0, sizeof(this->m_1bb0));
    std::memset(this->m_1c50, 0, sizeof(this->m_1c50));
    std::memset(this->m_1cf0, 0, sizeof(this->m_1cf0));
    this->m_1d60 = 0;
    this->m_1d68 = -1;
    this->m_1d70[0] = 0;
    this->m_1d70[1] = 0;
    this->m_staging.Initialize(GxDeviceD3d12()->m_device);
    // FUN_142288710 x3: the vertex, index and constant upload GxBufs.
    this->CreateUploadBufs();
    std::memset(this->m_2080, 0, sizeof(this->m_2080));
    this->m_indexBuf = nullptr;
    this->m_indexFormat = DXGI_FORMAT_R16_UINT;
    this->m_targetsDirty = 1;
    this->m_rtvCount = 0;
    std::memset(this->m_rtvs, 0, sizeof(this->m_rtvs));
    this->m_20c8 = 0;
    this->m_dsv.ptr = 0;
    this->m_dsvPtr = nullptr;
    this->m_pipeline = nullptr;
    this->m_vrs = vrs;
    std::memset(this->m_combiners, 0, sizeof(this->m_combiners));
    std::memset(this->m_wantCombiners, 0, sizeof(this->m_wantCombiners));
    this->m_shadingRate = 0;
    std::memset(this->m_20ee, 0, sizeof(this->m_20ee));
    this->m_stencilRef = 0xffffffff;
    this->m_topology = 0;
    this->m_boundIndexAddress = 0;
    if (this->m_copy.m_signals.size() == 0) {
        this->m_copy.m_signals.emplace_back();
    } else {
        this->m_copy.m_signals.m_size = 1;
    }
}

// ref: FUN_1422cc000
GraphicsCommandList::~GraphicsCommandList() {
    for (void*& buf : this->m_uploadBufs) {
        GxBufDestroy(reinterpret_cast<GxBuf**>(&buf));
    }
}

// ref: FUN_1422ce740
void GraphicsCommandList::BeginQueryScope() {
    this->m_queryScope = 1;
    this->m_pipeline = nullptr;
}

// ref: FUN_1422cea80
void GraphicsCommandList::EndQueryScope() {
    this->m_queryScope = 0;
}

// Clears every stage's bindings.
// ref: FUN_1422e4630
void GraphicsCommandList::ResetBindings() {
    for (uint32_t stage = 0; stage < 4; stage++) {
        for (uint32_t slot : { 1u, 3u }) {
            this->m_staging.m_bound[stage][slot] = 0;
        }
    }
    for (uint32_t stage = 0; stage < 4; stage++) {
        std::memset(reinterpret_cast<uint8_t*>(this) + 0xbb8 + stage * 0x100, 0, 0x100);
        std::memset(this->m_views[stage], 0, sizeof(this->m_views[stage]));
    }
}

// Starts the frame: the list's allocators, the binding caches and the bound targets.
// ref: FUN_1422d4120 (FUN_1422cec20 inline)
void GraphicsCommandList::BeginFrame() {
    this->m_copy.BeginFrame();
    std::memset(this->m_staging.m_bound, 0, sizeof(this->m_staging.m_bound));
    std::memset(this->m_staging.m_dirty, 0, sizeof(this->m_staging.m_dirty));
    std::memset(this->m_staging.m_uploaded, 0xff, sizeof(this->m_staging.m_uploaded));
    std::memset(reinterpret_cast<uint8_t*>(this) + 0xbb8, 0, 0x100);
    std::memset(reinterpret_cast<uint8_t*>(this) + 0xdb8, 0, 0x100);
    std::memset(reinterpret_cast<uint8_t*>(this) + 0xeb8, 0, 0x100);
    std::memset(this->m_1c50 + 0x00, 0, 0x50);
    this->m_pipelineDirty = 1;
    this->m_heapsDirty = 1;
    std::memset(this->m_ringPages, 0, sizeof(this->m_ringPages));
    this->m_topologyDirty = 1;
    this->m_copy.m_af8.m_size = 0;
    this->ResetBindings();
    std::memset(this->m_1ab0, 0, sizeof(this->m_1ab0));
    std::memset(this->m_wantCombiners, 0, sizeof(this->m_wantCombiners));
    this->m_shadingRate = 0;
    std::memset(this->m_20ee, 0, sizeof(this->m_20ee));
    this->m_combiners[0] = 0;
    this->m_combiners[1] = 0;
    this->m_boundIndexAddress = 0;
    this->m_indexBuf = nullptr;
    this->m_2080[0] = 0;
    this->m_2080[1] = 0;
    this->m_pipeline = nullptr;
    this->m_stencilRef = 0xffffffff;
    this->m_targetsDirty = 1;
    this->m_rtvCount = 0;
    this->m_dsvPtr = nullptr;
    this->m_context.Field<uint8_t>(0x74) = 1;
    std::memset(&this->m_context.Field<uint8_t>(0x88), 0, sizeof(GXRENDERTARGETS));
    std::memset(this->m_copy.m_b78 + 8, 0, 8);
    this->m_copy.m_b88 = this->m_copy.m_b88;
    for (UintVector5& signals : this->m_copy.m_signals) {
        signals.m_values.m_size = 0;
    }
}

// ref: FUN_1422ce780
void GraphicsCommandList::FlushBarriers() {
    this->m_copy.FlushBarriers();
    std::memset(this->m_1cf0, 0, sizeof(this->m_1cf0));
    this->m_1d60 = 0;
}

// Clears the masked targets: colours to `color`, the depth plane and stencil as the mask's
// bits 4 and 5 ask.
// ref: FUN_1422d81c0
void GraphicsCommandList::ClearTargets(GXRENDERTARGETS* targets, uint32_t mask, const float* color, float depth, uint8_t stencil) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0) {
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE colors[4];
    uint32_t colorCount = 0;
    TargetView* depthView = nullptr;
    if ((mask & 0xf) != 0) {
        for (uint32_t i = 0; i < 4; i++) {
            TargetView* view = targets->m_views[i + 1];
            if (view == nullptr || (mask & (1u << i)) == 0) {
                continue;
            }
            D3DX12Residency::ManagedObject* residency = TexResidency(view->m_tex);
            if (residency != nullptr) {
                this->m_copy.ResidencySet()->Insert(residency);
            }
            this->m_copy.Transition(&view->m_objects, D3D12_RESOURCE_STATE_RENDER_TARGET);
            colors[colorCount++] = view->m_handle;
        }
    }
    if ((mask & 0x30) != 0) {
        TargetView* view = targets->m_views[0];
        if (view != nullptr) {
            D3DX12Residency::ManagedObject* residency = TexResidency(view->m_tex);
            if (residency != nullptr) {
                this->m_copy.ResidencySet()->Insert(residency);
            }
            this->m_copy.Transition(&view->m_objects, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            depthView = view;
        }
    }
    this->m_copy.FlushBarriers();
    ID3D12GraphicsCommandList* list = this->m_copy.m_list;
    this->m_copy.m_empty = 0;
    for (uint32_t i = 0; i < colorCount; i++) {
        list->ClearRenderTargetView(colors[i], color, 0, nullptr);
    }
    if (depthView != nullptr) {
        list->ClearDepthStencilView(depthView->m_handle, static_cast<D3D12_CLEAR_FLAGS>((mask >> 4) & 3), depth, stencil, 0, nullptr);
    }
}

// Binds the targets: their descriptors, the viewport over them and the target size constants.
// ref: FUN_1422d3090
void GraphicsCommandList::SetTargets(GXRENDERTARGETS* targets) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return;
    }
    if (GxRenderTargetsUpdate(reinterpret_cast<GXRENDERTARGETS*>(&this->m_context.Field<uint8_t>(0x88)), targets)) {
        return;
    }
    if (targets->m_count != 0) {
        int32_t size[2];
        GxRenderTargetsSize(targets, size);
        // The context keeps its target as {x, y, height, width}.
        float viewport[4] = { 0.0f, 0.0f, static_cast<float>(size[1]), static_cast<float>(size[0]) };
        this->m_context.SetViewport(viewport);
        uint32_t count = 0;
        for (; count < 4; count++) {
            TargetView* view = targets->m_views[count + 1];
            if (targets->m_texs[count + 1] == nullptr) {
                break;
            }
            this->m_rtvs[count] = view->m_handle;
            D3DX12Residency::ManagedObject* residency = TexResidency(view->m_tex);
            if (residency != nullptr) {
                this->m_copy.ResidencySet()->Insert(residency);
            }
            this->m_copy.Transition(&view->m_objects, view->m_state);
        }
        this->m_rtvCount = count;
        if (targets->m_texs[0] == nullptr) {
            this->m_dsvPtr = nullptr;
        } else {
            TargetView* view = targets->m_views[0];
            this->m_dsv = view->m_handle;
            this->m_dsvPtr = &this->m_dsv;
            D3DX12Residency::ManagedObject* residency = TexResidency(view->m_tex);
            if (residency != nullptr) {
                this->m_copy.ResidencySet()->Insert(residency);
            }
            this->m_copy.Transition(&view->m_objects, view->m_state);
        }
        GxRenderContextSetTargetConstants(&this->m_context, targets);
    }
    this->m_targetsDirty = 1;
    this->m_context.Field<uint8_t>(0x18) = 1;
    this->m_context.Field<uint8_t>(0x74) = 1;
    this->m_pipeline = nullptr;
}

// Space for `size` bytes of constants for a stage's slot, bound at unlock.
// ref: FUN_1422dff60
float* GraphicsCommandList::LockConstants(uint8_t stage, uint32_t slot, uint32_t size) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0) {
        return reinterpret_cast<float*>(ScratchAllocate(&this->m_copy, size, 0x10));
    }
    UPLOADALLOC alloc;
    UploadAllocate(&this->m_copy, &alloc, size, 0x100);
    if (alloc.m_cpu == nullptr) {
        device->m_ready = 0;
        return reinterpret_cast<float*>(ScratchAllocate(&this->m_copy, size, 0x10));
    }
    auto binding = reinterpret_cast<UPLOADALLOC*>(reinterpret_cast<uint8_t*>(this) + (slot + 0x1d + stage * 0xe) * 0x20);
    *binding = alloc;
    return reinterpret_cast<float*>(alloc.m_cpu + alloc.m_offset);
}

// ref: FUN_1422e0040
void GraphicsCommandList::UnlockConstants(uint8_t stage, uint32_t slot) {
    auto binding = reinterpret_cast<UPLOADALLOC*>(reinterpret_cast<uint8_t*>(this) + (slot + 0x1d + stage * 0xe) * 0x20);
    if (GxDeviceD3d12()->m_ready != 0) {
        this->m_staging.SetConstants(stage, slot, binding->m_gpu + binding->m_offset);
    }
    std::memset(binding, 0, sizeof(UPLOADALLOC));
}

// Copies a region (or all) of one texture into another.
// ref: FUN_1422e14e0
bool GraphicsCommandList::CopyTex(Tex* source, Tex* dest, int32_t* sourceOrigin, int32_t* destOrigin, uint32_t* size,
    uint32_t slice, int32_t mip) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return false;
    }
    int32_t dx = destOrigin[0] - sourceOrigin[0];
    int32_t dy = destOrigin[1] - sourceOrigin[1];
    bool whole = false;
    if (slice == 0 && dest->m_dimension == source->m_dimension && dest->m_depth == 1 && dx == 0 && dy == 0 &&
        sourceOrigin[0] == 0 && sourceOrigin[1] == 0 && size[0] == source->m_width && size[1] == source->m_height &&
        dest->m_width == source->m_width && dest->m_height == source->m_height) {
        whole = true;
    }
    D3DX12Residency::ManagedObject* residency = TexResidency(source);
    if (residency != nullptr) {
        this->m_copy.ResidencySet()->Insert(residency);
    }
    residency = TexResidency(dest);
    if (residency != nullptr) {
        this->m_copy.ResidencySet()->Insert(residency);
    }
    // The states each object is in now, to go back to after the copy.
    StateTrackerMap& map = this->m_copy.m_states[this->m_copy.m_current];
    StateTracker* sourceTracker = map.find(reinterpret_cast<uint64_t>(source->m_objects.data()));
    StateTracker* destTracker = map.find(reinterpret_cast<uint64_t>(dest->m_objects.data()));
    uint32_t sourceState = sourceTracker == nullptr ? D3D12_RESOURCE_STATE_COPY_SOURCE : sourceTracker->m_current;
    uint32_t destState = destTracker == nullptr ? D3D12_RESOURCE_STATE_COPY_DEST : destTracker->m_current;
    bool a = this->m_copy.TransitionObjects(source->m_objects.data(), source->m_objects.size(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    bool b = this->m_copy.TransitionObjects(dest->m_objects.data(), dest->m_objects.size(), D3D12_RESOURCE_STATE_COPY_DEST);
    if (a || b) {
        this->m_copy.FlushBarriers();
    }
    D3D12_TEXTURE_COPY_LOCATION src = {};
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    D3D12_BOX box = {};
    src.pResource = source->m_objects[0].m_resource;
    dst.pResource = dest->m_objects[0].m_resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (whole) {
        src.SubresourceIndex = 0;
        dst.SubresourceIndex = 0;
        box.left = 0;
        box.top = 0;
        box.right = source->m_width;
        box.bottom = source->m_height;
        box.front = static_cast<UINT>(mip);
        box.back = static_cast<UINT>(mip + 1);
        dx = 0;
        dy = 0;
    } else {
        src.SubresourceIndex = slice;
        dst.SubresourceIndex = source->m_mipLevels * static_cast<uint32_t>(mip) + slice;
        box.left = static_cast<UINT>(sourceOrigin[0]);
        box.top = static_cast<UINT>(sourceOrigin[1]);
        box.right = sourceOrigin[0] + size[0];
        box.bottom = size[1] + sourceOrigin[1];
        box.front = 0;
        box.back = 1;
    }
    this->m_copy.m_empty = 0;
    this->m_copy.m_list->CopyTextureRegion(&dst, static_cast<UINT>(dx), static_cast<UINT>(dy), 0, &src, &box);
    if (sourceState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        this->m_copy.TransitionObjects(source->m_objects.data(), source->m_objects.size(), sourceState);
    }
    if (destState != D3D12_RESOURCE_STATE_COPY_DEST) {
        this->m_copy.TransitionObjects(dest->m_objects.data(), dest->m_objects.size(), destState);
    }
    return true;
}

// ---- the render context ------------------------------------------------------------------

GraphicsCommandList* RenderContextD3d12::Owner() {
    return reinterpret_cast<GraphicsCommandList*>(reinterpret_cast<uint8_t*>(this) - offsetof(GraphicsCommandList, m_context));
}

// ref: FUN_1422d3270
void RenderContextD3d12::SetRenderTargets(GXRENDERTARGETS* targets) {
    this->Owner()->SetTargets(targets);
}

// The colour arrives as 0xAARRGGBB.
// ref: FUN_1422d3010
void RenderContextD3d12::Clear(GXRENDERTARGETS* targets, uint16_t mask, uint32_t color, float depth, uint32_t stencil, uint8_t a7) {
    (void)a7;
    uint16_t targetMask = targets->m_mask;
    float rgba[4];
    GxColorToFloat4(rgba, color);  // FUN_141344390
    this->Owner()->ClearTargets(targets, targetMask & mask, rgba, depth, static_cast<uint8_t>(stencil));
    this->Owner()->SetTargets(targets);
}

// ref: FUN_1422e0030
float* RenderContextD3d12::LockConstants(uint8_t stage, uint32_t slot, uint32_t size) {
    return this->Owner()->LockConstants(stage, slot, size);
}

// ref: FUN_1422e00c0
void RenderContextD3d12::UnlockConstants(uint8_t stage, uint32_t slot) {
    this->Owner()->UnlockConstants(stage, slot);
}

// ref: FUN_1422e1870
bool RenderContextD3d12::CopyTex(Tex* source, Tex* dest, int32_t* sourceOrigin, int32_t* destOrigin, uint32_t* size,
    uint32_t slice, int32_t mip) {
    return this->Owner()->CopyTex(source, dest, sourceOrigin, destOrigin, size, slice, mip);
}

// Resolves a multisampled texture into another, putting both back in their states.
// ref: FUN_1422e2b20
bool GraphicsCommandList::ResolveTex(Tex* source, Tex* dest) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return false;
    }
    D3DX12Residency::ManagedObject* residency = TexResidency(source);
    if (residency != nullptr) {
        this->m_copy.ResidencySet()->Insert(residency);
    }
    residency = TexResidency(dest);
    if (residency != nullptr) {
        this->m_copy.ResidencySet()->Insert(residency);
    }
    StateTrackerMap& map = this->m_copy.m_states[this->m_copy.m_current];
    StateTracker* sourceTracker = map.find(reinterpret_cast<uint64_t>(source->m_objects.data()));
    StateTracker* destTracker = map.find(reinterpret_cast<uint64_t>(dest->m_objects.data()));
    uint32_t sourceState = sourceTracker == nullptr ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE : sourceTracker->m_current;
    uint32_t destState = destTracker == nullptr ? D3D12_RESOURCE_STATE_RESOLVE_DEST : destTracker->m_current;
    bool a = this->m_copy.TransitionObjects(source->m_objects.data(), source->m_objects.size(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    bool b = this->m_copy.TransitionObjects(dest->m_objects.data(), dest->m_objects.size(), D3D12_RESOURCE_STATE_RESOLVE_DEST);
    if (a || b) {
        this->m_copy.FlushBarriers();
    }
    this->m_copy.m_empty = 0;
    this->m_copy.m_list->ResolveSubresource(dest->m_objects[0].m_resource, 0, source->m_objects[0].m_resource, 0,
        ShaderFormat(source->m_format));
    if (sourceState != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        this->m_copy.TransitionObjects(source->m_objects.data(), source->m_objects.size(), sourceState);
    }
    if (destState != D3D12_RESOURCE_STATE_RESOLVE_DEST) {
        this->m_copy.TransitionObjects(dest->m_objects.data(), dest->m_objects.size(), destState);
    }
    return true;
}

// ref: FUN_1422e2d40
bool RenderContextD3d12::ResolveTex(Tex* source, Tex* dest) {
    return this->Owner()->ResolveTex(source, dest);
}

// ---- buffers -------------------------------------------------------------------------------

// The greatest common divisor (FUN_1422c9620).
static uint32_t BufGcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// Space for a buffer's contents in this frame's upload memory, aligned to its elements; the
// buffer reads from there until the next lock.
// ref: FUN_1422d4890
static void* BufLockUpload(CopyCommandList* list, GxBuf* buf) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0 || (buf->m_heap == nullptr && buf->m_type != GXBUF_DYNAMIC)) {
        device->m_ready = 0;
        return ScratchAllocate(list, buf->m_size, 0x10);
    }
    uint32_t alignment = (buf->m_usage & GXBUFUSAGE_CONSTANT) != 0 ? 0x100 : 8;
    uint32_t element = (buf->m_flags >> GXBUF_ELEMENT_SHIFT) & 0xfff;
    uint32_t common = BufGcd(alignment, element % alignment);
    UPLOADALLOC alloc;
    UploadAllocate(list, &alloc, ((alignment - 1) + (buf->m_size - common) + element) & static_cast<uint32_t>(-static_cast<int32_t>(alignment)),
        alignment);
    if (alloc.m_cpu == nullptr) {
        device->m_ready = 0;
        return ScratchAllocate(list, buf->m_size, 0x10);
    }
    uint32_t offset = alloc.m_offset + element - 1;
    offset -= offset % element;
    buf->m_offset = offset;
    buf->m_resource = alloc.m_resource;
    buf->m_gpu = alloc.m_gpu + offset;
    return alloc.m_cpu + offset;
}

// A static or stream buffer's written range is queued for the copy into its heap.
// ref: FUN_1422d49b0
void BufUnlockUpload(CopyCommandList* list, GxBuf* buf, uint32_t size) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0) {
        buf->m_flags |= GXBUF_WRITTEN;
        return;
    }
    if (buf->m_type == GXBUF_STATIC || buf->m_type == GXBUF_STREAM) {
        if (size == 0) {
            size = buf->m_size;
        }
        // DAT_14348c0d3 and the heap's +0xa0: a copy is pending.
        device->m_copiesPending = 1;
        FROST_UNIMPLEMENTED();  // the heap's own pending flag (+0xa0) comes with the buffer heaps
        list->m_bufs.push_back(BufPair { buf, size, 0 });
    }
    buf->m_flags |= GXBUF_WRITTEN;
}

// ref: FUN_1422cd800
void* BufLockList(CopyCommandList* list, GxBuf* buf) {
    if (buf->m_type == GXBUF_STREAM) {
        // A stream buffer moves to a fresh range of its heap; the old range is freed when the
        // frame is done (LAB_1422c7c40).
        FROST_UNIMPLEMENTED();
        buf->m_flags &= ~GXBUF_UNLOCKED;
        GxDeviceD3d12()->BufAllocate(buf);
    }
    return BufLockUpload(list, buf);
}

// The transient buffers each context writes its dynamic geometry and constants into
// (vertices 4 MB, indices 2 MB, constants 64 KB).
void GraphicsCommandList::CreateUploadBufs() {
    GXBUFDESC desc = {};
    desc.m_type = GXBUF_DYNAMIC;
    desc.m_usage = 0x1802;
    desc.m_size = 0x400000;
    desc.m_elementSize = 1;
    this->m_uploadBufs[0] = GxBufCreate(&desc);
    desc = {};
    desc.m_type = GXBUF_DYNAMIC;
    desc.m_usage = 0x1801;
    desc.m_size = 0x200000;
    desc.m_elementSize = 1;
    this->m_uploadBufs[1] = GxBufCreate(&desc);
    desc = {};
    desc.m_type = GXBUF_DYNAMIC;
    desc.m_usage = 0x1804;
    desc.m_size = 0x10000;
    desc.m_elementSize = 1;
    this->m_uploadBufs[2] = GxBufCreate(&desc);
}

// ref: FUN_1422cd8e0
void* RenderContextD3d12::BufLock(GxBuf* buf) {
    return BufLockList(&this->Owner()->m_copy, buf);
}

// ref: FUN_1422cd9f0
bool RenderContextD3d12::BufUnlock(GxBuf* buf, uint32_t size) {
    BufUnlockUpload(&this->Owner()->m_copy, buf, size);
    buf->m_flags |= GXBUF_UNLOCKED;
    return true;
}

// ref: FUN_1422d40c0
GxBuf* RenderContextD3d12::GetUploadBuf(uint8_t type) {
    return static_cast<GxBuf*>(this->Owner()->m_uploadBufs[type]);
}
