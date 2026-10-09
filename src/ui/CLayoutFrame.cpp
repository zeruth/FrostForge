#include "ui/CLayoutFrame.hpp"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <new>

#include "base/Coordinate.hpp"
#include "base/DataAllocator.hpp"
#include "base/ObjectAlloc.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

const float LAYOUT_UNSET = INFINITY;

const char CLayoutFrame::s_frameNodeTag[] = ".?AUFRAMENODE@CLayoutFrame@@";
const char CLayoutFrame::s_frameTag[] = ".PEAVCLayoutFrame@@";

namespace {

const char s_constFrameTag[] = ".PEBVCLayoutFrame@@";
const char s_framePointTag[] = ".?AVCFramePoint@@";
const char s_animDataTag[] = ".?AVCAnimationData@@";
const char s_layoutFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CLayoutFrame.cpp";

// The two-to-the-minus-22 tolerance size and offset changes are compared with (0x14261a954).
constexpr float LAYOUT_EPSILON = 2.38418579e-07f;

// 0x142e99c40 and 0x142e97a08.
CDataAllocator s_framePointAlloc(0x18, 0xaaa);
CDataAllocator s_animDataAlloc(0x58, 100);

// A frame waiting for its rect (0x20, from the "RESIZE" heap; the heap index sits at +0x1c).
struct RESIZENODE {
    RESIZENODE* m_next;
    RESIZENODE* m_prev;
    CLayoutFrame* m_frame;
    uint32_t m_pad18;
    uint32_t m_index;
};
static_assert(sizeof(RESIZENODE) == 0x20, "a resize node is 0x20 bytes");

// The resize list (0x142cce788: head, tail, count) and the pool its nodes come from
// (0x142e97a00).
RESIZENODE s_resizeList = {&s_resizeList, &s_resizeList, nullptr, 0, 0};
uint64_t s_resizeCount;
TObjectAllocHeap* s_resizeAlloc;

// The point slots each edge is solved from, in order (0x1426c79d0..0x1426c7a20).
const int32_t s_centerXPoints[3] = {FRAMEPOINT_TOP, FRAMEPOINT_CENTER, FRAMEPOINT_BOTTOM};
const int32_t s_centerYPoints[3] = {FRAMEPOINT_LEFT, FRAMEPOINT_CENTER, FRAMEPOINT_RIGHT};
const int32_t s_leftPoints[3] = {FRAMEPOINT_TOPLEFT, FRAMEPOINT_LEFT, FRAMEPOINT_BOTTOMLEFT};
const int32_t s_topPoints[3] = {FRAMEPOINT_TOPLEFT, FRAMEPOINT_TOP, FRAMEPOINT_TOPRIGHT};
const int32_t s_rightPoints[3] = {FRAMEPOINT_TOPRIGHT, FRAMEPOINT_RIGHT, FRAMEPOINT_BOTTOMRIGHT};
const int32_t s_bottomPoints[3] = {FRAMEPOINT_BOTTOMLEFT, FRAMEPOINT_BOTTOM, FRAMEPOINT_BOTTOMRIGHT};

bool PointUsed(const CFramePoint* point) {
    return point != nullptr && !(point->m_flags & CFramePoint::FLAG_UNUSED);
}

// The center of an edge pair, from both edges or one edge and the size.
// ref: FUN_1405fa3f0
float CenterFromEdges(float low, float high, float size) {
    if (low != LAYOUT_UNSET) {
        if (high != LAYOUT_UNSET) {
            return (low + high) * 0.5f;
        }
        if (size != 0.0f) {
            return size * 0.5f + low;
        }
    }
    if (high != LAYOUT_UNSET && size != 0.0f) {
        return high - size * 0.5f;
    }
    return LAYOUT_UNSET;
}

CAnimationData* EnsureAnimData(CLayoutFrame* frame) {
    CAnimationData* data = frame->m_animData;
    if (data == nullptr) {
        data = static_cast<CAnimationData*>(s_animDataAlloc.GetData(false, s_animDataTag, -2, 0x1f));
        if (data != nullptr) {
            std::memset(&data->m_transform, 0, sizeof(data->m_transform));
            std::memset(data->m_rectOffset, 0, sizeof(data->m_rectOffset));
            data->m_flags &= ~7u;
            data->m_alpha = 0xff;
        }
        frame->m_animData = data;
    }
    return data;
}

// Searches the anchor graph (points, then dependents) for a frame marked as an anchor root.
// ref: FUN_1405f7df0
CLayoutFrame* FindAnchorRoot(CLayoutFrame* frame, blz::vector<const CLayoutFrame*, s_constFrameTag>* visited,
    uint32_t excludedPoint, uint32_t depth) {
    if (depth >= 1500 || frame->m_visited != 0) {
        return nullptr;
    }
    if (frame->m_flags & CLayoutFrame::FLAG_ANCHOR_ROOT) {
        return frame;
    }
    frame->m_visited = 1;
    visited->push_back(frame);
    for (uint32_t i = 0; i < 9; i++) {
        CFramePoint* point = frame->m_points[i];
        if (i != excludedPoint && PointUsed(point) && point->m_relative != nullptr) {
            CLayoutFrame* root = FindAnchorRoot(point->m_relative, visited, 9, depth + 1);
            if (root != nullptr) {
                return root;
            }
        }
    }
    for (CLayoutFrame::FRAMENODE& node : frame->m_dependents) {
        CLayoutFrame* root = FindAnchorRoot(node.m_frame, visited, 9, depth + 1);
        if (root != nullptr) {
            return root;
        }
    }
    return nullptr;
}

}

// ---- CFramePoint ------------------------------------------------------------------------

// ref: FUN_14062c7e0
CFramePoint::CFramePoint(CLayoutFrame* relative, uint8_t relativePoint, float offsetX, float offsetY) {
    this->m_offsetY = offsetY;
    this->m_offsetX = offsetX;
    this->m_relative = relative;
    this->m_relativePoint = relativePoint;
    this->m_flags = 0;
}

// A change while the point is being read marks the read as looping.
// ref: FUN_14062c800
void CFramePoint::OffsetBy(float x, float y) {
    uint8_t flags = this->m_flags;
    if (flags & FLAG_UNUSED) {
        return;
    }
    this->m_flags = 0;
    this->m_offsetX = x + this->m_offsetX;
    this->m_offsetY = y + this->m_offsetY;
    if (flags & FLAG_COMPUTING) {
        this->m_flags = FLAG_COMPUTING | FLAG_LOOP;
    }
}

// ref: FUN_14062c830
void CFramePoint::ResetOffset() {
    uint8_t flags = this->m_flags;
    if (flags & FLAG_UNUSED) {
        return;
    }
    this->m_offsetX = 0.0f;
    this->m_offsetY = 0.0f;
    this->m_flags = 0;
    if (flags & FLAG_COMPUTING) {
        this->m_flags = FLAG_COMPUTING | FLAG_LOOP;
    }
}

// The relative frame's rect (from its own origin when it is an attachment origin). A rect
// that has to be solved first is solved now; a cycle back to this point fails the read.
// ref: FUN_14062c850
bool CFramePoint::GetRelativeRect(CRect* rect) {
    uint8_t flags = this->m_flags;
    bool computing = (flags & FLAG_COMPUTING) != 0;
    if (!computing) {
        flags |= FLAG_COMPUTING;
    }
    this->m_flags = flags & ~FLAG_LOOP;
    bool result = false;
    CLayoutFrame* relative = this->m_relative;
    bool ready = true;
    if (relative->m_flags & CLayoutFrame::FLAG_DIRTY) {
        relative->Resize(true);
        ready = !(this->m_flags & FLAG_LOOP);
    }
    if (ready && relative->GetRect(rect)) {
        if (relative->IsAttachmentOrigin()) {
            float left = rect->minX;
            rect->minY = 0.0f;
            rect->minX = 0.0f;
            rect->maxY = rect->maxY - rect->minY;
            rect->maxX = rect->maxX - left;
        }
        result = true;
    }
    if (!computing) {
        this->m_flags &= ~FLAG_COMPUTING;
    }
    return result;
}

// ref: FUN_14062c920
void CFramePoint::MarkUnused() {
    this->m_relativePoint = FRAMEPOINT_NUMPOINTS;
    this->m_offsetX = 0.0f;
    this->m_offsetY = 0.0f;
    this->m_relative = nullptr;
    uint8_t flags = this->m_flags;
    this->m_flags = FLAG_UNUSED;
    if (flags & FLAG_COMPUTING) {
        this->m_flags = FLAG_COMPUTING | FLAG_LOOP | FLAG_UNUSED;
    }
}

// ref: FUN_14062c940
void CFramePoint::SetPoint(CLayoutFrame* relative, uint8_t relativePoint, float offsetX, float offsetY) {
    this->m_offsetY = offsetY;
    this->m_offsetX = offsetX;
    this->m_relative = relative;
    this->m_relativePoint = relativePoint;
    uint8_t flags = this->m_flags;
    this->m_flags = 0;
    if (flags & FLAG_COMPUTING) {
        this->m_flags = FLAG_COMPUTING | FLAG_LOOP;
    }
}

// ref: FUN_14062c970
float CFramePoint::X(float scale) {
    CRect rect = {};
    if (!this->GetRelativeRect(&rect)) {
        return LAYOUT_UNSET;
    }
    switch (this->m_relativePoint) {
        case FRAMEPOINT_TOPLEFT:
        case FRAMEPOINT_LEFT:
        case FRAMEPOINT_BOTTOMLEFT:
            return scale * this->m_offsetX + rect.minX;
        case FRAMEPOINT_TOP:
        case FRAMEPOINT_CENTER:
        case FRAMEPOINT_BOTTOM:
            return (rect.maxX + rect.minX) * 0.5f + scale * this->m_offsetX;
        case FRAMEPOINT_TOPRIGHT:
        case FRAMEPOINT_RIGHT:
        case FRAMEPOINT_BOTTOMRIGHT:
            return scale * this->m_offsetX + rect.maxX;
        default:
            return LAYOUT_UNSET;
    }
}

// ref: FUN_14062ca70
float CFramePoint::Y(float scale) {
    CRect rect = {};
    if (!this->GetRelativeRect(&rect)) {
        return LAYOUT_UNSET;
    }
    switch (this->m_relativePoint) {
        case FRAMEPOINT_TOPLEFT:
        case FRAMEPOINT_TOP:
        case FRAMEPOINT_TOPRIGHT:
            return scale * this->m_offsetY + rect.maxY;
        case FRAMEPOINT_LEFT:
        case FRAMEPOINT_CENTER:
        case FRAMEPOINT_RIGHT:
            return (rect.minY + rect.maxY) * 0.5f + scale * this->m_offsetY;
        case FRAMEPOINT_BOTTOMLEFT:
        case FRAMEPOINT_BOTTOM:
        case FRAMEPOINT_BOTTOMRIGHT:
            return scale * this->m_offsetY + rect.minY;
        default:
            return LAYOUT_UNSET;
    }
}

// ---- construction -----------------------------------------------------------------------

// ref: FUN_1405f5df0
CLayoutFrame::CLayoutFrame() {
    for (CFramePoint*& point : this->m_points) {
        point = nullptr;
    }
    this->m_layoutState &= ~0xffu;
    this->m_visited = 0;
    this->m_rect = {};
    this->m_animData = nullptr;
    this->m_minResizeHeight = 0.0f;
    this->m_minResizeWidth = 0.0f;
    this->m_maxResizeHeight = 0.0f;
    this->m_maxResizeWidth = 0.0f;
    this->m_width = 0.0f;
    this->m_height = 0.0f;
    this->m_layoutScale = 1.0f;
    this->m_layoutDepth = 1.0f;
    this->m_flags = 0;
}

// ref: FUN_1405f5ea0
CLayoutFrame::~CLayoutFrame() {
    if (this->m_animData != nullptr) {
        s_animDataAlloc.PutData(this->m_animData);
        this->m_animData = nullptr;
    }
    this->DestroyLayout();
}

void* CLayoutFrame::GetLayoutParent() {
    return nullptr;
}

CScriptRegion* CLayoutFrame::GetScriptObject() {
    return nullptr;
}

CScriptRegion* CLayoutFrame::GetScriptRegion() {
    return nullptr;
}

// ref: FUN_1405f7120
void CLayoutFrame::ClearAllPointsVirtual() {
    this->ClearAllPoints();
    this->SetAnchorRestricted(false);
}

// A frame that loses its anchor restriction is no longer kept on screen.
// ref: FUN_1405f9150
void CLayoutFrame::OnAnchorRestrictedChanged(bool restricted) {
    if (restricted && (this->m_flags & FLAG_CLAMPED)) {
        this->m_flags &= ~FLAG_CLAMPED;
        this->Resize(false);
    }
}

// ref: FUN_1405f8da0
void CLayoutFrame::ScaleBy(float* scale) {
    for (CFramePoint* point : this->m_points) {
        if (PointUsed(point)) {
            point->m_offsetX = point->m_offsetX * *scale;
            point->m_offsetY = point->m_offsetY * *scale;
        }
    }
    this->m_width = *scale * this->m_width;
    this->m_height = this->m_height * *scale;
    this->m_minResizeHeight = this->m_minResizeHeight * *scale;
    this->m_minResizeWidth = this->m_minResizeWidth * *scale;
    this->m_maxResizeHeight = this->m_maxResizeHeight * *scale;
    this->m_maxResizeWidth = *scale * this->m_maxResizeWidth;
    this->Resize(false);
}

void CLayoutFrame::OnProtectFlagsChanged(uint16_t flag) {
    (void)flag;
}

bool CLayoutFrame::IsProtectedSelf(bool* propagated) {
    (void)propagated;
    return false;
}

// ref: FUN_1405f9f30
bool CLayoutFrame::SetLayoutScale(float scale, bool force) {
    if ((force || std::fabs(scale - this->m_layoutScale) >= LAYOUT_EPSILON) && 1.1920929e-07f < scale) {
        this->m_layoutScale = scale;
        this->Resize(false);
        return true;
    }
    return false;
}

// ref: FUN_1405fa370
void CLayoutFrame::SetWidth(float width) {
    if (width != this->m_width) {
        this->m_width = width;
        this->ClearRectFailed();
        this->Resize(false);
    }
}

// ref: FUN_1405f9ea0
void CLayoutFrame::SetHeight(float height) {
    if (height != this->m_height) {
        this->m_height = height;
        this->ClearRectFailed();
        this->Resize(false);
    }
}

// ref: FUN_1405fa320
void CLayoutFrame::SetSize(float width, float height) {
    if (width == this->m_width && height == this->m_height) {
        return;
    }
    this->m_width = width;
    this->m_height = height;
    this->ClearRectFailed();
    this->Resize(false);
}

// ref: FUN_1405f8950
float CLayoutFrame::GetWidth() {
    return this->m_width;
}

// ref: FUN_1405f81a0
float CLayoutFrame::GetHeight() {
    return this->m_height;
}

// The set size, or the solved one unless asked to ignore the rect.
// ref: FUN_1405f8800
void CLayoutFrame::GetSize(float* width, float* height, bool ignoreRect) {
    *width = this->GetWidth();
    *height = this->GetHeight();
    if (ignoreRect) {
        return;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        *width = (this->m_rect.maxX - this->m_rect.minX) / this->m_layoutScale;
        *height = (this->m_rect.maxY - this->m_rect.minY) / this->m_layoutScale;
    }
}

bool CLayoutFrame::CanBeQueried() {
    return true;
}

// ref: FUN_1405f8180
void CLayoutFrame::GetClampRectInsets(float* left, float* right, float* top, float* bottom) {
    *bottom = 0.0f;
    *top = 0.0f;
    *right = 0.0f;
    *left = 0.0f;
}

bool CLayoutFrame::IsAttachmentOrigin() {
    return false;
}

// A protected frame may only anchor to frames whose own anchors accept it.
// ref: FUN_1405f6e10
bool CLayoutFrame::CanBeAnchorFor(CLayoutFrame* frame) {
    if (frame->IsProtected()) {
        for (CFramePoint* point : this->m_points) {
            if (PointUsed(point) && point->m_relative != nullptr && !point->m_relative->CanBeAnchorFor(frame)) {
                return false;
            }
        }
    }
    return true;
}

// ref: FUN_1405f7d40
CLayoutFrame* CLayoutFrame::GetAnchorRoot(int32_t excludedPoint) {
    blz::embedded_vector<const CLayoutFrame*, 0x1000, s_constFrameTag> visited;
    CLayoutFrame* root = FindAnchorRoot(this, &visited, static_cast<uint32_t>(excludedPoint), 0);
    for (const CLayoutFrame* frame : visited) {
        const_cast<CLayoutFrame*>(frame)->m_visited = 0;
    }
    return root;
}

CLayoutFrame* CLayoutFrame::GetRelativeByName(const char* name, CLayoutFrame* self) {
    (void)name;
    (void)self;
    return nullptr;
}

bool CLayoutFrame::IsLayoutReady() {
    return true;
}

void CLayoutFrame::OnLayoutReady() {
}

void CLayoutFrame::ApplyHide() {
}

void CLayoutFrame::ApplyShow() {
}

void CLayoutFrame::OnLayoutDepthChanged() {
}

void CLayoutFrame::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    (void)rect;
    (void)previous;
}

// ref: FUN_1405f6730
void CLayoutFrame::ClearAnimTransform() {
    CAnimationData* data = this->m_animData;
    if (data != nullptr) {
        data->m_flags &= ~1u;
        std::memset(&data->m_transform, 0, sizeof(data->m_transform));
    }
    this->Resize(true);
}

// ref: FUN_1405f6760
void CLayoutFrame::ClearAnimRectOffset() {
    CAnimationData* data = this->m_animData;
    if (data != nullptr) {
        data->m_flags &= ~2u;
        std::memset(data->m_rectOffset, 0, sizeof(data->m_rectOffset));
    }
}

// ref: FUN_1405f6710
void CLayoutFrame::ClearAnimAlpha() {
    CAnimationData* data = this->m_animData;
    if (data != nullptr) {
        data->m_flags &= ~4u;
        data->m_alpha = 0xff;
    }
}

// ---- the resize list ----------------------------------------------------------------------

// ref: FUN_1405f64e0
void CLayoutFrame::CreateResizeAllocator() {
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x1f, s_layoutFile, SStrHashFnv1a(s_layoutFile), 0x65, 0);
    s_resizeAlloc = new (memory) TObjectAllocHeap("RESIZE", 0x20, 0x400, 0x1f);
}

// ref: FUN_1405f65c0
void CLayoutFrame::DestroyResizeAllocator() {
    RESIZENODE* node = s_resizeList.m_prev;
    s_resizeList.m_prev = &s_resizeList;
    s_resizeList.m_next = &s_resizeList;
    s_resizeCount = 0;
    while (node != &s_resizeList) {
        RESIZENODE* prev = node->m_prev;
        s_resizeAlloc->Free(node);
        node = prev;
    }
    if (s_resizeAlloc != nullptr) {
        s_resizeAlloc->~TObjectAllocHeap();
        SMemFree(s_resizeAlloc, nullptr, 0, 0);
    }
}

// ref: FUN_1405f61c0
void CLayoutFrame::AddToResizeList() {
    if (this->m_flags & FLAG_RESIZE_QUEUED) {
        return;
    }
    auto node = static_cast<RESIZENODE*>(s_resizeAlloc->Alloc());
    node->m_frame = this;
    node->m_next = s_resizeList.m_next;
    node->m_prev = &s_resizeList;
    s_resizeList.m_next = node;
    node->m_next->m_prev = node;
    s_resizeCount++;
    this->m_flags |= FLAG_RESIZE_QUEUED;
}

// Marks this frame and everything anchored to it for re-solving; true when one of them is
// shown and so needs its rect resolved.
// ref: FUN_1405f5fb0
bool CLayoutFrame::CollectResizes(blz::vector<CLayoutFrame*, s_frameTag>* out) {
    if (this->m_flags & FLAG_RECT_FAILED) {
        return false;
    }
    this->m_flags = (this->m_flags & ~FLAG_RECT_VALID) | FLAG_DIRTY;
    bool any = false;
    for (FRAMENODE& node : this->m_dependents) {
        if (node.m_frame->CollectResizes(out)) {
            any = true;
        }
    }
    if (!(this->m_layoutState & LAYOUT_VISIBLE) || any) {
        return any;
    }
    out->push_back(this);
    return true;
}

// ref: FUN_1405f9890
void CLayoutFrame::ResizePending() {
    blz::embedded_vector<CLayoutFrame*, 0x800, s_frameTag> frames;
    for (RESIZENODE* node = s_resizeList.m_prev; node != &s_resizeList; node = node->m_prev) {
        CLayoutFrame* frame = node->m_frame;
        if (frame != nullptr) {
            frame->CollectResizes(&frames);
            frame->m_flags &= ~FLAG_RESIZE_QUEUED;
        }
    }
    RESIZENODE* node = s_resizeList.m_prev;
    s_resizeList.m_prev = &s_resizeList;
    s_resizeList.m_next = &s_resizeList;
    s_resizeCount = 0;
    while (node != &s_resizeList) {
        RESIZENODE* prev = node->m_prev;
        s_resizeAlloc->Free(node);
        node = prev;
    }
    for (CLayoutFrame* frame : frames) {
        if (frame->m_flags & FLAG_DIRTY) {
            frame->OnFrameResize(0);
        }
    }
}

// Invalidates the rect of this frame and of every frame anchored to it.
// ref: FUN_1405f8a00
void CLayoutFrame::MarkDirty() {
    uint16_t flags = this->m_flags;
    if ((flags & FLAG_RECT_VALID) || !(flags & FLAG_DIRTY)) {
        this->m_flags = (flags & ~(FLAG_RECT_VALID | FLAG_RECT_FAILED)) | FLAG_DIRTY;
        for (FRAMENODE& node : this->m_dependents) {
            node.m_frame->MarkDirty();
        }
    }
}

// ref: FUN_1405f9180
void CLayoutFrame::ClearRectFailed() {
    if (this->m_flags & FLAG_RECT_FAILED) {
        this->m_flags &= ~FLAG_RECT_FAILED;
        for (FRAMENODE& node : this->m_dependents) {
            node.m_frame->ClearRectFailed();
        }
    }
}

// Solves the rect now (`now`, unless it already failed), or queues the frame to be solved
// with the rest of the pending ones.
// ref: FUN_1405f95f0
void CLayoutFrame::Resize(bool now) {
    if (!now || (this->m_flags & FLAG_RECT_FAILED)) {
        if (!(this->m_flags & FLAG_DIRTY) || !(this->m_flags & FLAG_RESIZE_QUEUED)) {
            this->MarkDirty();
            if (!(this->m_flags & FLAG_RESIZE_QUEUED)) {
                this->AddToResizeList();
            }
        }
        return;
    }
    if (!this->UpdateRect()) {
        return;
    }
    if (this->m_flags & FLAG_RESIZE_QUEUED) {
        RESIZENODE* node = s_resizeList.m_prev;
        while (node != &s_resizeList && node->m_frame != this) {
            node = node->m_prev;
        }
        node->m_frame = nullptr;
        this->m_flags &= ~FLAG_RESIZE_QUEUED;
    }
    for (FRAMENODE& node : this->m_dependents) {
        node.m_frame->Resize(false);
    }
}

// ref: FUN_1405f8fb0
bool CLayoutFrame::UpdateRect() {
    this->m_flags &= ~FLAG_DIRTY;
    CRect rect = {};
    if (!this->CalculateRect(&rect)) {
        this->m_flags |= FLAG_RECT_FAILED;
        return false;
    }
    CRect previous = this->m_rect;
    this->m_rect = rect;
    this->m_flags |= FLAG_RECT_VALID;
    this->ClearRectFailed();
    this->OnFrameSizeChanged(&this->m_rect, &previous);
    return (this->m_flags & FLAG_RECT_VALID) != 0;
}

// Solves the rect once every frame it is anchored to has one (those are solved first).
// ref: FUN_1405f9760
bool CLayoutFrame::OnFrameResize(int32_t depth) {
    if (depth > 500) {
        return false;
    }
    bool ready = true;
    for (CFramePoint* point : this->m_points) {
        if (point != nullptr && point->m_relative != nullptr) {
            CLayoutFrame* relative = point->m_relative;
            if (relative->m_flags & FLAG_DIRTY) {
                relative->OnFrameResize(depth + 1);
            }
            if (point->m_relative != nullptr && !(point->m_relative->m_flags & FLAG_RECT_VALID)) {
                ready = false;
            }
        }
    }
    if (!ready) {
        return false;
    }
    this->m_flags &= ~FLAG_DIRTY;
    CRect rect = {};
    if (!this->CalculateRect(&rect)) {
        this->m_flags |= FLAG_RECT_FAILED;
        return false;
    }
    CRect previous = this->m_rect;
    this->m_rect = rect;
    this->m_flags |= FLAG_RECT_VALID;
    this->ClearRectFailed();
    this->OnFrameSizeChanged(&this->m_rect, &previous);
    return (this->m_flags & FLAG_RECT_VALID) != 0;
}

// ---- solving the rect -----------------------------------------------------------------------

// ref: FUN_1405f85a0
bool CLayoutFrame::GetRect(CRect* rect) {
    if (this->m_flags & FLAG_RECT_VALID) {
        *rect = this->m_rect;
        return true;
    }
    return false;
}

// Each edge from its points, or from the opposite edge and the size; then kept on screen if
// clamped.
// ref: FUN_1405f6b70
bool CLayoutFrame::CalculateRect(CRect* rect) {
    rect->minX = this->CalculateLeft();
    rect->minY = this->CalculateBottom();
    rect->maxX = this->CalculateRight();
    float top = this->CalculateTop();
    rect->maxY = top;
    float left = rect->minX;
    float bottom = rect->minY;
    float right = rect->maxX;
    if (left == LAYOUT_UNSET || bottom == LAYOUT_UNSET || right == LAYOUT_UNSET || top == LAYOUT_UNSET) {
        return false;
    }
    if (top < bottom || right < left) {
        if (right < left) {
            rect->maxX = left;
            rect->minX = right;
        }
        if (top < bottom) {
            rect->maxY = bottom;
            rect->minY = top;
        }
    }
    if (!(this->m_flags & FLAG_CLAMPED)) {
        return true;
    }
    float insetLeft = 0.0f;
    float insetRight = 0.0f;
    float insetTop = 0.0f;
    float insetBottom = 0.0f;
    this->GetClampRectInsets(&insetLeft, &insetRight, &insetTop, &insetBottom);
    float scale = this->m_layoutScale;
    float minX = -(scale * insetLeft);
    float maxY = g_coordinateSpace.m_diagonalHeight - scale * insetTop;
    float minY = -(scale * insetBottom);
    float maxX = g_coordinateSpace.m_diagonalWidth - scale * insetRight;
    float x0 = rect->minX;
    float x1 = rect->maxX;
    if (x0 < minX) {
        rect->minX = minX;
        x1 = x1 - (x0 - minX);
        rect->maxX = x1;
        x0 = minX;
    }
    float y0 = rect->minY;
    if (y0 < minY) {
        rect->minY = minY;
        rect->maxY = rect->maxY - (y0 - minY);
        y0 = minY;
    }
    if (maxX < x1) {
        rect->maxX = maxX;
        rect->minX = x0 - (x1 - maxX);
    }
    float y1 = rect->maxY;
    if (maxY < y1) {
        rect->maxY = maxY;
        rect->minY = y0 - (y1 - maxY);
    }
    return true;
}

namespace {

// The first point of `slots` that resolves, read along X or Y; a point whose read looped
// restarts the search, at most ten times. `solved` is false when none resolved.
float EdgeFromPoints(CLayoutFrame* frame, const int32_t* slots, bool vertical, bool* solved) {
    for (int32_t attempt = 0; attempt < 10; attempt++) {
        bool restart = false;
        for (int32_t i = 0; i < 3; i++) {
            CFramePoint* point = frame->m_points[slots[i]];
            if (!PointUsed(point)) {
                continue;
            }
            float value = vertical ? point->Y(frame->m_layoutScale) : point->X(frame->m_layoutScale);
            if (point->m_flags & CFramePoint::FLAG_LOOP) {
                restart = true;
                break;
            }
            if (value != LAYOUT_UNSET) {
                *solved = true;
                return value;
            }
        }
        if (!restart) {
            break;
        }
    }
    *solved = false;
    return LAYOUT_UNSET;
}

// A low edge (left or bottom) from the high edge or the center, and the size.
float LowEdgeFromSize(float size, float high, float center) {
    if (high == LAYOUT_UNSET || size == 0.0f) {
        if (center == LAYOUT_UNSET) {
            return LAYOUT_UNSET;
        }
        if (size == 0.0f) {
            return high != LAYOUT_UNSET ? (center - high) + center : LAYOUT_UNSET;
        }
        return center - size * 0.5f;
    }
    return high - size;
}

// A high edge (right or top) from the low edge or the center, and the size.
float HighEdgeFromSize(float size, float low, float center) {
    if (low == LAYOUT_UNSET || size == 0.0f) {
        if (center == LAYOUT_UNSET) {
            return LAYOUT_UNSET;
        }
        if (size == 0.0f) {
            return low != LAYOUT_UNSET ? (center - low) + center : LAYOUT_UNSET;
        }
        return size * 0.5f + center;
    }
    return size + low;
}

}

// ref: FUN_1405f8c20
float CLayoutFrame::CalculateLeft() {
    if (this->m_layoutState & GUARD_LEFT) {
        return LAYOUT_UNSET;
    }
    this->m_layoutState |= GUARD_LEFT;
    bool solved;
    float result = EdgeFromPoints(this, s_leftPoints, false, &solved);
    if (!solved) {
        float scale = this->m_layoutScale;
        float width = this->GetWidth() * scale;
        float right = this->CalculateRight();
        float center = this->CalculateCenterX();
        result = LowEdgeFromSize(width, right, center);
    }
    this->m_layoutState &= ~GUARD_LEFT;
    return result;
}

// ref: FUN_1405f99f0
float CLayoutFrame::CalculateRight() {
    if (this->m_layoutState & GUARD_RIGHT) {
        return LAYOUT_UNSET;
    }
    this->m_layoutState |= GUARD_RIGHT;
    bool solved;
    float result = EdgeFromPoints(this, s_rightPoints, false, &solved);
    if (!solved) {
        float scale = this->m_layoutScale;
        float width = this->GetWidth() * scale;
        float left = this->CalculateLeft();
        float center = this->CalculateCenterX();
        result = HighEdgeFromSize(width, left, center);
    }
    this->m_layoutState &= ~GUARD_RIGHT;
    return result;
}

// ref: FUN_1405fa450
float CLayoutFrame::CalculateTop() {
    if (this->m_layoutState & GUARD_TOP) {
        return LAYOUT_UNSET;
    }
    this->m_layoutState |= GUARD_TOP;
    bool solved;
    float result = EdgeFromPoints(this, s_topPoints, true, &solved);
    if (!solved) {
        float scale = this->m_layoutScale;
        float height = this->GetHeight() * scale;
        float bottom = this->CalculateBottom();
        float center = this->CalculateCenterY();
        result = HighEdgeFromSize(height, bottom, center);
    }
    this->m_layoutState &= ~GUARD_TOP;
    return result;
}

// ref: FUN_1405f69f0
float CLayoutFrame::CalculateBottom() {
    if (this->m_layoutState & GUARD_BOTTOM) {
        return LAYOUT_UNSET;
    }
    this->m_layoutState |= GUARD_BOTTOM;
    bool solved;
    float result = EdgeFromPoints(this, s_bottomPoints, true, &solved);
    if (!solved) {
        float scale = this->m_layoutScale;
        float height = this->GetHeight() * scale;
        float top = this->CalculateTop();
        float center = this->CalculateCenterY();
        result = LowEdgeFromSize(height, top, center);
    }
    this->m_layoutState &= ~GUARD_BOTTOM;
    return result;
}

// ref: FUN_1405f6ea0
float CLayoutFrame::CalculateCenterX() {
    if (this->m_layoutState & GUARD_CENTERX) {
        return LAYOUT_UNSET;
    }
    this->m_layoutState |= GUARD_CENTERX;
    bool solved;
    float result = EdgeFromPoints(this, s_centerXPoints, false, &solved);
    if (!solved) {
        float scale = this->m_layoutScale;
        float width = this->GetWidth() * scale;
        float right = this->CalculateRight();
        float left = this->CalculateLeft();
        result = CenterFromEdges(left, right, width);
    }
    this->m_layoutState &= ~GUARD_CENTERX;
    return result;
}

// ref: FUN_1405f6fe0
float CLayoutFrame::CalculateCenterY() {
    if (this->m_layoutState & GUARD_CENTERY) {
        return LAYOUT_UNSET;
    }
    this->m_layoutState |= GUARD_CENTERY;
    bool solved;
    float result = EdgeFromPoints(this, s_centerYPoints, true, &solved);
    if (!solved) {
        float scale = this->m_layoutScale;
        float height = this->GetHeight() * scale;
        float top = this->CalculateTop();
        float bottom = this->CalculateBottom();
        result = CenterFromEdges(bottom, top, height);
    }
    this->m_layoutState &= ~GUARD_CENTERY;
    return result;
}

// ---- anchors ------------------------------------------------------------------------------

// ref: FUN_1405f6d50
int32_t CLayoutFrame::CheckAnchorFamilyConnection(CLayoutFrame* relative, int32_t point) {
    if (point >= 0 && point <= 8 && PointUsed(this->m_points[point]) && this->m_points[point]->m_relative == relative) {
        return 0;
    }
    CLayoutFrame* root = this->GetAnchorRoot(point);
    if (root == nullptr) {
        return 0;
    }
    CLayoutFrame* relativeRoot = relative->GetAnchorRoot(9);
    if (relativeRoot != nullptr && root != relativeRoot && (root != this || relativeRoot != relative)
        && ScriptRegion_IsActionRestricted(ACTION_ANCHOR_FAMILY_BRIDGING)) {
        return 3;
    }
    return 0;
}

// 0 set; 9 relative to itself, 8 relative to a frame anchored to it, 2 refused by the
// relative, 3 joining two anchor families.
// ref: FUN_1405f9f80
int32_t CLayoutFrame::SetPoint(int32_t point, CLayoutFrame* relative, int32_t relativePoint, float offsetX, float offsetY,
    bool now) {
    if (relative == this) {
        return 9;
    }
    if (relative->IsDependentOn(this)) {
        return 8;
    }
    if (!relative->CanBeAnchorFor(this)) {
        return 2;
    }
    if (this->CheckAnchorFamilyConnection(relative, point) == 3) {
        return 3;
    }
    this->ProtectScriptRegionFrom(relative);
    CFramePoint* existing = this->m_points[point];
    uint32_t mask = 1u << (point & 0x1f);
    if (existing == nullptr) {
        void* memory = s_framePointAlloc.GetData(false, s_framePointTag, -2, 0x1f);
        CFramePoint* created = nullptr;
        if (memory != nullptr) {
            created = new (memory) CFramePoint(relative, static_cast<uint8_t>(relativePoint), offsetX, offsetY);
        }
        this->m_points[point] = created;
    } else {
        CLayoutFrame* previous = existing->m_relative;
        if (previous == relative) {
            if (existing->m_relativePoint == static_cast<uint8_t>(relativePoint)
                && std::fabs(existing->m_offsetX - offsetX) < LAYOUT_EPSILON
                && std::fabs(existing->m_offsetY - offsetY) < LAYOUT_EPSILON) {
                return 0;
            }
            existing->SetPoint(relative, static_cast<uint8_t>(relativePoint), offsetX, offsetY);
            this->ClearRectFailed();
            if (now) {
                this->Resize(false);
            }
            return 0;
        }
        if (previous != nullptr) {
            previous->UnregisterResize(this, mask);
        }
        existing->SetPoint(relative, static_cast<uint8_t>(relativePoint), offsetX, offsetY);
    }
    this->ClearRectFailed();
    relative->RegisterResize(this, mask);
    if (now) {
        this->Resize(false);
    }
    return 0;
}

// TOPLEFT and BOTTOMRIGHT to the same corners of `relative`.
// ref: FUN_1405f9b60
int32_t CLayoutFrame::SetAllPoints(CLayoutFrame* relative, bool now) {
    if (relative == this) {
        return 9;
    }
    if (relative->IsDependentOn(this)) {
        return 8;
    }
    if (!relative->CanBeAnchorFor(this)) {
        return 2;
    }
    this->ProtectScriptRegionFrom(relative);
    this->ClearAllPoints();
    if (this->m_points[FRAMEPOINT_TOPLEFT] == nullptr) {
        void* memory = s_framePointAlloc.GetData(false, s_framePointTag, -2, 0x1f);
        this->m_points[FRAMEPOINT_TOPLEFT] =
            memory != nullptr ? new (memory) CFramePoint(relative, FRAMEPOINT_TOPLEFT, 0.0f, 0.0f) : nullptr;
    } else {
        this->m_points[FRAMEPOINT_TOPLEFT]->SetPoint(relative, FRAMEPOINT_TOPLEFT, 0.0f, 0.0f);
    }
    if (this->m_points[FRAMEPOINT_BOTTOMRIGHT] == nullptr) {
        void* memory = s_framePointAlloc.GetData(false, s_framePointTag, -2, 0x1f);
        this->m_points[FRAMEPOINT_BOTTOMRIGHT] =
            memory != nullptr ? new (memory) CFramePoint(relative, FRAMEPOINT_BOTTOMRIGHT, 0.0f, 0.0f) : nullptr;
    } else {
        this->m_points[FRAMEPOINT_BOTTOMRIGHT]->SetPoint(relative, FRAMEPOINT_BOTTOMRIGHT, 0.0f, 0.0f);
    }
    this->ClearRectFailed();
    relative->RegisterResize(this, 0x101);
    if (now) {
        this->Resize(false);
    }
    return 0;
}

// ref: FUN_1405f7140
void CLayoutFrame::ClearPoint(int32_t point) {
    if (point >= 0 && point < 9 && PointUsed(this->m_points[point])) {
        CFramePoint* framePoint = this->m_points[point];
        if (framePoint->m_relative != nullptr) {
            framePoint->m_relative->UnregisterResize(this, 1u << (point & 0x1f));
        }
        framePoint->MarkUnused();
    }
    for (CFramePoint* remaining : this->m_points) {
        if (PointUsed(remaining)) {
            return;
        }
    }
    this->SetAnchorRestricted(false);
}

// The points are kept (marked unused) for reuse; a restricted frame is invalidated.
// ref: FUN_1405f7c70
void CLayoutFrame::ClearAllPoints() {
    for (uint32_t i = 0; i < 9; i++) {
        CFramePoint* point = this->m_points[i];
        if (PointUsed(point)) {
            if (point->m_relative != nullptr) {
                point->m_relative->UnregisterResize(this, 1u << i);
            }
            point->MarkUnused();
        }
    }
    uint16_t flags = this->m_flags;
    if ((flags & FLAG_ANCHOR_RESTRICTED) && ((flags & FLAG_RECT_VALID) || !(flags & FLAG_DIRTY))) {
        this->m_flags = (flags & ~(FLAG_RECT_VALID | FLAG_RECT_FAILED)) | FLAG_DIRTY;
        for (FRAMENODE& node : this->m_dependents) {
            node.m_frame->MarkDirty();
        }
    }
}

// ref: FUN_1405f8390
CFramePoint* CLayoutFrame::GetPoint(int32_t point) {
    if (point >= 0 && point < 9 && this->m_points[point] != nullptr) {
        return PointUsed(this->m_points[point]) ? this->m_points[point] : nullptr;
    }
    return nullptr;
}

// ref: FUN_1405f89c0
bool CLayoutFrame::IsAnchored() {
    for (CFramePoint* point : this->m_points) {
        if (PointUsed(point)) {
            return true;
        }
    }
    return false;
}

// Whether this frame is anchored, directly or through others, to `frame`.
// ref: FUN_1405f8b90
bool CLayoutFrame::IsDependentOn(CLayoutFrame* frame) {
    CLayoutFrame* last = nullptr;
    for (CFramePoint* point : this->m_points) {
        if (!PointUsed(point)) {
            continue;
        }
        CLayoutFrame* relative = point->m_relative;
        if (relative == nullptr || relative == last) {
            continue;
        }
        if (relative == frame) {
            return true;
        }
        bool dependent = relative->IsDependentOn(frame);
        last = relative;
        if (dependent) {
            return true;
        }
    }
    return false;
}

// Records that `dependent` anchors to this frame with `points`; it takes this frame's anchor
// restriction, and this frame its protection.
// ref: FUN_1405f9340
void CLayoutFrame::RegisterResize(CLayoutFrame* dependent, uint32_t points) {
    for (FRAMENODE& node : this->m_dependents) {
        if (node.m_frame == dependent) {
            node.m_points |= points;
            return;
        }
    }
    FRAMENODE added = {};
    added.m_frame = dependent;
    added.m_points = points;
    this->m_dependents.push_back(added);
    dependent->SetAnchorRestricted((this->m_flags & FLAG_ANCHOR_RESTRICTED) != 0);
    dependent->PropagateProtectionTo(this);
}

// ref: FUN_1405faac0
void CLayoutFrame::UnregisterResize(CLayoutFrame* dependent, uint32_t points) {
    for (uint64_t i = 0; i < this->m_dependents.size(); i++) {
        FRAMENODE& node = this->m_dependents[i];
        if (node.m_frame != dependent) {
            continue;
        }
        node.m_points &= ~points;
        if (node.m_points == 0) {
            dependent->PropagateAnyProtectionTo(this);
            this->m_dependents.erase_unordered(i);
        }
        return;
    }
}

// ref: FUN_1405f6340
bool CLayoutFrame::OffsetPoints(float x, float y, bool now) {
    if (x == 0.0f && y == 0.0f) {
        return false;
    }
    uint64_t moved = 0;
    for (CFramePoint* point : this->m_points) {
        if (PointUsed(point)) {
            point->OffsetBy(x, y);
            moved++;
        }
    }
    if (moved != 0) {
        this->ClearRectFailed();
        if (now && (!(this->m_flags & FLAG_DIRTY) || !(this->m_flags & FLAG_RESIZE_QUEUED))) {
            this->MarkDirty();
            if (!(this->m_flags & FLAG_RESIZE_QUEUED)) {
                this->AddToResizeList();
            }
        }
    }
    return false;
}

// ref: FUN_1405f71d0
bool CLayoutFrame::ResetPointOffsets(bool now) {
    uint64_t reset = 0;
    for (CFramePoint* point : this->m_points) {
        if (PointUsed(point)) {
            point->ResetOffset();
            reset++;
        }
    }
    if (reset != 0) {
        this->ClearRectFailed();
        if (now && (!(this->m_flags & FLAG_DIRTY) || !(this->m_flags & FLAG_RESIZE_QUEUED))) {
            this->MarkDirty();
            if (!(this->m_flags & FLAG_RESIZE_QUEUED)) {
                this->AddToResizeList();
            }
        }
    }
    return false;
}

// Frees the points, cuts every frame anchored here loose, and leaves the resize list.
// ref: FUN_1405f7450
void CLayoutFrame::DestroyLayout() {
    uint32_t mask = 1;
    for (CFramePoint*& point : this->m_points) {
        if (point != nullptr) {
            if (!(point->m_flags & CFramePoint::FLAG_UNUSED) && point->m_relative != nullptr) {
                point->m_relative->UnregisterResize(this, mask);
            }
            s_framePointAlloc.PutData(point);
            point = nullptr;
        }
        mask = mask << 1 | mask >> 31;
    }
    for (FRAMENODE& node : this->m_dependents) {
        CLayoutFrame* dependent = node.m_frame;
        for (CFramePoint* point : dependent->m_points) {
            if (point != nullptr && point->m_relative == this) {
                point->MarkUnused();
            }
        }
        dependent->ClearRectFailed();
    }
    this->m_dependents.m_size = 0;
    for (RESIZENODE* node = s_resizeList.m_prev; node != &s_resizeList;) {
        RESIZENODE* prev = node->m_prev;
        if (node->m_frame == this) {
            node->m_next->m_prev = prev;
            node->m_prev->m_next = node->m_next;
            s_resizeAlloc->Free(node);
            s_resizeCount--;
        }
        node = prev;
    }
    this->m_flags &= ~FLAG_RESIZE_QUEUED;
}

// Re-anchors the frame by the corner (or edge midpoint, or center) of `target` nearest the
// matching one of its own, keeping its place.
// ref: FUN_1405fa5c0
bool CLayoutFrame::SnapToNearestAnchor(CLayoutFrame* target) {
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (!(this->m_flags & FLAG_RECT_VALID) || !(target->m_flags & FLAG_RECT_VALID)) {
        return false;
    }
    float bottom = this->m_rect.minY;
    float left = this->m_rect.minX;
    float top = this->m_rect.maxY;
    float right = this->m_rect.maxX;
    float targetBottom = target->m_rect.minY;
    float targetLeft = target->m_rect.minX;
    float targetTop = target->m_rect.maxY;
    float targetRight = target->m_rect.maxX;
    float targetCenterX = (targetRight - targetLeft) * 0.5f + targetLeft;
    float targetCenterY = (targetTop - targetBottom) * 0.5f + targetBottom;
    float centerX = (right - left) * 0.5f + left;
    float centerY = (top - bottom) * 0.5f + bottom;
    float leftDistance = (targetLeft - left) * (targetLeft - left);
    float topDistance = (targetTop - top) * (targetTop - top);
    float centerXDistance = (targetCenterX - centerX) * (targetCenterX - centerX);
    float rightDistance = (targetRight - right) * (targetRight - right);
    float centerYDistance = (targetCenterY - centerY) * (targetCenterY - centerY);
    float bottomDistance = (targetBottom - bottom) * (targetBottom - bottom);
    struct CANDIDATE {
        float m_distance;
        float m_x;
        float m_y;
    };
    const CANDIDATE candidates[9] = {
        {leftDistance + topDistance, left - targetLeft, top - targetTop},
        {centerXDistance + topDistance, centerX - targetCenterX, top - targetTop},
        {rightDistance + topDistance, right - targetRight, top - targetTop},
        {centerYDistance + leftDistance, left - targetLeft, centerY - targetCenterY},
        {centerXDistance + centerYDistance, centerX - targetCenterX, centerY - targetCenterY},
        {rightDistance + centerYDistance, right - targetRight, centerY - targetCenterY},
        {bottomDistance + leftDistance, left - targetLeft, bottom - targetBottom},
        {bottomDistance + centerXDistance, centerX - targetCenterX, bottom - targetBottom},
        {bottomDistance + rightDistance, right - targetRight, bottom - targetBottom},
    };
    float best = FLT_MAX;
    int32_t point = 0;
    float x = 0.0f;
    float y = 0.0f;
    for (int32_t i = 0; i < 9; i++) {
        if (candidates[i].m_distance < best) {
            best = candidates[i].m_distance;
            point = i;
            x = candidates[i].m_x;
            y = candidates[i].m_y;
        }
    }
    this->ClearAllPoints();
    float inverse = 1.0f / this->m_layoutScale;
    this->SetPoint(point, target, point, inverse * x, inverse * y, false);
    this->Resize(true);
    return true;
}

// Resizes by dragging the corner or edge `corner` by (dx, dy), honouring the resize bounds;
// dragging the center moves the frame.
// ref: FUN_1405f75f0
bool CLayoutFrame::SizeByDrag(CLayoutFrame* relative, float dx, float dy, int32_t corner) {
    float width = this->GetWidth();
    float height = this->GetHeight();
    float scale = this->m_layoutScale;
    float minWidth = this->m_minResizeWidth;
    float maxWidth = this->m_maxResizeWidth;
    float minHeight = this->m_minResizeHeight;
    float maxHeight = this->m_maxResizeHeight;
    switch (corner) {
        case FRAMEPOINT_TOPLEFT:
            width = width - (1.0f / scale) * dx;
            if (minWidth != 0.0f && width < minWidth) {
                dx = dx - (minWidth - width) * scale;
                width = minWidth;
            }
            if (maxWidth != 0.0f && maxWidth < width) {
                dx = dx + (width - maxWidth) * scale;
                width = maxWidth;
            }
            height = height + (1.0f / scale) * dy;
            if (minHeight != 0.0f && height < minHeight) {
                dy = dy + (minHeight - height) * scale;
                height = minHeight;
            }
            if (maxHeight != 0.0f && maxHeight < height) {
                dy = dy - (height - maxHeight) * scale;
                height = maxHeight;
            }
            break;
        case FRAMEPOINT_TOP:
            dx = 0.0f;
            height = height + dy / scale;
            if (minHeight != 0.0f && height < minHeight) {
                dy = dy + (minHeight - height) * scale;
                height = minHeight;
            }
            if (maxHeight != 0.0f && maxHeight < height) {
                dy = dy - (height - maxHeight) * scale;
                height = maxHeight;
            }
            break;
        case FRAMEPOINT_TOPRIGHT:
            width = width + (1.0f / scale) * dx;
            dx = 0.0f;
            if (minWidth != 0.0f && width < minWidth) {
                width = minWidth;
            }
            if (maxWidth != 0.0f && maxWidth < width) {
                width = maxWidth;
            }
            height = height + (1.0f / scale) * dy;
            if (minHeight != 0.0f && height < minHeight) {
                dy = dy + (minHeight - height) * scale;
                height = minHeight;
            }
            if (maxHeight != 0.0f && maxHeight < height) {
                dy = dy - (height - maxHeight) * scale;
                height = maxHeight;
            }
            break;
        case FRAMEPOINT_LEFT:
            dy = 0.0f;
            width = width - dx / scale;
            if (minWidth != 0.0f && width < minWidth) {
                dx = dx - (minWidth - width) * scale;
                width = minWidth;
            }
            if (maxWidth != 0.0f && maxWidth < width) {
                dx = dx + (width - maxWidth) * scale;
                width = maxWidth;
            }
            break;
        case FRAMEPOINT_CENTER: {
            CFramePoint* point = this->m_points[FRAMEPOINT_TOPLEFT];
            if (PointUsed(point) && point->m_relative != nullptr) {
                float inverse = 1.0f / scale;
                point->m_offsetX = inverse * dx + point->m_offsetX;
                point->m_offsetY = inverse * dy + point->m_offsetY;
                this->Resize(true);
                return true;
            }
            break;
        }
        case FRAMEPOINT_RIGHT:
            dy = 0.0f;
            width = width + dx / scale;
            if (minWidth != 0.0f && width < minWidth) {
                width = minWidth;
            }
            if (maxWidth != 0.0f && maxWidth < width) {
                width = maxWidth;
            }
            dx = 0.0f;
            break;
        case FRAMEPOINT_BOTTOMLEFT:
            width = width - (1.0f / scale) * dx;
            if (minWidth != 0.0f && width < minWidth) {
                dx = dx - (minWidth - width) * scale;
                width = minWidth;
            }
            if (maxWidth != 0.0f && maxWidth < width) {
                dx = dx + (width - maxWidth) * scale;
                width = maxWidth;
            }
            height = height - (1.0f / scale) * dy;
            if (minHeight != 0.0f && height < minHeight) {
                height = minHeight;
            }
            if (maxHeight != 0.0f && maxHeight < height) {
                height = maxHeight;
            }
            dy = 0.0f;
            break;
        case FRAMEPOINT_BOTTOM:
            height = height - dy / scale;
            dy = 0.0f;
            if (minHeight != 0.0f && height < minHeight) {
                height = minHeight;
            }
            if (maxHeight != 0.0f && maxHeight < height) {
                height = maxHeight;
            }
            dx = 0.0f;
            break;
        case FRAMEPOINT_BOTTOMRIGHT:
            width = width + (1.0f / scale) * dx;
            dx = 0.0f;
            if (minWidth != 0.0f && width < minWidth) {
                width = minWidth;
            }
            if (maxWidth != 0.0f && maxWidth < width) {
                width = maxWidth;
            }
            height = height - (1.0f / scale) * dy;
            if (minHeight != 0.0f && height < minHeight) {
                height = minHeight;
            }
            if (maxHeight != 0.0f && maxHeight < height) {
                height = maxHeight;
            }
            dy = 0.0f;
            break;
        default:
            break;
    }
    return this->ApplyDragSize(relative, width, height, dx, dy);
}

// Re-anchors TOPLEFT to `relative` at the dragged position and sets the size.
// ref: FUN_1405f7ad0
bool CLayoutFrame::ApplyDragSize(CLayoutFrame* relative, float width, float height, float dx, float dy) {
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (!(this->m_flags & FLAG_RECT_VALID)) {
        return false;
    }
    float bottom = this->m_rect.minY;
    float left = this->m_rect.minX;
    float top = this->m_rect.maxY;
    float right = this->m_rect.maxX;
    if (!(relative->m_flags & FLAG_RECT_VALID)) {
        return false;
    }
    float relativeLeft = relative->m_rect.minX;
    float relativeTop = relative->m_rect.maxY;
    this->ClearAllPoints();
    this->SetPoint(FRAMEPOINT_TOPLEFT, relative, FRAMEPOINT_TOPLEFT,
        (relativeLeft - (left + dx)) * (-1.0f / this->m_layoutScale), -((relativeTop - (top + dy)) / this->m_layoutScale),
        false);
    if (width == 0.0f) {
        width = (right - left) / this->m_layoutScale;
    }
    this->SetWidth(width);
    if (height == 0.0f) {
        height = (top - bottom) / this->m_layoutScale;
    }
    this->SetHeight(height);
    this->Resize(true);
    return true;
}

// ---- sizing, scale and clamping -----------------------------------------------------------

// ref: FUN_1405f8960
float CLayoutFrame::GetWidthActual(bool ignoreRect) {
    if (!ignoreRect) {
        if (this->m_flags & FLAG_DIRTY) {
            this->Resize(true);
        }
        if (this->m_flags & FLAG_RECT_VALID) {
            return (this->m_rect.maxX - this->m_rect.minX) / this->m_layoutScale;
        }
    }
    return this->GetWidth();
}

// ref: FUN_1405f81b0
float CLayoutFrame::GetHeightActual(bool ignoreRect) {
    if (!ignoreRect) {
        if (this->m_flags & FLAG_DIRTY) {
            this->Resize(true);
        }
        if (this->m_flags & FLAG_RECT_VALID) {
            return (this->m_rect.maxY - this->m_rect.minY) / this->m_layoutScale;
        }
    }
    return this->GetHeight();
}

// ref: FUN_1405fa2d0
void CLayoutFrame::SetResizeBounds(float minWidth, float minHeight, const OPTIONALBOUND* maxWidth,
    const OPTIONALBOUND* maxHeight) {
    float width = maxWidth->m_set ? maxWidth->m_value : 0.0f;
    float height = maxHeight->m_set ? maxHeight->m_value : 0.0f;
    this->m_minResizeHeight = minHeight;
    this->m_minResizeWidth = minWidth;
    this->m_maxResizeHeight = height;
    this->m_maxResizeWidth = width;
}

// ref: FUN_1405f86a0
void CLayoutFrame::GetResizeBounds(float* minWidth, float* minHeight, float* maxWidth, float* maxHeight) {
    *minWidth = this->m_minResizeWidth;
    *minHeight = this->m_minResizeHeight;
    *maxWidth = this->m_maxResizeWidth;
    *maxHeight = this->m_maxResizeHeight;
}

// ref: FUN_1405f9ee0
bool CLayoutFrame::SetLayoutDepth(float depth, bool force) {
    if (depth <= 0.2f) {
        depth = 0.2f;
    }
    if (!force && std::fabs(depth - this->m_layoutDepth) < LAYOUT_EPSILON) {
        return false;
    }
    this->m_layoutDepth = depth;
    this->OnLayoutDepthChanged();
    return true;
}

// ref: FUN_1405f9e10
void CLayoutFrame::SetClampedToScreen(bool clamped) {
    if (clamped == ((this->m_flags & FLAG_CLAMPED) != 0)) {
        return;
    }
    if (clamped && this->GetScriptRegion() != nullptr
        && !ScriptRegion_CheckAction(ACTION_CLAMPED_TO_SCREEN, this->GetScriptRegion(), "SetClampedToScreen")) {
        return;
    }
    if (clamped) {
        this->m_flags |= FLAG_CLAMPED;
    } else {
        this->m_flags &= ~FLAG_CLAMPED;
    }
    this->Resize(false);
}

// ---- restriction and protection -----------------------------------------------------------

// ref: FUN_1405f8a70
bool CLayoutFrame::IsAnchorRestricted() {
    return (this->m_flags & FLAG_ANCHOR_RESTRICTED) != 0;
}

// Spreads to every frame anchored here; an anchor root keeps its own.
// ref: FUN_1405f9d60
void CLayoutFrame::SetAnchorRestricted(bool restricted) {
    uint16_t flags = this->m_flags;
    if ((flags & FLAG_ANCHOR_ROOT) || restricted == ((flags & FLAG_ANCHOR_RESTRICTED) != 0)) {
        return;
    }
    this->m_flags = restricted ? (flags | FLAG_ANCHOR_RESTRICTED) : (flags & ~FLAG_ANCHOR_RESTRICTED);
    for (FRAMENODE& node : this->m_dependents) {
        node.m_frame->SetAnchorRestricted(restricted);
    }
    this->OnAnchorRestrictedChanged(restricted);
}

// ref: FUN_1405f9d30
void CLayoutFrame::SetAnchorRoot(bool root) {
    if (root) {
        this->m_flags |= FLAG_ANCHOR_ROOT;
    } else {
        this->m_flags &= ~FLAG_ANCHOR_ROOT;
    }
}

// Protected explicitly, or (re-evaluated when marked) through a protected dependent.
// ref: FUN_1405f8a80
bool CLayoutFrame::IsProtected() {
    uint16_t flags = this->m_flags;
    if (flags & FLAG_PROTECT_SET) {
        this->m_flags = flags | FLAG_PROTECT_BUSY;
        bool cycle = false;
        if (this->IsProtectedSelf(&cycle)) {
            this->m_flags |= FLAG_PROTECT_PROPAGATED;
        } else {
            for (FRAMENODE& node : this->m_dependents) {
                if (node.m_frame == nullptr) {
                    continue;
                }
                if (node.m_frame->m_flags & FLAG_PROTECT_BUSY) {
                    cycle = true;
                } else if (node.m_frame->IsProtected()) {
                    this->m_flags |= FLAG_PROTECT_PROPAGATED;
                    break;
                }
            }
        }
        uint16_t current = this->m_flags;
        flags = current & ~FLAG_PROTECT_BUSY;
        this->m_flags = flags;
        if (!cycle || (current & FLAG_PROTECT_PROPAGATED)) {
            flags = current & ~(FLAG_PROTECT_BUSY | FLAG_PROTECT_SET);
            this->m_flags = flags;
        }
    }
    return (flags & (FLAG_PROTECTED | FLAG_PROTECT_PROPAGATED)) != 0;
}

// ref: FUN_1405f8b60
void CLayoutFrame::GetProtectedState(bool* explicitly, bool* propagated) {
    *explicitly = (this->m_flags & FLAG_PROTECTED) != 0;
    *propagated = this->IsProtected();
}

// ref: FUN_1405fa200
void CLayoutFrame::SetProtectFlag(uint16_t flag) {
    if (this->m_flags & (FLAG_PROTECTED | FLAG_PROTECT_BUSY)) {
        return;
    }
    this->m_flags = (this->m_flags & ~FLAG_PROTECT_PROPAGATED) | flag | FLAG_PROTECT_BUSY;
    this->PropagateProtectFlags(flag);
    this->m_flags &= ~FLAG_PROTECT_BUSY;
}

// ref: FUN_1405fa250
void CLayoutFrame::SetProtected() {
    this->m_flags = (this->m_flags & ~(FLAG_PROTECT_PROPAGATED | FLAG_PROTECT_SET)) | FLAG_PROTECTED;
    this->OnProtectFlagsChanged(FLAG_PROTECT_PROPAGATED);
    for (CFramePoint* point : this->m_points) {
        if (PointUsed(point) && point->m_relative != nullptr) {
            point->m_relative->SetProtectFlag(FLAG_PROTECT_PROPAGATED);
        }
    }
}

// ref: FUN_1405f9290
void CLayoutFrame::PropagateProtectFlags(uint16_t flag) {
    this->OnProtectFlagsChanged(flag);
    for (CFramePoint* point : this->m_points) {
        if (PointUsed(point) && point->m_relative != nullptr) {
            point->m_relative->SetProtectFlag(flag);
        }
    }
}

// ref: FUN_1405f9050
void CLayoutFrame::PropagateProtectionTo(CLayoutFrame* target) {
    if ((this->m_flags & FLAG_PROTECT_SET) && !(target->m_flags & (FLAG_PROTECTED | FLAG_PROTECT_BUSY))) {
        target->m_flags = (target->m_flags & ~FLAG_PROTECT_PROPAGATED) | FLAG_PROTECT_SET | FLAG_PROTECT_BUSY;
        target->PropagateProtectFlags(FLAG_PROTECT_SET);
        target->m_flags &= ~FLAG_PROTECT_BUSY;
    }
    if ((this->m_flags & (FLAG_PROTECTED | FLAG_PROTECT_PROPAGATED))
        && !(target->m_flags & (FLAG_PROTECTED | FLAG_PROTECT_BUSY))) {
        target->m_flags |= FLAG_PROTECT_PROPAGATED | FLAG_PROTECT_BUSY;
        target->PropagateProtectFlags(FLAG_PROTECT_PROPAGATED);
        target->m_flags &= ~FLAG_PROTECT_BUSY;
    }
}

// ref: FUN_1405f90f0
void CLayoutFrame::PropagateAnyProtectionTo(CLayoutFrame* target) {
    if ((this->m_flags & (FLAG_PROTECTED | FLAG_PROTECT_PROPAGATED | FLAG_PROTECT_SET))
        && !(target->m_flags & (FLAG_PROTECTED | FLAG_PROTECT_BUSY))) {
        target->m_flags = (target->m_flags & ~FLAG_PROTECT_PROPAGATED) | FLAG_PROTECT_SET | FLAG_PROTECT_BUSY;
        target->PropagateProtectFlags(FLAG_PROTECT_SET);
        target->m_flags &= ~FLAG_PROTECT_BUSY;
    }
}

// Anchoring to a protected (forbidden) region protects this one's script object.
// ref: FUN_1405f91e0
void CLayoutFrame::ProtectScriptRegionFrom(CLayoutFrame* relative) {
    CScriptRegion* region = this->GetScriptRegion();
    if (region == nullptr) {
        return;
    }
    CScriptRegion* relativeRegion = relative->GetScriptRegion();
    if (relativeRegion == nullptr) {
        return;
    }
    uint16_t flags = relativeRegion->FrameScript_Object::m_flags;
    if (((flags & FSOBJ_FORBIDDEN) || (flags & FSOBJ_PROTECTED_METATABLE)) && (flags & FSOBJ_PROTECTED)) {
        region->FrameScript_Object::SetProtected();
    }
}

// ref: FUN_1405f9230
void CLayoutFrame::ProtectDependents() {
    for (FRAMENODE& node : this->m_dependents) {
        CScriptRegion* region = node.m_frame->GetScriptRegion();
        if (region == nullptr) {
            node.m_frame->ProtectDependents();
        } else {
            region->FrameScript_Object::SetProtected();
        }
    }
}

// ---- animation overrides ------------------------------------------------------------------

// ref: FUN_1405f6660
uint8_t CLayoutFrame::GetAnimAlpha() {
    return this->m_animData != nullptr ? this->m_animData->m_alpha : 0xff;
}

// ref: FUN_1405f6680
float* CLayoutFrame::GetAnimRectOffset() {
    return this->m_animData != nullptr ? this->m_animData->m_rectOffset : nullptr;
}

// ref: FUN_1405f66a0
bool CLayoutFrame::HasAnimAlpha() {
    return this->m_animData != nullptr && (this->m_animData->m_flags & 4);
}

// ref: FUN_1405f66c0
bool CLayoutFrame::HasAnimTransform() {
    return this->m_animData != nullptr && (this->m_animData->m_flags & 1);
}

// ref: FUN_1405f66e0
bool CLayoutFrame::HasAnimRectOffset() {
    return this->m_animData != nullptr && (this->m_animData->m_flags & 2);
}

// ref: FUN_1405f6790
void CLayoutFrame::SetAnimAlpha(uint8_t alpha) {
    CAnimationData* data = EnsureAnimData(this);
    data->m_flags |= 4;
    data->m_alpha = alpha;
}

// ref: FUN_1405f6830
void CLayoutFrame::SetAnimTransform(const ANIMTRANSFORM* transform) {
    CAnimationData* data = EnsureAnimData(this);
    data->m_flags |= 1;
    data->m_transform = *transform;
}

// ref: FUN_1405f6920
void CLayoutFrame::SetAnimRectOffset(const float* offset) {
    CAnimationData* data = EnsureAnimData(this);
    data->m_flags |= 2;
    std::memcpy(data->m_rectOffset, offset, sizeof(data->m_rectOffset));
}

// ---- the Lua-facing queries ---------------------------------------------------------------

// ref: FUN_1405f8030
bool CLayoutFrame::GetBottom(float* bottom) {
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetBottom()")) {
        return false;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        *bottom = this->m_rect.minY / this->m_layoutScale;
        return true;
    }
    return false;
}

// ref: FUN_1405f88d0
bool CLayoutFrame::GetTop(float* top) {
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetTop()")) {
        return false;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        *top = this->m_rect.maxY / this->m_layoutScale;
        return true;
    }
    return false;
}

// ref: FUN_1405f8210
bool CLayoutFrame::GetLeft(float* left) {
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetLeft()")) {
        return false;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        *left = this->m_rect.minX / this->m_layoutScale;
        return true;
    }
    return false;
}

// ref: FUN_1405f86d0
bool CLayoutFrame::GetRight(float* right) {
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetRight()")) {
        return false;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        *right = this->m_rect.maxX / this->m_layoutScale;
        return true;
    }
    return false;
}

// ref: FUN_1405f80b0
bool CLayoutFrame::GetCenter(float* x, float* y) {
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetCenter()")) {
        return false;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        float bottom = this->m_rect.minY;
        float halfHeight = (this->m_rect.maxY - bottom) * 0.5f;
        *x = ((this->m_rect.maxX - this->m_rect.minX) * 0.5f + this->m_rect.minX) / this->m_layoutScale;
        *y = (halfHeight + bottom) / this->m_layoutScale;
        return true;
    }
    return false;
}

// ref: FUN_1405f85c0
bool CLayoutFrame::GetRectLua(float* left, float* bottom, float* width, float* height) {
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetRect()")) {
        return false;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        float inverse = 1.0f / this->m_layoutScale;
        *left = inverse * this->m_rect.minX;
        *bottom = inverse * this->m_rect.minY;
        *width = (this->m_rect.maxX - this->m_rect.minX) * inverse;
        *height = (this->m_rect.maxY - this->m_rect.minY) * inverse;
        return true;
    }
    return false;
}

// ref: FUN_1405f8750
bool CLayoutFrame::GetScaledRect(float* left, float* bottom, float* width, float* height) {
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "GetScaledRect()")) {
        return false;
    }
    if (this->m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    if (this->m_flags & FLAG_RECT_VALID) {
        *left = this->m_rect.minX;
        *bottom = this->m_rect.minY;
        *width = this->m_rect.maxX - this->m_rect.minX;
        *height = this->m_rect.maxY - this->m_rect.minY;
        return true;
    }
    return false;
}

// ref: FUN_1405f88b0
void CLayoutFrame::GetSizeLua(bool ignoreRect, float* width, float* height) {
    this->GetSize(width, height, ignoreRect);
}

// ref: FUN_1405f8290
int32_t CLayoutFrame::GetNumPoints() {
    if (!this->CanBeQueried()) {
        FrameScript_ActionError("Wrong object type for function");
        return 0;
    }
    int32_t count = 0;
    for (CFramePoint* point : this->m_points) {
        if (PointUsed(point)) {
            count++;
        }
    }
    return count;
}

namespace {

void FillPointInfo(CFramePoint* point, int32_t index, CLayoutFrame::POINTINFO* info) {
    info->m_point = index;
    info->m_offsetX = point->m_offsetX;
    info->m_offsetY = point->m_offsetY;
    if (!(point->m_flags & 1)) {
        if (static_cast<void*>(point->m_relative) != static_cast<void*>(CSimpleTop::s_instance)) {
            info->m_relativeTo = point->m_relative->GetScriptRegion();
        }
        info->m_relativePoint = static_cast<int8_t>(point->m_relativePoint);
    }
}

}

// The `index`th set point.
// ref: FUN_1405f83f0
bool CLayoutFrame::GetPointInfo(int32_t index, POINTINFO* info) {
    if (!this->CanBeQueried()) {
        FrameScript_ActionError("Wrong object type for function");
        return false;
    }
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptObject(), "GetPoint()")) {
        return false;
    }
    int32_t found = 0;
    for (int32_t i = 0; i < 9; i++) {
        CFramePoint* point = this->m_points[i];
        if (!PointUsed(point)) {
            continue;
        }
        if (found == index) {
            FillPointInfo(point, i, info);
            return true;
        }
        found++;
    }
    return false;
}

// ref: FUN_1405f84d0
bool CLayoutFrame::GetPointInfoByName(int32_t point, POINTINFO* info) {
    if (!this->CanBeQueried()) {
        FrameScript_ActionError("Wrong object type for function");
        return false;
    }
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptObject(), "GetPointByName()")) {
        return false;
    }
    if (point < 0 || point >= 9 || !PointUsed(this->m_points[point])) {
        return false;
    }
    FillPointInfo(this->m_points[point], point, info);
    return true;
}

// ---- restrictions -------------------------------------------------------------------------

// ref: FUN_14062d0c0
const char* RestrictedActionToString(int32_t action) {
    switch (action) {
        case ACTION_ANCHOR_FAMILY_BRIDGING:
            return "AnchorFamilyBridging";
        case ACTION_FRAME_MEASUREMENT:
            return "FrameMeasurement";
        case ACTION_CLAMPED_TO_SCREEN:
            return "ClampedToScreen";
        case ACTION_SET_POINT:
            return "SetPoint";
        case ACTION_SET_MOVABLE:
            return "SetMovable";
        case ACTION_SET_RESIZABLE:
            return "SetResizable";
        default:
            return "UnknownAction";
    }
}

// ref: FUN_14062d130
const char* RestrictedReasonToString(int32_t reason) {
    switch (reason) {
        case 0:
        case 1:
            return "";
        case 2:
            return "Failure";
        case 3:
            return "SetPoint would result in anchor family connection";
        case 4:
            return "Can't measure restricted regions";
        case 5:
            return "Can't clamp restricted regions";
        case 6:
            return "Can't move restricted frames";
        case 7:
            return "Can't resize restricted frames";
        case 8:
            return "Cannot anchor to a region dependent on it";
        case 9:
            return "Cannot anchor to itself";
        default:
            return "UnknownAction";
    }
}

// ref: FUN_14062d1d0
bool ScriptRegion_IsActionRestricted(int32_t action) {
    if (action != ACTION_ANCHOR_FAMILY_BRIDGING && action == ACTION_FRAME_MEASUREMENT) {
        return lua_taint != nullptr;
    }
    return true;
}

// ref: FUN_14062d000
int32_t ScriptRegion_CheckRestriction(int32_t action, CScriptRegion* region) {
    CLayoutFrame* layout = static_cast<CLayoutFrame*>(region);
    switch (action) {
        case ACTION_ANCHOR_FAMILY_BRIDGING:
            return 3;
        case ACTION_FRAME_MEASUREMENT:
            if (lua_taint == nullptr) {
                return 0;
            }
            return layout->IsAnchorRestricted() ? 4 : 0;
        case ACTION_CLAMPED_TO_SCREEN:
            return layout->IsAnchorRestricted() ? 5 : 0;
        case ACTION_SET_MOVABLE:
            return layout->IsAnchorRestricted() ? 6 : 0;
        case ACTION_SET_RESIZABLE:
            return layout->IsAnchorRestricted() ? 7 : 0;
        default:
            return 2;
    }
}

// ref: FUN_14062cb70
bool ScriptRegion_CheckAction(int32_t action, CScriptRegion* region, const char* method) {
    if (region == nullptr) {
        FrameScript_ActionError("Invalid region passed to check report with method name: %s", method);
        return false;
    }
    int32_t reason = ScriptRegion_CheckRestriction(action, region);
    if (reason > 1) {
        const char* name = region->GetName();
        if (name == nullptr) {
            name = region->GetObjectTypeName();
        }
        FrameScript_ActionError("Action[%s] failed because[%s]: attempted from: %s:%s.", RestrictedActionToString(action),
            RestrictedReasonToString(reason), name, method);
    }
    return reason == 0;
}

// ref: FUN_14062d200
void ScriptRegion_RaiseRestriction(int32_t action, int32_t reason, CScriptRegion* region, const char* method, lua_State* L) {
    if (reason > 1) {
        const char* name = region->GetName();
        if (name == nullptr) {
            name = region->GetObjectTypeName();
        }
        luaL_error(L, "Action[%s] failed because[%s]: attempted from: %s:%s.", RestrictedActionToString(action),
            RestrictedReasonToString(reason), name, method);
    }
}
