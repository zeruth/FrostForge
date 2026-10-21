#include "ui/CSimpleTop.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#include "base/Coordinate.hpp"
#include "base/ObjectAlloc.hpp"
#include "os/Os.hpp"
#include "storm/hash/TSHashTable.hpp"
#include <cstddef>
#include "blz/Murmur.hpp"
#include "blz/RbTree.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/CAnimatableObject.hpp"
#include "ui/CRenderBatch.hpp"
#include "framescript/EventCallbacks.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/CSimpleRegion.hpp"
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

CSimpleTop* CSimpleTop::s_instance;  // DAT_142e979d8
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
        *x = (static_cast<float>(cursorX) / width) * g_coordinateSpace.m_diagonalWidth;   // DAT_142ccb9f4
    }
    if (y != nullptr) {
        *y = fromBottom * g_coordinateSpace.m_diagonalHeight;   // DAT_142ccb9f8
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
    this->m_imeEnabled = 0;
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

// The script events a mouse button going down and up fire, with the button's name.
// ref: FUN_141174d40
void UiSignalGlobalMouseDown(const char* button) {
    const char* args[1] = {button};
    EventCallbacks_Fire(0x1148, 0xcc7252048b03ae1bull, args);
}

// ref: FUN_141174d80
void UiSignalGlobalMouseUp(const char* button) {
    const char* args[1] = {button};
    EventCallbacks_Fire(0x1150, 0x74755d73e90641b7ull, args);
}

// ---- the mouse ------------------------------------------------------------------------------

namespace {

// The clock of the last mouse input (DAT_142e979e0).
int32_t s_lastMouseTime;  // DAT_142e979e0

const char s_fociCandidateTag[] = ".?AUFociCandidate@?1??FindBestMouseFoci@CSimpleTop@@AEBA?AV?$embedded_vector@"
    "PEAVCScriptRegion@@$01V?$allocator@PEAVCScriptRegion@@@blz@@@blz@@W4CSimpleEventType@@AEBUMouseInfo@"
    "WowClient@@_NPEAV34@@Z@";

// A region the mouse may go to (0x10): whether it lets the regions under it have the mouse
// too, and whether it was hit on a hyperlink.
struct FOCICANDIDATE {
    CScriptRegion* m_region;
    uint8_t m_propagates;
    uint8_t m_hyperlink;
};

// The frame a region draws in: a frame itself, or a texture's or font string's parent.
bool IsFrame(CScriptRegion* region) {
    return region->IsObjectType(CSimpleFrame::GetObjectType());
}

// The nearest ancestor that draws its own batch (FRAME_OWN_BATCH) or into a frame buffer.
CSimpleFrame* BatchParent(CSimpleFrame* frame) {
    CSimpleFrame* parent = frame->m_parent;
    while (parent != nullptr && (parent->m_frameFlags & CSimpleFrame::FRAME_OWN_BATCH) == 0
        && (parent->m_flags2 >> 0xd & 1) == 0) {
        parent = parent->m_parent;
    }
    return parent;
}

// How many batch parents are above a frame.
// ref: FUN_1405ee730
int32_t BatchDepth(CSimpleFrame* frame) {
    int32_t depth = 0;
    for (CSimpleFrame* parent = BatchParent(frame); parent != nullptr; parent = BatchParent(parent)) {
        depth++;
    }
    return depth;
}

// The first ancestor of a batched frame that is not batched itself.
CSimpleFrame* UnbatchedAncestor(CSimpleFrame* frame) {
    CSimpleFrame* parent = frame->m_parent;
    while (parent != nullptr && parent->IsBatched()) {
        parent = parent->m_parent;
    }
    return parent;
}

uint64_t RegionKey(CScriptRegion* region) {
    return static_cast<CSimpleRegion*>(region)->GetOwnDrawOrderKey();
}

// Two batched frames (and the regions in them, when the region is not the frame) ordered by
// the batches they share: the deeper one climbs to the other's depth, then both climb to a
// common batch parent. When one batch holds the other, the regions' own keys decide; otherwise
// the two batches' frames do. Whether b draws above a.
// ref: FUN_1405ef2d0
bool IsBatchedRegionAbove(CSimpleFrame* aFrame, CScriptRegion* aRegion, CSimpleFrame* bFrame, CScriptRegion* bRegion) {
    int32_t aDepth = BatchDepth(aFrame);
    int32_t bDepth = BatchDepth(bFrame);
    CSimpleFrame* a = aFrame;
    CSimpleFrame* b = bFrame;
    while (aDepth != bDepth) {
        if (aDepth <= bDepth) {
            b = BatchParent(b);
            bDepth--;
        } else {
            a = BatchParent(a);
            aDepth--;
        }
    }
    CSimpleFrame* aParent = BatchParent(a);
    CSimpleFrame* bParent = BatchParent(b);
    while (aParent != bParent) {
        a = aParent;
        b = bParent;
        aParent = BatchParent(a);
        bParent = BatchParent(b);
    }
    if (a == bFrame || b == aFrame) {
        uint64_t bKey = bRegion == nullptr ? bFrame->GetDrawOrder() : RegionKey(bRegion);
        uint64_t aKey = aRegion == nullptr ? aFrame->GetDrawOrder() : RegionKey(aRegion);
        return aKey < bKey;
    }
    uint64_t aOrder = a->GetDrawOrder();
    uint64_t bOrder = b->GetDrawOrder();
    return aOrder < bOrder;
}

}  // namespace

// ---- the keyboard ---------------------------------------------------------------------------

namespace {

const char s_keyRegionTag[] = ".PEAVCScriptRegion@@";

// The frames registered for `type` that want this event, topmost first (FUN_1405ea280 over a
// copy: the list may change while the event is handed on).
template <class Wants>
void KeyCandidates(CSimpleTop* top, int32_t type, blz::embedded_vector<CScriptRegion*, 8, s_regionVectorTag>* out,
    Wants&& wants) {
    top->m_dispatchDepth++;
    blz::vector<CScriptRegion*, s_regionVectorTag>& regions = top->m_inputRegions[type];
    for (uint64_t i = 0; i < regions.size(); i++) {
        CScriptRegion* region = regions[i];
        if (wants(region)) {
            out->push_back(region);
        }
    }
    top->m_dispatchDepth--;
    std::sort(out->begin(), out->end(), [top](CScriptRegion* a, CScriptRegion* b) {
        return top->IsRegionAbove(b, a);
    });
}

}  // namespace

// A character: offered to the frames that take characters, topmost first, until one keeps it.
// True when none did.
// ref: FUN_1405efc40
bool CSimpleTop::OnCharEvent(uint32_t character) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 1;
    blz::embedded_vector<CScriptRegion*, 8, s_regionVectorTag> frames;
    KeyCandidates(this, INPUT_EVENT_CHAR, &frames, [](CScriptRegion* region) {
        return region->HasCharHandler();
    });
    bool unhandled = true;
    for (CScriptRegion* region : frames) {
        if (region->OnChar(static_cast<int32_t>(character))) {
            unhandled = false;
            break;
        }
    }
    this->m_inputPhase = phase;
    return unhandled;
}

// A key going down: the hook hears it first, then the frames that take this key, topmost
// first; the one that keeps it holds the key (m_keyFocus) until it is let go.
// ref: FUN_1405f0680
bool CSimpleTop::OnKeyDownEvent(KEYEVENT* event) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 1;
    s_lastMouseTime = static_cast<int32_t>(event->m_time);
    if (this->m_keyDownHook != nullptr) {
        reinterpret_cast<void (*)(KEYEVENT*)>(this->m_keyDownHook)(event);
    }
    blz::embedded_vector<CScriptRegion*, 8, s_regionVectorTag> frames;
    KeyCandidates(this, INPUT_EVENT_KEY, &frames, [event](CScriptRegion* region) {
        return region->HasKeyHandler(event);
    });
    bool unhandled = true;
    for (CScriptRegion* region : frames) {
        if (region->OnKeyDown(event)) {
            if (event->m_key != -1) {
                this->m_keyFocus[event->m_key] = region;
            }
            unhandled = false;
            break;
        }
    }
    this->m_inputPhase = phase;
    return unhandled;
}

// A key that is already down again (auto-repeat): to the frame holding it, if any.
// ref: FUN_1405f0890
bool CSimpleTop::OnKeyDownRepeatEvent(KEYEVENT* event) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 1;
    if (event->m_key != -1) {
        if (CScriptRegion* region = this->m_keyFocus[event->m_key]) {
            region->Unknown168(event);
            this->m_inputPhase = phase;
            return false;
        }
    }
    this->m_inputPhase = phase;
    return true;
}

// A key let go: to the frame holding it, which lets it go. Print Screen (0x121) only reports
// its release, so its release is offered as a press.
// ref: FUN_1405f0900
bool CSimpleTop::OnKeyUpEvent(KEYEVENT* event) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 1;
    s_lastMouseTime = static_cast<int32_t>(event->m_time);
    bool result = true;
    CScriptRegion* region = event->m_key != -1 ? this->m_keyFocus[event->m_key] : nullptr;
    if (region != nullptr) {
        region->OnKeyUp(event);
        result = false;
    } else if (event->m_key == 0x121) {
        result = this->OnKeyDownEvent(event);
    }
    if (event->m_key != -1) {
        this->m_keyFocus[event->m_key] = nullptr;
    }
    this->m_inputPhase = phase;
    return result;
}

// Each side is taken at its frame. Two batched frames are ordered by their batches; when only
// one is batched it stands at its first unbatched ancestor; otherwise the draw orders (a
// texture's or font string's own key) decide.
// ref: FUN_1405ef5b0
bool CSimpleTop::IsRegionAbove(CScriptRegion* a, CScriptRegion* b) {
    bool aIsFrame = IsFrame(a);
    bool bIsFrame = IsFrame(b);
    CSimpleFrame* aFrame = aIsFrame ? static_cast<CSimpleFrame*>(a) : a->m_parent;
    CSimpleFrame* bFrame = bIsFrame ? static_cast<CSimpleFrame*>(b) : b->m_parent;
    if (aFrame->IsBatched() && bFrame->IsBatched()) {
        return IsBatchedRegionAbove(aFrame, aIsFrame ? nullptr : a, bFrame, bIsFrame ? nullptr : b);
    }
    if (aFrame->IsBatched()) {
        CSimpleFrame* ancestor = UnbatchedAncestor(aFrame);
        if (ancestor != nullptr && ancestor != bFrame) {
            uint64_t bOrder = bFrame->GetDrawOrder();
            return ancestor->GetDrawOrder() < bOrder;
        }
    } else if (bFrame->IsBatched()) {
        CSimpleFrame* ancestor = UnbatchedAncestor(bFrame);
        if (ancestor != nullptr && ancestor != aFrame) {
            uint64_t bOrder = ancestor->GetDrawOrder();
            return aFrame->GetDrawOrder() < bOrder;
        }
    }
    uint64_t bKey = bIsFrame ? b->GetDrawOrder() : RegionKey(b);
    uint64_t aKey = aIsFrame ? a->GetDrawOrder() : RegionKey(a);
    return aKey < bKey;
}

// The dragged frame follows the mouse by what it moved since the last step.
// ref: FUN_1405efb80
void CSimpleTop::UpdateMouseFocus(MOUSEEVENT* event) {
    float dy = event->m_y - this->m_dragY;
    float dx = event->m_x - this->m_dragX;
    if (dx != 0.0f || dy != 0.0f) {
        // FUN_1406155a0
        this->m_dragFrame->SizeByDrag(this, dx, dy, this->m_dragCorner);
        this->m_dragX = dx + this->m_dragX;
        this->m_dragY = dy + this->m_dragY;
    }
}

// The regions registered for `type` under the mouse, topmost first: each offers its target
// (a child of a scroll frame, a hyperlink), which goes in above the first candidate it draws
// above, dropping those below unless it lets the mouse through; one under a candidate that
// keeps the mouse is left out. `entered` gets the ones hit on a hyperlink.
// ref: FUN_1405ee440
void CSimpleTop::FindBestMouseFoci(blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag>* out, int32_t type,
    MOUSEEVENT* event, bool force, blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag>* entered) {
    if (entered != nullptr) {
        entered->clear();
    }
    blz::embedded_vector<FOCICANDIDATE, 2, s_fociCandidateTag> candidates;
    MOUSEEVENT copy = *event;
    this->m_dispatchDepth++;
    blz::vector<CScriptRegion*, s_regionVectorTag>& regions = this->m_inputRegions[type];
    for (uint64_t i = 0; i < regions.size(); i++) {
        CScriptRegion* region = regions[i];
        if (region == nullptr) {
            continue;
        }
        // FUN_1405ed030
        bool hyperlink = false;
        CScriptRegion* target = region->GetMouseTarget(type, &copy, &hyperlink, force);
        if (target == nullptr) {
            continue;
        }
        FOCICANDIDATE candidate;
        candidate.m_region = target;
        candidate.m_propagates = (static_cast<FrameScript_Object*>(target)->m_flags & 0x100) != 0;
        candidate.m_hyperlink = hyperlink;
        if (candidates.size() == 0) {
            candidates.push_back(candidate);
            continue;
        }
        bool placed = false;
        for (uint64_t j = 0; j < candidates.size(); j++) {
            if (!this->IsRegionAbove(candidates[j].m_region, target)) {
                continue;
            }
            if (!candidate.m_propagates) {
                candidates.resize(j);
            }
            // FUN_1405f49b0: inserted before the one it draws above
            candidates.push_back(candidate);
            for (uint64_t k = candidates.size() - 1; k > j; k--) {
                candidates[k] = candidates[k - 1];
            }
            candidates[j] = candidate;
            placed = true;
            break;
        }
        if (placed || !candidates.back().m_propagates) {
            continue;
        }
        candidates.push_back(candidate);
    }
    this->m_dispatchDepth--;
    out->clear();
    for (uint64_t i = 0; i < candidates.size(); i++) {
        out->push_back(candidates[i].m_region);
        if (entered != nullptr && candidates[i].m_hyperlink) {
            entered->push_back(candidates[i].m_region);
        }
    }
}

namespace {

// The regions under the mouse for `type` replace `list`: those no longer under it leave, the
// ones that came under it (or were hit on a hyperlink again) enter.
// ref: FUN_1405ed600
template <class Leave, class Enter>
void UpdateMouseRegions(CSimpleTop* top, MOUSEEVENT* event, int32_t type,
    blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag>* list, Leave leave, Enter enter) {
    // FUN_1405ebd60
    blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag> previous;
    previous.assign_from(*list);
    blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag> entered;
    blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag> found;
    top->FindBestMouseFoci(&found, type, event, true, &entered);
    // FUN_1405ecca0
    list->assign_from(found);
    auto contains = [](blz::vector<CScriptRegion*, s_regionVectorTag>& regions, CScriptRegion* region) {
        for (uint64_t i = 0; i < regions.size(); i++) {
            if (regions[i] == region) {
                return true;
            }
        }
        return false;
    };
    for (uint64_t i = 0; i < previous.size(); i++) {
        if (!contains(*list, previous[i])) {
            leave(previous[i]);
        }
    }
    for (uint64_t i = 0; i < entered.size(); i++) {
        leave(entered[i]);
    }
    for (uint64_t i = 0; i < list->size(); i++) {
        CScriptRegion* region = (*list)[i];
        if (contains(entered, region) || !contains(previous, region)) {
            enter(region);
        }
    }
}

// Whether the gamepad drives the cursor (FUN_141524780 on DAT_142d082d0). The gamepad is not
// ported, so it never does.
// ref: FUN_141524780
bool GamePadCursorActive() {
    FROST_UNIMPLEMENTED();
    return false;
}

}  // namespace

// A mouse button went down: the regions the button already holds get it, or else those under
// the mouse that take the button (found afresh when one of them keeps the mouse), each through
// OnMouseDown.
// ref: FUN_1405f12f0
void CSimpleTop::OnMouseDownEvent(MOUSEEVENT* event) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 2;
    auto hook = reinterpret_cast<bool (*)(MOUSEEVENT*, bool)>(this->m_mouseDownHook);
    this->m_mouseEvent = *event;
    this->m_mouseEvent.m_x = g_coordinateSpace.m_diagonalWidth * event->m_x;
    this->m_mouseEvent.m_y = event->m_y * g_coordinateSpace.m_diagonalHeight;
    s_lastMouseTime = this->m_mouseEvent.m_time;
    UiSignalGlobalMouseDown(MouseButtonToString(event->m_button));
    blz::vector<CScriptRegion*, s_regionVectorTag>* regions = ButtonRegions(&this->m_buttonFocus, event->m_button);
    if (hook != nullptr && regions->size() == 0 && hook(&this->m_mouseEvent, true)) {
        this->m_inputPhase = phase;
        return;
    }
    if (regions->size() == 0) {
        bool keeps = false;
        for (uint64_t i = 0; i < this->m_mouseOver.size(); i++) {
            CScriptRegion* region = this->m_mouseOver[i];
            if ((region->m_passThroughButtons & event->m_button) != 0 && (static_cast<FrameScript_Object*>(region)->m_flags & 0x100) == 0) {
                keeps = true;
                break;
            }
        }
        if (keeps) {
            blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag> found;
            this->FindBestMouseFoci(&found, INPUT_EVENT_CLICK, &this->m_mouseEvent, false, nullptr);
            // FUN_1405f4590
            regions->assign_from(found);
        } else {
            regions->assign_from(this->m_mouseOver);
        }
    }
    for (uint64_t i = 0; i < regions->size(); i++) {
        CScriptRegion* region = (*regions)[i];
        if ((region->m_passThroughButtons & event->m_button) != 0) {
            continue;
        }
        this->m_inMouseScript = 1;
        region->OnMouseDown(&this->m_mouseEvent, nullptr);
        this->m_inMouseScript = 0;
    }
    this->m_inputPhase = phase;
}

// A mouse button came up: a move or size drag ends; the regions the button went down on get
// OnMouseUp, and the button lets them go.
// ref: FUN_1405f1910
void CSimpleTop::OnMouseUpEvent(MOUSEEVENT* event) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 2;
    MOUSEEVENT copy = *event;
    auto hook = reinterpret_cast<bool (*)(MOUSEEVENT*, bool)>(this->m_mouseDownHook);
    copy.m_x = g_coordinateSpace.m_diagonalWidth * event->m_x;
    copy.m_y = event->m_y * g_coordinateSpace.m_diagonalHeight;
    s_lastMouseTime = copy.m_time;
    if (static_cast<uint32_t>(this->m_dragType - 1) < 2) {
        if (this->m_dragFrame != nullptr) {
            this->m_dragFrame->SnapToNearestAnchor(this);
        }
        this->m_dragFrame = nullptr;
        this->m_dragType = 0;
        this->m_inputPhase = phase;
    }
    blz::vector<CScriptRegion*, s_regionVectorTag>* regions = ButtonRegions(&this->m_buttonFocus, event->m_button);
    if (hook != nullptr && regions->size() == 0 && hook(&copy, false)) {
        this->m_inputPhase = phase;
        return;
    }
    for (uint64_t i = 0; i < regions->size(); i++) {
        CScriptRegion* region = (*regions)[i];
        this->m_inMouseScript = 1;
        region->OnMouseUp(&copy, nullptr);
        this->m_inMouseScript = 0;
    }
    regions->clear();
    UiSignalGlobalMouseUp(MouseButtonToString(event->m_button));
    this->m_inputPhase = phase;
}

// The wheel turned: the topmost region with a wheel handler under the mouse gets it. Returns
// the handler's answer, or true when no region took it.
// ref: FUN_1405f1ab0
bool CSimpleTop::OnMouseWheelEvent(MOUSEEVENT* event) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 2;
    MOUSEEVENT copy = *event;
    copy.m_x = g_coordinateSpace.m_diagonalWidth * event->m_x;
    copy.m_y = event->m_y * g_coordinateSpace.m_diagonalHeight;
    s_lastMouseTime = copy.m_time;
    float point[2] = {this->m_mouseEvent.m_x, this->m_mouseEvent.m_y};
    CScriptRegion* best = nullptr;
    this->m_dispatchDepth++;
    blz::vector<CScriptRegion*, s_regionVectorTag>& regions = this->m_inputRegions[INPUT_EVENT_WHEEL];
    for (uint64_t i = 0; i < regions.size(); i++) {
        CScriptRegion* region = regions[i];
        if (region->HasWheelHandler() && region->IsMouseOver(point)
            && (best == nullptr || this->IsRegionAbove(best, region))) {
            best = region;
        }
    }
    this->m_dispatchDepth--;
    if (best == nullptr) {
        this->m_inputPhase = phase;
        return true;
    }
    bool result = best->OnMouseWheel(&copy);
    this->m_inputPhase = phase;
    return result;
}

// Relative mouse motion (the cursor held): only the move hook sees it. False when the hook
// took it.
// ref: FUN_1405f1860
bool CSimpleTop::OnMouseDeltaEvent(MOUSEEVENT* event) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 2;
    MOUSEEVENT copy = *event;
    auto hook = reinterpret_cast<bool (*)(MOUSEEVENT*, bool)>(this->m_mouseMoveHook);
    copy.m_x = g_coordinateSpace.m_diagonalWidth * event->m_x;
    copy.m_y = event->m_y * g_coordinateSpace.m_diagonalHeight;
    s_lastMouseTime = copy.m_time;
    if (hook != nullptr && hook(&copy, true)) {
        this->m_inputPhase = phase;
        return false;
    }
    this->m_inputPhase = phase;
    return true;
}

// The window took (true) or lost the focus: the hook at 0xf48 hears it.
// ref: FUN_1405effb0
bool CSimpleTop::OnFocusChanged(bool active) {
    if (this->m_keyCaptureHook != nullptr) {
        reinterpret_cast<void (*)(bool)>(this->m_keyCaptureHook)(active);
    }
    return true;
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
    HTEXTFONT m_handle;                     // 0x10
};

// The handles (0x142cce7a0).
TSList<UNNEEDED_FONTHANDLE, TSGetLink<UNNEEDED_FONTHANDLE>, 8> s_unneededFontHandles;

}

// ref: FUN_1405f6260
void UiAddUnneededFontHandle(const HTEXTFONT* handle) {
    void* memory = SMemAlloc(sizeof(UNNEEDED_FONTHANDLE), 8, s_fontHandleTag, 0, -2, 8);
    auto node = new (memory) UNNEEDED_FONTHANDLE();
    s_unneededFontHandles.LinkToTail(node);
    node->m_handle = *handle;
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

// ref: FUN_1405edde0
void CSimpleTop::AddLayoutWaiting(CLayoutFrame* frame) {
    bool inserted;
    blz::chained_insert<LAYOUTNODE>(&this->m_layoutWaiting, frame, &inserted, s_layoutNodeTag, s_layoutBucketTag);
}

// ref: FUN_1405f3390
void CSimpleTop::RemoveLayoutWaiting(CLayoutFrame* frame) {
    LAYOUTNODE* node = blz::chained_unlink<LAYOUTNODE>(&this->m_layoutWaiting, frame);
    if (node != nullptr) {
        blz::chained_free_node(node, s_layoutNodeTag);
    }
}

// ---- the unneeded textures ------------------------------------------------------------------
//
// A texture the UI lets go of stays loaded a while: the cache holds a reference to it, by its
// handle, so a frame that asks for it again soon finds it ready. What it keeps is bounded in
// bytes (and per texture), and the oldest go first.

// A texture the UI let go of (0x58, its heap index at +0x5c).
struct UNEEDED_TEXTURE : TSHashObject<UNEEDED_TEXTURE> {
    HTEXTURE m_key;                             // 0x30 HASHKEY_PTR
    TSLink<UNEEDED_TEXTURE> m_lruLink;          // 0x38 oldest first, while it has a size
    HTEXTURE m_texture;                         // 0x48 the reference the cache holds
    int32_t m_size;                             // 0x50 bytes; 0 not loaded yet, -1 over the per-texture limit
    uint32_t m_pad54;
};
static_assert(sizeof(UNEEDED_TEXTURE) == 0x58, "UNEEDED_TEXTURE is 0x58 bytes");

template <>
struct TSHashTableTags<UNEEDED_TEXTURE> {
    static constexpr const char* s_fixedArray =
        ".?AV?$TSFixedArray@V?$TSExplicitList@UUNEEDED_TEXTURE@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray =
        ".?AV?$TSGrowableArray@V?$TSExplicitList@UUNEEDED_TEXTURE@@$0?CCCCCCCD@$07@@$07@@";
};

namespace {

const char s_unneededTableTag[] = ".?AV?$TSHashTable@UUNEEDED_TEXTURE@@VHASHKEY_PTR@@$07@@";
const char s_unneededListTag[] = ".?AV?$TSList@UUNEEDED_TEXTURE@@V?$TSGetExplicitLink@UUNEEDED_TEXTURE@@@@$07@@";
const char s_topFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleTop.cpp";

// TSHashTable<UNEEDED_TEXTURE, HASHKEY_PTR, 8> (vtable 0x1426c76f8).
class UNEEDEDTEXTURETABLE : public TSHashTable<UNEEDED_TEXTURE> {
    public:
    // ref: FUN_1405ea070
    void Delete(UNEEDED_TEXTURE* node) override {
        node->m_lruLink.Unlink();
        this->Unlink(node);
        SMemFree(node, s_unneededTableTag, -2, 0);
    }

    // ref: FUN_1405ea0f0
    UNEEDED_TEXTURE* NewNode(TSExplicitList<UNEEDED_TEXTURE>* slot, uint32_t extra, uint32_t flags) override {
        void* memory = SMemAlloc(extra + sizeof(UNEEDED_TEXTURE), 8, s_unneededListTag, 0, -2, flags | 8);
        std::memset(memory, 0, sizeof(UNEEDED_TEXTURE));
        auto node = static_cast<UNEEDED_TEXTURE*>(memory);
        slot->LinkToTail(node);
        return node;
    }

    // ref: FUN_1405eea60
    void Resize(uint32_t buckets) {
        this->Rehash(buckets);
    }
};

UNEEDEDTEXTURETABLE s_unneededTextures;     // DAT_142cce720
uint32_t s_unneededBudget;                  // DAT_142cce70c: the bytes the cache may keep
int32_t s_unneededMaxSize;                  // DAT_142cce71c: the largest texture it keeps
// DAT_142cce770: the entries with a size, oldest first (link at +0x38).
TSList<UNEEDED_TEXTURE, TSGetLink<UNEEDED_TEXTURE>, 8> s_unneededLru(offsetof(UNEEDED_TEXTURE, m_lruLink));  // DAT_142cce770
TObjectAllocHeap* s_unneededHeap;           // DAT_142e979e8: "WUNNEEDEDTEXTURE"

// HASHKEY_PTR: MurmurHash3's finalizer over the pointer, folded to 32 bits.
uint32_t PtrHash(const void* ptr) {
    return static_cast<uint32_t>(blz::fmix64(reinterpret_cast<uint64_t>(ptr)));
}

// Lets go of the entry's texture and the entry.
void UnneededFree(UNEEDED_TEXTURE* entry) {
    TextureFree(&entry->m_texture);
    s_unneededTextures.Unlink(entry);
    entry->m_lruLink.Unlink();
    s_unneededHeap->Free(entry);
}

}  // namespace

// The cache's limits by the machine (a quarter of them under 2 GB of video memory or 3.5 GB
// of system memory), and its heap.
// ref: FUN_1405eef20
void UiUnneededTexturesInitialize() {
    if (s_unneededHeap == nullptr) {
        void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x22, s_topFile, SStrHashFnv1a(s_topFile), 0x8cf, 0);
        s_unneededHeap = new (memory) TObjectAllocHeap("WUNNEEDEDTEXTURE", 0x60, 0x80, 0x22);
    }
    uint32_t dedicated;
    uint32_t shared;
    GxDeviceVideoMemory(&dedicated, &shared);
    uint64_t physical = OsGetPhysicalMemory();
    if (shared <= dedicated) {
        shared = dedicated;
    }
    uint32_t buckets;
    if (shared < 0x7d1 || (physical >> 0x14) < 0xdad) {
        s_unneededBudget = 0xa00000;
        s_unneededMaxSize = 0x10000;
        s_unneededTextures.m_slotlimit = 0x80;
        buckets = 0x80;
    } else {
        s_unneededBudget = 0x4000000;
        s_unneededMaxSize = 0x800000;
        s_unneededTextures.m_slotlimit = 0x100;
        buckets = 0x100;
    }
    if (s_unneededTextures.m_slotmod + 1 < buckets) {
        s_unneededTextures.Resize(buckets);
    }
    // RenderSettingsAddHandler(FUN_1405f1c00, nullptr): the UI's answer to a resolution
    // change, not ported.
    FROST_UNIMPLEMENTED();
}

// The UI lets go of a texture: one from a file is parked in the cache (its reference kept, and
// moved to the newest end once its size is known) before the caller's reference goes.
// ref: FUN_1405f3c30
void UiReleaseTexture(CTexture** texture) {
    if (TextureInFileCache(*texture)) {
        HTEXTURE key = *texture;
        uint32_t hash = PtrHash(key);
        UNEEDED_TEXTURE* entry = s_unneededTextures.Find(hash, [key](UNEEDED_TEXTURE* node) {
            return node->m_key == key;
        });
        bool measure;
        if (entry != nullptr) {
            measure = entry->m_size == 0 && TextureIsReady(key);
        } else {
            entry = static_cast<UNEEDED_TEXTURE*>(s_unneededHeap->Alloc());
            std::memset(entry, 0, sizeof(UNEEDED_TEXTURE));
            s_unneededTextures.Insert(entry, hash);
            entry->m_hashval = hash;
            entry->m_key = key;
            entry->m_texture = *texture;
            TextureAddRef(*texture);
            measure = TextureIsReady(*texture);
            if (!measure) {
                entry->m_size = 0;
            }
        }
        if (measure) {
            int32_t size = TextureGetMemorySize(*texture);
            entry->m_size = size <= s_unneededMaxSize ? size : -1;
        }
        if (0 < entry->m_size) {
            s_unneededLru.Link(entry)->Unlink();
            s_unneededLru.LinkToTail(entry);
        }
    }
    TextureFree(texture);
}

// Once a frame: the entries in use again (or that never loaded) go, then the oldest until the
// rest fit the budget. `all` empties the cache.
// ref: FUN_1405f27e0
void UiTrimUnneededTextures(bool all) {
    if (all) {
        UNEEDED_TEXTURE* entry = s_unneededTextures.m_fulllist.Head();
        while (entry != nullptr) {
            UNEEDED_TEXTURE* next = s_unneededTextures.m_fulllist.Link(entry)->Next();
            UnneededFree(entry);
            entry = next;
        }
        return;
    }
    uint32_t total = 0;
    UNEEDED_TEXTURE* entry = s_unneededTextures.m_fulllist.Head();
    while (entry != nullptr) {
        UNEEDED_TEXTURE* next = s_unneededTextures.m_fulllist.Link(entry)->Next();
        if (entry->m_size < 1 || 1 < static_cast<uint32_t>(TextureGetRefCount(entry->m_texture))) {
            UnneededFree(entry);
        } else {
            total += static_cast<uint32_t>(entry->m_size);
        }
        entry = next;
    }
    entry = s_unneededLru.Head();
    while (entry != nullptr) {
        UNEEDED_TEXTURE* next = s_unneededLru.Next(entry);
        if (total <= s_unneededBudget) {
            return;
        }
        total -= static_cast<uint32_t>(entry->m_size);
        UnneededFree(entry);
        entry = next;
    }
}

// The cache emptied and its heap destroyed.
// ref: FUN_1405ee2c0
void UiUnneededTexturesShutdown() {
    // RenderSettingsRemoveHandler(FUN_1405f1c00, nullptr): never added (see the initialize)
    UiTrimUnneededTextures(true);
    if (s_unneededHeap != nullptr) {
        s_unneededHeap->~TObjectAllocHeap();
        SMemFree(s_unneededHeap, s_topFile, -2, 0);
        s_unneededHeap = nullptr;
    }
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

// The mouse moved (`real`; or the focus must be recomputed): a drag follows it; otherwise the
// regions holding a button and those under the mouse get OnMouseMove, and the regions under
// it get their enters and leaves. True when nothing of the UI is under the mouse.
// ref: FUN_1405f1530
bool CSimpleTop::OnMouseMoveEvent(MOUSEEVENT* event, bool real) {
    uint8_t phase = this->m_inputPhase;
    this->m_inputPhase = 2;
    auto hook = reinterpret_cast<bool (*)(MOUSEEVENT*, bool)>(this->m_mouseMoveHook);
    MOUSEEVENT copy = *event;
    copy.m_flags = (copy.m_flags & ~0xff00) | (real ? 0x100 : 0);
    if (real) {
        copy.m_x = g_coordinateSpace.m_diagonalWidth * event->m_x;
        copy.m_y = event->m_y * g_coordinateSpace.m_diagonalHeight;
        if (copy.m_x == this->m_mouseEvent.m_x && copy.m_y == this->m_mouseEvent.m_y) {
            this->m_inputPhase = phase;
            return false;
        }
        s_lastMouseTime = copy.m_time;
    } else if (GamePadCursorActive()) {
        // FUN_141523cf0: the gamepad's cursor position, not reached while GamePadCursorActive
        // is false
    }
    this->m_mouseEvent = copy;
    if (this->m_dragFrame != nullptr) {
        this->UpdateMouseFocus(&copy);
        this->m_inputPhase = phase;
        return false;
    }
    if (hook != nullptr && hook(&copy, false)) {
        this->m_inputPhase = phase;
        return false;
    }
    // LAB_1405f4ba0
    ForEachButton(&this->m_buttonFocus, [&](int32_t, blz::vector<CScriptRegion*, s_regionVectorTag>* regions) {
        for (uint64_t i = 0; i < regions->size(); i++) {
            (*regions)[i]->OnMouseMove(&copy);
        }
    });
    if (this->m_mouseFocus != nullptr) {
        this->m_inputPhase = phase;
        return true;
    }
    // LAB_1405f4ca0, LAB_1405f4cb0
    UpdateMouseRegions(this, &copy, INPUT_EVENT_CLICK, &this->m_mouseOver,
        [real](CScriptRegion* region) { region->OnMouseFocusLost(real, false); },
        [real](CScriptRegion* region) { region->OnMouseOverChanged(real); });
    if (this->m_motionTracking != 0) {
        this->m_mouseMotion.clear();
    } else {
        // LAB_1405f4cc0, LAB_1405f4cd0
        UpdateMouseRegions(this, &copy, INPUT_EVENT_MOTION, &this->m_mouseMotion,
            [real](CScriptRegion* region) { region->OnLeave(real); },
            [real](CScriptRegion* region) { region->OnEnter(real); });
    }
    for (uint64_t i = 0; i < this->m_mouseMotion.size(); i++) {
        CScriptRegion* region = this->m_mouseMotion[i];
        if (!this->IsInButtonCapture(region)) {
            region->OnMouseMove(&copy);
        }
    }
    bool result = this->m_mouseOver.size() == 0 && this->m_mouseMotion.size() == 0;
    this->m_inputPhase = phase;
    return result;
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

// ref: FUN_1405ee420
void CSimpleTop::EnableIme(bool enable) {
    if (enable != (this->m_imeEnabled != 0)) {
        this->m_imeEnabled = enable;
        OsImeEnable(enable);
    }
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1405ef2d0 = (void*)&IsBatchedRegionAbove;
extern "C" void* const gen_p_142cce720 = (void*)&s_unneededTextures;
extern "C" void* const gen_p_142cce770 = (void*)&s_unneededLru;
extern "C" void* const gen_p_142e979e0 = (void*)&s_lastMouseTime;
// ---- end of generated globals ----
