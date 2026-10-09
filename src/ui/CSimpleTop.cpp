#include "ui/CSimpleTop.hpp"

#include <cstring>
#include <memory>
#include <new>

#include "base/Coordinate.hpp"
#include "blz/Murmur.hpp"
#include "blz/RbTree.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/CAnimatableObject.hpp"
#include "ui/CRenderBatch.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/CSimpleFrame.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/GxWindow.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/Gx.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "os/Window.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"

const char s_regionVectorTag[] = ".PEAVCScriptRegion@@";
const char s_frameVectorTag[] = ".PEAVCSimpleFrame@@";
const char s_deferredInputTag[] = ".?AUDeferredInputEventChange@CSimpleTop@@";
const char s_deferredScriptTag[] = ".?AUBasicDeferredScript@DeferredScriptBuffer@@";

CSimpleTop* CSimpleTop::s_instance;
uint8_t CSimpleTop::s_drawOnTopOnly;

namespace {

const char s_buttonNodeTag[] = ".?AU?$rb_node@U?$pair@$$CBW4MOUSEBUTTON@@V?$embedded_vector@PEAVCScriptRegion@@$01V?$"
                               "allocator@PEAVCScriptRegion@@@blz@@@blz@@@blz@@@blz@@";
const char s_regionIntNodeTag[] = ".?AU?$chained_hash_node@U?$pair@QEBVCScriptRegion@@H@blz@@@blz@@";

// A node of the mouse-button map (0x50): which regions a button went down on.
struct BUTTONNODE : blz::rb_node_base {
    int32_t m_button;                                                   // 0x20
    blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag> m_regions;  // 0x28
};
static_assert(sizeof(BUTTONNODE) == 0x50, "a mouse-button node is 0x50 bytes");

// The regions of a button, inserted when missing (FUN_1405ece50).
blz::vector<CScriptRegion*, s_regionVectorTag>* ButtonRegions(RBTREEHEADER* header, int32_t button) {
    blz::rb_node_base* parent = header;
    blz::rb_node_base** slot;
    if (header->m_parent == nullptr) {
        // an empty tree: the new node becomes the root
        slot = nullptr;
    } else {
        blz::rb_node_base* node = header->m_parent;
        blz::rb_node_base* last = node;
        bool less = false;
        while (node != nullptr) {
            last = node;
            less = button < static_cast<BUTTONNODE*>(node)->m_button;
            node = less ? node->m_left : node->m_right;
        }
        parent = last;
        if (less) {
            if (parent != header->m_left) {
                blz::rb_node_base* previous = blz::rb_prev(parent);
                if (button <= static_cast<BUTTONNODE*>(previous)->m_button) {
                    return &static_cast<BUTTONNODE*>(previous)->m_regions;
                }
            }
            slot = &parent->m_left;
        } else {
            if (button <= static_cast<BUTTONNODE*>(parent)->m_button) {
                return &static_cast<BUTTONNODE*>(parent)->m_regions;
            }
            slot = &parent->m_right;
        }
    }
    auto node = static_cast<BUTTONNODE*>(SMemAlloc(sizeof(BUTTONNODE), 0xb, s_buttonNodeTag, SStrHashFnv1a(s_buttonNodeTag), -6, 0));
    node->m_button = button;
    new (&node->m_regions) blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag>();
    header->m_size++;
    node->m_left = nullptr;
    node->m_right = nullptr;
    node->m_color = 0;
    node->m_parent = parent;
    if (parent == header) {
        header->m_parent = node;
        header->m_left = node;
        header->m_right = node;
    } else {
        *slot = node;
        if (parent == header->m_left && slot == &parent->m_left) {
            header->m_left = node;
        }
        if (parent == header->m_right && slot == &parent->m_right) {
            header->m_right = node;
        }
    }
    blz::rb_insert_fixup(&header->m_parent, node);
    return &node->m_regions;
}

// Calls `visit(button, regions)` for each button in order (FUN_1405ee650).
template <class Visit>
void ForEachButton(RBTREEHEADER* header, Visit visit) {
    for (blz::rb_node_base* node = header->m_left; node != header; node = blz::rb_next(node)) {
        auto button = static_cast<BUTTONNODE*>(node);
        visit(button->m_button, &button->m_regions);
    }
}

// Removes a region from a list, marking the mouse focus for a refresh (FUN_1405ed320).
void RemoveRegion(CSimpleTop* top, CScriptRegion* region, blz::vector<CScriptRegion*, s_regionVectorTag>* regions) {
    for (uint64_t i = 0; i < regions->size(); i++) {
        if ((*regions)[i] == region) {
            regions->erase(i, i + 1);
            top->m_mouseFocusDirty = 1;
            return;
        }
    }
}

struct REGIONINTNODE {
    REGIONINTNODE* m_next;
    CScriptRegion* m_key;
    int32_t m_value;
};

const char s_batchNodeTag[] = ".?AU?$chained_hash_node@U?$pair@QEAVCSimpleFrame@@PEAVCRenderBatch@@@blz@@@blz@@";
const char s_batchBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@QEAVCSimpleFrame@@PEAVCRenderBatch@@@blz@@@blz@@";
const char s_animNodeTag[] = ".?AU?$chained_hash_node@PEAVCAnimatableObject@@@blz@@";
const char s_animBucketTag[] = ".PEAU?$chained_hash_node@PEAVCAnimatableObject@@@blz@@";

// blz::chained_hash_node<pair<CSimpleFrame* const, CRenderBatch*>> (0x18).
struct BATCHNODE {
    BATCHNODE* m_next;
    CSimpleFrame* m_key;
    CRenderBatch* m_value;
};

// blz::chained_hash_node<CAnimatableObject*> (0x10).
struct ANIMNODE {
    ANIMNODE* m_next;
    CAnimatableObject* m_key;
};

}

namespace {

// Every texture finished: the strata batch is rebuilt (0x1405f4b90).
void OnTexturesLoaded(void* storage) {
    (void)storage;
    CSimpleTop::s_instance->m_strataDirty = 1;
}

// The device is back: every root frame's batches are rebuilt (FUN_1405efe10).
// ref: FUN_1405efe10
void OnDeviceCreated(void* param) {
    (void)param;
    CSimpleTop* top = CSimpleTop::s_instance;
    top->m_strataDirty = 1;
    for (CSimpleFrame* frame = top->m_rootFrames.Head(); frame != nullptr; frame = top->m_rootFrames.Next(frame)) {
        frame->RefreshBatch();
    }
}

// ref: FUN_1405efe80
void OnDeviceDestroyed(void* param) {
    (void)param;
    CSimpleTop* top = CSimpleTop::s_instance;
    for (CSimpleFrame* frame = top->m_rootFrames.Head(); frame != nullptr; frame = top->m_rootFrames.Next(frame)) {
        frame->RefreshRegions();
    }
}

}

// ref: FUN_140236a40
void UiGetCursorPosition(float* x, float* y) {
    int32_t cursorX;
    int32_t cursorY;
    OsGetCursorPosition(&cursorX, &cursorY);
    int32_t rect[4] = { 0, 0, 0, 0 };
    OsGetClientRect(rect);
    float height = static_cast<float>(rect[2] - rect[0]);
    if (height <= 1.0f) {
        height = 1.0f;
    }
    float fromBottom = 1.0f - static_cast<float>(cursorY) / height;
    if (x != nullptr) {
        float width = static_cast<float>(rect[3] - rect[1]);
        if (width <= 1.0f) {
            width = 1.0f;
        }
        *x = (static_cast<float>(cursorX) / width) * 0.8f;
    }
    if (y != nullptr) {
        *y = fromBottom * 0.6f;
    }
}

// ref: FUN_1405ebe10
CSimpleTop::CSimpleTop(void (*preUpdate)()) {
    this->m_dragFrame = nullptr;
    this->m_dragType = 0;
    this->m_dragCorner = 0;
    this->m_dragX = 0.0f;
    this->m_dragY = 0.0f;
    this->m_raiseCounter = 1;
    this->m_strataDirty = 0;
    this->m_frameBatches = CHAINEDHASH();
    this->m_layoutWaiting = CHAINEDHASH();
    this->m_cursorShown = 0;
    this->m_buttonFocus.m_parent = nullptr;
    this->m_buttonFocus.m_left = &this->m_buttonFocus;
    this->m_buttonFocus.m_right = &this->m_buttonFocus;
    this->m_buttonFocus.m_color = 0;
    this->m_buttonFocus.m_size = 0;
    this->m_mouseFocus = nullptr;
    this->m_motionTracking = 0;
    this->m_unknown318 = nullptr;
    this->m_unknown320 = 0;
    this->m_unknown324 = 0;
    std::memset(this->m_keyFocus, 0, sizeof(this->m_keyFocus));
    std::memset(this->m_padFocus, 0, sizeof(this->m_padFocus));
    this->m_dispatchDepth = 0;
    this->m_shownRootCount = 0;
    this->m_mouseFocusDirty = 1;
    std::memset(&this->m_mouseEvent, 0, sizeof(this->m_mouseEvent));
    this->m_currentButton = nullptr;
    this->m_mouseDownHook = nullptr;
    this->m_mouseMoveHook = nullptr;
    this->m_unknownF38 = nullptr;
    this->m_keyFilterHook = nullptr;
    this->m_keyCaptureHook = nullptr;
    this->m_keyDownHook = nullptr;
    this->m_padHook = nullptr;
    this->m_secureLoad = 1;
    this->m_unknownF61 = 0;
    this->m_inMouseScript = 0;
    this->m_inputSuspended = 0;
    this->m_resizing = 0;
    this->m_actionBlockedHook = nullptr;
    this->m_preUpdateHook = preUpdate;
    this->m_rootFrames.m_linkoffset = 0x200;
    this->m_updateFrames.m_linkoffset = 0x1f0;
    this->m_animating = CHAINEDHASH();
    this->m_regionLayoutVersions = CHAINEDHASH();
    this->m_layoutVersion = 0;
    this->m_layoutDirty = 0;
    std::memset(this->m_projection, 0, sizeof(this->m_projection));
    this->m_projection[0] = 1.0f;
    this->m_projection[5] = 1.0f;
    this->m_projection[10] = 1.0f;
    this->m_projection[15] = 1.0f;
    this->m_inputPhase = 0;
    s_instance = this;
    this->SetAnchorRoot(true);
    this->m_rect.minY = 0.0f;
    this->m_rect.minX = 0.0f;
    this->m_rect.maxY = g_coordinateSpace.m_diagonalHeight;
    this->m_rect.maxX = g_coordinateSpace.m_diagonalWidth;
    this->m_flags |= FLAG_RECT_VALID;
    this->m_mouseEvent.m_x = 0.5f;
    this->m_mouseEvent.m_y = 0.5f;
    this->m_mouseEvent.m_flags = 0x100;
    UiGetCursorPosition(&this->m_mouseEvent.m_x, &this->m_mouseEvent.m_y);
    this->m_lastMouseEvent = this->m_mouseEvent;
    CRenderBatch::Initialize();
    blz::function_storage loaded;
    loaded.m_invoke = reinterpret_cast<void*>(&OnTexturesLoaded);
    loaded.m_manager = &blz::function_trivial_manager;
    TextureAddAllLoadedCallback(&loaded, reinterpret_cast<uint64_t>(this));
    GxAddDeviceCreatedCallback(&OnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(&OnDeviceDestroyed, nullptr);
    this->m_layoutVersion = 1;
}

// ref: FUN_1405ec890
CSimpleTop::~CSimpleTop() {
    FROST_UNIMPLEMENTED();
    s_instance = nullptr;
}

// ref: FUN_1405f27d0
bool CSimpleTop::IsSecureLoad() {
    return this->m_secureLoad != 0;
}

// While input is being dispatched the change waits; otherwise the region joins the list for
// the kind (motion and click lists change the mouse focus).
// ref: FUN_1405f2fb0
void CSimpleTop::RegisterForInput(CScriptRegion* region, int32_t type) {
    if (this->m_dispatchDepth > 0) {
        this->DeferInputChange(region, type, true);
        return;
    }
    this->m_inputRegions[type].push_back(region);
    if (static_cast<uint32_t>(type - 2) < 2) {
        this->m_mouseFocusDirty = 1;
    }
}

// ref: FUN_1405f3e70
void CSimpleTop::UnregisterForInput(CScriptRegion* region, int32_t type, bool quiet) {
    if (this->m_dispatchDepth >= 1) {
        this->DeferInputChange(region, type, false);
        return;
    }
    auto* regions = &this->m_inputRegions[type];
    uint64_t index = 0;
    while (index < regions->size() && (*regions)[index] != region) {
        index++;
    }
    if (index == regions->size()) {
        return;
    }
    regions->erase(index, index + 1);
    if (type == 1) {
        for (CScriptRegion*& focus : this->m_keyFocus) {
            if (focus == region) {
                focus = nullptr;
            }
        }
    } else if (type == INPUT_EVENT_CLICK) {
        ForEachButton(&this->m_buttonFocus, [&](int32_t, blz::vector<CScriptRegion*, s_regionVectorTag>* list) {
            // FUN_1405f4c00
            RemoveRegion(this, region, list);
        });
        auto* over = static_cast<blz::vector<CScriptRegion*, s_regionVectorTag>*>(&this->m_mouseOver);
        for (uint64_t i = 0; i < over->size(); i++) {
            if ((*over)[i] == region) {
                over->erase(i, i + 1);
                this->m_mouseFocusDirty = 1;
                if (!quiet) {
                    region->OnMouseFocusLost(false, true);
                }
                return;
            }
        }
    } else if (type == INPUT_EVENT_MOTION) {
        auto* motion = static_cast<blz::vector<CScriptRegion*, s_regionVectorTag>*>(&this->m_mouseMotion);
        for (uint64_t i = 0; i < motion->size(); i++) {
            if ((*motion)[i] == region) {
                motion->erase(i, i + 1);
                this->m_mouseFocusDirty = 1;
                if (!quiet) {
                    region->OnLeave(false);
                }
                return;
            }
        }
    } else if (type == INPUT_EVENT_5) {
        for (CScriptRegion*& focus : this->m_padFocus) {
            if (focus == region) {
                focus = nullptr;
            }
        }
    }
}

// ref: FUN_1405eda20
void CSimpleTop::DeferInputChange(CScriptRegion* region, int32_t type, bool add) {
    DEFERREDINPUTEVENTCHANGE change;
    change.m_region = nullptr;
    change.m_type = -1;
    change.m_register = false;
    DEFERREDINPUTEVENTCHANGE& entry = this->m_deferredInput.emplace_back(change);
    entry.m_type = type;
    entry.m_register = add;
    entry.m_region = region;
}

// ref: FUN_1405ede10
void CSimpleTop::ApplyDeferredInputChanges() {
    blz::vector<DEFERREDINPUTEVENTCHANGE, s_deferredInputTag> changes;
    // FUN_1405f5b40: the changes are taken over and the queue left empty
    std::swap(changes.m_data, this->m_deferredInput.m_data);
    std::swap(changes.m_size, this->m_deferredInput.m_size);
    std::swap(changes.m_capacity, this->m_deferredInput.m_capacity);
    for (DEFERREDINPUTEVENTCHANGE& change : changes) {
        if (!change.m_register) {
            this->UnregisterForInput(change.m_region, change.m_type, false);
            continue;
        }
        CScriptRegion* region = change.m_region;
        if (!(region->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE)) {
            continue;
        }
        int32_t type = change.m_type;
        if (this->m_dispatchDepth < 1) {
            this->m_inputRegions[type].push_back(region);
            if (static_cast<uint32_t>(type - 2) < 2) {
                this->m_mouseFocusDirty = 1;
            }
        } else {
            this->DeferInputChange(region, type, true);
        }
    }
}

// ref: FUN_1405f1d90
void CSimpleTop::OnRegionDestroyed(CScriptRegion* region) {
    auto* over = static_cast<blz::vector<CScriptRegion*, s_regionVectorTag>*>(&this->m_mouseOver);
    RemoveRegion(this, region, over);
    auto* motion = static_cast<blz::vector<CScriptRegion*, s_regionVectorTag>*>(&this->m_mouseMotion);
    RemoveRegion(this, region, motion);
    ForEachButton(&this->m_buttonFocus, [&](int32_t, blz::vector<CScriptRegion*, s_regionVectorTag>* list) {
        // LAB_1405f4c90
        RemoveRegion(this, region, list);
    });
    if (this->m_unknown318 == region) {
        this->m_unknown318 = nullptr;
        if (this->m_unknown324) {
            this->m_unknown324 = 0;
        }
    }
    CHAINEDHASH* map = &this->m_regionLayoutVersions;
    if (map->m_buckets == nullptr) {
        return;
    }
    uint64_t index = blz::fmix64(reinterpret_cast<uint64_t>(region)) & (map->m_bucketCount - 1);
    auto link = reinterpret_cast<REGIONINTNODE**>(&map->m_buckets[index]);
    while (*link != nullptr && (*link)->m_key != region) {
        link = &(*link)->m_next;
    }
    REGIONINTNODE* node = *link;
    if (node == nullptr) {
        return;
    }
    int32_t version = node->m_value;
    *link = node->m_next;
    SMemFree(node, s_regionIntNodeTag, -6, 0);
    map->m_size--;
    if (version != this->m_layoutVersion) {
        region->OnLayoutVersionChanged();
    }
}

// ref: FUN_1405f41f0
void CSimpleTop::RemoveRegionInput(CScriptRegion* region, bool quiet) {
    region->UnregisterEnabledInput(quiet);
}

// ref: FUN_1405f35e0
void CSimpleTop::QueueScript(FrameScript_Object* object, int32_t index) {
    this->m_deferredScripts.Add(object, static_cast<uint8_t>(index));
}

// ref: FUN_1405ee940
int32_t CSimpleTop::GetRegionLayoutVersion(CScriptRegion* region) {
    CHAINEDHASH* map = &this->m_regionLayoutVersions;
    if (map->m_buckets != nullptr) {
        uint64_t index = blz::fmix64(reinterpret_cast<uint64_t>(region)) & (map->m_bucketCount - 1);
        for (auto node = reinterpret_cast<REGIONINTNODE*>(map->m_buckets[index]); node != nullptr; node = node->m_next) {
            if (node->m_key == region) {
                return node->m_value;
            }
        }
    }
    return this->m_layoutVersion;
}

// Shown frames go to the head of the root list (and are counted), hidden ones to the tail.
// ref: FUN_1405f3080
void CSimpleTop::AddRootFrame(CSimpleFrame* frame) {
    TSLink<CSimpleFrame>* link = this->m_rootFrames.Link(frame);
    if (frame->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE) {
        // FUN_1405efaa0
        link->Unlink();
        TSLink<CSimpleFrame>* terminator = &this->m_rootFrames.m_terminator;
        link->m_prevlink = terminator;
        link->m_next = terminator->m_next;
        terminator->NextLink(this->m_rootFrames.m_linkoffset)->m_prevlink = link;
        terminator->m_next = frame;
        this->m_shownRootCount++;
        return;
    }
    link->Unlink();
    this->m_rootFrames.LinkToTail(frame);
}

// ref: FUN_1405efb70
void CSimpleTop::MarkStrataDirty() {
    this->m_strataDirty = 1;
}

// ref: FUN_1405efb60
void CSimpleTop::InvalidateLayout() {
    this->m_layoutDirty = 1;
}

// ref: FUN_1405f4510
void CSimpleTop::AddUpdateCheck(CSimpleFrame* frame) {
    this->m_updateChecks.push_back(frame);
}

// A frame was shown: its input is registered and the root list reordered next update.
// ref: FUN_1405f3a90
void CSimpleTop::AddShownChange(CSimpleFrame* frame) {
    frame->RegisterEnabledInput();
    this->m_shownChanges.push_back(frame);
    this->m_shownRootCount++;
}

// ref: FUN_1406143e0
void CSimpleTop::AddPendingBatchUpdate(CSimpleFrame* frame) {
    this->m_batchUpdates.push_back(frame);
}

// A frame was hidden: it leaves the drag and its input, and the root list is reordered next
// update.
// ref: FUN_1405eee20
void CSimpleTop::AddHiddenChange(CSimpleFrame* frame) {
    this->m_shownChanges.push_back(frame);
    this->m_shownRootCount--;
    if (this->m_dragFrame == frame) {
        if (frame != nullptr) {
            frame->SnapToNearestAnchor(this);
        }
        this->m_dragFrame = nullptr;
        this->m_dragType = 0;
    }
    frame->UnregisterEnabledInput(frame->HasInputFlags(CScriptRegion::REGION_PROPAGATE_MOUSE));
}

// ref: FUN_1405f40c0
void CSimpleTop::RemoveFrame(CSimpleFrame* frame) {
    if (frame->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE) {
        this->m_shownRootCount--;
        if (this->m_dragFrame == frame) {
            frame->SnapToNearestAnchor(this);
            this->m_dragFrame = nullptr;
            this->m_dragType = 0;
        }
    }
    for (CSimpleFrame*& changed : this->m_shownChanges) {
        if (changed == frame) {
            changed = nullptr;
        }
    }
    this->m_rootFrames.Link(frame)->Unlink();
    if (frame->m_updateLink.IsLinked()) {
        this->m_updateFrames.Link(frame)->Unlink();
    }
}

// The frame being dragged follows a resize: it is raised, and the mouse focus refreshed.
// ref: FUN_1405efc10
void CSimpleTop::OnFrameSizeChanged(CSimpleFrame* frame) {
    (void)frame;
    if (this->m_dragFrame != nullptr) {
        this->m_dragFrame->Raise();
    }
    this->m_mouseFocusDirty = 1;
}

// ref: FUN_1405ee830
CRenderBatch* CSimpleTop::GetFrameBatch(CSimpleFrame* frame) {
    BATCHNODE* node = blz::chained_find<BATCHNODE>(&this->m_frameBatches, frame);
    if (node != nullptr) {
        return node->m_value;
    }
    CRenderBatch* batch = new CRenderBatch();
    bool inserted;
    // FUN_1405ebb30
    node = blz::chained_insert<BATCHNODE>(&this->m_frameBatches, frame, &inserted, s_batchNodeTag, s_batchBucketTag);
    node->m_value = batch;
    return batch;
}

// ref: FUN_1405ee9d0
CRenderBatch* CSimpleTop::FindFrameBatch(CSimpleFrame* frame) {
    BATCHNODE* node = blz::chained_find<BATCHNODE>(&this->m_frameBatches, frame);
    return node != nullptr ? node->m_value : nullptr;
}

// ref: FUN_1405ee300
void CSimpleTop::RemoveFrameBatch(CSimpleFrame* frame) {
    BATCHNODE* node = blz::chained_find<BATCHNODE>(&this->m_frameBatches, frame);
    if (node == nullptr) {
        return;
    }
    CRenderBatch* batch = node->m_value;
    batch->Clear();
    if (batch != nullptr) {
        delete batch;
    }
    node = blz::chained_unlink<BATCHNODE>(&this->m_frameBatches, frame);
    blz::chained_free_node(node, s_batchNodeTag);
}

// ref: FUN_1405ed9f0
void CSimpleTop::AddAnimating(CAnimatableObject* object) {
    bool inserted;
    // FUN_1405eb740
    blz::chained_insert<ANIMNODE>(&this->m_animating, object, &inserted, s_animNodeTag, s_animBucketTag);
}

// ref: FUN_1405f3120
void CSimpleTop::RemoveAnimating(CAnimatableObject* object) {
    ANIMNODE* node = blz::chained_unlink<ANIMNODE>(&this->m_animating, object);
    if (node != nullptr) {
        blz::chained_free_node(node, s_animNodeTag);
    }
}

// The frame takes the drag (it is raised and marked user placed) when it can be sized from
// where it is.
// ref: FUN_1405f3b30
bool CSimpleTop::StartDrag(CSimpleFrame* frame, int32_t type, float x, float y, int32_t corner) {
    if (!frame->ApplyDragSize(this, 0.0f, 0.0f, 0.0f, 0.0f)) {
        return false;
    }
    frame->Raise();
    frame->SetUserPlaced(true);
    this->m_dragY = y;
    this->m_dragX = x;
    this->m_dragFrame = frame;
    this->m_dragType = type;
    this->m_dragCorner = corner;
    this->UpdateMouseFocus(&CSimpleTop::s_instance->m_mouseEvent);
    return true;
}

// ref: FUN_1405f3bf0
void CSimpleTop::StopDrag() {
    if (this->m_dragFrame != nullptr) {
        this->m_dragFrame->SnapToNearestAnchor(this);
    }
    this->m_dragType = 0;
    this->m_dragFrame = nullptr;
}

// ref: FUN_1405ef500
bool CSimpleTop::IsInButtonCapture(CScriptRegion* region) {
    bool found = false;
    ForEachButton(&this->m_buttonFocus, [&](int32_t, blz::vector<CScriptRegion*, s_regionVectorTag>* regions) {
        for (CScriptRegion* captured : *regions) {
            if (captured == region) {
                found = true;
            }
        }
    });
    return found;
}

// ref: FUN_1405eed90
bool CSimpleTop::IsAnyButtonCaptured() {
    bool any = false;
    ForEachButton(&this->m_buttonFocus, [&](int32_t, blz::vector<CScriptRegion*, s_regionVectorTag>* regions) {
        if (regions->size() != 0) {
            any = true;
        }
    });
    return any;
}

// The region joins every button capture in progress.
// ref: FUN_1405edbf0
bool CSimpleTop::AddButtonCapture(CScriptRegion* region) {
    if (region == nullptr || this->IsInButtonCapture(region) || !this->IsAnyButtonCaptured()) {
        return false;
    }
    ForEachButton(&this->m_buttonFocus, [&](int32_t, blz::vector<CScriptRegion*, s_regionVectorTag>* regions) {
        if (regions->size() != 0) {
            regions->push_back(region);
        }
    });
    return true;
}

// ref: FUN_1405f31f0
bool CSimpleTop::ReleaseButtonCapture(CScriptRegion* region) {
    if (region == nullptr || !this->IsInButtonCapture(region)) {
        return false;
    }
    ForEachButton(&this->m_buttonFocus, [&](int32_t, blz::vector<CScriptRegion*, s_regionVectorTag>* regions) {
        for (uint64_t i = 0; i < regions->size(); i++) {
            if ((*regions)[i] == region) {
                regions->erase(i, i + 1);
                break;
            }
        }
    });
    return true;
}

// ref: FUN_1405ef5b0
bool CSimpleTop::IsRegionAbove(CScriptRegion* a, CScriptRegion* b) {
    (void)a;
    (void)b;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1405efb80
void CSimpleTop::UpdateMouseFocus(MOUSEEVENT* event) {
    (void)event;
    FROST_UNIMPLEMENTED();
}

// ---- the update ------------------------------------------------------------------------------

namespace {

const char s_layoutNodeTag[] = ".?AU?$chained_hash_node@PEAVCLayoutFrame@@@blz@@";
const char s_layoutBucketTag[] = ".PEAU?$chained_hash_node@PEAVCLayoutFrame@@@blz@@";
const char s_groupNodeTag[] = ".?AU?$chained_hash_node@PEAVCSimpleAnimGroup@@@blz@@";
const char s_groupBucketTag[] = ".PEAU?$chained_hash_node@PEAVCSimpleAnimGroup@@@blz@@";
const char s_fontHandleTag[] = ".?AV?$TSList@UUNNEEDED_FONTHANDLE@@V?$TSGetLink@UUNNEEDED_FONTHANDLE@@@@$07@@";

// blz::chained_hash_node<CLayoutFrame*> (0x10).
struct LAYOUTNODE {
    LAYOUTNODE* m_next;
    CLayoutFrame* m_key;
};

// blz::chained_hash_node<CSimpleAnimGroup*> (0x10).
struct GROUPNODE {
    GROUPNODE* m_next;
    CSimpleAnimGroup* m_key;
};

// A font handle dropped at the next update (0x20): its link and the shared handle.
struct UNNEEDED_FONTHANDLE {
    TSLink<UNNEEDED_FONTHANDLE> m_link;     // 0x00
    std::shared_ptr<void> m_handle;         // 0x10
};

// The handles (0x142cce7a0).
TSList<UNNEEDED_FONTHANDLE, TSGetLink<UNNEEDED_FONTHANDLE>, 8> s_unneededFontHandles;

}

// ref: FUN_1405f6260
void UiAddUnneededFontHandle(const std::weak_ptr<void>* handle) {
    void* memory = SMemAlloc(sizeof(UNNEEDED_FONTHANDLE), 8, s_fontHandleTag, 0, -2, 8);
    auto node = new (memory) UNNEEDED_FONTHANDLE();
    s_unneededFontHandles.LinkToTail(node);
    node->m_handle = handle->lock();
}

// ref: FUN_1405f7340
void UiReleaseUnneededFontHandles() {
    UNNEEDED_FONTHANDLE* node = s_unneededFontHandles.Head();
    while (node != nullptr) {
        UNNEEDED_FONTHANDLE* next = s_unneededFontHandles.Next(node);
        node->m_handle.reset();
        node->m_link.Unlink();
        node->~UNNEEDED_FONTHANDLE();
        SMemFree(node, s_fontHandleTag, -2, 0);
        node = next;
    }
}

// ref: FUN_1405f4200
void CSimpleTop::ProcessLayoutWaiting() {
    if (this->m_layoutWaiting.m_size == 0) {
        return;
    }
    CHAINEDHASH waiting = this->m_layoutWaiting;
    this->m_layoutWaiting = CHAINEDHASH();
    blz::chained_for_each<LAYOUTNODE>(&waiting, [&](LAYOUTNODE* node) {
        CLayoutFrame* frame = node->m_key;
        if (!frame->IsLayoutReady()) {
            bool inserted;
            blz::chained_insert<LAYOUTNODE>(&this->m_layoutWaiting, frame, &inserted, s_layoutNodeTag, s_layoutBucketTag);
        } else {
            frame->OnLayoutReady();
        }
    });
    blz::chained_destroy<LAYOUTNODE>(&waiting, s_layoutNodeTag, s_layoutBucketTag);
}

// ref: FUN_1405ee140
void CSimpleTop::ProcessShownChanges() {
    for (uint64_t i = 0; i < this->m_shownChanges.m_size; i++) {
        CSimpleFrame* frame = this->m_shownChanges.m_data[i];
        if (frame == nullptr) {
            continue;
        }
        this->m_rootFrames.Link(frame)->Unlink();
        if (static_cast<CLayoutFrame*>(frame)->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE) {
            this->m_rootFrames.LinkToHead(frame);
        } else {
            this->m_rootFrames.LinkToTail(frame);
        }
    }
    this->m_shownChanges.m_size = 0;
}

// ref: FUN_1405edfc0
void CSimpleTop::ProcessUpdateChecks() {
    for (uint64_t i = 0; i < this->m_updateChecks.m_size; i++) {
        CSimpleFrame* frame = this->m_updateChecks.m_data[i];
        bool wants = frame->WantsUpdateCheck();
        TSLink<CSimpleFrame>* link = this->m_updateFrames.Link(frame);
        if (wants == link->IsLinked()) {
            continue;
        }
        link->Unlink();
        if (wants) {
            this->m_updateFrames.LinkToHead(frame);
        }
    }
    this->m_updateChecks.m_size = 0;
}

// ref: FUN_1405f09a0
void CSimpleTop::Render() {
    float view[16];
    float projection[16];
    float offset[2] = { 0.0f, 0.0f };
    UiComputeMatrices(&this->m_rect, offset, view, projection);
    std::memcpy(this->m_projection, projection, sizeof(this->m_projection));
    blz::vector<CSimpleFrame*, s_frameVectorTag> updates;
    updates.swap(this->m_batchUpdates);
    for (CSimpleFrame* frame : updates) {
        frame->RebuildBatchIfDirty();
    }
    if (this->m_strataDirty != 0) {
        this->m_renderBatch.Clear();
        this->m_strataDirty = 0;
        this->ProcessShownChanges();
        int32_t count = this->m_shownRootCount;
        for (CSimpleFrame* frame = this->m_rootFrames.Head(); frame != nullptr;) {
            CSimpleFrame* next = this->m_rootFrames.Next(frame);
            if (count < 1) {
                break;
            }
            frame->AddToParentBatch(&this->m_renderBatch);
            frame = next;
            count--;
        }
    }
    FRAMEBUFFER* buffer = WindowGetFrameBuffer();
    g_gxRenderContext->SetRenderTargets(buffer->m_targets.m_count != 0 ? &buffer->m_targets : nullptr);
    this->m_renderBatch.Draw(s_drawOnTopOnly != 0, view, projection);
}

// ref: FUN_1405f0be0
void CSimpleTop::OnUpdate(float elapsed) {
    if (this->m_preUpdateHook != nullptr) {
        this->m_preUpdateHook();
    }
    CLayoutFrame::ResizePending();
    blz::chained_for_each<ANIMNODE>(&this->m_animating, [](ANIMNODE* node) {
        node->m_key->Unknown08();
    });
    this->ProcessLayoutWaiting();
    this->ProcessShownChanges();
    this->ProcessUpdateChecks();
    for (CSimpleFrame* frame = this->m_updateFrames.Head(); frame != nullptr;) {
        CSimpleFrame* next = this->m_updateFrames.Next(frame);
        frame->RunOnUpdate(elapsed);
        frame = next;
    }
    // Every playing group of every animating object advances once, however many objects
    // share it.
    CHAINEDHASH groups = CHAINEDHASH();
    blz::chained_for_each<ANIMNODE>(&this->m_animating, [&](ANIMNODE* node) {
        ANIMATABLEGROUPS* lists = node->m_key->m_groups;
        if (lists == nullptr || lists->m_playing == nullptr) {
            return;
        }
        TSExplicitList<CSimpleAnimGroup>* playing = lists->m_playing;
        for (CSimpleAnimGroup* group = playing->Head(); group != nullptr; group = playing->Link(group)->Next()) {
            bool inserted;
            blz::chained_insert<GROUPNODE>(&groups, group, &inserted, s_groupNodeTag, s_groupBucketTag);
        }
    });
    blz::chained_for_each<GROUPNODE>(&groups, [&](GROUPNODE* node) {
        CSimpleAnimGroup_OnUpdate(node->m_key, elapsed);
    });
    blz::chained_for_each<ANIMNODE>(&this->m_animating, [](ANIMNODE* node) {
        node->m_key->Unknown10();
    });
    UiReleaseUnneededFontHandles();
    if (this->m_mouseFocusDirty) {
        this->m_mouseFocusDirty = 0;
        this->OnMouseMoveEvent(&this->m_mouseEvent, false);
    }
    CLayoutFrame::ResizePending();
    if (this->m_layoutDirty) {
        int32_t version = this->m_layoutVersion;
        this->m_layoutDirty = 0;
        this->m_layoutVersion = 1;
        if (version != 1) {
            for (CSimpleFrame* frame = this->m_rootFrames.Head(); frame != nullptr;) {
                CSimpleFrame* next = this->m_rootFrames.Next(frame);
                frame->OnLayoutVersionChanged();
                frame = next;
            }
        }
    }
    this->m_lastMouseEvent = this->m_mouseEvent;
    blz::chained_destroy<GROUPNODE>(&groups, s_groupNodeTag, s_groupBucketTag);
}

// The mouse moved (or the focus must be recomputed with `simulated` set): the drag follows it,
// or the regions under it get OnEnter and OnLeave. Returns whether the UI took the event.
// ref: FUN_1405f1530
bool CSimpleTop::OnMouseMoveEvent(MOUSEEVENT* event, bool simulated) {
    (void)event;
    (void)simulated;
    FROST_UNIMPLEMENTED();
    return false;
}

// ---- DeferredScriptBuffer -------------------------------------------------------------------

// ref: FUN_140621e70
void DeferredScriptBuffer::Add(FrameScript_Object* object, uint8_t index) {
    if (this->m_depth < 1) {
        // FUN_140603620
        object->RunScript(index, nullptr);
        return;
    }
    DEFERREDSCRIPT script;
    script.m_object = object;
    script.m_index = index;
    this->m_queued.push_back(script);
}

// ref: FUN_140621e50
void DeferredScriptBuffer::End() {
    if (this->m_depth < 1) {
        return;
    }
    this->m_depth--;
    if (this->m_depth != 0) {
        return;
    }
    this->Flush();
}

// The queue moves to the running list and runs; scripts it queues run in the next round.
// ref: FUN_140622020
void DeferredScriptBuffer::Flush() {
    if (this->m_running.size() != 0) {
        return;
    }
    uint32_t rounds = 0;
    do {
        // FUN_1406221c0: the two vectors are swapped
        this->m_running.swap(this->m_queued);
        for (uint64_t i = 0; i < this->m_running.size(); i++) {
            DEFERREDSCRIPT& script = this->m_running[i];
            script.m_object->RunScript(script.m_index, nullptr);
        }
        rounds++;
        this->m_running.clear();
        if (this->m_queued.size() == 0) {
            return;
        }
    } while (rounds < 12);
    this->m_queued.clear();
}
