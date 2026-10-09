#ifndef UI_C_RENDER_BATCH_HPP
#define UI_C_RENDER_BATCH_HPP

// CRenderBatch (Engine/Source/Frame/CRenderBatch.cpp): what a frame with its own batch draws
// (0x68, CDataAllocator 0x142e97ba0). It holds the renderables the regions add (textured
// geometry, font string batches, draw callbacks, child batches and frame-buffer blits), the
// draw ranges they sort into, the font string batches by font, and the rect the batch clips to.

#include <cstdint>

#include "blz/ChainedHash.hpp"
#include "blz/Vector.hpp"
#include "ui/CLayoutFrame.hpp"

class CSimpleFrame;
class CTexture;
struct GxPipeline;
struct GxuStringBatch;
class GxRenderContext;

extern const char s_renderableTag[];
extern const char s_subBatchTag[];

// A draw callback (type 2): its parameter, the view and projection, and the frame buffer bound.
typedef void (*RENDERCALLBACK)(void* param, const float* view, const float* projection, void* frameBuffer);

// One thing to draw (CSimpleRenderable, 0x90).
struct CSimpleRenderable {
    // m_type.
    enum : uint8_t {
        RENDERABLE_GEOMETRY = 0,
        RENDERABLE_STRINGS = 1,
        RENDERABLE_CALLBACK = 2,
        RENDERABLE_BATCH = 3,
        RENDERABLE_FRAME_BUFFER = 4,
    };

    CSimpleFrame* m_frame;          // 0x00 whose frame buffer it draws into
    uint64_t m_drawOrder;           // 0x08
    uint8_t m_type;                 // 0x10
    uint8_t m_onTop;                // 0x11 drawn when only the top strata draws
    uint8_t m_pad12[6];
    // 0x18: the positions (C3Vector), or the string batch, callback, child batch or frame.
    union {
        const float* m_positions;
        GxuStringBatch* m_strings;
        RENDERCALLBACK m_callback;
        class CRenderBatch* m_batch;
        CSimpleFrame* m_bufferFrame;
    };
    // 0x20: the texture coordinates (C2Vector), or the callback's parameter.
    union {
        const float* m_texCoords;
        void* m_callbackParam;
    };
    const uint8_t* m_colors;        // 0x28 BGRA, m_colorStep / 4 apart
    const uint16_t* m_indices;      // 0x30
    const float* m_extraTexCoords[3];   // 0x38 per extra texture
    float m_desaturation;           // 0x50
    float m_snappingBias;           // 0x54 texel snapping; -1 snaps to the pixel grid
    uint16_t m_blend;               // 0x58 a pipeline blend {mode, mask}
    uint8_t m_extraTextures;        // 0x5a
    uint8_t m_atlas;                // 0x5b the first texture lives on an atlas page
    uint16_t m_colorStep;           // 0x5c 0 one colour, 4 one per vertex
    uint16_t m_indexCount;          // 0x5e
    uint16_t m_vertexCount;         // 0x60
    uint16_t m_pad62;
    float m_atlasScale;             // 0x64
    float m_atlasOffset[2];         // 0x68
    CTexture* m_textures[4];        // 0x70 each holding a reference

    CSimpleRenderable();
    // ref: FUN_14061dd20
    CSimpleRenderable(const CSimpleRenderable& other);
    CSimpleRenderable(CSimpleRenderable&& other) noexcept;
    // ref: FUN_14061e5d0
    CSimpleRenderable& operator=(const CSimpleRenderable& other);
    CSimpleRenderable& operator=(CSimpleRenderable&& other) noexcept;
    ~CSimpleRenderable();
};
static_assert(sizeof(CSimpleRenderable) == 0x90, "CSimpleRenderable is 0x90 bytes");

// A run of renderables drawn with one draw (CSimpleRenderablesSubBatch, 0x14): geometry ones
// sharing their state are merged, every other renderable is a run of its own.
struct CSimpleRenderablesSubBatch {
    uint16_t m_first;               // 0x00
    uint16_t m_pad02;
    uint32_t m_count;               // 0x04
    uint32_t m_indexCount;          // 0x08
    uint32_t m_indexStart;          // 0x0c
    uint32_t m_vertexCount;         // 0x10
};
static_assert(sizeof(CSimpleRenderablesSubBatch) == 0x14, "a sub-batch is 0x14 bytes");

class CRenderBatch {
    public:
    blz::vector<CSimpleRenderable, s_renderableTag> m_renderables;          // 0x00
    blz::vector<CSimpleRenderablesSubBatch, s_subBatchTag> m_ranges;        // 0x18
    blz::chained_hash_header m_stringBatches;   // 0x30 GxuStringBatch* by font
    CRect m_clipRect;                           // 0x50
    uint8_t m_clipped;                          // 0x60

    // ref: FUN_14061dce0
    CRenderBatch();
    // ref: FUN_14061e310
    ~CRenderBatch();
    // Batches come from their own pool (0x142e97ba0).
    static void* operator new(size_t size);
    static void operator delete(void* batch);

    // Registers the pipelines with the device (FUN_1406204d0) and removes them (FUN_14061ef70).
    static void Initialize();
    static void Shutdown();

    // Empties the batch for a rebuild.
    void Clear();
    // Adds a renderable (FUN_14061d740).
    CSimpleRenderable* Add(const CSimpleRenderable& renderable);
    // Adds textured geometry (FUN_140620500); false when it has nothing to draw or a texture is
    // not drawable.
    bool AddGeometry(CSimpleFrame* frame, uint64_t drawOrder, CTexture* texture, uint16_t blend, uint64_t vertexCount,
        const float* positions, const float* texCoords, uint16_t colorStep, const uint8_t* colors, uint64_t indexCount,
        const uint16_t* indices, float desaturation, bool onTop, float snappingBias, bool snapToPixelGrid, uint32_t extraCount,
        CTexture* const* extraTextures, const float* const* extraTexCoords);
    // Adds a child frame's batch at its draw order, and its blit when it is a frame buffer
    // (FUN_140620790).
    void AddChildBatch(CSimpleFrame* frame, uint64_t drawOrder, CRenderBatch* batch);
    // Adds a draw callback (FUN_1406208c0).
    void AddCallback(CSimpleFrame* frame, uint64_t drawOrder, RENDERCALLBACK callback, void* param, bool onTop);
    // The string batch for a font, made and added as a renderable the first time
    // (FUN_14061e950 with FUN_140620970's insertion).
    GxuStringBatch* GetStringBatch(CSimpleFrame* frame, uint64_t drawOrder, uint64_t font);

    // Sorts the renderables by frame buffer, draw order and state (FUN_140621150).
    void Sort();
    // Sorts, then splits the renderables into draw runs (FUN_14061fe80).
    void BuildRanges();
    // Draws: everything, or only what is on top (FUN_14061f160).
    void Draw(bool onTopOnly, const float* view, const float* projection);

    private:
    // Writes a run's vertices and indices and binds them (FUN_140620e20).
    void UploadRange(const CSimpleRenderablesSubBatch* range);
};
static_assert(sizeof(CRenderBatch) == 0x68, "CRenderBatch is 0x68 bytes");

class CSimpleTexture;
// Adds a texture region's quad with its masks; `owner` draws it in its place (FUN_140620b00).
void RenderBatchAddTexture(CRenderBatch* batch, CSimpleTexture* texture, CSimpleTexture* owner);

// The pipeline drawing geometry with `extraTextures` more textures (FUN_1406200f0).
GxPipeline* RenderBatchGetPipeline(uint8_t extraTextures, uint64_t depthStencil, uint32_t raster, uint16_t blend,
    uint8_t vertexFormat, uint8_t topology, uint32_t targetKind);

// The blend each region blend mode draws with (DAT_142e97b70).
extern uint16_t g_renderBatchBlends[5];

// Sets the viewport in normalised coordinates, clamped (FUN_1405dbd10).
void GxXformSetViewport(float minX, float maxX, float minY, float maxY, float minZ, float maxZ);
// The view and projection for drawing `rect` (UI units) at `offset` (FUN_141296240).
void UiComputeMatrices(const CRect* rect, const float* offset, float* view, float* projection);

#endif
