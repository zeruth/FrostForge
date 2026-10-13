#include "ui/CSimpleButton.hpp"

#include <cstring>
#include <new>

#include "base/DataAllocator.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "os/Os.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

uint32_t OsGetAsyncTimeMs();

int32_t CSimpleButton::s_metaTable = -2;
int32_t CSimpleButton::s_restrictedMetaTable = -2;

namespace {

// The buttons' pool (0x1447aed30, made by the static initializer 0x14020db60).
CDataAllocator s_buttonHeap(0x368, 0x190);

// The frame flags the button keeps (unnamed in the frame's enum).
constexpr uint32_t FRAME_BUTTON_DISABLED = 0x8;
constexpr uint32_t FRAME_BUTTON = 0x80;

// The script indices of a button's handlers.
enum : uint8_t {
    SCRIPT_ONCLICK = 0,
    SCRIPT_PRECLICK = 1,
    SCRIPT_POSTCLICK = 2,
    SCRIPT_ONDOUBLECLICK = 3,
    SCRIPT_ONENTER = 0xe,
    SCRIPT_ONLEAVE = 0xf,
    SCRIPT_ONENABLE = 0x21,
    SCRIPT_ONDISABLE = 0x22,
};

// The point a button's text is anchored by: its font's horizontal justification.
int32_t TextAnchorPoint(CSimpleFont* font) {
    if (font == nullptr) {
        return FRAMEPOINT_CENTER;
    }
    uint32_t flags = font->m_attributes.m_stringFlags;
    if (flags & 1) {
        return FRAMEPOINT_LEFT;
    }
    if (flags & 4) {
        return FRAMEPOINT_RIGHT;
    }
    return FRAMEPOINT_CENTER;
}

bool Contains(const blz::embedded_vector<CScriptRegion*, 2, s_regionVectorTag>& regions, CScriptRegion* region) {
    for (uint64_t i = 0; i < regions.m_size; i++) {
        if (regions.m_data[i] == region) {
            return true;
        }
    }
    return false;
}

// A font colour from the XML, set on the font object as the reference does in place.
void ApplyFontColor(CSimpleFont* font, uint32_t color) {
    if (font == nullptr) {
        return;
    }
    if (color != font->m_attributes.m_color || !(font->m_attributes.m_flags & CSimpleFontAttributes::HAS_COLOR)) {
        font->m_attributes.m_color = color;
        font->m_attributes.m_flags |= CSimpleFontAttributes::HAS_COLOR | CSimpleFontAttributes::CHANGED_COLOR;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_COLOR;
    font->UpdateFontables();
}

}

// ref: FUN_1424deaf0
uint64_t ButtonClickMask(const char* name) {
    if (name == nullptr || *name == '\0') {
        return 0;
    }
    static const struct {
        const char* m_name;
        uint64_t m_mask;
    } s_masks[] = {
        { "LeftButtonDown", 0x1 }, { "LeftButtonUp", 0x80000000ull },
        { "MiddleButtonDown", 0x2 }, { "MiddleButtonUp", 0x100000000ull },
        { "RightButtonDown", 0x4 }, { "RightButtonUp", 0x200000000ull },
        { "Button4Down", 0x8 }, { "Button4Up", 0x400000000ull },
        { "Button5Down", 0x10 }, { "Button5Up", 0x800000000ull },
        { "Button6Down", 0x20 }, { "Button6Up", 0x1000000000ull },
        { "Button7Down", 0x40 }, { "Button7Up", 0x2000000000ull },
        { "Button8Down", 0x80 }, { "Button8Up", 0x4000000000ull },
        { "Button9Down", 0x100 }, { "Button9Up", 0x8000000000ull },
        { "Button10Down", 0x200 }, { "Button10Up", 0x10000000000ull },
        { "Button11Down", 0x400 }, { "Button11Up", 0x20000000000ull },
        { "Button12Up", 0x40000000000ull }, { "Button12Down", 0x800 },
        { "Button13Up", 0x80000000000ull }, { "Button13Down", 0x1000 },
        { "Button14Up", 0x100000000000ull }, { "Button14Down", 0x2000 },
        { "Button15Up", 0x200000000000ull }, { "Button15Down", 0x4000 },
        { "Button16Up", 0x400000000000ull }, { "Button16Down", 0x8000 },
        { "Button17Up", 0x800000000000ull }, { "Button17Down", 0x10000 },
        { "Button18Up", 0x1000000000000ull }, { "Button18Down", 0x20000 },
        { "Button19Up", 0x2000000000000ull }, { "Button19Down", 0x40000 },
        { "Button20Down", 0x80000 }, { "Button20Up", 0x4000000000000ull },
        { "Button21Down", 0x100000 }, { "Button21Up", 0x8000000000000ull },
        { "Button22Up", 0x10000000000000ull }, { "Button22Down", 0x200000 },
        { "Button23Up", 0x20000000000000ull }, { "Button23Down", 0x400000 },
        { "Button24Up", 0x40000000000000ull }, { "Button24Down", 0x800000 },
        { "Button25Up", 0x80000000000000ull }, { "Button25Down", 0x1000000 },
        { "Button26Up", 0x100000000000000ull }, { "Button26Down", 0x2000000 },
        { "Button27Up", 0x200000000000000ull }, { "Button27Down", 0x4000000 },
        { "Button28Up", 0x400000000000000ull }, { "Button28Down", 0x8000000 },
        { "Button29Up", 0x800000000000000ull }, { "Button29Down", 0x10000000 },
        { "Button30Down", 0x20000000 }, { "Button30Up", 0x1000000000000000ull },
        { "Button31Down", 0x40000000 }, { "Button31Up", 0x2000000000000000ull },
        { "AnyDown", 0x7fffffff }, { "AnyUp", 0xffffffff80000000ull },
    };
    for (const auto& entry : s_masks) {
        if (_stricmp(name, entry.m_name) == 0) {
            return entry.m_mask;
        }
    }
    return 0;
}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1424aac80
CSimpleButton::CSimpleButton(CSimpleFrame* parent) : CSimpleFrame(parent) {
    this->m_state &= 0xfffffff0;
    this->m_buttonFlags &= 0xf8;
    this->m_pushedTextOffset[0] = 0.001f;
    this->m_pushedTextOffset[1] = -0.001f;
    this->m_lastClickTime = 0;
    this->m_registeredClicks = 0x80000000ull;
    this->m_text = nullptr;
    this->m_normalFont = nullptr;
    this->m_highlightFont = nullptr;
    this->m_disabledFont = nullptr;
    this->m_highlightTexture = nullptr;
    this->m_registeredMouse = ~0ull;
    this->m_stateTextures[0] = nullptr;
    this->m_stateTextures[1] = nullptr;
    this->m_stateTextures[2] = nullptr;
    this->SetEnabled(true);
    this->SetMouseMotionEnabled(true);
    this->SetMouseClickEnabled(true);
    this->SetFrameFlag(FRAME_BUTTON, true);
}

// ref: FUN_1424aad80
CSimpleButton::~CSimpleButton() = default;

void* CSimpleButton::operator new(size_t size) {
    (void)size;
    return s_buttonHeap.GetData(false, ".?AVCSimpleButton@@", -2, 0x1f);
}

// ref: FUN_1424aadc0
void CSimpleButton::operator delete(void* button) {
    if (button != nullptr) {
        s_buttonHeap.PutData(button);
    }
}

// DAT_14309fd34
// The reference's type id and its magic-static guard (CSimpleButton), held here so translated code
// and the port number the class once, alike.
int32_t g_cSimpleButtonType;  // DAT_14309fd34
int32_t g_cSimpleButtonTypeGuard;  // DAT_14309fd38

int32_t CSimpleButton::GetObjectType() {
    return FrameScript_TypeId(&g_cSimpleButtonType, &g_cSimpleButtonTypeGuard);
}

// ref: FUN_1424aafd0
void CSimpleButton::CreateScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    s_metaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleButton_FillScriptMethods, false);
    s_restrictedMetaTable = FrameScript_Object::CreateScriptMetaTable(L, CSimpleButton_FillScriptMethods, true);
}

// ref: FUN_1424ab020
void CSimpleButton::DestroyScriptMetaTables() {
    lua_State* L = FrameScript_GetContext();
    luaL_unref(L, LUA_REGISTRYINDEX, s_metaTable);
    luaL_unref(L, LUA_REGISTRYINDEX, s_restrictedMetaTable);
    s_metaTable = -2;
    s_restrictedMetaTable = -2;
}

// ---- FrameScript_Object ----------------------------------------------------------------------

// ref: FUN_1424ab150
int32_t CSimpleButton::GetScriptMetaTable() {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & FSOBJ_PROTECTED_METATABLE)) {
        return s_metaTable;
    }
    return s_restrictedMetaTable;
}

// ref: FUN_1424aca60
bool CSimpleButton::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    if (index == SCRIPT_ONCLICK || index == SCRIPT_PRECLICK || index == SCRIPT_POSTCLICK) {
        data->m_wrapper = "return function(self,button,down) %s end";
        return true;
    }
    if (index == SCRIPT_ONDOUBLECLICK) {
        data->m_wrapper = "return function(self,button) %s end";
        return true;
    }
    return this->CSimpleFrame::GetScriptData(index, data);
}

// ref: FUN_1424ab2c0
bool CSimpleButton::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Button") == 0 || _stricmp(name, "Frame") == 0 || _stricmp(name, "Region") == 0
        || _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_1424ab1c0
bool CSimpleButton::IsObjectType(int32_t type) {
    if (type == CSimpleButton::GetObjectType() || type == CSimpleFrame::GetObjectType()) {
        return true;
    }
    return this->CScriptRegion::IsObjectType(type);
}

// ref: FUN_1424ab130
const char* CSimpleButton::GetObjectTypeName() {
    return "Button";
}

// ref: FUN_1424ab100
uint32_t CSimpleButton::GetObjectSize() {
    return 0x368;
}

// ---- XML ----------------------------------------------------------------------------------------

// ref: FUN_1424ab340
void CSimpleButton::LoadXML(XMLOBJECT* node, CStatus* status, const char* intrinsic) {
    this->CSimpleFrame::LoadXML(node, status, intrinsic);
    if (!(node->m_flags & 0x80)) {
        return;
    }
    auto scriptStatus = static_cast<CScriptStatus*>(status);
    CSimpleFontString* text = nullptr;
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        switch (child->m_type) {
            case XMLTYPE_NORMALTEXTURE:
            case XMLTYPE_PUSHEDTEXTURE:
            case XMLTYPE_DISABLEDTEXTURE: {
                auto region = static_cast<CSimpleTexture*>(FrameXML_CreateTexture(child, this, scriptStatus));
                TEXTURESOURCE source = TEXTURESOURCE::FromTexture(region);
                int32_t state = child->m_type == XMLTYPE_NORMALTEXTURE ? BUTTON_NORMAL
                    : child->m_type == XMLTYPE_PUSHEDTEXTURE ? BUTTON_PUSHED
                                                             : BUTTON_DISABLED;
                this->SetStateTexture(state, &source);
                break;
            }
            case XMLTYPE_HIGHLIGHTTEXTURE: {
                auto region = static_cast<CSimpleTexture*>(FrameXML_CreateTexture(child, this, scriptStatus));
                uint8_t blend[2] = { 0, 0 };
                if (region != nullptr) {
                    blend[0] = region->m_blendMode;
                    blend[1] = 1;
                }
                TEXTURESOURCE source = TEXTURESOURCE::FromTexture(region);
                this->SetHighlight(&source, blend);
                break;
            }
            case XMLTYPE_BUTTONTEXT:
                text = static_cast<CSimpleFontString*>(FrameXML_CreateFontString(child, this, scriptStatus));
                break;
            case XMLTYPE_DISABLEDFONT:
            case XMLTYPE_NORMALFONT:
            case XMLTYPE_HIGHLIGHTFONT: {
                auto file = static_cast<XMLFILE*>(child);
                if (!(file->m_fileFlags & 1)) {
                    break;
                }
                CSimpleFont* font = CSimpleFont::GetFont(file->m_file.m_text, false);
                const char* which = "NORMAL";
                if (child->m_type == XMLTYPE_DISABLEDFONT) {
                    this->m_disabledFont = font;
                    which = "DISABLED";
                } else if (child->m_type == XMLTYPE_NORMALFONT) {
                    this->m_normalFont = font;
                } else {
                    this->m_highlightFont = font;
                    which = "HIGHLIGHT";
                }
                if (font == nullptr) {
                    status->Add(1, "Couldn't find %s font: %s", which, file->m_file.m_text);
                }
                break;
            }
            case XMLTYPE_NORMALCOLOR:
                ApplyFontColor(this->m_normalFont, XmlColorPack(static_cast<XMLCOLOR*>(child)));
                break;
            case XMLTYPE_DISABLEDCOLOR:
                ApplyFontColor(this->m_disabledFont, XmlColorPack(static_cast<XMLCOLOR*>(child)));
                break;
            case XMLTYPE_HIGHLIGHTCOLOR:
                ApplyFontColor(this->m_highlightFont, XmlColorPack(static_cast<XMLCOLOR*>(child)));
                break;
            case XMLTYPE_PUSHEDTEXTOFFSET: {
                float x = 0.0f;
                float y = 0.0f;
                // FUN_140667be0
                if (XMLDimensionGet(static_cast<XMLDIMENSION*>(child), &x, &y, status) && (this->m_state & 0xf) != BUTTON_PUSHED) {
                    this->m_pushedTextOffset[0] = x;
                    this->m_pushedTextOffset[1] = y;
                }
                break;
            }
            default:
                break;
        }
    }
    if (text != nullptr) {
        if (text != this->m_text) {
            this->SetFontString(text);
        }
    } else {
        this->UpdateTextState(static_cast<int32_t>(this->m_state << 28) >> 28);
    }
    auto xml = static_cast<XMLBUTTON*>(node);
    if (xml->m_buttonFlags & 1) {
        const char* value = FrameScript_GetText(xml->m_text.m_text, -1, 0, true);
        if (value == nullptr || *value == '\0') {
            value = xml->m_text.m_text;
        }
        this->SetText(value);
    }
    if (xml->m_buttonFlags & 2) {
        const char* cursor = xml->m_registerForClicks.m_text;
        char token[0x400];
        uint64_t masks = 0;
        SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
        while (token[0] != '\0') {
            masks |= ButtonClickMask(token);
            SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
        }
        this->m_registeredClicks = masks;
    }
    if (xml->m_buttonFlags & 4) {
        this->SetMotionScriptsWhileDisabled(((xml->m_buttonFlags >> 3) & 1) != 0);
    }
}

// ---- input ------------------------------------------------------------------------------------

// ref: FUN_1424abd40
bool CSimpleButton::OnMouseDown(MOUSEEVENT* event, const char* button) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended != 0 && static_cast<uint8_t>(event->m_flags) == 0) {
        return false;
    }
    uint64_t mask = button == nullptr ? static_cast<uint64_t>(static_cast<int64_t>(event->m_button)) : 0x7fffffffull;
    if ((this->m_registeredMouse & mask) == 0) {
        return true;
    }
    bool handled = this->CSimpleFrame::OnMouseDown(event, button);
    if (handled) {
        return true;
    }
    if (button == nullptr) {
        float point[2] = { event->m_x, event->m_y };
        if (!this->IsMouseOver(point)) {
            return false;
        }
    }
    if ((this->m_state & 0xf) != BUTTON_DISABLED) {
        if (this->m_registeredClicks & mask) {
            if (button == nullptr) {
                button = MouseButtonToString(event->m_button);
            }
            this->OnClick(button, true, static_cast<uint8_t>(event->m_flags) == 0, true, nullptr);
        }
        if (!(this->m_buttonFlags & BUTTON_STATE_LOCKED) && (this->m_state & 0xf) != BUTTON_DISABLED) {
            this->SetButtonState(BUTTON_PUSHED, false);
        }
    }
    return true;
}

// ref: FUN_1424abe70
bool CSimpleButton::OnMouseUp(MOUSEEVENT* event, const char* button) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended != 0 && static_cast<uint8_t>(event->m_flags) == 0) {
        return false;
    }
    uint64_t mask = button == nullptr ? static_cast<uint64_t>(static_cast<int64_t>(event->m_button)) << 31 : 0xffffffff80000000ull;
    if ((this->m_registeredMouse & mask) == 0) {
        return true;
    }
    if (this->CSimpleFrame::OnMouseUp(event, button)) {
        return true;
    }
    if ((this->m_state & 0xf) != BUTTON_PUSHED) {
        return true;
    }
    bool inside = true;
    if (button == nullptr) {
        float point[2] = { event->m_x, event->m_y };
        inside = this->IsMouseOver(point);
    }
    if (inside && (this->m_registeredClicks & mask) != 0) {
        if (button == nullptr) {
            button = MouseButtonToString(event->m_button);
        }
        uint32_t now = OsGetAsyncTimeMs();
        if (!this->HasScriptAny(SCRIPT_ONDOUBLECLICK) || this->m_lastClickTime == 0 || 300 < now - this->m_lastClickTime) {
            this->OnClick(button, false, static_cast<uint8_t>(event->m_flags) == 0, true, nullptr);
            this->m_lastClickTime = now;
        } else {
            this->OnDoubleClick(button);
            this->m_lastClickTime = 0;
        }
    }
    if (!(this->m_buttonFlags & BUTTON_STATE_LOCKED) && (this->m_state & 0xf) != BUTTON_DISABLED) {
        this->SetButtonState(BUTTON_NORMAL, false);
    }
    return true;
}

// ref: FUN_1424abd00
void CSimpleButton::QueueOnHide() {
    if ((this->m_state & 0xf) != BUTTON_DISABLED && !(this->m_buttonFlags & BUTTON_STATE_LOCKED)) {
        this->SetButtonState(BUTTON_NORMAL, false);
    }
    this->CSimpleFrame::QueueOnHide();
}

// ref: FUN_1424abc20
void CSimpleButton::OnEnter(bool motion) {
    if (CSimpleTop::s_instance->m_inputSuspended != 0) {
        return;
    }
    if ((this->m_state & 0xf) != BUTTON_DISABLED) {
        this->CSimpleFrame::OnEnter(motion);
        this->UpdateTextState(static_cast<int32_t>(this->m_state << 28) >> 28);
        return;
    }
    if (this->GetMotionScriptsWhileDisabled()) {
        // FUN_14060f370
        this->RunScript(SCRIPT_ONENTER, nullptr, motion);
    }
}

// ref: FUN_1424abc90
void CSimpleButton::OnLeave(bool motion) {
    if (CSimpleTop::s_instance->m_inputSuspended != 0) {
        return;
    }
    if ((this->m_state & 0xf) != BUTTON_DISABLED) {
        this->CSimpleFrame::OnLeave(motion);
        this->UpdateTextState(static_cast<int32_t>(this->m_state << 28) >> 28);
        return;
    }
    if (this->GetMotionScriptsWhileDisabled()) {
        // FUN_14060f380
        this->RunScript(SCRIPT_ONLEAVE, nullptr, motion);
    }
}

// ref: FUN_1424abbb0
void CSimpleButton::OnDragStart(MOUSEEVENT* event) {
    if (CSimpleTop::s_instance->m_inputSuspended != 0) {
        return;
    }
    if ((this->m_state & 0xf) != BUTTON_DISABLED && !(this->m_buttonFlags & BUTTON_STATE_LOCKED)) {
        this->SetButtonState(BUTTON_NORMAL, false);
    }
    if ((this->m_state & 0xf) != BUTTON_DISABLED) {
        this->CSimpleFrame::OnDragStart(event);
    }
}

// ref: FUN_1424acaa0
void CSimpleButton::RemoveRegion(CSimpleRegion* region) {
    if (region == this->m_text) {
        this->m_text = nullptr;
    }
    for (CSimpleTexture*& texture : this->m_stateTextures) {
        if (texture == region) {
            texture = nullptr;
        }
    }
    if (region == this->m_highlightTexture) {
        this->m_highlightTexture = nullptr;
    }
    this->CSimpleFrame::RemoveRegion(region);
}

// ref: FUN_1424ac670
void CSimpleButton::LockHighlight(bool lock) {
    this->CSimpleFrame::LockHighlight(lock);
    this->UpdateTextState(static_cast<int32_t>(this->m_state << 28) >> 28);
}

// ref: FUN_1424abae0
void CSimpleButton::ScaleBy(float* scale) {
    this->m_pushedTextOffset[0] *= *scale;
    this->m_pushedTextOffset[1] *= *scale;
    this->CSimpleFrame::ScaleBy(scale);
}

// ---- the button's slots -------------------------------------------------------------------

// ref: FUN_1424ac140
void CSimpleButton::SetEnabled(bool enabled) {
    if (((this->m_state & 0xf) != BUTTON_DISABLED) == enabled) {
        return;
    }
    CSimpleTop* top = CSimpleTop::s_instance;
    uint8_t script;
    if (!enabled) {
        if (Contains(top->m_mouseOver, this)) {
            this->OnMouseFocusLost(false, true);
        }
        if (Contains(top->m_mouseMotion, this) && !this->GetMotionScriptsWhileDisabled()) {
            this->OnLeave(false);
        }
        this->DisableDrawLayer(DRAWLAYER_HIGHLIGHT);
        this->SetButtonState(BUTTON_DISABLED, false);
        this->SetFrameFlag(FRAME_BUTTON_DISABLED, true);
        script = SCRIPT_ONDISABLE;
    } else {
        this->SetButtonState(BUTTON_NORMAL, false);
        if (Contains(top->m_mouseOver, this)) {
            this->OnMouseOverChanged(false);
        }
        if (Contains(top->m_mouseMotion, this) && !this->GetMotionScriptsWhileDisabled()) {
            this->OnEnter(false);
        }
        if (this->m_frameFlags & FRAME_HIGHLIGHT_LOCKED) {
            this->EnableDrawLayer(DRAWLAYER_HIGHLIGHT);
        }
        this->SetFrameFlag(FRAME_BUTTON_DISABLED, false);
        script = SCRIPT_ONENABLE;
    }
    if (this->FrameScript_Object::m_flags & FSOBJ_LOADING) {
        return;
    }
    // FUN_140603620
    this->RunScript(script, nullptr);
}

// ref: FUN_1424ab9e0
void CSimpleButton::OnClick(const char* button, bool down, bool fromInput, bool secure, const char* taint) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if ((fromInput && top->m_inputSuspended != 0) || (this->m_buttonFlags & BUTTON_IN_CLICK) || (this->m_state & 0xf) == BUTTON_DISABLED) {
        return;
    }
    const char* previous = top->m_currentButton;
    top->m_currentButton = button;
    this->m_buttonFlags |= BUTTON_IN_CLICK;
    if (secure) {
        // FUN_14060f6a0
        top->m_inMouseScript = 1;
    }
    // FUN_14060d8f0, once per handler
    this->RunScript(SCRIPT_PRECLICK, taint, button, down);
    this->RunScript(SCRIPT_ONCLICK, taint, button, down);
    this->RunScript(SCRIPT_POSTCLICK, taint, button, down);
    if (secure) {
        // FUN_14060e0c0
        top->m_inMouseScript = 0;
    }
    this->m_buttonFlags &= ~BUTTON_IN_CLICK;
    top->m_currentButton = previous;
}

// ref: FUN_1424abb10
void CSimpleButton::OnDoubleClick(const char* button) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (top->m_inputSuspended != 0 || (this->m_buttonFlags & BUTTON_IN_CLICK) || (this->m_state & 0xf) == BUTTON_DISABLED) {
        return;
    }
    const char* previous = top->m_currentButton;
    top->m_currentButton = button;
    this->m_buttonFlags |= BUTTON_IN_CLICK;
    top->m_inMouseScript = 1;
    // FUN_140603870
    this->RunScript(SCRIPT_ONDOUBLECLICK, nullptr, button);
    top->m_inMouseScript = 0;
    this->m_buttonFlags &= ~BUTTON_IN_CLICK;
    top->m_currentButton = previous;
}

// ref: FUN_1424ac040
void CSimpleButton::SetButtonState(int32_t state, bool locked) {
    if (static_cast<uint64_t>(static_cast<int64_t>(state)) >= 3) {
        return;
    }
    int32_t current = static_cast<int32_t>(this->m_state << 28) >> 28;
    this->m_buttonFlags = static_cast<uint8_t>((this->m_buttonFlags & ~BUTTON_STATE_LOCKED) | (locked ? BUTTON_STATE_LOCKED : 0));
    if (state == current) {
        return;
    }
    CSimpleTexture* old = this->m_stateTextures[current];
    if (old != nullptr && (this->m_stateTextures[state] != nullptr || state == BUTTON_NORMAL)) {
        old->Hide();
    }
    if (this->m_stateTextures[state] != nullptr) {
        this->m_stateTextures[state]->Show();
    }
    this->UpdateTextState(state);
    this->m_state = (this->m_state & 0xfffffff0) | static_cast<uint32_t>(state & 0xf);
}

// ref: FUN_1424ac6b0
void CSimpleButton::SetMotionScriptsWhileDisabled(bool enabled) {
    this->m_buttonFlags = static_cast<uint8_t>((this->m_buttonFlags & ~BUTTON_MOTION_WHILE_DISABLED)
        | (enabled ? BUTTON_MOTION_WHILE_DISABLED : 0));
}

// ref: FUN_1424ab110
bool CSimpleButton::GetMotionScriptsWhileDisabled() {
    return (this->m_buttonFlags >> 2 & 1) != 0;
}

// ---- the rest -----------------------------------------------------------------------------------

// ref: FUN_1424aaf40
void CSimpleButton::Click(const char* button, bool down) {
    if (!(this->FrameScript_Object::m_flags & FSOBJ_FORBIDDEN) && !(this->FrameScript_Object::m_flags & 4)) {
        const char* taint = FrameScript_GetContext() != nullptr ? lua_taint : nullptr;
        this->OnClick(button, down, false, false, taint);
        return;
    }
    FrameScript_ActionError(":Click cannot be called on Forbidden frames.");
}

// ref: FUN_1424ac750
bool CSimpleButton::SetStateTexture(int32_t state, const TEXTURESOURCE* source) {
    CSimpleTexture* adopted = source->GetTexture();
    CSimpleTexture* texture = this->m_stateTextures[state];
    if (adopted != nullptr && adopted == texture) {
        return true;
    }
    bool wasShown = false;
    if (texture != nullptr) {
        if (source->m_type == TEXTURESOURCE::SOURCE_ATLAS && texture->m_atlas == source->m_id) {
            return true;
        }
        if (texture->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) {
            wasShown = true;
            texture->Hide();
        }
    }
    if (adopted == nullptr) {
        if (texture == nullptr) {
            texture = new CSimpleTexture(this, DRAWLAYER_ARTWORK, 0, true);
        }
        this->m_stateTextures[state] = texture;
        if (texture == nullptr) {
            return true;
        }
    } else {
        this->m_stateTextures[state] = adopted;
        texture = adopted;
    }
    if (!texture->IsAnchored()) {
        texture->SetAllPoints(this, true);
    }
    texture->SetFrame(this, DRAWLAYER_ARTWORK, 0, false);
    if (wasShown || state == static_cast<int32_t>(this->m_state << 28) >> 28) {
        texture->Show();
    }
    if (adopted != nullptr) {
        return true;
    }
    return texture->SetTextureSource(source);
}

// ref: FUN_1424ac420
bool CSimpleButton::SetHighlight(const TEXTURESOURCE* source, const uint8_t* blendMode) {
    CSimpleTexture* adopted = source->GetTexture();
    CSimpleTexture* texture = this->m_highlightTexture;
    if (adopted != nullptr && texture == adopted) {
        texture->SetBlendMode(blendMode[1] != 0 ? blendMode[0] : 3);
        return true;
    }
    bool shown = texture != nullptr && (texture->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) != 0;
    if (adopted == nullptr) {
        if (texture == nullptr) {
            texture = new CSimpleTexture(this, DRAWLAYER_ARTWORK, 0, true);
            this->m_highlightTexture = texture;
            if (texture == nullptr) {
                return true;
            }
        }
    } else {
        if (texture != nullptr) {
            texture->SetFrame(this, DRAWLAYER_ARTWORK, 0, true);
            texture->Hide();
        }
        this->m_highlightTexture = adopted;
        texture = adopted;
    }
    if (!texture->IsAnchored()) {
        texture->SetAllPoints(this, true);
    }
    texture->SetFrame(this, DRAWLAYER_HIGHLIGHT, 0, true);
    texture->SetBlendMode(blendMode[1] != 0 ? blendMode[0] : 3);
    if (shown) {
        texture->Show();
    }
    if (adopted != nullptr) {
        return true;
    }
    return texture->SetTextureSource(source);
}

// ref: FUN_1424ac340
void CSimpleButton::SetFontString(CSimpleFontString* text) {
    if (text == this->m_text) {
        return;
    }
    if (this->m_text != nullptr) {
        this->m_text->Hide();
    }
    if (text != nullptr) {
        text->SetFrame(this, DRAWLAYER_ARTWORK, 0, true);
        if (!text->IsAnchored()) {
            int32_t point = TextAnchorPoint(this->m_normalFont);
            text->SetPoint(point, this, point, 0.0f, 0.0f, true);
        }
    }
    this->m_text = text;
    this->UpdateTextState(static_cast<int32_t>(this->m_state << 28) >> 28);
}

// ref: FUN_1424ac8e0
void CSimpleButton::SetText(const char* text) {
    if (text == nullptr || *text == '\0') {
        if (this->m_text == nullptr) {
            return;
        }
    } else if (this->m_text == nullptr) {
        auto created = new CSimpleFontString(this, DRAWLAYER_ARTWORK, true);
        if (created != this->m_text) {
            if (this->m_text != nullptr) {
                this->m_text->Hide();
            }
            if (created != nullptr) {
                created->SetFrame(this, DRAWLAYER_ARTWORK, 0, true);
                if (!created->IsAnchored()) {
                    int32_t point = TextAnchorPoint(this->m_normalFont);
                    created->SetPoint(point, this, point, 0.0f, 0.0f, true);
                }
            }
            this->m_text = created;
            this->UpdateTextState(static_cast<int32_t>(this->m_state << 28) >> 28);
        }
    }
    this->m_text->SetText(text, true);
}

// ref: FUN_1424acaf0
void CSimpleButton::UpdateTextState(int32_t state) {
    CSimpleFontString* text = this->m_text;
    if (text == nullptr) {
        return;
    }
    if ((this->m_state & 0xf) == BUTTON_PUSHED) {
        if (state != BUTTON_PUSHED) {
            text->OffsetPoints(-this->m_pushedTextOffset[0], -this->m_pushedTextOffset[1], true);
            if (state == BUTTON_DISABLED) {
                text->SetFontObject(this->m_disabledFont);
                return;
            }
        }
    } else if (state == BUTTON_PUSHED) {
        text->OffsetPoints(this->m_pushedTextOffset[0], this->m_pushedTextOffset[1], true);
    } else if (state == BUTTON_DISABLED) {
        text->SetFontObject(this->m_disabledFont);
        return;
    }
    if (!(this->m_flags2 & (1u << DRAWLAYER_HIGHLIGHT))) {
        text->SetFontObject(this->m_normalFont);
        return;
    }
    text->SetFontObject(this->m_highlightFont);
}

// ref: FUN_1424ab120
CSimpleTexture* CSimpleButton::GetNormalTexture() {
    return this->m_stateTextures[BUTTON_NORMAL];
}

// ref: FUN_1424ab140
CSimpleTexture* CSimpleButton::GetPushedTexture() {
    return this->m_stateTextures[BUTTON_PUSHED];
}

// ref: FUN_1424ab180
float CSimpleButton::GetTextHeight() {
    if (this->m_text == nullptr) {
        return 0.0f;
    }
    return this->m_text->GetHeight();
}

// ref: FUN_1424ab1a0
float CSimpleButton::GetTextWidth() {
    if (this->m_text == nullptr) {
        return 0.0f;
    }
    return this->m_text->GetStringWidth();
}

// ref: FUN_1424aae50
void CSimpleButton::ClearDisabledTexture() {
    if (this->m_stateTextures[BUTTON_DISABLED] != nullptr) {
        this->m_stateTextures[BUTTON_DISABLED]->SetTextureByName("", 0, 0, 0, true, true);
    }
}

// ref: FUN_1424aae90
void CSimpleButton::ClearHighlightTexture() {
    uint8_t blend[2] = { 0, 0 };
    TEXTURESOURCE none = TEXTURESOURCE::FromTexture(nullptr);
    this->SetHighlight(&none, blend);
}

// ref: FUN_1424aaec0
void CSimpleButton::ClearNormalTexture() {
    if (this->m_stateTextures[BUTTON_NORMAL] != nullptr) {
        this->m_stateTextures[BUTTON_NORMAL]->SetTextureByName("", 0, 0, 0, true, true);
    }
}

// ref: FUN_1424aaf00
void CSimpleButton::ClearPushedTexture() {
    if (this->m_stateTextures[BUTTON_PUSHED] != nullptr) {
        this->m_stateTextures[BUTTON_PUSHED]->SetTextureByName("", 0, 0, 0, true, true);
    }
}

namespace {

// ref: FUN_1424ab070
uint64_t OrMasks(const uint64_t* masks, uint64_t count) {
    uint64_t result = 0;
    for (uint64_t i = 0; i < count; i++) {
        result |= masks[i];
    }
    return result;
}

}

// ref: FUN_1424ac000
void CSimpleButton::RegisterForClicks(const uint64_t* masks, uint64_t count) {
    this->m_registeredClicks = OrMasks(masks, count);
}

// ref: FUN_1424ac020
void CSimpleButton::RegisterForMouse(const uint64_t* masks, uint64_t count) {
    this->m_registeredMouse = OrMasks(masks, count);
}
