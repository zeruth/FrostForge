#include "gx/GxBuf.hpp"

#include <new>

#include "gx/Gx.hpp"

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
