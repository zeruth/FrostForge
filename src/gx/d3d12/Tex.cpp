#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPlatform.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>

using namespace gxDx12Backend;

CGxDeviceD3d12* GxDeviceD3d12();

namespace {

const char s_deviceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\D3d12\\GxDeviceD3d12.cpp";
const char s_gxDeviceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Gx\\src\\GxDevice.cpp";
const char s_smemNewFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Storm\\H\\SMemNew.h";

// The DXGI formats of the Gx texture formats: as a resource, typeless, as a shader resource,
// as an unordered access view (DAT_142919aa0, DAT_142919a10, DAT_142919b30, DAT_142919bc0).
const uint32_t s_resourceFormats[0x24] = { 0, 28, 87, 87, 87, 87, 51, 71, 74, 77, 80, 83, 61, 62, 49, 30, 31, 56,
    54, 34, 10, 12, 46, 41, 42, 2, 45, 55, 40, 45, 20, 83, 28, 87, 61, 0 };
const uint32_t s_typelessFormats[0x24] = { 0, 27, 90, 90, 90, 90, 48, 70, 73, 76, 79, 82, 60, 60, 48, 27, 27, 53,
    53, 33, 9, 9, 44, 39, 39, 1, 44, 53, 39, 44, 19, 82, 27, 90, 60, 0 };
const uint32_t s_shaderFormats[0x24] = { 0, 28, 87, 87, 87, 87, 51, 71, 74, 77, 80, 83, 61, 62, 49, 30, 31, 56,
    54, 34, 10, 12, 46, 41, 42, 2, 46, 56, 41, 46, 21, 83, 28, 87, 61, 0 };
const uint32_t s_unorderedFormats[0x24] = { 0, 28, 87, 87, 87, 87, 0, 0, 0, 0, 0, 0, 61, 62, 49, 30, 31, 56,
    54, 34, 10, 12, 0, 41, 42, 2, 0, 56, 41, 0, 0, 0, 42, 42, 62, 0 };

// The Gx formats that are depth formats (0x1a..0x1e), and the compressed ones (7..0xb, 0x1f).
bool IsDepthFormat(uint8_t format) {
    return format >= 0x1a && format <= 0x1e;
}

bool IsCompressedFormat(uint8_t format) {
    return (format >= 7 && format <= 0xb) || format == 0x1f;
}

// The texture format a description's format becomes (DAT_142919f30).
const uint8_t s_formatRemap[0x24] = { 0, 1, 2, 2, 2, 2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 11, 32, 33, 34, 0 };

template <class T>
void ReleaseAndNull(T** object) {
    if (*object != nullptr) {
        (*object)->Release();
        *object = nullptr;
    }
}

}

// The levels a description asks for: its count, or the full chain (to 4x4 for compressed
// formats). ref: FUN_142288520
uint32_t TexDescMipLevels(const TEXDESC* desc) {
    if ((desc->m_usage >> 0x1c) != 0) {
        return desc->m_usage >> 0x1c;
    }
    if ((desc->m_usage & 7) < 2 && ((desc->m_usage >> 0x1b) & 1) == 0) {
        return 1;
    }
    uint32_t levels = 1;
    if (!IsCompressedFormat(desc->m_format)) {
        uint32_t size = desc->m_width > 1 ? desc->m_width : 1;
        if (desc->m_dimension != 4) {
            if (desc->m_height >= size) {
                size = desc->m_height;
            }
            if (desc->m_dimension == 3 && size <= desc->m_depth) {
                size = desc->m_depth;
            }
        }
        while (size != 1) {
            size >>= 1;
            levels++;
        }
        return levels;
    }
    uint32_t size = desc->m_height <= desc->m_width ? desc->m_height : desc->m_width;
    while (size > 4) {
        size >>= 1;
        levels++;
    }
    return levels;
}

// The D3D12 description of a texture (FUN_1422db820).
static D3D12_RESOURCE_DESC* ResourceDescFromTex(D3D12_RESOURCE_DESC* out, const TEXDESC* desc) {
    uint32_t width = desc->m_width != 0 ? desc->m_width : 1;
    uint32_t height = desc->m_height != 0 ? desc->m_height : 1;
    uint32_t depth = desc->m_depth != 0 ? desc->m_depth : 1;
    uint8_t levels = static_cast<uint8_t>(TexDescMipLevels(desc));
    bool depthFormat = IsDepthFormat(desc->m_format);
    bool target = ((desc->m_usage >> 0x12) & 1) != 0 || depthFormat;
    if (desc->m_dimension == 3) {
        out->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    } else {
        out->Dimension = desc->m_dimension != 4 ? D3D12_RESOURCE_DIMENSION_TEXTURE2D : D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    }
    out->Alignment = 0;
    out->Width = width;
    out->Height = height;
    out->MipLevels = levels;
    out->DepthOrArraySize = static_cast<UINT16>(desc->m_dimension == 1 ? 6 : depth);
    bool typeless = false;
    if (!depthFormat && (desc->m_usage2 & 2) == 0) {
        // The formats a view may reinterpret (0x20..0x22) are made typeless.
        typeless = desc->m_format >= 0x20 && desc->m_format <= 0x22;
    } else {
        typeless = true;
    }
    out->Format = static_cast<DXGI_FORMAT>(typeless ? s_typelessFormats[desc->m_format] : s_resourceFormats[desc->m_format]);
    out->SampleDesc.Count = 1u << ((desc->m_usage >> 0x15) & 7);
    out->SampleDesc.Quality = 0;
    out->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    uint32_t flags = 0;
    if (!depthFormat) {
        if (target) {
            flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
    } else {
        flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if ((desc->m_usage & 0x80000) == 0) {
            flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        }
    }
    if (((desc->m_usage >> 0x14) & 1) != 0) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    out->Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    return out;
}

// A texture from its description (FUN_1422908c0, the GxTex part).
static void TexInit(Tex* tex, const TEXDESC* desc, uint32_t id) {
    tex->m_width = static_cast<uint16_t>(desc->m_width);
    tex->m_height = static_cast<uint16_t>(desc->m_height);
    tex->m_depth = static_cast<uint16_t>(desc->m_depth);
    tex->m_dimension = desc->m_dimension;
    tex->m_format = desc->m_format;
    tex->m_flags &= ~0x40u;
    tex->m_09 = desc->m_11;
    tex->m_id = id;
    tex->m_usage = desc->m_usage;
    tex->m_usage2 = desc->m_usage2;
    uint32_t levels = TexDescMipLevels(desc);
    if (desc->m_depth == 0) {
        tex->m_depth = 1;
    }
    tex->m_flags &= ~1u;
    uint16_t size = tex->m_width <= tex->m_height ? tex->m_height : tex->m_width;
    if (tex->m_dimension == 3 && size <= tex->m_depth) {
        size = tex->m_depth;
    }
    uint32_t chain = 0;
    for (; size != 0; size >>= 1) {
        chain++;
    }
    tex->m_mipLevels = static_cast<uint8_t>(levels < chain ? levels : chain);
}

// ref: FUN_1422cb6b0
Tex::Tex(const TEXDESC* desc, uint32_t id) {
    std::memset(&this->m_width, 0, offsetof(Tex, m_objects));
    TexInit(this, desc, id);
    this->m_heap = nullptr;
    this->m_heapOffset = 0;
    this->m_size = 0;
    std::memset(&this->m_residency, 0, sizeof(this->m_residency));
    D3d12Object& object = this->m_objects.emplace_back();
    std::memset(&object, 0, sizeof(object));
}

// The view vectors' storage and the objects (FUN_1422cc600).
Tex::~Tex() {
    for (D3d12Object& object : this->m_objects) {
        ReleaseAndNull(&object.m_resource);
    }
}

// ref: FUN_1422ea7c0
Tex::Tex(Tex&& other) {
    std::memcpy(&this->m_width, &other.m_width, offsetof(Tex, m_objects));
    for (D3d12Object& object : other.m_objects) {
        this->m_objects.emplace_back(object);
        object.m_resource = nullptr;
    }
    other.m_objects.m_size = 0;
    auto moveTarget = [](TargetView* to, TargetView* from, Tex* owner) {
        to->m_tex = from->m_tex == nullptr ? nullptr : owner;
        to->m_id = from->m_id;
        to->m_mip = from->m_mip;
        to->m_state = from->m_state;
        for (D3d12Object* object : from->m_objects) {
            to->m_objects.emplace_back(object);
        }
        to->m_mipBegin = from->m_mipBegin;
        to->m_mipEnd = from->m_mipEnd;
        to->m_sliceBegin = from->m_sliceBegin;
        to->m_sliceEnd = from->m_sliceEnd;
        to->m_handle = from->m_handle;
    };
    auto moveShader = [](ShaderView* to, ShaderView* from, Tex* owner) {
        to->m_tex = from->m_tex == nullptr ? nullptr : owner;
        to->m_id = from->m_id;
        to->m_state = from->m_state;
        for (D3d12Object* object : from->m_objects) {
            to->m_objects.emplace_back(object);
        }
        to->m_mipBegin = from->m_mipBegin;
        to->m_mipEnd = from->m_mipEnd;
        to->m_sliceBegin = from->m_sliceBegin;
        to->m_sliceEnd = from->m_sliceEnd;
        std::memcpy(to->m_desc, from->m_desc, sizeof(to->m_desc));
        to->m_handle = from->m_handle;
    };
    moveShader(&this->m_srv, &other.m_srv, this);
    moveShader(&this->m_stencilSrv, &other.m_stencilSrv, this);
    moveShader(&this->m_uav, &other.m_uav, this);
    moveTarget(&this->m_rtv, &other.m_rtv, this);
    this->m_heap = other.m_heap;
    this->m_heapOffset = other.m_heapOffset;
    this->m_size = other.m_size;
    this->m_residency = other.m_residency;
}

// ---- HeapPage ----------------------------------------------------------------------------

// ref: FUN_1422c2350
void VariableSizeAllocationsManager::AddNewBlock(uint32_t offset, uint32_t size) {
    auto inserted = this->m_byOffset.emplace(offset, FreeBlockInfo { size, this->m_bySize.end() });
    if (!inserted.second) {
        return;
    }
    inserted.first->second.m_bySize = this->m_bySize.emplace(size, offset);
}

// Takes the smallest free block that fits, splitting it.
// ref: FUN_1422c27d0
uint32_t VariableSizeAllocationsManager::Allocate(uint32_t size) {
    if (this->m_freeSize < size) {
        return 0xffffffff;
    }
    auto smallest = this->m_bySize.lower_bound(size);
    if (smallest == this->m_bySize.end()) {
        return 0xffffffff;
    }
    uint32_t offset = smallest->second;
    uint32_t blockSize = smallest->first;
    this->m_bySize.erase(smallest);
    this->m_byOffset.erase(offset);
    if (blockSize - size != 0) {
        this->AddNewBlock(offset + size, blockSize - size);
    }
    this->m_freeSize -= size;
    return offset;
}

// Returns a block, merging it with its free neighbours.
// ref: FUN_1422c2d00
void VariableSizeAllocationsManager::Free(uint32_t offset, uint32_t size) {
    auto next = this->m_byOffset.upper_bound(offset);
    auto previous = next;
    uint32_t newOffset = offset;
    uint32_t newSize = size;
    if (previous != this->m_byOffset.begin()) {
        --previous;
        if (previous->first + previous->second.m_size == offset) {
            newOffset = previous->first;
            newSize += previous->second.m_size;
            this->m_bySize.erase(previous->second.m_bySize);
            this->m_byOffset.erase(previous);
        }
    }
    if (next != this->m_byOffset.end() && offset + size == next->first) {
        newSize += next->second.m_size;
        this->m_bySize.erase(next->second.m_bySize);
        this->m_byOffset.erase(next);
    }
    this->AddNewBlock(newOffset, newSize);
    this->m_freeSize += size;
}

// ref: FUN_1422cb610
HeapPage::HeapPage(uint32_t size) {
    this->m_heap = nullptr;
    std::memset(&this->m_residency, 0, sizeof(this->m_residency));
    this->m_allocations.m_maxSize = size;
    this->m_allocations.m_freeSize = size;
    this->m_allocations.AddNewBlock(0, size);
}

// ref: FUN_1422cc560
HeapPage::~HeapPage() {
    if (this->m_residency.m_underlying != nullptr) {
        GxDeviceD3d12()->m_residency.EndTrackingObject(&this->m_residency);
    }
    ReleaseAndNull(&this->m_heap);
}

// ---- creation ----------------------------------------------------------------------------

// Places a texture in a 64 MB heap page with room, making a page when none has.
// ref: FUN_1422cfa50
bool CGxDeviceD3d12::CreatePlaced(const D3D12_RESOURCE_DESC* desc, uint32_t state, const D3D12_CLEAR_VALUE* clear,
    bool mayFail, Tex* tex) {
    g_gxDeviceLock.Enter();
    D3D12_RESOURCE_ALLOCATION_INFO info = this->m_device->GetResourceAllocationInfo(0, 1, desc);
    tex->m_heap = nullptr;
    tex->m_heapOffset = 0;
    tex->m_size = static_cast<uint32_t>(info.SizeInBytes + 0xffff) & 0xffff0000;
    uint32_t pages = static_cast<uint32_t>(this->m_heapPages.size());
    if (mayFail) {
        pages--;
    }
    for (uint32_t i = 0; i < pages; i++) {
        HeapPage* page = this->m_heapPages[i];
        uint32_t offset = page->m_allocations.Allocate(tex->m_size);
        tex->m_heapOffset = offset;
        if (offset != 0xffffffff) {
            tex->m_heap = page;
            break;
        }
    }
    bool result = false;
    if (!mayFail && tex->m_heap == nullptr) {
        this->m_placedBytes += 0x4000000;
        D3D12_HEAP_DESC heapDesc = {};
        heapDesc.SizeInBytes = 0x4000000;
        heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapDesc.Properties.CreationNodeMask = 1;
        heapDesc.Properties.VisibleNodeMask = 1;
        heapDesc.Alignment = 0x10000;
        heapDesc.Flags = static_cast<D3D12_HEAP_FLAGS>(0x44);
        ID3D12Heap* heap = nullptr;
        HRESULT hr = this->m_device->CreateHeap(&heapDesc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
        if (FAILED(hr)) {
            this->Fail(hr, "CreateHeap failed");
            ReleaseAndNull(&heap);
            g_gxDeviceLock.Leave();
            return false;
        }
        auto page = new (SMemAlloc(0xb8, 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0x11bf, 0)) HeapPage(0x4000000);
        tex->m_heap = page;
        page->m_heap = heap;
        this->m_heapPages.push_back(page);
        page->m_residency.m_size = heapDesc.SizeInBytes;
        page->m_residency.m_underlying = page->m_heap;
        page->m_residency.m_lastGpuSyncPoint = 0;
        this->m_residency.BeginTrackingObject(&page->m_residency);
        tex->m_heapOffset = page->m_allocations.Allocate(tex->m_size);
    } else if (mayFail && tex->m_heap == nullptr) {
        g_gxDeviceLock.Leave();
        return false;
    }
    ID3D12Resource* resource = nullptr;
    HRESULT hr = this->m_device->CreatePlacedResource(tex->m_heap->m_heap, tex->m_heapOffset, desc,
        static_cast<D3D12_RESOURCE_STATES>(state), clear, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    if (FAILED(hr)) {
        this->Fail(hr, "CreatePlacedResource failed");
        tex->m_heap = nullptr;
    } else {
        HeapPage* page = tex->m_heap;
        page->m_textures.push_back(tex);
        tex->HeapIndex() = page->m_textures.size() - 1;
        ID3D12Resource* old = tex->m_objects[0].m_resource;
        tex->m_objects[0].m_resource = resource;
        resource = nullptr;
        if (old != nullptr) {
            old->Release();
        }
        result = true;
    }
    ReleaseAndNull(&resource);
    g_gxDeviceLock.Leave();
    return result;
}

// The view's objects: the texture's single object, or each subresource the range covers.
template <class View>
static void ViewAddObjects(View* view, Tex* tex) {
    if ((tex->m_flags & 0x40) != 0) {
        return;
    }
    if ((tex->m_usage2 & 1) == 0 && ((tex->m_usage >> 0x1b) & 1) == 0) {
        view->m_objects.push_back(tex->m_objects.data());
        return;
    }
    view->m_objects.reserve((view->m_mipEnd - view->m_mipBegin) * (view->m_sliceEnd - view->m_sliceBegin));
    for (uint32_t slice = view->m_sliceBegin; slice < view->m_sliceEnd; slice++) {
        for (uint32_t mip = view->m_mipBegin; mip < view->m_mipEnd; mip++) {
            view->m_objects.push_back(&tex->m_objects[tex->m_mipLevels * slice + mip]);
        }
    }
}

static uint64_t SliceRange(const Tex* tex, const VIEWRANGE* range) {
    if (tex->m_dimension != 5) {
        return 0x100000000ull;
    }
    uint32_t end = range->m_sliceCount == 0xffff ? tex->m_depth : range->m_sliceCount + range->m_slice;
    return (static_cast<uint64_t>(end) << 32) | range->m_slice;
}

// ref: FUN_1422da750
bool CGxDeviceD3d12::CreateTargetView(Tex* tex, const VIEWRANGE* range, ID3D12Resource* resource, TargetView* view) {
    if (resource == nullptr) {
        view->m_handle.ptr = 0;
    } else {
        D3D12_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = static_cast<DXGI_FORMAT>(s_resourceFormats[tex->m_format]);
        switch (tex->m_dimension) {
        case 1:
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = range->m_mip;
            desc.Texture2DArray.FirstArraySlice = 0;
            desc.Texture2DArray.ArraySize = 6;
            break;
        case 3: {
            uint32_t count;
            if (range->m_sliceCount == 0xffff) {
                count = tex->m_depth - range->m_slice;
            } else {
                count = range->m_slice;
            }
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            desc.Texture3D.MipSlice = range->m_mip;
            desc.Texture3D.FirstWSlice = range->m_slice;
            desc.Texture3D.WSize = count;
            break;
        }
        case 4:
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
            desc.Texture1D.MipSlice = range->m_mip;
            break;
        case 5: {
            uint32_t count = range->m_sliceCount == 0xffff ? tex->m_depth - range->m_slice : range->m_sliceCount;
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = range->m_mip;
            desc.Texture2DArray.FirstArraySlice = range->m_slice;
            desc.Texture2DArray.ArraySize = count;
            break;
        }
        default:
            if ((tex->m_usage & 0xe00000) == 0) {
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = range->m_mip;
                desc.Texture2D.PlaneSlice = 0;
            } else {
                desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
            }
            break;
        }
        if (!this->m_rtvHeap.Allocate(&view->m_handle)) {
            return false;
        }
        this->m_device->CreateRenderTargetView(resource, &desc, view->m_handle);
    }
    view->m_objects.m_size = 0;
    view->m_tex = tex;
    view->m_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    view->m_mipBegin = range->m_mip;
    view->m_mipEnd = range->m_mip + 1u;
    uint64_t slices = SliceRange(tex, range);
    view->m_sliceBegin = static_cast<uint32_t>(slices);
    view->m_sliceEnd = static_cast<uint32_t>(slices >> 32);
    view->m_id = static_cast<uint32_t>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->Field<uint32_t>(0x468))) - 1);
    view->m_mip = range->m_mip;
    ViewAddObjects(view, tex);
    return true;
}

// ref: FUN_1422da080
bool CGxDeviceD3d12::CreateDepthView(Tex* tex, const VIEWRANGE* range, ID3D12Resource* resource, TargetView* view) {
    if (resource == nullptr) {
        view->m_handle.ptr = 0;
    } else {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
        desc.Format = static_cast<DXGI_FORMAT>(s_resourceFormats[tex->m_format]);
        desc.Flags = static_cast<D3D12_DSV_FLAGS>(range->m_flags & 1);
        switch (tex->m_dimension) {
        case 1:
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = range->m_mip;
            desc.Texture2DArray.FirstArraySlice = 0;
            desc.Texture2DArray.ArraySize = 6;
            break;
        case 3:
            break;
        case 4:
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
            desc.Texture1D.MipSlice = range->m_mip;
            break;
        case 5: {
            uint32_t count = range->m_sliceCount == 0xffff ? tex->m_depth - range->m_slice : range->m_sliceCount;
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = range->m_mip;
            desc.Texture2DArray.FirstArraySlice = range->m_slice;
            desc.Texture2DArray.ArraySize = count;
            break;
        }
        default:
            if ((tex->m_usage & 0xe00000) == 0) {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = range->m_mip;
            } else {
                desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
            }
            break;
        }
        if (!this->m_dsvHeap.Allocate(&view->m_handle)) {
            return false;
        }
        this->m_device->CreateDepthStencilView(resource, &desc, view->m_handle);
    }
    view->m_objects.m_size = 0;
    view->m_tex = tex;
    view->m_state = (range->m_flags & 1) == 0 ? D3D12_RESOURCE_STATE_DEPTH_WRITE : 0xe0;
    view->m_mipBegin = range->m_mip;
    view->m_mipEnd = range->m_mip + 1u;
    uint64_t slices = SliceRange(tex, range);
    view->m_sliceBegin = static_cast<uint32_t>(slices);
    view->m_sliceEnd = static_cast<uint32_t>(slices >> 32);
    view->m_id = static_cast<uint32_t>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->Field<uint32_t>(0x468))) - 1);
    view->m_mip = range->m_mip;
    ViewAddObjects(view, tex);
    return true;
}

// ref: FUN_1422daa60
bool CGxDeviceD3d12::CreateShaderView(Tex* tex, const VIEWRANGE* range, ID3D12Resource* resource, ShaderView* view) {
    if (resource == nullptr) {
        view->m_handle.ptr = 0;
    } else {
        auto desc = reinterpret_cast<D3D12_SHADER_RESOURCE_VIEW_DESC*>(view->m_desc);
        std::memset(desc, 0, sizeof(view->m_desc));
        uint32_t format = s_shaderFormats[tex->m_format];
        if ((range->m_flags & 2) != 0) {
            format = tex->m_format == 0x1d ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        }
        desc->Format = static_cast<DXGI_FORMAT>(format);
        desc->Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        uint32_t mips = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(range->m_mipCount)));
        switch (tex->m_dimension) {
        case 1:
            desc->ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            desc->TextureCube.MostDetailedMip = range->m_mip;
            desc->TextureCube.MipLevels = mips;
            break;
        case 3:
            desc->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            desc->Texture3D.MostDetailedMip = range->m_mip;
            desc->Texture3D.MipLevels = mips;
            break;
        case 4:
            desc->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            desc->Texture1D.MostDetailedMip = range->m_mip;
            desc->Texture1D.MipLevels = mips;
            break;
        case 5: {
            uint32_t count = range->m_sliceCount == 0xffff ? tex->m_depth - range->m_slice : range->m_sliceCount;
            desc->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            desc->Texture2DArray.MostDetailedMip = range->m_mip;
            desc->Texture2DArray.MipLevels = mips;
            desc->Texture2DArray.FirstArraySlice = range->m_slice;
            desc->Texture2DArray.ArraySize = count;
            desc->Texture2DArray.PlaneSlice = (range->m_flags >> 1) & 1;
            break;
        }
        default:
            if ((tex->m_usage & 0xe00000) != 0) {
                desc->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            } else {
                desc->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                desc->Texture2D.MostDetailedMip = range->m_mip;
                desc->Texture2D.MipLevels = mips;
                desc->Texture2D.PlaneSlice = (range->m_flags >> 1) & 1;
            }
            break;
        }
        if (this->m_viewHeap.Allocate(&view->m_handle)) {
            this->m_device->CreateShaderResourceView(resource, desc, view->m_handle);
        }
    }
    view->m_objects.m_size = 0;
    view->m_tex = tex;
    view->m_state = ((tex->m_usage2 >> 2) & 1) != 0 ? 0xe0 : 0xc0;
    uint32_t end = range->m_mipCount == 0xff ? tex->m_mipLevels : range->m_mipCount + range->m_mip;
    view->m_mipBegin = range->m_mip;
    view->m_mipEnd = end;
    uint64_t slices = SliceRange(tex, range);
    view->m_sliceBegin = static_cast<uint32_t>(slices);
    view->m_sliceEnd = static_cast<uint32_t>(slices >> 32);
    view->m_id = static_cast<uint32_t>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->Field<uint32_t>(0x468))) - 1);
    ViewAddObjects(view, tex);
    return true;
}

// ref: FUN_1422dadb0
bool CGxDeviceD3d12::CreateUnorderedView(Tex* tex, const VIEWRANGE* range, ID3D12Resource* resource, ShaderView* view) {
    if (resource == nullptr) {
        view->m_handle.ptr = 0;
    } else {
        auto desc = reinterpret_cast<D3D12_UNORDERED_ACCESS_VIEW_DESC*>(view->m_desc);
        std::memset(view->m_desc, 0, sizeof(view->m_desc));
        desc->Format = static_cast<DXGI_FORMAT>(s_unorderedFormats[tex->m_format]);
        switch (tex->m_dimension) {
        case 1:
            desc->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc->Texture2DArray.MipSlice = range->m_mip;
            desc->Texture2DArray.ArraySize = 6;
            break;
        case 3: {
            uint32_t first;
            uint32_t count;
            if (range->m_sliceCount == 0xffff) {
                count = tex->m_depth - range->m_slice;
                first = range->m_slice;
            } else {
                count = range->m_slice;
                first = count;
            }
            desc->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            desc->Texture3D.MipSlice = range->m_mip;
            desc->Texture3D.FirstWSlice = first;
            desc->Texture3D.WSize = count;
            break;
        }
        case 4:
            desc->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            desc->Texture1D.MipSlice = range->m_mip;
            break;
        case 5: {
            uint32_t count = range->m_sliceCount == 0xffff ? tex->m_depth - range->m_slice : range->m_sliceCount;
            desc->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc->Texture2DArray.MipSlice = range->m_mip;
            desc->Texture2DArray.FirstArraySlice = range->m_slice;
            desc->Texture2DArray.ArraySize = count;
            break;
        }
        default:
            desc->ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            desc->Texture2D.MipSlice = range->m_mip;
            break;
        }
        if (this->m_viewHeap.Allocate(&view->m_handle)) {
            this->m_device->CreateUnorderedAccessView(resource, nullptr, desc, view->m_handle);
        }
    }
    view->m_objects.m_size = 0;
    view->m_tex = tex;
    view->m_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    view->m_mipBegin = range->m_mip;
    view->m_mipEnd = range->m_mip + 1u;
    uint64_t slices = SliceRange(tex, range);
    view->m_sliceBegin = static_cast<uint32_t>(slices);
    view->m_sliceEnd = static_cast<uint32_t>(slices >> 32);
    view->m_id = static_cast<uint32_t>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->Field<uint32_t>(0x468))) - 1);
    ViewAddObjects(view, tex);
    return true;
}

// Makes a texture's resource (committed, placed, or one handed in such as a back buffer),
// registers its states, and makes the views its usage asks for.
// ref: FUN_1422d9810
bool CGxDeviceD3d12::TexCreate(const TEXDESC* desc, Tex* tex, bool mayFail, ID3D12Resource* resource) {
    uint32_t usage = desc->m_usage;
    bool depthFormat = IsDepthFormat(tex->m_format);
    bool target = ((usage >> 0x12) & 1) != 0 || depthFormat;
    D3D12_RESOURCE_DESC resourceDesc;
    ResourceDescFromTex(&resourceDesc, desc);
    D3D12_CLEAR_VALUE clear = {};
    const D3D12_CLEAR_VALUE* clearValue = nullptr;
    uint32_t state = D3D12_RESOURCE_STATE_COMMON;
    clear.Format = static_cast<DXGI_FORMAT>(s_resourceFormats[tex->m_format]);
    if (resource == nullptr && depthFormat) {
        state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        clear.DepthStencil.Depth = 1.0f;
        clearValue = &clear;
    } else if (resource == nullptr && !target && (desc->m_usage & 0x100000) != 0) {
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    } else if (resource == nullptr && target) {
        state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        clearValue = &clear;
    } else if (target) {
        clearValue = &clear;
        if (depthFormat) {
            clear.DepthStencil.Depth = 1.0f;
        }
    }
    D3D12_RESOURCE_ALLOCATION_INFO info = this->m_device->GetResourceAllocationInfo(0, 1, &resourceDesc);
    bool committed = target || ((usage >> 0x14) & 1) != 0 || resource != nullptr;
    uint64_t largeSize = this->m_largeHeaps == 0 ? 0x1000000 : 0x4000000;
    if (largeSize <= info.SizeInBytes) {
        committed = true;
    }
    // The description names a GxTexHeap; its page is at +0x10.
    auto texHeap = static_cast<TEXHEAP*>(desc->m_heap);
    HeapPage* heap = texHeap != nullptr ? &texHeap->m_page : nullptr;
    if (heap != nullptr) {
        committed = false;
    }
    if (!committed) {
        if (heap == nullptr) {
            if (!this->CreatePlaced(&resourceDesc, state, clearValue, mayFail, tex) && mayFail) {
                return false;
            }
            this->m_placedCount++;
        } else {
            ID3D12Resource* placed = nullptr;
            HRESULT hr = this->m_device->CreatePlacedResource(heap->m_heap, desc->m_28, &resourceDesc,
                static_cast<D3D12_RESOURCE_STATES>(state), clearValue, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&placed));
            if (FAILED(hr)) {
                this->Fail(hr, "GxTexHeap TexCreate failed");
                tex->m_heap = nullptr;
                tex->m_heapOffset = 0;
                tex->m_size = 0;
                ReleaseAndNull(&placed);
                return false;
            }
            tex->m_heap = heap;
            tex->m_heapOffset = static_cast<uint32_t>(desc->m_28);
            tex->m_size = static_cast<uint32_t>(info.SizeInBytes);
            ID3D12Resource* old = tex->m_objects[0].m_resource;
            tex->m_objects[0].m_resource = placed;
            if (old != nullptr) {
                old->Release();
            }
        }
    } else {
        tex->m_size = static_cast<uint32_t>(info.SizeInBytes);
        ID3D12Resource** slot = &tex->m_objects[0].m_resource;
        if (resource == nullptr) {
            D3D12_HEAP_PROPERTIES heapProperties = {};
            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapProperties.CreationNodeMask = 1;
            heapProperties.VisibleNodeMask = 1;
            ReleaseAndNull(slot);
            HRESULT hr = this->m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                static_cast<D3D12_RESOURCE_STATES>(state), clearValue, __uuidof(ID3D12Resource), reinterpret_cast<void**>(slot));
            if (FAILED(hr)) {
                this->Fail(hr, "Tex Create Failed");
                GxLog("Committed resource create failed\n    HR:%x Format:%d Size:%llu (%u, %u, %u)", hr, tex->m_format,
                    info.SizeInBytes, tex->m_width, tex->m_height, tex->m_depth);
                return false;
            }
        } else {
            ReleaseAndNull(slot);
            *slot = resource;
        }
        tex->m_residency.m_underlying = *slot;
        tex->m_residency.m_size = info.SizeInBytes;
        tex->m_residency.m_lastGpuSyncPoint = 0;
        this->m_residency.BeginTrackingObject(&tex->m_residency);
        InterlockedAdd(reinterpret_cast<volatile LONG*>(&this->m_committedBytes), static_cast<LONG>(tex->m_size));
        this->m_committedCount++;
    }

    // The resource's states: one object, or one per subresource.
    bool untracked = depthFormat && ((usage >> 0x13) & 1) == 0 && ((usage >> 0x14) & 1) == 0 && (desc->m_usage2 & 1) == 0;
    g_gxDeviceLock.Enter();
    D3DX12Residency::ManagedObject* residency = committed ? &tex->m_residency : &tex->m_heap->m_residency;
    if ((desc->m_usage2 & 1) == 0 && ((usage >> 0x1b) & 1) == 0) {
        D3d12Object& object = tex->m_objects[0];
        object.m_subresource = 0xffffffff;
        object.m_residency = residency;
        object.m_untracked = untracked;
        *this->m_states.insert(reinterpret_cast<uint64_t>(&object)) = state;
    } else {
        uint32_t count = tex->m_mipLevels;
        if (tex->m_dimension == 5) {
            count = tex->m_mipLevels * tex->m_depth;
        }
        tex->m_objects.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            D3d12Object& object = tex->m_objects[i];
            if (i != 0 && object.m_resource != tex->m_objects[0].m_resource) {
                tex->m_objects[0].m_resource->AddRef();
                ReleaseAndNull(&object.m_resource);
                object.m_resource = tex->m_objects[0].m_resource;
            }
            object.m_residency = residency;
            object.m_subresource = i;
            object.m_untracked = 0;
            *this->m_states.insert(reinterpret_cast<uint64_t>(&object)) = state;
        }
    }
    g_gxDeviceLock.Leave();

    VIEWRANGE range = { 0, 0xff, 0, 0xffff, 0 };
    ID3D12Resource* created = tex->m_objects[0].m_resource;
    if ((tex->m_usage & 0x80000) != 0) {
        if (!this->CreateShaderView(tex, &range, created, &tex->m_srv)) {
            return false;
        }
        if (tex->m_format == 0x1d || tex->m_format == 0x1e) {
            VIEWRANGE stencil = { 0, 0xff, 0, 0xffff, 2 };
            if (!this->CreateShaderView(tex, &stencil, created, &tex->m_stencilSrv)) {
                return false;
            }
        }
    }
    if ((tex->m_usage & 0x40000) != 0) {
        bool ok = !IsDepthFormat(tex->m_format) ? this->CreateTargetView(tex, &range, created, &tex->m_rtv)
                                                : this->CreateDepthView(tex, &range, created, &tex->m_rtv);
        if (!ok) {
            return false;
        }
    }
    if ((tex->m_usage & 0x100000) != 0 && !this->CreateUnorderedView(tex, &range, created, &tex->m_uav)) {
        return false;
    }
    tex->m_flags |= 1;
    return true;
}

// Returns the views' descriptors to their heaps.
// ref: FUN_1422da380
void CGxDeviceD3d12::TexReleaseViews(Tex* tex) {
    if ((tex->m_flags & 1) == 0) {
        return;
    }
    if (tex->m_srv.m_handle.ptr != 0) {
        this->m_viewHeap.Free(tex->m_srv.m_handle.ptr);
        tex->m_srv.m_handle.ptr = 0;
    }
    if (tex->m_stencilSrv.m_handle.ptr != 0) {
        this->m_viewHeap.Free(tex->m_stencilSrv.m_handle.ptr);
        tex->m_stencilSrv.m_handle.ptr = 0;
    }
    if (tex->m_rtv.m_handle.ptr != 0) {
        DescriptorHeap& heap = IsDepthFormat(tex->m_format) ? this->m_dsvHeap : this->m_rtvHeap;
        heap.Free(tex->m_rtv.m_handle.ptr);
        tex->m_rtv.m_handle.ptr = 0;
    }
    if (tex->m_uav.m_handle.ptr != 0) {
        this->m_viewHeap.Free(tex->m_uav.m_handle.ptr);
        tex->m_uav.m_handle.ptr = 0;
    }
    if (tex->m_heap == nullptr) {
        this->m_committedCount--;
        InterlockedAdd(reinterpret_cast<volatile LONG*>(&this->m_committedBytes), -static_cast<LONG>(tex->m_size));
    } else {
        this->m_placedCount--;
    }
    HeapPage* page = tex->m_heap;
    if (page != nullptr && page->m_allocations.m_maxSize != 0) {
        g_gxDeviceLock.Enter();
        Tex* last = page->m_textures.back();
        last->HeapIndex() = tex->HeapIndex();
        page->m_textures[tex->HeapIndex()] = last;
        page->m_textures.m_size--;
        g_gxDeviceLock.Leave();
    }
}

// Forgets the texture's states, frees its heap range and releases the resource.
// ref: FUN_1422da5d0
void CGxDeviceD3d12::TexRelease(Tex* tex) {
    for (D3d12Object& object : tex->m_objects) {
        this->m_states.erase(reinterpret_cast<uint64_t>(&object));
    }
    HeapPage* page = tex->m_heap;
    if (page != nullptr && page->m_allocations.m_maxSize != 0) {
        page->m_allocations.Free(tex->m_heapOffset, tex->m_size);
    }
    if (tex->m_residency.m_underlying != nullptr) {
        this->m_residency.EndTrackingObject(&tex->m_residency);
    }
    ReleaseAndNull(&tex->m_objects[0].m_resource);
}

// The release a frame's callbacks run once the GPU is done with the texture (FUN_1422c7880).
static void TexReleaseCallback(void* param) {
    Tex* tex = *static_cast<Tex**>(param);
    GxDeviceD3d12()->TexRelease(tex);
    tex->~Tex();
    operator delete(tex, sizeof(Tex));
}

// Device slot 0x200: makes a Gx texture; false (with a usable empty texture) on failure.
// ref: FUN_1422e1880
bool CGxDeviceD3d12::TexCreate(const void* descHandle, void** out) {
    auto gxDesc = static_cast<const TEXDESC*>(descHandle);
    TEXDESC desc = *gxDesc;
    desc.m_format = s_formatRemap[gxDesc->m_format];
    if (((desc.m_usage >> 0x1b) & 1) != 0) {
        desc.m_usage |= 0x180000;
    }
    if (desc.m_heap != nullptr && this->m_options.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_1 &&
        static_cast<TEXHEAP*>(desc.m_heap)->m_targets != 0) {
        desc.m_usage |= 0x40000;
    }
    void* memory = SMemAlloc(sizeof(Tex), 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0x1427, 0);
    uint32_t id = static_cast<uint32_t>(InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->Field<uint32_t>(0x468))) - 1);
    Tex* tex = new (memory) Tex(&desc, id);
    if (this->m_ready == 0 || !this->TexCreate(&desc, tex, false, nullptr)) {
        // Views without a resource, so binding the texture still works.
        VIEWRANGE range = { 0, 0xff, 0, 0xffff, 0 };
        if ((tex->m_usage & 0x80000) != 0) {
            this->CreateShaderView(tex, &range, nullptr, &tex->m_srv);
            if (desc.m_format == 0x1d || desc.m_format == 0x1e) {
                VIEWRANGE stencil = { 0, 0xff, 0, 0xffff, 2 };
                this->CreateShaderView(tex, &stencil, nullptr, &tex->m_stencilSrv);
            }
        }
        if ((tex->m_usage & 0x100000) != 0) {
            this->CreateUnorderedView(tex, &range, nullptr, &tex->m_uav);
        }
        if ((tex->m_usage & 0x40000) != 0) {
            if (!IsDepthFormat(tex->m_format)) {
                this->CreateTargetView(tex, &range, nullptr, &tex->m_rtv);
            } else {
                this->CreateDepthView(tex, &range, nullptr, &tex->m_rtv);
            }
        }
        this->m_ready = 0;
    }
    *out = tex;
    return (tex->m_flags & 1) != 0;
}

// Device slot 0x208: the views go now, the resource when the frame is done.
// ref: FUN_1422e1b70
void CGxDeviceD3d12::TexDestroy(void* handle) {
    auto tex = static_cast<Tex*>(handle);
    this->TexReleaseViews(tex);
    FrameData* frame = this->m_frame;
    SCritSect* lock = static_cast<SCritSect*>(frame->m_callbacks.m_lock);
    lock->Enter();
    auto slot = static_cast<Tex**>(frame->m_callbacks.Add(TexReleaseCallback, sizeof(Tex*), 8));
    lock->Leave();
    if (slot != nullptr) {
        *slot = tex;
    }
}

// Device slot 0x260.
// ref: FUN_1422e1be0
void* CGxDeviceD3d12::TexTargetView(void* handle) {
    auto tex = static_cast<Tex*>(handle);
    if (tex != nullptr && (tex->m_flags & 1) != 0) {
        return &tex->m_rtv;
    }
    this->Fail(E_FAIL, "TexGetDefaultRenderTargetView tex not valid");
    return &this->DefaultTexD3d12()->m_rtv;
}

// Device slot 0x250.
// ref: FUN_1422e1c30
void* CGxDeviceD3d12::TexShaderView(void* handle, uint32_t plane) {
    auto tex = static_cast<Tex*>(handle);
    if (tex != nullptr && (tex->m_flags & 1) != 0) {
        return plane == 0 ? &tex->m_srv : &tex->m_stencilSrv;
    }
    this->Fail(E_FAIL, "TexGetDefaultShaderResourceView tex not valid");
    return &this->DefaultTexD3d12()->m_srv;
}

// Device slot 0x258.
// ref: FUN_1422e1c80
void* CGxDeviceD3d12::TexUnorderedView(void* handle) {
    auto tex = static_cast<Tex*>(handle);
    if (tex != nullptr && (tex->m_flags & 1) != 0) {
        return &tex->m_uav;
    }
    this->Fail(E_FAIL, "TexGetDefaultWritableResourceView tex not valid");
    return &this->DefaultTexD3d12()->m_uav;
}

// Device slot 0x210: the texture views fall back to (a 1x1 render target, made once).
// ref: FUN_1422e1cd0
void* CGxDeviceD3d12::DefaultTex() {
    return this->DefaultTexD3d12();
}

Tex* CGxDeviceD3d12::DefaultTexD3d12() {
    alignas(Tex) static uint8_t s_storage[sizeof(Tex)];  // DAT_1434910e0
    static Tex* s_tex = [] {
        TEXDESC desc = {};
        desc.m_dimension = 0;
        desc.m_width = 1;
        desc.m_height = 1;
        desc.m_depth = 1;
        desc.m_format = 2;
        desc.m_11 = 2;
        desc.m_usage = 0x81c2001;
        desc.m_usage2 = 3;
        Tex* tex = new (s_storage) Tex(&desc, 0);
        tex->m_flags |= 0x41;
        tex->m_srv.m_mipBegin = 0;
        tex->m_srv.m_mipEnd = 1;
        tex->m_stencilSrv.m_mipBegin = 0;
        tex->m_stencilSrv.m_mipEnd = 1;
        tex->m_rtv.m_mipBegin = 0;
        tex->m_rtv.m_mipEnd = 1;
        tex->m_srv.m_tex = tex;
        tex->m_uav.m_tex = tex;
        tex->m_rtv.m_tex = tex;
        tex->m_rtv.m_handle.ptr = 0;
        return tex;
    }();
    return s_tex;
}

// Device slot 0x1f8: the size and alignment the texture a description makes would take.
// ref: FUN_1422cde90
GXTEXALLOCINFO* CGxDeviceD3d12::TexAllocationInfo(GXTEXALLOCINFO* out, const void* descHandle) {
    TEXDESC desc = *static_cast<const TEXDESC*>(descHandle);
    desc.m_format = s_formatRemap[desc.m_format];
    if (((desc.m_usage >> 0x1b) & 1) != 0) {
        desc.m_usage |= 0x180000;
    }
    if (desc.m_heap != nullptr && this->m_options.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_1 &&
        static_cast<TEXHEAP*>(desc.m_heap)->m_targets != 0) {
        desc.m_usage |= 0x40000;
    }
    D3D12_RESOURCE_DESC resourceDesc;
    ResourceDescFromTex(&resourceDesc, &desc);
    D3D12_RESOURCE_ALLOCATION_INFO info = this->m_device->GetResourceAllocationInfo(0, 1, &resourceDesc);
    out->m_size = info.SizeInBytes;
    out->m_alignment = static_cast<uint32_t>(info.Alignment);
    return out;
}

// Device slot 0x1e8: a heap textures can be placed in.
// ref: FUN_1422e1e60
bool CGxDeviceD3d12::TexHeapCreate(const void* descHandle, void** out) {
    auto desc = static_cast<const TEXHEAPDESC*>(descHandle);
    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = desc->m_size;
    heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapDesc.Properties.CreationNodeMask = 1;
    heapDesc.Properties.VisibleNodeMask = 1;
    heapDesc.Alignment = desc->m_alignment < 0x10001 ? 0x10000 : desc->m_alignment;
    if (this->m_options.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_1) {
        heapDesc.Flags = static_cast<D3D12_HEAP_FLAGS>(desc->m_targets != 0 ? 0x84 : 0x44);
    }
    auto heap = static_cast<TEXHEAP*>(SMemAlloc(sizeof(TEXHEAP), 0xd, s_deviceFile, SStrHashFnv1a(s_deviceFile), 0x13a4, 0));
    heap->m_size = desc->m_size;
    heap->m_targets = desc->m_targets;
    new (&heap->m_page) HeapPage(0);
    HRESULT hr = this->m_device->CreateHeap(&heapDesc, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap->m_page.m_heap));
    if (FAILED(hr)) {
        this->Fail(hr, "CreateHeap Failed");
        return false;
    }
    g_gxDeviceLock.Enter();
    this->m_placedBytes += static_cast<uint32_t>(desc->m_size);
    g_gxDeviceLock.Leave();
    heap->m_page.m_residency.m_size = heapDesc.SizeInBytes;
    heap->m_page.m_residency.m_underlying = heap->m_page.m_heap;
    heap->m_page.m_residency.m_lastGpuSyncPoint = 0;
    this->m_residency.BeginTrackingObject(&heap->m_page.m_residency);
    *out = heap;
    return true;
}

// ---- the frame callbacks -----------------------------------------------------------------

static void CallbackFiller(void*) {
}

GxCallbackList::GxCallbackList() {
    this->m_pageSize = 0x10000;
    this->m_08 = CallbackFiller;
    this->m_lock = &g_gxDeviceLock;
}

GxCallbackList::~GxCallbackList() {
    for (CallbackPage& page : this->m_pages) {
        if (page.m_begin != nullptr) {
            SMemFree(page.m_begin, s_smemNewFile, 0xa8, 0);
        }
    }
    for (void* page : this->m_freePages) {
        if (page != nullptr) {
            SMemFree(page, s_smemNewFile, 0xa8, 0);
        }
    }
}

// Space for a callback's `size` bytes of data in the last page, or a new page; the record's
// header packs the function with the record length. ref: FUN_1422911b0
void* GxCallbackList::Add(void (*callback)(void*), uint64_t size, uint64_t alignment) {
    if (alignment < 9) {
        alignment = 8;
    }
    uint64_t rounded = (size + 7) & ~7ull;
    if (this->m_pages.size() == 0 ||
        reinterpret_cast<uint64_t>(this->m_pages.back().m_begin) + this->m_pageSize <
            ((reinterpret_cast<uint64_t>(this->m_pages.back().m_end) + 7 + alignment) & ~(alignment - 1)) + rounded) {
        void* page;
        if (this->m_freePages.size() == 0) {
            page = SMemAlloc(this->m_pageSize, 0xd, s_gxDeviceFile, SStrHashFnv1a(s_gxDeviceFile), 0x675, 0);
        } else {
            page = this->m_freePages.back();
            this->m_freePages.back() = nullptr;
            this->m_freePages.m_size--;
        }
        this->m_pages.push_back(CallbackPage { page, page });
    }
    CallbackPage& page = this->m_pages.back();
    uint64_t data = (reinterpret_cast<uint64_t>(page.m_end) + 7 + alignment) & ~(alignment - 1);
    auto header = reinterpret_cast<uint64_t*>(data - 8);
    uint64_t end = data + rounded;
    *header = (reinterpret_cast<uint64_t>(callback) & 0x0000ffffffffffffull) |
        (static_cast<uint64_t>((end - reinterpret_cast<uint64_t>(header)) >> 3) << 48);
    auto previous = static_cast<uint64_t*>(page.m_end);
    if (previous < header) {
        // A filler record over the alignment gap.
        *previous = (reinterpret_cast<uint64_t>(this->m_08) & 0x0000ffffffffffffull) |
            (static_cast<uint64_t>((reinterpret_cast<uint64_t>(header) - reinterpret_cast<uint64_t>(previous)) >> 3) << 48);
    }
    page.m_end = reinterpret_cast<void*>(end);
    return reinterpret_cast<void*>(data);
}

// Runs every record, then keeps the pages for reuse. ref: FUN_14229bdd0
void GxCallbackList::Run() {
    auto lock = static_cast<SCritSect*>(this->m_lock);
    lock->Enter();
    for (CallbackPage& page : this->m_pages) {
        auto record = static_cast<uint64_t*>(page.m_begin);
        while (record < page.m_end) {
            auto callback = reinterpret_cast<void (*)(void*)>(static_cast<int64_t>(*record << 16) >> 16);
            callback(record + 1);
            record += static_cast<uint16_t>(*record >> 48);
        }
        this->m_freePages.push_back(page.m_begin);
        page.m_begin = nullptr;
    }
    this->m_pages.m_size = 0;
    lock->Leave();
}

// Swaps the page lists with another list (FUN_1422a7b70, FUN_1422a7ee0 for the free pages).
void GxCallbackList::SwapPages(GxCallbackList* other) {
    std::swap(this->m_pages.m_data, other->m_pages.m_data);
    std::swap(this->m_pages.m_size, other->m_pages.m_size);
    std::swap(this->m_pages.m_capacity, other->m_pages.m_capacity);
    std::swap(this->m_freePages.m_data, other->m_freePages.m_data);
    std::swap(this->m_freePages.m_size, other->m_freePages.m_size);
    std::swap(this->m_freePages.m_capacity, other->m_freePages.m_capacity);
}

namespace gxDx12Backend {

// A Gx format as a resource format (DAT_142919aa0).
DXGI_FORMAT ResourceFormat(uint8_t format) {
    return static_cast<DXGI_FORMAT>(s_resourceFormats[format]);
}

}

namespace gxDx12Backend {

// A Gx format as a shader resource format (DAT_142919b30).
DXGI_FORMAT ShaderFormat(uint8_t format) {
    return static_cast<DXGI_FORMAT>(s_shaderFormats[format]);
}

}
