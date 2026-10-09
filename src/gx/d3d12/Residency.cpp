#include "gx/d3d12/Residency.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <cstring>

namespace D3DX12Residency {

namespace {

const char s_residencyFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\D3d12\\d3dx12Residency.h";

uint32_t ResidencyFileHash() {
    return SStrHashFnv1a(s_residencyFile);
}

// The generations whose submissions are still on the GPU: one entry per EnqueueSyncPoint,
// with the value each queue's fence reaches when that submission is done.
struct SyncPoint {
    struct QueueValue {
        QueueSyncPoint* m_queue;
        uint64_t m_value;
    };

    uint64_t m_generation;          // 0x00
    uint32_t m_count;               // 0x08
    uint32_t m_pad0c;
    LIST_ENTRY m_listEntry;         // 0x10
    QueueValue m_queues[1];         // 0x20
};

SyncPoint* SyncPointFromEntry(LIST_ENTRY* entry) {
    return CONTAINING_RECORD(entry, SyncPoint, m_listEntry);
}

ManagedObject* ObjectFromEntry(LIST_ENTRY* entry) {
    return CONTAINING_RECORD(entry, ManagedObject, m_listEntry);
}

void RemoveEntry(LIST_ENTRY* entry) {
    entry->Blink->Flink = entry->Flink;
    entry->Flink->Blink = entry->Blink;
}

void InsertHead(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Blink = head;
    entry->Flink = head->Flink;
    head->Flink->Blink = entry;
    head->Flink = entry;
}

void InsertTail(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->Flink = head;
    entry->Blink = head->Blink;
    head->Blink->Flink = entry;
    head->Blink = entry;
}

uint64_t SecondsToTicks(float ticksPerSecond, float seconds) {
    return static_cast<uint64_t>(ticksPerSecond * seconds);
}

HRESULT LastErrorResult() {
    HRESULT hr = static_cast<HRESULT>(GetLastError());
    if (hr > 0) {
        hr = static_cast<HRESULT>((static_cast<uint32_t>(hr) & 0xffff) | 0x80070000);
    }
    return hr;
}

// Frees a set the manager made (the work ring's and the master sets).
void DeleteSet(ResidencySet* set) {
    if (set == nullptr) {
        return;
    }
    if (set->m_set != nullptr) {
        delete[] set->m_set;
    }
    operator delete(set, sizeof(ResidencySet));
}

// Grows a set's array by half (from 0x1000 when empty), as Insert does.
void GrowSet(ResidencySet* set) {
    int32_t size = set->m_maxSize == 0 ? 0x1000 : static_cast<int32_t>(static_cast<float>(set->m_maxSize) * 1.5f);
    set->m_maxSize = size;
    auto grown = new (0xd, s_residencyFile, ResidencyFileHash(), 0x85) ManagedObject*[size];
    if (set->m_set != nullptr && grown != nullptr) {
        std::memcpy(grown, set->m_set, static_cast<size_t>(set->m_count) * sizeof(ManagedObject*));
    }
    ManagedObject** old = set->m_set;
    set->m_set = grown;
    if (old != nullptr) {
        delete[] old;
    }
}

}

// ref: FUN_1422dc670
HRESULT ResidencySet::Open() {
    CRITICAL_SECTION* mutex = this->m_mutex;
    if (mutex != nullptr) {
        EnterCriticalSection(mutex);
    }
    HRESULT hr = S_OK;
    if (this->m_isOpen == 0) {
        auto indices = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(this->m_mutex) + sizeof(CRITICAL_SECTION));
        uint64_t free = ~*indices;
        if (free != 0) {
            uint32_t index = 0;
            while (((free >> index) & 1) == 0) {
                index++;
            }
            this->m_index = index;
            *indices |= 1ull << index;
            this->m_count = 0;
            this->m_isOpen = 1;
            this->m_outOfMemory = 0;
        } else {
            hr = E_OUTOFMEMORY;
        }
    } else {
        hr = E_INVALIDARG;
    }
    if (mutex != nullptr) {
        LeaveCriticalSection(mutex);
    }
    return hr;
}

// ref: FUN_1422ce1a0
HRESULT ResidencySet::Close() {
    if (this->m_isOpen == 0) {
        return E_INVALIDARG;
    }
    if (this->m_outOfMemory == 1) {
        return E_OUTOFMEMORY;
    }
    for (int32_t i = 0; i < this->m_count; i++) {
        InterlockedAnd64(reinterpret_cast<volatile LONG64*>(&this->m_set[i]->m_commandListsUsedOn),
            ~static_cast<LONG64>(1ull << (this->m_index & 0x3f)));
    }
    uint32_t index = this->m_index;
    CRITICAL_SECTION* mutex = this->m_mutex;
    if (mutex != nullptr) {
        EnterCriticalSection(mutex);
    }
    auto indices = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(this->m_mutex) + sizeof(CRITICAL_SECTION));
    *indices &= ~(1ull << (index & 0x3f));
    this->m_index = 0xffffffff;
    this->m_isOpen = 0;
    if (mutex != nullptr) {
        LeaveCriticalSection(mutex);
    }
    this->m_isOpen = 0;
    return S_OK;
}

// ref: FUN_1422db640
bool ResidencySet::Insert(ManagedObject* object) {
    uint64_t bit = 1ull << (this->m_index & 0x3f);
    if ((object->m_commandListsUsedOn & bit) != 0) {
        return false;
    }
    InterlockedOr64(reinterpret_cast<volatile LONG64*>(&object->m_commandListsUsedOn), static_cast<LONG64>(bit));
    if (this->m_set == nullptr || this->m_maxSize <= this->m_count) {
        GrowSet(this);
    }
    if (this->m_set != nullptr) {
        this->m_set[this->m_count] = object;
        this->m_count++;
        return true;
    }
    this->m_outOfMemory = 1;
    return false;
}

// The constructor's part (inline in FUN_1422cacf0).
void ResidencyManager::Construct() {
    this->m_queueSize = 7;
    this->m_queue = nullptr;
    this->m_asyncWorkEvent = INVALID_HANDLE_VALUE;
    this->m_asyncThread = INVALID_HANDLE_VALUE;
    InitializeCriticalSectionAndSpinCount(&this->m_syncCS, 8);
    this->m_finishAsyncWork = 0;
    this->m_readIndex = 0;
    this->m_list88.Flink = &this->m_list88;
    this->m_list88.Blink = &this->m_list88;
    this->m_writeIndex = 0;
    this->m_numQueueSyncPoints = 0;
    this->m_asyncFence = nullptr;
    this->m_asyncFenceValue = 0;
    this->m_syncGeneration = 0;
    this->m_completionEvent = INVALID_HANDLE_VALUE;
    this->m_asyncCompletionEvent = INVALID_HANDLE_VALUE;
    this->m_device = nullptr;
    this->m_lru.m_numResident = 0;
    this->m_lru.m_numEvicted = 0;
    this->m_lru.m_residentSize = 0;
    this->m_lru.m_resident.Flink = &this->m_lru.m_resident;
    this->m_lru.m_resident.Blink = &this->m_lru.m_resident;
    this->m_lru.m_evicted.Flink = &this->m_lru.m_evicted;
    this->m_lru.m_evicted.Blink = &this->m_lru.m_evicted;
    this->m_device3 = nullptr;
    this->m_deviceDownlevel = nullptr;
    this->m_nodeIndex = 0;
    this->m_adapter = nullptr;
    InitializeCriticalSectionAndSpinCount(&this->m_mutex, 8);
    InitializeCriticalSectionAndSpinCount(&this->m_executionCS, 8);
    this->m_startEvicted = 0;
    this->m_queueSyncPoints.Flink = &this->m_queueSyncPoints;
    this->m_queueSyncPoints.Blink = &this->m_queueSyncPoints;
    this->m_syncPoints.Flink = &this->m_syncPoints;
    this->m_syncPoints.Blink = &this->m_syncPoints;
    this->m_minEvictionGracePeriod = 1.0f;
    this->m_maxEvictionGracePeriod = 60.0f;
    this->m_trimRatio = 0.7f;
    this->m_maxSoftwareQueueLatency = 6;
    this->m_setMutex = &this->m_setLock;
    static volatile LONG64 s_managerCount;  // DAT_143490a20
    this->m_id = static_cast<uint64_t>(InterlockedIncrement64(&s_managerCount));
    InitializeCriticalSectionAndSpinCount(&this->m_setLock, 8);
    this->m_setIndices = 0;
}

// ref: FUN_1422db330
HRESULT ResidencyManager::Initialize(ID3D12Device* device, uint32_t nodeIndex, IDXGIAdapter3* adapter,
    uint32_t maxLatency) {
    this->m_device = device;
    this->m_nodeIndex = nodeIndex;
    this->m_maxSoftwareQueueLatency = maxLatency;
    this->m_adapter = adapter;
    if (FAILED(device->QueryInterface(__uuidof(ID3D12Device3), reinterpret_cast<void**>(&this->m_device3)))) {
        this->m_asyncFenceValue++;
    }
    this->m_device->QueryInterface(__uuidof(ID3D12DeviceDownlevel), reinterpret_cast<void**>(&this->m_deviceDownlevel));
    if (this->m_deviceDownlevel != nullptr && this->m_device3 == nullptr) {
        return E_NOINTERFACE;
    }
    uint64_t size = static_cast<uint64_t>(maxLatency) + 1;
    this->m_queueSize = size;
    auto queue = new (0xd, s_residencyFile, ResidencyFileHash(), 0x85) AsyncWorkload[size];
    for (uint64_t i = 0; i < size; i++) {
        queue[i].m_syncGeneration = 0;
        queue[i].m_masterSet = nullptr;
        queue[i].m_fenceValueToSignal = 0;
    }
    AsyncWorkload* old = this->m_queue;
    this->m_queue = queue;
    if (old != nullptr) {
        delete[] old;
    }
    if (this->m_queue == nullptr) {
        return E_OUTOFMEMORY;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    float ticksPerSecond = static_cast<float>(frequency.QuadPart);
    this->m_minEvictionGracePeriodTicks = SecondsToTicks(ticksPerSecond, this->m_minEvictionGracePeriod);
    this->m_maxEvictionGracePeriodTicks = SecondsToTicks(ticksPerSecond, this->m_maxEvictionGracePeriod);
    HRESULT hr = this->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
        reinterpret_cast<void**>(&this->m_asyncFence));
    if (FAILED(hr)) {
        return hr;
    }
    // The reference tests these against INVALID_HANDLE_VALUE, which CreateEvent never returns.
    this->m_completionEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (this->m_completionEvent == INVALID_HANDLE_VALUE) {
        hr = LastErrorResult();
        if (FAILED(hr)) {
            return hr;
        }
    }
    this->m_asyncCompletionEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (this->m_asyncCompletionEvent == INVALID_HANDLE_VALUE) {
        hr = LastErrorResult();
        if (FAILED(hr)) {
            return hr;
        }
    }
    this->m_asyncWorkEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (this->m_asyncWorkEvent == INVALID_HANDLE_VALUE) {
        hr = LastErrorResult();
        if (FAILED(hr)) {
            return hr;
        }
    }
    if (this->m_device3 == nullptr) {
        this->m_asyncThread = CreateThread(nullptr, 0, AsyncThreadStart, this, 0, nullptr);
        if (this->m_asyncThread == INVALID_HANDLE_VALUE) {
            hr = LastErrorResult();
        }
    }
    return hr;
}

// ref: FUN_1422d0d50
void ResidencyManager::Destroy() {
    if (this->m_asyncFence != nullptr) {
        this->m_asyncFence->Release();
        this->m_asyncFence = nullptr;
    }
    if (this->m_completionEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(this->m_completionEvent);
        this->m_completionEvent = INVALID_HANDLE_VALUE;
    }
    // Drops the work the thread has not picked up.
    if (this->m_readIndex != this->m_writeIndex) {
        AsyncWorkload* work = &this->m_queue[this->m_readIndex % this->m_queueSize];
        this->m_readIndex++;
        while (work != nullptr && this->m_readIndex != this->m_writeIndex) {
            work = &this->m_queue[this->m_readIndex % this->m_queueSize];
            this->m_readIndex++;
        }
    }
    this->m_finishAsyncWork = 1;
    if (!SetEvent(this->m_asyncWorkEvent)) {
        GetLastError();
    }
    if (this->m_asyncThread != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(this->m_asyncThread, INFINITE);
        CloseHandle(this->m_asyncThread);
        this->m_asyncThread = INVALID_HANDLE_VALUE;
    }
    if (this->m_asyncWorkEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(this->m_asyncWorkEvent);
        this->m_asyncWorkEvent = INVALID_HANDLE_VALUE;
    }
    if (this->m_asyncCompletionEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(this->m_asyncCompletionEvent);
        this->m_asyncCompletionEvent = INVALID_HANDLE_VALUE;
    }
    while (this->m_queueSyncPoints.Flink != &this->m_queueSyncPoints) {
        LIST_ENTRY* entry = this->m_queueSyncPoints.Flink;
        QueueSyncPoint* point = CONTAINING_RECORD(entry, QueueSyncPoint, m_listEntry);
        if (point->m_fence != nullptr) {
            point->m_fence->Release();
            point->m_fence = nullptr;
        }
        RemoveEntry(this->m_queueSyncPoints.Flink);
        operator delete(point, sizeof(QueueSyncPoint));
    }
    if (this->m_device3 != nullptr) {
        this->m_device3->Release();
        this->m_device3 = nullptr;
    }
    if (this->m_deviceDownlevel != nullptr) {
        this->m_deviceDownlevel->Release();
        this->m_deviceDownlevel = nullptr;
    }
    while (this->m_syncPoints.Flink != &this->m_syncPoints) {
        LIST_ENTRY* entry = this->m_syncPoints.Flink;
        RemoveEntry(entry);
        delete[] reinterpret_cast<uint8_t*>(SyncPointFromEntry(entry));
    }
    AsyncWorkload* queue = this->m_queue;
    this->m_queue = nullptr;
    if (queue != nullptr) {
        // FUN_1422a8c20: each entry's master set goes with it, last first.
        for (uint64_t i = this->m_queueSize; i != 0; i--) {
            DeleteSet(queue[i - 1].m_masterSet);
        }
        delete[] queue;
    }
}

// ref: FUN_1422cd650
void ResidencyManager::BeginTrackingObject(ManagedObject* object) {
    EnterCriticalSection(&this->m_mutex);
    if (object != nullptr) {
        if (this->m_startEvicted != 0) {
            object->m_residencyStatus = ManagedObject::EVICTED;
            this->m_device->Evict(1, &object->m_underlying);
        }
        if (object->m_residencyStatus == ManagedObject::RESIDENT) {
            InsertHead(&this->m_lru.m_resident, &object->m_listEntry);
            this->m_lru.m_numResident++;
            this->m_lru.m_residentSize += object->m_size;
        } else {
            InsertHead(&this->m_lru.m_evicted, &object->m_listEntry);
            this->m_lru.m_numEvicted++;
        }
    }
    LeaveCriticalSection(&this->m_mutex);
}

// ref: FUN_1422c2c80
void ResidencyManager::EndTrackingObject(ManagedObject* object) {
    EnterCriticalSection(&this->m_mutex);
    RemoveEntry(&object->m_listEntry);
    if (object->m_residencyStatus == ManagedObject::RESIDENT) {
        this->m_lru.m_numResident--;
        this->m_lru.m_residentSize -= object->m_size;
    } else {
        this->m_lru.m_numEvicted--;
    }
    LeaveCriticalSection(&this->m_mutex);
}

void ResidencyManager::QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO* info) {
    if (this->m_adapter == nullptr) {
        this->m_deviceDownlevel->QueryVideoMemoryInfo(this->m_nodeIndex, group, info);
    } else {
        this->m_adapter->QueryVideoMemoryInfo(this->m_nodeIndex, group, info);
    }
}

// Builds one set from the lists' sets, each object once. S_FALSE when several lists together
// would not fit the budget: the caller then submits them in halves.
// ref: FUN_1422d3850
HRESULT ResidencyManager::PrepareMasterSet(ResidencySet** sets, uint32_t count, ResidencySet** master) {
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
    this->QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal = {};
    this->QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (sets[i] != nullptr) {
            if (sets[i]->m_isOpen != 0) {
                return E_INVALIDARG;
            }
            total += static_cast<uint32_t>(sets[i]->m_count);
        }
    }
    auto set = static_cast<ResidencySet*>(operator new(sizeof(ResidencySet), 0xd, s_residencyFile, ResidencyFileHash(), 0x7f));
    set->m_maxSize = 0;
    set->m_count = 0;
    set->m_mutex = nullptr;
    set->m_index = 0xffffffff;
    set->m_set = nullptr;
    set->m_isOpen = 0;
    set->m_outOfMemory = 0;
    set->m_mutex = this->m_setMutex;
    int32_t size = total != 0 ? static_cast<int32_t>(total) : 1;
    set->m_maxSize = size;
    auto array = new (0xd, s_residencyFile, ResidencyFileHash(), 0x85) ManagedObject*[size];
    ManagedObject** old = set->m_set;
    set->m_set = array;
    if (old != nullptr) {
        delete[] old;
    }
    HRESULT hr;
    if (set->m_set == nullptr) {
        hr = E_OUTOFMEMORY;
    } else {
        hr = set->Open();
        if (SUCCEEDED(hr)) {
            uint64_t sizeToMakeResident = 0;
            for (uint32_t i = 0; i < count; i++) {
                ResidencySet* source = sets[i];
                if (source == nullptr) {
                    continue;
                }
                for (int32_t j = 0; j < source->m_count; j++) {
                    if (set->Insert(source->m_set[j])) {
                        sizeToMakeResident += sets[i]->m_set[j]->m_size;
                    }
                }
            }
            hr = set->Close();
            if (SUCCEEDED(hr)) {
                if (count >= 2) {
                    hr = S_FALSE;
                }
                if (count < 2 || sizeToMakeResident <= local.Budget + nonLocal.Budget) {
                    ResidencySet* previous = *master;
                    *master = set;
                    DeleteSet(previous);
                    return S_OK;
                }
            }
        }
    }
    DeleteSet(set);
    return hr;
}

// Submits the lists behind their master set's paging work, then records the queue's fence
// value as a sync point.
// ref: FUN_1422c7430
HRESULT ResidencyManager::ExecuteMasterSet(ID3D12CommandQueue* queue, ResidencySet** master, void* execution) {
    struct EXECUTION {
        ID3D12CommandQueue* m_queue;
        ID3D12CommandList** m_lists;
        uint32_t m_count;
    };
    auto exec = static_cast<EXECUTION*>(execution);
    QueueSyncPoint* syncPoint = nullptr;
    HRESULT hr = this->GetQueueSyncPoint(queue, &syncPoint);
    if (SUCCEEDED(hr)) {
        EnterCriticalSection(&this->m_executionCS);
        uint64_t generation = this->m_syncGeneration;
        uint64_t fenceValue = this->m_asyncFenceValue;
        ResidencySet* set = *master;
        *master = nullptr;
        while (this->m_maxSoftwareQueueLatency <= this->m_writeIndex - this->m_readIndex) {
            WaitForSingleObject(this->m_asyncCompletionEvent, INFINITE);
        }
        AsyncWorkload* slot = &this->m_queue[this->m_writeIndex % this->m_queueSize];
        ResidencySet* previous = slot->m_masterSet;
        slot->m_masterSet = set;
        DeleteSet(previous);
        slot->m_fenceValueToSignal = fenceValue;
        slot->m_syncGeneration = generation;
        this->m_writeIndex++;
        if (SetEvent(this->m_asyncWorkEvent)) {
            hr = S_OK;
        } else {
            hr = LastErrorResult();
        }
        if (this->m_device3 != nullptr) {
            AsyncWorkload* work = nullptr;
            if (this->m_readIndex != this->m_writeIndex) {
                work = &this->m_queue[this->m_readIndex % this->m_queueSize];
                this->m_readIndex++;
            }
            this->ProcessPagingWork(work);
        }
        if (SUCCEEDED(hr)) {
            hr = queue->Wait(this->m_asyncFence, this->m_asyncFenceValue);
            if (this->m_device3 == nullptr) {
                this->m_asyncFenceValue++;
            }
        }
        exec->m_queue->ExecuteCommandLists(exec->m_count, exec->m_lists);
        if (SUCCEEDED(hr)) {
            hr = this->EnqueueSyncPoint(queue, syncPoint);
        }
        LeaveCriticalSection(&this->m_executionCS);
    }
    DeleteSet(*master);
    return hr;
}

// ref: FUN_1422d21e0
HRESULT ResidencyManager::ExecuteCommandLists(ID3D12CommandQueue* queue, ID3D12CommandList** lists,
    ResidencySet** sets, uint32_t count) {
    if (count == 0) {
        return S_OK;
    }
    ResidencySet* master = nullptr;
    HRESULT hr = this->PrepareMasterSet(sets, count, &master);
    if (SUCCEEDED(hr)) {
        if (master != nullptr) {
            struct {
                ID3D12CommandQueue* m_queue;
                ID3D12CommandList** m_lists;
                uint32_t m_count;
            } execution = { queue, lists, count };
            return this->ExecuteMasterSet(queue, &master, &execution);
        }
        uint32_t half = count >> 1;
        HRESULT first = this->ExecuteCommandLists(queue, lists, sets, half);
        HRESULT second = this->ExecuteCommandLists(queue, lists + half, sets + half, count - half);
        hr = (first != S_OK || second != S_OK) ? E_FAIL : S_OK;
    }
    DeleteSet(master);
    return hr;
}

// The fence the manager signals on `queue`, made and attached to the queue the first time.
// ref: FUN_1422d36d0
HRESULT ResidencyManager::GetQueueSyncPoint(ID3D12CommandQueue* queue, QueueSyncPoint** syncPoint) {
    // The private data's key is a GUID whose last eight bytes are the queue's address.
    GUID key = { 0xf0, 0, 0xd, { 0 } };
    std::memcpy(key.Data4, &queue, sizeof(queue));
    struct {
        QueueSyncPoint* m_point;
        uint64_t m_id;
    } data = {};
    *syncPoint = nullptr;
    UINT size = sizeof(data);
    HRESULT hr = queue->GetPrivateData(key, &size, &data);
    if (FAILED(hr) || this->m_id != data.m_id) {
        auto point = static_cast<QueueSyncPoint*>(
            operator new(sizeof(QueueSyncPoint), 0xd, s_residencyFile, ResidencyFileHash(), 0x401));
        point->m_fence = nullptr;
        point->m_listEntry.Flink = &point->m_listEntry;
        point->m_listEntry.Blink = &point->m_listEntry;
        point->m_value = 1;
        *syncPoint = point;
        HRESULT created = this->m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&point->m_fence));
        InsertTail(&this->m_queueSyncPoints, &(*syncPoint)->m_listEntry);
        InterlockedIncrement(&this->m_numQueueSyncPoints);
        if (FAILED(created)) {
            *syncPoint = data.m_point;
            return created;
        }
        data.m_point = *syncPoint;
        data.m_id = this->m_id;
        hr = queue->SetPrivateData(key, sizeof(data), &data);
    }
    *syncPoint = data.m_point;
    return hr;
}

// ref: FUN_1422e0200
HRESULT ResidencyManager::EnqueueSyncPoint(ID3D12CommandQueue* queue, QueueSyncPoint* syncPoint) {
    HRESULT hr = queue->Signal(syncPoint->m_fence, syncPoint->m_value);
    syncPoint->m_value++;
    if (SUCCEEDED(hr)) {
        EnterCriticalSection(&this->m_syncCS);
        uint32_t count = static_cast<uint32_t>(this->m_numQueueSyncPoints);
        uint64_t generation = this->m_syncGeneration;
        auto point = reinterpret_cast<SyncPoint*>(new (0xd, s_residencyFile, ResidencyFileHash(), 0x204)
                uint8_t[(static_cast<uint64_t>(count - 1) + 3) * 0x10]);
        if (point == nullptr) {
            LeaveCriticalSection(&this->m_syncCS);
            hr = E_OUTOFMEMORY;
        } else {
            point->m_count = count;
            point->m_generation = generation;
            point->m_listEntry.Flink = nullptr;
            point->m_listEntry.Blink = nullptr;
            uint32_t index = 0;
            for (LIST_ENTRY* entry = this->m_queueSyncPoints.Flink; entry != &this->m_queueSyncPoints;) {
                LIST_ENTRY* next = entry->Flink;
                QueueSyncPoint* queuePoint = CONTAINING_RECORD(entry, QueueSyncPoint, m_listEntry);
                point->m_queues[index].m_queue = queuePoint;
                point->m_queues[index].m_value = queuePoint->m_value - 1;
                index++;
                entry = next;
            }
            InsertTail(&this->m_syncPoints, &point->m_listEntry);
            LeaveCriticalSection(&this->m_syncCS);
        }
    }
    this->m_syncGeneration++;
    return hr;
}

// Frees the sync points the GPU has passed; returns the oldest it has not.
// ref: FUN_1422d0c80
void* ResidencyManager::DequeueCompletedSyncPoints() {
    EnterCriticalSection(&this->m_syncCS);
    while (this->m_syncPoints.Flink != &this->m_syncPoints) {
        SyncPoint* point = SyncPointFromEntry(this->m_syncPoints.Flink);
        for (uint32_t i = 0; i < point->m_count; i++) {
            if (point->m_queues[i].m_queue->m_fence->GetCompletedValue() < point->m_queues[i].m_value) {
                LeaveCriticalSection(&this->m_syncCS);
                return point;
            }
        }
        RemoveEntry(this->m_syncPoints.Flink);
        delete[] reinterpret_cast<uint8_t*>(point);
    }
    LeaveCriticalSection(&this->m_syncCS);
    return nullptr;
}

// Makes the master set's objects resident, evicting what has gone unused when they do not
// fit, and waiting on the GPU for objects it still needs when nothing else can go.
// ref: FUN_1422dcbb0
void ResidencyManager::ProcessPagingWork(AsyncWorkload* work) {
    auto oldest = static_cast<SyncPoint*>(this->DequeueCompletedSyncPoints());
    uint32_t numToMakeResident = 0;
    uint64_t sizeToMakeResident = 0;
    uint32_t numToEvict = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&this->m_mutex);
    ResidencySet* master = work->m_masterSet;
    auto toMakeResident = reinterpret_cast<void**>(new (0xd, s_residencyFile, ResidencyFileHash(), 0x85)
            ManagedObject*[static_cast<uint64_t>(master->m_count)]);
    auto toEvict = new (0xd, s_residencyFile, ResidencyFileHash(), 0x85) ID3D12Pageable*[this->m_lru.m_numResident];
    for (int32_t i = 0; i < work->m_masterSet->m_count; i++) {
        ManagedObject* object = work->m_masterSet->m_set[i];
        if (object->m_residencyStatus == ManagedObject::EVICTED) {
            toMakeResident[numToMakeResident++] = object;
            object->m_residencyStatus = ManagedObject::RESIDENT;
            RemoveEntry(&object->m_listEntry);
            InsertTail(&this->m_lru.m_resident, &object->m_listEntry);
            this->m_lru.m_numEvicted--;
            this->m_lru.m_numResident++;
            this->m_lru.m_residentSize += object->m_size;
            object = work->m_masterSet->m_set[i];
            sizeToMakeResident += object->m_size;
        }
        object->m_lastGpuSyncPoint = work->m_syncGeneration;
        work->m_masterSet->m_set[i]->m_lastUsedTimestamp = static_cast<uint64_t>(now.QuadPart);
        ManagedObject* touched = work->m_masterSet->m_set[i];
        RemoveEntry(&touched->m_listEntry);
        InsertTail(&this->m_lru.m_resident, &touched->m_listEntry);
    }

    // Objects unused for longer than the grace period, which shrinks as memory fills.
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {};
    this->QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    uint64_t gracePeriod = UINT64_MAX;
    if (local.Budget != 0) {
        double ratio = static_cast<double>(local.CurrentUsage) / static_cast<double>(local.Budget);
        if (1.0 <= ratio) {
            ratio = 1.0;
        }
        double trim = static_cast<double>(this->m_trimRatio);
        if (trim < ratio) {
            double scale = (1.0 - (ratio - trim) / static_cast<double>(1.0f - this->m_trimRatio)) *
                static_cast<double>(this->m_maxEvictionGracePeriodTicks - this->m_minEvictionGracePeriodTicks);
            gracePeriod = static_cast<uint64_t>(scale) + this->m_minEvictionGracePeriodTicks;
        }
    }
    LIST_ENTRY* resident = &this->m_lru.m_resident;
    for (LIST_ENTRY* entry = resident->Flink; entry != resident; entry = resident->Flink) {
        ManagedObject* object = ObjectFromEntry(entry);
        if ((oldest != nullptr && oldest->m_generation <= object->m_lastGpuSyncPoint) ||
            static_cast<uint64_t>(now.QuadPart) - object->m_lastUsedTimestamp <= gracePeriod) {
            break;
        }
        toEvict[numToEvict++] = object->m_underlying;
        object->m_residencyStatus = ManagedObject::EVICTED;
        RemoveEntry(entry);
        InsertTail(&this->m_lru.m_evicted, entry);
        this->m_lru.m_numResident--;
        this->m_lru.m_residentSize -= object->m_size;
        this->m_lru.m_numEvicted++;
    }
    if (numToEvict != 0) {
        this->m_device->Evict(numToEvict, toEvict);
    }

    if (numToMakeResident != 0) {
        uint32_t madeResident = 0;
        uint32_t start = 0;
        while (true) {
            DXGI_QUERY_VIDEO_MEMORY_INFO localInfo = {};
            this->QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localInfo);
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalInfo = {};
            this->QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalInfo);
            int64_t totalBudget = static_cast<int64_t>(localInfo.Budget + nonLocalInfo.Budget);
            int64_t totalUsage = static_cast<int64_t>(nonLocalInfo.CurrentUsage + localInfo.CurrentUsage);
            uint32_t batchStart = start;
            bool madeBatch = false;
            if (totalBudget - totalUsage >= 1) {
                uint32_t inBatch = 0;
                uint64_t batchSize = 0;
                uint32_t next = start;
                for (uint32_t i = start; i < numToMakeResident; i++) {
                    uint64_t grown = static_cast<ManagedObject*>(toMakeResident[i])->m_size + batchSize;
                    if (static_cast<uint64_t>(totalBudget - totalUsage) < grown) {
                        next = i;
                        break;
                    }
                    madeResident++;
                    toMakeResident[i] = static_cast<ManagedObject*>(toMakeResident[i])->m_underlying;
                    inBatch++;
                    batchSize = grown;
                }
                start = next;
                HRESULT hr;
                if (this->m_device3 == nullptr) {
                    hr = this->m_device->MakeResident(inBatch, reinterpret_cast<ID3D12Pageable**>(toMakeResident + batchStart));
                } else {
                    hr = this->m_device3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, inBatch,
                        reinterpret_cast<ID3D12Pageable**>(toMakeResident + batchStart), this->m_asyncFence,
                        this->m_asyncFenceValue + 1);
                    if (SUCCEEDED(hr)) {
                        this->m_asyncFenceValue++;
                    }
                }
                if (SUCCEEDED(hr)) {
                    sizeToMakeResident -= batchSize;
                    madeBatch = true;
                }
            }
            if ((totalBudget - totalUsage < 1 || madeBatch) && madeResident == numToMakeResident) {
                break;
            }

            // Out of room: evict what the GPU is done with, waiting for it if it must.
            LIST_ENTRY* first = resident->Flink;
            ManagedObject* lru = first != resident ? ObjectFromEntry(first) : nullptr;
            auto pending = static_cast<SyncPoint*>(this->DequeueCompletedSyncPoints());
            if (lru == nullptr || work->m_syncGeneration <= lru->m_lastGpuSyncPoint || pending == nullptr) {
                // Nothing more can go: page the rest in and let the OS sort it out.
                for (uint32_t i = madeResident; i < numToMakeResident; i++) {
                    toMakeResident[i] = static_cast<ManagedObject*>(toMakeResident[i])->m_underlying;
                }
                if (this->m_device3 == nullptr) {
                    this->m_device->MakeResident(numToMakeResident - start,
                        reinterpret_cast<ID3D12Pageable**>(toMakeResident + start));
                } else if (SUCCEEDED(this->m_device3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, numToMakeResident - start,
                               reinterpret_cast<ID3D12Pageable**>(toMakeResident + start), this->m_asyncFence,
                               this->m_asyncFenceValue + 1))) {
                    this->m_asyncFenceValue++;
                }
                break;
            }
            uint64_t target = pending->m_generation == work->m_syncGeneration ? pending->m_generation - 1 : pending->m_generation;
            // Drops the generations before the target and waits out the target itself. (The
            // reference re-reads the list head without an emptiness check: the pending point
            // is in the list and not past the target, so the walk stops at it.)
            EnterCriticalSection(&this->m_syncCS);
            while (this->m_syncPoints.Flink != &this->m_syncPoints) {
                SyncPoint* point = SyncPointFromEntry(this->m_syncPoints.Flink);
                if (target < point->m_generation) {
                    break;
                }
                if (point->m_generation < target) {
                    RemoveEntry(&point->m_listEntry);
                    delete[] reinterpret_cast<uint8_t*>(point);
                    continue;
                }
                HANDLE event = this->m_completionEvent;
                for (uint32_t i = 0; i < point->m_count; i++) {
                    ID3D12Fence* fence = point->m_queues[i].m_queue->m_fence;
                    if (fence->GetCompletedValue() < point->m_queues[i].m_value) {
                        fence->SetEventOnCompletion(point->m_queues[i].m_value, event);
                        WaitForSingleObject(event, INFINITE);
                    }
                }
                RemoveEntry(&point->m_listEntry);
                delete[] reinterpret_cast<uint8_t*>(point);
                break;
            }
            LeaveCriticalSection(&this->m_syncCS);
            numToEvict = 0;
            int64_t needed = totalUsage + static_cast<int64_t>(sizeToMakeResident);
            for (LIST_ENTRY* entry = resident->Flink; entry != resident && ObjectFromEntry(entry)->m_lastGpuSyncPoint <= target &&
                 totalBudget <= needed;
                 entry = resident->Flink) {
                ManagedObject* object = ObjectFromEntry(entry);
                toEvict[numToEvict++] = object->m_underlying;
                object->m_residencyStatus = ManagedObject::EVICTED;
                RemoveEntry(entry);
                InsertTail(&this->m_lru.m_evicted, entry);
                this->m_lru.m_numResident--;
                this->m_lru.m_residentSize -= object->m_size;
                this->m_lru.m_numEvicted++;
                needed -= static_cast<int64_t>(object->m_size);
            }
            this->m_device->Evict(numToEvict, toEvict);
        }
    }
    delete[] reinterpret_cast<ManagedObject**>(toMakeResident);
    delete[] toEvict;
    LeaveCriticalSection(&this->m_mutex);
    if (this->m_device3 == nullptr) {
        this->m_asyncFence->Signal(work->m_fenceValueToSignal);
    }
    ResidencySet* set = work->m_masterSet;
    work->m_masterSet = nullptr;
    DeleteSet(set);
}

// The paging thread, when the OS has no queued MakeResident.
// ref: FUN_1422cd580
DWORD WINAPI ResidencyManager::AsyncThreadStart(void* param) {
    auto manager = static_cast<ResidencyManager*>(param);
    while (true) {
        if (manager->m_readIndex != manager->m_writeIndex) {
            AsyncWorkload* work = &manager->m_queue[manager->m_readIndex % manager->m_queueSize];
            manager->m_readIndex++;
            while (work != nullptr) {
                manager->ProcessPagingWork(work);
                if (!SetEvent(manager->m_asyncCompletionEvent)) {
                    GetLastError();
                }
                if (manager->m_readIndex == manager->m_writeIndex) {
                    break;
                }
                work = &manager->m_queue[manager->m_readIndex % manager->m_queueSize];
                manager->m_readIndex++;
            }
        }
        WaitForSingleObject(manager->m_asyncWorkEvent, INFINITE);
        if (!ResetEvent(manager->m_asyncWorkEvent)) {
            GetLastError();
        }
        if (manager->m_finishAsyncWork != 0) {
            return 0;
        }
    }
}

}
