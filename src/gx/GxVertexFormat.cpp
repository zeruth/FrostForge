#include "gx/GxVertexFormat.hpp"

#include <cstring>
#include <new>

#include "gx/GxDevice.hpp"

#include "gx/GxVertexFormats.inc"

// ref: FUN_14228c910
uint32_t GxVertexFormatStride(uint32_t format) {
    return g_gxVertexFormats[format].m_stride;
}

// ref: FUN_14228c8e0
uint32_t GxVertexFormatOffset(uint32_t format, uint32_t semantic) {
    return g_gxVertexFormats[format].m_bySemantic[semantic]->m_offset;
}

// The derived fields: each attribute by its semantic, the semantics present and those in the
// instance stream. CGxDevice's constructor does this over the static table.
void GxVertexFormatsInitialize() {
    for (uint32_t i = 0; i < GX_VERTEX_FORMAT_COUNT; i++) {
        GXVERTEXFORMATDESC& format = g_gxVertexFormats[i];
        std::memset(format.m_bySemantic, 0, sizeof(format.m_bySemantic));
        format.m_mask = 0;
        format.m_instanceMask = 0;
        for (uint32_t k = 0; k < format.m_count; k++) {
            const GXVERTEXATTRIB* attrib = &format.m_attribs[k];
            format.m_bySemantic[attrib->m_semantic] = attrib;
            format.m_mask |= 1u << (attrib->m_semantic & 0x1f);
            if (static_cast<uint8_t>(attrib->m_stream) == 1) {
                format.m_instanceMask |= 1u << (attrib->m_semantic & 0x1f);
            }
        }
    }
}

// One block: the pointer to the desc, the desc, its attributes, then their names.
// ref: FUN_142290d80
GXVERTEXBUFFERFORMAT* GxVertexBufferFormatCopy(const GXVERTEXBUFFERFORMATDESC* desc) {
    uint64_t size = (static_cast<uint64_t>(desc->m_count) + 1) * 0x20;
    uint64_t nameOffsets[64] = {};
    for (uint32_t i = 0; i < desc->m_count && i < 64; i++) {
        const char* name = desc->m_attribs[i].m_name;
        if (name != nullptr) {
            nameOffsets[i] = size;
            size += std::strlen(name) + 1;
        }
    }
    auto block = static_cast<uint8_t*>(GxGetAllocator()->allocate(size, 0x10));
    auto format = reinterpret_cast<GXVERTEXBUFFERFORMAT*>(block);
    format->m_desc = nullptr;
    auto attribs = reinterpret_cast<GXVERTEXATTRIBDESC*>(block + 0x20);
    for (uint32_t i = 0; i < desc->m_count; i++) {
        attribs[i] = desc->m_attribs[i];
    }
    format->m_copy = *desc;
    format->m_desc = &format->m_copy;
    format->m_copy.m_attribs = attribs;
    for (uint32_t i = 0; i < desc->m_count && i < 64; i++) {
        const char* name = desc->m_attribs[i].m_name;
        if (name == nullptr) {
            continue;
        }
        auto copy = reinterpret_cast<char*>(block + nameOffsets[i]);
        uint64_t room = size - nameOffsets[i];
        uint64_t length = std::strlen(name);
        if (room != 0) {
            if (room - 1 < length) {
                length = room - 1;
            }
            std::memcpy(copy, name, length);
            copy[length] = '\0';
        }
        attribs[i].m_name = copy;
    }
    return format;
}
