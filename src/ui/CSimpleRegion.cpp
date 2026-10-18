#include "ui/CSimpleRegion.hpp"

#include "base/DataAllocator.hpp"
#include "blz/Vector.hpp"
#include "storm/Memory.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "util/Unimplemented.hpp"

int64_t CSimpleRegion::s_count;

namespace {

const char s_regionVectorTag[] = ".PEAVCSimpleRegion@@";

// The gradients' pool (0x142e99d00).
CDataAllocator s_gradientHeap(sizeof(CGradient), 0x80);  // DAT_142e99d00

// The regions whose deletion waits for a frame to finish walking its list (0x142e99ce8).
blz::vector<CSimpleRegion*, s_regionVectorTag> s_deferredDeletes;

}

// ref: FUN_1406309c0
CSimpleRegion::CSimpleRegion(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show) {
    this->m_regionFlags = 0;
    this->m_color = 0xffffffff;
    this->m_scale = 1.0f;
    this->m_alpha = 0xff;
    this->m_drawLayer = 0;
    this->m_subLevel = 0;
    s_count++;
    if (frame != nullptr) {
        this->SetFrame(frame, drawLayer, subLevel, show);
        this->SetSourceLocation(frame->GetScriptLocation());
    }
}

// ref: FUN_140630ab0
CSimpleRegion::~CSimpleRegion() {
    this->SetFrame(nullptr, DRAWLAYER_BORDER, 0, false);
    if (this->m_regionFlags & REGIONFLAG_GRADIENT) {
        // FUN_1406315d0(this, false), inlined
        if (this->m_gradient != nullptr) {
            s_gradientHeap.PutData(this->m_gradient);
        }
        this->m_regionFlags &= ~REGIONFLAG_GRADIENT;
        this->m_alpha = 0xff;
        this->m_color = 0xffffffff;
        this->MarkFrameBatchDirty();
    }
    s_count--;
    this->m_drawLink.Unlink();
    this->m_regionLink.Unlink();
}

// ref: FUN_1406149c0
CAnimatableObject* CSimpleRegion::GetAnimatableObject() {
    return this;
}

// ref: FUN_140623dc0
void CSimpleRegion::DumpHierarchy(uint32_t indent, HIERARCHYDUMPFUNC dump, void* context) {
    char line[0x80] = {};
    if (indent != 0) {
        memset(line, '.', indent);
    }
    alignas(blz::string) uint8_t storage[sizeof(blz::string)];
    blz::string* name = this->GetDebugName(reinterpret_cast<blz::string*>(storage), false);
    int32_t length = static_cast<int32_t>(SStrPrintf(line + indent, 0x80 - indent, "Region %s\n", name->m_data));
    name->~string();
    dump(line, length + static_cast<int32_t>(indent), context);
}

// The region's settings, then the animations under it.
// ref: FUN_140631090
void CSimpleRegion::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CScriptRegion::LoadXML(node, status, intrinsic);
    CSimpleFrame_LoadAnimations(this, node, status);
}

// ref: FUN_140628ac0
void CSimpleRegion::SetParentArg(PARENTARG* parent) {
    CSimpleFrame* frame = parent->m_set ? parent->m_frame : nullptr;
    this->SetFrame(frame, this->m_drawLayer, this->m_subLevel, (this->m_layoutState & LAYOUT_SHOWN) != 0);
    parent->m_set = false;
}

// ref: FUN_140626640
void CSimpleRegion::Hide() {
    this->m_layoutState &= ~LAYOUT_SHOWN;
    this->ApplyHide();
}

// ref: FUN_140629050
void CSimpleRegion::Show() {
    this->m_layoutState |= LAYOUT_SHOWN;
    this->ApplyShow();
}

// ref: FUN_14061ba30
void CSimpleRegion::SetShown(bool shown) {
    if (shown) {
        this->Show();
        return;
    }
    this->Hide();
}

// ref: FUN_140628590
void CSimpleRegion::SetAlpha(uint8_t alpha) {
    if (this->m_regionFlags & REGIONFLAG_GRADIENT) {
        return;
    }
    uint32_t color = 0;
    this->GetVertexColor(&color);
    color = (color & 0x00ffffff) | (static_cast<uint32_t>(alpha) << 24);
    this->SetVertexColor(&color);
}

// ref: FUN_140624d20
uint8_t CSimpleRegion::GetAlpha() {
    uint32_t color = 0;
    this->GetVertexColor(&color);
    return static_cast<uint8_t>(color >> 24);
}

// ref: FUN_1406318a0
void CSimpleRegion::SetVertexColor(uint32_t rgb, const uint8_t* alpha) {
    uint8_t a;
    if (alpha[1] != 0) {
        a = alpha[0];
    } else {
        a = (this->m_regionFlags & REGIONFLAG_GRADIENT) ? 0xff : this->m_alpha;
    }
    uint32_t color = (rgb & 0x00ffffff) | (static_cast<uint32_t>(a) << 24);
    this->SetHasGradient(false);
    if (this->m_alpha != a || ((color ^ this->m_color) & 0x00ffffff) != 0) {
        this->m_alpha = a;
        this->m_color = color;
        this->UpdateEffectiveAlpha();
    }
}

// ref: FUN_1406310d0
void CSimpleRegion::UpdateEffectiveAlpha() {
    CSimpleFrame* frame = this->m_parent;
    if (frame == nullptr) {
        return;
    }
    uint8_t parentAlpha;
    if (this->m_regionFlags & REGIONFLAG_IGNORE_PARENT_ALPHA) {
        parentAlpha = 0xff;
    } else {
        parentAlpha = frame->GetEffectiveAlpha();
    }
    if (!(this->m_regionFlags & REGIONFLAG_GRADIENT)) {
        uint32_t alpha = static_cast<uint32_t>(this->m_alpha) * parentAlpha / 0xff;
        this->m_color = (this->m_color & 0x00ffffff) | (alpha << 24);
        return;
    }
    // each corner's colour carries its own alpha times the frame's
    CGradient* gradient = this->m_gradient;
    for (uint32_t i = gradient->m_capacity; i != 0; i--) {
        uint32_t alpha = static_cast<uint32_t>(gradient->m_alpha[i - 1]) * parentAlpha / 0xff;
        gradient->m_colors[i - 1] = (gradient->m_colors[i - 1] & 0x00ffffff) | (alpha << 24);
    }
}

void CSimpleRegion::Unknown290() {
}

// ref: FUN_140631320
void CSimpleRegion::SetDrawLayer(uint8_t layer, int8_t subLevel) {
    if (static_cast<uint8_t>(subLevel + 8) < 0x10) {
        this->SetFrame(this->m_parent, layer, subLevel, (this->m_layoutState & LAYOUT_SHOWN) != 0);
        return;
    }
    FrameScript_ObjectError(this, "CSimpleRegion::SetDrawLayerScript", "SetDrawLayer: sublevel must be between %d and %d!", -8, 7);
}

// ref: FUN_140625050
void CSimpleRegion::GetDrawLayer(uint32_t* layer, int8_t* subLevel) {
    *layer = this->m_drawLayer;
    *subLevel = this->m_subLevel;
}

// ref: FUN_140630fd0
uint64_t CSimpleRegion::GetOwnDrawOrderKey() {
    if (this->m_parent == nullptr) {
        return 0;
    }
    return this->m_parent->GetDrawOrderKey(this->m_drawLayer, this->m_subLevel);
}

void CSimpleRegion::Unknown2B8() {
}

void CSimpleRegion::Unknown2C0() {
}

void CSimpleRegion::Unknown2C8() {
}

void CSimpleRegion::Unknown2D0() {
}

// ref: FUN_140631740
bool CSimpleRegion::SetLayoutScale(float scale, bool force) {
    (void)force;
    return this->CLayoutFrame::SetLayoutScale(scale * this->m_scale, false);
}

// ref: FUN_140630e60
bool CSimpleRegion::CanBeAnchorFor(CLayoutFrame* frame) {
    return !frame->IsProtected();
}

// ref: FUN_140631190
void CSimpleRegion::OnLayoutReady() {
    if ((this->m_layoutState & LAYOUT_VISIBLE) && this->m_parent != nullptr && this->IsLayoutReady()) {
        this->m_parent->MarkBatchDirty();
    }
}

// The region leaves its frame's drawing and its input, and OnHide is queued.
// ref: FUN_140631030
void CSimpleRegion::ApplyHide() {
    if ((this->m_layoutState & LAYOUT_VISIBLE) && this->m_parent != nullptr) {
        // FUN_1405eeef0
        this->UnregisterEnabledInput(this->HasInputFlags(REGION_PROPAGATE_MOUSE));
        this->m_parent->RemoveDrawRegion(this);
        this->m_layoutState &= ~LAYOUT_VISIBLE;
        this->QueueOnHide();
    }
}

// Shown, with a visible frame: the region joins its frame's drawing and its input, and OnShow
// is queued.
// ref: FUN_1406319b0
void CSimpleRegion::ApplyShow() {
    CSimpleFrame* frame = this->m_parent;
    if (!(this->m_layoutState & LAYOUT_SHOWN) || frame == nullptr || !(frame->m_layoutState & LAYOUT_VISIBLE)
        || (this->m_layoutState & LAYOUT_VISIBLE)) {
        return;
    }
    if (!(static_cast<FrameScript_Object*>(frame)->m_flags & FSOBJ_LOADING) && (this->CLayoutFrame::m_flags & FLAG_DIRTY)) {
        this->CLayoutFrame::Resize(true);
    }
    frame->AddDrawRegion(this);
    this->m_layoutState |= LAYOUT_VISIBLE;
    // FUN_1405f3b20
    this->RegisterEnabledInput();
    this->QueueOnShow();
}

// The first group to play makes the layout resize now; the alpha it starts from is captured.
// ref: FUN_140630c90
void CSimpleRegion::OnAnimStarted(bool alpha, bool transform, bool rectOffset) {
    (void)rectOffset;
    if (this->m_groups == nullptr || this->m_groups->m_playing == nullptr || this->m_groups->m_playing->Head() == nullptr) {
        // FUN_1405f6700
        this->CLayoutFrame::Resize(true);
    }
    if (alpha) {
        this->CLayoutFrame::SetAnimAlpha(this->GetAlpha());
    }
    if (transform) {
        this->Unknown2C0();
    }
}

// A group that sets its final alpha leaves it behind; each override no group animates any
// more is dropped (the alpha one back into the region's own alpha).
// ref: FUN_140630d40
void CSimpleRegion::OnAnimStopped(CSimpleAnimGroup* group, bool alpha, bool transform, bool rectOffset) {
    uint8_t flags = CSimpleAnimGroup_GetFlags(group);
    if (!(flags & 8) && (flags & 0x10)) {
        uint8_t current = this->CLayoutFrame::HasAnimAlpha() ? this->CLayoutFrame::GetAnimAlpha() : this->m_alpha;
        this->CLayoutFrame::SetAnimAlpha(CSimpleAnimGroup_GetFinalAlpha(group, this, current));
    }
    if (alpha && this->CLayoutFrame::HasAnimAlpha()) {
        this->SetAlpha(this->CLayoutFrame::GetAnimAlpha());
        this->CLayoutFrame::ClearAnimAlpha();
    }
    if (transform) {
        this->CLayoutFrame::ClearAnimTransform();
    }
    if (rectOffset) {
        this->CLayoutFrame::ClearAnimRectOffset();
    }
}

// ref: FUN_140631230
void CSimpleRegion::SetAnimAlpha(uint8_t alpha) {
    uint32_t color;
    if (!(this->m_regionFlags & REGIONFLAG_GRADIENT)) {
        color = (this->m_color & 0x00ffffff) | (static_cast<uint32_t>(alpha) << 24);
    } else {
        color = 0x00ffffff | (static_cast<uint32_t>(alpha) << 24);
        if (this->m_gradient != nullptr) {
            s_gradientHeap.PutData(this->m_gradient);
        }
        this->m_regionFlags &= ~REGIONFLAG_GRADIENT;
        this->m_alpha = 0xff;
        this->m_color = 0xffffffff;
        this->MarkFrameBatchDirty();
    }
    if (this->m_alpha != alpha || ((color ^ this->m_color) & 0x00ffffff) != 0) {
        this->m_alpha = alpha;
        this->m_color = color;
        this->UpdateEffectiveAlpha();
    }
}

// ref: FUN_1406149e0
CScriptRegion* CSimpleRegion::GetScriptRegion() {
    return this;
}

// ref: FUN_140631380
void CSimpleRegion::SetFrame(CSimpleFrame* frame, uint8_t drawLayer, int8_t subLevel, bool show) {
    if (this->m_parent == frame) {
        if (this->m_drawLayer == drawLayer && this->m_subLevel == subLevel) {
            if (show == ((this->m_layoutState & LAYOUT_SHOWN) != 0)) {
                return;
            }
            if (!show) {
                this->Hide();
                return;
            }
        } else {
            if (this->m_layoutState & LAYOUT_SHOWN) {
                this->Hide();
            }
            this->m_drawLayer = drawLayer;
            this->m_subLevel = subLevel;
            if (!show) {
                return;
            }
        }
        this->Show();
        return;
    }
    if (this->m_parent != nullptr) {
        this->CLayoutFrame::ApplyHide();
        this->m_parent->RemoveRegion(this);
    }
    PARENTARG parent = {frame, true};
    this->CScriptRegion::SetParentArg(&parent);
    this->m_drawLayer = drawLayer;
    this->m_subLevel = subLevel;
    if (this->m_parent == nullptr) {
        return;
    }
    this->m_parent->AddRegion(this);
    float scale = (this->m_regionFlags & REGIONFLAG_IGNORE_PARENT_SCALE) ? 1.0f : this->m_parent->m_layoutScale;
    this->CLayoutFrame::SetLayoutScale(scale, false);
    this->CLayoutFrame::SetLayoutDepth(this->m_parent->m_layoutDepth, false);
    this->UpdateEffectiveAlpha();
    if (!show) {
        this->Hide();
        return;
    }
    this->Show();
}

// ref: FUN_140631000
bool CSimpleRegion::GetVertexColor(uint32_t* argb) {
    if (this->m_regionFlags & REGIONFLAG_GRADIENT) {
        *argb = 0xffffffff;
        return true;
    }
    *argb = (this->m_color & 0x00ffffff) | (static_cast<uint32_t>(this->m_alpha) << 24);
    return true;
}

// ref: FUN_1406317d0
void CSimpleRegion::SetVertexColor(const uint32_t* argb) {
    this->SetHasGradient(false);
    uint8_t alpha = static_cast<uint8_t>(*argb >> 24);
    if (this->m_alpha != alpha || ((this->m_color ^ *argb) & 0x00ffffff) != 0) {
        this->m_alpha = alpha;
        this->m_color = *argb;
        this->UpdateEffectiveAlpha();
    }
}

// Orientation 0 runs the colours across (left min, right max), anything else up and down.
// ref: FUN_140631510
void CSimpleRegion::SetGradient(int32_t orientation, const uint32_t* minColor, const uint32_t* maxColor) {
    this->SetHasGradient(true);
    CGradient* gradient = this->m_gradient;
    const uint32_t* first = orientation == 0 ? minColor : maxColor;
    const uint32_t* second = orientation == 0 ? maxColor : minColor;
    if (orientation == 0) {
        gradient->m_alpha[0] = static_cast<uint8_t>(*first >> 24);
        gradient->m_colors[0] = *first;
        gradient->m_alpha[1] = static_cast<uint8_t>(*first >> 24);
        gradient->m_colors[1] = *first;
        gradient->m_alpha[2] = static_cast<uint8_t>(*second >> 24);
        gradient->m_colors[2] = *second;
        gradient->m_alpha[3] = static_cast<uint8_t>(*second >> 24);
        gradient->m_colors[3] = *second;
    } else {
        gradient->m_alpha[1] = static_cast<uint8_t>(*minColor >> 24);
        gradient->m_colors[1] = *minColor;
        gradient->m_alpha[3] = static_cast<uint8_t>(*minColor >> 24);
        gradient->m_colors[3] = *minColor;
        gradient->m_alpha[0] = static_cast<uint8_t>(*maxColor >> 24);
        gradient->m_colors[0] = *maxColor;
        gradient->m_alpha[2] = static_cast<uint8_t>(*maxColor >> 24);
        gradient->m_colors[2] = *maxColor;
    }
    this->UpdateEffectiveAlpha();
}

// ref: FUN_1406315d0
void CSimpleRegion::SetHasGradient(bool gradient) {
    bool has = (this->m_regionFlags & REGIONFLAG_GRADIENT) != 0;
    if (!gradient) {
        if (!has) {
            return;
        }
        if (this->m_gradient != nullptr) {
            s_gradientHeap.PutData(this->m_gradient);
        }
        this->m_regionFlags &= ~REGIONFLAG_GRADIENT;
        this->m_alpha = 0xff;
        this->m_color = 0xffffffff;
    } else {
        if (has) {
            return;
        }
        auto created = static_cast<CGradient*>(s_gradientHeap.GetData(false, ".?AUCGradient@@", -2, 0x1f));
        if (created != nullptr) {
            created->m_count = 4;
            created->m_capacity = 4;
            for (uint32_t& color : created->m_colors) {
                color = 0;
            }
            memset(created->m_alpha, 0, created->m_count);
        }
        this->m_regionFlags |= REGIONFLAG_GRADIENT;
        this->m_gradient = created;
    }
    this->MarkFrameBatchDirty();
}

// ref: FUN_1406316e0
void CSimpleRegion::SetIgnoreParentScale(bool ignore) {
    if (ignore == ((this->m_regionFlags & REGIONFLAG_IGNORE_PARENT_SCALE) != 0)) {
        return;
    }
    this->m_regionFlags = (this->m_regionFlags & ~REGIONFLAG_IGNORE_PARENT_SCALE) | (ignore ? REGIONFLAG_IGNORE_PARENT_SCALE : 0);
    if (this->m_parent == nullptr) {
        return;
    }
    if (ignore) {
        this->CLayoutFrame::SetLayoutScale(1.0f, false);
        return;
    }
    this->CLayoutFrame::SetLayoutScale(this->m_parent->m_layoutScale, false);
}

// ref: FUN_140631750
void CSimpleRegion::SetScale(float scale) {
    if (scale <= 0.0f) {
        const char* name = this->GetName();
        if (name == nullptr) {
            name = this->GetObjectTypeName();
        }
        FrameScript_ActionError("%s:SetScale(scale): Scale must be > 0", name);
        return;
    }
    if (scale != this->m_scale) {
        this->m_scale = scale;
        this->CLayoutFrame::SetLayoutScale(this->m_parent->m_layoutScale, false);
    }
}

// ref: FUN_1406311e0
void CSimpleRegion::MarkFrameBatchDirty() {
    if ((this->m_layoutState & LAYOUT_VISIBLE) && this->m_parent != nullptr && this->IsLayoutReady()) {
        this->m_parent->MarkBatchDirty();
    }
}

// ref: FUN_140630e80
void CSimpleRegion::DeferDelete(CSimpleRegion* region) {
    s_deferredDeletes.push_back(region);
}

// ref: FUN_140630f20
void CSimpleRegion::FlushDeferredDeletes() {
    if (s_deferredDeletes.size() == 0) {
        return;
    }
    for (uint64_t i = 0; i < s_deferredDeletes.size(); i++) {
        CSimpleRegion* region = s_deferredDeletes[i];
        if (region != nullptr) {
            delete region;
        }
    }
    s_deferredDeletes.clear();
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142e99d00 = (void*)&s_gradientHeap;
// ---- end of generated globals ----
