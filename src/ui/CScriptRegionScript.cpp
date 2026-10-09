// The Lua methods of every region (CScriptRegion's table 0x142cf9630) and of everything laid
// out by anchors (CLayoutFrame's table 0x142cf98a0).

#include "ui/CScriptObjectScript.hpp"

#include "blz/String.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/CLayoutFrame.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace {

using namespace ScriptMethod;

CScriptRegion* RegionSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CScriptRegion*>(Self(L, CScriptRegion::GetObjectType(), index, usage));
}

CLayoutFrame* Layout(CScriptRegion* region) {
    return static_cast<CLayoutFrame*>(region);
}

// Self for the methods that report through the object error instead of the usage error.
// ref: FUN_1424ccec0
CScriptRegion* GetRegionFromLua(lua_State* L) {
    const char* error = nullptr;
    auto region = static_cast<CScriptRegion*>(FrameScript_Object::GetObjectFromLua(&error, L, CScriptRegion::GetObjectType(), 1, false, false));
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    if (region != nullptr && !Layout(region)->CanBeQueried()) {
        FrameScript_ActionError("Wrong object type for function");
        return nullptr;
    }
    return region;
}

// The same lookup without the queryable check, for the script-handler methods.
CScriptRegion* GetScriptRegionFromLua(lua_State* L) {
    const char* error = nullptr;
    auto region = static_cast<CScriptRegion*>(FrameScript_Object::GetObjectFromLua(&error, L, CScriptRegion::GetObjectType(), 1, false, false));
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    return region;
}

// ---- CScriptRegion ---------------------------------------------------------------------

// ref: FUN_140a69780
int CanChangeProtectedState(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local canChange = self:CanChangeProtectedState()");
    bool canChange = region->IsAccessibleToScripts();
    RaisePendingError(L, region, "CanChangeProtectedState");
    return PushBool(L, canChange);
}

// ref: FUN_140a6a520
int EnableMouse(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:EnableMouse([enable])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    region->EnableMouse(enable);
    RaisePendingError(L, region, "EnableMouse");
    return 0;
}

// ref: FUN_140a6b2f0
int EnableMouseMotion(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:EnableMouseMotion([enable])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    region->SetMouseMotionEnabled(enable);
    RaisePendingError(L, region, "EnableMouseMotion");
    return 0;
}

// ref: FUN_140a6c0c0
int EnableMouseWheel(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:EnableMouseWheel([enable])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    region->EnableMouseWheel(enable);
    RaisePendingError(L, region, "EnableMouseWheel");
    return 0;
}

// ref: FUN_140a6ce90
int GetBottom(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local bottom = self:GetBottom()");
    float bottom = 0.0f;
    bool valid = Layout(region)->GetBottom(&bottom);
    RaisePendingError(L, region, "GetBottom");
    return valid ? PushUIUnits(L, bottom) : 0;
}

// ref: FUN_140a6e870
int GetCenter(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local x, y = self:GetCenter()");
    float x = 0.0f;
    float y = 0.0f;
    bool valid = Layout(region)->GetCenter(&x, &y);
    RaisePendingError(L, region, "GetCenter");
    if (!valid) {
        return 0;
    }
    int32_t results = PushUIUnits(L, x);
    return results + PushUIUnits(L, y);
}

// ref: FUN_140a70290
int GetHeight(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local height = self:GetHeight([ignoreRect])");
    bool ignoreRect = OptionalBool(L, &index);
    float height = Layout(region)->GetHeightActual(ignoreRect);
    RaisePendingError(L, region, "GetHeight");
    return PushUIUnits(L, height);
}

// ref: FUN_140a71070
int GetLeft(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local left = self:GetLeft()");
    float left = 0.0f;
    bool valid = Layout(region)->GetLeft(&left);
    RaisePendingError(L, region, "GetLeft");
    return valid ? PushUIUnits(L, left) : 0;
}

// ref: FUN_140a72a50
int GetRect(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local left, bottom, width, height = self:GetRect()");
    float left = 0.0f;
    float bottom = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool valid = Layout(region)->GetRectLua(&left, &bottom, &width, &height);
    RaisePendingError(L, region, "GetRect");
    if (!valid) {
        return 0;
    }
    int32_t results = PushUIUnits(L, left);
    results += PushUIUnits(L, bottom);
    results += PushUIUnits(L, width);
    return results + PushUIUnits(L, height);
}

// ref: FUN_140a74490
int GetRight(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local right = self:GetRight()");
    float right = 0.0f;
    bool valid = Layout(region)->GetRight(&right);
    RaisePendingError(L, region, "GetRight");
    return valid ? PushUIUnits(L, right) : 0;
}

// ref: FUN_140a75e70
int GetScaledRect(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local left, bottom, width, height = self:GetScaledRect()");
    float left = 0.0f;
    float bottom = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool valid = Layout(region)->GetScaledRect(&left, &bottom, &width, &height);
    RaisePendingError(L, region, "GetScaledRect");
    if (!valid) {
        return 0;
    }
    int32_t results = PushUIUnits(L, left);
    results += PushUIUnits(L, bottom);
    results += PushUIUnits(L, width);
    return results + PushUIUnits(L, height);
}

// ref: FUN_1424cc810
int GetScriptBody(lua_State* L) {
    CScriptRegion* region = GetScriptRegionFromLua(L);
    return region != nullptr ? region->GetScript(L) : 0;
}

// ref: FUN_140a778b0
int GetScript(lua_State* L) {
    int results = GetScriptBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_140a78560
int GetSize(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local width, height = self:GetSize([ignoreRect])");
    bool ignoreRect = OptionalBool(L, &index);
    float width = 0.0f;
    float height = 0.0f;
    Layout(region)->GetSizeLua(ignoreRect, &width, &height);
    RaisePendingError(L, region, "GetSize");
    int32_t results = PushUIUnits(L, width);
    return results + PushUIUnits(L, height);
}

struct SOURCELOCATIONCONTEXT {
    CScriptRegion* m_region = nullptr;
    blz::string m_location;
};

// ref: FUN_140a54f30
int32_t GetSourceLocationBody(lua_State* L, void* context) {
    auto ctx = static_cast<SOURCELOCATIONCONTEXT*>(context);
    int32_t index = 1;
    ctx->m_region = RegionSelf(L, &index, "Usage: local location = self:GetSourceLocation()");
    alignas(blz::string) uint8_t storage[sizeof(blz::string)];
    blz::string* location = ctx->m_region->GetSourceLocationString(reinterpret_cast<blz::string*>(storage));
    blz::string_assign(reinterpret_cast<blz::string_header*>(&ctx->m_location), location->m_data, location->m_size);
    location->~string();
    RaisePendingError(L, ctx->m_region, "GetSourceLocation");
    return PushLString(L, ctx->m_location.m_data, ctx->m_location.m_size);
}

// ref: FUN_140a793e0
int GetSourceLocation(lua_State* L) {
    SOURCELOCATIONCONTEXT context;
    return ProtectedCall(L, GetSourceLocationBody, &context);
}

// ref: FUN_140a7a140
int GetTop(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local top = self:GetTop()");
    float top = 0.0f;
    bool valid = Layout(region)->GetTop(&top);
    RaisePendingError(L, region, "GetTop");
    return valid ? PushUIUnits(L, top) : 0;
}

// ref: FUN_140a7bb20
int GetWidth(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local width = self:GetWidth([ignoreRect])");
    bool ignoreRect = OptionalBool(L, &index);
    float width = Layout(region)->GetWidthActual(ignoreRect);
    RaisePendingError(L, region, "GetWidth");
    return PushUIUnits(L, width);
}

// ref: FUN_140a7c900
int HasScript(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local hasScript = self:HasScript(scriptName)";
    auto region = RegionSelf(L, &index, usage);
    const char* name = RequiredString(L, &index, usage);
    bool has = region->HasScript(name);
    RaisePendingError(L, region, "HasScript");
    return PushBool(L, has);
}

// ref: FUN_140a7d780
int Hide(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:Hide()");
    region->Hide();
    RaisePendingError(L, region, "Hide");
    return 0;
}

// ref: FUN_1424cc890
int HookScriptBody(lua_State* L) {
    CScriptRegion* region = GetScriptRegionFromLua(L);
    return region != nullptr ? region->HookScript(L) : 0;
}

// ref: FUN_140a7e510
int HookScript(lua_State* L) {
    int results = HookScriptBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_140a7f1c0
int IsAnchoringRestricted(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isRestricted = self:IsAnchoringRestricted()");
    bool restricted = Layout(region)->IsAnchorRestricted();
    RaisePendingError(L, region, "IsAnchoringRestricted");
    return PushBool(L, restricted);
}

// ref: FUN_140a7ff60
int IsDragging(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isDragging = self:IsDragging()");
    bool dragging = region->IsDragging();
    RaisePendingError(L, region, "IsDragging");
    return PushBool(L, dragging);
}

// ref: FUN_140a80d00
int IsMouseClickEnabled(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local enabled = self:IsMouseClickEnabled()");
    bool enabled = region->IsMouseClickEnabled();
    RaisePendingError(L, region, "IsMouseClickEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_140a81aa0
int IsMouseEnabled(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local enabled = self:IsMouseEnabled()");
    bool enabled = region->IsMouseEnabled();
    RaisePendingError(L, region, "IsMouseEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_140a82840
int IsMouseMotionEnabled(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local enabled = self:IsMouseMotionEnabled()");
    bool enabled = region->IsMouseMotionEnabled();
    RaisePendingError(L, region, "IsMouseMotionEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_140a835e0
int IsMouseMotionFocus(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isMouseMotionFocus = self:IsMouseMotionFocus()");
    bool focus = region->IsInMotionList();
    RaisePendingError(L, region, "IsMouseMotionFocus");
    return PushBool(L, focus);
}

// ref: FUN_140a84380
int IsMouseOver(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local isMouseOver = self:IsMouseOver([offsetTop, offsetBottom, offsetLeft, offsetRight])";
    auto region = RegionSelf(L, &index, usage);
    bool present;
    float top = OptionalUIUnits(L, &index, usage, &present);
    float bottom = OptionalUIUnits(L, &index, usage, &present);
    float left = OptionalUIUnits(L, &index, usage, &present);
    float right = OptionalUIUnits(L, &index, usage, &present);
    bool over = region->IsMouseOverOffset(top, bottom, left, right);
    RaisePendingError(L, region, "IsMouseOver");
    return PushBool(L, over);
}

// ref: FUN_140a85660
int IsMouseWheelEnabled(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local enabled = self:IsMouseWheelEnabled()");
    bool enabled = region->IsMouseWheelEnabled();
    RaisePendingError(L, region, "IsMouseWheelEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_140a86400
int IsProtected(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isProtected, isProtectedExplicitly = self:IsProtected()");
    bool isProtected = false;
    bool explicitly = false;
    Layout(region)->GetProtectedState(&isProtected, &explicitly);
    RaisePendingError(L, region, "IsProtected");
    int32_t results = PushBool(L, isProtected);
    return results + PushBool(L, explicitly);
}

// ref: FUN_140a87210
int IsRectValid(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isValid = self:IsRectValid()");
    bool valid = (Layout(region)->m_flags & 1) != 0;
    RaisePendingError(L, region, "IsRectValid");
    return PushBool(L, valid);
}

// ref: FUN_140a87fb0
int IsShown(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isShown = self:IsShown()");
    bool shown = (Layout(region)->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) != 0;
    RaisePendingError(L, region, "IsShown");
    return PushBool(L, shown);
}

// ref: FUN_140a88d50
int IsVisible(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isVisible = self:IsVisible()");
    bool visible = (Layout(region)->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE) != 0;
    RaisePendingError(L, region, "IsVisible");
    return PushBool(L, visible);
}

// ref: FUN_140a89af0
int SetMouseClickEnabled(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:SetMouseClickEnabled([enabled])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    region->SetMouseClickEnabled(enable);
    RaisePendingError(L, region, "SetMouseClickEnabled");
    return 0;
}

// ref: FUN_140a8a8c0
int SetMouseMotionEnabled(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:SetMouseMotionEnabled([enabled])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    region->CScriptRegion::SetMouseMotionEnabled(enable);
    RaisePendingError(L, region, "SetMouseMotionEnabled");
    return 0;
}

struct SETPARENTCONTEXT {
    CScriptRegion* m_region = nullptr;
    CScriptRegion::PARENTARG m_parent = {};
};

// ref: FUN_140a55c30
int32_t SetParentBody(lua_State* L, void* context) {
    auto ctx = static_cast<SETPARENTCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetParent([parent])";
    ctx->m_region = RegionSelf(L, &index, usage);
    if (lua_type(L, index) < 1) {
        index++;
    } else if (FrameScript_Object::GetObjectFromLua(nullptr, L, CSimpleFrame::GetObjectType(), index, false, false) == nullptr) {
        luaL_argerror(L, index, usage);
    } else {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else {
            ctx->m_parent.m_frame = static_cast<CSimpleFrame*>(
                FrameScript_Object::GetObjectFromLua(nullptr, L, CSimpleFrame::GetObjectType(), index, false, false));
            index++;
            ctx->m_parent.m_set = true;
        }
    }
    CScriptRegion::PARENTARG parent = {};
    parent.m_set = ctx->m_parent.m_set;
    if (parent.m_set) {
        parent.m_frame = ctx->m_parent.m_frame;
    }
    ctx->m_region->SetParentArg(&parent);
    RaisePendingError(L, ctx->m_region, "SetParent");
    return 0;
}

// ref: FUN_140a8b690
int SetParent(lua_State* L) {
    SETPARENTCONTEXT context;
    return ProtectedCall(L, SetParentBody, &context);
}

struct PASSTHROUGHCONTEXT {
    CScriptRegion* m_region = nullptr;
    uint32_t m_buttons[32] = {};
    uint64_t m_count = 0;
};

// ref: FUN_140a5c860
int32_t SetPassThroughButtonsBody(lua_State* L, void* context) {
    auto ctx = static_cast<PASSTHROUGHCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetPassThroughButtons(buttons)";
    ctx->m_region = RegionSelf(L, &index, usage);
    MouseButtons(L, &index, usage, ctx->m_buttons, 32, &ctx->m_count);
    if (!CanChangeProtected(ctx->m_region)) {
        return 0;
    }
    ctx->m_region->SetPassThroughButtons(ctx->m_buttons, ctx->m_count);
    RaisePendingError(L, ctx->m_region, "SetPassThroughButtons");
    return 0;
}

// ref: FUN_140a8c3c0
int SetPassThroughButtons(lua_State* L) {
    if (!IsActionPermitted(0xe)) {
        return 0;
    }
    PASSTHROUGHCONTEXT context;
    return ProtectedCall(L, SetPassThroughButtonsBody, &context);
}

// ref: FUN_140a8d160
int SetProtected(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:SetProtected()");
    if (!CanChangeProtected(region)) {
        return 0;
    }
    Layout(region)->SetProtected();
    RaisePendingError(L, region, "SetProtected");
    return 0;
}

// ref: FUN_1424cce40
int SetScriptBody(lua_State* L) {
    CScriptRegion* region = GetScriptRegionFromLua(L);
    return region != nullptr ? region->SetScript(L) : 0;
}

// ref: FUN_140a8def0
int SetScript(lua_State* L) {
    int results = SetScriptBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_140a8eba0
int SetShown(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:SetShown([show])");
    bool show = OptionalBool(L, &index);
    region->SetShown(show);
    RaisePendingError(L, region, "SetShown");
    return 0;
}

// ref: FUN_140a8f970
int Show(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:Show()");
    region->Show();
    RaisePendingError(L, region, "Show");
    return 0;
}

// ---- CLayoutFrame ----------------------------------------------------------------------

// ref: FUN_140a5d530
int AdjustPointsOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:AdjustPointsOffset(x, y)";
    auto region = RegionSelf(L, &index, usage);
    float x = RequiredUIUnits(L, &index, usage);
    float y = RequiredUIUnits(L, &index, usage);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    Layout(region)->OffsetPoints(x, y, true);
    RaisePendingError(L, region, "AdjustPointsOffset");
    return 0;
}

// ref: FUN_140a5e4d0
int ClearAllPoints(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:ClearAllPoints()");
    Layout(region)->ClearAllPointsVirtual();
    RaisePendingError(L, region, "ClearAllPoints");
    return 0;
}

// ref: FUN_140a5f260
int ClearPoint(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:ClearPoint(point)";
    auto region = RegionSelf(L, &index, usage);
    uint32_t point = RequiredFramePoint(L, &index, usage);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    Layout(region)->ClearPoint(static_cast<int32_t>(point));
    RaisePendingError(L, region, "ClearPoint");
    return 0;
}

// ref: FUN_140a60030
int ClearPointsOffset(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: self:ClearPointsOffset()");
    if (!CanChangeProtected(region)) {
        return 0;
    }
    Layout(region)->ResetPointOffsets(true);
    RaisePendingError(L, region, "ClearPointsOffset");
    return 0;
}

// ref: FUN_140a60dc0
int GetNumPoints(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local numPoints = self:GetNumPoints()");
    int32_t count = Layout(region)->GetNumPoints();
    RaisePendingError(L, region, "GetNumPoints");
    return PushUInt(L, static_cast<uint32_t>(count));
}

int32_t PushPointInfo(lua_State* L, const CLayoutFrame::POINTINFO* info) {
    int32_t results = PushFramePoint(L, static_cast<uint32_t>(info->m_point));
    results += PushObject(L, info->m_relativeTo);
    results += PushFramePoint(L, static_cast<uint32_t>(info->m_relativePoint));
    results += PushUIUnits(L, info->m_offsetX);
    return results + PushUIUnits(L, info->m_offsetY);
}

// ref: FUN_140a61b90
int GetPoint(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local point, relativeTo, relativePoint, offsetX, offsetY = self:GetPoint([anchorIndex])";
    auto region = RegionSelf(L, &index, usage);
    uint32_t anchor = OptionalIndex(L, &index, usage);
    CLayoutFrame::POINTINFO info = {};
    bool valid = Layout(region)->GetPointInfo(static_cast<int32_t>(anchor), &info);
    RaisePendingError(L, region, "GetPoint");
    return valid ? PushPointInfo(L, &info) : 0;
}

// ref: FUN_140a636f0
int GetPointByName(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local point, relativeTo, relativePoint, offsetX, offsetY = self:GetPointByName(point)";
    auto region = RegionSelf(L, &index, usage);
    uint32_t point = RequiredFramePoint(L, &index, usage);
    CLayoutFrame::POINTINFO info = {};
    bool valid = Layout(region)->GetPointInfoByName(static_cast<int32_t>(point), &info);
    RaisePendingError(L, region, "GetPointByName");
    return valid ? PushPointInfo(L, &info) : 0;
}

// The relative an anchoring call names at `index`: a frame name, a region, or nil for the
// screen. Anything else leaves `relative` as it was and the index unconsumed.
bool ReadRelative(lua_State* L, CScriptRegion* region, int32_t index, CLayoutFrame** relative, const char** error) {
    int32_t type = lua_type(L, index);
    if (type == LUA_TSTRING) {
        *relative = Layout(region)->GetRelativeByName(lua_tolstring(L, index, nullptr), Layout(region));
        return true;
    }
    if (type == LUA_TTABLE) {
        auto object = static_cast<CScriptRegion*>(FrameScript_Object::GetObjectFromLua(error, L, CScriptRegion::GetObjectType(), index, false, false));
        *relative = object != nullptr ? Layout(object) : nullptr;
        return true;
    }
    if (type == LUA_TNIL) {
        *relative = CSimpleTop::s_instance;
        return true;
    }
    return false;
}

// ref: FUN_1424cc910
int SetAllPointsBody(lua_State* L) {
    CScriptRegion* region = GetRegionFromLua(L);
    if (region == nullptr) {
        return 0;
    }
    if (!region->CanQueryRect()) {
        if (CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
            CSimpleTop::s_instance->m_actionBlockedHook(reinterpret_cast<CSimpleFrame*>(region));
        }
        return 0;
    }
    auto relative = static_cast<CLayoutFrame*>(Layout(region)->GetLayoutParent());
    const char* error = nullptr;
    if (lua_isstring(L, 2)) {
        relative = Layout(region)->GetRelativeByName(lua_tolstring(L, 2, nullptr), Layout(region));
    } else {
        ReadRelative(L, region, 2, &relative, &error);
    }
    if (relative == nullptr) {
        if (error == nullptr) {
            FrameScript_ObjectError(region, "SetAllPoints", "Couldn't find region named '%s'", lua_tolstring(L, 2, nullptr));
        } else {
            FrameScript_ObjectError(region, "SetAllPoints", "%s", error);
        }
        return 0;
    }
    CScriptRegion* owner = relative->GetScriptRegion();
    if (owner != nullptr && !owner->IsAccessible()) {
        FrameScript_ObjectError(region, "SetAllPoints", "Cannot set points to forbidden region '%s' from a tainted context",
            GetDisplayName(owner));
        return 0;
    }
    int32_t reason = Layout(region)->SetAllPoints(relative, true);
    ScriptRegion_RaiseRestriction(ACTION_SET_POINT, reason, region, "SetAllPoints", L);
    return 0;
}

// ref: FUN_140a65120
int SetAllPoints(lua_State* L) {
    int results = SetAllPointsBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_140a65dd0
int SetHeight(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetHeight(height)";
    auto region = RegionSelf(L, &index, usage);
    float height = RequiredUIUnits(L, &index, usage);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    Layout(region)->SetHeight(height);
    RaisePendingError(L, region, "SetHeight");
    return 0;
}

// ref: FUN_1424ccae0
int SetPointBody(lua_State* L) {
    CScriptRegion* region = GetRegionFromLua(L);
    if (region == nullptr) {
        return 0;
    }
    if (!lua_isstring(L, 2)) {
        FrameScript_ObjectError(region, "SetPoint", "Usage: (\"point\" [, region or nil] [, \"relativePoint\"] [, offsetX, offsetY]");
        return 0;
    }
    auto relative = static_cast<CLayoutFrame*>(Layout(region)->GetLayoutParent());
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    const char* pointName = lua_tolstring(L, 2, nullptr);
    uint32_t point;
    if (!StringToFramePoint(pointName, &point)) {
        FrameScript_ObjectError(region, "SetPoint", "Invalid region point %s", pointName);
        return 0;
    }
    int32_t index = 3;
    uint32_t relativePoint = point;
    const char* error = nullptr;
    if (ReadRelative(L, region, 3, &relative, &error)) {
        index = 4;
    }
    if (relative == nullptr) {
        if (error == nullptr) {
            FrameScript_ObjectError(region, "SetPoint", "Couldn't find region named '%s'", lua_tolstring(L, 3, nullptr));
        } else {
            FrameScript_ObjectError(region, "SetPoint", "%s", error);
        }
        return 0;
    }
    CScriptRegion* owner = relative->GetScriptRegion();
    if (owner != nullptr && !owner->IsAccessible()) {
        FrameScript_ObjectError(region, "SetPoint", "Cannot set point to forbidden region '%s' from a tainted context", GetDisplayName(owner));
        return 0;
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        const char* relativeName = lua_tolstring(L, index, nullptr);
        if (!StringToFramePoint(relativeName, &relativePoint)) {
            FrameScript_ObjectError(region, "SetPoint", "Unknown region point %s", relativeName);
            return 0;
        }
        index++;
    }
    if (lua_isnumber(L, index) && lua_isnumber(L, index + 1)) {
        offsetX = ToLayoutUnits(lua_tonumber(L, index));
        offsetY = ToLayoutUnits(lua_tonumber(L, index + 1));
    }
    int32_t reason = Layout(region)->SetPoint(static_cast<int32_t>(point), relative, static_cast<int32_t>(relativePoint), offsetX, offsetY, true);
    ScriptRegion_RaiseRestriction(ACTION_SET_POINT, reason, region, "SetPoint", L);
    return 0;
}

// ref: FUN_140a66c80
int SetPoint(lua_State* L) {
    int results = SetPointBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_140a67930
int SetSize(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetSize(x, y)";
    auto region = RegionSelf(L, &index, usage);
    float width = RequiredUIUnits(L, &index, usage);
    float height = RequiredUIUnits(L, &index, usage);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    Layout(region)->SetSize(width, height);
    RaisePendingError(L, region, "SetSize");
    return 0;
}

// ref: FUN_140a688d0
int SetWidth(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetWidth(width)";
    auto region = RegionSelf(L, &index, usage);
    float width = RequiredUIUnits(L, &index, usage);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    Layout(region)->SetWidth(width);
    RaisePendingError(L, region, "SetWidth");
    return 0;
}

// 0x142cf9630
const luaL_Reg s_regionMethods[] = {
    {"CanChangeProtectedState", CanChangeProtectedState},
    {"EnableMouse", EnableMouse},
    {"EnableMouseMotion", EnableMouseMotion},
    {"EnableMouseWheel", EnableMouseWheel},
    {"GetBottom", GetBottom},
    {"GetCenter", GetCenter},
    {"GetHeight", GetHeight},
    {"GetLeft", GetLeft},
    {"GetRect", GetRect},
    {"GetRight", GetRight},
    {"GetScaledRect", GetScaledRect},
    {"GetScript", GetScript},
    {"GetSize", GetSize},
    {"GetSourceLocation", GetSourceLocation},
    {"GetTop", GetTop},
    {"GetWidth", GetWidth},
    {"HasScript", HasScript},
    {"Hide", Hide},
    {"HookScript", HookScript},
    {"IsAnchoringRestricted", IsAnchoringRestricted},
    {"IsDragging", IsDragging},
    {"IsMouseClickEnabled", IsMouseClickEnabled},
    {"IsMouseEnabled", IsMouseEnabled},
    {"IsMouseMotionEnabled", IsMouseMotionEnabled},
    {"IsMouseMotionFocus", IsMouseMotionFocus},
    {"IsMouseOver", IsMouseOver},
    {"IsMouseWheelEnabled", IsMouseWheelEnabled},
    {"IsProtected", IsProtected},
    {"IsRectValid", IsRectValid},
    {"IsShown", IsShown},
    {"IsVisible", IsVisible},
    {"SetMouseClickEnabled", SetMouseClickEnabled},
    {"SetMouseMotionEnabled", SetMouseMotionEnabled},
    {"SetParent", SetParent},
    {"SetPassThroughButtons", SetPassThroughButtons},
    {"SetProtected", SetProtected},
    {"SetScript", SetScript},
    {"SetShown", SetShown},
    {"Show", Show},
};

// 0x142cf98a0
const luaL_Reg s_layoutMethods[] = {
    {"AdjustPointsOffset", AdjustPointsOffset},
    {"ClearAllPoints", ClearAllPoints},
    {"ClearPoint", ClearPoint},
    {"ClearPointsOffset", ClearPointsOffset},
    {"GetNumPoints", GetNumPoints},
    {"GetPoint", GetPoint},
    {"GetPointByName", GetPointByName},
    {"SetAllPoints", SetAllPoints},
    {"SetHeight", SetHeight},
    {"SetPoint", SetPoint},
    {"SetSize", SetSize},
    {"SetWidth", SetWidth},
};

}

// ref: FUN_140ae59e0
void CScriptRegion_FillScriptMethods(lua_State* L) {
    CScriptObject_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_regionMethods, sizeof(s_regionMethods) / sizeof(s_regionMethods[0]));
    FrameScript_Object::FillScriptMethodTable(L, s_layoutMethods, sizeof(s_layoutMethods) / sizeof(s_layoutMethods[0]));
}
