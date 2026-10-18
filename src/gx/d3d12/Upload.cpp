#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxFormat.hpp"
#include "gx/GxPlatform.hpp"
#include "images/Images.hpp"
#include <cstring>

// Texture and buffer contents from the CPU: written into the upload ring and copied into the
// resource on a command list. The device's upload interface does it on the copy queue at once;
// the graphics list does it in the frame.

using namespace gxDx12Backend;

CGxDeviceD3d12* GxDeviceD3d12();

DataUploadPage* NextUploadPage(CopyCommandList* list);
bool TrackState(CopyCommandList* list, D3d12Object* object, uint32_t state);
void* BufLockList(CopyCommandList* list, GxBuf* buf);
void BufUnlockUpload(CopyCommandList* list, GxBuf* buf, uint32_t size);

namespace {

const uint32_t PAGE_SIZE = 0x200000;

D3DX12Residency::ManagedObject* TexResidency(Tex* tex) {
    if (tex->m_residency.m_underlying != nullptr) {
        return &tex->m_residency;
    }
    if (tex->m_heap != nullptr) {
        return &tex->m_heap->m_residency;
    }
    return nullptr;
}

void CopyRegion(CopyCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dest, uint32_t x, uint32_t y, uint32_t z,
    const D3D12_TEXTURE_COPY_LOCATION* source) {
    list->m_list->CopyTextureRegion(dest, x, y, z, source, nullptr);
}

// The pixel converter on one chunk, from the texture's data format to its resource format.
void ConvertChunk(Tex* tex, uint32_t columns, uint32_t rows, const uint8_t* source, uint32_t sourcePitch, uint8_t* dest,
    uint32_t destPitch) {
    int32_t rect[2] = { static_cast<int32_t>(columns), static_cast<int32_t>(rows) };
    uint32_t destFormat = GxFormatPixelFormat(tex->m_format);
    uint32_t sourceFormat = GxFormatPixelFormat(tex->m_09);
    PixelConvert(rect, 0, source, sourcePitch, static_cast<int32_t>(sourceFormat), dest, destPitch,
        static_cast<int32_t>(destFormat));
}

}

namespace gxDx12Backend {

// The texture's residency object joins the list's residency set.
// ref: FUN_1422d90c0
void CopyCommandList::AddResidency(uint32_t index, Tex* tex) {
    D3DX12Residency::ManagedObject* object = TexResidency(tex);
    if (object != nullptr) {
        this->m_residencySets[index]->Insert(object);
    }
}

// One level of one face (or every slice of a volume level) from `data` into the upload ring and
// onto the texture: whole slices when they fit what is left of a page, otherwise as many rows
// as fit, a page at a time. `rect` is {top, left, bottom, right} at level 0.
// ref: FUN_1422e33a0
void CopyCommandList::UploadSubresource(Tex* tex, const int32_t* rect, uint32_t levels, uint32_t face, uint32_t mip,
    uint32_t width, uint32_t height, uint32_t depth, const uint8_t* data, uint32_t pitch) {
    (void)width;
    (void)depth;  // (the slice count comes from the texture, tex->m_depth, as in the reference)
    if (GxDeviceD3d12()->m_ready == 0) {
        return;
    }
    uint32_t top = static_cast<uint32_t>(rect[0]) >> (mip & 0x1f);
    uint32_t left = static_cast<uint32_t>(rect[1]) >> (mip & 0x1f);
    uint32_t bottom = static_cast<uint32_t>(rect[2]) >> (mip & 0x1f);
    uint32_t right = static_cast<uint32_t>(rect[3]) >> (mip & 0x1f);
    uint32_t slices = static_cast<uint32_t>(static_cast<int32_t>(tex->m_depth)) >> (mip & 0x1f);
    if (slices == 0) {
        slices = 1;
    }
    uint32_t columns = (right <= left + 1 ? left + 1 : right) - left;
    uint32_t rows = (bottom <= top + 1 ? top + 1 : bottom) - top;
    bool block = static_cast<uint8_t>(tex->m_09 - 7) < 5;
    if (block) {
        uint32_t leftOver = left & 3;
        left &= ~3u;
        columns = (columns + 3 + leftOver) & ~3u;
        if (columns < 5) {
            columns = 4;
        }
        uint32_t topOver = top & 3;
        top &= ~3u;
        rows = (topOver + 3 + rows) & ~3u;
        if (rows < 5) {
            rows = 4;
        }
    }
    uint32_t rowCount = rows;
    if ((tex->m_usage & 0x4000000) == 0) {
        // The data is the whole level: start at the rectangle.
        uint32_t column;
        uint32_t row;
        if (!block) {
            column = GxFormatBits(tex->m_09) * left >> 3;
            row = top;
        } else {
            column = (left >> 2) * GxFormatBlockBytes(tex->m_09);
            row = top >> 2;
        }
        data += static_cast<uint64_t>(column) + static_cast<uint64_t>(row * pitch);
    }
    uint32_t shift = block ? 2 : 0;
    uint32_t bits = GxFormatBits(tex->m_format);
    uint32_t end = rows + top;
    uint32_t sliceStride = (height >> shift) * pitch;
    uint32_t unaligned = (bits * (columns << shift) >> 3) + 0xff;
    uint32_t rowPitch = unaligned & 0xffffff00;
    uint32_t sliceBytes = (rows >> shift) * rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = tex->m_objects[0].m_resource;
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.SubresourceIndex = levels * face + mip;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Footprint.Format = ResourceFormat(tex->m_format);
    source.PlacedFootprint.Footprint.Width = columns;
    source.PlacedFootprint.Footprint.RowPitch = rowPitch;
    this->m_empty = 0;

    DataUploadPage* page = this->m_page;
    if (sliceBytes <= PAGE_SIZE) {
        if (slices == 0) {
            return;
        }
        uint32_t slice = 0;
        uint32_t rowCursor = top;
        const uint8_t* partial = data;
        uint32_t done;
        if (page != nullptr) {
            goto check;
        }
        do {
            page = NextUploadPage(this);
            if (page == nullptr) {
                return;
            }
        check:
            if (page->m_cpu == nullptr) {
                return;
            }
            source.pResource = page->m_resource;
            if (rowCursor != top) {
                // The rest of a slice the last page could not hold. The rows go to the start of
                // the page (it is new, so its cursor is 0 there anyway).
                source.PlacedFootprint.Offset = page->m_cursor;
                source.PlacedFootprint.Footprint.Depth = 1;
                uint32_t left2 = end - rowCursor;
                uint8_t* cpu = page->m_cpu;
                page->m_begin = page->m_cursor;
                page->m_cursor = (left2 >> shift) * rowPitch + page->m_cursor;
                source.PlacedFootprint.Footprint.Height = left2;
                ConvertChunk(tex, columns, left2, partial, pitch, cpu, rowPitch);
                CopyRegion(this, &dest, left, rowCursor, slice, &source);
                slice++;
                data += sliceStride;
                rowCursor = top;
                if (slices <= slice) {
                    return;
                }
            }
            page->m_cursor = (page->m_cursor + 0x1ff) & 0xfffffe00;
            done = slice;
            {
                uint32_t fit = static_cast<uint32_t>((static_cast<uint64_t>(PAGE_SIZE) - page->m_cursor) / sliceBytes);
                uint32_t count = slices - slice;
                if (fit < count) {
                    count = fit;
                }
                if (count != 0) {
                    source.PlacedFootprint.Footprint.Height = rowCount;
                    source.PlacedFootprint.Offset = page->m_cursor;
                    source.PlacedFootprint.Footprint.Depth = count;
                    for (uint32_t i = 0; i < count; i++) {
                        uint32_t begin = page->m_cursor;
                        page->m_begin = begin;
                        uint8_t* cpu = page->m_cpu;
                        page->m_cursor = begin + sliceBytes;
                        ConvertChunk(tex, columns, rowCount, data, pitch, cpu + begin, rowPitch);
                        data += sliceStride;
                    }
                    CopyRegion(this, &dest, left, top, slice, &source);
                    done = slice + count;
                    slice = done;
                    if (slices <= done) {
                        return;
                    }
                }
            }
            page->m_cursor = (page->m_cursor + 0x1ff) & 0xfffffe00;
            {
                uint32_t fitRows = static_cast<uint32_t>((static_cast<uint64_t>(PAGE_SIZE - page->m_cursor) << shift) / rowPitch)
                    & static_cast<uint32_t>(-(1 << shift));
                if (fitRows != 0) {
                    source.PlacedFootprint.Footprint.Depth = 1;
                    uint32_t begin = page->m_cursor;
                    uint8_t* cpu = page->m_cpu;
                    page->m_begin = begin;
                    page->m_cursor = (fitRows >> shift) * rowPitch + begin;
                    source.PlacedFootprint.Offset = begin;
                    source.PlacedFootprint.Footprint.Height = fitRows;
                    ConvertChunk(tex, columns, fitRows, data, pitch, cpu + begin, rowPitch);
                    partial = data + static_cast<uint64_t>(pitch * (fitRows >> shift));
                    CopyRegion(this, &dest, left, top, slice, &source);
                    rowCursor += fitRows;
                    done = slice;
                }
            }
        } while (done < slices);
        return;
    }

    // A slice larger than a page: rows a page at a time.
    source.PlacedFootprint.Footprint.Depth = 1;
    for (uint32_t slice = 0; slice < slices; slice++) {
        if (top < end) {
            uint32_t row = top;
            const uint8_t* rowData = data;
            if (page != nullptr) {
                goto checkLarge;
            }
            do {
                page = NextUploadPage(this);
                if (page == nullptr) {
                    return;
                }
            checkLarge:
                if (page->m_cpu == nullptr) {
                    return;
                }
                page->m_cursor = (page->m_cursor + 0x1ff) & 0xfffffe00;
                uint32_t fitRows = static_cast<uint32_t>((static_cast<uint64_t>(PAGE_SIZE - page->m_cursor) << shift) / rowPitch)
                    & static_cast<uint32_t>(-(1 << shift));
                uint32_t count = end - row;
                if (fitRows < count) {
                    count = fitRows;
                }
                if (count != 0) {
                    source.pResource = page->m_resource;
                    source.PlacedFootprint.Offset = page->m_cursor;
                    uint32_t begin = page->m_cursor;
                    uint8_t* cpu = page->m_cpu;
                    page->m_begin = begin;
                    page->m_cursor = (count >> shift) * rowPitch + begin;
                    source.PlacedFootprint.Footprint.Height = count;
                    ConvertChunk(tex, columns, count, rowData, pitch, cpu + begin, rowPitch);
                    rowData += static_cast<uint64_t>((count >> shift) * pitch);
                    CopyRegion(this, &dest, left, row, slice, &source);
                }
                page = nullptr;
                row += count;
            } while (row < end);
        }
        data += sliceStride;
    }
}

// Every level of every face, from what `callback` hands out for each.
// ref: FUN_1422e3b90
void CopyCommandList::UploadTex(Tex* tex, const int32_t* rect, GXTEXUPDATEPROC callback, void* param) {
    uint32_t faces = tex->m_dimension == 1 ? 6 : 1;
    uint8_t levels = tex->m_mipLevels;
    for (uint32_t face = 0; face < faces; face++) {
        uint32_t depth = tex->m_depth;
        uint32_t height = tex->m_height;
        uint32_t width = tex->m_width;
        for (uint32_t mip = 0; mip < levels; mip++) {
            const void* data = nullptr;
            uint32_t pitch;
            uint32_t slice = tex->m_dimension == 3 ? depth : face;
            callback(param, width, height, slice, mip, &pitch, &data);
            if (data != nullptr && (tex->m_usage & 0x40000) == 0) {
                this->UploadSubresource(tex, rect, levels, face, mip, width, height, depth,
                    static_cast<const uint8_t*>(data), pitch);
            }
            width = width >> 1 < 2 ? 1 : width >> 1;
            height = height >> 1 < 2 ? 1 : height >> 1;
            depth = depth >> 1 < 2 ? 1 : depth >> 1;
        }
    }
}

// In the frame: the texture to COPY_DEST, its levels uploaded, and with `shaderResource` on to
// the shader-resource states and no longer tracked.
// ref: FUN_1422e2d50
bool CopyCommandList::UpdateTex(Tex* tex, const int32_t* rect, bool shaderResource, GXTEXUPDATEPROC callback,
    void* param) {
    if (GxDeviceD3d12()->m_ready == 0) {
        return false;
    }
    this->FlushBarriers();
    this->AddResidency(this->m_current, tex);
    for (D3d12Object& object : tex->m_objects) {
        object.m_untracked = 0;
        TrackState(this, &object, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    this->FlushBarriers();
    uint32_t faces = tex->m_dimension == 1 ? 6 : 1;
    uint8_t levels = tex->m_mipLevels;
    for (uint32_t face = 0; face < faces; face++) {
        uint32_t width = tex->m_width;
        uint32_t height = tex->m_height;
        uint32_t depth = tex->m_depth;
        for (uint32_t mip = 0; mip < levels; mip++) {
            uint32_t slice = tex->m_dimension == 3 ? depth : face;
            const void* data = nullptr;
            uint32_t pitch;
            callback(param, width, height, slice, mip, &pitch, &data);
            if (data != nullptr && (tex->m_usage & 0x40000) == 0) {
                this->UploadSubresource(tex, rect, levels, face, mip, width, height, depth,
                    static_cast<const uint8_t*>(data), pitch);
            }
            width = width >> 1 < 2 ? 1 : width >> 1;
            height = height >> 1 < 2 ? 1 : height >> 1;
            depth = depth >> 1 < 2 ? 1 : depth >> 1;
        }
    }
    if (shaderResource) {
        for (D3d12Object& object : tex->m_objects) {
            if (object.m_untracked == 0) {
                TrackState(this, &object, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            object.m_untracked = 1;
        }
    }
    return true;
}

}

// The upload interface's texture update: recorded on the copy queue's list and run at once. The
// texture goes back to COMMON on the copy queue; with `shaderResource` the frame's upload list
// moves it to the shader-resource states, after which it is no longer tracked.
// ref: FUN_14230a460
void CGxDeviceD3d12::TexUpload(Tex* tex, const int32_t* rect, bool shaderResource, GXTEXUPDATEPROC callback,
    void* param) {
    if (this->m_ready == 0) {
        return;
    }
    GxDeviceCallLock lock;
    CopyCommandList* copy = this->m_computePtr;
    copy->Reset();
    for (D3d12Object& object : tex->m_objects) {
        D3D12_RESOURCE_BARRIER& barrier = copy->m_barriers.emplace_back();
        std::memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = object.m_resource;
        barrier.Transition.Subresource = object.m_subresource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    copy->FlushBarriers();
    copy->AddResidency(copy->m_current, tex);
    copy->UploadTex(tex, rect, callback, param);
    CopyCommandList* upload = this->m_copyPtr;
    for (D3d12Object& object : tex->m_objects) {
        D3D12_RESOURCE_BARRIER& barrier = copy->m_barriers.emplace_back();
        std::memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = object.m_resource;
        barrier.Transition.Subresource = object.m_subresource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        // The upload list learns the object is in COMMON (its starting state unknown).
        bool inserted = false;
        StateTracker* tracker = upload->m_states[0].insert(reinterpret_cast<uint64_t>(&object), &inserted);
        if (inserted) {
            tracker->m_first = 0xffffffff;
            tracker->m_current = 0;
        }
        if (shaderResource) {
            if (object.m_untracked == 0) {
                TrackState(upload, &object, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            object.m_untracked = 1;
        }
    }
    copy->FlushBarriers();
    upload->AddResidency(upload->m_current, tex);
    copy->FlushBarriers();
    copy->CloseList(0);
    ID3D12CommandList* list = copy->m_lists[0];
    HRESULT hr = this->m_residency.ExecuteCommandLists(this->m_copyQueue.m_queue, &list, copy->m_residencySets.data(), 1);
    if (FAILED(hr)) {
        this->Fail(hr, "ExecuteSingle failed");
    }
    this->m_copyQueue.Signal();
}

// A buffer's contents from a callback, through the copy queue's list.
// ref: FUN_14230a350
void CGxDeviceD3d12::BufUpload(GxBuf* buf, uint32_t size, GXBUFUPDATEPROC callback, void* param) {
    if (this->m_ready == 0) {
        return;
    }
    GxDeviceCallLock lock;
    void* data = BufLockList(this->m_computePtr, buf);
    callback(param, data);
    BufUnlockUpload(this->m_computePtr, buf, size);  // FUN_1422cd9d0
    buf->m_flags |= GXBUF_UNLOCKED;
}

// ref: FUN_14230a3d0
void CGxDeviceD3d12::BufWrite(GxBuf* buf, const void* source, uint32_t size) {
    if (this->m_ready == 0) {
        return;
    }
    GxDeviceCallLock lock;
    void* data = BufLockList(this->m_computePtr, buf);
    std::memcpy(data, source, size);
    BufUnlockUpload(this->m_computePtr, buf, size);  // FUN_1422cd9d0
    buf->m_flags |= GXBUF_UNLOCKED;
}

// ref: FUN_1422ccf40
uint64_t Queue::Signal() {
    if (this->m_queue != nullptr && this->m_fence != nullptr) {
        this->m_fenceValue++;
        HRESULT hr = this->m_queue->Signal(this->m_fence, this->m_fenceValue);
        if (FAILED(hr)) {
            this->m_fenceValue--;
            GxDeviceD3d12()->Fail(hr, "AddFenceToQueue failed");
        }
    }
    this->m_frameFences[GxDeviceD3d12()->m_frameIndex] = this->m_fenceValue;
    return this->m_fenceValue;
}

// ref: FUN_1422e3390
bool RenderContextD3d12::UpdateTex(Tex* tex, const int32_t* rect, bool shaderResource, GXTEXUPDATEPROC callback,
    void* param) {
    return this->Owner()->m_copy.UpdateTex(tex, rect, shaderResource, callback, param);
}
