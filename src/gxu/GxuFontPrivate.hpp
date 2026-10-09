#ifndef GXU_GXU_FONT_PRIVATE_HPP
#define GXU_GXU_FONT_PRIVATE_HPP

// The Gxu font module's own types (Engine/Source/Gxu/GxuFont*.cpp): fonts and their glyph
// caches, faces, strings and the batches strings draw through, laid out as the reference lays
// them out.

#include <cstdint>

#include "base/Handle.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "storm/list/TSLink.hpp"

struct FT_FaceRec_;
struct GxBuf;
struct GxPipeline;
class GxRenderContext;
struct CGxString;
class CGxuFont;

// The fields a hash table node starts with (TSHashObject), declared in each node so the key
// that follows packs at +0x2c as the reference packs it.
#define GXU_HASH_OBJECT(T)              \
    TSLink<T> m_linktoslot;             \
    TSLink<T> m_linktofull;             \
    void* m_table = nullptr;            \
    uint32_t m_hashval = 0;

// A plain intrusive link with a sentinel: the font list, the string lists, a font's strings
// and a batch entry's strings.
struct GXULINK {
    GXULINK* m_next = nullptr;
    GXULINK* m_prev = nullptr;

    void InitSentinel() {
        this->m_next = this;
        this->m_prev = this;
    }
    // Leaves the list it is in, if any.
    void Unlink() {
        if (this->m_next != nullptr) {
            this->m_prev->m_next = this->m_next;
            this->m_next->m_prev = this->m_prev;
        }
        this->m_next = nullptr;
        this->m_prev = nullptr;
    }
    // Joins `list` at its tail.
    void LinkToTail(GXULINK* list) {
        this->m_prev = list->m_prev;
        this->m_next = list;
        list->m_prev = this;
        this->m_prev->m_next = this;
    }
};

// blz::vector as the module stores it: data, size, and the capacity with bit 63 set while the
// storage is not owned.
template <class T>
struct GXUVECTOR {
    T* m_data = nullptr;        // 0x00
    uint64_t m_size = 0;        // 0x08
    uint64_t m_capacity = 0;    // 0x10

    bool Owned() const {
        return static_cast<int64_t>(this->m_capacity) >= 0;
    }
    uint64_t Capacity() const {
        return this->m_capacity & 0x7fffffffffffffffull;
    }
};

// A glyph quad's vertex (GxVertexPT, 0x14): position and texture coordinate.
struct GXVERTEXPT {
    float m_x, m_y, m_z;        // 0x00
    float m_u, m_v;             // 0x0c
};

// A string's vertices on one glyph page (0x30).
struct GXUPAGEGEOMETRY {
    GXUVECTOR<GXVERTEXPT> m_vertices;   // 0x00
    GXUVECTOR<uint32_t> m_colors;       // 0x18: CArgb per vertex
};

// What a string remembers per gradient glyph (GradientInfo, 8): the page and the vertex.
struct GXUGRADIENTINFO {
    uint8_t m_page;
    uint8_t m_pad[3];
    uint32_t m_vertex;
};

// A hyperlink's extent in a string (GxuFontHyperlinkInfo, 0x40).
struct GXUHYPERLINKINFO {
    float m_rect[4];            // 0x00: left, top, right, bottom (normalized)
    const char* m_link;         // 0x10
    int32_t m_linkLength;       // 0x18
    int32_t m_pad1c;
    const char* m_text;         // 0x20
    int32_t m_textLength;       // 0x28
    int32_t m_pad2c;
    const char* m_color;        // 0x30
    int32_t m_colorLength;      // 0x38
    int32_t m_pad3c;
};

// An embedded texture or atlas (|T and |A) parsed out of a string, and as the string keeps it
// (GxuEmbeddedTextureInfo, 0x58; kept with the rectangle and sizes normalized).
struct GXUEMBEDDEDTEXTURE {
    float m_rect[4];            // 0x00: left, top, right, bottom
    const char* m_path;         // 0x10
    int32_t m_pathLength;       // 0x18
    int32_t m_atlasId;          // 0x1c
    const char* m_atlas;        // 0x20
    int32_t m_atlasLength;      // 0x28
    int32_t m_fileDataId;       // 0x2c
    float m_height;             // 0x30
    float m_width;              // 0x34
    float m_offsetX;            // 0x38
    float m_offsetY;            // 0x3c
    float m_texCoords[4];       // 0x40: top, left, bottom, right
    uint8_t m_vertexColor[4];   // 0x50: b, g, r, a
    uint8_t m_hasTexCoords;     // 0x54
    uint8_t m_hasVertexColor;   // 0x55
    uint8_t m_pad56[2];
};
static_assert(sizeof(GXUEMBEDDEDTEXTURE) == 0x58, "GXUEMBEDDEDTEXTURE is 0x58 bytes");

// The running state of hyperlink and texture parsing while a line is laid out (the 0xa0-byte
// block WriteGeometry is handed).
struct GXULINKSTATE {
    int32_t m_state;            // 0x00: 0 none, 1 in a link, 2 a link with a texture
    int32_t m_pad04;
    float m_rect[4];            // 0x08: top, left, bottom, right in pixels
    const char* m_link;         // 0x18
    int32_t m_linkLength;       // 0x20
    int32_t m_pad24;
    const char* m_text;         // 0x28
    int32_t m_textLength;       // 0x30
    int32_t m_pad34;
    const char* m_color;        // 0x38
    int32_t m_colorLength;      // 0x40
    int32_t m_pad44;
    GXUEMBEDDEDTEXTURE m_texture;  // 0x48
};
static_assert(sizeof(GXULINKSTATE) == 0xa0, "GXULINKSTATE is 0xa0 bytes");

// ---- faces -----------------------------------------------------------------------------------

// A loaded face (FACEDATA, "HFACE", 0x50): the font file's bytes and the FreeType face over
// them, in the face table by file data id.
struct FACEDATA : public CHandleObject {
    GXU_HASH_OBJECT(FACEDATA)               // 0x10
    uint32_t m_fileDataId = 0;  // 0x3c
    void* m_data = nullptr;     // 0x40
    FT_FaceRec_* m_face = nullptr;  // 0x48

    ~FACEDATA() override;
};
static_assert(sizeof(FACEDATA) == 0x50, "FACEDATA is 0x50 bytes");

template <>
struct TSHashTableTags<FACEDATA> {
    static constexpr const char* s_fixedArray = ".?AV?$TSFixedArray@V?$TSExplicitList@UFACEDATA@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray = ".?AV?$TSGrowableArray@V?$TSExplicitList@UFACEDATA@@$0?CCCCCCCD@$07@@$07@@";
};

// ---- glyphs ----------------------------------------------------------------------------------

// What the glyph renderer hands back (0x30): the bitmap and its metrics.
struct GXUGLYPHBITMAP {
    uint8_t* m_data = nullptr;  // 0x00
    float m_advance = 0.0f;     // 0x08
    float m_bearing = 0.0f;     // 0x0c
    uint32_t m_pitch = 0;       // 0x10
    uint32_t m_width = 0;       // 0x14
    uint32_t m_rows = 0;        // 0x18
    uint32_t m_cellWidth = 0;   // 0x1c: the width with the outline
    int32_t m_yOffset = 0;      // 0x20: rows of padding above the glyph
    int32_t m_yOffsetBelow = 0; // 0x24
    uint64_t m_pad28 = 0;
};

// A cached glyph (CharCodeDesc, 0x88 followed by its bitmap at 4 bits per pixel, or 8 with
// an outline): in the font's glyph table by code, in its row of a page by position, and in
// the font's recently-used list.
struct CharCodeDesc {
    GXU_HASH_OBJECT(CharCodeDesc)
    uint32_t m_code;            // 0x2c (the HASHKEY_NONE key is the hash itself)
    TSLink<CharCodeDesc> m_rowLink;     // 0x30
    TSLink<CharCodeDesc> m_recentLink;  // 0x40
    float m_texCoords[4];       // 0x50: the texel rectangle {row x0, u0, row x1, u1}
    uint32_t m_glyphCode;       // 0x60
    uint32_t m_page;            // 0x64
    uint32_t m_row;             // 0x68
    uint32_t m_top;             // 0x6c
    uint32_t m_bottom;          // 0x70
    uint32_t m_width;           // 0x74
    int32_t m_yOffset;          // 0x78
    float m_advance;            // 0x7c
    float m_bearing;            // 0x80
    uint32_t m_pad84;           // 0x84; the bitmap follows at 0x88

    uint8_t* Bitmap() {
        return reinterpret_cast<uint8_t*>(this) + 0x88;
    }
};
static_assert(sizeof(CharCodeDesc) == 0x88, "CharCodeDesc is 0x88 bytes before its bitmap");

template <>
struct TSHashTableTags<CharCodeDesc> {
    static constexpr const char* s_fixedArray = ".?AV?$TSFixedArray@V?$TSExplicitList@UCharCodeDesc@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray = ".?AV?$TSGrowableArray@V?$TSExplicitList@UCharCodeDesc@@$0?CCCCCCCD@$07@@$07@@";
};

// TSHashTable<CharCodeDesc, HASHKEY_NONE, 8> (vtable 0x1426d29b0).
class CHARCODETABLE : public TSHashTable<CharCodeDesc> {
    public:
    void Delete(CharCodeDesc* node) override;
    CharCodeDesc* NewNode(TSExplicitList<CharCodeDesc>* slot, uint32_t extra, uint32_t flags) override;
};

// A kerning pair's step (KernNode, 0x38), keyed by the pair.
struct KernNode {
    GXU_HASH_OBJECT(KernNode)
    uint32_t m_pair;            // 0x2c: previous << 16 | current
    float m_step;               // 0x30
    uint32_t m_pad34;
};
static_assert(sizeof(KernNode) == 0x38, "KernNode is 0x38 bytes");

template <>
struct TSHashTableTags<KernNode> {
    static constexpr const char* s_fixedArray = ".?AV?$TSFixedArray@V?$TSExplicitList@UKernNode@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray = ".?AV?$TSGrowableArray@V?$TSExplicitList@UKernNode@@$0?CCCCCCCD@$07@@$07@@";
};

// TSHashTable<KernNode, KERNINGHASHKEY, 8> (vtable 0x1426d2c48, 0x50).
class KERNTABLE : public TSHashTable<KernNode> {
    public:
    void Delete(KernNode* node) override;
    KernNode* NewNode(TSExplicitList<KernNode>* slot, uint32_t extra, uint32_t flags) override;
};

// blz::unordered_set<uint32_t> as a font keeps the codes it evicted (0x20).
struct GXUCODESET {
    uint64_t m_bucketCount = 8;     // 0x00
    void** m_buckets = nullptr;     // 0x08
    uint64_t m_size = 0;            // 0x10
    float m_maxLoad = 1.0f;         // 0x18
};

// A strip of a glyph page (TEXTURECACHEROW, 0x20): its glyphs in order along it.
struct TEXTURECACHEROW {
    uint32_t m_largestGap;      // 0x00
    uint32_t m_pad04;
    TSExplicitList<CharCodeDesc> m_glyphs;  // 0x08, link offset 0x30
};
static_assert(sizeof(TEXTURECACHEROW) == 0x20, "TEXTURECACHEROW is 0x20 bytes");

// One of a font's eight glyph textures (TEXTURECACHE, 0x38): rows of `cell` texels, packed as
// layers of four bits (two with an outline) in two-byte texels.
struct TEXTURECACHE {
    void* m_texture;            // 0x00: GxTex
    CGxuFont* m_font;           // 0x08
    uint8_t m_dirty;            // 0x10
    uint8_t m_pad11[3];
    uint32_t m_width;           // 0x14: along a row
    uint32_t m_height;          // 0x18
    uint32_t m_layers;          // 0x1c
    // TSFixedArray<TEXTURECACHEROW, 8>
    uint32_t m_rowCount;        // 0x20
    uint32_t m_pad24;
    TEXTURECACHEROW* m_rows;    // 0x28
    uint32_t m_rowCapacity;     // 0x30
    uint32_t m_pad34;
};
static_assert(sizeof(TEXTURECACHE) == 0x38, "TEXTURECACHE is 0x38 bytes");

// blz::shared_ptr<SlugRuntimeData>.
struct GXUSLUGPTR {
    void* m_ptr = nullptr;
    void* m_control = nullptr;
};

// A font: a face at one pixel size, its glyph cache and the strings that use it (0x3b0).
class CGxuFont {
    public:
    GXULINK m_link;             // 0x000: the font list
    uint32_t m_lastFrame;       // 0x010
    uint32_t m_pad014;
    GXULINK m_strings;          // 0x018: CGxString::m_fontLink
    CHARCODETABLE m_glyphs;     // 0x028
    KERNTABLE* m_kerning;       // 0x078
    GXUCODESET* m_evicted;      // 0x080
    GXUCODESET* m_evictedPrevious;  // 0x088
    float* m_kernCache;         // 0x090: 0x5f * 0x5f printable pairs, -1 until known
    // The glyphs by use, the most recent first (link offset 0x40).
    TSExplicitList<CharCodeDesc> m_recent;  // 0x098
    CHandleObject* m_face;      // 0x0b0: HFACE
    char m_path[0x104];         // 0x0b8
    int32_t m_fileDataId;       // 0x1bc
    GXUSLUGPTR m_slug;          // 0x1c0
    uint32_t m_cellSize;        // 0x1d0: the pixel size plus the outline, at most 256
    uint32_t m_baseline;        // 0x1d4
    uint32_t m_flags;           // 0x1d8: 1 outline, 2 monochrome, 4 scalable (slug),
                                //        8 thick outline, 0x10 exact size, 0x20 new, 0x40 ...
    float m_requestedHeight;    // 0x1dc
    float m_unitScale;          // 0x1e0: pixels per font unit
    uint32_t m_pad1e4;
    TEXTURECACHE m_pages[8];    // 0x1e8
    uint32_t m_pixelSize;       // 0x3a8
    uint32_t m_pad3ac;

    CGxuFont();                 // FUN_140688300
    ~CGxuFont();                // FUN_1406974a0

    bool InitializeById(int32_t fileDataId, uint32_t flags, float height);  // FUN_1406992b0
    bool InitializeByName(const char* path, uint32_t flags, float height);   // FUN_140699330
    bool Initialize();          // FUN_140699d40
    bool SetSize();             // FUN_14069a2b0
    void Destroy();             // FUN_140697dc0
    void ClearGlyphs();         // FUN_140697f10
    void ReleaseSlug();         // FUN_140698500
    void Rebuild();             // FUN_1406991f0
    void SwapEvicted();         // FUN_14069a460

    CharCodeDesc* GetGlyph(uint32_t code);              // FUN_140699940
    float GetAdvance(uint32_t code);                    // FUN_140698930
    float GetKernedAdvance(uint32_t previous, uint32_t code);   // FUN_1406985a0
    float GetMonoAdvance(uint32_t previous, uint32_t code);     // FUN_140698860
    float GetBearing(CharCodeDesc* glyph, bool scaled, float height);  // FUN_1406989b0
    uint32_t GetPixelSize() const {                     // FUN_140698a20
        return this->m_pixelSize;
    }
    bool HasGlyphs(const char* text);                   // FUN_140697c10
    void NoteEvicted(uint32_t code);                    // FUN_140697b00
    void* SlugData();                                   // FUN_140698a50
    void* SlugGlyphs();                                 // FUN_140698a30
    bool UsesSlug();                                    // FUN_140699240
    void PrepareSlug();                                 // FUN_1406993e0
};
static_assert(sizeof(CGxuFont) == 0x3b0, "CGxuFont is 0x3b0 bytes");

// ---- strings ---------------------------------------------------------------------------------

// The Slug path's geometry (0x38): vertices of 0x44 bytes and 16-bit indices.
struct GXUSLUGGEOMETRY {
    GXUVECTOR<uint8_t> m_vertices;  // 0x00: SlugVertex2D, 0x44 each
    GXUVECTOR<uint16_t> m_indices;  // 0x18
    uint8_t m_valid;            // 0x30
    uint8_t m_pad31[7];
};

// A laid-out string (CGxString, 0x1a0).
struct CGxString {
    GXULINK m_link;             // 0x000: all strings, or the ones holding geometry
    int32_t m_lineCount;        // 0x010
    uint32_t m_lastFrame;       // 0x014
    GXULINK m_fontLink;         // 0x018
    GXULINK m_batchLink;        // 0x028
    GXUPAGEGEOMETRY* m_pages[8];    // 0x038
    GXUSLUGGEOMETRY m_slug;     // 0x078
    float m_scale;              // 0x0b0
    float m_rotation;           // 0x0b4
    float m_requestedHeight;    // 0x0b8
    float m_height;             // 0x0bc
    float m_position[3];        // 0x0c0
    uint32_t m_color;           // 0x0cc: CImVector (b, g, r, a)
    uint32_t m_shadowColor;     // 0x0d0
    float m_shadowOffset[2];    // 0x0d4
    float m_width;              // 0x0dc
    float m_maxHeight;          // 0x0e0
    uint32_t m_pade4;
    CGxuFont* m_font;           // 0x0e8
    // blz::string
    char* m_text;               // 0x0f0
    uint64_t m_textLength;      // 0x0f8
    uint64_t m_textCapacity;    // 0x100
    char m_textBuffer[0x10];    // 0x108
    int32_t m_justifyV;         // 0x118: 0 top, 1 middle, 2 bottom
    int32_t m_justifyH;         // 0x11c: 0 left, 1 center, 2 right
    float m_spacing;            // 0x120
    uint32_t m_flags;           // 0x124
    uint32_t m_pageMask;        // 0x128
    uint8_t m_dirty;            // 0x12c
    uint8_t m_pad12d[3];
    int32_t m_gradientStart;    // 0x130
    int32_t m_gradientLength;   // 0x134
    int32_t m_glyphCount;       // 0x138
    float m_drawPosition[3];    // 0x13c
    float m_textScale;          // 0x148
    float m_overflow;           // 0x14c
    int32_t m_script;           // 0x150
    uint32_t m_pad154;
    GXUVECTOR<GXUHYPERLINKINFO> m_hyperlinks;   // 0x158
    GXUVECTOR<GXUEMBEDDEDTEXTURE> m_textures;   // 0x170
    GXUVECTOR<GXUGRADIENTINFO> m_gradients;     // 0x188

    CGxString();                // the creation's initialization (FUN_14068a7e0)
    ~CGxString();               // FUN_140688ae0

    bool Initialize(float height, const float* position, float width, float maxHeight, CGxuFont* font,
        const char* text, int32_t justifyV, int32_t justifyH, float spacing, uint32_t flags, const uint32_t* color,
        float textScale, float scale, float rotation);  // FUN_140694760
    void ClearGeometry();       // FUN_1406936c0
    bool CheckGeometry();       // FUN_140693600
    void ValidateGeometry();    // FUN_140693670
    void BuildGeometry();       // FUN_140693a10
    void WriteGeometry(const char* text, int32_t length, GXUVECTOR<uint32_t>* colors, float* pen, uint32_t* pages,
        GXULINKSTATE* link, bool slug);                 // FUN_140694910
    void ComputePosition();     // FUN_140695c70
    void SetColor(const uint32_t* color);               // FUN_140695e20
    void SetShadow(const float* offset, const uint32_t* color);  // FUN_140693570
    int32_t SetGradient(int32_t start, int32_t length); // FUN_140695e70
    void SetPosition(const float* position);            // FUN_1406960c0
    void PageChanged(uint32_t page);                    // FUN_1406960e0
    void ReleaseIfIdle();       // FUN_140696100
    void Recalculate();         // FUN_1406946e0
    int32_t PageVertexCount(int32_t page);              // FUN_1406935e0
    void RenderPage(uint8_t* vertices, const float* transform, int32_t page, int32_t first, int32_t count,
        const float* texScale);                         // FUN_140696130
    void RenderSlug(uint8_t* vertices, uint16_t* indices, const float* transform, int32_t firstVertex,
        int32_t vertexCount, int32_t firstIndex, int32_t indexCount, int32_t baseVertex);  // FUN_1406961f0
    int32_t GetHyperlinks(GXUHYPERLINKINFO** links);    // FUN_140694680
    int32_t GetTextures(GXUEMBEDDEDTEXTURE** textures); // FUN_140694620
};
static_assert(sizeof(CGxString) == 0x1a0, "CGxString is 0x1a0 bytes");

// ---- batches ---------------------------------------------------------------------------------

// A font's entry in a batch (BATCHEDRENDERFONTDESC, 0x50): its strings.
struct BATCHEDRENDERFONTDESC {
    GXU_HASH_OBJECT(BATCHEDRENDERFONTDESC)
    uint32_t m_pad2c;
    CGxuFont* m_key;            // 0x30
    CGxuFont* m_font;           // 0x38
    GXULINK m_strings;          // 0x40: CGxString::m_batchLink

    void Render(GxRenderContext* context, float* constants, uint32_t blend, uint8_t sampleMode,
        uint32_t target);       // FUN_14069cad0
    void RenderSlug(GxRenderContext* context, float* constants, uint32_t blend, uint8_t sampleMode,
        uint32_t target);       // FUN_14069d5e0
};
static_assert(sizeof(BATCHEDRENDERFONTDESC) == 0x50, "BATCHEDRENDERFONTDESC is 0x50 bytes");

template <>
struct TSHashTableTags<BATCHEDRENDERFONTDESC> {
    static constexpr const char* s_fixedArray =
        ".?AV?$TSFixedArray@V?$TSExplicitList@UBATCHEDRENDERFONTDESC@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray =
        ".?AV?$TSGrowableArray@V?$TSExplicitList@UBATCHEDRENDERFONTDESC@@$0?CCCCCCCD@$07@@$07@@";
};

// The parameters a batch is drawn with (the 0x18 bytes GxuStringBatchRender builds).
struct GXUBATCHRENDER {
    const float* m_clip;        // 0x00
    uint8_t m_multisample;      // 0x08
    uint8_t m_pad09[7];
    void* m_mask;               // 0x10
};

// A batch of strings drawn together, grouped by font (0x58: TSHashTable<BATCHEDRENDERFONTDESC,
// HASHKEY_PTR, 8>, vtable 0x1426d29d0, then the batch's flags).
class GxuStringBatch : public TSHashTable<BATCHEDRENDERFONTDESC> {
    public:
    uint32_t m_flags = 0;       // 0x50: 1 world (no pixel snap), 2, 4 clear after drawing, 8

    void Delete(BATCHEDRENDERFONTDESC* node) override;
    BATCHEDRENDERFONTDESC* NewNode(TSExplicitList<BATCHEDRENDERFONTDESC>* slot, uint32_t extra,
        uint32_t flags) override;

    void AddString(CGxString* string);                  // FUN_14069a8e0
    void Render(GxRenderContext* context, const float* view, const float* projection,
        GXUBATCHRENDER* params);                        // FUN_14069cde0
    void RenderSlug(GxRenderContext* context, const float* view, const float* projection,
        GXUBATCHRENDER* params);                        // FUN_14069daf0
    void Clear();               // FUN_140688f50
};
static_assert(sizeof(GxuStringBatch) == 0x58, "GxuStringBatch is 0x58 bytes");

// ---- module state ----------------------------------------------------------------------------

extern GXULINK g_gxuFonts;              // 0x142cf5de0
extern GXULINK g_gxuStrings;            // 0x142cf5df0
extern GXULINK g_gxuStringsWithGeometry;  // 0x142cf5e00
extern void* g_gxuFreeTypeLibrary;      // 0x142e9cfa8 (FT_Library)
extern uint32_t g_gxuScreenWidth;       // 0x142e9cf98
extern uint32_t g_gxuScreenHeight;      // 0x142e9cf9c
extern float g_gxuLineGap;              // 0x142e9cfa0
extern float g_gxuIndent;               // 0x142e9cfa4
extern GxBuf* g_gxuQuadIndices;         // 0x142e9cfd8
extern uint8_t g_gxuWorldText;          // 0x142e9cfe0
extern uint8_t g_gxuHasClip;            // 0x142e9cfe1
extern float g_gxuClip[4];              // 0x142cf5f10
extern float g_gxuPadding[4];           // 0x142cf5f00

// The control code at `text` (FUN_140689b30): its kind (0 a character, 1 |c colour, 2 |r,
// 3 a newline, 4 ||, 5 |H, 6 |h, 7 |T, 8 |t, 9 |K, 10 |k, 11 |W, 12 |w, 13 |A, 14 |a), its
// length in *advance, the colour of a |c in *color and the character in *code.
int32_t GxuDetermineControlCode(const char* text, int32_t* advance, uint32_t* color, uint32_t flags, uint32_t* code);

// The pipeline a batch entry draws with (FUN_14068bfe0, FUN_14068c020 for Slug).
GxPipeline* GxuFontPipeline(uint32_t outline, uint32_t blend, uint32_t target, uint8_t sampleMode);
GxPipeline* GxuFontSlugPipeline(uint32_t supersample, uint32_t weight, uint32_t blend, uint32_t target,
    uint8_t sampleMode);

// Layout (GxuFontUtil.cpp).
float GxuFontPixelHeight(int32_t pixels);           // FUN_140691810
float GxuFontRoundHeight(bool scaled, float height);    // FUN_140691830
float GxuFontRoundWidth(bool scaled, float width);      // FUN_140691890
float GxuFontGetOneToOneHeight(CGxuFont* font);     // FUN_14068b180
uint32_t GxuFontComputePixelSize(FT_FaceRec_* face, float height, uint32_t flags);  // FUN_140697b70
int32_t GxuFontMissingScript(const char* text, CGxuFont* font, int32_t* missing);  // FUN_14068fb60
void* GxuFontFreeTypeAlloc(void* memory, long size);    // FUN_14068fcb0
void GxuFontFreeTypeFree(void* memory, void* block);    // FUN_14068fd10
void* GxuFontFreeTypeRealloc(void* memory, long currentSize, long newSize, void* block);  // FUN_14068fd30
float GxuFontScaledAdvance(uint32_t code, CGxuFont* font, float height, uint32_t flags, const char* next);  // FUN_14068fda0
bool GxuFontCanBreakBetween(uint32_t previous, uint32_t code);  // FUN_14068f500
int32_t GxuFontScriptOf(uint32_t code);             // FUN_14068fc50
void GxuFontGetLineExtent(CGxuFont* font, const char* text, float height, float width, int32_t* length, float* extent,
    const char** next, float indent, uint32_t flags, uint8_t* wrapped, float* textureHeight, float scale);  // FUN_14068eac0
int32_t GxuFontGetTextExtentWithPositions(CGxuFont* font, const char* text, float height, float width,
    uint32_t length, float* extent, uint32_t flags, float indent, float scale, int32_t* consumed,
    float* positions);                              // FUN_14068ff00
void GxuFontGetWrappedExtent(CGxuFont* font, const char* text, uint32_t length, float height, float* extent,
    uint32_t flags, float indent, float scale);     // FUN_1406903a0
bool GxuFontParseAtlasCode(const char* text, GXUEMBEDDEDTEXTURE* texture, float lineHeight, float scale,
    float height);                                  // FUN_1406907c0
bool GxuFontParseTextureCode(const char* text, GXUEMBEDDEDTEXTURE* texture, float lineHeight, float scale,
    float height);                                  // FUN_140690da0
bool GxuFontSnapProjection(GxRenderContext* context, const float* clip, const float* depth, float* matrix);  // FUN_1406918f0

// Faces (GxuFontFace.cpp).
CHandleObject* GxuFontFaceCreateById(uint32_t fileDataId, void* library);   // FUN_140692320
CHandleObject* GxuFontFaceCreateByName(const char* path, void* library);    // FUN_140692480
FT_FaceRec_* GxuFontFaceGetFace(CHandleObject* face);   // FUN_140692310
FT_FaceRec_* GxuFontFaceFindById(uint32_t fileDataId);  // FUN_140692680
FT_FaceRec_* GxuFontFaceFindByName(const char* path);   // FUN_140692740
bool GxuFontFaceExistsById(uint32_t fileDataId);    // FUN_140692840
bool GxuFontFaceExistsByName(const char* path);     // FUN_140692900
void GxuFontFaceCloseAll();     // FUN_1406922a0

// Glyph rendering (IGxuFontGlyph.cpp).
bool IGxuFontGlyphRender(FT_FaceRec_* face, uint32_t pixelSize, uint32_t code, int32_t baseline,
    GXUGLYPHBITMAP* bitmap, bool monochrome, int32_t outline);  // FUN_1406a0d30
void IGxuFontGlyphRenderMissing(uint32_t pixelSize, GXUGLYPHBITMAP* bitmap, bool monochrome, int32_t outline);  // FUN_1406a0fb0

// Glyph cache (GxuFontMiscClasses.cpp).
CharCodeDesc* GxuFontNewGlyph(GXUGLYPHBITMAP* bitmap, CGxuFont* font, uint32_t code);  // FUN_14069bcb0
CharCodeDesc* TextureCacheRowAllocate(TEXTURECACHEROW* row, uint32_t code, GXUGLYPHBITMAP* bitmap, uint32_t rowIndex,
    uint32_t cellSize, uint32_t length, uint32_t rowsPerLayer, CGxuFont* font);  // FUN_14069bdd0
void TextureCacheRowEvict(TEXTURECACHEROW* row, CharCodeDesc* glyph, GXUGLYPHBITMAP* bitmap, uint32_t length,
    CGxuFont* font);            // FUN_14069c330
void TextureCacheCreate(TEXTURECACHE* cache, bool scalable, uint32_t cellSize);   // FUN_1406a0570
void TextureCacheSetRows(TEXTURECACHE* cache, CGxuFont* font, uint32_t cellSize, uint32_t layers);  // FUN_1406a0660
CharCodeDesc* TextureCacheAllocate(TEXTURECACHE* cache, uint32_t code, GXUGLYPHBITMAP* bitmap, CGxuFont* font);  // FUN_1406a0400
void TextureCacheUpload(TEXTURECACHE* cache);       // FUN_1406a0cc0
void TextureCacheClear(TEXTURECACHE* cache);        // the clears in FUN_140697f10

// Slug (GxuFontSlug.cpp).
bool GxuSlugEnabled();          // UseSlug, FUN_1406a2590
bool GxuSlugSupersampling();    // FUN_1406a25f0
bool GxuSlugOpticalWeight();    // FUN_1406a25c0
bool GxuSlugForceGenerate();    // FUN_1406a2560
void GxuSlugLoadById(GXUSLUGPTR* out, int32_t fileDataId, uint32_t flags);  // FUN_1406a18f0
void GxuSlugLoadByName(GXUSLUGPTR* out, const char* path, uint32_t flags);  // FUN_1406a1ca0
void GxuSlugRelease(GXUSLUGPTR* slug);
void GxuSlugGetTextures(void* slug, void** curves, void** bands);    // FUN_1406a1cf0
// The Slug path's per-string pieces (FUN_14069f530, FUN_14069f870, FUN_14069f010).
void GxuSlugCountGeometry(CGxString* string, float* width, int32_t* glyphs);
void GxuSlugWriteGlyph(void* desc, GXUSLUGGEOMETRY* geometry);
void GxuSlugFillGeometry(GXUSLUGGEOMETRY* geometry, uint8_t* vertices, uint16_t* indices, const float* transform,
    int32_t firstVertex, int32_t vertexCount, int32_t firstIndex, int32_t indexCount, int32_t baseVertex);
void GxuSlugMoveGeometry(GXUSLUGGEOMETRY* to, GXUSLUGGEOMETRY* from);  // FUN_140696f60

#endif
