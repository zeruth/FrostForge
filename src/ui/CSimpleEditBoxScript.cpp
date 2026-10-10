// The Lua methods of edit boxes (CSimpleEditBox's table 0x142cf7d20, 0x4e methods).

#include <cstring>

#include "base/Coordinate.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleEditBox.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/UIScriptArgs.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

namespace {

using namespace ScriptMethod;

CSimpleEditBox* BoxSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleEditBox*>(Self(L, CSimpleEditBox::GetObjectType(), index, usage));
}

bool Present(lua_State* L, int32_t index) {
    return lua_type(L, index) >= 1;
}

bool OptionalFlag(lua_State* L, int32_t* index) {
    bool value = false;
    if (Present(L, *index)) {
        value = lua_toboolean(L, *index) != 0;
    }
    (*index)++;
    return value;
}

const char* RequiredText(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
        return "";
    }
    if (!lua_isstring(L, *index)) {
        luaL_argerror(L, *index, usage);
        return "";
    }
    return lua_tolstring(L, (*index)++, nullptr);
}

int Done(lua_State* L, CSimpleEditBox* box, const char* method) {
    RaisePendingError(L, box, method);
    return 0;
}

// ---- no arguments ------------------------------------------------------------------------------

// ref: FUN_140905a10
int ClearFocus(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:ClearFocus()");
    box->ClearFocus();
    return Done(L, box, "ClearFocus");
}

// ref: FUN_140906780
int ClearHighlightText(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:ClearHighlightText()");
    box->ClearHighlight();
    return Done(L, box, "ClearHighlightText");
}

// ref: FUN_1409074f0
int ClearHistory(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:ClearHistory()");
    box->ClearHistory();
    return Done(L, box, "ClearHistory");
}

// ref: FUN_140908260
int Disable(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:Disable()");
    box->SetEnabled(false);
    return Done(L, box, "Disable");
}

// ref: FUN_140908fd0
int Enable(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:Enable()");
    box->SetEnabled(true);
    return Done(L, box, "Enable");
}

// ref: FUN_14092aef0
int ResetInputMode(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:ResetInputMode()");
    box->ResetInputMode();
    return Done(L, box, "ResetInputMode");
}

// ref: FUN_140931c70
int SetFocus(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:SetFocus()");
    // FUN_141a9a310
    box->SetFocus(true);
    return Done(L, box, "SetFocus");
}

// ref: FUN_14093f860
int SetSecurityDisablePaste(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:SetSecurityDisablePaste()");
    box->m_editFlags |= CSimpleEditBox::EDIT_SECURITY_DISABLE_PASTE;
    return Done(L, box, "SetSecurityDisablePaste");
}

// ref: FUN_1409405c0
int SetSecurityDisableSetText(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:SetSecurityDisableSetText()");
    box->m_editFlags |= CSimpleEditBox::EDIT_SECURITY_DISABLE_SET_TEXT;
    return Done(L, box, "SetSecurityDisableSetText");
}

// ref: FUN_140947670
int ToggleInputLanguage(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: self:ToggleInputLanguage()");
    box->ToggleInputLanguage();
    return Done(L, box, "ToggleInputLanguage");
}

// ---- getters ---------------------------------------------------------------------------------

template <uint32_t Flag>
int GetFlag(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    bool value = (box->m_editFlags & Flag) != 0;
    RaisePendingError(L, box, method);
    return PushBool(L, value);
}

// ref: FUN_140909d40
int GetAltArrowKeyMode(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_ALT_ARROW_KEY_MODE>(L, "Usage: local altMode = self:GetAltArrowKeyMode()", "GetAltArrowKeyMode");
}

// ref: FUN_1409227f0
int IsAlphabeticOnly(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_ALPHABETIC_ONLY>(L, "Usage: local enabled = self:IsAlphabeticOnly()", "IsAlphabeticOnly");
}

// ref: FUN_140923570
int IsAutoFocus(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_AUTO_FOCUS>(L, "Usage: local autoFocus = self:IsAutoFocus()", "IsAutoFocus");
}

// ref: FUN_1409242f0
int IsCountInvisibleLetters(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_COUNT_INVISIBLE_LETTERS>(L, "Usage: local countInvisibleLetters = self:IsCountInvisibleLetters()",
        "IsCountInvisibleLetters");
}

// ref: FUN_140926b70
int IsMultiLine(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_MULTI_LINE>(L, "Usage: local multiline = self:IsMultiLine()", "IsMultiLine");
}

// ref: FUN_1409278f0
int IsNumeric(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_NUMERIC>(L, "Usage: local isNumeric = self:IsNumeric()", "IsNumeric");
}

// ref: FUN_140928670
int IsNumericFullRange(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_NUMERIC_FULL_RANGE>(L, "Usage: local isNumeric = self:IsNumericFullRange()", "IsNumericFullRange");
}

// ref: FUN_1409293f0
int IsPassword(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_PASSWORD>(L, "Usage: local isPassword = self:IsPassword()", "IsPassword");
}

// ref: FUN_14092a170
int IsSecureText(lua_State* L) {
    return GetFlag<CSimpleEditBox::EDIT_SECURE_TEXT>(L, "Usage: local isSecure = self:IsSecureText()", "IsSecureText");
}

// ref: FUN_140925070
int IsEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local isEnabled = self:IsEnabled()");
    bool enabled = !((box->m_frameFlags >> 3) & 1);
    RaisePendingError(L, box, "IsEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_140925df0
int IsInIMECompositionMode(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local isInIMECompositionMode = self:IsInIMECompositionMode()");
    bool composing = box->m_imeComposing;
    RaisePendingError(L, box, "IsInIMECompositionMode");
    return PushBool(L, composing);
}

// ref: FUN_14091ef70
int HasFocus(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local hasFocus = self:HasFocus()");
    bool focus = box->HasFocus();
    RaisePendingError(L, box, "HasFocus");
    return PushBool(L, focus);
}

// ref: FUN_14091fcf0
int HasText(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local hasText = self:HasText()");
    bool text = box->HasText();
    RaisePendingError(L, box, "HasText");
    return PushBool(L, text);
}

// ref: FUN_14090aac0
int GetBlinkSpeed(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local cursorBlinkSpeedSec = self:GetBlinkSpeed()");
    float speed = box->m_blinkSpeed;
    RaisePendingError(L, box, "GetBlinkSpeed");
    return PushFloat(L, speed);
}

// ref: FUN_14090b840
int GetCursorPosition(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local cursorPosition = self:GetCursorPosition()");
    int32_t position = box->m_cursorPos;
    RaisePendingError(L, box, "GetCursorPosition");
    return PushInt(L, position);
}

// ref: FUN_14091d460
int GetUTF8CursorPosition(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local cursorPosition = self:GetUTF8CursorPosition()");
    // FUN_140253350
    int32_t position = SStrLenUTF8N(box->m_text, box->m_cursorPos);
    RaisePendingError(L, box, "GetUTF8CursorPosition");
    return PushInt(L, position);
}

// ref: FUN_14090fca0
int GetHistoryLines(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local numHistoryLines = self:GetHistoryLines()");
    int32_t lines = box->m_historyLines;
    RaisePendingError(L, box, "GetHistoryLines");
    return PushInt(L, lines);
}

// ref: FUN_140914020
int GetMaxBytes(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local maxBytes = self:GetMaxBytes()");
    int32_t bytes = box->m_maxBytes + 1;
    RaisePendingError(L, box, "GetMaxBytes");
    return PushInt(L, bytes);
}

// ref: FUN_140914da0
int GetMaxLetters(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local maxLetters = self:GetMaxLetters()");
    int32_t letters = box->m_maxLetters;
    RaisePendingError(L, box, "GetMaxLetters");
    return PushInt(L, letters);
}

// ref: FUN_14091e1f0
int GetVisibleTextByteLimit(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local maxVisibleBytes = self:GetVisibleTextByteLimit()");
    int32_t limit = box->m_visibleTextByteLimit;
    RaisePendingError(L, box, "GetVisibleTextByteLimit");
    return PushInt(L, limit);
}

// ref: FUN_140915b20
int GetNumLetters(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local numLetters = self:GetNumLetters()");
    int32_t letters = 0;
    if (!box->GetNumLetters(&letters)) {
        return 0;
    }
    RaisePendingError(L, box, "GetNumLetters");
    return PushInt(L, letters);
}

// ref: FUN_1408ccbd0
int32_t GetNumberBody(lua_State* L, void* context) {
    int32_t index = 1;
    auto box = static_cast<CSimpleEditBox**>(context);
    *box = BoxSelf(L, &index, "Usage: local number = self:GetNumber()");
    float number = (*box)->GetNumber();
    RaisePendingError(L, *box, "GetNumber");
    return PushFloat(L, number);
}

// ref: FUN_1409174f0
int GetNumber(lua_State* L) {
    CSimpleEditBox* box = nullptr;
    return ProtectedCall(L, GetNumberBody, &box);
}

// ref: FUN_1408cb2d0
int32_t GetDisplayTextBody(lua_State* L, void* context) {
    int32_t index = 1;
    auto box = static_cast<CSimpleEditBox**>(context);
    *box = BoxSelf(L, &index, "Usage: local displayText = self:GetDisplayText()");
    int32_t capacity = (*box)->m_textCapacity + 1;
    char* buffer = static_cast<char*>(SMemAlloc(static_cast<uint64_t>(capacity), 0x1f, ".D", 0, -6, 0));
    int32_t results = 0;
    if ((*box)->GetDisplayText(buffer, capacity)) {
        RaisePendingError(L, *box, "GetDisplayText");
        results = PushStringOrNil(L, buffer);
    }
    SMemFree(buffer, ".D", -6, 0);
    return results;
}

// ref: FUN_14090c5c0
int GetDisplayText(lua_State* L) {
    CSimpleEditBox* box = nullptr;
    return ProtectedCall(L, GetDisplayTextBody, &box);
}

// ref: FUN_14091aa80
int GetText(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local text = self:GetText()");
    const char* text = box->GetText();
    RaisePendingError(L, box, "GetText");
    return PushStringOrNil(L, text);
}

// ref: FUN_14090d330
int GetFont(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local name, fontHeight, flags = self:GetFont()");
    const char* path = "";
    float height = 0.0f;
    uint8_t flags = 0;
    box->GetFont(&path, &height, &flags);
    RaisePendingError(L, box, "GetFont");
    if (path == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, path);
    }
    const COORDINATESPACE& space = g_coordinateSpace;
    lua_pushnumber(L, static_cast<double>((space.m_aspectCompensation * 1024.0f * height) / space.m_diagonalWidth));
    // FUN_1408c6dd0
    PushStringOrNil(L, FontFlagsToString(flags));
    return 3;
}

// ref: FUN_14090e1a0
int GetFontObject(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local font = self:GetFontObject()");
    CSimpleFont* font = box->m_font;
    RaisePendingError(L, box, "GetFontObject");
    return PushObject(L, font);
}

// ref: FUN_14090ef20
int GetHighlightColor(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local r, g, b, a = self:GetHighlightColor()");
    RaisePendingError(L, box, "GetHighlightColor");
    // FUN_1408c6c60
    return PushColor(L, box->m_highlightColor);
}

// ref: FUN_14091b800
int GetTextColor(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local r, g, b, a = self:GetTextColor()");
    // FUN_1424b1b10
    uint32_t color = box->m_font->m_attributes.m_color;
    RaisePendingError(L, box, "GetTextColor");
    return PushColor(L, color);
}

// ref: FUN_140918200
int GetShadowColor(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local r, g, b, a = self:GetShadowColor()");
    // FUN_1424b1ae0
    uint32_t color = box->m_font->m_attributes.m_shadowColor;
    RaisePendingError(L, box, "GetShadowColor");
    return PushColor(L, color);
}

// ref: FUN_140918f80
int GetShadowOffset(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local x, y = self:GetShadowOffset()");
    // FUN_1424b1af0
    const float* offset = box->m_font->m_attributes.m_shadowOffset;
    RaisePendingError(L, box, "GetShadowOffset");
    PushFloat(L, offset[0]);
    PushFloat(L, offset[1]);
    return 2;
}

// ref: FUN_140919d00
int GetSpacing(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local fontHeight = self:GetSpacing()");
    // FUN_1424b1b00
    float spacing = box->m_font->m_attributes.m_spacing;
    RaisePendingError(L, box, "GetSpacing");
    return PushUIUnits(L, spacing);
}

// ref: FUN_140910a20
int GetIndentedWordWrap(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local isIndented = self:GetIndentedWordWrap()");
    // FUN_1424b1850
    bool indented = ((box->m_font->m_attributes.m_stringFlags >> 0x11) & 1) != 0;
    RaisePendingError(L, box, "GetIndentedWordWrap");
    return PushBool(L, indented);
}

// ref: FUN_1409117a0
int GetInputLanguage(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local language = self:GetInputLanguage()");
    const char* language = box->GetInputLanguage();
    RaisePendingError(L, box, "GetInputLanguage");
    return PushStringOrNil(L, language);
}

// ref: FUN_140912520
int GetJustifyH(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local justifyH = self:GetJustifyH()");
    // FUN_1424b1880
    uint32_t justify = box->m_font->m_attributes.m_stringFlags & 7;
    RaisePendingError(L, box, "GetJustifyH");
    return PushJustify(L, justify);
}

// ref: FUN_1409132a0
int GetJustifyV(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local justifyV = self:GetJustifyV()");
    // FUN_1424b1890
    uint32_t justify = box->m_font->m_attributes.m_stringFlags & 0x38;
    RaisePendingError(L, box, "GetJustifyV");
    return PushJustify(L, justify);
}

// ref: FUN_14091c580
int GetTextInsets(lua_State* L) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, "Usage: local left, right, top, bottom = self:GetTextInsets()");
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    box->GetTextInsets(&left, &right, &top, &bottom);
    RaisePendingError(L, box, "GetTextInsets");
    PushUIUnits(L, left);
    PushUIUnits(L, right);
    PushUIUnits(L, top);
    PushUIUnits(L, bottom);
    return 4;
}

// ---- setters ---------------------------------------------------------------------------------

// ref: FUN_140904c20
int AddHistoryLine(lua_State* L) {
    const char* usage = "Usage: self:AddHistoryLine(text)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    const char* text = RequiredText(L, &index, usage);
    box->AddHistoryLine(text);
    return Done(L, box, "AddHistoryLine");
}

// ref: FUN_140921a00
int Insert(lua_State* L) {
    const char* usage = "Usage: self:Insert(text)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    const char* text = RequiredText(L, &index, usage);
    box->InsertScript(text);
    return Done(L, box, "Insert");
}

// ref: FUN_140943d80
int SetText(lua_State* L) {
    const char* usage = "Usage: self:SetText(text)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    const char* text = RequiredText(L, &index, usage);
    box->SetTextScript(text);
    return Done(L, box, "SetText");
}

// ref: FUN_140920a70
int HighlightText(lua_State* L) {
    const char* usage = "Usage: self:HighlightText([start, stop])";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    int32_t start = 0;
    if (Present(L, index)) {
        start = RequiredInt32(L, &index, usage);
    } else {
        index++;
    }
    int32_t stop = -1;
    if (Present(L, index)) {
        stop = RequiredInt32(L, &index, usage);
    }
    box->HighlightText(start, stop);
    return Done(L, box, "HighlightText");
}

template <void (CSimpleEditBox::*Setter)(bool)>
int SetOptionalBool(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    bool value = OptionalFlag(L, &index);
    (box->*Setter)(value);
    return Done(L, box, method);
}

// ref: FUN_14092bc60
int SetAlphabeticOnly(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetAlphabeticOnly>(L, "Usage: self:SetAlphabeticOnly([enabled])", "SetAlphabeticOnly");
}

// ref: FUN_14093a600
int SetMultiLine(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetMultiLine>(L, "Usage: self:SetMultiLine([multiline])", "SetMultiLine");
}

// ref: FUN_14093c1a0
int SetNumeric(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetNumeric>(L, "Usage: self:SetNumeric([isNumeric])", "SetNumeric");
}

// ref: FUN_14093cf50
int SetNumericFullRange(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetNumericFullRange>(L, "Usage: self:SetNumericFullRange([isNumeric])", "SetNumericFullRange");
}

// ref: FUN_14093dd00
int SetPassword(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetPassword>(L, "Usage: self:SetPassword([isPassword])", "SetPassword");
}

// ref: FUN_14093eab0
int SetSecureText(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetSecureText>(L, "Usage: self:SetSecureText([isSecure])", "SetSecureText");
}

// ref: FUN_140936180
int SetIndentedWordWrap(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetIndentedWordWrap>(L, "Usage: self:SetIndentedWordWrap([isIndented])", "SetIndentedWordWrap");
}

// ref: FUN_140930ec0
int SetEnabled(lua_State* L) {
    return SetOptionalBool<&CSimpleEditBox::SetEnabled>(L, "Usage: self:SetEnabled([enabled])", "SetEnabled");
}

template <uint32_t Flag>
int SetFlag(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    bool value = OptionalFlag(L, &index);
    box->m_editFlags = value ? (box->m_editFlags | Flag) : (box->m_editFlags & ~Flag);
    return Done(L, box, method);
}

// ref: FUN_14092ca10 (FUN_1424b5860)
int SetAltArrowKeyMode(lua_State* L) {
    return SetFlag<CSimpleEditBox::EDIT_ALT_ARROW_KEY_MODE>(L, "Usage: self:SetAltArrowKeyMode([altMode])", "SetAltArrowKeyMode");
}

// ref: FUN_14092d7c0 (FUN_1424b5880)
int SetAutoFocus(lua_State* L) {
    return SetFlag<CSimpleEditBox::EDIT_AUTO_FOCUS>(L, "Usage: self:SetAutoFocus([autoFocus])", "SetAutoFocus");
}

// ref: FUN_14092f360 (FUN_1424b5b70)
int SetCountInvisibleLetters(lua_State* L) {
    return SetFlag<CSimpleEditBox::EDIT_COUNT_INVISIBLE_LETTERS>(L, "Usage: self:SetCountInvisibleLetters([countInvisibleLetters])",
        "SetCountInvisibleLetters");
}

// ref: FUN_14092e570
int SetBlinkSpeed(lua_State* L) {
    const char* usage = "Usage: self:SetBlinkSpeed(cursorBlinkSpeedSec)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    box->m_blinkSpeed = RequiredFinite(L, &index, usage);
    return Done(L, box, "SetBlinkSpeed");
}

// ref: FUN_14093b3b0
int SetNumber(lua_State* L) {
    const char* usage = "Usage: self:SetNumber(number)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    float number = RequiredFinite(L, &index, usage);
    box->SetNumber(number);
    return Done(L, box, "SetNumber");
}

// ref: FUN_140930110
int SetCursorPosition(lua_State* L) {
    const char* usage = "Usage: self:SetCursorPosition(cursorPosition)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    int32_t position = RequiredInt32(L, &index, usage);
    box->SetCursorPositionScript(position);
    return Done(L, box, "SetCursorPosition");
}

// ref: FUN_1409353d0
int SetHistoryLines(lua_State* L) {
    const char* usage = "Usage: self:SetHistoryLines(numHistoryLines)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    int32_t lines = RequiredInt32(L, &index, usage);
    box->SetHistoryLines(lines);
    return Done(L, box, "SetHistoryLines");
}

// ref: FUN_140938a90
int SetMaxBytes(lua_State* L) {
    const char* usage = "Usage: self:SetMaxBytes(maxBytes)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    int32_t bytes = RequiredInt32(L, &index, usage);
    box->m_maxBytes = 0 < bytes ? bytes - 1 : -1;
    return Done(L, box, "SetMaxBytes");
}

// ref: FUN_140939850
int SetMaxLetters(lua_State* L) {
    const char* usage = "Usage: self:SetMaxLetters(maxLetters)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    box->m_maxLetters = RequiredInt32(L, &index, usage);
    return Done(L, box, "SetMaxLetters");
}

// ref: FUN_1409468c0
int SetVisibleTextByteLimit(lua_State* L) {
    const char* usage = "Usage: self:SetVisibleTextByteLimit(maxVisibleBytes)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    int32_t limit = RequiredInt32(L, &index, usage);
    box->SetVisibleTextByteLimit(limit);
    return Done(L, box, "SetVisibleTextByteLimit");
}

// ref: FUN_140936f30
int SetJustifyH(lua_State* L) {
    const char* usage = "Usage: self:SetJustifyH(justifyH)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    box->SetJustifyH(justify);
    return Done(L, box, "SetJustifyH");
}

// ref: FUN_140937ce0
int SetJustifyV(lua_State* L) {
    const char* usage = "Usage: self:SetJustifyV(justifyV)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    box->SetJustifyV(justify);
    return Done(L, box, "SetJustifyV");
}

// ref: FUN_140942f30
int SetSpacing(lua_State* L) {
    const char* usage = "Usage: self:SetSpacing(fontHeight)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    float spacing = RequiredUIUnits(L, &index, usage);
    box->SetSpacing(spacing);
    return Done(L, box, "SetSpacing");
}

// ref: FUN_140942050
int SetShadowOffset(lua_State* L) {
    const char* usage = "Usage: self:SetShadowOffset(offset)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    float offset[2] = { 0.0f, 0.0f };
    RequiredPair(L, &index, usage, offset);
    box->SetShadowOffset(offset);
    return Done(L, box, "SetShadowOffset");
}

// ref: FUN_1409458a0
int SetTextInsets(lua_State* L) {
    const char* usage = "Usage: self:SetTextInsets(left, right, top, bottom)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    float left = RequiredUIUnits(L, &index, usage);
    float right = RequiredUIUnits(L, &index, usage);
    float top = RequiredUIUnits(L, &index, usage);
    float bottom = RequiredUIUnits(L, &index, usage);
    box->SetTextInsets(left, right, top, bottom);
    return Done(L, box, "SetTextInsets");
}

// ref: FUN_1409329e0
int SetFont(lua_State* L) {
    const char* usage = "Usage: local success = self:SetFont(fontFile, height, flags)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    const char* path = RequiredText(L, &index, usage);
    // FUN_1408c6720
    float height = 0.0f;
    if (lua_type(L, index) == LUA_TNUMBER) {
        height = CSimpleFont_ConvertHeight(static_cast<float>(lua_tonumber(L, index)));
    }
    index++;
    // FUN_1408c61d0
    uint8_t flags = 0;
    if (lua_type(L, index) == LUA_TSTRING) {
        StringToFontFlags(&flags, lua_tolstring(L, index, nullptr));
    }
    bool set = box->SetFont(path, height, &flags);
    RaisePendingError(L, box, "SetFont");
    return PushBool(L, set);
}

// ref: FUN_140933830
int SetFontObject(lua_State* L) {
    const char* usage = "Usage: self:SetFontObject(font)";
    int32_t index = 1;
    CSimpleEditBox* box = BoxSelf(L, &index, usage);
    // FUN_140a508c0: a font object, or a font's name.
    CSimpleFont* font = nullptr;
    if (lua_type(L, index) == LUA_TSTRING) {
        font = CSimpleFont::GetFont(lua_tolstring(L, index, nullptr), false);
    } else if (lua_type(L, index) == LUA_TTABLE) {
        const char* error = nullptr;
        font = static_cast<CSimpleFont*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFont::GetObjectType(), index, false, false));
    }
    if (font == nullptr) {
        luaL_argerror(L, index, usage);
        return 0;
    }
    box->SetFontObject(font);
    return Done(L, box, "SetFontObject");
}

struct COLORARGS {
    CSimpleEditBox* m_box;
    uint32_t m_rgb;
    uint8_t m_alpha[2];
};

template <void (CSimpleEditBox::*Setter)(const uint32_t*, const uint8_t*)>
int32_t SetColorBody(lua_State* L, void* context, const char* usage, const char* method) {
    auto args = static_cast<COLORARGS*>(context);
    int32_t index = 1;
    args->m_box = BoxSelf(L, &index, usage);
    args->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, args->m_alpha);
    (args->m_box->*Setter)(&args->m_rgb, args->m_alpha);
    RaisePendingError(L, args->m_box, method);
    return 0;
}

// ref: FUN_1408cd8a0
int32_t SetHighlightColorBody(lua_State* L, void* context) {
    return SetColorBody<&CSimpleEditBox::SetHighlightColor>(L, context, "Usage: self:SetHighlightColor(color [, a])", "SetHighlightColor");
}

// ref: FUN_1408ce710
int32_t SetShadowColorBody(lua_State* L, void* context) {
    return SetColorBody<&CSimpleEditBox::SetShadowColor>(L, context, "Usage: self:SetShadowColor(color [, a])", "SetShadowColor");
}

// ref: FUN_1408cf580
int32_t SetTextColorBody(lua_State* L, void* context) {
    return SetColorBody<&CSimpleEditBox::SetTextColor>(L, context, "Usage: self:SetTextColor(color [, a])", "SetTextColor");
}

// ref: FUN_1409346a0
int SetHighlightColor(lua_State* L) {
    COLORARGS args = {};
    return ProtectedCall(L, SetHighlightColorBody, &args);
}

// ref: FUN_140941320
int SetShadowColor(lua_State* L) {
    COLORARGS args = {};
    return ProtectedCall(L, SetShadowColorBody, &args);
}

// ref: FUN_140944b70
int SetTextColor(lua_State* L) {
    COLORARGS args = {};
    return ProtectedCall(L, SetTextColorBody, &args);
}

// DAT_142cf7d20
const luaL_Reg s_editBoxMethods[] = {
    { "AddHistoryLine", AddHistoryLine },
    { "ClearFocus", ClearFocus },
    { "ClearHighlightText", ClearHighlightText },
    { "ClearHistory", ClearHistory },
    { "Disable", Disable },
    { "Enable", Enable },
    { "GetAltArrowKeyMode", GetAltArrowKeyMode },
    { "GetBlinkSpeed", GetBlinkSpeed },
    { "GetCursorPosition", GetCursorPosition },
    { "GetDisplayText", GetDisplayText },
    { "GetFont", GetFont },
    { "GetFontObject", GetFontObject },
    { "GetHighlightColor", GetHighlightColor },
    { "GetHistoryLines", GetHistoryLines },
    { "GetIndentedWordWrap", GetIndentedWordWrap },
    { "GetInputLanguage", GetInputLanguage },
    { "GetJustifyH", GetJustifyH },
    { "GetJustifyV", GetJustifyV },
    { "GetMaxBytes", GetMaxBytes },
    { "GetMaxLetters", GetMaxLetters },
    { "GetNumLetters", GetNumLetters },
    { "GetNumber", GetNumber },
    { "GetShadowColor", GetShadowColor },
    { "GetShadowOffset", GetShadowOffset },
    { "GetSpacing", GetSpacing },
    { "GetText", GetText },
    { "GetTextColor", GetTextColor },
    { "GetTextInsets", GetTextInsets },
    { "GetUTF8CursorPosition", GetUTF8CursorPosition },
    { "GetVisibleTextByteLimit", GetVisibleTextByteLimit },
    { "HasFocus", HasFocus },
    { "HasText", HasText },
    { "HighlightText", HighlightText },
    { "Insert", Insert },
    { "IsAlphabeticOnly", IsAlphabeticOnly },
    { "IsAutoFocus", IsAutoFocus },
    { "IsCountInvisibleLetters", IsCountInvisibleLetters },
    { "IsEnabled", IsEnabled },
    { "IsInIMECompositionMode", IsInIMECompositionMode },
    { "IsMultiLine", IsMultiLine },
    { "IsNumeric", IsNumeric },
    { "IsNumericFullRange", IsNumericFullRange },
    { "IsPassword", IsPassword },
    { "IsSecureText", IsSecureText },
    { "ResetInputMode", ResetInputMode },
    { "SetAlphabeticOnly", SetAlphabeticOnly },
    { "SetAltArrowKeyMode", SetAltArrowKeyMode },
    { "SetAutoFocus", SetAutoFocus },
    { "SetBlinkSpeed", SetBlinkSpeed },
    { "SetCountInvisibleLetters", SetCountInvisibleLetters },
    { "SetCursorPosition", SetCursorPosition },
    { "SetEnabled", SetEnabled },
    { "SetFocus", SetFocus },
    { "SetFont", SetFont },
    { "SetFontObject", SetFontObject },
    { "SetHighlightColor", SetHighlightColor },
    { "SetHistoryLines", SetHistoryLines },
    { "SetIndentedWordWrap", SetIndentedWordWrap },
    { "SetJustifyH", SetJustifyH },
    { "SetJustifyV", SetJustifyV },
    { "SetMaxBytes", SetMaxBytes },
    { "SetMaxLetters", SetMaxLetters },
    { "SetMultiLine", SetMultiLine },
    { "SetNumber", SetNumber },
    { "SetNumeric", SetNumeric },
    { "SetNumericFullRange", SetNumericFullRange },
    { "SetPassword", SetPassword },
    { "SetSecureText", SetSecureText },
    { "SetSecurityDisablePaste", SetSecurityDisablePaste },
    { "SetSecurityDisableSetText", SetSecurityDisableSetText },
    { "SetShadowColor", SetShadowColor },
    { "SetShadowOffset", SetShadowOffset },
    { "SetSpacing", SetSpacing },
    { "SetText", SetText },
    { "SetTextColor", SetTextColor },
    { "SetTextInsets", SetTextInsets },
    { "SetVisibleTextByteLimit", SetVisibleTextByteLimit },
    { "ToggleInputLanguage", ToggleInputLanguage },
};
static_assert(sizeof(s_editBoxMethods) / sizeof(s_editBoxMethods[0]) == 0x4e, "the edit box table has 0x4e methods");

}

// ref: FUN_140a50f50
void CSimpleEditBox_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_editBoxMethods, 0x4e);
}
