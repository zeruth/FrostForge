#ifndef GX_D3D12_RESIDENCY_HPP
#define GX_D3D12_RESIDENCY_HPP

// D3DX12Residency (Engine/Source/Gx/src/D3d12/d3dx12Residency.h): Microsoft's residency
// manager sample, compiled into the client with its allocations routed through Storm. The
// names are the sample's; the layout and behaviour are the binary's.

#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

// The D3D12-on-Windows-7 device interface the manager falls back to for memory budgets.
MIDL_INTERFACE("74eaee3f-2f4b-476d-82ba-2b85cb49e310")
ID3D12DeviceDownlevel : public IUnknown {
    public:
    virtual HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT nodeIndex, DXGI_MEMORY_SEGMENT_GROUP group,
        DXGI_QUERY_VIDEO_MEMORY_INFO* info) = 0;
};

namespace D3DX12Residency {

// An object the manager tracks (0x40 bytes): made resident before a command list that uses
// it runs, evicted when unused for long enough and memory is short.
struct ManagedObject {
    enum : uint32_t {
        RESIDENT = 0,
        EVICTED = 1,
    };

    uint32_t m_residencyStatus;     // 0x00
    ID3D12Pageable* m_underlying;   // 0x08
    uint64_t m_size;                // 0x10
    uint64_t m_lastGpuSyncPoint;    // 0x18
    uint64_t m_lastUsedTimestamp;   // 0x20
    uint64_t m_commandListsUsedOn;  // 0x28: one bit per open residency set
    LIST_ENTRY m_listEntry;         // 0x30: in the manager's resident or evicted list
};
static_assert(sizeof(ManagedObject) == 0x40, "ManagedObject is 0x40 bytes");

// The objects one command list uses (0x28 bytes).
struct ResidencySet {
    uint32_t m_index;               // 0x00: the bit it owns in each object's mask, -1 when closed
    ManagedObject** m_set;          // 0x08
    int32_t m_maxSize;              // 0x10
    int32_t m_count;                // 0x14
    uint8_t m_isOpen;               // 0x18
    uint8_t m_outOfMemory;          // 0x19
    uint8_t m_pad1a[6];
    CRITICAL_SECTION* m_mutex;      // 0x20: the manager's set lock; the index mask follows it

    HRESULT Open();                         // FUN_1422dc670
    HRESULT Close();                        // FUN_1422ce1a0
    bool Insert(ManagedObject* object);     // FUN_1422db640
};
static_assert(sizeof(ResidencySet) == 0x28, "ResidencySet is 0x28 bytes");

// A fence per queue that the manager signals after each submission (0x20 bytes).
struct QueueSyncPoint {
    ID3D12Fence* m_fence;           // 0x00
    uint64_t m_value;               // 0x08: the next value to signal
    LIST_ENTRY m_listEntry;         // 0x10
};

// One entry of the async work ring (0x18 bytes).
struct AsyncWorkload {
    uint64_t m_syncGeneration;      // 0x00
    ResidencySet* m_masterSet;      // 0x08
    uint64_t m_fenceValueToSignal;  // 0x10
};

// The objects resident and evicted, oldest use first (0x30 bytes).
struct LRUCache {
    LIST_ENTRY m_resident;          // 0x00
    LIST_ENTRY m_evicted;           // 0x10
    uint32_t m_numResident;         // 0x20
    uint32_t m_numEvicted;          // 0x24
    uint64_t m_residentSize;        // 0x28
};

// The manager (0x1d0 bytes at CGxDeviceD3d12+0x658).
class ResidencyManager {
    public:
    uint64_t m_queueSize;           // 0x000
    AsyncWorkload* m_queue;         // 0x008: new[]'d, MaxLatency + 1 entries
    HANDLE m_asyncWorkEvent;        // 0x010
    HANDLE m_asyncThread;           // 0x018
    CRITICAL_SECTION m_syncCS;      // 0x020: guards the sync point list
    uint8_t m_finishAsyncWork;      // 0x048
    uint8_t m_pad49[7];
    uint64_t m_readIndex;           // 0x050
    uint64_t m_writeIndex;          // 0x058
    LIST_ENTRY m_queueSyncPoints;   // 0x060
    volatile LONG m_numQueueSyncPoints;  // 0x070
    uint32_t m_pad74;
    ID3D12Fence* m_asyncFence;      // 0x078
    uint64_t m_asyncFenceValue;     // 0x080
    LIST_ENTRY m_list88;            // 0x088
    LIST_ENTRY m_syncPoints;        // 0x098: the generations still in flight
    uint64_t m_syncGeneration;      // 0x0a8
    HANDLE m_completionEvent;       // 0x0b0
    HANDLE m_asyncCompletionEvent;  // 0x0b8
    ID3D12Device* m_device;         // 0x0c0
    ID3D12Device3* m_device3;       // 0x0c8: EnqueueMakeResident, when the OS has it
    ID3D12DeviceDownlevel* m_deviceDownlevel;  // 0x0d0
    uint32_t m_nodeIndex;           // 0x0d8
    uint32_t m_paddc;
    IDXGIAdapter3* m_adapter;       // 0x0e0
    LRUCache m_lru;                 // 0x0e8
    CRITICAL_SECTION m_mutex;       // 0x118
    CRITICAL_SECTION m_executionCS; // 0x140
    uint8_t m_startEvicted;         // 0x168
    uint8_t m_pad169[3];
    float m_minEvictionGracePeriod; // 0x16c: seconds
    uint64_t m_minEvictionGracePeriodTicks;  // 0x170
    float m_maxEvictionGracePeriod; // 0x178: seconds
    uint32_t m_pad17c;
    uint64_t m_maxEvictionGracePeriodTicks;  // 0x180
    float m_trimRatio;              // 0x188
    uint32_t m_maxSoftwareQueueLatency;  // 0x18c
    uint64_t m_id;                  // 0x190: tells this manager's queue data from another's
    CRITICAL_SECTION* m_setMutex;   // 0x198: points at m_setLock
    CRITICAL_SECTION m_setLock;     // 0x1a0
    uint64_t m_setIndices;          // 0x1c8: the residency set bits in use

    // The constructor's part of CGxDeviceD3d12::CGxDeviceD3d12 (FUN_1422cacf0).
    void Construct();

    HRESULT Initialize(ID3D12Device* device, uint32_t nodeIndex, IDXGIAdapter3* adapter, uint32_t maxLatency);  // FUN_1422db330
    void Destroy();                                                                         // FUN_1422d0d50
    void BeginTrackingObject(ManagedObject* object);                                        // FUN_1422cd650
    void EndTrackingObject(ManagedObject* object);                                          // FUN_1422c2c80
    HRESULT ExecuteCommandLists(ID3D12CommandQueue* queue, ID3D12CommandList** lists, ResidencySet** sets,
        uint32_t count);                                                                    // FUN_1422d21e0

    private:
    HRESULT PrepareMasterSet(ResidencySet** sets, uint32_t count, ResidencySet** master);  // FUN_1422d3850
    HRESULT ExecuteMasterSet(ID3D12CommandQueue* queue, ResidencySet** master, void* execution);  // FUN_1422c7430
    HRESULT GetQueueSyncPoint(ID3D12CommandQueue* queue, QueueSyncPoint** syncPoint);       // FUN_1422d36d0
    HRESULT EnqueueSyncPoint(ID3D12CommandQueue* queue, QueueSyncPoint* syncPoint);          // FUN_1422e0200
    void* DequeueCompletedSyncPoints();                                                     // FUN_1422d0c80
    void QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO* info);
    void ProcessPagingWork(AsyncWorkload* work);                                            // FUN_1422dcbb0

    public:
    static DWORD WINAPI AsyncThreadStart(void* manager);                                    // FUN_1422cd580
};
static_assert(offsetof(ResidencyManager, m_syncCS) == 0x20, "ResidencyManager layout");
static_assert(offsetof(ResidencyManager, m_asyncFence) == 0x78, "ResidencyManager layout");
static_assert(offsetof(ResidencyManager, m_device) == 0xc0, "ResidencyManager layout");
static_assert(offsetof(ResidencyManager, m_lru) == 0xe8, "ResidencyManager layout");
static_assert(offsetof(ResidencyManager, m_mutex) == 0x118, "ResidencyManager layout");
static_assert(offsetof(ResidencyManager, m_startEvicted) == 0x168, "ResidencyManager layout");
static_assert(offsetof(ResidencyManager, m_maxSoftwareQueueLatency) == 0x18c, "ResidencyManager layout");
static_assert(offsetof(ResidencyManager, m_setLock) == 0x1a0, "ResidencyManager layout");
static_assert(sizeof(ResidencyManager) == 0x1d0, "ResidencyManager is 0x1d0 bytes");

}

#endif
