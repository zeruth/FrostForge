#include "ui/CScriptRegion.hpp"

#include <cstring>

#include "base/Coordinate.hpp"
#include "base/Status.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lblizzard.h"
#include "lua.h"
}

namespace {

// The hit-rect insets a plain region has (0x142e97b10): none.
const float s_noInsets[4] = {0.0f, 0.0f, 0.0f, 0.0f};

const char s_mouseButtonTag[] = ".?AW4MOUSEBUTTON@@";

}

// ref: FUN_14060da50
CScriptRegion::CScriptRegion() {
    this->m_parent = nullptr;
    this->m_enabledInput = 0;
    this->m_inputFlags = 0;
    this->m_location.m_file = 0;
    this->m_location.m_line = 0;
    this->m_passThroughButtons = 0;
    // a guard-check call on the UI root with the new region stands here; it does nothing
}

// ref: FUN_14060dad0
CScriptRegion::~CScriptRegion() {
    CSimpleTop::s_instance->RemoveRegionInput(this, true);
    CSimpleTop::s_instance->OnRegionDestroyed(this);
}

// The handlers every region accepts: OnLoad, OnShow, OnHide, OnEnter/OnLeave (self, motion),
// OnMouseDown (self, button), OnMouseUp (self, button, upInside), OnMouseWheel (self, delta).
// ref: FUN_14060f6b0
bool CScriptRegion::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    switch (index) {
        case 4:
        case 12:
        case 13:
            return true;
        case 14:
        case 15:
            data->m_update = 3;
            data->m_wrapper = "return function(self,motion) %s end";
            return true;
        case 16:
            data->m_update = 2;
            data->m_wrapper = "return function(self,button) %s end";
            return true;
        case 17:
            data->m_update = 2;
            data->m_wrapper = "return function(self,button,upInside) %s end";
            return true;
        case 18:
            data->m_update = 4;
            data->m_wrapper = "return function(self,delta) %s end";
            return true;
        default:
            return false;
    }
}

// ref: FUN_14060f690
void CScriptRegion::SetSourceLocation(const SCRIPTLOCATION* location) {
    // FUN_1406607d0
    this->m_location.m_file = location->m_file;
    this->m_location.m_line = location->m_line;
}

// ref: FUN_14060f680
void CScriptRegion::PreLoadXML(XMLOBJECT* node, CStatus* status) {
    (void)status;
    this->SetSourceLocation(&node->m_location);
}

// Protecting a region protects what hangs off it, and the objects in its keyed table.
// ref: FUN_14060eef0
void CScriptRegion::OnProtected() {
    this->CLayoutFrame::ProtectDependents();
    // The reference walks the object list slot 0x88 returns and protects each entry; no
    // region class ported so far returns one.
    if (this->GetAnimatableObject() != nullptr) {
        FROST_UNIMPLEMENTED();
    }
}

// ref: FUN_14060e4b0
bool CScriptRegion::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Region") == 0 || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_14060e470
bool CScriptRegion::IsObjectType(int32_t type) {
    return type == CScriptRegion::GetObjectType() || this->CScriptObject::IsObjectType(type);
}

// ref: FUN_14060e2c0
const char* CScriptRegion::GetObjectTypeName() {
    return "Region";
}

// ref: FUN_14060e240
uint32_t CScriptRegion::GetObjectSize() {
    return 0x1d8;
}

// ref: FUN_14060e2e0
const SCRIPTLOCATION* CScriptRegion::GetScriptLocation() {
    return &this->m_location;
}

// ref: FUN_14060e2f0
void CScriptRegion::GetSourceLocation(blz::string_header* text) {
    ScriptLocationToString(&this->m_location, text);
}

// ref: FUN_14060e2d0
CScriptObject* CScriptRegion::GetParent() {
    return this->m_parent;
}

// The region half of an element: size, anchors (or setAllPoints), parentKey, parentArray, the
// mouse settings, the pass-through buttons and protection.
// ref: FUN_14060e7c0
void CScriptRegion::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    (void)intrinsic;
    auto region = static_cast<XMLREGION*>(node);
    this->PreLoadXML(node, status);
    XMLOBJECT* size = XMLObjectFindChild(node, XMLTYPE_SIZE);
    float width;
    float height;
    if (size != nullptr && XMLDimensionGet(static_cast<XMLDIMENSION*>(size), &width, &height, status)) {
        CLayoutFrame* layout = this;
        layout->SetWidth(width);
        layout->SetHeight(height);
    }
    uint32_t setAllPoints = region->m_setAllPoints;
    XMLOBJECT* anchors = XMLObjectFindChild(node, XMLTYPE_ANCHORS);
    if (anchors == nullptr) {
        if ((setAllPoints & 3) == 3) {
            auto relative = static_cast<CLayoutFrame*>(this->CLayoutFrame::GetLayoutParent());
            this->CLayoutFrame::SetAllPoints(relative, true);
        }
    } else {
        for (XMLOBJECT* anchor = anchors->m_child; anchor != nullptr; anchor = anchor->m_next) {
            if (anchor->m_type != XMLTYPE_ANCHOR) {
                continue;
            }
            XMLStatusSetLocation(static_cast<CScriptStatus*>(status), anchor);
            if (anchor->m_type == XMLTYPE_ANCHOR) {
                CScriptRegion_LoadAnchor(anchor, this, this, status);
            } else if (anchor->m_type == XMLTYPE_STARTANCHOR || anchor->m_type == XMLTYPE_ENDANCHOR) {
                CScriptRegion_LoadCenteredAnchor(anchor, this, this, status);
            }
        }
        this->CLayoutFrame::Resize(false);
    }
    uint32_t flags = region->m_regionFlags;
    if (flags & 4) {
        this->SetParentKey(region->m_parentKey.m_text);
    }
    if (flags & 8) {
        FrameXML_AddToParentArray(this, region->m_parentArray.m_text, status);
    }
    if ((flags & 0xc00) == 0xc00) {
        if (!this->IsMouseMotionEnabled()) {
            this->m_inputFlags |= REGION_MOUSE_MOTION;
            this->UpdateMotionRegistration();
        }
        if (!this->IsMouseClickEnabled()) {
            this->m_inputFlags |= REGION_MOUSE_CLICK;
            this->UpdateClickRegistration();
        }
    }
    if ((flags & 0x3000) == 0x3000 && !this->IsMouseClickEnabled()) {
        this->m_inputFlags |= REGION_MOUSE_CLICK;
        this->UpdateClickRegistration();
    }
    if ((flags & 0xc000) == 0xc000 && !this->IsMouseMotionEnabled()) {
        this->m_inputFlags |= REGION_MOUSE_MOTION;
        this->UpdateMotionRegistration();
    }
    if (flags & 0x20000) {
        blz::vector<uint32_t, s_mouseButtonTag> buttons;
        const char* cursor = region->m_passThroughButtons.m_text;
        char token[0x400];
        SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
        while (token[0] != '\0') {
            buttons.push_back(StringToMouseButton(token));
            SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
        }
        uint32_t mask = 0;
        for (uint32_t button : buttons) {
            mask |= button;
        }
        this->m_passThroughButtons = mask;
    }
    if ((flags & 0xc0000) == 0xc0000) {
        this->m_inputFlags |= REGION_CLICK_THROUGH;
    }
    if ((flags & 0x300000) == 0x300000
        && (CSimpleFrame::s_loading == nullptr || CSimpleTop::s_instance->IsSecureLoad())) {
        this->CLayoutFrame::SetProtected();
    }
}

// ref: FUN_14060ee20
void CScriptRegion::LoadXMLScripts(XMLOBJECT* node, CStatus* status, int32_t own) {
    lua_State* L = FrameScript_GetContext();
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_flags & 0x400) {
            XMLStatusSetLocation(static_cast<CScriptStatus*>(status), child);
            this->LoadScriptXML(L, static_cast<XMLSCRIPTHANDLER*>(child), status, own);
            this->OnScriptHandlerLoaded(child);
        }
    }
}

// ref: FUN_14060f190
void CScriptRegion::PostLoadXML(XMLOBJECT* node, CStatus* status) {
    if (!(node->m_flags & 0x100)) {
        for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
            if (child->m_type == XMLTYPE_SCRIPTS) {
                this->LoadXMLScripts(child, status, 1);
            }
        }
    }
    this->RunOnLoad();
    if ((this->m_layoutState & LAYOUT_VISIBLE) && !(this->FrameScript_Object::m_flags & 0x10) && this->HasScriptAny(12)) {
        CSimpleTop::s_instance->QueueScript(this, 12);
    }
}

// ref: FUN_14060f560
void CScriptRegion::SetParentArg(PARENTARG* parent) {
    if (!parent->m_set) {
        this->m_parent = nullptr;
    } else {
        CSimpleFrame* frame = parent->m_frame;
        this->m_parent = frame;
        if (frame != nullptr) {
            uint16_t flags = static_cast<FrameScript_Object*>(frame)->m_flags;
            if (((flags & FSOBJ_FORBIDDEN) || (flags & FSOBJ_PROTECTED_METATABLE)) && (flags & FSOBJ_PROTECTED)) {
                this->FrameScript_Object::SetProtected();
            }
        }
    }
    parent->m_set = false;
}

void CScriptRegion::Hide() {
}

void CScriptRegion::Show() {
}

void CScriptRegion::SetShown(bool shown) {
    (void)shown;
}

// ref: FUN_14060e130
uint8_t CScriptRegion::GetEffectiveAlpha() {
    return this->GetAlpha();
}

// ref: FUN_14060e530
bool CScriptRegion::IsDragging() {
    CScriptRegion* parent = static_cast<CScriptRegion*>(this->m_parent);
    return parent != nullptr && parent->IsDragging();
}

bool CScriptRegion::OnMouseMove(MOUSEEVENT* event) {
    (void)event;
    return false;
}

// ref: FUN_14060e150
uint64_t CScriptRegion::GetDrawOrderKey(int32_t layer, int32_t subLevel) {
    CScriptRegion* parent = static_cast<CScriptRegion*>(this->m_parent);
    return parent != nullptr ? parent->GetDrawOrderKey(layer, subLevel) : 0;
}

// ref: FUN_14060e450
uint64_t CScriptRegion::GetDrawOrder() {
    CScriptRegion* parent = static_cast<CScriptRegion*>(this->m_parent);
    return parent != nullptr ? parent->GetDrawOrder() : 0;
}

// ref: FUN_14060dc30
bool CScriptRegion::IsBatched() {
    CScriptRegion* parent = static_cast<CScriptRegion*>(this->m_parent);
    return parent != nullptr && parent->IsBatched();
}

bool CScriptRegion::HasCharHandler() {
    return false;
}

bool CScriptRegion::Unknown120() {
    return false;
}

bool CScriptRegion::HasKeyHandler() {
    return false;
}

bool CScriptRegion::HasPadButtonHandler(void* event) {
    (void)event;
    return false;
}

bool CScriptRegion::HasPadStickHandler(void* event) {
    (void)event;
    return false;
}

bool CScriptRegion::HasWheelHandler() {
    return false;
}

bool CScriptRegion::OnChar(int32_t key) {
    (void)key;
    return false;
}

bool CScriptRegion::Unknown150(void* event) {
    (void)event;
    return false;
}

bool CScriptRegion::OnKeyDown(void* event) {
    (void)event;
    return false;
}

bool CScriptRegion::OnKeyUp(void* event) {
    (void)event;
    return false;
}

bool CScriptRegion::Unknown168(void* event) {
    (void)event;
    return false;
}

// ref: FUN_14060eff0
void CScriptRegion::OnMouseDown(MOUSEEVENT* event, const char* button) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended) {
        return;
    }
    if (button == nullptr) {
        button = MouseButtonToString(event->m_button);
    }
    const char* previous = top->m_currentButton;
    top->m_currentButton = button;
    top->m_inMouseScript = 1;
    // FUN_140603870
    this->RunScript(16, nullptr, button);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    top->m_currentButton = previous;
}

// ref: FUN_14060f080
void CScriptRegion::OnMouseUp(MOUSEEVENT* event, const char* button) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended) {
        return;
    }
    if (button == nullptr) {
        button = MouseButtonToString(event->m_button);
    }
    const char* previous = top->m_currentButton;
    top->m_currentButton = button;
    float point[2] = {event->m_x, event->m_y};
    bool inside = this->IsMouseOver(point);
    top->m_inMouseScript = 1;
    // FUN_14060d8f0
    this->RunScript(17, nullptr, button, inside);
    CSimpleTop::s_instance->m_inMouseScript = 0;
    top->m_currentButton = previous;
}

bool CScriptRegion::OnMouseWheel(MOUSEEVENT* event) {
    (void)event;
    return false;
}

bool CScriptRegion::OnPadButtonDown(void* event) {
    (void)event;
    return false;
}

bool CScriptRegion::OnPadButtonUp(void* event) {
    (void)event;
    return false;
}

bool CScriptRegion::OnPadStick(void* event) {
    (void)event;
    return false;
}

// ref: FUN_14060e0d0
CScriptRegion* CScriptRegion::GetMouseTarget(int32_t type, MOUSEEVENT* event, bool* handled, bool force) {
    (void)handled;
    if (CSimpleTop::s_instance->m_inputSuspended) {
        return nullptr;
    }
    return this->HitTest(type, event, force) != 0 ? this : nullptr;
}

// Clicks need the click flag and a hit; the rest need the motion flag, a button that does
// not pass through (unless forced), and a hit.
// ref: FUN_14060fad0
int32_t CScriptRegion::HitTest(int32_t type, MOUSEEVENT* event, bool force) {
    if (type == INPUT_EVENT_MOTION) {
        if (!this->IsMouseMotionEnabled()) {
            return 0;
        }
    } else if (!this->IsMouseClickEnabled()
        || (!force && (static_cast<uint32_t>(event->m_button) & this->m_passThroughButtons) != 0)) {
        return 0;
    }
    float point[2] = {event->m_x, event->m_y};
    return this->IsMouseOver(point) ? 1 : 0;
}

// ref: FUN_14060e5a0
bool CScriptRegion::IsMouseMotionEnabled() {
    return (this->m_inputFlags & REGION_MOUSE_MOTION) != 0;
}

// ref: FUN_14060e550
bool CScriptRegion::IsMouseClickEnabled() {
    return (this->m_inputFlags & REGION_MOUSE_CLICK) != 0;
}

// ref: FUN_14060e030
void CScriptRegion::EnableMouseWheel(bool enable) {
    bool enabled = (this->m_enabledInput & 0x10) != 0;
    if (!enable) {
        if (enabled) {
            if (this->m_layoutState & LAYOUT_VISIBLE) {
                CSimpleTop::s_instance->UnregisterForInput(this, INPUT_EVENT_WHEEL, false);
            }
            this->m_enabledInput &= ~0x10;
        }
    } else if (!enabled) {
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->RegisterForInput(this, INPUT_EVENT_WHEEL);
        }
        this->m_enabledInput |= 0x10;
    }
}

// ref: FUN_14060e720
bool CScriptRegion::IsMouseWheelEnabled() {
    return (this->m_enabledInput >> 4) & 1;
}

// ref: FUN_14060e560
bool CScriptRegion::IsMouseEnabled() {
    return this->IsMouseClickEnabled() && this->IsMouseMotionEnabled();
}

// ref: FUN_14060df30
void CScriptRegion::EnableMouse(bool enable) {
    if (enable != this->IsMouseMotionEnabled()) {
        this->SetInputFlag(REGION_MOUSE_MOTION, enable);
        this->UpdateMotionRegistration();
    }
    if (enable != this->IsMouseClickEnabled()) {
        this->SetInputFlag(REGION_MOUSE_CLICK, enable);
        this->UpdateClickRegistration();
    }
}

// ref: FUN_14060dfd0
void CScriptRegion::SetMouseMotionEnabled(bool enable) {
    if (enable != this->IsMouseMotionEnabled()) {
        this->SetInputFlag(REGION_MOUSE_MOTION, enable);
        this->UpdateMotionRegistration();
    }
}

bool CScriptRegion::GetHyperlinksPropagated() {
    return false;
}

bool CScriptRegion::GetHyperlinksEnabled() {
    return false;
}

// ref: FUN_14060f150
void CScriptRegion::QueueOnShow() {
    if (!(this->FrameScript_Object::m_flags & 0x10) && this->HasScriptAny(12)) {
        CSimpleTop::s_instance->QueueScript(this, 12);
    }
}

// ref: FUN_14060efb0
void CScriptRegion::QueueOnHide() {
    if (!(this->FrameScript_Object::m_flags & 0x10) && this->HasScriptAny(13)) {
        CSimpleTop::s_instance->QueueScript(this, 13);
    }
}

// ref: FUN_14060ef70
void CScriptRegion::OnEnter(bool motion) {
    if (!CSimpleTop::s_instance->m_inputSuspended) {
        // FUN_1406039a0
        this->RunScript(14, nullptr, motion);
    }
}

// ref: FUN_14060ef90
void CScriptRegion::OnLeave(bool motion) {
    if (!CSimpleTop::s_instance->m_inputSuspended) {
        this->RunScript(15, nullptr, motion);
    }
}

void CScriptRegion::OnMouseOverChanged(bool over) {
    (void)over;
}

void CScriptRegion::OnMouseFocusLost(bool clear, bool quiet) {
    (void)clear;
    (void)quiet;
}

void CScriptRegion::OnDragStart(MOUSEEVENT* event) {
    (void)event;
}

void CScriptRegion::OnDragStop(MOUSEEVENT* event) {
    (void)event;
}

void CScriptRegion::OnReceiveDrag(MOUSEEVENT* event) {
    (void)event;
}

void CScriptRegion::OnLayoutVersionChanged() {
}

// ref: FUN_14060e140
const float* CScriptRegion::GetHitRectInsets() {
    return s_noInsets;
}

// ref: FUN_14060f770
bool CScriptRegion::IsMouseOver(const float* point) {
    if (this->CLayoutFrame::m_flags & FLAG_DIRTY) {
        this->CLayoutFrame::Resize(true);
    }
    if (!(this->CLayoutFrame::m_flags & FLAG_RECT_VALID)) {
        return false;
    }
    const float* insets = this->GetHitRectInsets();
    float scale = this->m_layoutScale;
    CRect hit;
    hit.minY = scale * insets[0] + this->m_rect.minY;
    hit.maxY = this->m_rect.maxY - scale * insets[2];
    hit.minX = scale * insets[1] + this->m_rect.minX;
    hit.maxX = this->m_rect.maxX - scale * insets[3];
    CRect transformed;
    CRect* rect = this->TransformRect(&transformed, &hit);
    float x = point[0];
    if (rect->minX <= x && (x < rect->maxX || x == rect->maxX)) {
        float y = point[1];
        if (rect->minY <= y && (y < rect->maxY || y == rect->maxY)) {
            return true;
        }
    }
    return false;
}

// A rect in the region's space carried to the screen through its parents.
// ref: FUN_14060dbd0
CRect* CScriptRegion::TransformRect(CRect* out, const CRect* rect) {
    CScriptRegion* parent = static_cast<CScriptRegion*>(this->m_parent);
    if (parent != nullptr) {
        if (static_cast<CSimpleFrame*>(parent)->m_frameFlags & 0x40) {
            parent = parent->m_parent;
        }
        CRect copy = *rect;
        return parent->TransformRect(out, &copy);
    }
    *out = *rect;
    return out;
}

// ref: FUN_14060e510
bool CScriptRegion::IsInScrollFrame() {
    CScriptRegion* parent = static_cast<CScriptRegion*>(this->m_parent);
    return parent != nullptr && parent->IsInScrollFrame();
}

// ref: FUN_14060e730
bool CScriptRegion::IsOnScreen() {
    CRect rect = this->m_rect;
    CRect screen;
    CRect* transformed = this->TransformRect(&screen, &rect);
    float bottom = transformed->minY;
    float left = transformed->minX;
    float top = transformed->maxY;
    float right = transformed->maxX;
    if (bottom < top && left < right) {
        if (bottom <= 0.0f) {
            bottom = 0.0f;
        }
        float height = g_coordinateSpace.m_diagonalHeight;
        if (top <= height) {
            height = top;
        }
        if (bottom < height) {
            if (left <= 0.0f) {
                left = 0.0f;
            }
            float width = g_coordinateSpace.m_diagonalWidth;
            if (right <= width) {
                width = right;
            }
            if (left < width) {
                return true;
            }
        }
    }
    return false;
}

// A handler that implies mouse input turns it on (autoEnableInput).
// ref: FUN_14060dd50
void CScriptRegion::OnScriptHandlerLoaded(XMLOBJECT* handler) {
    uint32_t flags = static_cast<XMLSCRIPTHANDLER*>(handler)->m_handlerFlags;
    if ((flags & 0x40) && (flags & 0x200)) {
        if (this->IsMouseMotionEnabled() != true) {
            this->m_inputFlags |= REGION_MOUSE_MOTION;
            this->UpdateMotionRegistration();
        }
        if (this->IsMouseClickEnabled() != true) {
            this->m_inputFlags |= REGION_MOUSE_CLICK;
            this->UpdateClickRegistration();
        }
    }
    if ((flags & 0x80) && (flags & 0x200)) {
        if (this->IsMouseMotionEnabled() != true) {
            this->m_inputFlags |= REGION_MOUSE_MOTION;
            this->UpdateMotionRegistration();
        }
        if (this->IsMouseClickEnabled() != true) {
            this->m_inputFlags |= REGION_MOUSE_CLICK;
            this->UpdateClickRegistration();
        }
    }
    if ((flags & 0x100) && (flags & 0x200) && !(this->m_enabledInput & 0x10)) {
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->RegisterForInput(this, INPUT_EVENT_WHEEL);
        }
        this->m_enabledInput |= 0x10;
    }
}

// ref: FUN_14060f390
void CScriptRegion::RunOnLoad() {
    // FUN_140603620
    this->RunScript(4, nullptr);
}

// ref: FUN_14060e220
void* CScriptRegion::GetLayoutParent() {
    CScriptRegion* parent = static_cast<CScriptRegion*>(this->m_parent);
    CLayoutFrame* layout = parent != nullptr ? static_cast<CLayoutFrame*>(parent) : nullptr;
    if (layout == nullptr) {
        layout = CSimpleTop::s_instance;
    }
    return layout;
}

// ref: FUN_14060dc60
CScriptRegion* CScriptRegion::GetScriptObject() {
    return this->GetScriptRegion();
}

// ref: FUN_14060dc50
CScriptRegion* CScriptRegion::GetScriptRegion() {
    return this;
}

// ref: FUN_14060e250
int32_t CScriptRegion::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// The layout frame of a global region of a name; a locked-down object still finds one.
// ref: FUN_14060e170
CLayoutFrame* CScriptRegion::GetRelativeByName(const char* name, CLayoutFrame* self) {
    (void)self;
    char buffer[0x400];
    this->CreateName(name, buffer, sizeof(buffer));
    bool inaccessible = (this->FrameScript_Object::m_flags & FSOBJ_LOCKDOWN)
        || !(this->FrameScript_Object::m_flags & FSOBJ_ACCESSIBLE);
    lua_State* L = FrameScript_GetContext();
    lua_pushstring(L, buffer);
    lua_rawget(L, LUA_GLOBALSINDEX);
    auto region = static_cast<CScriptRegion*>(
        FrameScript_Object::GetObjectFromLua(nullptr, L, CScriptRegion::GetObjectType(), -1, false, inaccessible));
    lua_settop(L, -2);
    return region != nullptr ? static_cast<CLayoutFrame*>(region) : nullptr;
}

// ref: FUN_14060dc70
CRect* CScriptRegion::GetHitRect(CRect* rect) {
    if (this->CLayoutFrame::m_flags & FLAG_RECT_VALID) {
        const float* insets = this->GetHitRectInsets();
        float scale = this->m_layoutScale;
        rect->minY = scale * insets[0] + this->m_rect.minY;
        rect->minX = scale * insets[1] + this->m_rect.minX;
        rect->maxY = this->m_rect.maxY - scale * insets[2];
        rect->maxX = this->m_rect.maxX - scale * insets[3];
        return rect;
    }
    *rect = {};
    return rect;
}

// ref: FUN_14060f240
bool CScriptRegion::CanQueryRect() {
    if (this->CLayoutFrame::IsProtected() && lua_taint != nullptr) {
        return CSimpleTop::s_instance->IsSecureLoad();
    }
    return true;
}

// ref: FUN_14060dd20
bool CScriptRegion::IsAccessibleToScripts() {
    if (this->CLayoutFrame::IsProtected() && !CSimpleTop::s_instance->IsSecureLoad()) {
        return false;
    }
    return true;
}

// ref: FUN_14060dee0
void CScriptRegion::EnableInputEvent(int32_t type) {
    uint16_t bit = static_cast<uint16_t>(1 << (type & 0x1f));
    if (!(this->m_enabledInput & bit)) {
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->RegisterForInput(this, type);
        }
        this->m_enabledInput |= bit;
    }
}

// ref: FUN_14060f460
void CScriptRegion::SetInputEvent(int32_t type, bool enable) {
    uint16_t bit = static_cast<uint16_t>(1 << (type & 0x1f));
    if (!enable) {
        if (this->m_enabledInput & bit) {
            if (this->m_layoutState & LAYOUT_VISIBLE) {
                CSimpleTop::s_instance->UnregisterForInput(this, type, false);
            }
            this->m_enabledInput &= ~bit;
        }
    } else if (!(this->m_enabledInput & bit)) {
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->RegisterForInput(this, type);
        }
        this->m_enabledInput |= bit;
    }
}

// The buttons a click passes through on, OR'd from the list SetPassThroughButtons read.
// ref: FUN_14060f5c0
void CScriptRegion::SetPassThroughButtons(const uint32_t* buttons, uint64_t count) {
    uint32_t mask = 0;
    for (uint64_t i = 0; i < count; i++) {
        mask |= buttons[i];
    }
    this->m_passThroughButtons = mask;
}

// ref: FUN_14060f500
void CScriptRegion::SetMouseClickEnabled(bool enable) {
    if (enable != this->IsMouseClickEnabled()) {
        this->SetInputFlag(REGION_MOUSE_CLICK, enable);
        this->UpdateClickRegistration();
    }
}

// ref: FUN_14060f660
void CScriptRegion::SetInputFlag(uint8_t flag, bool set) {
    if (set) {
        this->m_inputFlags |= flag;
        return;
    }
    this->m_inputFlags &= ~flag;
}

// ref: FUN_14060e7b0
bool CScriptRegion::HasInputFlags(uint8_t flags) {
    return (this->m_inputFlags & flags) == flags;
}

// Motion input is wanted while motion is enabled or a motion script needs it.
// ref: FUN_14060f990
void CScriptRegion::UpdateClickRegistration() {
    bool wanted = this->IsMouseClickEnabled() || this->GetHyperlinksEnabled();
    bool registered = (this->m_enabledInput & 4) != 0;
    if (!wanted) {
        if (!registered) {
            return;
        }
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->UnregisterForInput(this, INPUT_EVENT_CLICK, false);
        }
        this->m_enabledInput &= ~4;
        return;
    }
    if (!registered) {
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->RegisterForInput(this, INPUT_EVENT_CLICK);
        }
        this->m_enabledInput |= 4;
    }
}

// ref: FUN_14060fa30
void CScriptRegion::UpdateMotionRegistration() {
    bool wanted = this->IsMouseMotionEnabled() || this->GetHyperlinksEnabled();
    bool registered = (this->m_enabledInput & 8) != 0;
    if (!wanted) {
        if (!registered) {
            return;
        }
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->UnregisterForInput(this, INPUT_EVENT_MOTION, false);
        }
        this->m_enabledInput &= ~8;
        return;
    }
    if (!registered) {
        if (this->m_layoutState & LAYOUT_VISIBLE) {
            CSimpleTop::s_instance->RegisterForInput(this, INPUT_EVENT_MOTION);
        }
        this->m_enabledInput |= 8;
    }
}

// Registers each enabled input event kind (as the region is shown).
// ref: FUN_14060f280
void CScriptRegion::RegisterEnabledInput() {
    for (int32_t type = 0; type < 7; type++) {
        if (this->m_enabledInput & (1 << type)) {
            CSimpleTop::s_instance->RegisterForInput(this, type);
        }
    }
}

// ref: FUN_14060f880
void CScriptRegion::UnregisterEnabledInput(bool quiet) {
    for (int32_t type = 0; type < 7; type++) {
        if (this->m_enabledInput & (1 << type)) {
            CSimpleTop::s_instance->UnregisterForInput(this, type, quiet);
        }
    }
}

// Whether the cursor is over the region's rect grown by the given offsets.
// ref: FUN_14060e5f0
bool CScriptRegion::IsMouseOverOffset(float top, float bottom, float left, float right) {
    CSimpleTop* root = CSimpleTop::s_instance;
    if (!ScriptRegion_CheckAction(ACTION_FRAME_MEASUREMENT, this->GetScriptRegion(), "IsMouseOver()") || root == nullptr) {
        return false;
    }
    if (this->CLayoutFrame::m_flags & FLAG_DIRTY) {
        this->CLayoutFrame::Resize(true);
    }
    CRect rect = {};
    if (!this->CLayoutFrame::GetRect(&rect)) {
        return false;
    }
    float inverse = 1.0f / this->m_layoutScale;
    float x = inverse * root->m_mouseEvent.m_x;
    float y = inverse * root->m_mouseEvent.m_y;
    return inverse * rect.minX + left < x && x < inverse * rect.maxX + right && inverse * rect.minY + bottom < y
        && y < inverse * rect.maxY + top;
}

// ref: FUN_14060e5b0
bool CScriptRegion::IsInMotionList() {
    for (CScriptRegion* region : CSimpleTop::s_instance->m_mouseMotion) {
        if (region == this) {
            return true;
        }
    }
    return false;
}

// ref: FUN_14060e300
blz::string* CScriptRegion::GetSourceLocationString(blz::string* text) {
    blz::inline_string<0x30c> location;
    this->GetSourceLocation(location.header());
    new (text) blz::string();
    text->construct(location.m_data, location.m_size);
    return text;
}

namespace {

// The anchor arms differ only in how the anchor's numbers are read.
template <bool Centered>
void LoadAnchorImpl(XMLOBJECT* node, CScriptRegion* region, CLayoutFrame* layout, CStatus* status) {
    auto anchor = static_cast<XMLANCHOR*>(node);
    CLayoutFrame* self = region != nullptr ? static_cast<CLayoutFrame*>(region) : nullptr;
    const char* name = region->GetName();
    const char* owner = name != nullptr ? name : "Unknown";
    uint32_t point;
    uint32_t relativePoint;
    float x = 0.0f;
    float y = 0.0f;
    bool ok = Centered ? XMLAnchorGetCentered(anchor, &point, &relativePoint, &x, &y, status)
                       : XMLAnchorGet(anchor, &point, &relativePoint, &x, &y, status);
    if (!ok) {
        status->Add(1, "%s: Invalid anchor point info.", owner);
        return;
    }
    uint32_t flags = anchor->m_anchorFlags;
    if ((flags & 0xc) == 0xc) {
        status->Add(1, "%s: has relativeTo AND relativeKey (bad dog, no biscuit!)", owner);
        return;
    }
    CLayoutFrame* relative = self;
    if (flags & 4) {
        relative = static_cast<CLayoutFrame*>(region)->GetRelativeByName(anchor->m_relativeTo.m_text, self);
        if (relative == nullptr) {
            status->Add(1, "%s: Couldn't find relative frame: %s", owner, anchor->m_relativeTo.m_text);
            return;
        }
        if (relative == self) {
            status->Add(1, "%s: anchored to itself: %s", owner, anchor->m_relativeTo.m_text);
            return;
        }
    }
    if (flags & 8) {
        CScriptObject* object = region->GetObjectByPath(anchor->m_relativeKey.m_text);
        if (object == nullptr) {
            status->Add(1, "%s: Couldn't find relative key: %s", owner, anchor->m_relativeKey.m_text);
            return;
        }
        if (!object->IsObjectType(CScriptRegion::GetObjectType())) {
            status->Add(1, "%s: Attempt to anchor to non-region: %s", owner, anchor->m_relativeKey.m_text);
            return;
        }
        relative = static_cast<CLayoutFrame*>(static_cast<CScriptRegion*>(object));
        if (relative == self) {
            status->Add(1, "%s: anchored to itself: %s", owner, anchor->m_relativeKey.m_text);
            return;
        }
    }
    if ((flags & 0xc) == 0) {
        relative = static_cast<CLayoutFrame*>(region->GetLayoutParent());
    }
    layout->SetPoint(static_cast<int32_t>(point), relative, static_cast<int32_t>(relativePoint), x, y, false);
}

}

// ref: FUN_14060d280
void CScriptRegion_LoadAnchor(XMLOBJECT* anchor, CScriptRegion* region, CLayoutFrame* layout, CStatus* status) {
    LoadAnchorImpl<false>(anchor, region, layout, status);
}

// ref: FUN_14060d520
void CScriptRegion_LoadCenteredAnchor(XMLOBJECT* anchor, CScriptRegion* region, CLayoutFrame* layout, CStatus* status) {
    LoadAnchorImpl<true>(anchor, region, layout, status);
}

// ref: FUN_140610860
const char* MouseButtonToString(int32_t button) {
    switch (static_cast<uint32_t>(button)) {
        case 0x1:
            return "LeftButton";
        case 0x2:
            return "MiddleButton";
        case 0x4:
            return "RightButton";
        case 0x8:
            return "Button4";
        case 0x10:
            return "Button5";
        case 0x20:
            return "Button6";
        case 0x40:
            return "Button7";
        case 0x80:
            return "Button8";
        case 0x100:
            return "Button9";
        case 0x200:
            return "Button10";
        case 0x400:
            return "Button11";
        case 0x800:
            return "Button12";
        case 0x1000:
            return "Button13";
        case 0x2000:
            return "Button14";
        case 0x4000:
            return "Button15";
        case 0x8000:
            return "Button16";
        case 0x10000:
            return "Button17";
        case 0x20000:
            return "Button18";
        case 0x40000:
            return "Button19";
        case 0x80000:
            return "Button20";
        case 0x100000:
            return "Button21";
        case 0x200000:
            return "Button22";
        case 0x400000:
            return "Button23";
        case 0x800000:
            return "Button24";
        case 0x1000000:
            return "Button25";
        case 0x2000000:
            return "Button26";
        case 0x4000000:
            return "Button27";
        case 0x8000000:
            return "Button28";
        case 0x10000000:
            return "Button29";
        case 0x20000000:
            return "Button30";
        case 0x40000000:
            return "Button31";
        default:
            return "UNKNOWN";
    }
}

namespace {

// The ASCII comparison the reference inlines: only A-Z fold to lower case.
bool EqualsFolded(const char* a, const char* b) {
    while (true) {
        int32_t ca = static_cast<int8_t>(*a++);
        int32_t cb = static_cast<int8_t>(*b++);
        if (static_cast<uint32_t>(ca - 'A') < 26) {
            ca += 0x20;
        }
        if (static_cast<uint32_t>(cb - 'A') < 26) {
            cb += 0x20;
        }
        if (ca != cb) {
            return false;
        }
        if (ca == 0) {
            return true;
        }
    }
}

struct MOUSEBUTTONNAME {
    const char* m_name;
    uint32_t m_button;
};

const MOUSEBUTTONNAME s_mouseButtonNames[] = {
    {"LeftButton", 0x1}, {"Button1", 0x1}, {"RightButton", 0x4}, {"Button2", 0x4}, {"MiddleButton", 0x2},
    {"Button3", 0x2}, {"Button4", 0x8}, {"Button5", 0x10}, {"Button6", 0x20}, {"Button7", 0x40},
    {"Button8", 0x80}, {"Button9", 0x100}, {"Button10", 0x200}, {"Button11", 0x400}, {"Button12", 0x800},
    {"Button13", 0x1000}, {"Button14", 0x2000}, {"Button15", 0x4000}, {"Button16", 0x8000},
    {"Button17", 0x10000}, {"Button18", 0x20000}, {"Button19", 0x40000}, {"Button20", 0x80000},
    {"Button21", 0x100000}, {"Button22", 0x200000}, {"Button23", 0x400000}, {"Button24", 0x800000},
    {"Button25", 0x1000000}, {"Button26", 0x2000000}, {"Button27", 0x4000000}, {"Button28", 0x8000000},
    {"Button29", 0x10000000}, {"Button30", 0x20000000}, {"Button31", 0x40000000},
};

}

// ref: FUN_140611e30
uint32_t StringToMouseButton(const char* name) {
    if (name == nullptr || *name == '\0') {
        return 0;
    }
    for (const MOUSEBUTTONNAME& entry : s_mouseButtonNames) {
        if (EqualsFolded(name, entry.m_name)) {
            return entry.m_button;
        }
    }
    return 0;
}
