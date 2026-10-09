#include "ui/CSimpleTop.hpp"

#include <cstring>
#include <new>

#include "base/Coordinate.hpp"
#include "blz/Murmur.hpp"
#include "blz/RbTree.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/CSimpleFrame.hpp"
#include "util/Unimplemented.hpp"

const char s_regionVectorTag[] = ".PEAVCScriptRegion@@";
const char s_frameVectorTag[] = ".PEAVCSimpleFrame@@";
const char s_deferredInputTag[] = ".?AUDeferredInputEventChange@CSimpleTop@@";

CSimpleTop* CSimpleTop::s_instance;

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

}

// ref: FUN_1405ebe10
CSimpleTop::CSimpleTop(void (*preUpdate)()) {
    this->m_dragFrame = nullptr;
    this->m_dragType = 0;
    this->m_dragCorner = 0;
    this->m_dragX = 0.0f;
    this->m_dragY = 0.0f;
    this->m_unknown1C0 = 1;
    this->m_strataDirty = 0;
    // FUN_14061dce0: the root CRenderBatch (not ported yet)
    std::memset(this->m_renderBatch, 0, sizeof(this->m_renderBatch));
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
    this->m_frameCreatedHook = nullptr;
    this->m_preUpdateHook = preUpdate;
    std::memset(this->m_deferredScripts, 0, sizeof(this->m_deferredScripts));
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
    // FUN_140236a40 (the cursor position in UI units), FUN_1406204d0 (batch statics), the input
    // handler registration (FUN_1412814f0) and the Gx device callbacks (FUN_14228dfb0 /
    // FUN_14228e040) follow in the reference.
    FROST_UNIMPLEMENTED();
    this->m_lastMouseEvent = this->m_mouseEvent;
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
    } else if (type == INPUT_EVENT_MOTION) {
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
    } else if (type == INPUT_EVENT_CLICK) {
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
        if (!(region->m_layoutState & CLayoutFrame::LAYOUT_SHOWN)) {
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
    // FUN_140621e70: DeferredScriptBuffer::Add (not ported yet)
    (void)object;
    (void)index;
    FROST_UNIMPLEMENTED();
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
    if (frame->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) {
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
