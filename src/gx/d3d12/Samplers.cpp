// Texture binding on the D3D12 graphics list, and the sampler descriptors textures ask for.

#include <cstring>

#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/GxPlatform.hpp"

CGxDeviceD3d12* GxDeviceD3d12();

namespace {

// The residency object a texture is paged by: its own when committed, its heap page's when
// placed (FUN_1422d9100's test).
D3DX12Residency::ManagedObject* TexResidency(gxDx12Backend::Tex* tex) {
    if (tex->m_residency.m_underlying != nullptr) {
        return &tex->m_residency;
    }
    if (tex->m_heap != nullptr) {
        return &tex->m_heap->m_residency;
    }
    return nullptr;
}

// A sampler key's filter, its comparison form, address modes and border colours
// (DAT_14291c190, DAT_14291c1a8, DAT_14291c1c0, DAT_14291c1e0).
const D3D12_FILTER s_filters[6] = {
    D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_POINT,
    D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_FILTER_ANISOTROPIC,
};
const D3D12_FILTER s_comparisonFilters[6] = {
    D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
    D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
    D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, D3D12_FILTER_COMPARISON_ANISOTROPIC,
};
const D3D12_TEXTURE_ADDRESS_MODE s_addressModes[8] = {
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
    D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
    static_cast<D3D12_TEXTURE_ADDRESS_MODE>(0), static_cast<D3D12_TEXTURE_ADDRESS_MODE>(0),
};
const float s_borderColors[6][4] = {
    { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f },
    { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 0.0f, 1.0f },
};

// The sampler key a texture's usage describes: filter and three address modes in the low
// bytes, then comparison, anisotropy and the LOD range.
uint64_t SamplerKey(const gxDx12Backend::Tex* tex) {
    uint64_t usage = static_cast<uint64_t>(tex->m_usage) | (static_cast<uint64_t>(tex->m_usage2) << 32);
    uint16_t low = static_cast<uint16_t>(((usage >> 3) & 1) | 0x8002);
    uint16_t high = static_cast<uint16_t>((((static_cast<uint16_t>(usage >> 13) * 2) ^ low) & 0x3e) ^ low);
    uint64_t key = (usage & 0xff) | (((usage >> 4) & 0xff) << 8) | (((usage >> 7) & 0xff) << 16) | (((usage >> 10) & 0xff) << 24) |
        (static_cast<uint64_t>(high) << 32);
    return key & 0xffff07070707ull;
}

}

// The reference keeps the descriptors in a probed map it reads without the lock and a deque
// the creating thread appends to under it; one map under the device lock does the same work.
// ref: FUN_1422c9640
D3D12_CPU_DESCRIPTOR_HANDLE CGxDeviceD3d12::SamplerFor(uint64_t key) {
    g_gxDeviceLock.Enter();
    bool inserted = false;
    void** slot = this->m_samplers.insert(key, &inserted);
    if (!inserted && *slot != nullptr) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = { reinterpret_cast<uint64_t>(*slot) };
        g_gxDeviceLock.Leave();
        return handle;
    }
    uint16_t high = static_cast<uint16_t>(key >> 32);
    D3D12_SAMPLER_DESC desc = {};
    desc.Filter = (key & 0x100000000ull) != 0 ? s_comparisonFilters[key & 0xff] : s_filters[key & 0xff];
    desc.AddressU = s_addressModes[(key >> 8) & 0xff];
    desc.AddressV = s_addressModes[(key >> 16) & 0xff];
    desc.AddressW = s_addressModes[(key >> 24) & 0xff];
    desc.MipLODBias = 0.0f;
    desc.MaxAnisotropy = (high >> 1) & 0x1f;
    desc.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>((high & 1) + 1);
    std::memcpy(desc.BorderColor, s_borderColors[(key >> 8) & 0xff], sizeof(desc.BorderColor));
    desc.MinLOD = static_cast<float>((high >> 6) & 0x1f);
    desc.MaxLOD = static_cast<float>(high >> 11);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
    if (this->m_samplerHeap.Allocate(&handle)) {
        this->m_device->CreateSampler(&desc, handle);
    }
    *slot = reinterpret_cast<void*>(handle.ptr);
    g_gxDeviceLock.Leave();
    return handle;
}

namespace gxDx12Backend {

// The texture's view goes in the stage's shader resource slot and its sampler in the sampler
// slot; nothing changes when the same texture is bound again.
// ref: FUN_1422e43f0
void GraphicsCommandList::SetTexture(uint8_t stage, uint32_t slot, Tex* tex) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return;
    }
    uint32_t id = tex != nullptr ? tex->m_id : 0;
    if (this->m_boundTextures[stage][slot] == id) {
        return;
    }
    this->m_boundTextures[stage][slot] = id;
    D3D12_CPU_DESCRIPTOR_HANDLE sampler = {};
    if (tex == nullptr) {
        this->m_views[stage][slot] = nullptr;
        if (slot < 8) {
            this->m_1ab0[stage][slot] = 0;
        }
        this->m_staging.SetView(stage, BINDING_SRV, slot, D3D12_CPU_DESCRIPTOR_HANDLE {}, 0);
    } else {
        D3DX12Residency::ManagedObject* residency = TexResidency(tex);
        if (residency != nullptr) {
            this->m_copy.ResidencySet()->Insert(residency);
        }
        uint32_t state = stage == 3 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : tex->m_srv.m_state;
        this->m_copy.TransitionObjects(tex->m_objects.data(), tex->m_objects.size(), state);
        this->m_views[stage][slot] = &tex->m_srv;
        if (slot < 8) {
            this->m_1ab0[stage][slot] = 0;
        }
        if (tex->m_srv.m_handle.ptr == 0) {
            this->m_staging.CreateShaderResourceView(stage, slot, tex->m_objects[0].m_resource,
                reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(tex->m_srv.m_desc));
        } else {
            this->m_staging.SetView(stage, BINDING_SRV, slot, tex->m_srv.m_handle, 0);
        }
        sampler = GxDeviceD3d12()->SamplerFor(SamplerKey(tex));
    }
    this->m_staging.SetView(stage, BINDING_SAMPLER, slot, sampler, 0);
}

}

// ref: FUN_1422e4620
void gxDx12Backend::RenderContextD3d12::SetTexture(uint8_t stage, uint32_t slot, Tex* tex) {
    this->Owner()->SetTexture(stage, slot, tex);
}

namespace gxDx12Backend {

// SetTexture without the sampler, and with the unordered view when asked: the texture's
// residency is noted, its objects move to the view's state, and the view lands in the slot.
// ref: FUN_1422e3d20
void GraphicsCommandList::BindTexture(uint8_t stage, uint32_t slot, Tex* tex, uint8_t unordered) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return;
    }
    uint32_t id = tex != nullptr ? tex->m_id : 0;
    if (this->m_boundTextures[stage][slot] == id) {
        return;
    }
    this->m_boundTextures[stage][slot] = id;
    if (tex == nullptr) {
        this->m_views[stage][slot] = nullptr;
        if (slot < 8) {
            this->m_1ab0[stage][slot] = 0;
        }
        this->m_staging.SetView(stage, BINDING_SRV, slot, D3D12_CPU_DESCRIPTOR_HANDLE {}, 0);
        this->m_staging.SetView(stage, BINDING_UAV, slot, D3D12_CPU_DESCRIPTOR_HANDLE {}, 0);
        return;
    }
    D3DX12Residency::ManagedObject* residency = TexResidency(tex);
    if (residency != nullptr) {
        this->m_copy.ResidencySet()->Insert(residency);
    }
    if (unordered == 0) {
        this->m_views[stage][slot] = &tex->m_srv;
        if (slot < 8) {
            this->m_1ab0[stage][slot] = 0;
        }
        if (tex->m_srv.m_handle.ptr == 0) {
            this->m_staging.CreateShaderResourceView(stage, slot, tex->m_objects[0].m_resource,
                reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(tex->m_srv.m_desc));
        } else {
            this->m_staging.SetView(stage, BINDING_SRV, slot, tex->m_srv.m_handle, 0);
        }
        uint32_t state = stage == 3 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : tex->m_srv.m_state;
        this->m_copy.TransitionObjects(tex->m_objects.data(), tex->m_objects.size(), state);
    } else {
        this->m_views[stage][slot] = nullptr;
        this->m_1ab0[stage][slot] = reinterpret_cast<uint64_t>(&tex->m_uav);
        if (tex->m_uav.m_handle.ptr == 0) {
            this->m_staging.CreateUnorderedAccessView(stage, slot, tex->m_objects[0].m_resource, nullptr,
                reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(tex->m_uav.m_desc));
        } else {
            this->m_staging.SetView(stage, BINDING_UAV, slot, tex->m_uav.m_handle, 0);
        }
        this->m_copy.TransitionObjects(tex->m_objects.data(), tex->m_objects.size(), tex->m_uav.m_state);
    }
}

// ref: FUN_1422defa0
void GraphicsCommandList::SetSampler(uint8_t stage, uint32_t slot, uint64_t key) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return;
    }
    this->m_staging.SetView(stage, BINDING_SAMPLER, slot, GxDeviceD3d12()->SamplerFor(key), 0);
}

}

// ref: FUN_1422e3fd0
void gxDx12Backend::RenderContextD3d12::BindTexture(uint8_t stage, uint32_t slot, Tex* tex, uint8_t unordered) {
    this->Owner()->BindTexture(stage, slot, tex, unordered);
}

// ref: FUN_1422df040
void gxDx12Backend::RenderContextD3d12::SetSampler(uint8_t stage, uint32_t slot, uint64_t key) {
    this->Owner()->SetSampler(stage, slot, key);
}

// ref: FUN_1422dfbf0
void gxDx12Backend::RenderContextD3d12::SetShadingRateCombiners(const uint8_t* combiners) {
    if (combiners != nullptr) {
        this->Owner()->m_wantCombiners[0] = combiners[0];
        this->Owner()->m_wantCombiners[1] = combiners[1];
    }
}
