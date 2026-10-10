#include "gx/GxBuf.hpp"

#include <new>

#include "gx/Gx.hpp"
#include "gx/GxVertexFormat.hpp"
#include "gx/d3d12/CommandList.hpp"

namespace {

// The buffer itself goes once the frame that last used it is done (LAB_14228d5e0).
void GxBufFree(void* storage) {
    GxBuf* buf = *static_cast<GxBuf**>(storage);
    if (buf == nullptr) {
        return;
    }
    if (buf->m_link.m_next != nullptr) {
        buf->m_link.m_prev->m_next = buf->m_link.m_next;
        buf->m_link.m_next->m_prev = buf->m_link.m_prev;
    }
    buf->m_link.m_prev = nullptr;
    buf->m_link.m_next = nullptr;
    GxGetAllocator()->deallocate(buf);
}

}

// ref: FUN_1422905f0
GxBuf::GxBuf(const GXBUFDESC* desc) {
    this->m_flags = 0;
    this->m_size = static_cast<uint32_t>(desc->m_size);
    this->m_usage = desc->m_usage;
    this->m_type = desc->m_type;
    uint32_t flags = (this->m_flags & ~GXBUF_FORMAT_MASK) | (desc->m_format & GXBUF_FORMAT_MASK);
    this->m_flags = ((((desc->m_elementSize & 0xfff) << GXBUF_ELEMENT_SHIFT) | (flags & 0xff000fff)) & ~GXBUF_UNLOCKED) | GXBUF_WRITTEN;
    this->m_link.m_prev = nullptr;
    this->m_link.m_next = nullptr;
}

// ref: FUN_142288710
GxBuf* GxBufCreate(const GXBUFDESC* desc) {
    void* memory = GxGetAllocator()->allocate(sizeof(GxBuf), 0x10);
    GxBuf* buf = memory != nullptr ? new (memory) GxBuf(desc) : nullptr;
    if (desc->m_type != GXBUF_DYNAMIC) {
        AcquireSRWLockExclusive(&g_gxRegisteredBufsLock);
        GXREGISTRYLINK* link = &buf->m_link;
        link->m_prev = g_gxRegisteredBufs.m_prev;
        link->m_next = &g_gxRegisteredBufs;
        g_gxRegisteredBufs.m_prev = link;
        link->m_prev->m_next = link;
        ReleaseSRWLockExclusive(&g_gxRegisteredBufsLock);
    }
    static_cast<CGxDevice*>(g_gxDevice)->BufCreate(buf);
    return buf;
}

// ref: FUN_1422887c0
GxBuf* GxBufCreateFromInfo(const GXBUFINFO* info) {
    GXBUFDESC desc = {};
    desc.m_type = info->m_type;
    desc.m_size = info->m_size;
    desc.m_10 = info->m_10;
    desc.m_format = 0;
    desc.m_elementSize = 1;
    if (info->m_bind == 0) {
        desc.m_usage = 0x1802;
        if (info->m_format != 0xff) {
            desc.m_usage = 0x1a22;
            desc.m_format = g_gxVertexFormats[info->m_format].m_stride;
            desc.m_elementSize = desc.m_format;
        }
    } else if (info->m_bind == 1) {
        desc.m_usage = 0x1801;
        if (info->m_format != 0xff) {
            desc.m_usage = 0x1a41;
            desc.m_elementSize = info->m_format == 1 ? 2 : 4;
        }
    } else {
        desc.m_usage = 0x1804;
        desc.m_elementSize = 0x10;
    }
    void* memory = GxGetAllocator()->allocate(sizeof(GxBuf), 0x10);
    GxBuf* buf = memory != nullptr ? new (memory) GxBuf(&desc) : nullptr;
    if (desc.m_type != GXBUF_DYNAMIC) {
        AcquireSRWLockExclusive(&g_gxRegisteredBufsLock);
        GXREGISTRYLINK* link = &buf->m_link;
        link->m_prev = g_gxRegisteredBufs.m_prev;
        link->m_next = &g_gxRegisteredBufs;
        g_gxRegisteredBufs.m_prev = link;
        link->m_prev->m_next = link;
        ReleaseSRWLockExclusive(&g_gxRegisteredBufsLock);
    }
    static_cast<CGxDevice*>(g_gxDevice)->BufCreate(buf);
    return buf;
}

// ref: FUN_142288900
GxBuf* GxBufCreateSimple(uint8_t kind, uint8_t type, uint32_t stride, uint32_t count) {
    GXBUFDESC desc = {};
    desc.m_type = type;
    desc.m_size = static_cast<uint64_t>(stride * count);
    desc.m_format = 0;
    desc.m_elementSize = 1;
    if (kind == 0) {
        desc.m_usage = 0x1802;
    } else if (kind == 1) {
        desc.m_usage = 0x1801;
    } else {
        desc.m_usage = 0x1804;
        desc.m_elementSize = 0x10;
    }
    return GxBufCreate(&desc);
}

// ref: FUN_142288a00
GxBuf* GxBufCreateSized(uint8_t kind, uint8_t type, uint64_t size, const char* name) {
    (void)name;
    GXBUFDESC desc = {};
    desc.m_type = type;
    desc.m_size = size;
    desc.m_format = 0;
    desc.m_elementSize = 1;
    if (kind == 0) {
        desc.m_usage = 0x1802;
    } else if (kind == 1) {
        desc.m_usage = 0x1801;
    } else {
        desc.m_usage = 0x1804;
        desc.m_elementSize = 0x10;
    }
    return GxBufCreate(&desc);
}

// ref: FUN_142290c90
GxBuf* GxBufTransient(GxRenderContext* context, uint8_t type, uint32_t stride, uint32_t count) {
    GxBuf* buf = context->GetUploadBuf(type);
    buf->m_flags &= ~GXBUF_UNLOCKED;
    buf->m_size = stride * count;
    return buf;
}

// ref: FUN_14228f830
void GxSetVertexStream(GxRenderContext* context, GxBuf* buf, uint8_t format) {
    CGxDevice::VERTEXFORMATREF layouts;
    static_cast<CGxDevice*>(g_gxDevice)->GetVertexFormat(&layouts, format);
    GXVERTEXSTREAM stream;
    stream.m_buf = buf;
    stream.m_layout = layouts.m_layouts[0];
    stream.m_offset = 0;
    stream.m_size = buf->m_size;
    context->SetVertexBuffers(0, 1, &stream);
}

// ref: FUN_142288af0
void GxBufDestroy(GxBuf** handle) {
    GxBuf* buf = *handle;
    if (buf == nullptr) {
        return;
    }
    if (buf->m_type != GXBUF_DYNAMIC) {
        AcquireSRWLockExclusive(&g_gxRegisteredBufsLock);
        if (buf->m_link.m_next != nullptr) {
            buf->m_link.m_prev->m_next = buf->m_link.m_next;
            buf->m_link.m_next->m_prev = buf->m_link.m_prev;
        }
        buf->m_link.m_prev = nullptr;
        buf->m_link.m_next = nullptr;
        ReleaseSRWLockExclusive(&g_gxRegisteredBufsLock);
    }
    auto device = static_cast<CGxDevice*>(g_gxDevice);
    device->BufDestroy(buf);
    blz::function_storage callback = {};
    callback.m_invoke = reinterpret_cast<void*>(GxBufFree);
    callback.m_manager = blz::function_trivial_manager;
    *reinterpret_cast<GxBuf**>(callback.m_storage) = buf;
    device->DeferCallback(&callback);
    *handle = nullptr;
}
