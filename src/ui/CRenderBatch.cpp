#include "ui/CRenderBatch.hpp"

#include "base/DataAllocator.hpp"
#include "util/Unimplemented.hpp"

const char s_renderableTag[] = ".?AUCSimpleRenderable@@";
const char s_subBatchTag[] = ".?AVCSimpleRenderablesSubBatch@@";

namespace {

// The batches' pool (0x142e97ba0, made by the static initializer 0x1401df6e0).
CDataAllocator s_batchHeap(0x68, 100);

}

// ref: FUN_14061dce0
CRenderBatch::CRenderBatch() {
    this->m_stringBatches.m_bucketCount = 8;
    this->m_stringBatches.m_buckets = nullptr;
    this->m_stringBatches.m_size = 0;
    this->m_stringBatches.m_maxLoad = 1.0f;
    this->m_clipped = 0;
}

// The renderables release their textures and the font batches go (FUN_14061ed10, then the
// vectors' storage).
// ref: FUN_14061e310
CRenderBatch::~CRenderBatch() {
    this->Clear();
    this->m_clipped = 0;
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

// ref: FUN_14061ed10
void CRenderBatch::Clear() {
    // Each font string batch is released (FUN_14068ab40) and each renderable lets go of its
    // textures (FUN_14061d580); both belong to the UI render port, which is not done yet.
    if (this->m_renderables.size() != 0 || this->m_stringBatches.m_size != 0) {
        FROST_UNIMPLEMENTED();
    }
    this->m_renderables.clear();
    this->m_subBatches.clear();
}

// ref: FUN_140620790
void CRenderBatch::AddChildBatch(CSimpleFrame* frame, uint64_t drawOrder, CRenderBatch* batch) {
    (void)frame;
    (void)drawOrder;
    (void)batch;
    FROST_UNIMPLEMENTED();
}
