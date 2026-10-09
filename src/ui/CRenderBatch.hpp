#ifndef UI_C_RENDER_BATCH_HPP
#define UI_C_RENDER_BATCH_HPP

// CRenderBatch (Engine/Source/Frame/CRenderBatch.cpp): what a frame with its own batch draws
// (0x68, CDataAllocator 0x142e97ba0). It holds the renderables the regions add, the batches of
// child frames (sub-batches, ordered by draw order), the font string batches, and the rect the
// batch clips to.

#include <cstdint>

#include "blz/ChainedHash.hpp"
#include "blz/Vector.hpp"
#include "ui/CLayoutFrame.hpp"

class CSimpleFrame;

extern const char s_renderableTag[];
extern const char s_subBatchTag[];

// One thing to draw (CSimpleRenderable, 0x90).
struct CSimpleRenderable {
    uint8_t m_data[0x90];
};

// A child frame's batch in this one (CSimpleRenderablesSubBatch, 0x18).
struct CSimpleRenderablesSubBatch {
    CSimpleFrame* m_frame;
    uint64_t m_drawOrder;
    class CRenderBatch* m_batch;
};

class CRenderBatch {
    public:
    blz::vector<CSimpleRenderable, s_renderableTag> m_renderables;          // 0x00
    blz::vector<CSimpleRenderablesSubBatch, s_subBatchTag> m_subBatches;    // 0x18
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

    // Empties the batch for a rebuild.
    void Clear();
    // Adds a child frame's batch at its draw order.
    void AddChildBatch(CSimpleFrame* frame, uint64_t drawOrder, CRenderBatch* batch);
};
static_assert(sizeof(CRenderBatch) == 0x68, "CRenderBatch is 0x68 bytes");

#endif
