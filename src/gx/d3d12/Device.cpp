#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxVertexFormat.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/GxShader.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <initializer_list>
#include <new>

using namespace gxDx12Backend;

namespace {

const char s_deviceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\D3d12\\GxDeviceD3d12.cpp";

// The descriptor counts of the CPU-side RTV and DSV heaps (DAT_142d43968, DAT_142d43970);
// doubled and the device recreated when a heap runs out.
uint32_t s_rtvLimit = 0x200;
uint32_t s_dsvLimit = 0x64;

// The query counts per query heap (occlusion, timestamps) (DAT_142919cd0).
const uint32_t s_queryCounts[2] = { 0x10, 0x10 };

uint8_t s_samplerHeapFull;  // DAT_1434910d0

void GxDeviceLog(const char* format, ...);

// FUN_142292410: a variadic copy of GxLog the device code calls.
void GxDeviceLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    GxLogV(format, args);
    va_end(args);
}

void ReleaseCom(IUnknown* object) {
    if (object != nullptr) {
        object->Release();
    }
}

template <class T>
void ReleaseAndNull(T** object) {
    if (*object != nullptr) {
        (*object)->Release();
        *object = nullptr;
    }
}

}

GxUploadInterface* g_gxUploadInterface;  // DAT_14348b648

// Builds the D3D12 device in Gx's state block.
// ref: FUN_1422a8d70
CGxDevice* GxDeviceD3d12Create(void* block) {
    const OSVERSION* version = OsGetVersion();
    if (version->m_major == 6 && version->m_minor == 1 && version->m_wine == 0) {
        // Windows 7 runs D3D12 through D3D12On7: a 0x4760-byte subclass (vtable 0x142915ce0).
        FROST_UNIMPLEMENTED();
        return nullptr;
    }
    std::memset(block, 0, sizeof(CGxDeviceD3d12));
    return new (block) CGxDeviceD3d12();
}

CGxDeviceD3d12* GxDeviceD3d12() {
    return static_cast<CGxDeviceD3d12*>(static_cast<CGxDevice*>(g_gxDevice));
}

// ---- small objects -----------------------------------------------------------------------

// ref: FUN_1422cab90
DescriptorHeap::DescriptorHeap() {
    this->m_heap = nullptr;
    this->m_type = 0;
    this->m_cpuStart = 0;
    this->m_gpuStart = 0;
    this->m_increment = 0;
    this->m_next = 0;
    this->m_end = 0;
    InitializeSRWLock(&this->m_lock);
}

DescriptorHeap::~DescriptorHeap() {
    ReleaseAndNull(&this->m_heap);
}

// Takes a descriptor from the free list, else from the end; a full heap fails the device so
// it is recreated larger (the shader-visible view heap only flags it).
// ref: FUN_1422d4500
bool DescriptorHeap::Allocate(D3D12_CPU_DESCRIPTOR_HANDLE* handle) {
    AcquireSRWLockExclusive(&this->m_lock);
    if (this->m_type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV &&
        (this->m_end - this->m_next) / this->m_increment + static_cast<uint32_t>(this->m_free.size()) <
            static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(s_rtvLimit) * 0.2f))) {
        GxDeviceD3d12()->m_rtvHeapLow = 1;
    }
    bool allocated = true;
    uint32_t offset;
    if (this->m_free.size() == 0) {
        offset = this->m_next;
        if (offset == this->m_end) {
            if (this->m_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
                s_samplerHeapFull = 1;
                handle->ptr = 0;
                ReleaseSRWLockExclusive(&this->m_lock);
                return false;
            }
            g_gxDeviceLock.Enter();
            GxDeviceLog("Failed to allocate a descriptor: %s", this->m_name.m_data);
            CGxDeviceD3d12* device = GxDeviceD3d12();
            if (device->m_ready != 0) {
                device->m_ready = 0;
                if (this->m_type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) {
                    s_rtvLimit *= 2;
                    GxDeviceLog("Retrying with RTV limit of %u");
                } else if (this->m_type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) {
                    s_dsvLimit *= 2;
                    GxDeviceLog("Retrying with DSV limit of %u");
                }
            }
            g_gxDeviceLock.Leave();
            handle->ptr = 0;
            ReleaseSRWLockExclusive(&this->m_lock);
            return false;
        }
        this->m_next = this->m_increment + offset;
    } else {
        offset = this->m_free.back();
        this->m_free.pop_back();
    }
    handle->ptr = static_cast<SIZE_T>(offset) + this->m_cpuStart;
    ReleaseSRWLockExclusive(&this->m_lock);
    return allocated;
}

// The free in FUN_1422da380: the offset goes on the free list.
void DescriptorHeap::Free(uint64_t handle) {
    AcquireSRWLockExclusive(&this->m_lock);
    *this->m_free.push_back_slot() = static_cast<uint32_t>(handle - this->m_cpuStart);
    ReleaseSRWLockExclusive(&this->m_lock);
}

// ref: FUN_1422d0020
bool Queue::Create() {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    uint64_t frames = device->m_frames.size();
    uint64_t old = this->m_frameFences.size();
    this->m_frameFences.resize(frames);
    (void)old;
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(this->m_type);
    ReleaseAndNull(&this->m_queue);
    HRESULT hr = device->m_device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&this->m_queue));
    const char* failure;
    if (FAILED(hr)) {
        failure = "D3D12 Create CreateCommandQueue failed";
    } else {
        ReleaseAndNull(&this->m_fence);
        hr = device->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&this->m_fence));
        if (FAILED(hr)) {
            failure = "CreateFence failed";
        } else {
            this->m_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (this->m_event != nullptr) {
                return true;
            }
            failure = "Create Fence Event failed";
            hr = E_FAIL;
        }
    }
    device->Fail(hr, failure);
    return false;
}

// Signals the queue and waits for the GPU to reach it.
// ref: FUN_1422e6450
void Queue::Flush() {
    if (this->m_queue != nullptr && this->m_fence != nullptr) {
        this->m_fenceValue++;
        HRESULT hr = this->m_queue->Signal(this->m_fence, this->m_fenceValue);
        if (FAILED(hr)) {
            GxDeviceD3d12()->Fail(hr, "WaitUntilCompleted Failed");
            this->m_fenceValue--;
        }
    }
    this->WaitForValue(this->m_fenceValue);
}

// ref: FUN_1422e64b0
void Queue::WaitForValue(uint64_t value) {
    HRESULT hr = E_FAIL;
    if (this->m_fence == nullptr || this->m_event == nullptr ||
        FAILED(hr = this->m_fence->SetEventOnCompletion(value, this->m_event))) {
        GxDeviceD3d12()->Fail(hr, "WaitUntilCompleted failed");
        return;
    }
    // Three seconds, unless the device asked to wait forever. The reference also waits
    // forever under a debugger, but its check (FUN_1402ca290) returns false in this build.
    DWORD timeout = 3000;
    if (GxDeviceD3d12()->Field<uint8_t>(0xaa) == 1) {
        timeout = INFINITE;
    }
    DWORD result = WaitForSingleObjectEx(this->m_event, timeout, FALSE);
    if (result == WAIT_OBJECT_0) {
        return;
    }
    if (result == WAIT_TIMEOUT) {
        GxDeviceD3d12()->Fail(static_cast<HRESULT>(0x80070102), "WaitForSingleObjectEx Timeout");
        return;
    }
    if (result == WAIT_FAILED) {
        DWORD error = GetLastError();
        HRESULT code = static_cast<HRESULT>(error);
        if (code > 0) {
            code = static_cast<HRESULT>((error & 0xffff) | 0x80070000);
        }
        GxDeviceD3d12()->Fail(code, "WaitForSingleObjectEx Failed");
        GxLog("GetLastError: 0x%08x", error);
        return;
    }
    GxLog("dwResult: 0x%08x", result);
    GxDeviceD3d12()->Fail(E_FAIL, "WaitForSingleObjectEx Other");
}

// ref: FUN_1422a8bb0
Queue::~Queue() {
    ReleaseAndNull(&this->m_queue);
    ReleaseAndNull(&this->m_fence);
    if (this->m_event != nullptr) {
        CloseHandle(this->m_event);
    }
}

FrameData::FrameData() {
    this->m_queryReadback[0] = nullptr;
    this->m_queryReadback[1] = nullptr;
    this->m_queryPending[0] = 0;
    this->m_queryPending[1] = 0;
}

FrameData::~FrameData() {
    for (IUnknown* object : this->m_releases) {
        ReleaseCom(object);
    }
    ReleaseAndNull(&this->m_queryReadback[1]);
    ReleaseAndNull(&this->m_queryReadback[0]);
}

// Releases what the frame held once the GPU is done with it.
// ref: FUN_1422a5810
void FrameData::ReleaseObjects() {
    for (IUnknown*& object : this->m_releases) {
        if (object != nullptr) {
            IUnknown* released = object;
            object = nullptr;
            released->Release();
        }
    }
    this->m_releases.m_size = 0;
}

// ---- the device --------------------------------------------------------------------------

namespace {

CGxDeviceD3d12* InterfaceOwner(CGxDeviceD3d12::SyncInterface* upload) {
    return reinterpret_cast<CGxDeviceD3d12*>(reinterpret_cast<uint8_t*>(upload) - offsetof(CGxDeviceD3d12, m_syncInterface));
}

}

// ref: FUN_1422cf970
void CGxDeviceD3d12::SyncInterface::BufUpdate(GxBuf* buf, uint32_t size, GXBUFUPDATEPROC callback, void* param) {
    InterfaceOwner(this)->BufUpload(buf, size, callback, param);
}

// ref: FUN_1422cf980
void CGxDeviceD3d12::SyncInterface::BufWrite(GxBuf* buf, const void* data, uint32_t size) {
    InterfaceOwner(this)->BufWrite(buf, data, size);
}

// ref: FUN_1422cf990
void CGxDeviceD3d12::SyncInterface::TexUpdate(void* tex, const int32_t* rect, bool shaderResource,
    GXTEXUPDATEPROC callback, void* param) {
    InterfaceOwner(this)->TexUpload(static_cast<Tex*>(tex), rect, shaderResource, callback, param);
}

// ref: FUN_1422cacf0
CGxDeviceD3d12::CGxDeviceD3d12() : CGxDevice(GXAPI_D3D12) {
    this->m_rootSignatures[0] = nullptr;
    this->m_rootSignatures[1] = nullptr;
    this->m_drawSignature = nullptr;
    this->m_drawIndexedSignature = nullptr;
    this->m_dispatchSignature = nullptr;
    this->m_adapter1 = nullptr;
    this->m_adapter3 = nullptr;
    this->m_device = nullptr;
    this->m_queryHeaps[0] = nullptr;
    this->m_queryHeaps[1] = nullptr;
    std::memset(&this->m_localMemory, 0, sizeof(this->m_localMemory));
    std::memset(&this->m_nonLocalMemory, 0, sizeof(this->m_nonLocalMemory));
    this->m_548 = 0;
    this->m_directQueue.m_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    this->m_computeQueue.m_type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    this->m_copyQueue.m_type = D3D12_COMMAND_LIST_TYPE_COPY;
    this->m_residency.Construct();
    std::memset(&this->m_options, 0, sizeof(this->m_options));
    this->m_rootSignatureVersion = 0;
    this->m_tearing = 0;
    this->m_vrs = 0;
    this->m_largeHeaps = 0;
    this->m_currentChain = nullptr;
    this->m_primaryChain = nullptr;
    this->m_frame = nullptr;
    this->m_frameIndex = 0;
    this->m_queryFlags[0] = 0;
    this->m_queryFlags[1] = 0;
    this->m_copiesPending = 0;
    InitializeSRWLock(&this->m_submitLock);
    InitializeConditionVariable(&this->m_submitCondition);
    this->m_submitPending = 0;
    this->m_rtvHeap.m_name;
    this->m_samplers.init(0.75f);
    std::memset(this->m_samplerReleases, 0, sizeof(this->m_samplerReleases));
    InitializeSRWLock(&this->m_samplerLock);
    this->m_samplers.m_maxLoad = 0.5f;
    this->m_states.init(0.75f);
    this->m_graphicsPtr = nullptr;
    this->m_copyPtr = nullptr;
    this->m_computePtr = nullptr;
    std::memset(this->m_latency, 0, sizeof(this->m_latency));
    this->m_generateMips = nullptr;
    this->m_committedBytes = 0;
    this->m_placedBytes = 0;
    this->m_committedCount = 0;
    this->m_placedCount = 0;
    this->m_4738 = 0;
    this->m_computeWaitValue = 0;
    this->m_states.m_maxLoad = 0.5f;
}

// ref: FUN_1422a84a0 (the members go in reverse; the lists were destroyed with the device)
CGxDeviceD3d12::~CGxDeviceD3d12() {
}

// Clears m_ready and reports (FUN_142295750, which jumps to slot 0x2a8).
void CGxDeviceD3d12::Fail(HRESULT hr, const char* text) {
    this->m_ready = 0;
    this->ReportError(hr, text);
}

// _com_error::ErrorMessage (FUN_14229bcb0): the system's text for the code, its line break
// trimmed, or "IDispatch error #n" / "Unknown error 0x%0lX".
static const char* ComErrorMessage(HRESULT hr, char** allocated) {
    *allocated = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<char*>(allocated), 0, nullptr);
    char* text = *allocated;
    if (text != nullptr) {
        size_t length = std::strlen(text);
        if (length >= 2 && text[length - 1] == '\n') {
            text[length - 1] = '\0';
            if (text[length - 2] == '\r') {
                text[length - 2] = '\0';
            }
        }
        return text;
    }
    text = static_cast<char*>(LocalAlloc(LMEM_FIXED, 0x20));
    *allocated = text;
    if (text == nullptr) {
        return nullptr;
    }
    uint32_t code = static_cast<uint32_t>(hr);
    if (code + 0x7ffbfe00u < 0xfe00 && static_cast<uint16_t>(code - 0x200) != 0) {
        sprintf_s(text, 0x20, "IDispatch error #%d", static_cast<uint16_t>(code - 0x200));
    } else {
        sprintf_s(text, 0x20, "Unknown error 0x%0lX", code);
    }
    return text;
}

// ref: FUN_1422e65e0
void CGxDeviceD3d12::ReportError(HRESULT hr, const char* text) {
    char* allocated;
    GxLog("Error %s: %s (0x%08X).", text, ComErrorMessage(hr, &allocated), hr);
    if (allocated != nullptr) {
        LocalFree(allocated);
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED) {
        if (this->m_device != nullptr) {
            GxCountDeviceRemoved();
            HRESULT reason = this->m_device->GetDeviceRemovedReason();
            GxLog("Device Removed Reason: %s (0x%08X).", ComErrorMessage(reason, &allocated), reason);
            if (allocated != nullptr) {
                LocalFree(allocated);
            }
        }
    } else if (hr == E_OUTOFMEMORY) {
        blz::inline_string<0x1000> report;
        this->ReportMemory(report.header());
    }
}

// The memory report logged when the device runs out (FUN_1422dbce0).
void CGxDeviceD3d12::ReportMemory(blz::string_header* report) {
    (void)report;
    FROST_UNIMPLEMENTED();  // FUN_1422dbce0: the heap, texture and buffer totals
}

// Takes the adapter Prism chose: the software one, or the DXGI adapter with its LUID.
// ref: FUN_1422cdfb0
bool CGxDeviceD3d12::FindAdapter() {
    pm::GPU* gpu = g_gxPrismInstance->GetGpu(this->m_gpu);
    if (gpu->m_type == pm::GPU_SOFTWARE) {
        ReleaseAndNull(&this->m_adapter1);
        GxLog("Choosing software adapter");
        return true;
    }
    ReleaseAndNull(&this->m_adapter1);
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0;; index++) {
        HRESULT hr;
        if (GxPlatformFactory6() == nullptr) {
            hr = GxPlatformFactory1()->EnumAdapters1(index, &adapter);
        } else {
            hr = GxPlatformFactory6()->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
        }
        if (hr == DXGI_ERROR_NOT_FOUND) {
            GxLog("Failed to find prism Gpu by LUID");
            if (GxPlatformFactory6() == nullptr) {
                GxPlatformFactory1()->EnumAdapters1(0, &adapter);
            } else {
                GxPlatformFactory6()->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    __uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
            }
            break;
        }
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        uint64_t luid = (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
        if (this->Field<uint64_t>(0xc0) == luid) {
            break;
        }
        ReleaseAndNull(&adapter);
    }
    if (this->m_adapter1 != adapter) {
        if (adapter != nullptr) {
            adapter->AddRef();
        }
        IDXGIAdapter1* old = this->m_adapter1;
        this->m_adapter1 = adapter;
        ReleaseCom(old);
    }
    ReleaseCom(adapter);
    return true;
}

// ref: FUN_1422eb8a0
bool CGxDeviceD3d12::CreateD3dDevice(uint32_t* featureLevel) {
    if (GxPlatformFactory5() == nullptr) {
        GxDeviceLog("No Factory5");
        return false;
    }
    BOOL tearing = FALSE;
    if (SUCCEEDED(GxPlatformFactory5()->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        this->m_tearing = tearing != FALSE;
    }
    GxDeviceLog("DXGI_FEATURE_PRESENT_ALLOW_TEARING:%s", this->m_tearing != 0 ? "true" : "false");
    if (this->m_adapter1 == nullptr) {
        if (!GxGetAllowSoftwareGpu()) {
            GxDeviceLog("No supported GPU found");
            return false;
        }
        IDXGIFactory5* factory = GxPlatformFactory5();
        ReleaseAndNull(&this->m_adapter1);
        if (SUCCEEDED(factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&this->m_adapter1)))) {
            GxDeviceLog("Using Warp Adapter");
        }
    }
    ReleaseAndNull(&this->m_adapter3);
    HRESULT hr = this->m_adapter1->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&this->m_adapter3));
    const char* failure;
    if (FAILED(hr) && this->m_adapter3 == nullptr) {
        failure = "Adapter QueryInterface adapter3 failed";
    } else {
        ReleaseAndNull(&this->m_device);
        hr = GxPlatformD3d12CreateDevice()(this->m_adapter3, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
            reinterpret_cast<void**>(&this->m_device));
        if (SUCCEEDED(hr)) {
            *featureLevel = D3D_FEATURE_LEVEL_11_0;
            const D3D_FEATURE_LEVEL levels[3] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_0 };
            D3D12_FEATURE_DATA_FEATURE_LEVELS query = {};
            query.NumFeatureLevels = 3;
            query.pFeatureLevelsRequested = levels;
            if (SUCCEEDED(this->m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &query, sizeof(query)))) {
                *featureLevel = query.MaxSupportedFeatureLevel;
            }
            return this->m_directQueue.Create() && this->m_computeQueue.Create() && this->m_copyQueue.Create();
        }
        failure = "D3D12 Create Device failed";
    }
    this->ReportError(hr, failure);
    return false;
}

// ref: FUN_1422ebb30
void CGxDeviceD3d12::ReleaseAdapter() {
    ReleaseAndNull(&this->m_adapter3);
}

// ref: FUN_1422ec2e0
void CGxDeviceD3d12::QueryVideoMemory() {
    if (this->m_ready != 0) {
        HRESULT hr = this->m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &this->m_localMemory);
        if (FAILED(hr)) {
            this->ReportError(hr, "QueryVideoMemoryInfo (local) failed");
        }
    }
    if (this->m_ready != 0) {
        HRESULT hr = this->m_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &this->m_nonLocalMemory);
        if (FAILED(hr)) {
            this->ReportError(hr, "QueryVideoMemoryInfo (non local) failed");
        }
    }
}

namespace {

// The engine's root signatures in D3D12 1.1 form: the graphics one by stage, the compute one
// for every stage at once.
HRESULT SerializeRootSignatures11(ID3DBlob** graphics, ID3DBlob** compute, ID3DBlob** errors, bool computeOnly) {
    D3D12_DESCRIPTOR_RANGE1 srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0x40, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 samplerRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0x10, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 uavRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 vertexSrvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0x40, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 vertexSamplerRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0x10, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 bindlessCbv = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, UINT_MAX, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 bindlessSrv1 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 bindlessSrv2 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 bindlessSrv3 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 3, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE1 bindlessSampler = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

    auto table = [](const D3D12_DESCRIPTOR_RANGE1* range, D3D12_SHADER_VISIBILITY visibility) {
        D3D12_ROOT_PARAMETER1 parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = range;
        parameter.ShaderVisibility = visibility;
        return parameter;
    };
    auto cbv = [](uint32_t reg, D3D12_SHADER_VISIBILITY visibility) {
        D3D12_ROOT_PARAMETER1 parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = reg;
        parameter.Descriptor.RegisterSpace = 0;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
        parameter.ShaderVisibility = visibility;
        return parameter;
    };

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    HRESULT hr = S_OK;
    if (!computeOnly) {
        D3D12_ROOT_PARAMETER1 parameters[0x1a];
        parameters[0] = table(&srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[1] = table(&samplerRange, D3D12_SHADER_VISIBILITY_PIXEL);
        for (uint32_t i = 0; i < 10; i++) {
            parameters[2 + i] = cbv(i, D3D12_SHADER_VISIBILITY_PIXEL);
        }
        for (uint32_t i = 0; i < 6; i++) {
            parameters[12 + i] = cbv(i, D3D12_SHADER_VISIBILITY_VERTEX);
        }
        parameters[18] = table(&uavRange, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[19] = table(&vertexSrvRange, D3D12_SHADER_VISIBILITY_VERTEX);
        parameters[20] = table(&vertexSamplerRange, D3D12_SHADER_VISIBILITY_VERTEX);
        parameters[21] = table(&bindlessCbv, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[22] = table(&bindlessSrv1, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[23] = table(&bindlessSrv2, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[24] = table(&bindlessSrv3, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[25] = table(&bindlessSampler, D3D12_SHADER_VISIBILITY_PIXEL);
        desc.Desc_1_1.NumParameters = 0x1a;
        desc.Desc_1_1.pParameters = parameters;
        desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        hr = GxPlatformSerializeVersionedRootSignature()(&desc, graphics, errors);
        if (FAILED(hr)) {
            return hr;
        }
        return hr;
    }
    D3D12_ROOT_PARAMETER1 parameters[0xb];
    for (uint32_t i = 0; i < 3; i++) {
        parameters[i] = cbv(i, D3D12_SHADER_VISIBILITY_ALL);
    }
    parameters[3] = table(&srvRange, D3D12_SHADER_VISIBILITY_ALL);
    parameters[4] = table(&samplerRange, D3D12_SHADER_VISIBILITY_ALL);
    parameters[5] = table(&uavRange, D3D12_SHADER_VISIBILITY_ALL);
    parameters[6] = table(&bindlessCbv, D3D12_SHADER_VISIBILITY_ALL);
    parameters[7] = table(&bindlessSrv1, D3D12_SHADER_VISIBILITY_ALL);
    parameters[8] = table(&bindlessSrv2, D3D12_SHADER_VISIBILITY_ALL);
    parameters[9] = table(&bindlessSrv3, D3D12_SHADER_VISIBILITY_ALL);
    parameters[10] = table(&bindlessSampler, D3D12_SHADER_VISIBILITY_ALL);
    desc.Desc_1_1.NumParameters = 0xb;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    return GxPlatformSerializeVersionedRootSignature()(&desc, compute, errors);
}

// The same in 1.0 form, without the bindless tables.
HRESULT SerializeRootSignatures10(ID3DBlob** out, ID3DBlob** errors, bool computeOnly) {
    D3D12_DESCRIPTOR_RANGE srvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0x40, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE samplerRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0x10, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE uavRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 8, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    // The vertex stage's resource table holds two views in the 1.0 form.
    D3D12_DESCRIPTOR_RANGE vertexSrvRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE vertexSamplerRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0x10, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

    auto table = [](const D3D12_DESCRIPTOR_RANGE* range, D3D12_SHADER_VISIBILITY visibility) {
        D3D12_ROOT_PARAMETER parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = range;
        parameter.ShaderVisibility = visibility;
        return parameter;
    };
    auto cbv = [](uint32_t reg, D3D12_SHADER_VISIBILITY visibility) {
        D3D12_ROOT_PARAMETER parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = reg;
        parameter.ShaderVisibility = visibility;
        return parameter;
    };

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!computeOnly) {
        D3D12_ROOT_PARAMETER parameters[0x15];
        parameters[0] = table(&srvRange, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[1] = table(&samplerRange, D3D12_SHADER_VISIBILITY_PIXEL);
        for (uint32_t i = 0; i < 10; i++) {
            parameters[2 + i] = cbv(i, D3D12_SHADER_VISIBILITY_PIXEL);
        }
        for (uint32_t i = 0; i < 6; i++) {
            parameters[12 + i] = cbv(i, D3D12_SHADER_VISIBILITY_VERTEX);
        }
        parameters[18] = table(&uavRange, D3D12_SHADER_VISIBILITY_PIXEL);
        parameters[19] = table(&vertexSrvRange, D3D12_SHADER_VISIBILITY_VERTEX);
        parameters[20] = table(&vertexSamplerRange, D3D12_SHADER_VISIBILITY_VERTEX);
        desc.NumParameters = 0x15;
        desc.pParameters = parameters;
        return GxPlatformSerializeRootSignature()(&desc, D3D_ROOT_SIGNATURE_VERSION_1, out, errors);
    }
    D3D12_ROOT_PARAMETER parameters[6];
    for (uint32_t i = 0; i < 3; i++) {
        parameters[i] = cbv(i, D3D12_SHADER_VISIBILITY_ALL);
    }
    parameters[3] = table(&srvRange, D3D12_SHADER_VISIBILITY_ALL);
    parameters[4] = table(&samplerRange, D3D12_SHADER_VISIBILITY_ALL);
    parameters[5] = table(&uavRange, D3D12_SHADER_VISIBILITY_ALL);
    desc.NumParameters = 6;
    desc.pParameters = parameters;
    return GxPlatformSerializeRootSignature()(&desc, D3D_ROOT_SIGNATURE_VERSION_1, out, errors);
}

// Makes one of the descriptor heaps (the four blocks inline in FUN_1422d5d50).
HRESULT CreateHeap(ID3D12Device* device, DescriptorHeap* heap, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, const char* name) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = count;
    HRESULT hr = device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap->m_heap));
    if (FAILED(hr)) {
        return hr;
    }
    heap->m_type = desc.Type;
    heap->m_cpuStart = heap->m_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    heap->m_gpuStart = heap->m_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    uint32_t increment = device->GetDescriptorHandleIncrementSize(desc.Type);
    heap->m_next = 0;
    heap->m_increment = increment;
    heap->m_end = increment * count;
    blz::string_assign(heap->m_name.header(), name, std::strlen(name));
    return hr;
}

}

// ref: FUN_1422d5d50
bool CGxDeviceD3d12::DeviceCreateCore(const GXDEVICEPARAMS* params) {
    if (GxPlatformFactory1() == nullptr) {
        GxDeviceLog("No Factory1");
        return false;
    }
    uint64_t frames;
    if (params->m_vsync == 0) {
        frames = params->m_maxFrameLatency < 2 ? 2 : (params->m_maxFrameLatency > 3 ? 3 : params->m_maxFrameLatency);
    } else {
        frames = 3;
    }
    this->m_frames.resize(frames);
    this->m_frame = &this->m_frames[this->m_frameIndex];
    if (!this->FindAdapter()) {
        return false;
    }
    uint32_t featureLevel;
    if (!this->CreateD3dDevice(&featureLevel)) {
        return false;
    }
    this->m_rootSignatureVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    if (FAILED(this->m_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &this->m_rootSignatureVersion, 4))) {
        this->m_rootSignatureVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }
    this->Field<uint8_t>(0x12a) = 1;
    this->Field<uint8_t>(0x14b) = 1;
    HRESULT hr = this->m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &this->m_options, sizeof(this->m_options));
    if (FAILED(hr)) {
        this->ReportError(hr, "CheckFeatureSupport failed");
        this->Field<uint8_t>(0x14c) = 0;
    } else if (this->m_options.TypedUAVLoadAdditionalFormats) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        hr = this->m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
        if (FAILED(hr) || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) == 0) {
            GxDeviceLog("DXGI_FORMAT_B8G8R8A8_UNORM UAV support: false");
            this->Field<uint8_t>(0x14c) = 0;
        } else {
            GxDeviceLog("DXGI_FORMAT_B8G8R8A8_UNORM UAV support: true");
            this->Field<uint8_t>(0x14c) = 1;
        }
    }

    const char* failure = "CreateDescriptorHeap failed";
    if (FAILED(hr = CreateHeap(this->m_device, &this->m_rtvHeap, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, s_rtvLimit, "RTV Heap")) ||
        FAILED(hr = CreateHeap(this->m_device, &this->m_dsvHeap, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, s_dsvLimit, "DSV Heap"))) {
        this->Fail(hr, failure);
        return false;
    }
    if (FAILED(hr = CreateHeap(this->m_device, &this->m_samplerHeap, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 0x800, "Sampler Heap"))) {
        this->Fail(hr, failure);
        return false;
    }
    s_samplerHeapFull = 0;
    if (FAILED(hr = CreateHeap(this->m_device, &this->m_viewHeap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 24000, "CBV/SRV/UAV Heap"))) {
        this->Fail(hr, failure);
        return false;
    }
    this->Field<uint16_t>(0x128) = 0x101;
    this->m_ready = 1;
    DXGI_ADAPTER_DESC1 adapterDesc;
    if (this->m_adapter1 != nullptr && SUCCEEDED(this->m_adapter1->GetDesc1(&adapterDesc))) {
        this->Field<uint32_t>(0x15c) = static_cast<uint32_t>(adapterDesc.DedicatedVideoMemory >> 20);
        this->Field<uint32_t>(0x160) = static_cast<uint32_t>(adapterDesc.SharedSystemMemory >> 20);
    }
    this->Field<uint8_t>(0x12b) = 0;
    this->Field<uint8_t>(0x12c) = 0;
    if (GxPlatformHasNvapi() && this->Field<uint8_t>(0x12e) == 0) {
        // FUN_1401a5d50 (NvAPI_D3D12_IsNvShaderExtnOpCodeSupported for the barycentrics ops).
        // DIVERGED: NVAPI is not vendored; the extension stays off.
    }
    if (this->Field<uint16_t>(0xa0) == 0x10de && this->Field<uint8_t>(0xab) == 3) {
        this->Field<uint8_t>(0x12e) = 0;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
    if (SUCCEEDED(this->m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6)))) {
        this->m_vrs = options6.AdditionalShadingRatesSupported != FALSE;
    }
    GXSHADERSETTINGS shaders = {};
    shaders.m_family = this->Field<int32_t>(0x88);
    shaders.m_cacheless = params->m_cachelessShaders;
    shaders.m_05 = 0;
    GxShaderSetFamily(&shaders);

    // The root signatures.
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    bool legacy = this->m_rootSignatureVersion == D3D_ROOT_SIGNATURE_VERSION_1_0 || this->m_options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_3;
    if (legacy) {
        hr = SerializeRootSignatures10(&blob, &errors, false);
        if (FAILED(hr)) {
            this->Fail(hr, "D3D12SerializeRootSignature failed");
        }
    } else {
        hr = SerializeRootSignatures11(&blob, nullptr, &errors, false);
        if (FAILED(hr)) {
            this->Fail(hr, "D3D12SerializeVersionedRootSignature failed");
            return false;
        }
    }
    hr = this->m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
        reinterpret_cast<void**>(&this->m_rootSignatures[0]));
    ReleaseAndNull(&blob);
    ReleaseAndNull(&errors);
    if (FAILED(hr)) {
        this->Fail(hr, "CreateRootSignature failed");
        return false;
    }
    if (legacy) {
        hr = SerializeRootSignatures10(&blob, &errors, true);
        if (FAILED(hr)) {
            this->Fail(hr, "D3D12SerializeRootSignature failed");
            return false;
        }
    } else {
        hr = SerializeRootSignatures11(nullptr, &blob, &errors, true);
        if (FAILED(hr)) {
            this->Fail(hr, "D3D12SerializeVersionedRootSignature failed");
            return false;
        }
    }
    hr = this->m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
        reinterpret_cast<void**>(&this->m_rootSignatures[1]));
    ReleaseAndNull(&blob);
    ReleaseAndNull(&errors);
    if (FAILED(hr)) {
        this->Fail(hr, "CreateRootSignature failed");
        return false;
    }

    // The indirect command signatures.
    D3D12_INDIRECT_ARGUMENT_DESC argument = {};
    D3D12_COMMAND_SIGNATURE_DESC signature = {};
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    signature.ByteStride = 0x10;
    signature.NodeMask = 1;
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    if (this->m_device != nullptr) {
        hr = this->m_device->CreateCommandSignature(&signature, nullptr, __uuidof(ID3D12CommandSignature),
            reinterpret_cast<void**>(&this->m_drawSignature));
        if (FAILED(hr)) {
            this->Fail(hr, "CreateCommandSignature D3D12_INDIRECT_ARGUMENT_TYPE_DRAW failed");
        }
    }
    signature.ByteStride = 0x14;
    signature.NodeMask = 1;
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    if (this->m_device != nullptr) {
        hr = this->m_device->CreateCommandSignature(&signature, nullptr, __uuidof(ID3D12CommandSignature),
            reinterpret_cast<void**>(&this->m_drawIndexedSignature));
        if (FAILED(hr)) {
            this->Fail(hr, "CreateCommandSignature D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED failed");
        }
    }
    signature.ByteStride = 0xc;
    signature.NodeMask = 0;
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    if (this->m_device != nullptr) {
        hr = this->m_device->CreateCommandSignature(&signature, nullptr, __uuidof(ID3D12CommandSignature),
            reinterpret_cast<void**>(&this->m_dispatchSignature));
        if (FAILED(hr)) {
            this->Fail(hr, "CreateCommandSignature D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH failed");
        }
    }
    this->m_residency.Initialize(this->m_device, 0, this->m_adapter3, 3);

    // The graphics list, then the upload list (direct) and the copy list.
    auto data = reinterpret_cast<const uint8_t*>(this->m_data);
    auto graphics = new (this->m_graphics) GraphicsCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT, data[0x219]);
    this->m_graphicsPtr = graphics;
    if (this->m_ready == 0) {
        return false;
    }
    graphics->BeginFrame();
    graphics->m_copy.Reset();
    if (graphics->m_copy.m_list != nullptr) {
        graphics->m_copy.m_list->SetGraphicsRootSignature(this->m_rootSignatures[0]);
        graphics->m_copy.m_list->SetComputeRootSignature(this->m_rootSignatures[1]);
    }
    g_gxRenderContext = &graphics->m_context;
    auto upload = new (this->m_copyList) CopyCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    this->m_copyPtr = upload;
    if (this->m_ready == 0) {
        return false;
    }
    upload->BeginFrame();
    upload->Reset();
    auto copy = new (this->m_computeList) CopyCommandList(D3D12_COMMAND_LIST_TYPE_COPY);
    this->m_computePtr = copy;
    if (this->m_ready == 0) {
        return false;
    }
    g_gxUploadInterface = &this->m_syncInterface;
    GxShaderServiceStart();

    // The mip generation pipeline, which the device cannot work without.
    int32_t mipsShader = GxShaderComputeId(0x12);
    auto pipeline = static_cast<GENERATEMIPSPIPELINE*>(
        SMemAlloc(sizeof(GENERATEMIPSPIPELINE), 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0x1146, 0));
    std::memset(pipeline, 0, sizeof(GENERATEMIPSPIPELINE));
    pipeline->m_shader = mipsShader;
    if (this->m_ready != 0) {
        this->CreateGenerateMips(pipeline);
    }
    this->m_generateMips = pipeline;
    if (pipeline->m_pipeline == nullptr) {
        GxLog("GenerateMipsPipeline create failed");
        return false;
    }
    this->m_directQueue.m_queue->GetTimestampFrequency(&this->Field<uint64_t>(0x130));

    // The query heaps and each frame's readback buffers for them.
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t count = s_queryCounts[i];
        ID3D12QueryHeap* heap = nullptr;
        if (i == 0 || i == 1) {
            D3D12_QUERY_HEAP_DESC desc = {};
            desc.Type = i == 0 ? D3D12_QUERY_HEAP_TYPE_OCCLUSION : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            desc.Count = i == 1 ? count * 2 : count;
            hr = this->m_device->CreateQueryHeap(&desc, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&heap));
            if (FAILED(hr)) {
                this->Fail(hr, "CreateQueryHeap Failed");
                ReleaseAndNull(&heap);
            }
        }
        ID3D12QueryHeap* old = this->m_queryHeaps[i];
        this->m_queryHeaps[i] = heap;
        ReleaseCom(old);
        uint32_t size = i == 1 ? count * 0x10 : count * 8;
        for (FrameData& frame : this->m_frames) {
            ID3D12Resource* buffer = this->CreateReadbackBuffer(size);
            ReleaseCom(frame.m_queryReadback[i]);
            frame.m_queryReadback[i] = buffer;
        }
    }
    // FUN_14234d120: the NVIDIA Reflex latency markers. DIVERGED: NVAPI is not vendored.
    this->ReregisterObjects();
    return true;
}

// ref: FUN_1422d78a0
ID3D12Resource* CGxDeviceD3d12::CreateReadbackBuffer(uint32_t size) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    HRESULT hr = this->m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (FAILED(hr)) {
        this->Fail(hr, "CreateReadbackBuffer CreateCommittedResource failed");
    }
    return resource;
}

// Builds the mip generation compute pipeline from its shader.
// ref: FUN_1422d4d60
bool CGxDeviceD3d12::CreateGenerateMips(GENERATEMIPSPIPELINE* pipeline) {
    GXSHADERBUFFER buffer;
    const GXSHADERPERMUTE* permute = GxShaderGetPermute(pipeline->m_shader, pipeline->m_permute, &buffer);
    if (permute == nullptr) {
        char name[0x100];
        GxDeviceLog("GetComputePermute failed: %s", GxShaderName(pipeline->m_shader, name, sizeof(name)));
        this->m_ready = 0;
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = this->m_rootSignatures[1];
    desc.CS.pShaderBytecode = reinterpret_cast<const uint8_t*>(&permute->m_bytecodeOffset) + permute->m_bytecodeOffset;
    desc.CS.BytecodeLength = permute->m_bytecodeSize;
    pipeline->m_20 = permute->m_20;
    pipeline->m_18 = permute->m_18;
    pipeline->m_38 = permute->m_38;
    pipeline->m_30 = permute->m_30;
    pipeline->m_08 = permute->m_08;
    ID3D12PipelineState* state = nullptr;
    HRESULT hr = this->m_device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&state));
    if (FAILED(hr)) {
        this->Fail(hr, "IComputePipelineCreate failed");
    }
    pipeline->m_pipeline = state;
    return state != nullptr;
}

// ref: FUN_1422d0f80
bool CGxDeviceD3d12::DeviceCreate(const GXDEVICEPARAMS* params) {
    GxLog("D3d12 Device Create");
    this->SetParams(params);
    if (GxPlatformHasD3d12() && this->DeviceCreateCore(params)) {
        GxNotifyOnDeviceCreate();
        GxLog("D3d12 Device Create Successful");
        return true;
    }
    GxLog("D3d12 Device Create Failed");
    GxLog("D3d12 Device Destroy");
    this->WaitForGpu();
    GxNotifyOnDeviceDestroy();
    this->DeviceDestroy();
    return false;
}

// ref: FUN_1422d1020
void CGxDeviceD3d12::DeviceShutdown() {
    GxLog("D3d12 Device Destroy");
    this->WaitForGpu();
    GxNotifyOnDeviceDestroy();
    this->DeviceDestroy();
}

// Drains the three queues.
// ref: FUN_1422e61d0
void CGxDeviceD3d12::WaitForGpu() {
    if (this->m_directQueue.m_queue == nullptr || this->m_directQueue.m_fence == nullptr) {
        return;
    }
    GxDeviceCallLock lock;
    if (this->m_copyQueue.m_queue != nullptr && this->m_copyQueue.m_fence != nullptr) {
        this->m_copyQueue.m_fenceValue++;
        HRESULT hr = this->m_copyQueue.m_queue->Signal(this->m_copyQueue.m_fence, this->m_copyQueue.m_fenceValue);
        if (FAILED(hr)) {
            this->Fail(hr, "WaitUntilCompleted Failed");
            this->m_copyQueue.m_fenceValue--;
        }
    }
    this->m_copyQueue.WaitForValue(this->m_copyQueue.m_fenceValue);
    this->m_computeQueue.Flush();
    this->m_directQueue.Flush();
}

// Tears the device's objects down in the reverse of DeviceCreateCore.
// ref: FUN_1422d7c80
void CGxDeviceD3d12::DeviceDestroy() {
    auto graphics = this->m_graphicsPtr;
    if (graphics != nullptr) {
        GxRenderTargetsClear(&graphics->m_context.Field<GXRENDERTARGETS>(0x88));
        graphics->FlushBarriers();
    }
    auto upload = this->m_copyPtr;
    if (upload != nullptr && upload->m_barriers.size() != 0) {
        upload->m_empty = 0;
        upload->m_list->ResourceBarrier(static_cast<UINT>(upload->m_barriers.size()), upload->m_barriers.data());
        upload->m_barriers.m_size = 0;
    }
    if (graphics != nullptr) {
        graphics->m_copy.CloseLists();
    }
    if (upload != nullptr) {
        upload->CloseLists();
    }
    ReleaseAndNull(&this->m_queryHeaps[0]);
    ReleaseAndNull(&this->m_queryHeaps[1]);
    this->ReleaseFrames();
    this->m_ready = 0;
    if (graphics != nullptr) {
        graphics->~GraphicsCommandList();
    }
    g_gxRenderContext = nullptr;
    if (this->m_copyPtr != nullptr) {
        this->m_copyPtr->~CopyCommandList();
    }
    if (this->m_computePtr != nullptr) {
        this->m_computePtr->~CopyCommandList();
    }
    g_gxUploadInterface = nullptr;
    this->ReleaseRegisteredObjects();
    GxShaderServiceStop();
    auto pipeline = static_cast<GENERATEMIPSPIPELINE*>(this->m_generateMips);
    if (pipeline != nullptr) {
        IUnknown* state = pipeline->m_pipeline;
        g_gxDeviceLock.Enter();
        this->m_frame->m_releases.push_back(state);
        g_gxDeviceLock.Leave();
        pipeline->m_pipeline = nullptr;
        operator delete(pipeline, sizeof(GENERATEMIPSPIPELINE));
        this->m_generateMips = nullptr;
    }
    this->ReleaseFrames();
    ReleaseAndNull(&this->m_rootSignatures[0]);
    ReleaseAndNull(&this->m_rootSignatures[1]);
    ReleaseAndNull(&this->m_drawSignature);
    ReleaseAndNull(&this->m_drawIndexedSignature);
    ReleaseAndNull(&this->m_dispatchSignature);
    this->ReleaseSamplers();
    for (HeapPage* page : this->m_heapPages) {
        if (page != nullptr) {
            page->~HeapPage();
            operator delete(page, 0xb8);
        }
    }
    this->m_heapPages.m_size = 0;
    for (auto pages : { &this->m_vertexPages, &this->m_indexPages, &this->m_otherPages }) {
        for (BufferPage* page : *pages) {
            if (page != nullptr) {
                page->~BufferPage();
                operator delete(page, sizeof(BufferPage));
            }
        }
        pages->m_size = 0;
    }
    for (auto& pages : this->m_freeUploadPages) {
        for (DescriptorUploadPage& page : pages) {
            ReleaseAndNull(&page.m_heap);
        }
        pages.m_size = 0;
    }
    ReleaseAndNull(&this->m_directQueue.m_queue);
    ReleaseAndNull(&this->m_directQueue.m_fence);
    CloseHandle(this->m_directQueue.m_event);
    this->m_directQueue.m_event = nullptr;
    ReleaseAndNull(&this->m_computeQueue.m_queue);
    ReleaseAndNull(&this->m_computeQueue.m_fence);
    CloseHandle(this->m_computeQueue.m_event);
    this->m_computeQueue.m_event = nullptr;
    ReleaseAndNull(&this->m_copyQueue.m_queue);
    ReleaseAndNull(&this->m_copyQueue.m_fence);
    CloseHandle(this->m_copyQueue.m_event);
    this->m_copyQueue.m_event = nullptr;
    ReleaseAndNull(&this->m_rtvHeap.m_heap);
    ReleaseAndNull(&this->m_dsvHeap.m_heap);
    ReleaseAndNull(&this->m_samplerHeap.m_heap);
    ReleaseAndNull(&this->m_viewHeap.m_heap);
    while (this->m_swapChains.size() != 0) {
        this->SwapChainDestroy(this->m_swapChains.back());
    }
    this->m_swapChains.m_size = 0;
    this->ReleaseFrames();
    this->m_frames.resize(0);
    this->m_frame = nullptr;
    this->m_residency.Destroy();
    ReleaseAndNull(&this->m_adapter1);
    this->ReleaseAdapter();
    ReleaseAndNull(&this->m_device);
}

// The samplers made for the device and the ones waiting to be released (FUN_1422e7410 and
// the loop over the sampler map in FUN_1422d7c80): their descriptors go back on the heap.
void CGxDeviceD3d12::ReleaseSamplers() {
    this->m_samplers.for_each([this](uint64_t, void*& sampler) {
        if (sampler != nullptr) {
            this->m_samplerHeap.Free(reinterpret_cast<uint64_t>(sampler));
        }
    });
    this->m_samplers.clear();
}

// Waits for every frame the GPU still has and releases what each held.
// ref: FUN_1422dca40
void CGxDeviceD3d12::ReleaseFrames() {
    uint32_t count = static_cast<uint32_t>(this->m_frames.size());
    uint8_t current = this->m_frameIndex;
    if (count != 0) {
        uint32_t index = (current + 1u) % count;
        uint32_t done = 0;
        do {
            while (true) {
                uint32_t previous = (count - 1 + index) % count;
                this->m_frameIndex = static_cast<uint8_t>(previous);
                this->m_frame = &this->m_frames[previous & 0xff];
                FrameData& frame = this->m_frames[index];
                if (frame.m_callbacks.m_pages.size() == 0 && frame.m_releases.size() == 0) {
                    break;
                }
                done = 0;
                if (frame.m_callbacks.m_pages.size() != 0) {
                    this->m_callbacks.SwapPages(&frame.m_callbacks);
                    this->m_callbacks.Run();
                }
                frame.ReleaseObjects();
                index = (index + 1) % count;
            }
            index = (index + 1) % count;
            done++;
        } while (done < count);
    }
    this->m_frameIndex = current;
    this->m_frame = &this->m_frames[current];
}

// ---- buffers and deferred work -------------------------------------------------------------

namespace {

// A deferred function's record: the function runs, then its storage is destroyed
// (FUN_1422967d0).
void RunDeferredFunction(void* record) {
    auto function = static_cast<blz::function_storage*>(record);
    reinterpret_cast<void (*)(void*)>(function->m_invoke)(function->m_storage);
    function->m_manager(function->m_storage, nullptr, 0);
}

}

// Device slot 0x58: the function moves into this frame's callbacks.
// ref: FUN_1422a8e10
void CGxDeviceD3d12::DeferCallback(blz::function_storage* callback) {
    gxDx12Backend::FrameData* frame = this->m_frame;
    auto lock = static_cast<SCritSect*>(frame->m_callbacks.m_lock);
    lock->Enter();
    auto record = static_cast<blz::function_storage*>(frame->m_callbacks.Add(RunDeferredFunction, sizeof(blz::function_storage), 8));
    lock->Leave();
    if (record != nullptr) {
        callback->m_manager(record->m_storage, callback->m_storage, 2);
        record->m_invoke = callback->m_invoke;
        record->m_manager = callback->m_manager;
        callback->m_invoke = nullptr;
        callback->m_manager = blz::function_empty_manager;
    }
    if (callback->m_manager != blz::function_empty_manager) {
        callback->m_manager(callback->m_storage, nullptr, 0);
    }
}

// Device slot 0x1a0: a static buffer takes its range of a heap now.
// ref: FUN_1422cd720
bool CGxDeviceD3d12::BufCreate(GxBuf* buf) {
    buf->m_resource = nullptr;
    buf->m_offset = 0;
    buf->m_gpu = 0;
    buf->m_heap = nullptr;
    buf->m_heapOffset = 0;
    buf->m_heapSize = 0;
    if (buf->m_type == GXBUF_STATIC) {
        this->BufAllocate(buf);
    }
    return buf != nullptr;
}

// Device slot 0x1a8: a buffer's heap range is freed once the frame is done.
// ref: FUN_1422cd8f0
void CGxDeviceD3d12::BufDestroy(GxBuf* buf) {
    auto page = static_cast<BufferPage*>(buf->m_heap);
    if (page != nullptr && buf->m_type != GXBUF_DYNAMIC) {
        BufFreeLater(this->m_frame, page, buf->m_heapOffset, buf->m_heapSize);
    }
    buf->m_flags &= ~GXBUF_UNLOCKED;
}

namespace {

struct BUFFREE {
    VariableSizeAllocationsManager* m_allocations;
    uint32_t m_offset;
    uint32_t m_size;
};

// ref: LAB_1422c7c40
void BufFreeRange(void* record) {
    auto range = static_cast<BUFFREE*>(record);
    range->m_allocations->Free(range->m_offset, range->m_size);
}

}

void gxDx12Backend::BufFreeLater(FrameData* frame, BufferPage* page, uint32_t offset, uint32_t size) {
    auto lock = static_cast<SCritSect*>(frame->m_callbacks.m_lock);
    lock->Enter();
    auto range = static_cast<BUFFREE*>(frame->m_callbacks.Add(BufFreeRange, sizeof(BUFFREE), 8));
    lock->Leave();
    if (range != nullptr) {
        range->m_allocations = &page->m_allocations;
        range->m_offset = offset;
        range->m_size = size;
    }
}

// ref: inline in FUN_1422d5a60
BufferPage::BufferPage(uint32_t size) {
    this->m_resource = nullptr;
    this->m_allocations.m_maxSize = size;
    this->m_allocations.m_freeSize = size;
    this->m_allocations.AddNewBlock(0, size);
    std::memset(&this->m_residency, 0, sizeof(this->m_residency));
    this->m_copyPending = 0;
}

BufferPage::~BufferPage() {
    if (this->m_residency.m_underlying != nullptr) {
        GxDeviceD3d12()->m_residency.EndTrackingObject(&this->m_residency);
    }
    ReleaseAndNull(&this->m_resource);
}

namespace {

// The page list a buffer usage's ranges come from.
blz::vector<BufferPage*, s_bufferPageTag>* BufPages(CGxDeviceD3d12* device, uint32_t usage) {
    if ((usage & GXBUFUSAGE_INDEX) != 0) {
        return &device->m_indexPages;
    }
    if ((usage & (GXBUFUSAGE_VERTEX | GXBUFUSAGE_CONSTANT)) != 0) {
        return &device->m_vertexPages;
    }
    return &device->m_otherPages;
}

// The greatest common divisor (FUN_1422c9620).
uint32_t BufAlignGcd(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

}

// A 32 MB committed buffer in the state its usage is read in, tracked for residency.
// ref: FUN_1422d5a60
BufferPage* CGxDeviceD3d12::CreateBufferPage(uint32_t usage) {
    blz::vector<BufferPage*, s_bufferPageTag>* pages = BufPages(this, usage);
    auto page = new (SMemAlloc(sizeof(BufferPage), 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0x1275, 0))
        BufferPage(0x2000000);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 0x2000000;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uint32_t state;
    if ((usage & GXBUFUSAGE_INDEX) != 0) {
        state = 0xc2;
    } else if ((usage & (GXBUFUSAGE_VERTEX | GXBUFUSAGE_CONSTANT)) != 0) {
        state = 0xc1;
    } else {
        state = 0xc0;
    }
    ID3D12Resource* resource = nullptr;
    HRESULT hr = this->m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        static_cast<D3D12_RESOURCE_STATES>(state), nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (FAILED(hr) || resource == nullptr) {
        this->Fail(hr, "ICreateBufferPage Create Failed");
        ReleaseAndNull(&resource);
        page->~BufferPage();
        operator delete(page, sizeof(BufferPage));
        return nullptr;
    }
    page->m_residency.m_underlying = resource;
    page->m_residency.m_size = 0x2000000;
    page->m_residency.m_lastGpuSyncPoint = 0;
    page->m_resource = resource;
    this->m_residency.BeginTrackingObject(&page->m_residency);
    this->m_bufferBytes += 0x2000000;
    pages->push_back(page);
    return page;
}

// A range for a static or stream buffer in its usage's pages, a new page when none has room.
// ref: FUN_1422d46a0
bool CGxDeviceD3d12::BufAllocate(GxBuf* buf) {
    if (this->m_ready == 0) {
        return false;
    }
    if (buf->m_type == GXBUF_DYNAMIC) {
        buf->m_heap = nullptr;
        buf->m_heapOffset = 0;
        buf->m_heapSize = 0;
    } else {
        uint32_t alignment = (buf->m_usage & GXBUFUSAGE_CONSTANT) != 0 ? 0x100 : 8;
        uint32_t element = (buf->m_flags >> GXBUF_ELEMENT_SHIFT) & 0xfff;
        uint32_t common = BufAlignGcd(alignment, element % alignment);
        uint32_t size = ((alignment - 1) + (buf->m_size - common) + element) & static_cast<uint32_t>(-static_cast<int32_t>(alignment));
        g_gxDeviceLock.Enter();
        blz::vector<BufferPage*, s_bufferPageTag>* pages = BufPages(this, buf->m_usage);
        BufferPage* found = nullptr;
        uint32_t offset = 0xffffffff;
        for (BufferPage* page : *pages) {
            offset = page->m_allocations.Allocate(size);
            if (offset != 0xffffffff) {
                found = page;
                break;
            }
        }
        if (found == nullptr) {
            found = this->CreateBufferPage(buf->m_usage);
            if (found == nullptr) {
                g_gxDeviceLock.Leave();
                return false;
            }
            offset = found->m_allocations.Allocate(size);
        }
        buf->m_heap = found;
        buf->m_heapOffset = offset;
        buf->m_heapSize = size;
        g_gxDeviceLock.Leave();
    }
    buf->m_flags &= ~GXBUF_UNLOCKED;
    return true;
}

// Device slot 0x148: the layout is kept as a copy; pipelines read it when they are built.
// ref: FUN_14229b570
uint16_t* CGxDeviceD3d12::CreateVertexBufferFormat(uint16_t* result, const GXVERTEXBUFFERFORMATDESC* desc,
    GXVERTEXBUFFERFORMAT** format) {
    *format = GxVertexBufferFormatCopy(desc);
    result[0] = 0;
    *reinterpret_cast<uint32_t*>(result + 2) = 0;
    return result;
}

// ---- the device's own slots that run translated ---------------------------------------------

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_1422cda20(unsigned long long, unsigned long long);
unsigned long long FUN_1422cda90(unsigned long long, unsigned long long);
unsigned long long FUN_1422ceaa0(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_1422ceb70(unsigned long long, unsigned long long);
}

// A graphics command list of its own (a pass that records apart, the shadow band's): the port's
// GraphicsCommandList, as the device makes its main one, handed out as its render context. The
// name the caller gives (`a2`) is the reference's debug label, not kept.
// ref: FUN_1422ce330
void* CGxDeviceD3d12::Slot090(void* out, uint64_t a2) {
    (void)a2;
    auto data = reinterpret_cast<const uint8_t*>(this->m_data);
    void* memory = SMemAlloc(sizeof(GraphicsCommandList), 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0xbbb, 0);
    auto list = new (memory) GraphicsCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT, data[0x219]);
    *static_cast<void**>(out) = &list->m_context;
    return out;
}

// A compute command list (FUN_1422c9f00 with type 2, 0x1d80 bytes) handed out as its compute
// context (+0xfb8, vtable 0x14291a070). The compute context is not ported (GraphicsCommandList
// leaves m_computeVtable null), so there is none to give: the caller gets null, which the
// ray-traced shadow passes asking for one check. Forwarded to the translation, the reference's
// constructor half-built a list beside the port's backend and wrote through a wild pointer
// (2026-10-22).
// ref: FUN_1422ce820
void* CGxDeviceD3d12::Slot0d8(void* out, uint64_t a2) {
    (void)a2;
    *static_cast<void**>(out) = nullptr;
    return out;
}

// A GPU event: one from the free list (+0x46f8, its count at +0x4700), else a new one from the
// event deque (+0x46d8, FUN_1422e8450) stamped with the frame (+0x46f0), under the device lock;
// its fence made afresh on the port's D3D12 device (the reference's DAT_14348bb70).
extern "C" unsigned long long FUN_1422e8450(unsigned long long);
// ref: FUN_1422d1580
void* CGxDeviceD3d12::Slot0f0(void* out) {
    auto self = reinterpret_cast<uint8_t*>(this);
    auto& count = *reinterpret_cast<uint64_t*>(self + 0x4700);
    uint64_t* event = nullptr;
    g_gxDeviceLock.Enter();
    if (count == 0) {
        uint32_t frame = *reinterpret_cast<uint32_t*>(self + 0x46f0);
        __try {
            event = reinterpret_cast<uint64_t*>(FUN_1422e8450(reinterpret_cast<unsigned long long>(self + 0x46d8)));
        } __except (GenFault(0x1422e8450ull, GetExceptionInformation())) {
            event = nullptr;
        }
        if (event != nullptr) {
            event[0] = 0;
            event[1] = 0;
            *reinterpret_cast<uint32_t*>(event + 2) = frame;
        }
    } else {
        event = reinterpret_cast<uint64_t**>(*reinterpret_cast<uint64_t*>(self + 0x46f8))[count - 1];
        count--;
    }
    g_gxDeviceLock.Leave();
    if (event != nullptr) {
        auto& fence = *reinterpret_cast<ID3D12Fence**>(event + 1);
        if (fence != nullptr) {
            ID3D12Fence* old = fence;
            fence = nullptr;
            old->Release();
        }
        HRESULT hr = this->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
        if (FAILED(hr)) {
            this->Fail(hr, "GxEventCreate failed");
        }
    }
    *static_cast<void**>(out) = event;
    return out;
}

extern "C" unsigned long long FUN_1422ddd40(unsigned long long, unsigned long long);
// translated: FUN_1422ddd40
void* CGxDeviceD3d12::Slot1c0(const void* desc) {
    __try {
        return reinterpret_cast<void*>(FUN_1422ddd40(reinterpret_cast<unsigned long long>(this), (unsigned long long)(desc)));
    } __except (GenFault(0x1422ddd40ull, GetExceptionInformation())) {
        return nullptr;
    }
}
// A 0x20-byte description copied to the heap.
// translated: FUN_1422cda20
void* CGxDeviceD3d12::Slot1b0(const void* desc) {
    __try {
        return reinterpret_cast<void*>(FUN_1422cda20(reinterpret_cast<unsigned long long>(this),
            reinterpret_cast<unsigned long long>(desc)));
    } __except (GenFault(0x1422cda20ull, GetExceptionInformation())) {
        return nullptr;
    }
}

// translated: FUN_1422cda90
void CGxDeviceD3d12::Slot1b8(void* object) {
    __try {
        FUN_1422cda90(reinterpret_cast<unsigned long long>(this), reinterpret_cast<unsigned long long>(object));
    } __except (GenFault(0x1422cda90ull, GetExceptionInformation())) {
    }
}

// A compute shader object (0x38 bytes): its kind and permutation, loaded at once when shaders
// are loaded up front (DAT_14348b689, FUN_1422d4d60).
// translated: FUN_1422ceaa0
void* CGxDeviceD3d12::CreateComputeShader(uint32_t kind, uint16_t permutation) {
    __try {
        return reinterpret_cast<void*>(FUN_1422ceaa0(reinterpret_cast<unsigned long long>(this), kind, permutation));
    } __except (GenFault(0x1422ceaa0ull, GetExceptionInformation())) {
        return nullptr;
    }
}

// Its pipeline state released through the device's deferred releases (+0xa48), then the object.
// translated: FUN_1422ceb70
void CGxDeviceD3d12::DestroyComputeShader(void* shader) {
    __try {
        FUN_1422ceb70(reinterpret_cast<unsigned long long>(this), reinterpret_cast<unsigned long long>(shader));
    } __except (GenFault(0x1422ceb70ull, GetExceptionInformation())) {
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1422cda20=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1422cda90=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1422ceaa0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1422ceb70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1422dbce0=GenForwardMissing")
// ---- end of link fallbacks ----
