#ifndef GX_GX_VERTEX_FORMAT_HPP
#define GX_GX_VERTEX_FORMAT_HPP

// Gx's vertex formats: 0x2b fixed layouts (Engine/Source/Gx) every vertex stream is described
// by, and what each becomes on a device (pmVertexBufferFormat: a stream's input layout).

#include <cstdint>

// The semantics an attribute can have (their names at 0x142913490).
enum GXVERTEXSEMANTIC : uint32_t {
    GXVA_POSITION = 0,
    GXVA_BLENDWEIGHT = 1,
    GXVA_BLENDINDICES = 2,
    GXVA_NORMAL = 3,
    GXVA_COLOR0 = 4,
    GXVA_COLOR1 = 5,
    GXVA_TEXCOORD0 = 6,
    GXVA_TEXCOORD1 = 7,
    GXVA_TEXCOORD2 = 8,
    GXVA_TEXCOORD3 = 9,
    GXVA_TEXCOORD4 = 10,
    GXVA_TEXCOORD5 = 11,
    GXVA_COUNT = 16,
};

// One attribute of a format (16 bytes).
struct GXVERTEXATTRIB {
    uint32_t m_semantic;    // GXVERTEXSEMANTIC
    uint32_t m_type;        // an index into s_gxVertexAttribFormats
    uint32_t m_offset;
    uint32_t m_stream;      // 0, or 1 for the instance stream
};

// A format (0xa0): its attributes, then what the device's constructor derives from them.
struct GXVERTEXFORMATDESC {
    const GXVERTEXATTRIB* m_attribs;                // 0x00
    uint32_t m_count;                               // 0x08
    uint32_t m_stride;                              // 0x0c
    const GXVERTEXATTRIB* m_bySemantic[GXVA_COUNT]; // 0x10
    uint32_t m_mask;                                // 0x90: the semantics present
    uint32_t m_instanceMask;                        // 0x94: those in stream 1
    uint64_t m_98;
};
static_assert(sizeof(GXVERTEXFORMATDESC) == 0xa0, "GXVERTEXFORMATDESC is 0xa0 bytes");

constexpr uint32_t GX_VERTEX_FORMAT_COUNT = 0x2b;
extern GXVERTEXFORMATDESC g_gxVertexFormats[GX_VERTEX_FORMAT_COUNT];  // 0x142d41d78

// A stream's attribute as the device sees it (pmVertexAttributeDesc, 0x20).
struct GXVERTEXATTRIBDESC {
    uint32_t m_semantic;    // 0x00
    uint8_t m_format;       // 0x04: a pm vertex format
    uint8_t m_pad05[3];
    uint32_t m_offset;      // 0x08
    uint32_t m_pad0c;
    const char* m_name;     // 0x10: the HLSL semantic name
    uint32_t m_index;       // 0x18: the HLSL semantic index
    uint32_t m_pad1c;
};

// A stream's layout as the device makes it (pmVertexBufferFormatDesc, 0x18).
struct GXVERTEXBUFFERFORMATDESC {
    uint32_t m_stream;      // 0x00
    uint32_t m_stride;      // 0x04
    uint8_t m_instanced;    // 0x08
    uint8_t m_pad09[3];
    uint32_t m_count;       // 0x0c
    const GXVERTEXATTRIBDESC* m_attribs;  // 0x10
};

// What a device keeps of a stream layout: the desc copied with its attributes and names
// behind it.
struct GXVERTEXBUFFERFORMAT {
    GXVERTEXBUFFERFORMATDESC* m_desc;   // 0x00: this + 8
    GXVERTEXBUFFERFORMATDESC m_copy;    // 0x08
    // GXVERTEXATTRIBDESC m_attribs[m_copy.m_count] at 0x20, then the names
};

// A format's stride (FUN_14228c910) and an attribute's offset in it (FUN_14228c8e0).
uint32_t GxVertexFormatStride(uint32_t format);
uint32_t GxVertexFormatOffset(uint32_t format, uint32_t semantic);

// The derived fields of every format (the end of FUN_142290650).
void GxVertexFormatsInitialize();

// The copy a device keeps of a stream layout (FUN_142290d80).
GXVERTEXBUFFERFORMAT* GxVertexBufferFormatCopy(const GXVERTEXBUFFERFORMATDESC* desc);

#endif
