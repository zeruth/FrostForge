#include "ui/CRenderBatch.hpp"
#include "base/Coordinate.hpp"

#include "base/Matrix.hpp"

#include <algorithm>
#include <cstring>
#include <new>

#include "base/DataAllocator.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/GxWindow.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "gxu/GxuFont.hpp"
#include "os/Sync.hpp"
#include "shadereffect/PipelineMap.hpp"
#include "texture/Texture.hpp"
#include "ui/CSimpleFrame.hpp"

const char s_renderableTag[] = ".?AUCSimpleRenderable@@";
const char s_subBatchTag[] = ".?AVCSimpleRenderablesSubBatch@@";

uint16_t g_renderBatchBlends[5];

namespace {

const char s_frameBufferNodeTag[] = ".?AU?$chained_hash_node@U?$pair@QEBVCSimpleFrame@@VCachedFrameBuffer@@@blz@@@blz@@";
const char s_frameBufferBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@QEBVCSimpleFrame@@VCachedFrameBuffer@@@blz@@@blz@@";
const char s_stringNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CB_KPEAUGxuStringBatch@@@blz@@@blz@@";
const char s_stringBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CB_KPEAUGxuStringBatch@@@blz@@@blz@@";

// The screen in UI units (DAT_142ccb9f4, DAT_142ccb9f8).
const float& UI_WIDTH = g_coordinateSpace.m_diagonalWidth;
const float& UI_HEIGHT = g_coordinateSpace.m_diagonalHeight;

// The batches' pool (0x142e97ba0, made by the static initializer 0x1401df6e0).
CDataAllocator s_batchHeap(0x68, 100);

// The geometry pipelines: the cache read without a lock (DAT_142cce868) and the ones built since
// the last frame start (DAT_142cce888), behind DAT_142e97b98.
blz::chained_hash_header s_pipelines;
blz::chained_hash_header s_newPipelines;
OSMUTEX s_pipelineLock = { SRWLOCK_INIT };

// The frame-buffer blit (DAT_142e97bc8).
GxPipeline* s_blitPipeline;

// The geometry's depth-stencil and raster state (DAT_142cf5e30, DAT_142cf5e60).
const uint64_t s_depthStencil = 0x700000020ull;
const uint32_t s_raster = 0x10000;

// CachedFrameBuffer: a frame that draws into a target of its own, by frame (DAT_142cce8a8).
struct FRAMEBUFFERNODE {
    FRAMEBUFFERNODE* m_next;
    CSimpleFrame* m_key;
    FRAMEBUFFER m_buffer;

    FRAMEBUFFERNODE() {
        std::memset(&this->m_buffer, 0, sizeof(this->m_buffer));
        for (FRAMEBUFFERTARGET& target : this->m_buffer.m_color) {
            target.m_renderTarget = 1;
        }
        this->m_buffer.m_depth.m_renderTarget = 1;
        std::memset(this->m_buffer.m_colorEntries, 0xff, sizeof(this->m_buffer.m_colorEntries));
        this->m_buffer.m_depthEntry = 0xff;
    }
};
static_assert(sizeof(FRAMEBUFFERNODE) == 0x100, "a cached frame buffer node is 0x100 bytes");
blz::chained_hash_header s_frameBuffers;

// The frame buffer the batches are drawing into, null for the screen (DAT_142e97bd0).
FRAMEBUFFER* s_boundFrameBuffer;

// chained_hash_node<pair<uint64_t const, GxuStringBatch*>> (0x18 bytes).
struct STRINGBATCHNODE {
    STRINGBATCHNODE* m_next;
    uint64_t m_key;
    GxuStringBatch* m_batch;
};

// ref: FUN_14061e6c0
FRAMEBUFFER* CachedFrameBuffer(CSimpleFrame* frame) {
    bool inserted;
    return &blz::chained_insert<FRAMEBUFFERNODE>(&s_frameBuffers, frame, &inserted, s_frameBufferNodeTag,
        s_frameBufferBucketTag)->m_buffer;
}

// Whether `inner`'s frame buffer is drawn into `outer` (or `outer` is the screen) (FUN_1406171d0).
// ref: FUN_1406171d0
bool FrameBufferNestedIn(CSimpleFrame* inner, CSimpleFrame* outer) {
    if (inner == nullptr || inner == outer) {
        return false;
    }
    while (inner != nullptr) {
        if (outer == nullptr) {
            return true;
        }
        if (inner->m_parent == nullptr) {
            return false;
        }
        inner = inner->m_parent->m_frameBuffer;
        if (inner == outer) {
            return true;
        }
    }
    return false;
}

void* GxTexOf(CTexture* texture) {
    return texture != nullptr ? TextureGetGxTex(texture) : nullptr;
}

uint32_t TextureCount(const CSimpleRenderable& renderable) {
    return renderable.m_type == CSimpleRenderable::RENDERABLE_GEOMETRY ? renderable.m_extraTextures + 1u : 1u;
}

// The draw order of renderables: frame buffers inside others first, then by frame buffer, draw
// order and type, then by what would change the pipeline or textures.
bool RenderableLess(const CSimpleRenderable& a, const CSimpleRenderable& b) {
    CSimpleFrame* bufferA = a.m_frame->m_frameBuffer;
    CSimpleFrame* bufferB = b.m_frame->m_frameBuffer;
    if (FrameBufferNestedIn(bufferA, bufferB)) {
        return true;
    }
    if (FrameBufferNestedIn(bufferB, bufferA)) {
        return false;
    }
    if (bufferA != bufferB) {
        return reinterpret_cast<uintptr_t>(bufferA) < reinterpret_cast<uintptr_t>(bufferB);
    }
    if (a.m_drawOrder != b.m_drawOrder) {
        return a.m_drawOrder < b.m_drawOrder;
    }
    if (a.m_type != b.m_type) {
        return a.m_type < b.m_type;
    }
    switch (a.m_type) {
    case CSimpleRenderable::RENDERABLE_GEOMETRY: {
        if (a.m_extraTextures != b.m_extraTextures) {
            return a.m_extraTextures < b.m_extraTextures;
        }
        if (a.m_desaturation != b.m_desaturation) {
            return a.m_desaturation < b.m_desaturation;
        }
        if (a.m_snappingBias != b.m_snappingBias) {
            return a.m_snappingBias < b.m_snappingBias;
        }
        if (a.m_blend != b.m_blend) {
            return a.m_blend < b.m_blend;
        }
        uint32_t count = TextureCount(a);
        for (uint32_t i = 0; i < count; i++) {
            auto texA = reinterpret_cast<uintptr_t>(TextureGetGxTex(a.m_textures[i]));
            auto texB = reinterpret_cast<uintptr_t>(TextureGetGxTex(b.m_textures[i]));
            if (texA != texB) {
                return texA < texB;
            }
        }
        return false;
    }
    case CSimpleRenderable::RENDERABLE_STRINGS:
    case CSimpleRenderable::RENDERABLE_CALLBACK:
    case CSimpleRenderable::RENDERABLE_BATCH:
    case CSimpleRenderable::RENDERABLE_FRAME_BUFFER:
        return reinterpret_cast<uintptr_t>(a.m_strings) < reinterpret_cast<uintptr_t>(b.m_strings);
    default:
        return false;
    }
}

// The view times the projection, transposed for the shaders.
void ViewProjection(const float* view, const float* projection, float* out) {
    for (uint32_t column = 0; column < 4; column++) {
        for (uint32_t row = 0; row < 4; row++) {
            out[column * 4 + row] = view[row * 4 + 3] * projection[12 + column] + view[row * 4 + 2] * projection[8 + column]
                + view[row * 4 + 1] * projection[4 + column] + view[row * 4] * projection[column];
        }
    }
}

void Identity(float* m) {
    std::memset(m, 0, 0x40);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

// A clip rect and whether it applies.
struct CLIPRECT {
    CRect m_rect;
    uint8_t m_clipped;
};

// What drawing a clipped batch changes and puts back: the rect, the viewport it maps to, the
// matrices it replaced and the viewport it found (on the stack in FUN_14061f160).
struct CLIPSTATE {
    uint8_t m_active;               // 0x00
    uint8_t m_empty;                // 0x01 nothing of the batch shows
    uint8_t m_pad02[2];
    CRect m_rect;                   // 0x04 UI units
    CRect m_viewport;               // 0x14 normalised
    float m_projection[16];         // 0x24 saved
    float m_view[16];               // 0x64 saved
    uint32_t m_padA4;
    float* m_viewOut;               // 0xa8
    float* m_projectionOut;         // 0xb0
    float* m_viewProjectionOut;     // 0xb8
    float m_savedViewport[6];       // 0xc0
};

// The viewport and matrices for the clip rect.
// ref: FUN_14061eb10
void ClipApply(CLIPSTATE* state, float* view, float* projection, float* viewProjection) {
    if (state->m_active == 0) {
        return;
    }
    GxXformSetViewport(state->m_viewport.minX, state->m_viewport.maxX, state->m_viewport.minY, state->m_viewport.maxY,
        0.0f, 1.0f);
    float offset[2] = { 0.0f, 0.0f };
    UiComputeMatrices(&state->m_rect, offset, view, projection);
    ViewProjection(view, projection, viewProjection);
}

// ref: FUN_14061dde0
CLIPSTATE* ClipBegin(CLIPSTATE* state, CLIPRECT* clip, float* view, float* projection, float* viewProjection) {
    state->m_active = 0;
    state->m_empty = 0;
    std::memset(&state->m_rect, 0, sizeof(state->m_rect));
    std::memset(&state->m_viewport, 0, sizeof(state->m_viewport));
    std::memcpy(state->m_projection, projection, sizeof(state->m_projection));
    std::memcpy(state->m_view, view, sizeof(state->m_view));
    state->m_viewOut = view;
    state->m_projectionOut = projection;
    state->m_viewProjectionOut = viewProjection;
    if (clip->m_clipped == 0) {
        clip->m_clipped = 0;
        return state;
    }
    GxRenderContext* context = g_gxRenderContext;
    for (uint32_t i = 0; i < 6; i++) {
        state->m_savedViewport[i] = context->Field<float>(0x1c + i * 4);
    }
    state->m_viewport = clip->m_rect;
    float viewportMinX = context->Field<float>(0x1c) * UI_WIDTH;
    float viewportMaxX = context->Field<float>(0x20) * UI_WIDTH;
    float viewportMinY = context->Field<float>(0x24) * UI_HEIGHT;
    float viewportMaxY = context->Field<float>(0x28) * UI_HEIGHT;
    CRect& viewport = state->m_viewport;
    viewport.maxX = viewportMaxX <= viewport.maxX ? viewportMaxX : viewport.maxX;
    viewport.minX = viewportMinX <= viewport.minX ? viewport.minX : viewportMinX;
    viewport.maxY = viewport.maxY <= viewportMaxY ? viewport.maxY : viewportMaxY;
    viewport.minY = viewport.minY <= viewportMinY ? viewportMinY : viewport.minY;
    if (viewport.maxX <= viewport.minX || viewport.maxY <= viewport.minY) {
        state->m_empty = 1;
        clip->m_clipped = 0;
        return state;
    }
    state->m_rect = viewport;
    // The reference computes these matrices and drops them.
    float unusedView[16];
    float unusedProjection[16];
    Identity(unusedView);
    Identity(unusedProjection);
    float offset[2] = { 0.0f, 0.0f };
    UiComputeMatrices(&viewport, offset, unusedView, unusedProjection);
    float minX = viewport.minX / UI_WIDTH;
    viewport.minX = minX;
    float minY = viewport.minY / UI_HEIGHT;
    viewport.minY = minY;
    float maxX = viewport.maxX / UI_WIDTH;
    viewport.maxX = maxX;
    float maxY = viewport.maxY / UI_HEIGHT;
    viewport.maxY = maxY;
    if (minX < 0.0f) {
        maxX = maxX - minX;
        viewport.minX = 0.0f;
        minX = 0.0f;
        viewport.maxX = maxX;
    }
    if (1.0f < maxX) {
        float over = maxX - 1.0f;
        viewport.maxX = 1.0f;
        minX = minX - over;
        viewport.minX = minX;
        maxX = 1.0f;
    }
    if (minY < 0.0f) {
        viewport.minY = 0.0f;
        maxY = maxY - minY;
        minY = 0.0f;
        viewport.maxY = maxY;
    }
    if (1.0f < maxY) {
        viewport.maxY = 1.0f;
        minY = minY - (maxY - 1.0f);
        viewport.minY = minY;
        maxY = 1.0f;
    }
    float left = minX <= 0.0f ? 0.0f : minX;
    float right = 1.0f <= maxX ? 1.0f : maxX;
    float bottom = minY <= 0.0f ? 0.0f : minY;
    float top = 1.0f <= maxY ? 1.0f : maxY;
    if (left < right && bottom < top) {
        state->m_active = 1;
        ClipApply(state, view, projection, viewProjection);
    } else {
        state->m_empty = 1;
    }
    clip->m_clipped = 0;
    return state;
}

// ref: FUN_14061e3a0
void ClipEnd(CLIPSTATE* state) {
    if (state->m_active == 0) {
        return;
    }
    std::memcpy(state->m_projectionOut, state->m_projection, sizeof(state->m_projection));
    std::memcpy(state->m_viewOut, state->m_view, sizeof(state->m_view));
    ViewProjection(state->m_viewOut, state->m_projectionOut, state->m_viewProjectionOut);
    GxXformSetViewport(state->m_savedViewport[0], state->m_savedViewport[1], state->m_savedViewport[2],
        state->m_savedViewport[3], state->m_savedViewport[4], state->m_savedViewport[5]);
}

// The vertices of a run: position, colour (BGRA turned RGBA), the first texture coordinates
// (moved onto the atlas page when the texture lives on one) and `extra` more; the indices
// rebased onto each renderable's first vertex.
// ref: FUN_14061d110 (no extra), FUN_14061cba0 (one), FUN_14061cd60 (two), FUN_14061cf20 (three)
void FillVertices(CSimpleRenderable* renderables, const CSimpleRenderablesSubBatch* range, uint8_t* vertices,
    uint16_t* indices, uint32_t extra, uint32_t stride) {
    uint16_t base = 0;
    for (uint32_t i = 0; i < range->m_count; i++) {
        const CSimpleRenderable& r = renderables[range->m_first + i];
        for (uint32_t v = 0; v < r.m_vertexCount; v++) {
            uint8_t* out = vertices + static_cast<uint64_t>(v) * stride;
            std::memcpy(out, r.m_positions + v * 3, 0xc);
            const uint8_t* color = r.m_colors + ((static_cast<uint32_t>(r.m_colorStep) * v) >> 2) * 4;
            out[0xc] = color[2];
            out[0xd] = color[1];
            out[0xe] = color[0];
            out[0xf] = color[3];
            const float* texCoord = r.m_texCoords + v * 2;
            if (r.m_atlas == 0) {
                std::memcpy(out + 0x10, texCoord, 8);
            } else {
                float coords[2] = { r.m_atlasScale * texCoord[0] + r.m_atlasOffset[0],
                    r.m_atlasScale * texCoord[1] + r.m_atlasOffset[1] };
                std::memcpy(out + 0x10, coords, 8);
            }
            for (uint32_t e = 0; e < extra; e++) {
                std::memcpy(out + 0x18 + e * 8, r.m_extraTexCoords[e] + v * 2, 8);
            }
        }
        vertices += static_cast<uint64_t>(r.m_vertexCount) * stride;
        for (uint32_t k = 0; k < r.m_indexCount; k++) {
            indices[k] = static_cast<uint16_t>(base + r.m_indices[k]);
        }
        base = static_cast<uint16_t>(base + r.m_vertexCount);
        indices += r.m_indexCount;
    }
}

// ref: FUN_14061ee70
void OnDeviceCreated(void* param) {
    (void)param;
    g_renderBatchBlends[0] = 0xf000;
    g_renderBatchBlends[2] = 0xf002;
    g_renderBatchBlends[1] = 0xf001;
    g_renderBatchBlends[3] = 0xf003;
    g_renderBatchBlends[4] = 0xf004;
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_psPermute = 2;
    key.m_topology = 4;
    uint32_t raster = 0x10000;
    std::memcpy(key.m_raster, &raster, 4);
    uint64_t depthStencil = 0x700000020ull;
    std::memcpy(key.m_depthStencil, &depthStencil, 8);
    RENDERTARGETFORMATS formats = RenderTargetFormats(0xc);
    std::memcpy(key.m_targetFormats, &formats, 4);
    key.m_depthFormat = formats.m_depth;
    key.m_vertexShader = 6;
    key.m_pixelShader = 0x3d;
    key.m_blend = 0xf002;
    s_blitPipeline = GxPipelineCreate(&key, "UI FrameBuffer Blit");
}

// ref: FUN_14061efa0
void OnDeviceDestroyed(void* param) {
    (void)param;
    PipelineMapDestroyPipelines(&s_pipelines);
    PipelineMapClear(&s_pipelines);
    PipelineMapDestroyPipelines(&s_newPipelines);
    PipelineMapClear(&s_newPipelines);
    GxPipelineDestroy(&s_blitPipeline);
}

}

// ---- CSimpleRenderable --------------------------------------------------------------------------

CSimpleRenderable::CSimpleRenderable() {
    std::memset(static_cast<void*>(this), 0, sizeof(*this));
}

CSimpleRenderable::CSimpleRenderable(const CSimpleRenderable& other) {
    for (CTexture* texture : other.m_textures) {
        if (texture != nullptr) {
            TextureAddRef(texture);
        }
    }
    std::memcpy(static_cast<void*>(this), &other, sizeof(*this));
}

CSimpleRenderable::CSimpleRenderable(CSimpleRenderable&& other) noexcept {
    std::memcpy(static_cast<void*>(this), &other, sizeof(*this));
    std::memset(other.m_textures, 0, sizeof(other.m_textures));
}

CSimpleRenderable& CSimpleRenderable::operator=(const CSimpleRenderable& other) {
    if (this != &other) {
        for (CTexture* texture : other.m_textures) {
            if (texture != nullptr) {
                TextureAddRef(texture);
            }
        }
        for (CTexture*& texture : this->m_textures) {
            CTexture* old = texture;
            if (old != nullptr) {
                TextureFree(&old);
            }
        }
        std::memcpy(static_cast<void*>(this), &other, sizeof(*this));
    }
    return *this;
}

CSimpleRenderable& CSimpleRenderable::operator=(CSimpleRenderable&& other) noexcept {
    if (this != &other) {
        for (CTexture*& texture : this->m_textures) {
            if (texture != nullptr) {
                TextureFree(&texture);
            }
        }
        std::memcpy(static_cast<void*>(this), &other, sizeof(*this));
        std::memset(other.m_textures, 0, sizeof(other.m_textures));
    }
    return *this;
}

// ref: FUN_14061d580
CSimpleRenderable::~CSimpleRenderable() {
    for (CTexture*& texture : this->m_textures) {
        if (texture != nullptr) {
            TextureFree(&texture);
        }
    }
}

// ---- CRenderBatch -----------------------------------------------------------------------------

CRenderBatch::CRenderBatch() {
    this->m_stringBatches.m_bucketCount = 8;
    this->m_stringBatches.m_buckets = nullptr;
    this->m_stringBatches.m_size = 0;
    this->m_stringBatches.m_maxLoad = 1.0f;
    this->m_clipped = 0;
}

CRenderBatch::~CRenderBatch() {
    this->Clear();
    this->m_clipped = 0;
    // FUN_14061e280: the string batch map's nodes and buckets.
    blz::chained_destroy<STRINGBATCHNODE>(&this->m_stringBatches, s_stringNodeTag, s_stringBucketTag);
}

void* CRenderBatch::operator new(size_t size) {
    (void)size;
    return s_batchHeap.GetData(false, ".?AVCRenderBatch@@", -2, 0x22);
}

void CRenderBatch::operator delete(void* batch) {
    if (batch != nullptr) {
        s_batchHeap.PutData(batch);
    }
}

// ref: FUN_1406204d0
void CRenderBatch::Initialize() {
    GxAddDeviceCreatedCallback(&OnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
}

// ref: FUN_14061ef70
void CRenderBatch::Shutdown() {
    GxRemoveDeviceCreatedCallback(&OnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
    OnDeviceDestroyed(nullptr);
}

// The string batches go back to their font, the renderables let their textures go.
// ref: FUN_14061ed10
void CRenderBatch::Clear() {
    blz::chained_for_each<STRINGBATCHNODE>(&this->m_stringBatches, [](STRINGBATCHNODE* node) {
        GxuStringBatchDestroy(node->m_batch);
    });
    if (this->m_stringBatches.m_buckets != nullptr) {
        for (uint64_t i = 0; i < this->m_stringBatches.m_bucketCount; i++) {
            for (auto node = static_cast<STRINGBATCHNODE*>(this->m_stringBatches.m_buckets[i]); node != nullptr;) {
                STRINGBATCHNODE* next = node->m_next;
                SMemFree(node, s_stringNodeTag, -6, 0);
                node = next;
            }
            this->m_stringBatches.m_buckets[i] = nullptr;
        }
    }
    this->m_stringBatches.m_size = 0;
    this->m_renderables.clear();
    this->m_ranges.m_size = 0;
}

// ref: FUN_14061d740
CSimpleRenderable* CRenderBatch::Add(const CSimpleRenderable& renderable) {
    return &this->m_renderables.emplace_back(renderable);
}

// ref: FUN_140620500
bool CRenderBatch::AddGeometry(CSimpleFrame* frame, uint64_t drawOrder, CTexture* texture, uint16_t blend,
    uint64_t vertexCount, const float* positions, const float* texCoords, uint16_t colorStep, const uint8_t* colors,
    uint64_t indexCount, const uint16_t* indices, float desaturation, bool onTop, float snappingBias, bool snapToPixelGrid,
    uint32_t extraCount, CTexture* const* extraTextures, const float* const* extraTexCoords) {
    if (indexCount == 0 || vertexCount == 0 || TextureGetGxTex(texture) == nullptr) {
        return false;
    }
    for (uint32_t i = 0; i < extraCount; i++) {
        if (TextureGetGxTex(extraTextures[i]) == nullptr) {
            return false;
        }
    }
    CSimpleRenderable renderable;
    renderable.m_frame = frame;
    renderable.m_drawOrder = drawOrder;
    renderable.m_type = CSimpleRenderable::RENDERABLE_GEOMETRY;
    renderable.m_onTop = onTop;
    TextureAddRef(texture);
    renderable.m_textures[0] = texture;
    renderable.m_desaturation = desaturation;
    renderable.m_blend = blend;
    if (snappingBias <= 0.0f) {
        renderable.m_snappingBias = snapToPixelGrid ? -1.0f : 0.0f;
    } else {
        renderable.m_snappingBias = snappingBias;
    }
    renderable.m_positions = positions;
    renderable.m_texCoords = texCoords;
    renderable.m_colors = colors;
    renderable.m_indices = indices;
    renderable.m_colorStep = colorStep;
    renderable.m_indexCount = static_cast<uint16_t>(indexCount < 0xffff ? indexCount : 0xffff);
    renderable.m_vertexCount = static_cast<uint16_t>(vertexCount < 0xffff ? vertexCount : 0xffff);
    renderable.m_atlas = TextureHasAtlas(renderable.m_textures[0]);
    if (renderable.m_atlas != 0) {
        TextureGetAtlasCoords(renderable.m_textures[0], renderable.m_atlasOffset, &renderable.m_atlasScale);
    }
    for (uint32_t i = 0; i < extraCount; i++) {
        renderable.m_extraTexCoords[i] = extraTexCoords[i];
        TextureAddRef(extraTextures[i]);
        renderable.m_textures[1 + i] = extraTextures[i];
    }
    renderable.m_extraTextures = static_cast<uint8_t>(extraCount);
    this->Add(renderable);
    return true;
}

// ref: FUN_140620790
void CRenderBatch::AddChildBatch(CSimpleFrame* frame, uint64_t drawOrder, CRenderBatch* batch) {
    CSimpleRenderable child;
    child.m_frame = frame;
    child.m_drawOrder = drawOrder;
    child.m_type = CSimpleRenderable::RENDERABLE_BATCH;
    child.m_onTop = 1;
    child.m_batch = batch;
    this->Add(child);
    if ((frame->m_flags2 & CSimpleFrame::FRAME2_FRAME_BUFFER) != 0 && frame->m_parent != nullptr) {
        CSimpleRenderable blit;
        blit.m_frame = frame->m_parent;
        blit.m_drawOrder = drawOrder;
        blit.m_type = CSimpleRenderable::RENDERABLE_FRAME_BUFFER;
        blit.m_onTop = 1;
        blit.m_bufferFrame = frame;
        this->Add(blit);
    }
}

// ref: FUN_1406208c0
void CRenderBatch::AddCallback(CSimpleFrame* frame, uint64_t drawOrder, RENDERCALLBACK callback, void* param, bool onTop) {
    CSimpleRenderable renderable;
    renderable.m_frame = frame;
    renderable.m_drawOrder = drawOrder;
    renderable.m_type = CSimpleRenderable::RENDERABLE_CALLBACK;
    renderable.m_onTop = onTop;
    renderable.m_callback = callback;
    renderable.m_callbackParam = param;
    this->Add(renderable);
}

// ref: FUN_14061e950
GxuStringBatch* CRenderBatch::GetStringBatch(CSimpleFrame* frame, uint64_t drawOrder, uint64_t font) {
    bool inserted;
    STRINGBATCHNODE* node = blz::chained_insert<STRINGBATCHNODE>(&this->m_stringBatches, font, &inserted, s_stringNodeTag,
        s_stringBucketTag);
    if (node->m_batch != nullptr) {
        return node->m_batch;
    }
    GxuStringBatch* strings = GxuStringBatchCreate();
    node->m_batch = strings;
    CSimpleRenderable renderable;
    renderable.m_frame = frame;
    renderable.m_drawOrder = drawOrder;
    renderable.m_type = CSimpleRenderable::RENDERABLE_STRINGS;
    renderable.m_onTop = frame != nullptr && (frame->m_flags2 & CSimpleFrame::FRAME2_STRATA) == 0x480;
    renderable.m_strings = strings;
    this->Add(renderable);
    return strings;
}

// A merge sort, as stable as the reference's (FUN_14061db00 over FUN_14061d2b0 and
// FUN_14061d900).
void CRenderBatch::Sort() {
    std::stable_sort(this->m_renderables.begin(), this->m_renderables.end(), &RenderableLess);
}

void CRenderBatch::BuildRanges() {
    this->Sort();
    uint64_t count = this->m_renderables.size();
    this->m_ranges.reserve(count);
    if (count == 0) {
        return;
    }
    uint32_t totalIndices = 0;
    uint32_t totalVertices = 0;
    const CSimpleRenderable* head = nullptr;
    for (uint64_t i = 0; i < count; i++) {
        const CSimpleRenderable& r = this->m_renderables[i];
        bool merge = false;
        if (head != nullptr && r.m_type == CSimpleRenderable::RENDERABLE_GEOMETRY
            && head->m_type == CSimpleRenderable::RENDERABLE_GEOMETRY && head->m_desaturation == r.m_desaturation
            && head->m_snappingBias == r.m_snappingBias && r.m_blend == head->m_blend && r.m_extraTextures == head->m_extraTextures) {
            merge = true;
            uint32_t textures = TextureCount(r);
            for (uint32_t k = 0; k < textures; k++) {
                if (TextureGetGxTex(r.m_textures[k]) != TextureGetGxTex(head->m_textures[k])) {
                    merge = false;
                    break;
                }
            }
        }
        if (!merge) {
            CSimpleRenderablesSubBatch range = {};
            range.m_first = static_cast<uint16_t>(i);
            if (r.m_type == CSimpleRenderable::RENDERABLE_GEOMETRY) {
                range.m_indexStart = totalIndices;
            }
            this->m_ranges.push_back(range);
            head = &r;
        }
        if (r.m_type == CSimpleRenderable::RENDERABLE_GEOMETRY) {
            CSimpleRenderablesSubBatch& last = this->m_ranges.back();
            last.m_count++;
            last.m_indexCount += r.m_indexCount;
            last.m_vertexCount += r.m_vertexCount;
            totalIndices += r.m_indexCount;
            totalVertices += r.m_vertexCount;
        }
    }
    if (totalVertices > 0xffff || totalIndices > 0x3ffff) {
        this->Clear();
    }
}

void CRenderBatch::UploadRange(const CSimpleRenderablesSubBatch* range) {
    GxRenderContext* context = g_gxRenderContext;
    const CSimpleRenderable& first = this->m_renderables[range->m_first];
    uint32_t extra = 0;
    if (first.m_type == CSimpleRenderable::RENDERABLE_GEOMETRY && first.m_extraTextures != 0) {
        extra = first.m_extraTextures < 3 ? first.m_extraTextures : 3;
    }
    uint32_t stride = 0x18 + extra * 8;
    uint8_t format = static_cast<uint8_t>(0xc + extra);
    GxBuf* vertices = GxBufTransient(context, 0, stride, range->m_vertexCount);
    GxBuf* indices = GxBufTransient(context, 1, 2, range->m_indexCount);
    auto vertexData = static_cast<uint8_t*>(context->BufLock(vertices));
    auto indexData = static_cast<uint16_t*>(context->BufLock(indices));
    FillVertices(this->m_renderables.data(), range, vertexData, indexData, extra, stride);
    context->BufUnlock(vertices, 0);
    context->BufUnlock(indices, 0);
    GxSetVertexStream(context, vertices, format);
    context->SetIndexBuffer(indices, 1);
}

void CRenderBatch::Draw(bool onTopOnly, const float* viewIn, const float* projectionIn) {
    float view[16];
    float projection[16];
    std::memcpy(view, viewIn, sizeof(view));
    std::memcpy(projection, projectionIn, sizeof(projection));
    if (this->m_renderables.size() == 0) {
        return;
    }
    if (this->m_ranges.size() == 0) {
        this->BuildRanges();
    }
    uint64_t rangeCount = this->m_ranges.size();
    if (rangeCount == 0) {
        return;
    }
    float viewProjection[16];
    ViewProjection(view, projection, viewProjection);
    CLIPRECT clip;
    clip.m_clipped = this->m_clipped;
    if (clip.m_clipped != 0) {
        clip.m_rect = this->m_clipRect;
    }
    CLIPSTATE state;
    ClipBegin(&state, &clip, view, projection, viewProjection);
    if (state.m_empty == 0) {
        GxRenderContext* context = g_gxRenderContext;
        GXSIZE screen;
        WindowGetResolution(&screen);
        float screenSize[4] = { static_cast<float>(screen.m_width), static_cast<float>(screen.m_height), 0.0f, 1.0f };
        uint8_t lastType = 5;
        uint32_t lastTextures = 0;
        float lastDesaturation = -99.91f;
        float lastSnappingBias = -99.91f;
        void* bound[4] = {};
        uint64_t index = 0;
        for (uint64_t i = 0; i < rangeCount; i++) {
            const CSimpleRenderablesSubBatch* range = &this->m_ranges[i];
            CSimpleRenderable* r = &this->m_renderables[index];
            if (onTopOnly && r->m_onTop == 0) {
                index += r->m_type == CSimpleRenderable::RENDERABLE_GEOMETRY ? range->m_count : 1;
                continue;
            }
            uint8_t type = r->m_type;
            uint32_t textures = TextureCount(*r);
            if (lastType != type && state.m_active != 0) {
                ClipApply(&state, view, projection, viewProjection);
            }
            CSimpleFrame* owner = r->m_frame->m_frameBuffer;
            FRAMEBUFFER* buffer = owner != nullptr ? CachedFrameBuffer(owner) : nullptr;
            if (buffer == nullptr || buffer->m_bound != 0) {
                if (s_boundFrameBuffer != buffer) {
                    FRAMEBUFFER* target = buffer != nullptr ? buffer : WindowGetFrameBuffer();
                    context->SetRenderTargets(target->m_targets.m_count != 0 ? &target->m_targets : nullptr);
                    s_boundFrameBuffer = buffer;
                }
            } else {
                GXSIZE size;
                WindowGetResolution(&size);
                int32_t dimensions[2] = { size.m_width, size.m_height };
                FrameBufferSetupKind(buffer, dimensions, 0xc);
                FrameBufferBind(buffer);
                context->Clear(buffer->m_targets.m_count != 0 ? &buffer->m_targets : nullptr, 1, 0, 1.0f, 0, 0);
                s_boundFrameBuffer = buffer;
            }
            switch (type) {
            case CSimpleRenderable::RENDERABLE_GEOMETRY: {
                uint8_t extra = r->m_extraTextures;
                uint8_t format = static_cast<uint8_t>(extra == 0 ? 0xc : extra == 1 ? 0xd : extra == 2 ? 0xe : 0xf);
                GxPipeline* pipeline = RenderBatchGetPipeline(extra, s_depthStencil, s_raster, r->m_blend, format, 3, 0xc);
                context->SetPipeline(pipeline);
                if (lastType != type || lastTextures != textures) {
                    float* constants = context->LockConstants(0, 0, 0x50);
                    std::memcpy(constants, viewProjection, 0x40);
                    std::memcpy(constants + 0x10, screenSize, 0x10);
                    context->UnlockConstants(0, 0);
                }
                this->UploadRange(range);
                bool rebind;
                if (lastType == type && lastDesaturation == r->m_desaturation && r->m_snappingBias == lastSnappingBias) {
                    rebind = lastTextures != textures;
                } else {
                    float* vertex = context->LockConstants(0, 1, 0x10);
                    vertex[0] = r->m_snappingBias;
                    vertex[1] = 0.0f;
                    vertex[2] = 0.0f;
                    vertex[3] = 0.0f;
                    context->UnlockConstants(0, 1);
                    float* pixel = context->LockConstants(2, 0, 0x10);
                    pixel[0] = r->m_desaturation;
                    pixel[1] = r->m_snappingBias;
                    pixel[2] = 0.0f;
                    pixel[3] = 0.0f;
                    context->UnlockConstants(2, 0);
                    lastDesaturation = r->m_desaturation;
                    lastSnappingBias = r->m_snappingBias;
                    rebind = lastType != type || lastTextures != textures;
                }
                if (!rebind) {
                    for (uint32_t k = 0; k < 4; k++) {
                        if (bound[k] != GxTexOf(r->m_textures[k])) {
                            rebind = true;
                            break;
                        }
                    }
                }
                if (rebind) {
                    for (uint32_t k = 0; k < 4; k++) {
                        bound[k] = GxTexOf(r->m_textures[k]);
                        context->SetTexture(2, k, static_cast<gxDx12Backend::Tex*>(bound[k]));
                    }
                    float width = 1.0f;
                    float height = 1.0f;
                    if (bound[0] != nullptr) {
                        width = static_cast<float>(static_cast<uint16_t*>(bound[0])[0]);
                        height = static_cast<float>(static_cast<uint16_t*>(bound[0])[1]);
                    }
                    float* size = context->LockConstants(0, 2, 0x10);
                    size[0] = width;
                    size[1] = height;
                    size[2] = 0.0f;
                    size[3] = 0.0f;
                    context->UnlockConstants(0, 2);
                }
                context->DrawIndexed(range->m_indexCount, 1, 0, 0, 0, 0);
                index += range->m_count;
                break;
            }
            case CSimpleRenderable::RENDERABLE_STRINGS:
                GxuStringBatchRender(context, r->m_strings, view, projection, nullptr);
                index++;
                break;
            case CSimpleRenderable::RENDERABLE_CALLBACK:
                r->m_callback(r->m_callbackParam, view, projection, s_boundFrameBuffer);
                index++;
                break;
            case CSimpleRenderable::RENDERABLE_BATCH:
                r->m_batch->Draw(onTopOnly, view, projection);
                index++;
                break;
            case CSimpleRenderable::RENDERABLE_FRAME_BUFFER: {
                FRAMEBUFFER* source = CachedFrameBuffer(r->m_bufferFrame);
                context->SetPipeline(s_blitPipeline);
                context->SetTexture(2, 0, source->m_targets.m_texs[1]);
                float* alpha = context->LockConstants(2, 0, 4);
                alpha[0] = static_cast<float>(r->m_bufferFrame->GetAlpha()) * (1.0f / 255.0f);
                context->UnlockConstants(2, 0);
                context->Draw(3, 1, 0, 0, 0);
                FrameBufferRelease(source);
                index++;
                break;
            }
            default:
                break;
            }
            lastType = r->m_type;
            lastTextures = TextureCount(*r);
        }
    }
    ClipEnd(&state);
}

// ---- pipelines and helpers -----------------------------------------------------------------------

GxPipeline* RenderBatchGetPipeline(uint8_t extraTextures, uint64_t depthStencil, uint32_t raster, uint16_t blend,
    uint8_t vertexFormat, uint8_t topology, uint32_t targetKind) {
    PIPELINEHASH key;
    PipelineHashMake(&key, extraTextures, extraTextures, depthStencil, blend, raster, vertexFormat, topology, targetKind, 0);
    PIPELINENODE* cached = PipelineMapFind(&s_pipelines, key);
    if (cached != nullptr) {
        return cached->m_pipeline;
    }
    OsMutexLock(&s_pipelineLock);
    GxPipeline* pipeline;
    PIPELINENODE* fresh = PipelineMapFind(&s_newPipelines, key);
    if (fresh != nullptr) {
        pipeline = fresh->m_pipeline;
    } else {
        GXPIPELINEKEY desc;
        std::memset(&desc, 0, sizeof(desc));
        desc.m_vsPermute = extraTextures;
        desc.m_psPermute = extraTextures;
        desc.m_topology = topology;
        std::memcpy(desc.m_raster, &raster, 4);
        std::memcpy(desc.m_depthStencil, &depthStencil, 8);
        RENDERTARGETFORMATS formats = RenderTargetFormats(targetKind);
        std::memcpy(desc.m_targetFormats, &formats, 4);
        desc.m_depthFormat = formats.m_depth;
        desc.m_vertexShader = 0xa9;
        desc.m_pixelShader = 0x104;
        desc.m_vertexFormat = vertexFormat;
        desc.m_blend = blend;
        pipeline = GxPipelineCreate(&desc, "ObjectFillEffect");
        PipelineMapInsert(&s_newPipelines, key)->m_pipeline = pipeline;
        TRANSIENTPIPELINEMAP transient = { 1, &s_newPipelines, &s_pipelines };
        TransientPipelineMapAdd(&transient);
    }
    OsMutexUnlock(&s_pipelineLock);
    return pipeline;
}

// ref: FUN_1405dbd10
void GxXformSetViewport(float minX, float maxX, float minY, float maxY, float minZ, float maxZ) {
    GxRenderContext* context = g_gxRenderContext;
    if (1.0f <= maxX) {
        maxX = 1.0f;
    }
    if (1.0f <= maxY) {
        maxY = 1.0f;
    }
    if (minY <= 0.0f) {
        minY = 0.0f;
    }
    if (minX <= 0.0f) {
        minX = 0.0f;
    }
    float values[6] = { minX, maxX, minY, maxY, minZ, maxZ };
    bool changed = false;
    for (uint32_t i = 0; i < 6; i++) {
        if (values[i] != context->Field<float>(0x1c + i * 4)) {
            changed = true;
        }
    }
    if (changed) {
        for (uint32_t i = 0; i < 6; i++) {
            context->Field<float>(0x1c + i * 4) = values[i];
        }
        context->Field<uint8_t>(0x18) = 1;
    }
}

// ref: FUN_141296240
void UiComputeMatrices(const CRect* rect, const float* offset, float* view, float* projection) {
    float centerY = (rect->maxY + rect->minY) * 0.5f;
    float centerX = (rect->maxX + rect->minX) * 0.5f;
    Identity(projection);
    Orthographic(rect->minX - centerX, rect->maxX - centerX, rect->minY - centerY, rect->maxY - centerY, -500.0f, 500.0f,
        projection);
    float flip[3] = { 1.0f, 1.0f, -1.0f };
    MatrixScale(projection, flip);
    Identity(view);
    float move[3] = { offset[0] - centerX, offset[1] - centerY, 0.0f };
    MatrixTranslate(view, move);
}
