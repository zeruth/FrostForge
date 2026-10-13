#include "ui/CSimpleScrollFrame.hpp"

#include <cmath>
#include <cstring>

#include "base/Coordinate.hpp"
#include "base/DataAllocator.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

int32_t CSimpleScrollFrame::s_metaTable = -2;
int32_t CSimpleScrollFrame::s_restrictedMetaTable = -2;

namespace {

// The scroll frames' pool (0x1447aee78, made by the static initializer 0x14020dce0).
CDataAllocator s_scrollFrameHeap(0x320, 5);

// The scripts of a scroll frame.
enum : uint8_t {
    SCRIPT_ONSCROLLRANGECHANGED = 0x25,
    SCRIPT_ONVERTICALSCROLL = 0x26,
    SCRIPT_ONHORIZONTALSCROLL = 0x27,
};

// The smallest change that counts (DAT_14261a958, 2^-20).
constexpr float SCROLL_EPSILON = 9.5367432e-07f;

// A layout length in UI units, as the scripts get it.
float ToUIUnits(float value) {
    const COORDINATESPACE& space = g_coordinateSpace;
    return space.m_aspectCompensation * 1024.0f * value * (1.0f / space.m_diagonalWidth);
}

const char* DisplayName(CSimpleFrame* frame) {
    const char* name = frame->GetName();
    return name != nullptr ? name : frame->GetObjectTypeName();
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1424c0c80
CSimpleScrollFrame::CSimpleScrollFrame(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_updateScrollChild = false;
    this->m_scrollChild = nullptr;
    this->m_horizontalRange = 0.0f;
    this->m_verticalRange = 0.0f;
    this->m_horizontalScroll = 0.0f;
    this->m_verticalScroll = 0.0f;
}

// ref: FUN_1424c0cf0
CSimpleScrollFrame::~CSimpleScrollFrame() = default;

void* CSimpleScrollFrame::operator new(size_t size) {
    (void)size;
    return s_scrollFrameHeap.GetData(false, ".?AVCSimpleScrollFrame@@", -2, 0x1f);
}

void CSimpleScrollFrame::operator delete(void* frame) {
    if (frame != nullptr) {
        s_scrollFrameHeap.PutData(frame);
    }
}

// DAT_14309fef4
// The reference's type id and its magic-static guard (CSimpleScrollFrame), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleScrollFrameType;  // DAT_14309fef4
int32_t g_cSimpleScrollFrameTypeGuard;  // DAT_14309fef8

int32_t CSimpleScrollFrame::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleScrollFrameType, &g_cSimpleScrollFrameTypeGuard);
}

// ref: FUN_1424c0d80
void CSimpleScrollFrame::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleScrollFrame_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleScrollFrame_FillScriptMethods, true);
}

// ref: FUN_1424c0dd0
void CSimpleScrollFrame::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1424c0e40
int32_t CSimpleScrollFrame::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424c1790
bool CSimpleScrollFrame::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (index == SCRIPT_ONSCROLLRANGECHANGED) {
        data->m_wrapper = "return function(self,xrange,yrange) %s end";
        return true;
    }
    if (index == SCRIPT_ONVERTICALSCROLL || index == SCRIPT_ONHORIZONTALSCROLL) {
        data->m_wrapper = "return function(self,offset) %s end";
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_1424c0ff0
bool CSimpleScrollFrame::IsObjectTypeName(const char* name) {
    return _stricmp(name, "ScrollFrame") == 0 || _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1424c0e90
bool CSimpleScrollFrame::IsObjectType(int32_t type) {
    if (type == CSimpleScrollFrame::GetObjectType() || type == CSimpleFrame::GetObjectType()
        || type == CScriptRegion::GetObjectType()) {
        return true;
    }
    return this->CScriptObject::IsObjectType(type);
}

// ref: FUN_1424c0e30
const char* CSimpleScrollFrame::GetObjectTypeName() {
    return "ScrollFrame";
}

// ref: FUN_1424c0e20
uint32_t CSimpleScrollFrame::GetObjectSize() {
    return 0x320;
}

// ---- XML ----------------------------------------------------------------------------------------

// ref: FUN_1424c1070
void CSimpleScrollFrame::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    XMLOBJECT* scrollChild = XMLObjectFindChild(node, XMLTYPE_SCROLLCHILD);
    if (scrollChild == nullptr) {
        return;
    }
    XMLOBJECT* frameNode = scrollChild->m_child;
    if (frameNode == nullptr) {
        status->Add(1, "Frame %s: Scroll frame created without scroll child", DisplayName(this));
        return;
    }
    if (!(frameNode->m_flags & 0x100)) {
        status->Add(1, "Frame %s: Invalid scroll child", DisplayName(this));
        return;
    }
    CSimpleFrame* child = FrameXML_CreateFrame(frameNode, this, static_cast<CScriptStatus*>(status));
    if (child == nullptr) {
        return;
    }
    this->SetScrollChild(child);
}

// ---- overrides ----------------------------------------------------------------------------------

// ref: FUN_1424c0e70
bool CSimpleScrollFrame::IsChildShown(CScriptRegion* child) {
    return (child->m_layoutState & LAYOUT_SHOWN) && child != this->m_scrollChild;
}

// ref: FUN_1424c12a0
void CSimpleScrollFrame::RunOnUpdate(float elapsed) {
    this->CSimpleFrame::RunOnUpdate(elapsed);
    if (this->m_updateScrollChild) {
        this->UpdateScrollChildRect();
    }
}

bool CSimpleScrollFrame::WantsUpdate() {
    return true;
}

// ref: FUN_1424c1280
void CSimpleScrollFrame::OnSizeChanged(float width, float height) {
    this->CSimpleFrame::OnSizeChanged(width, height);
    this->m_updateScrollChild = true;
}

// ref: FUN_1424c17d0
void CSimpleScrollFrame::RemoveChild(CSimpleFrame* child) {
    if (child == this->m_scrollChild) {
        this->m_scrollChild = nullptr;
    }
    this->CSimpleFrame::RemoveChild(child);
}

// ---- scrolling ----------------------------------------------------------------------------------

void CSimpleScrollFrame::AnchorScrollChild() {
    CSimpleFrame* child = this->m_scrollChild;
    if (child == nullptr) {
        return;
    }
    child->ClearAllPointsVirtual();
    child->SetPoint(FRAMEPOINT_TOPLEFT, this, FRAMEPOINT_TOPLEFT, -this->m_horizontalScroll, this->m_verticalScroll, true);
}

// ref: FUN_1424c1560
void CSimpleScrollFrame::SetScrollChild(CSimpleFrame* child) {
    if (this->m_scrollChild == child) {
        return;
    }
    for (CSimpleFrame* frame = this; frame != nullptr; frame = frame->m_parent) {
        if (frame == child) {
            FrameScript_ActionError("Would create a loop adding child %s", DisplayName(child));
            return;
        }
    }
    if (this->m_scrollChild != nullptr) {
        this->m_scrollChild->SetInScrollFrame(false, 1);
        PARENTARG none = { nullptr, true };
        this->m_scrollChild->SetParentArg(&none);
    }
    this->m_scrollChild = child;
    if (child != nullptr) {
        PARENTARG parent = { this, true };
        child->SetParentArg(&parent);
        this->m_scrollChild->SetInScrollFrame(true, 2);
    }
    this->AnchorScrollChild();
    this->m_updateScrollChild = true;
    this->MarkParentBatchDirty();
}

// ref: FUN_1424c1490
void CSimpleScrollFrame::SetHorizontalScroll(float offset) {
    if (std::fabs(offset - this->m_horizontalScroll) < SCROLL_EPSILON) {
        return;
    }
    this->m_horizontalScroll = offset;
    this->AnchorScrollChild();
    // FUN_140603740
    this->RunScript(SCRIPT_ONHORIZONTALSCROLL, nullptr, ToUIUnits(this->m_horizontalScroll));
}

// ref: FUN_1424c16c0
void CSimpleScrollFrame::SetVerticalScroll(float offset) {
    if (std::fabs(offset - this->m_verticalScroll) < SCROLL_EPSILON) {
        return;
    }
    this->m_verticalScroll = offset;
    this->AnchorScrollChild();
    this->RunScript(SCRIPT_ONVERTICALSCROLL, nullptr, ToUIUnits(this->m_verticalScroll));
}

// ref: FUN_1424c17f0
void CSimpleScrollFrame::UpdateScrollChildRect() {
    if (this->CLayoutFrame::m_flags & FLAG_DIRTY) {
        this->Resize(true);
    }
    CRect rect = {};
    if (!this->GetRect(&rect)) {
        this->m_updateScrollChild = false;
        return;
    }
    if (this->m_scrollChild == nullptr) {
        this->m_updateScrollChild = false;
        return;
    }
    float inverse = 1.0f / this->m_layoutScale;
    float height = rect.maxY - rect.minY;
    float width = rect.maxX - rect.minX;
    float oldVertical = this->m_verticalRange;
    float oldHorizontal = this->m_horizontalRange;
    CRect bounds;
    this->m_scrollChild->GetBoundsRect(&bounds);
    float horizontal = (bounds.maxX - bounds.minX) - width * inverse;
    this->m_horizontalRange = horizontal <= 0.0f ? 0.0f : horizontal;
    float vertical = (bounds.maxY - bounds.minY) - height * inverse;
    this->m_verticalRange = vertical <= 0.0f ? 0.0f : vertical;
    this->m_scrollChild->UpdateClipRect();
    if (std::fabs(this->m_horizontalRange - oldHorizontal) >= SCROLL_EPSILON
        || std::fabs(this->m_verticalRange - oldVertical) >= SCROLL_EPSILON) {
        // FUN_140612970
        this->RunScript(SCRIPT_ONSCROLLRANGECHANGED, nullptr, ToUIUnits(this->m_horizontalRange), ToUIUnits(this->m_verticalRange));
    }
    this->m_updateScrollChild = false;
}

// ---- script methods (table 0x142cf9960) ------------------------------------------------------

namespace {

using namespace ScriptMethod;

CSimpleScrollFrame* ScrollSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleScrollFrame*>(Self(L, CSimpleScrollFrame::GetObjectType(), index, usage));
}

int PushFloatField(lua_State* L, const char* usage, const char* method, float CSimpleScrollFrame::*field) {
    int32_t index = 1;
    CSimpleScrollFrame* frame = ScrollSelf(L, &index, usage);
    float value = frame->*field;
    RaisePendingError(L, frame, method);
    return PushUIUnits(L, value);
}

// ref: FUN_140a9d6a0
int GetHorizontalScroll(lua_State* L) {
    return PushFloatField(L, "Usage: local offset = self:GetHorizontalScroll()", "GetHorizontalScroll",
        &CSimpleScrollFrame::m_horizontalScroll);
}

// ref: FUN_140a9e420
int GetHorizontalScrollRange(lua_State* L) {
    return PushFloatField(L, "Usage: local range = self:GetHorizontalScrollRange()", "GetHorizontalScrollRange",
        &CSimpleScrollFrame::m_horizontalRange);
}

// ref: FUN_140a9ff70
int GetVerticalScroll(lua_State* L) {
    return PushFloatField(L, "Usage: local offset = self:GetVerticalScroll()", "GetVerticalScroll",
        &CSimpleScrollFrame::m_verticalScroll);
}

// ref: FUN_140aa0cf0
int GetVerticalScrollRange(lua_State* L) {
    return PushFloatField(L, "Usage: local range = self:GetVerticalScrollRange()", "GetVerticalScrollRange",
        &CSimpleScrollFrame::m_verticalRange);
}

// ref: FUN_140a9f1a0
int GetScrollChild(lua_State* L) {
    int32_t index = 1;
    CSimpleScrollFrame* frame = ScrollSelf(L, &index, "Usage: local scrollChild = self:GetScrollChild()");
    CSimpleFrame* child = frame->m_scrollChild;
    RaisePendingError(L, frame, "GetScrollChild");
    return PushObject(L, child);
}

// ref: FUN_140aa1a70
int SetHorizontalScroll(lua_State* L) {
    const char* usage = "Usage: self:SetHorizontalScroll(offset)";
    int32_t index = 1;
    CSimpleScrollFrame* frame = ScrollSelf(L, &index, usage);
    float offset = RequiredUIUnits(L, &index, usage);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->SetHorizontalScroll(offset);
    RaisePendingError(L, frame, "SetHorizontalScroll");
    return 0;
}

// ref: FUN_140aa3730
int SetVerticalScroll(lua_State* L) {
    const char* usage = "Usage: self:SetVerticalScroll(offset)";
    int32_t index = 1;
    CSimpleScrollFrame* frame = ScrollSelf(L, &index, usage);
    float offset = RequiredUIUnits(L, &index, usage);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->SetVerticalScroll(offset);
    RaisePendingError(L, frame, "SetVerticalScroll");
    return 0;
}

// ref: FUN_140aa2910
int SetScrollChild(lua_State* L) {
    const char* usage = "Usage: self:SetScrollChild(scrollChild)";
    int32_t index = 1;
    CSimpleScrollFrame* frame = ScrollSelf(L, &index, usage);
    CSimpleFrame* child = nullptr;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else {
        child = static_cast<CSimpleFrame*>(Self(L, CSimpleFrame::GetObjectType(), &index, usage));
    }
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->SetScrollChild(child);
    RaisePendingError(L, frame, "SetScrollChild");
    return 0;
}

// ref: FUN_140aa45d0
int UpdateScrollChildRect(lua_State* L) {
    int32_t index = 1;
    CSimpleScrollFrame* frame = ScrollSelf(L, &index, "Usage: self:UpdateScrollChildRect()");
    frame->UpdateScrollChildRect();
    RaisePendingError(L, frame, "UpdateScrollChildRect");
    return 0;
}

// DAT_142cf9960
const luaL_Reg s_scrollFrameMethods[] = {
    { "GetHorizontalScroll", GetHorizontalScroll },
    { "GetHorizontalScrollRange", GetHorizontalScrollRange },
    { "GetScrollChild", GetScrollChild },
    { "GetVerticalScroll", GetVerticalScroll },
    { "GetVerticalScrollRange", GetVerticalScrollRange },
    { "SetHorizontalScroll", SetHorizontalScroll },
    { "SetScrollChild", SetScrollChild },
    { "SetVerticalScroll", SetVerticalScroll },
    { "UpdateScrollChildRect", UpdateScrollChildRect },
};

}

// ref: FUN_140ae5920
void CSimpleScrollFrame_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_scrollFrameMethods, 9);
}
