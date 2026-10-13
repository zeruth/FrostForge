#include "ui/CSimpleStatusBar.hpp"

#include <cstring>

#include "base/DataAllocator.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UIScriptArgs.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

int32_t CSimpleStatusBar::s_metaTable = -2;
int32_t CSimpleStatusBar::s_restrictedMetaTable = -2;

namespace {

// The status bars' pool (0x1447aef00).
CDataAllocator s_statusBarHeap(0x318, 5);  // DAT_1447aef00

// The scripts of a status bar.
enum : uint8_t {
    SCRIPT_ONVALUECHANGED = 0x28,
    SCRIPT_ONMINMAXCHANGED = 0x42,
};

// The widest range a bar takes (DAT_14297fff4, DAT_14297fff8).
constexpr float BAR_VALUE_MAX = 1.0e18f;
constexpr float BAR_VALUE_MIN = -1.0e18f;

// The bar texture's coordinates as top, left, bottom, right: the whole texture
// (DAT_1426c8890).
constexpr float BAR_TEXCOORDS_RECT[4] = { 0.0f, 0.0f, 1.0f, 1.0f };  // DAT_1426c8890
// The whole texture turned a quarter, as the four corners (DAT_14261a9d0, DAT_142737610).
constexpr float BAR_TEXCOORDS_ROTATED[8] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };

bool InRange(float value) {
    return BAR_VALUE_MIN <= value && value <= BAR_VALUE_MAX;
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1424c2cf0
CSimpleStatusBar::CSimpleStatusBar(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_barFlags &= 0xffffff80;
    this->m_minValue = 0.0f;
    this->m_maxValue = 0.0f;
    this->m_value = 0.0f;
    this->m_barTexture = nullptr;
}

// ref: FUN_1424c2d60
CSimpleStatusBar::~CSimpleStatusBar() = default;

void* CSimpleStatusBar::operator new(size_t size) {
    (void)size;
    return s_statusBarHeap.GetData(false, ".?AVCSimpleStatusBar@@", -2, 0x1f);
}

void CSimpleStatusBar::operator delete(void* bar) {
    if (bar != nullptr) {
        s_statusBarHeap.PutData(bar);
    }
}

// DAT_14309ff04
int32_t CSimpleStatusBar::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_1424c2df0
void CSimpleStatusBar::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleStatusBar_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleStatusBar_FillScriptMethods, true);
}

// ref: FUN_1424c2e40
void CSimpleStatusBar::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1424c2f00
int32_t CSimpleStatusBar::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424c3de0
bool CSimpleStatusBar::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (index == SCRIPT_ONVALUECHANGED) {
        data->m_wrapper = "return function(self,value) %s end";
        return true;
    }
    if (index == SCRIPT_ONMINMAXCHANGED) {
        data->m_wrapper = "return function(self,min,max) %s end";
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_1424c3100
bool CSimpleStatusBar::IsObjectTypeName(const char* name) {
    return _stricmp(name, "StatusBar") == 0 || _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1424c2fa0
bool CSimpleStatusBar::IsObjectType(int32_t type) {
    if (type == CSimpleStatusBar::GetObjectType() || type == CSimpleFrame::GetObjectType()
        || type == CScriptRegion::GetObjectType()) {
        return true;
    }
    return this->CScriptObject::IsObjectType(type);
}

// ref: FUN_1424c2ef0
const char* CSimpleStatusBar::GetObjectTypeName() {
    return "StatusBar";
}

// ref: FUN_1424c2ee0
uint32_t CSimpleStatusBar::GetObjectSize() {
    return 0x318;
}

// ---- XML ----------------------------------------------------------------------------------------

// ref: FUN_1424c3180
void CSimpleStatusBar::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x1000)) {
        return;
    }
    auto xml = static_cast<XMLRANGE*>(node);
    auto scriptStatus = static_cast<CScriptStatus*>(status);
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_BARCOLOR) {
            uint32_t color = XmlColorPack(static_cast<XMLCOLOR*>(child));
            // The colour's own alpha is not passed on: the alpha argument says "not given".
            uint8_t alpha[2] = { 0, 0 };
            this->SetStatusBarColor(&color, alpha);
        } else if (child->m_type == XMLTYPE_BARTEXTURE) {
            auto region = static_cast<CSimpleTexture*>(FrameXML_CreateTexture(child, this, scriptStatus));
            TEXTURESOURCE source = TEXTURESOURCE::FromTexture(region);
            this->SetStatusBarTexture(&source);
            if (this->m_barTexture != nullptr) {
                this->m_barTexture->SetDrawLayer(static_cast<uint8_t>(xml->m_drawLayer), 0);
            }
        }
    }
    if ((xml->m_rangeFlags & 6) == 6) {
        float minValue = xml->m_minValue;
        float maxValue = xml->m_maxValue;
        if (!InRange(minValue) || !InRange(maxValue)) {
            const char* name = this->GetName();
            if (name == nullptr) {
                name = this->GetObjectTypeName();
            }
            status->Add(2, "Frame %s: Min or Max out of range", name);
        } else if (BAR_VALUE_MAX < maxValue - minValue) {
            status->Add(2, "Frame %s: Min and Max too far apart", GetDisplayName(this));
        } else {
            this->SetMinMaxValues(minValue, maxValue);
        }
        if (xml->m_rangeFlags & 8) {
            this->SetValue(xml->m_defaultValue);
        }
    }
    if (xml->m_rangeFlags & 1) {
        this->SetOrientation(static_cast<int32_t>(xml->m_orientation));
    }
    if (xml->m_rangeFlags & 0x40) {
        this->SetRotatesTexture(((xml->m_rangeFlags >> 9) & 1) != 0);
    }
    if ((xml->m_rangeFlags & 0x80) && this->GetFillStyle() != FILLSTYLE_REVERSE) {
        this->m_barFlags |= BAR_FILL_STYLE | BAR_DIRTY;
    }
    if (xml->m_rangeFlags & 0x100) {
        this->SetFillStyle(xml->m_fillStyle);
    }
}

// ---- layout ------------------------------------------------------------------------------------

// ref: FUN_1424c3e10
void CSimpleStatusBar::RemoveRegion(CSimpleRegion* region) {
    if (region == this->m_barTexture) {
        this->m_barTexture = nullptr;
    }
    this->CSimpleFrame::RemoveRegion(region);
}

// ref: FUN_1424c3500
void CSimpleStatusBar::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    this->CSimpleFrame::OnFrameSizeChanged(rect, previous);
    this->m_barFlags |= BAR_DIRTY;
}

// Places the bar texture over the filled part of the frame, cropping (or, turned, rotating)
// its coordinates to match.
// ref: FUN_1424c3520
void CSimpleStatusBar::RunOnUpdate(float elapsed) {
    this->CSimpleFrame::RunOnUpdate(elapsed);
    if (!(this->m_barFlags & BAR_DIRTY) || this->m_barTexture == nullptr
        || (this->m_barFlags & (BAR_RANGE_SET | BAR_VALUE_SET)) != (BAR_RANGE_SET | BAR_VALUE_SET)) {
        return;
    }
    float fill = this->GetFillAmount();
    float width;
    float height;
    this->GetSize(&width, &height, false);
    if (fill <= 0.0f) {
        this->m_barTexture->Hide();
    } else {
        this->m_barTexture->Show();
    }
    CSimpleTexture* bar = this->m_barTexture;
    // The texture keeps its coordinates when it tiles.
    bool tiles = (bar->m_textureFlags & (CSimpleTexture::TEXTURE_HORIZ_TILE | CSimpleTexture::TEXTURE_VERT_TILE)) != 0;
    bool rotates = (this->m_barFlags & BAR_ROTATES_TEXTURE) != 0;
    uint8_t style = this->GetFillStyle();
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    if (!(this->m_barFlags & BAR_VERTICAL)) {
        if (!tiles) {
            if (!rotates) {
                const float rect[4] = { 0.0f, 0.0f, 1.0f, fill };
                bar->SetTexCoordsRect(rect);
            } else {
                const float corners[8] = { 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f - fill, 1.0f, 1.0f - fill };
                bar->SetTexCoords(corners);
            }
        }
        if (style == FILLSTYLE_STANDARD || style == FILLSTYLE_STANDARD_NO_RANGE_FILL) {
            right = (fill - 1.0f) * width;
        } else if (style == FILLSTYLE_CENTER) {
            left = (1.0f - fill) * width * 0.5f;
            right = (fill - 1.0f) * width * 0.5f;
        } else if (style == FILLSTYLE_REVERSE) {
            left = (1.0f - fill) * width;
        }
    } else {
        if (!tiles) {
            if (!rotates) {
                const float rect[4] = { 1.0f - fill, 0.0f, 1.0f, 1.0f };
                bar->SetTexCoordsRect(rect);
            } else {
                const float corners[8] = { fill, 0.0f, 0.0f, 0.0f, fill, 1.0f, 0.0f, 1.0f };
                bar->SetTexCoords(corners);
            }
        }
        if (style == FILLSTYLE_STANDARD || style == FILLSTYLE_STANDARD_NO_RANGE_FILL) {
            top = (fill - 1.0f) * height;
        } else if (style == FILLSTYLE_CENTER) {
            top = (fill - 1.0f) * height * 0.5f;
            bottom = (1.0f - fill) * height * 0.5f;
        } else if (style == FILLSTYLE_REVERSE) {
            bottom = (1.0f - fill) * height;
        }
    }
    bar->SetPoint(FRAMEPOINT_TOPLEFT, this, FRAMEPOINT_TOPLEFT, left, top, true);
    bar->SetPoint(FRAMEPOINT_TOPRIGHT, this, FRAMEPOINT_TOPRIGHT, right, top, true);
    bar->SetPoint(FRAMEPOINT_BOTTOMLEFT, this, FRAMEPOINT_BOTTOMLEFT, left, bottom, true);
    bar->SetPoint(FRAMEPOINT_BOTTOMRIGHT, this, FRAMEPOINT_BOTTOMRIGHT, right, bottom, true);
    this->m_barFlags &= ~BAR_DIRTY;
}

// The bar is placed in the frame's update (slot 0x2a8, FUN_14026bea0).
bool CSimpleStatusBar::WantsUpdate() {
    return true;
}

// ---- the value ---------------------------------------------------------------------------------

// ref: FUN_1424c2e90
float CSimpleStatusBar::GetFillAmount() {
    float range = this->m_maxValue - this->m_minValue;
    if (0.0f < range) {
        return (this->m_value - this->m_minValue) / range;
    }
    if (range == 0.0f && this->GetFillStyle() == FILLSTYLE_STANDARD_NO_RANGE_FILL) {
        return 1.0f;
    }
    return 0.0f;
}

// ref: FUN_1424c3bd0
void CSimpleStatusBar::SetMinMaxValues(float minValue, float maxValue) {
    if (maxValue <= minValue) {
        minValue = maxValue;
    }
    if (!InRange(minValue) || !InRange(maxValue)) {
        FrameScript_ActionError("Min or Max out of range");
        return;
    }
    if (BAR_VALUE_MAX < maxValue - minValue) {
        FrameScript_ActionError("Min and Max too far apart");
        return;
    }
    if ((this->m_barFlags & BAR_RANGE_SET) && minValue == this->m_minValue && maxValue == this->m_maxValue) {
        return;
    }
    this->m_minValue = minValue;
    this->m_barFlags |= BAR_DIRTY | BAR_RANGE_SET;
    this->m_maxValue = maxValue;
    // FUN_140612970
    this->RunScript(SCRIPT_ONMINMAXCHANGED, nullptr, minValue, maxValue);
    if (this->m_barFlags & BAR_VALUE_SET) {
        this->SetValue(this->m_value);
    }
}

// ref: FUN_1424c3d90
void CSimpleStatusBar::SetValue(float value) {
    uint32_t flags = this->m_barFlags;
    if (!(flags & BAR_RANGE_SET)) {
        return;
    }
    float clamped = this->m_maxValue;
    if (value <= this->m_maxValue) {
        clamped = this->m_minValue <= value ? value : this->m_minValue;
    }
    if ((flags & BAR_VALUE_SET) && clamped == this->m_value) {
        return;
    }
    this->m_value = clamped;
    this->m_barFlags = flags | BAR_DIRTY | BAR_VALUE_SET;
    // FUN_140603740
    this->RunScript(SCRIPT_ONVALUECHANGED, nullptr, clamped);
}

// ref: FUN_1424c3b90
void CSimpleStatusBar::SetFillStyle(uint8_t style) {
    if (style != this->GetFillStyle()) {
        this->m_barFlags = (this->m_barFlags & ~BAR_FILL_STYLE) | ((static_cast<uint32_t>(style) << 4) & BAR_FILL_STYLE) | BAR_DIRTY;
    }
}

// ref: FUN_1424c3cb0
void CSimpleStatusBar::SetOrientation(int32_t orientation) {
    this->m_barFlags = (this->m_barFlags & ~BAR_VERTICAL) | ((static_cast<uint32_t>(orientation) << 6) & BAR_VERTICAL) | BAR_DIRTY;
}

// ref: FUN_1424c3cd0
void CSimpleStatusBar::SetRotatesTexture(bool rotates) {
    CSimpleTexture* bar = this->m_barTexture;
    if (bar == nullptr) {
        return;
    }
    if (!rotates) {
        bar->SetTexCoordsRect(BAR_TEXCOORDS_RECT);
    } else {
        bar->SetTexCoords(BAR_TEXCOORDS_ROTATED);
    }
    this->m_barFlags = (this->m_barFlags & ~BAR_ROTATES_TEXTURE) | (rotates ? BAR_ROTATES_TEXTURE : 0) | BAR_DIRTY;
}

// ---- the bar texture ---------------------------------------------------------------------------

// ref: FUN_1424c38e0
bool CSimpleStatusBar::SetStatusBarTexture(const TEXTURESOURCE* source) {
    CSimpleTexture* adopted = source->GetTexture();
    if (adopted != nullptr && adopted == this->m_barTexture) {
        return true;
    }
    if (this->m_barTexture != nullptr) {
        this->m_barTexture->Hide();
    }
    this->m_barFlags |= BAR_DIRTY;
    // A texture reused for a new file keeps its tiling.
    bool reused = false;
    if (adopted != nullptr) {
        this->m_barTexture = adopted;
    } else if (this->m_barTexture != nullptr) {
        reused = true;
    } else {
        this->m_barTexture = new CSimpleTexture(this, DRAWLAYER_ARTWORK, 0, true);
        if (this->m_barTexture == nullptr) {
            return true;
        }
    }
    CSimpleTexture* bar = this->m_barTexture;
    bar->SetFrame(this, DRAWLAYER_ARTWORK, 0, true);
    bar->SetAllPoints(this, true);
    if (adopted != nullptr) {
        return true;
    }
    if (this->m_barTexture == nullptr) {
        return false;
    }
    int32_t type = source->m_type;
    if (type == TEXTURESOURCE::SOURCE_FILE || type == TEXTURESOURCE::SOURCE_FILE_ID) {
        uint32_t wrapH = 0;
        uint32_t wrapV = 0;
        if (reused) {
            wrapH = (bar->m_textureFlags & CSimpleTexture::TEXTURE_HORIZ_TILE) ? 1 : 0;
            wrapV = (bar->m_textureFlags & CSimpleTexture::TEXTURE_VERT_TILE) ? 1 : 0;
        }
        return bar->SetTextureById(source->GetFileDataId(), wrapH, wrapV, 1, true, false);
    }
    if (type == TEXTURESOURCE::SOURCE_ATLAS) {
        int32_t member = source->m_id;
        return bar->SetAtlas(&member, 1, false, 0, 0);
    }
    return false;
}

// ref: FUN_1424c3af0
bool CSimpleStatusBar::SetColorFill(const uint8_t* rgb, const uint8_t* alpha) {
    if (this->m_barTexture == nullptr) {
        TEXTURESOURCE none = {};
        this->SetStatusBarTexture(&none);
    }
    if (this->m_barTexture == nullptr) {
        return false;
    }
    uint8_t bytes[4] = { rgb[0], rgb[1], rgb[2], alpha[1] != 0 ? alpha[0] : static_cast<uint8_t>(0xff) };
    uint32_t argb;
    std::memcpy(&argb, bytes, sizeof(argb));
    return this->m_barTexture->SetColorTexture(&argb);
}

// ref: FUN_1424c3d50
void CSimpleStatusBar::SetStatusBarColor(const uint32_t* rgb, const uint8_t* alpha) {
    if (this->m_barTexture != nullptr) {
        this->m_barTexture->SetVertexColor(*rgb, alpha);
    }
}

// ref: FUN_1424c2f30
uint32_t CSimpleStatusBar::GetStatusBarColor() {
    if (this->m_barTexture == nullptr) {
        return 0xffffffff;
    }
    uint32_t argb = 0;
    this->m_barTexture->GetVertexColor(&argb);
    return argb;
}

// ref: FUN_1424c3d70
void CSimpleStatusBar::SetStatusBarDesaturation(float desaturation) {
    if (this->m_barTexture != nullptr) {
        this->m_barTexture->SetDesaturation(desaturation);
    }
}

// ref: FUN_1424c2f80
float CSimpleStatusBar::GetStatusBarDesaturation() {
    if (this->m_barTexture == nullptr) {
        return 0.0f;
    }
    return this->m_barTexture->m_desaturation;
}

// ---- script methods (table 0x142cf9b20) ------------------------------------------------------

namespace {

using namespace ScriptMethod;

CSimpleStatusBar* BarSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleStatusBar*>(Self(L, CSimpleStatusBar::GetObjectType(), index, usage));
}

// ref: FUN_140ab5680
int GetFillStyle(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local fillStyle = self:GetFillStyle()");
    uint8_t style = bar->GetFillStyle();
    RaisePendingError(L, bar, "GetFillStyle");
    return PushStringOrNil(L, FillStyleToString(style));
}

// ref: FUN_140ab6460
int GetMinMaxValues(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local minValue, maxValue = self:GetMinMaxValues()");
    float minValue = bar->m_minValue;
    float maxValue = bar->m_maxValue;
    RaisePendingError(L, bar, "GetMinMaxValues");
    lua_pushnumber(L, static_cast<double>(minValue));
    lua_pushnumber(L, static_cast<double>(maxValue));
    return 2;
}

// ref: FUN_140ab7270
int GetOrientation(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local orientation = self:GetOrientation()");
    uint32_t orientation = (bar->m_barFlags >> 6) & 1;
    RaisePendingError(L, bar, "GetOrientation");
    // FUN_140a54e90
    return PushStringOrNil(L, OrientationToString(orientation));
}

// ref: FUN_140ab7ff0
int GetReverseFill(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local isReverseFill = self:GetReverseFill()");
    bool reverse = bar->GetFillStyle() == CSimpleStatusBar::FILLSTYLE_REVERSE;
    RaisePendingError(L, bar, "GetReverseFill");
    return PushBool(L, reverse);
}

// ref: FUN_140ab8d80
int GetRotatesTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local rotatesTexture = self:GetRotatesTexture()");
    bool rotates = (bar->m_barFlags & CSimpleStatusBar::BAR_ROTATES_TEXTURE) != 0;
    RaisePendingError(L, bar, "GetRotatesTexture");
    return PushBool(L, rotates);
}

// ref: FUN_140ab9b00
int GetStatusBarColor(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local r, g, b, a = self:GetStatusBarColor()");
    uint32_t argb = bar->GetStatusBarColor();
    RaisePendingError(L, bar, "GetStatusBarColor");
    if (!lua_checkstack(L, 4)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    return PushColor(L, argb);
}

// ref: FUN_140aba8b0
int GetStatusBarDesaturation(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local desaturation = self:GetStatusBarDesaturation()");
    float desaturation = bar->GetStatusBarDesaturation();
    RaisePendingError(L, bar, "GetStatusBarDesaturation");
    return PushFloat(L, desaturation);
}

// ref: FUN_140abb630
int GetStatusBarTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local texture = self:GetStatusBarTexture()");
    CSimpleTexture* texture = bar->m_barTexture;
    RaisePendingError(L, bar, "GetStatusBarTexture");
    return PushObject(L, texture);
}

// ref: FUN_140abc3b0
int GetValue(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local value = self:GetValue()");
    float value = bar->m_value;
    RaisePendingError(L, bar, "GetValue");
    return PushFloat(L, value);
}

// ref: FUN_140abd130
int IsStatusBarDesaturated(lua_State* L) {
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, "Usage: local desaturated = self:IsStatusBarDesaturated()");
    bool desaturated = 0.0f < bar->GetStatusBarDesaturation();
    RaisePendingError(L, bar, "IsStatusBarDesaturated");
    return PushBool(L, desaturated);
}

struct COLORARGS {
    CSimpleStatusBar* m_bar;        // 0x00
    uint32_t m_rgb;                 // 0x08
    uint8_t m_alpha[2];             // 0x0c value, present
};

// ref: FUN_140a58490
int32_t SetColorFillBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetColorFill(color [, a])";
    auto args = static_cast<COLORARGS*>(context);
    int32_t index = 1;
    args->m_bar = BarSelf(L, &index, usage);
    // FUN_140a548f0
    args->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, args->m_alpha);
    args->m_bar->SetColorFill(reinterpret_cast<const uint8_t*>(&args->m_rgb), args->m_alpha);
    RaisePendingError(L, args->m_bar, "SetColorFill");
    return 0;
}

// ref: FUN_140abdec0
int SetColorFill(lua_State* L) {
    COLORARGS args = {};
    return ProtectedCall(L, SetColorFillBody, &args);
}

// ref: FUN_140abebf0
int SetFillStyle(lua_State* L) {
    const char* usage = "Usage: self:SetFillStyle(fillStyle)";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    uint8_t style = 0;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (lua_type(L, index) != LUA_TSTRING || !StringToFillStyle(lua_tolstring(L, index, nullptr), &style)) {
        luaL_argerror(L, index, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    }
    bar->SetFillStyle(style);
    RaisePendingError(L, bar, "SetFillStyle");
    return 0;
}

// ref: FUN_140abfa80
int SetMinMaxValues(lua_State* L) {
    const char* usage = "Usage: self:SetMinMaxValues(minValue, maxValue)";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    // FUN_140a542f0
    float minValue = RequiredFinite(L, &index, usage);
    float maxValue = RequiredFinite(L, &index, usage);
    bar->SetMinMaxValues(minValue, maxValue);
    RaisePendingError(L, bar, "SetMinMaxValues");
    return 0;
}

// ref: FUN_140ac0860
int SetOrientation(lua_State* L) {
    const char* usage = "Usage: self:SetOrientation(orientation)";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    // FUN_140a54cf0
    uint32_t orientation = 0;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (lua_type(L, index) != LUA_TSTRING || !StringToOrientation(lua_tolstring(L, index, nullptr), &orientation)) {
        luaL_argerror(L, index, usage);
    } else {
        index++;
    }
    bar->SetOrientation(static_cast<int32_t>(orientation));
    RaisePendingError(L, bar, "SetOrientation");
    return 0;
}

// ref: FUN_140ac1610
int SetReverseFill(lua_State* L) {
    const char* usage = "Usage: self:SetReverseFill(isReverseFill)";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    bool reverse = RequiredBool(L, &index, usage);
    bar->SetFillStyle(reverse ? CSimpleStatusBar::FILLSTYLE_REVERSE : CSimpleStatusBar::FILLSTYLE_STANDARD);
    RaisePendingError(L, bar, "SetReverseFill");
    return 0;
}

// ref: FUN_140ac23e0
int SetRotatesTexture(lua_State* L) {
    const char* usage = "Usage: self:SetRotatesTexture(rotatesTexture)";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    bool rotates = RequiredBool(L, &index, usage);
    bar->SetRotatesTexture(rotates);
    RaisePendingError(L, bar, "SetRotatesTexture");
    return 0;
}

// ref: FUN_140a59290
int32_t SetStatusBarColorBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetStatusBarColor(color [, a])";
    auto args = static_cast<COLORARGS*>(context);
    int32_t index = 1;
    args->m_bar = BarSelf(L, &index, usage);
    args->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, args->m_alpha);
    args->m_bar->SetStatusBarColor(&args->m_rgb, args->m_alpha);
    RaisePendingError(L, args->m_bar, "SetStatusBarColor");
    return 0;
}

// ref: FUN_140ac31a0
int SetStatusBarColor(lua_State* L) {
    COLORARGS args = {};
    return ProtectedCall(L, SetStatusBarColorBody, &args);
}

// ref: FUN_140ac3ed0
int SetStatusBarDesaturated(lua_State* L) {
    const char* usage = "Usage: self:SetStatusBarDesaturated([desaturated])";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    float desaturation = 0.0f;
    if (lua_type(L, index) >= 1) {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else if (lua_toboolean(L, index) != 0) {
            desaturation = 1.0f;
        }
    }
    bar->SetStatusBarDesaturation(desaturation);
    RaisePendingError(L, bar, "SetStatusBarDesaturated");
    return 0;
}

// ref: FUN_140ac4c90
int SetStatusBarDesaturation(lua_State* L) {
    const char* usage = "Usage: self:SetStatusBarDesaturation(desaturation)";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    float desaturation = RequiredUnit(L, &index, usage);
    bar->SetStatusBarDesaturation(desaturation);
    RaisePendingError(L, bar, "SetStatusBarDesaturation");
    return 0;
}

struct ASSETARGS {
    CSimpleStatusBar* m_bar;
    TEXTURESOURCE m_source;
};

// ref: FUN_140a5a090
int32_t SetStatusBarTextureBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetStatusBarTexture(asset)";
    auto args = static_cast<ASSETARGS*>(context);
    int32_t index = 1;
    args->m_bar = BarSelf(L, &index, usage);
    args->m_source = TextureSourceArgument(L, &index, usage);
    args->m_bar->SetStatusBarTexture(&args->m_source);
    RaisePendingError(L, args->m_bar, "SetStatusBarTexture");
    return 0;
}

// ref: FUN_140ac5a40
int SetStatusBarTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetStatusBarTextureBody, &args);
}

// ref: FUN_140ac6760
int SetValue(lua_State* L) {
    const char* usage = "Usage: self:SetValue(value)";
    int32_t index = 1;
    CSimpleStatusBar* bar = BarSelf(L, &index, usage);
    float value = RequiredFinite(L, &index, usage);
    bar->SetValue(value);
    RaisePendingError(L, bar, "SetValue");
    return 0;
}

// DAT_142cf9b20
const luaL_Reg s_statusBarMethods[] = {
    { "GetFillStyle", GetFillStyle },
    { "GetMinMaxValues", GetMinMaxValues },
    { "GetOrientation", GetOrientation },
    { "GetReverseFill", GetReverseFill },
    { "GetRotatesTexture", GetRotatesTexture },
    { "GetStatusBarColor", GetStatusBarColor },
    { "GetStatusBarDesaturation", GetStatusBarDesaturation },
    { "GetStatusBarTexture", GetStatusBarTexture },
    { "GetValue", GetValue },
    { "IsStatusBarDesaturated", IsStatusBarDesaturated },
    { "SetColorFill", SetColorFill },
    { "SetFillStyle", SetFillStyle },
    { "SetMinMaxValues", SetMinMaxValues },
    { "SetOrientation", SetOrientation },
    { "SetReverseFill", SetReverseFill },
    { "SetRotatesTexture", SetRotatesTexture },
    { "SetStatusBarColor", SetStatusBarColor },
    { "SetStatusBarDesaturated", SetStatusBarDesaturated },
    { "SetStatusBarDesaturation", SetStatusBarDesaturation },
    { "SetStatusBarTexture", SetStatusBarTexture },
    { "SetValue", SetValue },
};
static_assert(sizeof(s_statusBarMethods) / sizeof(s_statusBarMethods[0]) == 0x15, "the status bar table has 0x15 methods");

}

// ref: FUN_140ae5980
void CSimpleStatusBar_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_statusBarMethods, 0x15);
}
