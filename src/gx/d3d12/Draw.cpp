// The D3D12 draw path: the descriptor ring, the state a draw commits to the list, and the
// render context's pipeline, stream and draw slots.

#include <cstring>

#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/d3d12/Pipeline.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxVertexFormat.hpp"
#include "util/Unimplemented.hpp"

CGxDeviceD3d12* GxDeviceD3d12();

namespace {

// Primitives per vertex for each topology and the vertices a strip starts with
// (DAT_142d43890, DAT_142d438a8).
const uint32_t s_verticesPerPrimitive[6] = { 1, 2, 1, 3, 1, 0 };
const int32_t s_primitiveOffsets[6] = { 0, 0, 1, 0, 2, -1 };

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

}

// The primitives `count` vertices make in a topology.
// ref: FUN_142291b30
uint32_t GxPrimitiveCount(uint8_t topology, uint32_t count) {
    if (s_verticesPerPrimitive[topology] == 1) {
        return count - s_primitiveOffsets[topology];
    }
    return count / s_verticesPerPrimitive[topology] - s_primitiveOffsets[topology];
}

namespace gxDx12Backend {

// ref: FUN_1422d9540
DescriptorUploadPage* GraphicsCommandList::AllocateDescriptors(int32_t type, uint32_t count, bool keep) {
    CGxDeviceD3d12* device = GxDeviceD3d12();
    if (device->m_ready == 0) {
        return nullptr;
    }
    PerFrameResources* frame = this->m_copy.m_frame;
    DescriptorUploadPage* page = frame->m_currentPages[type];
    if (page == nullptr || page->m_capacity < count * page->m_increment + page->m_cursor) {
        if (keep) {
            return nullptr;
        }
        page = nullptr;
        uint32_t descriptors = 0x800;
        blz::vector<DescriptorUploadPage, s_uploadPageTag>* pages = &frame->m_uploadPages[type];
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER || count < 0x401) {
            g_gxDeviceLock.Enter();
            auto& pool = device->m_freeUploadPages[type];
            if (pool.size() != 0) {
                DescriptorUploadPage reused = pool.back();
                pool.m_size--;
                page = &pages->emplace_back();
                *page = reused;
            }
            g_gxDeviceLock.Leave();
        } else {
            descriptors = count + 0x800;
            pages = &frame->m_uploadPages[4];
        }
        if (page == nullptr) {
            page = &pages->emplace_back();
            std::memset(page, 0, sizeof(DescriptorUploadPage));
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type);
            desc.NumDescriptors = descriptors;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            HRESULT hr = device->m_device->CreateDescriptorHeap(&desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&page->m_heap));
            if (FAILED(hr)) {
                device->Fail(hr, "CreateDescriptorHeap failed");
                return nullptr;
            }
            page->m_type = static_cast<uint32_t>(type);
            page->m_cpu = page->m_heap->GetCPUDescriptorHandleForHeapStart();
            page->m_gpu = page->m_heap->GetGPUDescriptorHandleForHeapStart();
            page->m_increment = device->m_device->GetDescriptorHandleIncrementSize(desc.Type);
            page->m_capacity = page->m_increment * descriptors;
        }
        page->m_cursor = 0;
        frame->m_currentPages[type] = page;
        this->m_heapsDirty = 1;
    }
    page->m_begin = page->m_cursor;
    page->m_cursor += count * page->m_increment;
    return page;
}

// ref: FUN_1422d5460
void GraphicsCommandList::CommitBindings() {
    GxPipeline* pipeline = this->m_pipeline;
    uint32_t totals[4] = {};
    uint32_t changes[4] = {};
    uint32_t flags = 0;
    bool needed = false;
    if (pipeline != nullptr) {
        uint64_t dirtyNeeded = 0;
        static const uint8_t s_stages[2] = { 0, 2 };
        for (uint8_t stage : s_stages) {
            const PIPELINESTAGE& reads = pipeline->m_stages[stage];
            const uint64_t needs[4] = { reads.m_constants, reads.m_srvs, reads.m_uavs, reads.m_samplers };
            uint32_t kinds = s_bindingTableKinds[stage];
            while (kinds != 0) {
                uint32_t kind = LowestBit(kinds);
                kinds &= kinds - 1;
                uint64_t bound = this->m_staging.m_bound[stage][kind];
                if (bound == 0) {
                    continue;
                }
                uint64_t dirty = this->m_staging.m_dirty[stage][kind];
                int32_t type = s_bindingHeapTypes[kind];
                uint32_t count = HighestBit(bound) + 1;
                dirtyNeeded |= dirty & needs[kind];
                if ((needs[kind] & this->m_staging.m_uploaded[stage][kind] & dirty) != 0) {
                    changes[type] += count;
                }
                totals[type] += count;
            }
        }
        flags = pipeline->m_stages[2].m_flags;
        needed = dirtyNeeded != 0;
    }
    if ((flags & 0x1f) != 0) {
        // The pipeline's extra tables (the five at +0x1c00 with their cached addresses at
        // +0x1ca0, FUN_1422e5610) are not ported; no glue shader is known to ask for them.
        FROST_UNIMPLEMENTED();
    }
    ID3D12GraphicsCommandList* list = this->m_copy.m_list;
    if (needed) {
        uint64_t ring[8] = {};
        ID3D12DescriptorHeap* heaps[2] = {};
        uint32_t heapCount = 0;
        uint32_t fresh = 0;
        PerFrameResources* frame = this->m_copy.m_frame;
        for (int32_t type = 0; type < 2; type++) {
            DescriptorUploadPage* page = frame->m_currentPages[type];
            bool reallocate = page == nullptr || page != this->m_ringPages[type] || GxDeviceD3d12()->m_ready == 0;
            if (!reallocate) {
                uint32_t end = changes[type] * page->m_increment + page->m_cursor;
                if (page->m_capacity < end) {
                    reallocate = true;
                } else {
                    page->m_begin = page->m_cursor;
                    page->m_cursor = end;
                }
            }
            if (reallocate) {
                page = this->AllocateDescriptors(type, totals[type], false);
                if (page == nullptr) {
                    GxDeviceD3d12()->m_ready = 0;
                    return;
                }
                fresh |= 1u << type;
                this->m_ringPages[type] = page;
            }
            ring[type] = page->m_cpu.ptr + page->m_begin;
            ring[type + 4] = page->m_gpu.ptr + page->m_begin;
            heaps[heapCount++] = page->m_heap;
        }
        if (this->m_heapsDirty) {
            list->SetDescriptorHeaps(heapCount, heaps);
            this->m_heapsDirty = 0;
        }
        this->m_staging.UploadTables(list, pipeline, ring, fresh);
    }
    pipeline = this->m_pipeline;
    if (pipeline == nullptr) {
        return;
    }
    for (uint8_t stage = 0; stage < 3; stage++) {
        uint64_t need = pipeline->m_stages[stage].m_constants;
        uint64_t dirty = this->m_staging.m_dirty[stage][BINDING_CBV];
        uint64_t bits = dirty & need;
        uint32_t base = g_stagingBases[stage][BINDING_CBV];
        while (bits != 0) {
            uint32_t slot = LowestBit(bits);
            bits &= bits - 1;
            list->SetGraphicsRootConstantBufferView(s_bindingRootParameters[stage][BINDING_CBV] + slot,
                this->m_staging.m_constants[base + slot]);
        }
        this->m_staging.m_dirty[stage][BINDING_CBV] = ~need & dirty;
    }
}

// ref: FUN_1422d9140
void GraphicsCommandList::CommitState(bool indexed, uint8_t shadingRate) {
    ID3D12GraphicsCommandList* list = this->m_copy.m_list;
    if (this->m_targetsDirty) {
        list->OMSetRenderTargets(this->m_rtvCount, this->m_rtvs, FALSE, this->m_dsvPtr);
        this->m_targetsDirty = 0;
    }
    if (this->m_pipelineDirty) {
        list->SetPipelineState(this->m_pipeline->m_pso);
        this->m_pipelineDirty = 0;
    }
    if (this->m_stencilRef != this->m_pipeline->m_desc.m_13) {
        list->OMSetStencilRef(this->m_pipeline->m_desc.m_13);
        this->m_stencilRef = this->m_pipeline->m_desc.m_13;
    }
    if (this->m_topologyDirty) {
        list->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(this->m_topology));
        this->m_topologyDirty = 0;
    }
    RenderContextD3d12& context = this->m_context;
    // The context keeps its target size as {height, width} at +0x10.
    float height = context.Field<float>(0x10);
    float width = context.Field<float>(0x14);
    if (context.Field<uint8_t>(0x18)) {
        context.Field<uint8_t>(0x18) = 0;
        D3D12_VIEWPORT viewport;
        float left = static_cast<float>(static_cast<int32_t>(context.Field<float>(0x1c) * width + 0.5f));
        float top = static_cast<float>(static_cast<int32_t>((1.0f - context.Field<float>(0x28)) * height + 0.5f));
        viewport.TopLeftX = left;
        viewport.TopLeftY = top;
        viewport.Width = static_cast<float>(static_cast<int32_t>(context.Field<float>(0x20) * width + 0.5f)) - left;
        viewport.Height = static_cast<float>(static_cast<int32_t>((1.0f - context.Field<float>(0x24)) * height + 0.5f)) - top;
        viewport.MinDepth = context.Field<float>(0x2c);
        viewport.MaxDepth = context.Field<float>(0x30);
        list->RSSetViewports(1, &viewport);
    }
    if (context.Field<uint8_t>(0x74)) {
        context.Field<uint8_t>(0x74) = 0;
        int32_t left = static_cast<int32_t>(context.Field<float>(0x7c) * width + 0.5f);
        if (left < 0) {
            left = 0;
        }
        int32_t right = static_cast<int32_t>(context.Field<float>(0x84) * width + 1.0f);
        int32_t top = static_cast<int32_t>((1.0f - context.Field<float>(0x80)) * height + 0.5f);
        if (top < 0) {
            top = 0;
        }
        int32_t bottom = static_cast<int32_t>((1.0f - context.Field<float>(0x78)) * height + 1.0f);
        if (static_cast<int32_t>(width) < right) {
            right = static_cast<int32_t>(width);
        }
        if (static_cast<int32_t>(height) < bottom) {
            bottom = static_cast<int32_t>(height);
        }
        if (right - left < 0) {
            right = left;
        }
        if (bottom - top < 0) {
            bottom = top;
        }
        D3D12_RECT scissor = { left, top, right, bottom };
        list->RSSetScissorRects(1, &scissor);
    }
    GxBuf* indices = this->m_indexBuf;
    if (indexed && indices != nullptr && this->m_boundIndexAddress != indices->m_gpu) {
        D3D12_INDEX_BUFFER_VIEW view;
        view.BufferLocation = indices->m_gpu;
        view.SizeInBytes = indices->m_size;
        view.Format = static_cast<DXGI_FORMAT>(this->m_indexFormat);
        list->IASetIndexBuffer(&view);
        this->m_boundIndexAddress = indices->m_gpu;
        if (indices->m_heap != nullptr) {
            // A static buffer's heap joins the residency set (FUN_1422db640); the buffer
            // heaps are not ported.
            FROST_UNIMPLEMENTED();
        }
    }
    this->CommitBindings();
    this->m_copy.FlushBarriers();
    if (this->m_shadingRate != shadingRate || this->m_combiners[0] != this->m_wantCombiners[0] ||
        this->m_combiners[1] != this->m_wantCombiners[1]) {
        D3D12_SHADING_RATE_COMBINER combiners[2] = {
            static_cast<D3D12_SHADING_RATE_COMBINER>(this->m_wantCombiners[0]),
            static_cast<D3D12_SHADING_RATE_COMBINER>(this->m_wantCombiners[1]),
        };
        if (this->m_copy.m_list5 != nullptr) {
            this->m_copy.m_list5->RSSetShadingRate(static_cast<D3D12_SHADING_RATE>(shadingRate), combiners);
        }
        this->m_combiners[0] = this->m_wantCombiners[0];
        this->m_combiners[1] = this->m_wantCombiners[1];
        this->m_shadingRate = shadingRate;
    }
}

// The barriers still go; the context counts the draws it skipped (+0xf0).
void GraphicsCommandList::SkipDraw() {
    this->m_copy.FlushBarriers();
    this->m_context.Field<uint32_t>(0xf0)++;
}

}

using namespace gxDx12Backend;

// ref: FUN_1422dc9e0
void RenderContextD3d12::SetPipeline(GxPipeline* pipeline) {
    GraphicsCommandList* owner = this->Owner();
    if (pipeline == owner->m_pipeline || pipeline == nullptr) {
        return;
    }
    owner->m_pipeline = pipeline;
    owner->m_pipelineDirty = 1;
    uint32_t topology = static_cast<uint32_t>(s_gxTopologies[pipeline->m_desc.m_topology]);
    if (owner->m_topology != topology) {
        owner->m_topology = topology;
        owner->m_topologyDirty = 1;
    }
}

// ref: FUN_1422dfe60
void RenderContextD3d12::SetVertexBuffers(uint32_t first, uint32_t count, const GXVERTEXSTREAM* streams) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return;
    }
    D3D12_VERTEX_BUFFER_VIEW views[16];
    for (uint32_t i = 0; i < count && i < 16; i++) {
        const GXVERTEXSTREAM& stream = streams[i];
        if (stream.m_buf->m_heap != nullptr) {
            // A static buffer's heap joins the residency set (FUN_1422db640); the buffer heaps
            // are not ported.
            FROST_UNIMPLEMENTED();
        }
        views[i].BufferLocation = stream.m_buf->m_gpu + stream.m_offset;
        views[i].SizeInBytes = stream.m_size;
        views[i].StrideInBytes = stream.m_layout->m_desc->m_stride;
    }
    this->Owner()->m_copy.m_list->IASetVertexBuffers(first, count, views);
}

// ref: FUN_1422dca20
void RenderContextD3d12::SetIndexBuffer(GxBuf* buf, uint8_t format) {
    GraphicsCommandList* owner = this->Owner();
    owner->m_indexBuf = buf;
    owner->m_indexFormat = format == 1 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
}

// ref: FUN_1422d1120
void RenderContextD3d12::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance,
    uint8_t shadingRate) {
    GraphicsCommandList* owner = this->Owner();
    GxPipeline* pipeline = owner->m_pipeline;
    if (GxDeviceD3d12()->m_ready == 0 || pipeline == nullptr || pipeline->m_pso == nullptr) {
        owner->SkipDraw();
        return;
    }
    owner->CommitState(false, shadingRate);
    uint32_t primitives = GxPrimitiveCount(pipeline->m_desc.m_topology, vertexCount);
    owner->m_copy.m_empty = 0;
    owner->m_copy.m_18 += primitives * instanceCount;
    owner->m_copy.m_list->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
}

// ref: FUN_1422d11f0
void RenderContextD3d12::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, int32_t baseVertex,
    uint32_t startInstance, uint8_t shadingRate) {
    GraphicsCommandList* owner = this->Owner();
    GxPipeline* pipeline = owner->m_pipeline;
    if (GxDeviceD3d12()->m_ready == 0 || pipeline == nullptr || pipeline->m_pso == nullptr) {
        owner->SkipDraw();
        return;
    }
    owner->CommitState(true, shadingRate);
    uint32_t primitives = GxPrimitiveCount(pipeline->m_desc.m_topology, indexCount);
    owner->m_copy.m_empty = 0;
    owner->m_copy.m_18 += primitives * instanceCount;
    owner->m_copy.m_list->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
}
