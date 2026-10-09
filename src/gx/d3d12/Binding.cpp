// The D3D12 binding model: descriptors wait in per-stage CPU staging heaps at fixed slots and
// are copied into the shader-visible ring as root tables when a draw reads them; constant
// buffers go straight to root descriptors.

#include <cstring>

#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/d3d12/Pipeline.hpp"
#include "gx/GxPlatform.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

CGxDeviceD3d12* GxDeviceD3d12();

namespace gxDx12Backend {

const uint32_t s_bindingCounts[BINDING_STAGES][4] = {
    { 6, 64, 0, 16 },
    { 0, 0, 0, 0 },
    { 10, 64, 8, 16 },
    { 3, 64, 8, 16 },
    { 0, 0, 1, 0 },
};
const int32_t s_bindingHeapTypes[4] = {
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
};
const uint32_t s_bindingTableKinds[BINDING_STAGES] = { 10, 0, 14, 14, 0 };
const uint32_t s_bindingRootParameters[4][4] = {
    { 12, 19, 0, 20 },
    { 21, 0, 0, 0 },
    { 2, 0, 18, 1 },
    { 0, 3, 5, 4 },
};
uint32_t g_stagingCounts[4];
uint32_t g_stagingBases[BINDING_STAGES][4];

namespace {

const char s_deviceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\D3d12\\GxDeviceD3d12.cpp";

// Each stage's kinds are laid out one after another: constants, shader resources and
// unordered views in the view heap, samplers in theirs.
// ref: FUN_14020b870 (a static initializer)
struct StagingLayout {
    StagingLayout() {
        for (uint32_t stage = 0; stage < BINDING_STAGES; stage++) {
            g_stagingBases[stage][BINDING_CBV] = g_stagingCounts[0];
            g_stagingCounts[0] += s_bindingCounts[stage][BINDING_CBV];
            g_stagingBases[stage][BINDING_SRV] = g_stagingCounts[0];
            g_stagingCounts[0] += s_bindingCounts[stage][BINDING_SRV];
            g_stagingBases[stage][BINDING_UAV] = g_stagingCounts[0];
            g_stagingCounts[0] += s_bindingCounts[stage][BINDING_UAV];
            g_stagingBases[stage][BINDING_SAMPLER] = g_stagingCounts[1];
            g_stagingCounts[1] += s_bindingCounts[stage][BINDING_SAMPLER];
        }
    }
} s_stagingLayout;

uint32_t HighestBit(uint64_t mask) {
    uint32_t bit = 63;
    while ((mask >> bit) == 0) {
        bit--;
    }
    return bit;
}

uint32_t LowestBit(uint64_t mask) {
    uint32_t bit = 0;
    while (((mask >> bit) & 1) == 0) {
        bit++;
    }
    return bit;
}

// Copies the runs of set bits in `mask` from the staging slots at `source` into the table at
// `dest`, in one CopyDescriptors.
// ref: FUN_1422ceee0
void CopyDescriptorRuns(ID3D12Device* device, uint64_t mask, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE source,
    D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t increment) {
    if (mask == 0) {
        return;
    }
    UINT sizes[32];
    D3D12_CPU_DESCRIPTOR_HANDLE sources[32];
    D3D12_CPU_DESCRIPTOR_HANDLE dests[32];
    uint32_t count = 0;
    if (mask == ~0ull) {
        sizes[0] = 0x40;
        sources[0] = source;
        dests[0] = dest;
        count = 1;
    } else {
        uint32_t position = 0;
        while (mask != 0 && count < 32) {
            uint32_t skip = LowestBit(mask);
            position += skip;
            mask >>= skip;
            uint64_t inverted = ~mask;
            uint32_t run = inverted == 0 ? 64 : LowestBit(inverted);
            uint32_t offset = position * increment;
            sizes[count] = run;
            sources[count].ptr = source.ptr + offset;
            dests[count].ptr = dest.ptr + offset;
            count++;
            position += run;
            mask = run >= 64 ? 0 : mask >> run;
        }
    }
    device->CopyDescriptors(count, dests, sizes, count, sources, sizes, type);
}

}

// The CPU heaps each slot's descriptor is kept in, and the constant buffer addresses.
// ref: FUN_1422db0e0
HRESULT DescriptorStagingHeap::Initialize(ID3D12Device* device) {
    std::memset(this->m_bound, 0, sizeof(this->m_bound));
    std::memset(this->m_dirty, 0, sizeof(this->m_dirty));
    std::memset(this->m_uploaded, 0xff, sizeof(this->m_uploaded));
    this->m_device = device;
    for (uint32_t type = 0; type < 4; type++) {
        if (g_stagingCounts[type] == 0) {
            continue;
        }
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type);
        desc.NumDescriptors = g_stagingCounts[type];
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ID3D12DescriptorHeap* heap = nullptr;
        HRESULT hr = device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
        if (FAILED(hr)) {
            GxDeviceD3d12()->Fail(hr, "DescriptorStagingHeap Initialize CreateDescriptorHeap failed");
            if (heap != nullptr) {
                heap->Release();
            }
            return hr;
        }
        this->m_cpu[type] = heap->GetCPUDescriptorHandleForHeapStart();
        this->m_increments[type] = device->GetDescriptorHandleIncrementSize(desc.Type);
        ID3D12DescriptorHeap* previous = this->m_heaps[type];
        this->m_heaps[type] = heap;
        if (previous != nullptr) {
            previous->Release();
        }
    }
    if (g_stagingCounts[0] != 0) {
        auto constants = static_cast<uint64_t*>(SMemAlloc(static_cast<uint64_t>(g_stagingCounts[0]) * 8, 0xd, s_deviceFile,
            SStrHashFnv1a(s_deviceFile), 0x2234, 0));
        uint64_t* previous = this->m_constants;
        this->m_constants = constants;
        if (previous != nullptr) {
            SMemFree(previous, s_deviceFile, 0, 0);
        }
        if (this->m_constants == nullptr) {
            return E_OUTOFMEMORY;
        }
        std::memset(this->m_constants, 0, static_cast<uint64_t>(g_stagingCounts[0]) * 8);
    }
    return S_OK;
}

// A descriptor copied into a stage's slot (none clears it); a constant buffer view keeps its
// address for the root descriptor.
// ref: FUN_1422df5b0
void DescriptorStagingHeap::SetView(uint8_t stage, uint32_t kind, uint32_t slot, D3D12_CPU_DESCRIPTOR_HANDLE source, uint64_t address) {
    if (stage >= BINDING_STAGES || kind >= 4 || slot >= s_bindingCounts[stage][kind]) {
        return;
    }
    uint64_t bit = 1ull << (slot & 0x3f);
    if (source.ptr == 0) {
        this->m_bound[stage][kind] &= ~bit;
        return;
    }
    int32_t type = s_bindingHeapTypes[kind];
    uint32_t index = g_stagingBases[stage][kind] + slot;
    D3D12_CPU_DESCRIPTOR_HANDLE dest = { this->m_cpu[type].ptr + static_cast<uint64_t>(index * this->m_increments[type]) };
    this->m_device->CopyDescriptorsSimple(1, dest, source, static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
    if (type == 0) {
        this->m_constants[index] = address;
    }
    this->m_bound[stage][kind] |= bit;
    this->m_dirty[stage][kind] |= bit;
}

// A shader resource view made straight into a stage's slot.
// ref: FUN_1422dfa10
void DescriptorStagingHeap::CreateShaderResourceView(uint8_t stage, uint32_t slot, ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc) {
    if (stage >= BINDING_STAGES || slot >= s_bindingCounts[stage][BINDING_SRV]) {
        return;
    }
    uint32_t index = g_stagingBases[stage][BINDING_SRV] + slot;
    uint64_t bit = 1ull << (slot & 0x3f);
    D3D12_CPU_DESCRIPTOR_HANDLE dest = { this->m_cpu[0].ptr + static_cast<uint64_t>(index * this->m_increments[0]) };
    this->m_device->CreateShaderResourceView(resource, desc, dest);
    this->m_constants[index] = 0;
    this->m_bound[stage][BINDING_SRV] |= bit;
    this->m_dirty[stage][BINDING_SRV] |= bit;
}

// A constant buffer's address for a stage's slot (0 unbinds it).
// ref: FUN_1422df440
void DescriptorStagingHeap::SetConstants(uint8_t stage, uint32_t slot, uint64_t address) {
    if (stage >= BINDING_STAGES || slot >= s_bindingCounts[stage][BINDING_CBV]) {
        return;
    }
    uint64_t bit = 1ull << (slot & 0x3f);
    if (address == 0) {
        this->m_bound[stage][BINDING_CBV] &= ~bit;
        return;
    }
    this->m_constants[g_stagingBases[stage][BINDING_CBV] + slot] = address;
    this->m_bound[stage][BINDING_CBV] |= bit;
    this->m_dirty[stage][BINDING_CBV] |= bit;
}

// The vertex and pixel stages' tables: a table moves to a fresh place in the ring when its
// heap's page is new or the pipeline reads a slot changed since it was uploaded, and only the
// slots the pipeline reads are copied.
// ref: FUN_1422e5340
void DescriptorStagingHeap::UploadTables(ID3D12GraphicsCommandList* list, const void* pipelineHandle, uint64_t* ring, uint32_t fresh) {
    auto pipeline = static_cast<const GxPipeline*>(pipelineHandle);
    if (pipeline == nullptr) {
        return;
    }
    static const uint8_t s_stages[2] = { 0, 2 };
    for (uint8_t stage : s_stages) {
        const PIPELINESTAGE& reads = pipeline->m_stages[stage];
        const uint64_t needs[4] = { reads.m_constants, reads.m_srvs, reads.m_uavs, reads.m_samplers };
        uint32_t kinds = s_bindingTableKinds[stage];
        while (kinds != 0) {
            uint32_t kind = LowestBit(kinds);
            kinds &= kinds - 1;
            int32_t type = s_bindingHeapTypes[kind];
            uint64_t need = needs[kind];
            uint64_t bound = this->m_bound[stage][kind];
            if (bound == 0) {
                continue;
            }
            uint32_t count = HighestBit(bound) + 1;
            uint64_t uploaded = this->m_uploaded[stage][kind];
            uint64_t dirty = this->m_dirty[stage][kind];
            D3D12_CPU_DESCRIPTOR_HANDLE table = this->m_tables[stage][kind];
            if (((fresh >> (type & 0x1f)) & 1) != 0 || (need & uploaded & dirty) != 0) {
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = { ring[4 + type] };
                list->SetGraphicsRootDescriptorTable(s_bindingRootParameters[stage][kind], gpu);
                table.ptr = ring[type];
                this->m_tables[stage][kind] = table;
                uint64_t size = static_cast<uint64_t>(count * this->m_increments[type]);
                ring[type] += size;
                ring[4 + type] += size;
                dirty = bound;
                uploaded = count < 0x40 ? ~0ull << count : 0;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE source = {};
            if (stage < BINDING_STAGES && kind < 4 && s_bindingCounts[stage][kind] != 0) {
                source.ptr = this->m_cpu[s_bindingHeapTypes[kind]].ptr +
                    static_cast<uint64_t>(this->m_increments[s_bindingHeapTypes[kind]] * g_stagingBases[stage][kind]);
            }
            CopyDescriptorRuns(this->m_device, dirty & need, table, source, static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type),
                this->m_increments[type]);
            this->m_uploaded[stage][kind] = uploaded | need;
            this->m_dirty[stage][kind] = ~need & dirty;
        }
    }
}

}
