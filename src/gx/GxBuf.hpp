#ifndef GX_GX_BUF_HPP
#define GX_GX_BUF_HPP

// GxBuf (Engine/Source/Gx, 0x70 bytes): a buffer of vertices, indices or constants. Static
// buffers keep their contents in the device's buffer heaps and are copied there from the
// upload ring at unlock; dynamic ones are written straight into the upload ring each time
// they are locked, which is what the render contexts' transient buffers are.

#include <cstdint>

#include "gx/GxDevice.hpp"

enum GXBUFTYPE : uint8_t {
    GXBUF_STATIC = 0,
    GXBUF_STREAM = 1,       // re-allocated in its heap on every lock
    GXBUF_DYNAMIC = 2,      // lives in the upload ring only
};

// GxBuf::m_usage (low bits; the rest is the API's bind flags).
enum : uint32_t {
    GXBUFUSAGE_INDEX = 0x1,
    GXBUFUSAGE_VERTEX = 0x2,
    GXBUFUSAGE_CONSTANT = 0x4,
};

// GxBuf::m_flags.
enum : uint32_t {
    GXBUF_FORMAT_MASK = 0xfff,              // bits 0..11
    GXBUF_ELEMENT_SHIFT = 12,               // bits 12..23: the element size
    GXBUF_UNLOCKED = 0x1000000,
    GXBUF_WRITTEN = 0x2000000,
};

// What GxBufCreate is asked for (0x28).
struct GXBUFDESC {
    uint8_t m_type;         // 0x00 GXBUFTYPE
    uint8_t m_pad01[7];
    uint64_t m_size;        // 0x08
    uint64_t m_10;          // 0x10
    uint32_t m_usage;       // 0x18
    uint32_t m_format;      // 0x1c
    uint32_t m_elementSize; // 0x20
    uint32_t m_pad24;
};

struct GxBuf {
    uint32_t m_size;        // 0x00
    uint32_t m_usage;       // 0x04
    uint8_t m_type;         // 0x08
    uint8_t m_pad09[3];
    uint32_t m_flags;       // 0x0c
    // The registry of buffers the device recreates (0x142d43850), static and stream ones.
    GXREGISTRYLINK m_link;  // 0x10
    uint64_t m_20;          // 0x20
    void* m_resource;       // 0x28: what the GPU reads it from (an upload page's resource)
    uint32_t m_offset;      // 0x30: its offset there
    uint32_t m_pad34;
    uint64_t m_gpu;         // 0x38: its GPU virtual address
    void* m_heap;           // 0x40: the buffer heap a static buffer lives in
    uint32_t m_heapOffset;  // 0x48
    uint32_t m_heapSize;    // 0x4c
    uint8_t m_50[0x20];     // 0x50

    GxBuf(const GXBUFDESC* desc);
};
static_assert(sizeof(GxBuf) == 0x70, "GxBuf is 0x70 bytes");

GxBuf* GxBufCreate(const GXBUFDESC* desc);
void GxBufDestroy(GxBuf** buf);

#endif
