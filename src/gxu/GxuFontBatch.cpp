// Gxu string batches (Engine/Source/Gxu/GxuFontMain.cpp and GxuFontMiscClasses.cpp): strings
// grouped by font and drawn page by page as quads through the font pipelines.

#include <cmath>
#include <cstring>
#include <new>

#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxShader.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "gxu/GxuFont.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

namespace {

const char s_descListTag[] = ".?AV?$TSList@UBATCHEDRENDERFONTDESC@@V?$TSGetExplicitLink@UBATCHEDRENDERFONTDESC@@@@$07@@";
const char s_descTableTag[] = ".?AV?$TSHashTable@UBATCHEDRENDERFONTDESC@@VHASHKEY_PTR@@$07@@";
const char s_fontName[] = "GxuFont";
const char s_slugName[] = "GxuSlug";

// The quads a draw holds: 0x800 vertices, 0xc00 indices.
const int32_t BATCH_VERTICES = 0x800;

// The font pipelines: [outline][blend][target][multisample] and the two for the plain
// target (DAT_142e9cc40, DAT_142e9cd40); the Slug ones (DAT_142e9cd50, DAT_142e9cf50) and
// their vertex format (DAT_142e9cf70).
GxPipeline* s_pipelines[2][4][2][2];
GxPipeline* s_targetPipelines[2];
GxPipeline* s_slugPipelines[2][2][4][2][2];
GxPipeline* s_slugTargetPipelines[2][2];
void* s_slugVertexFormat;

// The depth-stencil states (DAT_142cf5e30), blends (DAT_142cf5e50) and raster states
// (DAT_142cf5e60) the pipelines are keyed with.
const uint64_t s_depthStates[4] = { 0x0000000700000020ull, 0x0000000700000015ull, 0x0000000700000011ull,
    0x000000070000003dull };
const uint16_t s_blends[4] = { 0xf000, 0xf002, 0xf003, 0xf001 };
const uint32_t s_rasterStates[2] = { 0x00010000, 0x00020001 };

}  // namespace

// ref: FUN_14068c4e0
uint64_t FontDepthState(int32_t index) {
    return s_depthStates[index];
}


// ref: FUN_14068c4c0
uint16_t FontBlend(int32_t index) {
    return s_blends[index];
}


// ref: FUN_14068c500
uint32_t FontRaster(int32_t index) {
    return s_rasterStates[index];
}

namespace {

void SetKeyTargets(GXPIPELINEKEY* key, uint32_t kind) {
    RENDERTARGETFORMATS formats = RenderTargetFormats(kind);
    std::memcpy(key->m_targetFormats, &formats, 4);
    key->m_depthFormat = formats.m_depth;
}

void SetKeyDepth(GXPIPELINEKEY* key, int32_t index) {
    uint64_t state = FontDepthState(index);
    std::memcpy(key->m_depthStencil, &state, 8);
}

// Draws the vertices written since the last flush as quads, filling the shared quad index
// buffer the first time, and opens the vertex buffer again.
// ref: FUN_14069e610
uint8_t* Flush(GxRenderContext* context, GxBuf** buf, int32_t vertices) {
    int32_t indices = (vertices / 4) * 6;
    context->BufUnlock(*buf, static_cast<uint32_t>(vertices * 0x18));
    GxBuf* quads = g_gxuQuadIndices;
    if (GxDevExists() && (quads->m_flags & 0x3000000) != 0x3000000) {
        auto index = static_cast<int16_t*>(context->BufLock(g_gxuQuadIndices));
        int16_t base = 0;
        for (int32_t i = 0; i < 0x200; i++) {
            index[0] = base;
            index[1] = static_cast<int16_t>(base + 2);
            index[2] = static_cast<int16_t>(base + 1);
            index[3] = static_cast<int16_t>(base + 2);
            index[4] = static_cast<int16_t>(base + 3);
            index[5] = static_cast<int16_t>(base + 1);
            index += 6;
            base = static_cast<int16_t>(base + 4);
        }
        context->BufUnlock(g_gxuQuadIndices, 0);
    }
    quads = g_gxuQuadIndices;
    if (!GxDevExists() || (quads->m_flags & 0x3000000) == 0x3000000) {
        GxSetVertexStream(context, *buf, 0xc);
        context->SetIndexBuffer(g_gxuQuadIndices, 1);
        context->DrawIndexed(static_cast<uint32_t>(indices), 1, 0, 0, 0, 0);
    }
    return static_cast<uint8_t*>(context->BufLock(*buf));
}

}

// The font pipelines (FUN_140688fc0): a vertex and pixel shader per outline, eight depth and
// target combinations, each with and without multisampling, and two for the plain target.
// ref: FUN_140688fc0
void GxuFontCreatePipelines() {
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_topology = 3;
    key.m_blend = FontBlend(1);
    uint32_t raster = FontRaster(0);
    std::memcpy(key.m_raster, &raster, 4);
    key.m_vertexShader = 0x43;
    key.m_pixelShader = 0x76;
    key.m_vertexFormat = 0xc;
    key.m_samplesLog2 = 0;
    for (int32_t sample = 0; sample < 2; sample++) {
        for (uint16_t outline = 0; outline < 2; outline++) {
            key.m_vsPermute = outline;
            uint16_t pixel = outline != 0 ? 6 : 0;
            static const int32_t s_depth[8] = { 0, 0, 0, 0, 0, 1, 0, 2 };
            for (int32_t entry = 0; entry < 8; entry++) {
                SetKeyDepth(&key, s_depth[entry]);
                SetKeyTargets(&key, (entry & 1) != 0 ? 6 : 0xc);
                key.m_psPermute = pixel;
                s_pipelines[outline][entry >> 1][entry & 1][sample] = GxPipelineCreate(&key, s_fontName);
            }
        }
        key.m_samplesLog2 = g_renderSettings.m_msaaSamples;
    }
    key.m_samplesLog2 = 0;
    SetKeyDepth(&key, 0);
    SetKeyTargets(&key, 0xc);
    for (uint16_t outline = 0; outline < 2; outline++) {
        key.m_vsPermute = outline;
        key.m_psPermute = static_cast<uint16_t>((outline != 0 ? 7 : 1) + g_renderSettings.m_msaaSamples);
        s_targetPipelines[outline] = GxPipelineCreate(&key, s_fontName);
    }
}

// The Slug pipelines (FUN_140689380).
extern "C" unsigned long long FUN_140689380();
// translated: FUN_140689380
void GxuFontCreateSlugPipelines() {
    __try {
        FUN_140689380();
    } __except (GenFault(0x140689380ull, GetExceptionInformation())) {
    }
}

// ref: FUN_140689970
void GxuFontDestroyPipelines(void* param) {
    (void)param;
    for (GXULINK* link = g_gxuFonts.m_next; link != &g_gxuFonts; link = link->m_next) {
        reinterpret_cast<CGxuFont*>(link)->ReleaseSlug();
    }
    for (GXULINK* link = g_gxuFonts.m_next; link != &g_gxuFonts; link = link->m_next) {
        reinterpret_cast<CGxuFont*>(link)->ClearGlyphs();
    }
    for (GXULINK* link = g_gxuStringsWithGeometry.m_next; link != &g_gxuStringsWithGeometry;) {
        GXULINK* next = link->m_next;
        reinterpret_cast<CGxString*>(link)->ClearGeometry();
        link = next;
    }
    for (auto& outline : s_pipelines) {
        for (auto& blend : outline) {
            for (auto& target : blend) {
                for (GxPipeline*& pipeline : target) {
                    GxPipelineDestroy(&pipeline);
                }
            }
        }
    }
    for (GxPipeline*& pipeline : s_targetPipelines) {
        GxPipelineDestroy(&pipeline);
    }
    for (auto& a : s_slugPipelines) {
        for (auto& b : a) {
            for (auto& c : b) {
                for (auto& d : c) {
                    for (GxPipeline*& pipeline : d) {
                        GxPipelineDestroy(&pipeline);
                    }
                }
            }
        }
    }
    for (auto& a : s_slugTargetPipelines) {
        for (GxPipeline*& pipeline : a) {
            GxPipelineDestroy(&pipeline);
        }
    }
    s_slugVertexFormat = nullptr;
    GxBufDestroy(&g_gxuQuadIndices);
}

// The quads' shared index buffer.
// ref: FUN_14069c2f0
void GxuFontCreateQuadIndices() {
    g_gxuQuadIndices = GxBufCreateSimple(1, 0, 2, 0xc00);
}

// ref: FUN_14068bfe0
GxPipeline* GxuFontPipeline(uint32_t outline, uint32_t blend, uint32_t target, uint8_t sampleMode) {
    if (target == 2) {
        return s_targetPipelines[outline];
    }
    return s_pipelines[outline][blend][target][sampleMode];
}

// ref: FUN_14068c020
GxPipeline* GxuFontSlugPipeline(uint32_t supersample, uint32_t weight, uint32_t blend, uint32_t target,
    uint8_t sampleMode) {
    if (target == 2) {
        return s_slugTargetPipelines[supersample][weight];
    }
    return s_slugPipelines[supersample][weight][blend][target][sampleMode];
}

// The projection for screen text: an orthographic view of the clip (or viewport) snapped to
// whole pixels, with the viewport narrowed to it.
// ref: FUN_1406918f0
bool GxuFontSnapProjection(GxRenderContext* context, const float* clip, const float* depth, float* matrix) {
    bool flipped = false;
    float left;
    float right;
    float bottom;
    float top;
    float zMin;
    float zMax;
    if (clip == nullptr) {
        left = context->Field<float>(0x1c);
        right = context->Field<float>(0x20);
        bottom = context->Field<float>(0x24);
        top = context->Field<float>(0x28);
        zMin = context->Field<float>(0x2c);
        zMax = context->Field<float>(0x30);
    } else {
        zMax = 1.0f;
        zMin = 0.0f;
        left = clip[1];
        right = clip[3];
        bottom = clip[0];
        top = clip[2];
        if (clip[2] < clip[0]) {
            bottom = clip[2];
            top = clip[0];
        }
        flipped = clip[2] < clip[0];
    }
    float x0 = floorf(static_cast<float>(g_gxuScreenWidth) * left);
    float x1 = floorf(static_cast<float>(g_gxuScreenWidth) * right);
    if (!(x0 < x1)) {
        return false;
    }
    float y0 = floorf(static_cast<float>(g_gxuScreenHeight) * bottom);
    float y1 = floorf(static_cast<float>(g_gxuScreenHeight) * top);
    if (!(y0 < y1)) {
        return false;
    }
    if (y1 < static_cast<float>(g_gxuScreenHeight)) {
        if (2.0f < y1 - y0) {
            float a = static_cast<float>(g_gxuScreenHeight) * bottom * 0.5f;
            float rounded = roundf(a);
            float b = static_cast<float>(g_gxuScreenHeight) * top * 0.5f;
            y0 = rounded * 2.0f;
            y1 = (rounded <= a ? floorf(b) : ceilf(b)) * 2.0f;
        }
        float view[6];
        view[0] = x0 / static_cast<float>(g_gxuScreenWidth);
        view[1] = x1 / static_cast<float>(g_gxuScreenWidth);
        view[2] = y0 / static_cast<float>(g_gxuScreenHeight);
        view[3] = y1 / static_cast<float>(g_gxuScreenHeight);
        if (view[0] < 0.0f) {
            view[0] = 0.0f;
        }
        if (1.0f < view[1]) {
            view[1] = 1.0f;
        }
        if (view[2] < 0.0f) {
            view[2] = 0.0f;
        }
        if (1.0f < view[3]) {
            view[3] = 1.0f;
        }
        view[4] = zMin;
        view[5] = zMax;
        float* current = &context->Field<float>(0x1c);
        if (std::memcmp(current, view, sizeof(view)) != 0) {
            std::memcpy(current, view, sizeof(view));
            context->Field<uint8_t>(0x18) = 1;
        }
    }
    float zNear = -1.0f;
    float zFar = 1.0f;
    if (depth != nullptr) {
        zNear = *depth - 1000.0f;
        zFar = *depth + 1000.0f;
    }
    Orthographic(x0, x1, y0, y1, zNear, zFar, matrix);
    if (flipped) {
        float center[3] = { (x1 + x0) * 0.5f, (y1 + y0) * 0.5f, 0.0f };
        MatrixTranslate(matrix, center);
        float flip[3] = { 1.0f, -1.0f, 1.0f };
        MatrixScale(matrix, flip);
        float back[3] = { -center[0], -center[1], 0.0f };
        MatrixTranslate(matrix, back);
    }
    return true;
}

// TSHashTable<BATCHEDRENDERFONTDESC, HASHKEY_PTR, 8>'s node deletion (FUN_14068c080).
// ref: FUN_14068c080
void GxuStringBatch::Delete(BATCHEDRENDERFONTDESC* node) {
    for (GXULINK* link = node->m_strings.m_next; link != &node->m_strings && link != nullptr;) {
        GXULINK* next = link->m_next;
        link->m_next = nullptr;
        link->m_prev = nullptr;
        link = next;
    }
    node->m_strings.m_next = nullptr;
    node->m_strings.m_prev = nullptr;
    this->Unlink(node);
    node->m_linktofull.Unlink();
    node->m_linktoslot.Unlink();
    SMemFree(node, s_descTableTag, -2, 0);
}

// ref: FUN_14068c190
BATCHEDRENDERFONTDESC* GxuStringBatch::NewNode(TSExplicitList<BATCHEDRENDERFONTDESC>* slot, uint32_t extra,
    uint32_t flags) {
    void* memory = SMemAlloc(extra + sizeof(BATCHEDRENDERFONTDESC), 8, s_descListTag, 0, -2, flags | 8);
    auto node = new (memory) BATCHEDRENDERFONTDESC();
    node->m_pad2c = 0;
    node->m_key = nullptr;
    node->m_font = nullptr;
    node->m_strings.InitSentinel();
    slot->LinkToTail(node);
    return node;
}

// A string into the batch, under its font's entry.
// ref: FUN_14069a8e0
void GxuStringBatch::AddString(CGxString* string) {
    CGxuFont* font = string->m_font;
    uint64_t key = reinterpret_cast<uint64_t>(font);
    uint32_t hash = 0x811c9dc5;
    for (int32_t i = 0; i < 8; i++) {
        hash = (hash ^ static_cast<uint32_t>((key >> (i * 8)) & 0xff)) * 0x1000193;
    }
    BATCHEDRENDERFONTDESC* desc = this->Find(hash, [font](BATCHEDRENDERFONTDESC* found) {
        return found->m_key == font;
    });
    if (desc == nullptr) {
        desc = this->New(hash, 0, 0);
        desc->m_hashval = hash;
        desc->m_key = font;
        desc->m_font = font;
    }
    string->m_batchLink.Unlink();
    string->m_batchLink.LinkToTail(&desc->m_strings);
}

// ref: FUN_140688f50
void GxuStringBatch::Clear() {
    this->TSHashTable<BATCHEDRENDERFONTDESC>::Clear(false);
}

// A font's strings, page by page: the page texture, then every string's quads on it.
// ref: FUN_14069cad0
void BATCHEDRENDERFONTDESC::Render(GxRenderContext* context, float* constants, uint32_t blend, uint8_t sampleMode,
    uint32_t target) {
    if (g_gxuQuadIndices == nullptr) {
        return;
    }
    for (GXULINK* link = this->m_strings.m_next; link != &this->m_strings; link = link->m_next) {
        reinterpret_cast<CGxString*>(reinterpret_cast<uint8_t*>(link) - 0x28)->CheckGeometry();
    }
    for (uint32_t i = 0; i < 8; i++) {
        TextureCacheUpload(&this->m_font->m_pages[i]);
    }
    GxBuf* buf = GxBufTransient(context, 0, 0x18, BATCH_VERTICES);
    auto vertices = static_cast<uint8_t*>(context->BufLock(buf));
    int32_t room = BATCH_VERTICES;
    context->SetPipeline(GxuFontPipeline(this->m_font->m_flags & 1, blend, target, sampleMode));
    constants[12] = 0.0f;
    constants[13] = 0.0f;
    constants[14] = 0.0f;
    constants[15] = 1.0f;
    for (int32_t page = 0; page < 8; page++) {
        TEXTURECACHE* cache = &this->m_font->m_pages[page];
        if (cache->m_texture == nullptr) {
            continue;
        }
        context->SetTexture(2, 0, static_cast<gxDx12Backend::Tex*>(cache->m_texture));
        float texScale[2] = { 1.0f / static_cast<float>(cache->m_width), 1.0f / static_cast<float>(cache->m_height) };
        for (GXULINK* link = this->m_strings.m_next; link != &this->m_strings; link = link->m_next) {
            auto string = reinterpret_cast<CGxString*>(reinterpret_cast<uint8_t*>(link) - 0x28);
            if (string->m_pages[page] == nullptr) {
                continue;
            }
            int32_t remaining = string->PageVertexCount(page);
            int32_t first = 0;
            while (remaining != 0) {
                int32_t take = remaining < room ? remaining : room;
                string->RenderPage(vertices, constants, page, first, take, texScale);
                remaining -= take;
                first += take;
                vertices += static_cast<size_t>(take) * 0x18;
                room -= take;
                if (room == 0) {
                    vertices = Flush(context, &buf, BATCH_VERTICES);
                    room = BATCH_VERTICES;
                }
            }
        }
        if (room != BATCH_VERTICES) {
            vertices = Flush(context, &buf, BATCH_VERTICES - room);
            room = BATCH_VERTICES;
        }
    }
    context->BufUnlock(buf, 0);
}

// ref: FUN_14069d5e0
void BATCHEDRENDERFONTDESC::RenderSlug(GxRenderContext* context, float* constants, uint32_t blend,
    uint8_t sampleMode, uint32_t target) {
    (void)context;
    (void)constants;
    (void)blend;
    (void)sampleMode;
    (void)target;
    FROST_UNIMPLEMENTED();
}

// Draws the batch: screen text through a pixel-snapped projection of the clip, each font's
// strings with its pipeline; entries with no strings left are dropped.
// ref: FUN_14069cde0
void GxuStringBatch::Render(GxRenderContext* context, const float* view, const float* projection,
    GXUBATCHRENDER* params) {
    for (BATCHEDRENDERFONTDESC* desc = this->m_fulllist.Head(); desc != nullptr;
         desc = this->m_fulllist.Link(desc)->Next()) {
        for (GXULINK* link = desc->m_strings.m_next; link != &desc->m_strings; link = link->m_next) {
            reinterpret_cast<CGxString*>(reinterpret_cast<uint8_t*>(link) - 0x28)->ValidateGeometry();
        }
    }
    for (BATCHEDRENDERFONTDESC* desc = this->m_fulllist.Head(); desc != nullptr;
         desc = this->m_fulllist.Link(desc)->Next()) {
        if (desc->m_strings.m_next != &desc->m_strings) {
            auto string = reinterpret_cast<CGxString*>(reinterpret_cast<uint8_t*>(desc->m_strings.m_next) - 0x28);
            if (string->m_font->UsesSlug()) {
                this->RenderSlug(context, view, projection, params);
                return;
            }
            break;
        }
    }
    float matrix[16];
    std::memcpy(matrix, projection, sizeof(matrix));
    float transform[16];
    std::memcpy(transform, view, sizeof(transform));
    bool draw = true;
    if ((this->m_flags & 1) == 0) {
        const float* clip = g_gxuHasClip != 0 ? g_gxuClip : nullptr;
        for (int32_t i = 0; i < 16; i++) {
            transform[i] = (i % 5) == 0 ? 1.0f : 0.0f;
        }
        draw = GxuFontSnapProjection(context, clip, params->m_clip, matrix);
    } else {
        g_gxuWorldText = 1;
        FROST_UNIMPLEMENTED();
    }
    uint32_t flags = this->m_flags;
    uint32_t target = 0;
    if (params->m_multisample != 0) {
        target = (params->m_mask != nullptr ? 1 : 0) + 1;
    }
    if (draw) {
        float* constants = context->LockConstants(0, 1, 0x40);
        for (int32_t row = 0; row < 4; row++) {
            for (int32_t column = 0; column < 4; column++) {
                float value = 0.0f;
                for (int32_t k = 0; k < 4; k++) {
                    value += transform[row * 4 + k] * matrix[k * 4 + column];
                }
                constants[column * 4 + row] = value;
            }
        }
        context->UnlockConstants(0, 1);
        if (params->m_mask != nullptr) {
            context->BindTexture(2, 1, static_cast<gxDx12Backend::Tex*>(params->m_mask), 0);
            context->SetSampler(2, 1, 0x800200000001ull);
        }
        for (BATCHEDRENDERFONTDESC* desc = this->m_fulllist.Head(); desc != nullptr;) {
            BATCHEDRENDERFONTDESC* next = this->m_fulllist.Link(desc)->Next();
            if (desc->m_strings.m_next == &desc->m_strings) {
                this->Unlink(desc);
                this->Delete(desc);
            } else {
                desc->Render(context, transform, flags & 3, static_cast<uint8_t>((flags >> 3) & 1), target);
                if ((this->m_flags & 4) != 0) {
                    for (GXULINK* link = desc->m_strings.m_next; link != &desc->m_strings;) {
                        GXULINK* following = link->m_next;
                        link->m_next = nullptr;
                        link->m_prev = nullptr;
                        link = following;
                    }
                    desc->m_strings.InitSentinel();
                }
            }
            desc = next;
        }
    }
    g_gxuWorldText = 0;
}

// ref: FUN_14069daf0
void GxuStringBatch::RenderSlug(GxRenderContext* context, const float* view, const float* projection,
    GXUBATCHRENDER* params) {
    (void)context;
    (void)view;
    (void)projection;
    (void)params;
    FROST_UNIMPLEMENTED();
}
