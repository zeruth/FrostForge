#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxPlatform.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <algorithm>
#include <cstring>
#include <new>

uint32_t g_gxPresentSpin;

using namespace gxDx12Backend;

CGxDeviceD3d12* GxDeviceD3d12();

namespace {

const char s_deviceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\D3d12\\GxDeviceD3d12.cpp";

// The DXGI formats of the swap chain formats (DAT_142919c50).
const uint32_t s_swapChainFormats[4] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS };

// The bytes a query's result takes, by query type (DAT_1429163e8).
const uint64_t s_queryResultSize[2] = { 8, 0x10 };

// The four most recent presents' statistics, indexed by present count (DAT_143491350).
struct PRESENTSTATS {
    uint8_t m_done;
    uint8_t m_pad[3];
    uint32_t m_presentCount;
    uint64_t m_submitted;
    uint64_t m_shown;
};
PRESENTSTATS s_presentStats[4];

// The IDXGISwapChain::Present entry recorded at the first swap chain, and its first bytes,
// to notice an overlay patching it (DAT_143490ae0, DAT_143490ae8, DAT_143490a68).
void** s_presentEntry;
uint8_t s_presentCode[0x20];
uint8_t s_presentRecorded;
uint8_t s_presentModified;          // DAT_143490a69
uint8_t s_deviceRemovedPending;     // DAT_143490a6a

template <class T>
void ReleaseAndNull(T** object) {
    if (*object != nullptr) {
        (*object)->Release();
        *object = nullptr;
    }
}

// A GPU timer or occlusion query (0x20 bytes).
struct QUERY {
    uint8_t m_type;                 // 0x00: 0 occlusion, 1 timestamp pair
    uint8_t m_pad01[7];
    uint64_t m_result[2];           // 0x08
    uint8_t m_state;                // 0x18: 1 issued, 2 result read, 3 lost
    uint8_t m_index;                // 0x19: its slot in this frame's query heap
    uint8_t m_pad1a[6];
};

uint64_t TicksToNanoseconds(uint64_t ticks) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    uint64_t f = static_cast<uint64_t>(frequency.QuadPart);
    return ((ticks % f) * 1000000000) / f + (ticks / f) * 1000000000;
}

}

// Records the swap chain's Present entry and code once (FUN_1422958b0).
static void RecordPresentEntry(IDXGISwapChain3* chain) {
    if (s_presentRecorded != 0 || OsGetVersion()->m_wine != 0) {
        return;
    }
    s_presentEntry = *reinterpret_cast<void***>(chain) + 8;
    if (s_presentEntry != nullptr) {
        std::memcpy(s_presentCode, *s_presentEntry, sizeof(s_presentCode));
    }
    s_presentRecorded = 1;
}

// Presents one swap chain, putting back a patched Present after a device removal.
// ref: FUN_142295d70
static void PresentSwapChain(CGxDeviceD3d12* device, IDXGISwapChain3* chain, UINT syncInterval, UINT flags) {
    void** entry = s_presentEntry;
    if (s_presentModified == 0) {
        bool moved = entry != nullptr && entry != *reinterpret_cast<void***>(chain) + 8;
        if (!moved && entry != nullptr && std::memcmp(*entry, s_presentCode, sizeof(s_presentCode)) != 0) {
            GxLog("Detected SwapChain::Present Modified");
            s_presentModified = 1;
        }
    }
    if (s_deviceRemovedPending != 0 && entry != nullptr) {
        const char* message = nullptr;
        if (entry == *reinterpret_cast<void***>(chain) + 8) {
            DWORD protect;
            if (std::memcmp(*entry, s_presentCode, sizeof(s_presentCode)) != 0 &&
                VirtualProtect(*entry, sizeof(s_presentCode), PAGE_EXECUTE_READWRITE, &protect)) {
                std::memcpy(*entry, s_presentCode, sizeof(s_presentCode));
                DWORD previous;
                VirtualProtect(*entry, sizeof(s_presentCode), protect, &previous);
                FlushInstructionCache(GetCurrentProcess(), *entry, sizeof(s_presentCode));
                message = "Received DeviceRemoved:::";
            }
        } else {
            message = "Received DeviceRemoved!!!";
        }
        if (message != nullptr) {
            GxLog(message);
            s_deviceRemovedPending = 0;
        }
    }
    s_deviceRemovedPending = 0;
    HRESULT hr = chain->Present(syncInterval, flags);
    g_gxPresentSpin = 0;
    if (FAILED(hr)) {
        device->Field<uint8_t>(0x149) = 0;
        const char* text = hr == DXGI_ERROR_DEVICE_REMOVED ? "DeviceRemoved" : "DeviceError";
        s_deviceRemovedPending = 1;
        device->m_ready = 0;
        device->ReportError(hr, text);
    } else {
        device->Field<uint8_t>(0x149) = 1;
    }
}

// ---- swap chains -------------------------------------------------------------------------

// ref: FUN_1422e1100
void* CGxDeviceD3d12::SwapChainCreate(const GXSWAPCHAINPARAMS* gxParams) {
    if (this->m_ready == 0) {
        return nullptr;
    }
    auto params = reinterpret_cast<const SWAPCHAINPARAMS*>(gxParams);
    void* memory = SMemAlloc(sizeof(SwapChain), 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0xcf1, 0);
    auto chain = new (memory) SwapChain();
    chain->m_window = params->m_window;
    chain->m_width = 0;
    chain->m_height = 0;
    chain->m_requestWidth = params->m_width;
    chain->m_requestHeight = params->m_height;
    chain->m_texFormat = params->m_format == 0 ? 2 : 0x14;
    chain->m_vsync = params->m_vsync;
    chain->m_primary = params->m_primary;
    chain->m_swapChain = nullptr;
    chain->m_758 = nullptr;
    chain->m_current = 0;
    chain->m_dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    this->m_swapChains.push_back(chain);
    if (!this->CreateSwapChain(chain, params)) {
        this->SwapChainDestroy(chain);
        chain = nullptr;
    }
    if (params->m_primary != 0) {
        this->m_primaryChain = chain;
    }
    return chain;
}

// ref: FUN_1422ebf50
bool CGxDeviceD3d12::CreateSwapChain(SwapChain* chain, const SWAPCHAINPARAMS* params) {
    if (params->m_width == 0) {
        RECT rect;
        if (!GetClientRect(chain->m_window, &rect)) {
            chain->m_width = 1;
            chain->m_height = 1;
        } else {
            chain->m_width = static_cast<uint16_t>(rect.right != 0 ? rect.right : 1);
            chain->m_height = static_cast<uint16_t>(rect.bottom != 0 ? rect.bottom : 1);
        }
    } else {
        chain->m_width = params->m_width;
        chain->m_height = params->m_height;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = chain->m_width;
    desc.Height = chain->m_height;
    desc.Format = static_cast<DXGI_FORMAT>(s_swapChainFormats[params->m_format]);
    chain->m_dxgiFormat = desc.Format;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = static_cast<UINT>(this->m_frames.size());
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = this->m_tearing != 0 ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* created = nullptr;
    HRESULT hr = GxPlatformFactory5()->CreateSwapChainForHwnd(this->m_directQueue.m_queue, chain->m_window, &desc, nullptr,
        nullptr, &created);
    bool result = false;
    if (FAILED(hr)) {
        this->Fail(hr, "CreateSwapChainForHwnd failed");
    } else {
        ReleaseAndNull(&chain->m_swapChain);
        hr = created->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&chain->m_swapChain));
        if (FAILED(hr)) {
            this->Fail(hr, "SwapChain QueryInferface failed");
        } else {
            RecordPresentEntry(chain->m_swapChain);
            hr = GxPlatformFactory5()->MakeWindowAssociation(chain->m_window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
            if (FAILED(hr)) {
                this->ReportError(hr, "MakeWindowAssociation failed");
            }
            result = this->CreateBackBuffers(chain);
        }
    }
    ReleaseAndNull(&created);
    return result;
}

// One Tex per swap chain buffer, its render target view over the buffer.
// ref: FUN_1422ebb60
bool CGxDeviceD3d12::CreateBackBuffers(SwapChain* chain) {
    if (chain == nullptr || chain->m_swapChain == nullptr) {
        return true;
    }
    DXGI_SWAP_CHAIN_DESC desc;
    chain->m_swapChain->GetDesc(&desc);
    TEXDESC texDesc = {};
    texDesc.m_dimension = 2;
    texDesc.m_width = chain->m_width;
    texDesc.m_height = chain->m_height;
    texDesc.m_depth = 1;
    texDesc.m_format = 2;
    texDesc.m_11 = 2;
    texDesc.m_usage = 0x10042001;
    texDesc.m_usage2 = 0;
    for (uint32_t i = 0; i < desc.BufferCount; i++) {
        uint32_t id = static_cast<uint32_t>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->Field<uint32_t>(0x468))) - 1);
        chain->m_buffers.emplace_back(&texDesc, id);
        ID3D12Resource* buffer = nullptr;
        HRESULT hr = chain->m_swapChain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffer));
        if (FAILED(hr)) {
            this->Fail(hr, "SwapChain GetBuffer failed");
            return false;
        }
        if (!this->TexCreate(&texDesc, &chain->m_buffers[i], false, buffer)) {
            this->m_ready = 0;
            GxLog("SwapChain tex create failed");
            return false;
        }
    }
    return true;
}

// ref: FUN_1422ebe80
void CGxDeviceD3d12::ReleaseBackBuffers(SwapChain* chain) {
    g_gxDeviceLock.Enter();
    for (Tex& buffer : chain->m_buffers) {
        this->TexReleaseViews(&buffer);
        this->TexRelease(&buffer);
    }
    chain->m_buffers.clear();
    if (chain->m_primary != 0) {
        this->Field<uint64_t>(0x150) = 0;
        this->Field<uint8_t>(0x14e) = 0;
        for (PRESENTSTATS& stats : s_presentStats) {
            stats.m_done = 1;
        }
    }
    g_gxDeviceLock.Leave();
}

// ref: FUN_1422e1270
void CGxDeviceD3d12::SwapChainDestroy(void* handle) {
    auto chain = static_cast<SwapChain*>(handle);
    if (chain == nullptr) {
        return;
    }
    g_gxDeviceLock.Enter();
    if (this->m_primaryChain == chain) {
        this->m_primaryChain = nullptr;
    }
    // The swap chain and its resources go when the frame does.
    IUnknown* swapChain = chain->m_swapChain;
    chain->m_swapChain = nullptr;
    g_gxDeviceLock.Enter();
    this->m_frame->m_releases.push_back(swapChain);
    g_gxDeviceLock.Leave();
    this->ReleaseBackBuffers(chain);
    IUnknown* resource = chain->m_758;
    chain->m_758 = nullptr;
    g_gxDeviceLock.Enter();
    this->m_frame->m_releases.push_back(resource);
    g_gxDeviceLock.Leave();
    for (IUnknown*& object : chain->m_760) {
        IUnknown* released = object;
        object = nullptr;
        g_gxDeviceLock.Enter();
        this->m_frame->m_releases.push_back(released);
        g_gxDeviceLock.Leave();
    }
    for (uint64_t i = 0; i < this->m_swapChains.size(); i++) {
        if (this->m_swapChains[i] == chain) {
            this->m_swapChains.erase_unordered(i);
            break;
        }
    }
    chain->~SwapChain();
    operator delete(chain, sizeof(SwapChain));
    g_gxDeviceLock.Leave();
}

// Starts drawing into a swap chain, resizing its buffers to the window first.
// ref: FUN_1422e0f80
bool CGxDeviceD3d12::SwapChainBegin(void* handle) {
    if (this->m_ready == 0) {
        return false;
    }
    auto chain = static_cast<SwapChain*>(handle);
    this->m_currentChain = chain;
    uint16_t width;
    uint16_t height;
    if (chain->m_requestWidth == 0) {
        RECT rect;
        if (!GetClientRect(chain->m_window, &rect)) {
            width = 1;
            height = 1;
        } else {
            width = static_cast<uint16_t>(rect.right != 0 ? rect.right : 1);
            height = static_cast<uint16_t>(rect.bottom != 0 ? rect.bottom : 1);
        }
        if (width == chain->m_width && height == chain->m_height) {
            return true;
        }
    } else {
        if (chain->m_width == chain->m_requestWidth && chain->m_height == chain->m_requestHeight) {
            return true;
        }
        width = chain->m_requestWidth;
        height = chain->m_requestHeight;
    }
    chain->m_width = width;
    chain->m_height = height;
    this->WaitForGpu();
    this->ReleaseBackBuffers(chain);
    if (chain->m_swapChain != nullptr) {
        HRESULT hr = chain->m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
            this->m_tearing != 0 ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(hr)) {
            this->Fail(hr, "ResizeBuffers failed");
            return false;
        }
    }
    if (this->CreateBackBuffers(chain)) {
        return true;
    }
    this->ReleaseBackBuffers(chain);
    return false;
}

// ref: FUN_1422eb860
void* CGxDeviceD3d12::SwapChainGetBackBuffer(void* handle) {
    auto chain = static_cast<SwapChain*>(handle);
    uint16_t index = static_cast<uint16_t>(chain->m_swapChain->GetCurrentBackBufferIndex());
    chain->m_current = index;
    return &chain->m_buffers[index];
}

// Queues the swap chain for presenting at the end of the frame, its buffer back to PRESENT.
// ref: FUN_1422e1490
void CGxDeviceD3d12::SwapChainPresent(void* handle) {
    auto chain = static_cast<SwapChain*>(handle);
    this->m_presents.push_back(chain);
    Tex& buffer = chain->m_buffers[chain->m_current];
    this->m_graphicsPtr->m_copy.TransitionObjects(buffer.m_objects.data(), buffer.m_objects.size(), D3D12_RESOURCE_STATE_PRESENT);
}

// ref: FUN_1422e1480
void CGxDeviceD3d12::SwapChainEnd(void* chain) {
    (void)chain;
    this->m_currentChain = nullptr;
}

// Presents the queued swap chains, with the latency markers around them.
// ref: FUN_1422ec1d0
void CGxDeviceD3d12::PresentAll() {
    // FUN_1422df780(4/5): the NVIDIA Reflex present markers. DIVERGED: NVAPI is not vendored.
    for (SwapChain* chain : this->m_presents) {
        if (this->m_ready == 0) {
            return;
        }
        UINT flags = 0;
        if (this->m_tearing != 0 && chain->m_vsync == 0) {
            flags = DXGI_PRESENT_ALLOW_TEARING;
        }
        PresentSwapChain(this, chain->m_swapChain, chain->m_vsync, flags);
        if (chain->m_primary != 0) {
            UINT count = 0;
            chain->m_swapChain->GetLastPresentCount(&count);
            PRESENTSTATS& stats = s_presentStats[count & 3];
            stats.m_presentCount = count;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            stats.m_submitted = TicksToNanoseconds(static_cast<uint64_t>(now.QuadPart));
            stats.m_shown = 0;
            stats.m_done = 0;
        }
    }
    this->m_presents.m_size = 0;
}

// When the primary chain's presents were shown, and how many are still queued.
// ref: FUN_1422ec360
void CGxDeviceD3d12::UpdateFrameStatistics() {
    if (this->m_primaryChain == nullptr) {
        return;
    }
    DXGI_FRAME_STATISTICS stats;
    HRESULT hr = this->m_primaryChain->m_swapChain->GetFrameStatistics(&stats);
    if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        this->Field<uint64_t>(0x150) = 0;
        this->Field<uint8_t>(0x14e) = 0;
        for (PRESENTSTATS& entry : s_presentStats) {
            entry.m_done = 1;
        }
        return;
    }
    if (FAILED(hr)) {
        return;
    }
    uint32_t count = stats.PresentCount;
    PRESENTSTATS& entry = s_presentStats[count & 3];
    uint8_t queued = 0;
    if (entry.m_presentCount == count) {
        if (entry.m_done == 1) {
            return;
        }
        uint64_t shown = TicksToNanoseconds(static_cast<uint64_t>(stats.SyncQPCTime.QuadPart));
        entry.m_shown = shown;
        entry.m_done = 1;
        if (entry.m_submitted < shown) {
            this->Field<uint64_t>(0x150) = shown - entry.m_submitted;
        }
    } else {
        queued = entry.m_done == 0;
    }
    for (uint32_t back = 1; back < 4 && back <= count; back++) {
        PRESENTSTATS& older = s_presentStats[(count - back) & 3];
        bool pending = false;
        if (older.m_done == 0) {
            if (older.m_presentCount < count) {
                older.m_done = 1;
            }
            pending = older.m_presentCount >= count;
        }
        queued = static_cast<uint8_t>(queued + pending);
    }
    this->Field<uint8_t>(0x14e) = queued;
}

// ---- queries -----------------------------------------------------------------------------

// Device slot 0x278 (FUN_1422dd7c0).
void* CGxDeviceD3d12::QueryCreate(uint8_t type) {
    auto query = static_cast<QUERY*>(SMemAlloc(sizeof(QUERY), 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0xf92, 0));
    query->m_type = type;
    query->m_result[0] = 0;
    query->m_result[1] = 0;
    query->m_state = 0;
    query->m_index = 0;
    return query;
}

static void QueryFree(void* param) {
    void* query = *static_cast<void**>(param);
    SMemFree(query, s_deviceFile, -2, 0);
}

// Device slot 0x280: freed once the frame is done with it (FUN_1422dd830).
void CGxDeviceD3d12::QueryDestroy(void* query) {
    FrameData* frame = this->m_frame;
    auto lock = static_cast<SCritSect*>(frame->m_callbacks.m_lock);
    lock->Enter();
    auto slot = static_cast<void**>(frame->m_callbacks.Add(QueryFree, sizeof(void*), 8));
    lock->Leave();
    if (slot != nullptr) {
        *slot = query;
    }
}

// Device slot 0x288: a query's result size, or whether its result is in (FUN_1422dd930).
bool CGxDeviceD3d12::QueryInfo(void* handle, uint8_t what, uint32_t* out) {
    if (this->m_ready == 0) {
        return false;
    }
    auto query = static_cast<QUERY*>(handle);
    if (what == 0) {
        *out = static_cast<uint32_t>(s_queryResultSize[query->m_type]);
        return true;
    }
    if (what == 1) {
        *out = query->m_state == 2;
        return query->m_state != 3;
    }
    return false;
}

// Device slot 0x198 (FUN_1422a8d50).
uint32_t CGxDeviceD3d12::FrameCount() {
    return static_cast<uint32_t>(this->m_frames.size());
}

// Device slot 0x80, after the frame went to the GPU (FUN_1422db7e0): the latency marker
// (FUN_14234d180; DIVERGED: NVAPI is not vendored) and the present statistics.
void CGxDeviceD3d12::FrameMarker() {
    this->UpdateFrameStatistics();
}

// Device slot 0x88 (FUN_1422df780): the NVIDIA Reflex markers. DIVERGED: NVAPI is not vendored.
void CGxDeviceD3d12::LatencyMarker(uint32_t marker, uint32_t frame) {
    (void)marker;
    (void)frame;
}

// Device slot 0x290: copies a read result out (FUN_1422b8750).
bool CGxDeviceD3d12::QueryGetResult(void* handle, void* out, uint32_t size) {
    auto query = static_cast<QUERY*>(handle);
    if (query->m_state != 2) {
        return false;
    }
    std::memcpy(out, query->m_result, size);
    return true;
}

// Starts a query on the graphics list: occlusion begins, a timer writes its first stamp.
// ref: FUN_1422dd560
static bool QueryBegin(GraphicsCommandList* graphics, QUERY* query) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0) {
        return false;
    }
    ID3D12GraphicsCommandList* list = graphics->m_copy.m_list;
    graphics->m_copy.m_empty = 0;
    g_gxDeviceLock.Enter();
    uint8_t index = device->m_queryFlags[query->m_type];
    device->m_queryFlags[query->m_type] = static_cast<uint8_t>(index + 1);
    query->m_index = index;
    device->m_frame->m_queries.push_back(query);
    g_gxDeviceLock.Leave();
    if (query->m_type != 0) {
        list->EndQuery(device->m_queryHeaps[query->m_type], D3D12_QUERY_TYPE_TIMESTAMP, query->m_index * 2u);
        return true;
    }
    list->BeginQuery(device->m_queryHeaps[0], D3D12_QUERY_TYPE_OCCLUSION, 0);
    return true;
}

// ref: FUN_1422dd8a0
static bool QueryEnd(GraphicsCommandList* graphics, QUERY* query) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0) {
        return false;
    }
    graphics->m_copy.m_empty = 0;
    ID3D12GraphicsCommandList* list = graphics->m_copy.m_list;
    if (query->m_type == 0) {
        list->EndQuery(device->m_queryHeaps[0], D3D12_QUERY_TYPE_OCCLUSION, query->m_index);
    } else {
        list->EndQuery(device->m_queryHeaps[query->m_type], D3D12_QUERY_TYPE_TIMESTAMP, query->m_index * 2u + 1);
    }
    query->m_state = 1;
    return true;
}

// ---- the frame ---------------------------------------------------------------------------

// Device slot 0x180: the frame's start, with its timer query.
// ref: FUN_1422d2c20
bool CGxDeviceD3d12::FrameBegin(void* query) {
    if (this->m_ready == 0) {
        return false;
    }
    this->SubmitBegin();
    return QueryBegin(this->m_graphicsPtr, static_cast<QUERY*>(query));
}

namespace {

// The pages fullest first, so a range comes from the fullest page with room (FUN_1422c7e80
// for buffer pages, FUN_1422c8290 for texture heap pages: introsorts by free bytes).
template <class Page, const char* Tag>
void SortByFree(blz::vector<Page*, Tag>& pages) {
    std::sort(pages.begin(), pages.end(), [](const Page* a, const Page* b) {
        return a->m_allocations.m_freeSize < b->m_allocations.m_freeSize;
    });
}

// The empty pages at the end of a sorted list go; returns the bytes they held.
// ref: FUN_1422c7690
uint64_t TrimBufferPages(blz::vector<BufferPage*, s_bufferPageTag>& pages) {
    uint64_t freed = 0;
    while (pages.size() != 0) {
        BufferPage* page = pages[pages.size() - 1];
        if (page->m_allocations.m_freeSize != page->m_allocations.m_maxSize) {
            break;
        }
        freed += page->m_allocations.m_maxSize;
        pages.m_size--;
        page->~BufferPage();
        operator delete(page, sizeof(BufferPage));
    }
    return freed;
}

// ref: FUN_1422c76f0
uint64_t TrimHeapPages(blz::vector<HeapPage*, s_heapPageTag>& pages) {
    uint64_t freed = 0;
    while (pages.size() != 0) {
        HeapPage* page = pages[pages.size() - 1];
        if (page->m_allocations.m_freeSize != page->m_allocations.m_maxSize) {
            break;
        }
        freed += page->m_allocations.m_maxSize;
        pages.m_size--;
        page->~HeapPage();
        operator delete(page, 0xb8);
    }
    return freed;
}

}

// The buffer pages and texture heap pages in allocation order (inline in FUN_1422db9f0,
// FUN_1422d2a30 and FUN_1422e00d0).
void CGxDeviceD3d12::SortPages() {
    SortByFree(this->m_vertexPages);
    SortByFree(this->m_indexPages);
    SortByFree(this->m_otherPages);
    SortByFree(this->m_heapPages);
}

// The empty pages are released (inline in FUN_1422db9f0 and FUN_1422e00d0).
uint32_t CGxDeviceD3d12::TrimPages() {
    TrimBufferPages(this->m_vertexPages);
    TrimBufferPages(this->m_indexPages);
    TrimBufferPages(this->m_otherPages);
    uint32_t freed = static_cast<uint32_t>(TrimHeapPages(this->m_heapPages));
    this->m_placedBytes -= freed;
    return freed;
}

// Device slot 0x10: after a texture reload the pages are sorted and the empty ones released.
// The texture heap defragmenter that follows (FUN_1422d0150) is not ported.
// ref: FUN_1422e00d0
void CGxDeviceD3d12::Slot010() {
    if (this->m_ready == 0) {
        return;
    }
    g_gxDeviceLock.Enter();
    GxDeviceCallLock lock;
    this->SortPages();
    this->TrimPages();
    g_gxDeviceLock.Leave();
}

// Takes the video memory reading and trims the transient pools when the budget allows.
// ref: FUN_1422db9f0
void CGxDeviceD3d12::SubmitBegin() {
    g_gxDeviceLock.Enter();
    GxDeviceCallLock lock;
    this->QueryVideoMemory();
    if ((this->Field<uint8_t>(0x86b) == 0 && this->m_localMemory.CurrentUsage <= this->m_localMemory.Budget) ||
        this->Field<uint32_t>(0x138) <= static_cast<uint32_t>(this->m_548) + 10000u) {
        this->SortPages();
        if (this->Field<uint32_t>(0x138) % 300 == 0) {
            this->TrimPages();
        }
    } else {
        if (this->Field<uint8_t>(0x86b) != 0) {
            GxLog("Low Render Target threshold reached");
        }
        this->m_548 = this->Field<uint32_t>(0x138);
        GxNotifyOnLowMemory();
        this->Flush();
        if (this->m_ready != 0) {
            g_gxDeviceLock.Enter();
            GxDeviceCallLock flushLock;
            this->SortPages();
            this->TrimPages();
            // The texture heap defragmenter (FUN_1422d0150) is not ported.
            g_gxDeviceLock.Leave();
        }
        this->Field<uint8_t>(0x86b) = 0;
    }
    g_gxDeviceLock.Leave();
}

// Submits the frame and starts the next one.
// ref: FUN_1422d3280
void* CGxDeviceD3d12::FrameEnd(void* out, void* query) {
    AcquireSRWLockExclusive(&this->m_submitLock);
    while (this->m_submitPending != 0) {
        SleepConditionVariableSRW(&this->m_submitCondition, &this->m_submitLock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&this->m_submitLock);
    g_gxDeviceLock.Enter();
    GxDeviceCallLock lock;
    QueryEnd(this->m_graphicsPtr, static_cast<QUERY*>(query));
    this->m_frame->m_queryPending[0] = this->m_queryFlags[0];
    this->m_frame->m_queryPending[1] = this->m_queryFlags[1];
    GraphicsCommandList* graphics = this->m_graphicsPtr;
    graphics->m_copy.FlushBarriers();
    if (this->m_frame->m_queryPending[0] != 0) {
        graphics->m_copy.m_empty = 0;
        graphics->m_copy.m_list->ResolveQueryData(this->m_queryHeaps[0], D3D12_QUERY_TYPE_OCCLUSION, 0,
            this->m_frame->m_queryPending[0], this->m_frame->m_queryReadback[0], 0);
    }
    if (this->m_frame->m_queryPending[1] != 0) {
        graphics->m_copy.m_empty = 0;
        graphics->m_copy.m_list->ResolveQueryData(this->m_queryHeaps[1], D3D12_QUERY_TYPE_TIMESTAMP, 0,
            this->m_frame->m_queryPending[1] * 2u, this->m_frame->m_queryReadback[1], 0);
    }
    this->FrameSubmit();
    uint32_t draws = 0;
    for (CopyCommandList* list : this->m_activeLists) {
        draws += list->m_18;
        list->m_18 = 0;
    }
    this->Field<uint32_t>(0x158) = draws;
    this->m_frameIndex = static_cast<uint8_t>((this->m_frameIndex + 1u) % this->m_frames.size());
    this->m_frame = &this->m_frames[this->m_frameIndex];
    this->m_submitPending = 0;
    this->WaitForFrame(out);
    this->StartFrame();
    g_gxDeviceLock.Leave();
    return out;
}

// Waits until the GPU is done with the frame that is next to be reused; out is the time spent.
// ref: FUN_1422e62a0
void* CGxDeviceD3d12::WaitForFrame(void* out) {
    uint8_t index = this->m_frameIndex;
    auto done = [index](Queue& queue) {
        return queue.m_frameFences[index] <= queue.m_fence->GetCompletedValue();
    };
    if (done(this->m_copyQueue) && done(this->m_computeQueue) && done(this->m_directQueue)) {
        *static_cast<uint64_t*>(out) = 0;
        return out;
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    if (!done(this->m_copyQueue)) {
        this->m_copyQueue.WaitForValue(this->m_copyQueue.m_frameFences[this->m_frameIndex]);
    }
    if (!done(this->m_computeQueue)) {
        this->m_computeQueue.WaitForValue(this->m_computeQueue.m_frameFences[this->m_frameIndex]);
    }
    if (!done(this->m_directQueue)) {
        this->m_directQueue.WaitForValue(this->m_directQueue.m_frameFences[this->m_frameIndex]);
    }
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    *static_cast<uint64_t*>(out) = TicksToNanoseconds(static_cast<uint64_t>(end.QuadPart - start.QuadPart));
    return out;
}

// The new frame: the finished frame's query results, its upload pages back, its releases and
// callbacks, and every list reset onto its allocators.
// ref: FUN_1422de840
void CGxDeviceD3d12::StartFrame() {
    FrameData* frame = this->m_frame;
    void* mapped[2] = {};
    for (uint32_t i = 0; i < 2; i++) {
        if (frame->m_queryPending[i] != 0) {
            HRESULT hr = frame->m_queryReadback[i]->Map(0, nullptr, &mapped[i]);
            if (FAILED(hr)) {
                this->Fail(hr, "Map failed");
                break;
            }
        }
    }
    for (void* entry : frame->m_queries) {
        auto query = static_cast<QUERY*>(entry);
        if (mapped[query->m_type] != nullptr) {
            std::memcpy(query->m_result, static_cast<uint8_t*>(mapped[query->m_type]) + query->m_index * s_queryResultSize[query->m_type],
                s_queryResultSize[query->m_type]);
        }
        query->m_state = 2;
    }
    frame->m_queries.m_size = 0;
    D3D12_RANGE none = { 0, 0 };
    if (frame->m_queryPending[0] != 0) {
        frame->m_queryReadback[0]->Unmap(0, &none);
    }
    frame->m_queryPending[0] = 0;
    this->m_queryFlags[0] = 0;
    if (frame->m_queryPending[1] != 0) {
        frame->m_queryReadback[1]->Unmap(0, &none);
    }
    frame->m_queryPending[1] = 0;
    this->m_queryFlags[1] = 0;

    // Each list's pages of this frame go back to the free lists, its large uploads are dropped.
    for (CopyCommandList* list : this->m_activeLists) {
        list->m_page = nullptr;
        PerFrameResources* resources = &list->m_frames[this->m_frameIndex];
        for (DataUploadPage& page : resources->m_largePages) {
            ReleaseAndNull(&page.m_resource);
        }
        resources->m_largePages.m_size = 0;
        for (uint32_t i = 0; i < 4; i++) {
            for (DescriptorUploadPage& page : resources->m_uploadPages[i]) {
                this->m_freeUploadPages[i].push_back(page);
            }
            resources->m_uploadPages[i].m_size = 0;
            resources->m_currentPages[i] = nullptr;
        }
        for (DescriptorUploadPage& page : resources->m_uploadPages[4]) {
            IUnknown* heap = page.m_heap;
            page.m_heap = nullptr;
            g_gxDeviceLock.Enter();
            frame->m_releases.push_back(heap);
            g_gxDeviceLock.Leave();
        }
        resources->m_uploadPages[4].m_size = 0;
    }
    // FUN_1422c9950 / FUN_1422e7410: the samplers released last frame go back on their heap.
    this->ReleaseOldSamplers();
    this->ReleaseUploadBuffers();
    GraphicsCommandList* graphics = this->m_graphicsPtr;
    graphics->BeginFrame();
    graphics->m_copy.Reset();
    if (graphics->m_copy.m_list != nullptr) {
        graphics->m_copy.m_list->SetGraphicsRootSignature(this->m_rootSignatures[0]);
        graphics->m_copy.m_list->SetComputeRootSignature(this->m_rootSignatures[1]);
    }
    if (this->m_ready != 0) {
        this->m_copyPtr->BeginFrame();
    }
    this->m_copyPtr->Reset();
    this->m_computePtr->BeginFrame();
    if (frame->m_callbacks.m_pages.size() != 0) {
        this->m_callbacks.SwapPages(&frame->m_callbacks);
        this->m_callbacks.Run();
    }
    frame->ReleaseObjects();
    this->Field<uint32_t>(0x138)++;
}

// Closes the lists, resolves each list's starting states against the device's, and submits
// everything with its residency sets.
// ref: FUN_1422d1690
void CGxDeviceD3d12::FrameSubmit() {
    GraphicsCommandList* graphics = this->m_graphicsPtr;
    // FUN_14230a320: the compute queue waits for the copies.
    if (this->m_computeWaitValue < this->m_copyQueue.m_fenceValue) {
        this->m_computeWaitValue = this->m_copyQueue.m_fenceValue;
        if (this->m_directQueue.m_queue != nullptr && this->m_copyQueue.m_fence != nullptr) {
            HRESULT hr = this->m_directQueue.m_queue->Wait(this->m_copyQueue.m_fence, this->m_copyQueue.m_fenceValue);
            if (FAILED(hr)) {
                this->Fail(hr, "GpuWaitOnOtherFenceHead failed");
            }
        }
    }
    // FUN_1422d4bb0: the graphics list's current list goes on the submit list.
    this->m_graphicsSubmits.push_back(EnqueuePair { &graphics->m_copy, graphics->m_copy.m_current, 0 });
    std::memset(&graphics->m_context.Field<uint8_t>(0x88), 0, 0x68);
    graphics->FlushBarriers();

    // Compute lists with nothing in them are closed and dropped.
    auto& compute = this->m_computeSubmits;
    uint64_t kept = 0;
    for (uint64_t i = 0; i < compute.size(); i++) {
        CopyCommandList* list = compute[i];
        if (list->m_empty != 0) {
            if (list->m_af8.size() != 0) {
                GxLog("Empty compute commandlist with Signals! This is probably a bug");
                compute[kept++] = list;
            } else {
                list->CloseList(0);
            }
        } else {
            compute[kept++] = list;
        }
    }
    compute.m_size = kept;
    auto& submits = this->m_graphicsSubmits;
    kept = 0;
    for (uint64_t i = 0; i < submits.size(); i++) {
        EnqueuePair pair = submits[i];
        if (pair.m_list->m_empty != 0) {
            pair.m_list->CloseList(pair.m_index);
        } else {
            submits[kept++] = pair;
        }
    }
    submits.m_size = kept;

    // The upload list's states first, then each compute list's, then the graphics lists' in
    // order, each resolved against the one before it.
    CopyCommandList* upload = this->m_copyPtr;
    this->TransitionList(upload, 0, nullptr, 0, 0);
    CopyCommandList* previous = upload;
    for (CopyCommandList* list : compute) {
        this->TransitionList(list, 0, previous, 0, 0xb4);
        previous = list;
    }
    CopyCommandList* barrierList = upload;
    uint32_t barrierIndex = 0;
    for (EnqueuePair& pair : submits) {
        this->TransitionList(pair.m_list, pair.m_index, barrierList, barrierIndex, 0);
        barrierList->FlushBarriers();
        barrierList->CloseList(barrierIndex);
        barrierIndex = pair.m_index;
        barrierList = pair.m_list;
    }
    barrierList->FlushBarriers();
    barrierList->CloseList(barrierIndex);

    // The upload list runs first on the direct queue.
    ID3D12CommandList* uploadList = upload->m_lists[0];
    HRESULT hr = this->m_residency.ExecuteCommandLists(this->m_directQueue.m_queue, &uploadList,
        upload->m_residencySets.data(), 1);
    if (FAILED(hr)) {
        this->Fail(hr, "ExecuteSingle failed");
    }
    if (this->m_directQueue.m_queue != nullptr && this->m_directQueue.m_fence != nullptr) {
        this->m_directQueue.m_fenceValue++;
        hr = this->m_directQueue.m_queue->Signal(this->m_directQueue.m_fence, this->m_directQueue.m_fenceValue);
        if (FAILED(hr)) {
            this->m_directQueue.m_fenceValue--;
            this->Fail(hr, "AddFenceToQueue failed");
        }
    }
    this->m_directQueue.m_frameFences[this->m_frameIndex] = this->m_directQueue.m_fenceValue;

    // Compute lists on the compute queue, after the direct queue's work.
    if (compute.size() != 0) {
        if (this->m_computeQueue.m_queue != nullptr && this->m_directQueue.m_fence != nullptr) {
            hr = this->m_computeQueue.m_queue->Wait(this->m_directQueue.m_fence, this->m_directQueue.m_fenceValue);
            if (FAILED(hr)) {
                this->Fail(hr, "GpuWaitOnOtherFenceHead failed");
            }
        }
        for (CopyCommandList* list : compute) {
            list->FlushBarriers();
            if (list->m_lists[0] != nullptr) {
                hr = list->m_lists[0]->Close();
                if (FAILED(hr)) {
                    this->Fail(hr, "RealCloseSingle failed");
                }
            }
            list->m_residencySets[0]->Close();
            ID3D12CommandList* single = list->m_lists[0];
            hr = this->m_residency.ExecuteCommandLists(this->m_computeQueue.m_queue, &single, list->m_residencySets.data(), 1);
            if (FAILED(hr)) {
                this->Fail(hr, "ExecuteSingle failed");
            }
            // FROST: the list's queued signals (+0xb00) go to the compute queue; nothing queues
            // them until the compute context is ported.
        }
        if (this->m_computeQueue.m_queue != nullptr && this->m_computeQueue.m_fence != nullptr) {
            this->m_computeQueue.m_fenceValue++;
            hr = this->m_computeQueue.m_queue->Signal(this->m_computeQueue.m_fence, this->m_computeQueue.m_fenceValue);
            if (FAILED(hr)) {
                this->m_computeQueue.m_fenceValue--;
                this->Fail(hr, "AddFenceToQueue failed");
            }
        }
        this->m_computeQueue.m_frameFences[this->m_frameIndex] = this->m_computeQueue.m_fenceValue;
        compute.m_size = 0;
    }

    // The graphics lists, batched between the points where one has signals to send.
    if (submits.size() != 0) {
        for (EnqueuePair& pair : submits) {
            this->m_submitLists.push_back(pair.m_list->m_lists[pair.m_index]);
            this->m_submitSets.push_back(pair.m_list->m_residencySets[pair.m_index]);
        }
        hr = this->m_residency.ExecuteCommandLists(this->m_directQueue.m_queue, this->m_submitLists.data(),
            this->m_submitSets.data(), static_cast<uint32_t>(this->m_submitLists.size()));
        if (FAILED(hr)) {
            this->Fail(hr, "ExecuteCommandLists(2)");
        }
        this->m_submitLists.m_size = 0;
        this->m_submitSets.m_size = 0;
        submits.m_size = 0;
        this->PresentAll();
    }
    this->FlushCopies();
    if (this->m_directQueue.m_queue != nullptr && this->m_directQueue.m_fence != nullptr) {
        this->m_directQueue.m_fenceValue++;
        hr = this->m_directQueue.m_queue->Signal(this->m_directQueue.m_fence, this->m_directQueue.m_fenceValue);
        if (FAILED(hr)) {
            this->m_directQueue.m_fenceValue--;
            this->Fail(hr, "AddFenceToQueue failed");
        }
    }
    this->m_directQueue.m_frameFences[this->m_frameIndex] = this->m_directQueue.m_fenceValue;
    if (this->m_copyQueue.m_queue != nullptr && this->m_directQueue.m_fence != nullptr) {
        hr = this->m_copyQueue.m_queue->Wait(this->m_directQueue.m_fence, this->m_directQueue.m_fenceValue);
        if (FAILED(hr)) {
            this->Fail(hr, "GpuWaitOnOtherFenceHead failed");
        }
    }
    this->m_submitSets.m_size = 0;
    this->m_submitLists.m_size = 0;
}

// The states each object of a list must start in, compared with the states the device last
// left it in: the differences become barriers at the end of the list before (or the upload
// list), and the device's states move on to where this list leaves them.
// ref: FUN_1422d24d0
void CGxDeviceD3d12::TransitionList(CopyCommandList* list, uint32_t index, CopyCommandList* barrierList, uint32_t barrierIndex,
    uint32_t mask) {
    StateTrackerMap& map = list->m_states[index];
    CopyCommandList* upload = this->m_copyPtr;
    map.for_each([&](uint64_t key, StateTracker& tracker) {
        auto object = reinterpret_cast<D3d12Object*>(key);
        uint32_t* current = this->m_states.find(key);
        uint32_t first = tracker.m_first;
        if (first != 0xffffffff && current != nullptr && *current != first) {
            uint32_t before = *current;
            if ((mask & first) != 0) {
                first &= ~mask;
            }
            CopyCommandList* target = (mask & before) == 0 ? barrierList : upload;
            if (target != nullptr) {
                D3D12_RESOURCE_BARRIER& barrier = target->m_barriers.emplace_back();
                std::memset(&barrier, 0, sizeof(barrier));
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = object->m_resource;
                barrier.Transition.Subresource = object->m_subresource;
                barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
                barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(first);
                // The barrier list needs the object resident too.
                if (object->m_residency != nullptr) {
                    target->m_residencySets[target == barrierList ? barrierIndex : 0]->Insert(object->m_residency);
                }
            }
        }
        if (current != nullptr) {
            *current = tracker.m_current;
        }
    });
    map.clear();
    if (barrierList != nullptr && barrierList->m_barriers.size() != 0) {
        barrierList->m_empty = 0;
        barrierList->m_lists[barrierIndex]->ResourceBarrier(static_cast<UINT>(barrierList->m_barriers.size()),
            barrierList->m_barriers.data());
        barrierList->m_barriers.m_size = 0;
    }
}

// The copies and uploads recorded on the upload list since the last frame, behind barriers
// that put the upload buffers' targets back for drawing.
// ref: FUN_1422cf0e0
bool CGxDeviceD3d12::FlushCopies() {
    if (this->m_ready == 0 || this->m_copiesPending == 0) {
        return false;
    }
    this->m_copiesPending = 0;
    CopyCommandList* upload = this->m_copyPtr;
    upload->Reset();
    struct {
        blz::vector<BufferPage*, s_bufferPageTag>* m_pages;
        uint32_t m_state;
    } lists[3] = {
        { &this->m_vertexPages, 0xc1 },
        { &this->m_indexPages, 0xc2 },
        { &this->m_otherPages, 0xc0 },
    };
    // The pages written to go to COPY_DEST...
    for (auto& entry : lists) {
        for (BufferPage* page : *entry.m_pages) {
            if (page->m_copyPending == 0) {
                continue;
            }
            D3D12_RESOURCE_BARRIER& barrier = upload->m_barriers.emplace_back();
            barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = page->m_resource;
            barrier.Transition.Subresource = 0;
            barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(entry.m_state);
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            if (page->m_residency.m_underlying != nullptr) {
                upload->ResidencySet()->Insert(&page->m_residency);
            }
        }
    }
    upload->FlushBarriers();
    // ...each buffer written this frame is copied from its upload memory into its range, and
    // reads from there now...
    for (CopyCommandList* list : this->m_activeLists) {
        for (BufPair& pair : list->m_bufs) {
            auto buf = static_cast<GxBuf*>(pair.m_buf);
            ID3D12Resource* dest = static_cast<BufferPage*>(buf->m_heap)->m_resource;
            uint32_t element = (buf->m_flags >> GXBUF_ELEMENT_SHIFT) & 0xfff;
            uint32_t offset = buf->m_heapOffset + element - 1;
            offset -= offset % element;
            upload->m_list->CopyBufferRegion(dest, offset, static_cast<ID3D12Resource*>(buf->m_resource), buf->m_offset,
                pair.m_size);
            buf->m_resource = dest;
            buf->m_offset = offset;
            buf->m_gpu = dest->GetGPUVirtualAddress() + offset;
        }
        list->m_bufs.m_size = 0;
    }
    // ...and the pages go back to being read.
    for (auto& entry : lists) {
        for (BufferPage* page : *entry.m_pages) {
            if (page->m_copyPending == 0) {
                continue;
            }
            page->m_copyPending = 0;
            D3D12_RESOURCE_BARRIER& barrier = upload->m_barriers.emplace_back();
            barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = page->m_resource;
            barrier.Transition.Subresource = 0;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(entry.m_state);
            if (page->m_residency.m_underlying != nullptr) {
                upload->ResidencySet()->Insert(&page->m_residency);
            }
        }
    }
    upload->FlushBarriers();
    if (upload->m_lists[0] != nullptr) {
        HRESULT hr = upload->m_lists[0]->Close();
        if (FAILED(hr)) {
            this->Fail(hr, "RealCloseSingle failed");
        }
    }
    upload->m_residencySets[0]->Close();
    ID3D12CommandList* single = upload->m_lists[0];
    HRESULT hr = this->m_residency.ExecuteCommandLists(this->m_directQueue.m_queue, &single, upload->m_residencySets.data(), 1);
    if (FAILED(hr)) {
        this->Fail(hr, "ExecuteSingle failed");
    }
    // The states the upload list left behind become the device's.
    upload->m_states[0].for_each([this](uint64_t key, StateTracker& tracker) {
        *this->m_states.insert(key) = tracker.m_current;
    });
    return true;
}

// Waits for the GPU and starts over (FUN_1422d2a30), when memory runs low.
void CGxDeviceD3d12::Flush() {
    if (this->m_ready == 0) {
        return;
    }
    g_gxDeviceLock.Enter();
    GxDeviceCallLock lock;
    this->FrameSubmit();
    this->WaitForGpu();
    this->m_graphicsPtr->BeginFrame();
    this->m_graphicsPtr->m_copy.Reset();
    if (this->m_graphicsPtr->m_copy.m_list != nullptr) {
        this->m_graphicsPtr->m_copy.m_list->SetGraphicsRootSignature(this->m_rootSignatures[0]);
        this->m_graphicsPtr->m_copy.m_list->SetComputeRootSignature(this->m_rootSignatures[1]);
    }
    if (this->m_ready != 0) {
        this->m_copyPtr->BeginFrame();
    }
    this->m_copyPtr->Reset();
    this->m_computePtr->BeginFrame();
    this->ReleaseFrames();
    this->SortPages();
    g_gxDeviceLock.Leave();
}

// The samplers last frames released (FUN_1422c9950 and FUN_1422e7410).
void CGxDeviceD3d12::ReleaseOldSamplers() {
    // Nothing releases samplers until GxSampler is ported; the queue at +0xe30 stays empty.
}

// The upload buffer pages (+0xb08..+0xb68) sorted and trimmed each frame (FUN_1422c7e80 et al).
void CGxDeviceD3d12::ReleaseUploadBuffers() {
    // Empty until GxBuf is ported.
}

// Whether a format multisamples at 2^samples with no quality level (the multisample quality
// levels feature query). An AMD part (+0xaa 2) is held to four samples.
// ref: FUN_1422d1060
bool CGxDeviceD3d12::FormatSupportsMultisample(uint8_t format, uint8_t samples, int32_t quality) {
    if (this->m_ready == 0) {
        return false;
    }
    uint32_t count = 1u << (samples & 0x1f);
    if (count > 0x20 || (1u << (quality & 0x1f)) > 0x20 || quality != 0 || this->m_device == nullptr ||
        (Field<uint8_t>(0xaa) == 2 && count > 4)) {
        return false;
    }
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
    levels.Format = gxDx12Backend::ResourceFormat(format);
    levels.SampleCount = count;
    if (FAILED(this->m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels, sizeof(levels)))) {
        return false;
    }
    return levels.NumQualityLevels != 0;
}

// The reference hands the mode to NVIDIA Reflex (NvAPI_D3D_SetSleepMode through the latency
// block at +0x46c8). DIVERGED: NVAPI and Reflex are stubs here, so there is nothing to set.
// ref: FUN_1422df730
void CGxDeviceD3d12::SetLowLatencyMode(uint8_t mode) {
    (void)mode;
}

// Variable-rate shading: the tier (+0x869) picks a row of supported rates (DAT_14291b220).
// ref: FUN_1422db790
bool CGxDeviceD3d12::ShadingRateSupported(uint32_t rate, int32_t a3, uint8_t a4) {
    static const uint32_t s_rates[2][3] = { { 0x33, 0x33, 0x33 }, { 0x673, 0x73, 0x33 } };
    if (reinterpret_cast<const uint8_t*>(this->m_data)[0x219] != 0 && rate < 3 && a3 == 0) {
        return ((s_rates[this->m_vrs][rate] >> (a4 & 0x1f)) & 1) != 0;
    }
    return a4 == 0;
}
