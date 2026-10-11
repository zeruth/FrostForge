#include "ui/CSimpleSlider.hpp"

#include <cmath>
#include <cstring>

#include "base/DataAllocator.hpp"
#include "base/Status.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

uint32_t OsGetAsyncTimeMs();

int32_t CSimpleSlider::s_metaTable = -2;
int32_t CSimpleSlider::s_restrictedMetaTable = -2;

namespace {

// The sliders' pool (0x1447aeec0, made by the static initializer 0x14020dd10).
CDataAllocator s_sliderHeap(0x320, 5);

// The frame flag a disabled slider carries.
constexpr uint32_t FRAME_DISABLED = 0x8;

// The scripts of a slider.
enum : uint8_t {
    SCRIPT_ONENABLE = 0x21,
    SCRIPT_ONDISABLE = 0x22,
    SCRIPT_ONVALUECHANGED = 0x28,
    SCRIPT_ONMINMAXCHANGED = 0x42,
};

// The smallest change that counts (DAT_14261a954, 2^-22).
constexpr float SLIDER_EPSILON = 2.3841858e-07f;
// The smallest step (DAT_1426c6f7c, 2^-23) and the XML default (DAT_142618f58).
constexpr float SLIDER_MIN_STEP = 1.1920929e-07f;
constexpr float SLIDER_DEFAULT_STEP = 0.001f;

// At most `m_max` events per `m_interval` milliseconds (FUN_1406d1880, FUN_141653a80).
struct EVENTTHROTTLE {
    uint32_t m_max;
    uint32_t m_interval;
    uint32_t m_count;
    uint32_t m_last;

    bool Allow() {
        uint32_t now = OsGetAsyncTimeMs();
        if (this->m_max <= this->m_count && this->m_interval <= now - this->m_last) {
            this->m_last = now;
            this->m_count = 0;
        }
        this->m_count++;
        return this->m_count <= this->m_max;
    }
};

// Dragging re-reads the mouse at most once per 10 ms (DAT_1447aeee8).
EVENTTHROTTLE s_dragThrottle = { 1, 10, 1, 0 };

// A new thumb's image from a source that is not a texture object.
// ref: FUN_1425b3600
bool ApplyTextureSource(CSimpleTexture* texture, const TEXTURESOURCE* source) {
    int32_t type = source->m_type;
    if (type == TEXTURESOURCE::SOURCE_FILE || type == TEXTURESOURCE::SOURCE_FILE_ID) {
        return texture->SetTextureById(source->GetFileDataId(), 0, 0, 1, true, false);
    }
    if (type == TEXTURESOURCE::SOURCE_ATLAS) {
        int32_t member = source->m_id;
        return texture->SetAtlas(&member, 1, false, 0, 0);
    }
    return false;
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1424c1b40
CSimpleSlider::CSimpleSlider(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_minValue = 0.0f;
    this->m_range = 0.0f;
    this->m_value = 0.0f;
    this->m_valueStep = 0.0f;
    this->m_sliderFlags = 0x100;
    this->m_thumbTexture = nullptr;
    this->SetMouseMotionEnabled(true);
    this->SetMouseClickEnabled(true);
}

// ref: FUN_1424c1be0
CSimpleSlider::~CSimpleSlider() = default;

void* CSimpleSlider::operator new(size_t size) {
    (void)size;
    return s_sliderHeap.GetData(false, ".?AVCSimpleSlider@@", -2, 0x1f);
}

void CSimpleSlider::operator delete(void* slider) {
    if (slider != nullptr) {
        s_sliderHeap.PutData(slider);
    }
}

// DAT_14309fefc
int32_t CSimpleSlider::GetObjectType() {
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_1424c1c50
void CSimpleSlider::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleSlider_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleSlider_FillScriptMethods, true);
}

// ref: FUN_1424c1ca0
void CSimpleSlider::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1424c1d00
int32_t CSimpleSlider::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424c2ca0
bool CSimpleSlider::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (index == SCRIPT_ONVALUECHANGED) {
        data->m_wrapper = "return function(self,value,userInput) %s end";
        return true;
    }
    if (index == SCRIPT_ONMINMAXCHANGED) {
        data->m_wrapper = "return function(self,min,max) %s end";
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_1424c1fa0
bool CSimpleSlider::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Slider") == 0 || _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1424c1e40
bool CSimpleSlider::IsObjectType(int32_t type) {
    if (type == CSimpleSlider::GetObjectType() || type == CSimpleFrame::GetObjectType()
        || type == CScriptRegion::GetObjectType()) {
        return true;
    }
    return this->CScriptObject::IsObjectType(type);
}

// ref: FUN_1424c1cf0
const char* CSimpleSlider::GetObjectTypeName() {
    return "Slider";
}

// ref: FUN_1424c0e20
uint32_t CSimpleSlider::GetObjectSize() {
    return 0x320;
}

// ---- XML ----------------------------------------------------------------------------------------

// ref: FUN_1424c2040
void CSimpleSlider::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x20000)) {
        return;
    }
    auto xml = static_cast<XMLSLIDER*>(node);
    auto scriptStatus = static_cast<CScriptStatus*>(status);
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_THUMBTEXTURE) {
            uint32_t drawLayer = xml->m_drawLayer;
            auto region = static_cast<CSimpleTexture*>(FrameXML_CreateTexture(child, this, scriptStatus));
            TEXTURESOURCE source = TEXTURESOURCE::FromTexture(region);
            this->SetThumbTexture(&source, drawLayer);
        }
    }
    this->SetValueStep((xml->m_rangeFlags & 0x10) ? xml->m_valueStep : SLIDER_DEFAULT_STEP);
    this->m_sliderFlags = (this->m_sliderFlags & ~SLIDER_STEPS_PER_PAGE)
        | static_cast<uint8_t>((xml->m_rangeFlags & 0x20) ? xml->m_stepsPerPage : 0);
    if ((xml->m_rangeFlags & 6) == 6) {
        this->SetMinMaxValues(xml->m_minValue, xml->m_maxValue);
        if (xml->m_rangeFlags & 8) {
            this->SetValue(xml->m_defaultValue, false);
        }
    }
    if (xml->m_rangeFlags & 1) {
        this->SetOrientation(static_cast<int32_t>(xml->m_orientation));
    }
    if (xml->m_sliderFlags & 1) {
        this->m_sliderFlags = (this->m_sliderFlags & ~SLIDER_OBEY_STEP_ON_DRAG) | (xml->m_obeyStepOnDrag ? SLIDER_OBEY_STEP_ON_DRAG : 0);
    }
}

// ---- input --------------------------------------------------------------------------------------

// ref: FUN_1424c2530
bool CSimpleSlider::OnMouseMove(MOUSEEVENT* event) {
    if (s_dragThrottle.Allow()) {
        this->UpdateFromMouse(event);
    }
    return this->CSimpleFrame::OnMouseMove(event);
}

// ref: FUN_1424c23c0
bool CSimpleSlider::OnMouseDown(MOUSEEVENT* event, const char* button) {
    if (CSimpleTop::s_instance->m_inputSuspended != 0) {
        return false;
    }
    if (!(this->m_frameFlags & FRAME_DISABLED)) {
        int8_t stepsPerPage = static_cast<int8_t>(this->m_sliderFlags & SLIDER_STEPS_PER_PAGE);
        float point[2] = { event->m_x, event->m_y };
        if (stepsPerPage > 0 && this->m_thumbTexture != nullptr && !this->m_thumbTexture->IsPointInRect(point)) {
            CRect thumb = {};
            this->m_thumbTexture->GetRect(&thumb);
            int32_t direction;
            if ((this->m_sliderFlags & SLIDER_ORIENTATION) == 0) {
                direction = thumb.maxX < event->m_x ? 1 : -1;
            } else {
                direction = thumb.maxY > event->m_y ? 1 : -1;
            }
            this->SetValue(static_cast<float>(direction * stepsPerPage) * this->m_valueStep + this->m_value, false);
        } else {
            this->m_sliderFlags |= SLIDER_DRAGGING;
            this->UpdateFromMouse(event);
        }
    }
    return this->CSimpleFrame::OnMouseDown(event, button);
}

// ref: FUN_1424c2500
bool CSimpleSlider::OnMouseUp(MOUSEEVENT* event, const char* button) {
    if (CSimpleTop::s_instance->m_inputSuspended != 0) {
        return false;
    }
    this->m_sliderFlags &= ~SLIDER_DRAGGING;
    return this->CSimpleFrame::OnMouseUp(event, button);
}

// ref: FUN_1424c23b0
void CSimpleSlider::QueueOnHide() {
    this->m_sliderFlags &= ~SLIDER_DRAGGING;
    this->CSimpleFrame::QueueOnHide();
}

// ref: FUN_1424c2cd0
void CSimpleSlider::RemoveRegion(CSimpleRegion* region) {
    if (region == this->m_thumbTexture) {
        this->m_thumbTexture = nullptr;
    }
    this->CSimpleFrame::RemoveRegion(region);
}

// ---- layout ------------------------------------------------------------------------------------

// ref: FUN_1424c25f0
void CSimpleSlider::RunOnUpdate(float elapsed) {
    this->CSimpleFrame::RunOnUpdate(elapsed);
    uint32_t flags = this->m_sliderFlags;
    if (!(flags & SLIDER_THUMB_DIRTY) || this->m_thumbTexture == nullptr || !(flags & SLIDER_RANGE_SET)
        || !(flags & SLIDER_VALUE_SET)) {
        return;
    }
    float fraction = std::fabs(this->m_range) >= SLIDER_EPSILON ? (this->m_value - this->m_minValue) / this->m_range : 0.0f;
    CSimpleTexture* thumb = this->m_thumbTexture;
    int32_t point;
    float x = 0.0f;
    float y = 0.0f;
    if ((flags & SLIDER_ORIENTATION) == 0) {
        float length = (this->m_rect.maxX - this->m_rect.minX) / this->m_layoutScale;
        x = (length - thumb->GetWidth()) * fraction;
        point = FRAMEPOINT_LEFT;
    } else {
        float length = (this->m_rect.maxY - this->m_rect.minY) / this->m_layoutScale;
        y = -((length - thumb->GetHeight()) * fraction);
        point = FRAMEPOINT_TOP;
    }
    thumb->SetPoint(point, this, point, x, y, true);
    this->m_sliderFlags &= ~SLIDER_THUMB_DIRTY;
}

bool CSimpleSlider::WantsUpdate() {
    return true;
}

// ref: FUN_1424c2390
void CSimpleSlider::OnFrameSizeChanged(const CRect* rect, const CRect* previous) {
    this->CSimpleFrame::OnFrameSizeChanged(rect, previous);
    this->m_sliderFlags |= SLIDER_THUMB_DIRTY;
}

// ---- the value ---------------------------------------------------------------------------------

int32_t CSimpleSlider::GetOrientation() const {
    return static_cast<int32_t>(this->m_sliderFlags << 22) >> 30;
}

// ref: FUN_1424c2870
void CSimpleSlider::SetOrientation(int32_t orientation) {
    this->m_sliderFlags = (this->m_sliderFlags & ~SLIDER_ORIENTATION) | ((static_cast<uint32_t>(orientation) << 8) & SLIDER_ORIENTATION);
    if (this->m_thumbTexture != nullptr) {
        this->m_thumbTexture->ClearAllPointsVirtual();
    }
    this->m_sliderFlags |= SLIDER_THUMB_DIRTY;
}

float CSimpleSlider::StepValue(float value, bool snap) const {
    float step = this->m_valueStep;
    if (snap && step != 0.0f && value < this->m_minValue + this->m_range) {
        float offset = value - this->m_minValue;
        int32_t steps = offset > 0.0f ? static_cast<int32_t>((step * 0.5f + offset) / step)
                                      : static_cast<int32_t>((offset - step * 0.5f) / step);
        value = static_cast<float>(steps) * step + this->m_minValue;
    }
    float clamped = this->m_minValue <= value ? value : this->m_minValue;
    float max = this->m_minValue + this->m_range;
    return clamped <= max ? clamped : max;
}

// ref: FUN_1424c27a0
void CSimpleSlider::SetMinMaxValues(float minValue, float maxValue) {
    if (maxValue < minValue) {
        FrameScript_ActionError("max must be >= min");
        return;
    }
    float range = maxValue - minValue;
    if ((this->m_sliderFlags & SLIDER_RANGE_SET) && std::fabs(range - this->m_range) < SLIDER_EPSILON
        && std::fabs(this->m_minValue - minValue) < SLIDER_EPSILON) {
        return;
    }
    this->m_sliderFlags |= SLIDER_RANGE_SET | SLIDER_THUMB_DIRTY;
    this->m_minValue = minValue;
    this->m_range = range;
    // FUN_140612970
    this->RunScript(SCRIPT_ONMINMAXCHANGED, nullptr, minValue, maxValue);
    if (this->m_sliderFlags & SLIDER_VALUE_SET) {
        this->SetValue(this->m_value, false);
    }
}

// ref: FUN_1424c2a90
void CSimpleSlider::SetValue(float value, bool userInput) {
    uint32_t flags = this->m_sliderFlags;
    if (!(flags & SLIDER_RANGE_SET)) {
        return;
    }
    float stepped = this->StepValue(value, !userInput || (flags & SLIDER_OBEY_STEP_ON_DRAG));
    if ((flags & SLIDER_VALUE_SET) && std::fabs(stepped - this->m_value) < SLIDER_EPSILON && !userInput) {
        return;
    }
    this->m_sliderFlags = flags | SLIDER_VALUE_SET | SLIDER_THUMB_DIRTY;
    this->m_value = stepped;
    // FUN_1424c19e0
    this->RunScript(SCRIPT_ONVALUECHANGED, nullptr, stepped, userInput);
}

// ref: FUN_1424c2b90
void CSimpleSlider::SetValueStep(float step) {
    if (step <= SLIDER_MIN_STEP) {
        step = SLIDER_MIN_STEP;
    }
    if (std::fabs(step - this->m_valueStep) < SLIDER_EPSILON) {
        return;
    }
    uint32_t flags = this->m_sliderFlags;
    this->m_valueStep = step;
    if (!(flags & SLIDER_VALUE_SET) || !(flags & SLIDER_RANGE_SET)) {
        return;
    }
    float old = this->m_value;
    float stepped = this->StepValue(old, true);
    if (std::fabs(stepped - old) >= SLIDER_EPSILON) {
        this->m_value = stepped;
        this->m_sliderFlags = flags | SLIDER_VALUE_SET | SLIDER_THUMB_DIRTY;
        this->RunScript(SCRIPT_ONVALUECHANGED, nullptr, stepped, false);
    }
}

// ref: FUN_1424c28c0
bool CSimpleSlider::SetThumbTexture(const TEXTURESOURCE* source, uint32_t drawLayer) {
    CSimpleTexture* adopted = source->GetTexture();
    if (adopted != nullptr && adopted == this->m_thumbTexture) {
        return true;
    }
    if (this->m_thumbTexture != nullptr) {
        this->m_thumbTexture->Hide();
    }
    this->m_sliderFlags |= SLIDER_THUMB_DIRTY;
    CSimpleTexture* thumb = this->m_thumbTexture;
    if (adopted != nullptr) {
        thumb = adopted;
        this->m_thumbTexture = adopted;
    } else if (thumb == nullptr) {
        thumb = new CSimpleTexture(nullptr, DRAWLAYER_ARTWORK, 0, true);
        this->m_thumbTexture = thumb;
        if (thumb == nullptr) {
            return true;
        }
    }
    thumb->SetFrame(this, static_cast<uint8_t>(drawLayer), 0, true);
    this->m_thumbTexture->ClearAllPointsVirtual();
    if (adopted != nullptr) {
        return true;
    }
    if (this->m_thumbTexture == nullptr) {
        return false;
    }
    return ApplyTextureSource(this->m_thumbTexture, source);
}

// ref: FUN_1424c2020
bool CSimpleSlider::IsDraggingThumb() const {
    return (this->m_sliderFlags & SLIDER_DRAGGING) && this->m_thumbTexture != nullptr;
}

// ref: FUN_1424c1d30
void CSimpleSlider::UpdateFromMouse(MOUSEEVENT* event) {
    if (!(this->m_sliderFlags & SLIDER_DRAGGING) || this->m_thumbTexture == nullptr) {
        return;
    }
    CSimpleTexture* thumb = this->m_thumbTexture;
    float length;
    float position;
    if ((this->m_sliderFlags & SLIDER_ORIENTATION) == 0) {
        length = (this->m_rect.maxX - this->m_rect.minX) - thumb->GetWidth();
        position = event->m_x - (thumb->GetWidth() * 0.5f + this->m_rect.minX);
    } else {
        length = (this->m_rect.maxY - this->m_rect.minY) - thumb->GetHeight();
        position = (this->m_rect.maxY - thumb->GetHeight() * 0.5f) - event->m_y;
    }
    this->SetValue((position / length) * this->m_range + this->m_minValue, true);
}

// ---- script methods (table 0x142cf99f0) ------------------------------------------------------

namespace {

using namespace ScriptMethod;

CSimpleSlider* SliderSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleSlider*>(Self(L, CSimpleSlider::GetObjectType(), index, usage));
}

// ref: FUN_14060d7c0
void SetSliderEnabled(CSimpleSlider* slider, bool enabled) {
    slider->SetFrameFlag(FRAME_DISABLED, !enabled);
    if (!(slider->FrameScript_Object::m_flags & FSOBJ_LOADING)) {
        // FUN_140603620
        slider->RunScript(enabled ? SCRIPT_ONENABLE : SCRIPT_ONDISABLE, nullptr);
    }
}

// ref: FUN_140aa5340
int Disable(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: self:Disable()");
    SetSliderEnabled(slider, false);
    RaisePendingError(L, slider, "Disable");
    return 0;
}

// ref: FUN_140aa60d0
int Enable(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: self:Enable()");
    SetSliderEnabled(slider, true);
    RaisePendingError(L, slider, "Enable");
    return 0;
}

// ref: FUN_140aae870
int SetEnabled(lua_State* L) {
    const char* usage = "Usage: self:SetEnabled(enabled)";
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, usage);
    bool enabled = RequiredBool(L, &index, usage);
    SetSliderEnabled(slider, enabled);
    RaisePendingError(L, slider, "SetEnabled");
    return 0;
}

// ref: FUN_140aadaf0
int IsEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local enabled = self:IsEnabled()");
    bool enabled = !(slider->m_frameFlags & FRAME_DISABLED);
    RaisePendingError(L, slider, "IsEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_140aa6e60
int GetMinMaxValues(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local minValue, maxValue = self:GetMinMaxValues()");
    float minValue = slider->m_minValue;
    float range = slider->m_range;
    RaisePendingError(L, slider, "GetMinMaxValues");
    lua_pushnumber(L, static_cast<double>(minValue));
    lua_pushnumber(L, static_cast<double>(minValue + range));
    return 2;
}

// ref: FUN_140aa7c70
int GetObeyStepOnDrag(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local isObeyStepOnDrag = self:GetObeyStepOnDrag()");
    bool obey = (slider->m_sliderFlags & CSimpleSlider::SLIDER_OBEY_STEP_ON_DRAG) != 0;
    RaisePendingError(L, slider, "GetObeyStepOnDrag");
    return PushBool(L, obey);
}

// ref: FUN_140aa89f0
int GetOrientation(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local orientation = self:GetOrientation()");
    int32_t orientation = slider->GetOrientation();
    RaisePendingError(L, slider, "GetOrientation");
    // FUN_140a54e90
    return PushStringOrNil(L, OrientationToString(static_cast<uint32_t>(orientation)));
}

// ref: FUN_140aa9770
int GetStepsPerPage(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local stepsPerPage = self:GetStepsPerPage()");
    int32_t steps = static_cast<int8_t>(slider->m_sliderFlags & CSimpleSlider::SLIDER_STEPS_PER_PAGE);
    RaisePendingError(L, slider, "GetStepsPerPage");
    return PushInt(L, steps);
}

// ref: FUN_140aaa4f0
int GetThumbTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local texture = self:GetThumbTexture()");
    CSimpleTexture* thumb = slider->m_thumbTexture;
    RaisePendingError(L, slider, "GetThumbTexture");
    return PushObject(L, thumb);
}

// ref: FUN_140aab270
int GetValue(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local value = self:GetValue()");
    float value = slider->m_value;
    RaisePendingError(L, slider, "GetValue");
    // FUN_140800790
    return PushFloat(L, value);
}

// ref: FUN_140aabff0
int GetValueStep(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local valueStep = self:GetValueStep()");
    float step = slider->m_valueStep;
    RaisePendingError(L, slider, "GetValueStep");
    return PushFloat(L, step);
}

// ref: FUN_140aacd70
int IsDraggingThumb(lua_State* L) {
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, "Usage: local isDraggingThumb = self:IsDraggingThumb()");
    bool dragging = slider->IsDraggingThumb();
    RaisePendingError(L, slider, "IsDraggingThumb");
    return PushBool(L, dragging);
}

// ref: FUN_140aaf660
int SetMinMaxValues(lua_State* L) {
    const char* usage = "Usage: self:SetMinMaxValues(minValue, maxValue)";
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, usage);
    // FUN_140a542f0
    float minValue = RequiredFinite(L, &index, usage);
    float maxValue = RequiredFinite(L, &index, usage);
    slider->SetMinMaxValues(minValue, maxValue);
    RaisePendingError(L, slider, "SetMinMaxValues");
    return 0;
}

// ref: FUN_140ab0440
int SetObeyStepOnDrag(lua_State* L) {
    const char* usage = "Usage: self:SetObeyStepOnDrag(obeyStepOnDrag)";
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, usage);
    bool obey = RequiredBool(L, &index, usage);
    slider->m_sliderFlags = (slider->m_sliderFlags & ~CSimpleSlider::SLIDER_OBEY_STEP_ON_DRAG)
        | (obey ? CSimpleSlider::SLIDER_OBEY_STEP_ON_DRAG : 0);
    RaisePendingError(L, slider, "SetObeyStepOnDrag");
    return 0;
}

// ref: FUN_140ab1210
int SetOrientation(lua_State* L) {
    const char* usage = "Usage: self:SetOrientation(orientation)";
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, usage);
    // FUN_140a54cf0
    uint32_t orientation = 0;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (lua_type(L, index) != LUA_TSTRING || !StringToOrientation(lua_tolstring(L, index, nullptr), &orientation)) {
        luaL_argerror(L, index, usage);
    } else {
        index++;
    }
    slider->SetOrientation(static_cast<int32_t>(orientation));
    RaisePendingError(L, slider, "SetOrientation");
    return 0;
}

// ref: FUN_140ab1fc0
int SetStepsPerPage(lua_State* L) {
    const char* usage = "Usage: self:SetStepsPerPage(stepsPerPage)";
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, usage);
    int32_t steps = RequiredInt32(L, &index, usage);
    slider->m_sliderFlags = (slider->m_sliderFlags & ~CSimpleSlider::SLIDER_STEPS_PER_PAGE) | static_cast<uint8_t>(steps);
    RaisePendingError(L, slider, "SetStepsPerPage");
    return 0;
}

struct ASSETARGS {
    CSimpleSlider* m_slider;
    TEXTURESOURCE m_source;
};

// ref: FUN_140a577b0
int32_t SetThumbTextureBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetThumbTexture(asset)";
    auto args = static_cast<ASSETARGS*>(context);
    int32_t index = 1;
    args->m_slider = SliderSelf(L, &index, usage);
    args->m_source = TextureSourceArgument(L, &index, usage);
    args->m_slider->SetThumbTexture(&args->m_source, DRAWLAYER_OVERLAY);
    RaisePendingError(L, args->m_slider, "SetThumbTexture");
    return 0;
}

// ref: FUN_140ab2db0
int SetThumbTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetThumbTextureBody, &args);
}

// ref: FUN_140ab3ad0
int SetValue(lua_State* L) {
    const char* usage = "Usage: self:SetValue(value [, treatAsMouseEvent])";
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, usage);
    float value = RequiredFinite(L, &index, usage);
    bool userInput = false;
    if (lua_type(L, index) >= 1) {
        userInput = lua_toboolean(L, index) != 0;
    }
    slider->SetValue(value, userInput);
    RaisePendingError(L, slider, "SetValue");
    return 0;
}

// ref: FUN_140ab48d0
int SetValueStep(lua_State* L) {
    const char* usage = "Usage: self:SetValueStep(valueStep)";
    int32_t index = 1;
    CSimpleSlider* slider = SliderSelf(L, &index, usage);
    float step = RequiredFinite(L, &index, usage);
    slider->SetValueStep(step);
    RaisePendingError(L, slider, "SetValueStep");
    return 0;
}

// DAT_142cf99f0
const luaL_Reg s_sliderMethods[] = {
    { "Disable", Disable },
    { "Enable", Enable },
    { "GetMinMaxValues", GetMinMaxValues },
    { "GetObeyStepOnDrag", GetObeyStepOnDrag },
    { "GetOrientation", GetOrientation },
    { "GetStepsPerPage", GetStepsPerPage },
    { "GetThumbTexture", GetThumbTexture },
    { "GetValue", GetValue },
    { "GetValueStep", GetValueStep },
    { "IsDraggingThumb", IsDraggingThumb },
    { "IsEnabled", IsEnabled },
    { "SetEnabled", SetEnabled },
    { "SetMinMaxValues", SetMinMaxValues },
    { "SetObeyStepOnDrag", SetObeyStepOnDrag },
    { "SetOrientation", SetOrientation },
    { "SetStepsPerPage", SetStepsPerPage },
    { "SetThumbTexture", SetThumbTexture },
    { "SetValue", SetValue },
    { "SetValueStep", SetValueStep },
};
static_assert(sizeof(s_sliderMethods) / sizeof(s_sliderMethods[0]) == 0x13, "the slider table has 0x13 methods");

}

// ref: FUN_140ae5950
void CSimpleSlider_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_sliderMethods, 0x13);
}
