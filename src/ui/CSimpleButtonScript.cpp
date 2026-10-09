// The Lua methods of buttons (CSimpleButton's table 0x142cf77e0, 0x2d methods).

#include <cstring>

#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptEvents.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleButton.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/TextureAtlas.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

void CSimpleFrame_FillScriptMethods(lua_State* L);

namespace {

using namespace ScriptMethod;

CSimpleButton* ButtonSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleButton*>(Self(L, CSimpleButton::GetObjectType(), index, usage));
}

bool Present(lua_State* L, int32_t index) {
    return lua_type(L, index) >= 1;
}

// FUN_1408a7520: a texture asset argument: nil (none), a FileDataID, a path or atlas name, or
// a texture object. False when the value is none of those.
bool AssetValue(lua_State* L, int32_t index, TEXTURESOURCE* source) {
    int32_t type = lua_type(L, index);
    if (type == LUA_TNIL) {
        *source = TEXTURESOURCE::FromTexture(nullptr);
        return true;
    }
    if (type == LUA_TNUMBER) {
        *source = TEXTURESOURCE::FromFileId(static_cast<int32_t>(lua_tonumber(L, index)));
        return true;
    }
    if (type == LUA_TSTRING) {
        *source = TEXTURESOURCE::FromString(lua_tolstring(L, index, nullptr));
        return true;
    }
    if (type != LUA_TTABLE) {
        return false;
    }
    const char* error = nullptr;
    auto texture = static_cast<CSimpleTexture*>(
        FrameScript_Object::GetObjectFromLua(&error, L, CSimpleTexture::GetObjectType(), index, false, false));
    if (texture == nullptr) {
        return false;
    }
    *source = TEXTURESOURCE::FromTexture(texture);
    return true;
}

// FUN_1407ff990: the asset argument at *index, required.
TEXTURESOURCE RequiredAsset(lua_State* L, int32_t* index, const char* usage) {
    TEXTURESOURCE source = TEXTURESOURCE::FromFileId(0);
    if (!Present(L, *index)) {
        luaL_error(L, usage);
        return source;
    }
    if (!AssetValue(L, *index, &source)) {
        luaL_argerror(L, *index, usage);
        return source;
    }
    (*index)++;
    return source;
}

// FUN_1407d1140: an atlas name's member (0 for none).
int32_t AtlasArgument(lua_State* L, int32_t* index) {
    int32_t member = 0;
    if (Present(L, *index)) {
        TEXTUREATLASSCALE scale = { 0, 0 };
        TextureAtlasFindByName(&member, lua_tolstring(L, *index, nullptr), &scale);
    }
    (*index)++;
    return member;
}

// The atlas argument the Set*Atlas methods read: required, a string (or a number).
int32_t RequiredAtlas(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
        return 0;
    }
    if (!lua_isstring(L, *index)) {
        luaL_argerror(L, *index, usage);
        return 0;
    }
    return AtlasArgument(L, index);
}

// FUN_1407ff1e0: an optional blend mode name: {mode, set}.
void OptionalBlendMode(lua_State* L, int32_t* index, const char* usage, uint8_t* blend) {
    blend[0] = 0;
    blend[1] = 0;
    if (!Present(L, *index)) {
        (*index)++;
        return;
    }
    uint8_t mode = 0;
    if (lua_type(L, *index) != LUA_TSTRING || !StringToBlendMode(lua_tolstring(L, *index, nullptr), &mode)) {
        luaL_argerror(L, *index, usage);
        return;
    }
    blend[0] = mode;
    blend[1] = 1;
    (*index)++;
}

// FUN_1407fe210: the click names from *index to the end, each a mask.
void ClickMasks(lua_State* L, int32_t* index, const char* usage, uint64_t* masks, uint64_t* count) {
    *count = 0;
    int32_t top = lua_gettop(L);
    while (*index <= top && *count < 64) {
        uint64_t mask = 0;
        if (!Present(L, *index)) {
            luaL_error(L, usage);
        } else if (lua_type(L, *index) != LUA_TSTRING || (mask = ButtonClickMask(lua_tolstring(L, *index, nullptr))) == 0) {
            luaL_argerror(L, *index, usage);
        } else {
            (*index)++;
        }
        masks[(*count)++] = mask;
    }
}

// FUN_140c7d5c0: a button state's name.
bool ButtonStateFromName(const char* name, uint64_t length, int32_t* state) {
    uint64_t id = FrameScriptEvents::GetEventID(name, length);
    if (id == 0x01aa82d8bb5ddbb1ull) {
        *state = CSimpleButton::BUTTON_DISABLED;
        return true;
    }
    if (id == 0x16d14279d85e5306ull) {
        *state = CSimpleButton::BUTTON_NORMAL;
        return true;
    }
    if (id == 0xec4325a68fe39d6cull) {
        *state = CSimpleButton::BUTTON_PUSHED;
        return true;
    }
    return false;
}

const char* ButtonStateName(int32_t state) {
    switch (state) {
        case CSimpleButton::BUTTON_DISABLED:
            return "DISABLED";
        case CSimpleButton::BUTTON_NORMAL:
            return "NORMAL";
        case CSimpleButton::BUTTON_PUSHED:
            return "PUSHED";
        default:
            return nullptr;
    }
}

int32_t Finish(lua_State* L, CSimpleButton* button, const char* method, int32_t results) {
    RaisePendingError(L, button, method);
    return results;
}

// ---- clears -----------------------------------------------------------------------------------

// ref: FUN_140863fb0
int ClearDisabledTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:ClearDisabledTexture()");
    button->ClearDisabledTexture();
    return Finish(L, button, "ClearDisabledTexture", 0);
}

// ref: FUN_140864d20
int ClearHighlightTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:ClearHighlightTexture()");
    button->ClearHighlightTexture();
    return Finish(L, button, "ClearHighlightTexture", 0);
}

// ref: FUN_140865a90
int ClearNormalTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:ClearNormalTexture()");
    button->ClearNormalTexture();
    return Finish(L, button, "ClearNormalTexture", 0);
}

// ref: FUN_140866800
int ClearPushedTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:ClearPushedTexture()");
    button->ClearPushedTexture();
    return Finish(L, button, "ClearPushedTexture", 0);
}

// ---- clicks and state ---------------------------------------------------------------------

// ref: FUN_140867570
int Click(lua_State* L) {
    const char* usage = "Usage: self:Click([button, isDown])";
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    const char* which = "LeftButton";
    if (Present(L, index)) {
        if (!lua_isstring(L, index)) {
            luaL_argerror(L, index, usage);
        } else {
            which = lua_tolstring(L, index, nullptr);
        }
    }
    index++;
    bool down = false;
    if (Present(L, index)) {
        down = lua_toboolean(L, index) != 0;
    }
    button->Click(which, down);
    return Finish(L, button, "Click", 0);
}

// ref: FUN_1408683a0
int Disable(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:Disable()");
    if (!CanChangeProtected(button)) {
        return 0;
    }
    button->SetEnabled(false);
    return Finish(L, button, "Disable", 0);
}

// ref: FUN_140869110
int Enable(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:Enable()");
    if (!CanChangeProtected(button)) {
        return 0;
    }
    button->SetEnabled(true);
    return Finish(L, button, "Enable", 0);
}

// ref: FUN_14087ca20
int SetEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:SetEnabled([enabled])");
    bool enabled = false;
    if (Present(L, index)) {
        enabled = lua_toboolean(L, index) != 0;
    }
    if (!CanChangeProtected(button)) {
        return 0;
    }
    button->SetEnabled(enabled);
    return Finish(L, button, "SetEnabled", 0);
}

// ref: FUN_140875d10
int IsEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local isEnabled = self:IsEnabled()");
    bool enabled = (button->m_state & 0xf) != CSimpleButton::BUTTON_DISABLED;
    RaisePendingError(L, button, "IsEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_140869e80
int GetButtonState(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local buttonState = self:GetButtonState()");
    int32_t state = static_cast<int32_t>(button->m_state << 28) >> 28;
    RaisePendingError(L, button, "GetButtonState");
    return PushStringOrNil(L, ButtonStateName(state));
}

// ref: FUN_1408792c0
int SetButtonState(lua_State* L) {
    const char* usage = "Usage: self:SetButtonState(buttonState [, lock])";
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    int32_t state = CSimpleButton::BUTTON_DISABLED;
    if (!Present(L, index)) {
        luaL_error(L, usage);
    } else {
        size_t length = 0;
        const char* name = lua_type(L, index) == LUA_TSTRING ? lua_tolstring(L, index, &length) : nullptr;
        if (name == nullptr || !ButtonStateFromName(name, length, &state)) {
            luaL_argerror(L, index, usage);
        } else {
            index++;
        }
    }
    bool locked = false;
    if (Present(L, index)) {
        locked = lua_toboolean(L, index) != 0;
    }
    button->SetButtonState(state, locked);
    return Finish(L, button, "SetButtonState", 0);
}

// ref: FUN_140876a90
int LockHighlight(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:LockHighlight()");
    button->LockHighlight(true);
    return Finish(L, button, "LockHighlight", 0);
}

// ref: FUN_1408896b0
int UnlockHighlight(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: self:UnlockHighlight()");
    button->LockHighlight(false);
    return Finish(L, button, "UnlockHighlight", 0);
}

// ref: FUN_140880cc0
int SetHighlightLocked(lua_State* L) {
    const char* usage = "Usage: self:SetHighlightLocked(locked)";
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    bool locked = false;
    if (!Present(L, index)) {
        luaL_error(L, usage);
    } else {
        locked = lua_toboolean(L, index) != 0;
    }
    button->LockHighlight(locked);
    return Finish(L, button, "SetHighlightLocked", 0);
}

// ref: FUN_14086f030
int GetMotionScriptsWhileDisabled(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local motionScriptsWhileDisabled = self:GetMotionScriptsWhileDisabled()");
    bool enabled = button->GetMotionScriptsWhileDisabled();
    RaisePendingError(L, button, "GetMotionScriptsWhileDisabled");
    return PushBool(L, enabled);
}

// ref: FUN_1408827c0
int SetMotionScriptsWhileDisabled(lua_State* L) {
    const char* usage = "Usage: self:SetMotionScriptsWhileDisabled(motionScriptsWhileDisabled)";
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    bool enabled = false;
    if (!Present(L, index)) {
        luaL_error(L, usage);
    } else {
        enabled = lua_toboolean(L, index) != 0;
    }
    button->SetMotionScriptsWhileDisabled(enabled);
    return Finish(L, button, "SetMotionScriptsWhileDisabled", 0);
}

struct CLICKARGS {
    CSimpleButton* m_button;
    uint64_t m_masks[64];
    uint64_t m_count;
};

// ref: FUN_14080bf50
int32_t RegisterForClicksBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:RegisterForClicks(buttons)";
    auto args = static_cast<CLICKARGS*>(context);
    int32_t index = 1;
    args->m_button = ButtonSelf(L, &index, usage);
    ClickMasks(L, &index, usage, args->m_masks, &args->m_count);
    args->m_button->RegisterForClicks(args->m_masks, args->m_count);
    RaisePendingError(L, args->m_button, "RegisterForClicks");
    return 0;
}

// ref: FUN_140877800
int RegisterForClicks(lua_State* L) {
    CLICKARGS args = {};
    return ProtectedCall(L, RegisterForClicksBody, &args);
}

// ref: FUN_14080cc20
int32_t RegisterForMouseBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:RegisterForMouse(buttons)";
    auto args = static_cast<CLICKARGS*>(context);
    int32_t index = 1;
    args->m_button = ButtonSelf(L, &index, usage);
    ClickMasks(L, &index, usage, args->m_masks, &args->m_count);
    args->m_button->RegisterForMouse(args->m_masks, args->m_count);
    RaisePendingError(L, args->m_button, "RegisterForMouse");
    return 0;
}

// ref: FUN_140878560
int RegisterForMouse(lua_State* L) {
    CLICKARGS args = {};
    return ProtectedCall(L, RegisterForMouseBody, &args);
}

// ---- textures --------------------------------------------------------------------------------

// ref: FUN_14086b9e0 (FUN_1411e6cc0)
int GetDisabledTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local texture = self:GetDisabledTexture()");
    CSimpleTexture* texture = button->m_stateTextures[CSimpleButton::BUTTON_DISABLED];
    RaisePendingError(L, button, "GetDisabledTexture");
    return PushObject(L, texture);
}

// ref: FUN_140870b30
int GetNormalTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local texture = self:GetNormalTexture()");
    CSimpleTexture* texture = button->GetNormalTexture();
    RaisePendingError(L, button, "GetNormalTexture");
    return PushObject(L, texture);
}

// ref: FUN_1408726f0
int GetPushedTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local texture = self:GetPushedTexture()");
    CSimpleTexture* texture = button->GetPushedTexture();
    RaisePendingError(L, button, "GetPushedTexture");
    return PushObject(L, texture);
}

// ref: FUN_14086e2b0
int GetHighlightTexture(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local texture = self:GetHighlightTexture()");
    CSimpleTexture* texture = button->m_highlightTexture;
    RaisePendingError(L, button, "GetHighlightTexture");
    return PushObject(L, texture);
}

struct ASSETARGS {
    CSimpleButton* m_button;
    TEXTURESOURCE m_source;
    uint8_t m_blend[2];
};

template <int32_t State>
int32_t SetStateTextureBody(lua_State* L, void* context, const char* usage, const char* method) {
    auto args = static_cast<ASSETARGS*>(context);
    int32_t index = 1;
    args->m_button = ButtonSelf(L, &index, usage);
    args->m_source = RequiredAsset(L, &index, usage);
    args->m_button->SetStateTexture(State, &args->m_source);
    RaisePendingError(L, args->m_button, method);
    return 0;
}

// ref: FUN_14080d8f0 (FUN_1424ac130)
int32_t SetDisabledTextureBody(lua_State* L, void* context) {
    return SetStateTextureBody<CSimpleButton::BUTTON_DISABLED>(L, context, "Usage: self:SetDisabledTexture(asset)", "SetDisabledTexture");
}

// ref: FUN_14080ffc0 (FUN_1424ac700)
int32_t SetNormalTextureBody(lua_State* L, void* context) {
    return SetStateTextureBody<CSimpleButton::BUTTON_NORMAL>(L, context, "Usage: self:SetNormalTexture(asset)", "SetNormalTexture");
}

// ref: FUN_140810c90 (FUN_1424ac740)
int32_t SetPushedTextureBody(lua_State* L, void* context) {
    return SetStateTextureBody<CSimpleButton::BUTTON_PUSHED>(L, context, "Usage: self:SetPushedTexture(asset)", "SetPushedTexture");
}

// ref: FUN_14087bd00
int SetDisabledTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetDisabledTextureBody, &args);
}

// ref: FUN_1408850e0
int SetNormalTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetNormalTextureBody, &args);
}

// ref: FUN_140887ba0
int SetPushedTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetPushedTextureBody, &args);
}

// ref: FUN_14080f2c0
int32_t SetHighlightTextureBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetHighlightTexture(asset [, blendMode])";
    auto args = static_cast<ASSETARGS*>(context);
    int32_t index = 1;
    args->m_button = ButtonSelf(L, &index, usage);
    args->m_source = RequiredAsset(L, &index, usage);
    OptionalBlendMode(L, &index, usage, args->m_blend);
    args->m_button->SetHighlight(&args->m_source, args->m_blend);
    RaisePendingError(L, args->m_button, "SetHighlightTexture");
    return 0;
}

// ref: FUN_140881a90
int SetHighlightTexture(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetHighlightTextureBody, &args);
}

// ref: FUN_14080e5c0 (FUN_1424ac630)
int32_t SetHighlightAtlasBody(lua_State* L, void* context) {
    const char* usage = "Usage: self:SetHighlightAtlas(atlas [, blendMode])";
    auto args = static_cast<ASSETARGS*>(context);
    int32_t index = 1;
    args->m_button = ButtonSelf(L, &index, usage);
    int32_t member = RequiredAtlas(L, &index, usage);
    OptionalBlendMode(L, &index, usage, args->m_blend);
    args->m_source = TEXTURESOURCE::FromAtlas(member);
    args->m_button->SetHighlight(&args->m_source, args->m_blend);
    RaisePendingError(L, args->m_button, "SetHighlightAtlas");
    return 0;
}

// ref: FUN_14087f2e0
int SetHighlightAtlas(lua_State* L) {
    ASSETARGS args = {};
    return ProtectedCall(L, SetHighlightAtlasBody, &args);
}

template <int32_t State>
int SetStateAtlas(lua_State* L, const char* usage, const char* method) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    int32_t member = RequiredAtlas(L, &index, usage);
    TEXTURESOURCE source = TEXTURESOURCE::FromAtlas(member);
    button->SetStateTexture(State, &source);
    return Finish(L, button, method, 0);
}

// ref: FUN_14087a1b0 (FUN_1424ac100)
int SetDisabledAtlas(lua_State* L) {
    return SetStateAtlas<CSimpleButton::BUTTON_DISABLED>(L, "Usage: self:SetDisabledAtlas(atlas)", "SetDisabledAtlas");
}

// ref: FUN_140883590 (FUN_1424ac6d0)
int SetNormalAtlas(lua_State* L) {
    return SetStateAtlas<CSimpleButton::BUTTON_NORMAL>(L, "Usage: self:SetNormalAtlas(atlas)", "SetNormalAtlas");
}

// ref: FUN_140885e00 (FUN_1424ac710)
int SetPushedAtlas(lua_State* L) {
    return SetStateAtlas<CSimpleButton::BUTTON_PUSHED>(L, "Usage: self:SetPushedAtlas(atlas)", "SetPushedAtlas");
}

// ---- text and fonts ------------------------------------------------------------------------

// ref: FUN_14086c760
int GetFontString(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local fontString = self:GetFontString()");
    CSimpleFontString* text = button->m_text;
    RaisePendingError(L, button, "GetFontString");
    return PushObject(L, text);
}

// ref: FUN_14087d7e0
int SetFontString(lua_State* L) {
    const char* usage = "Usage: self:SetFontString(fontString)";
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    CSimpleFontString* text = nullptr;
    if (!Present(L, index)) {
        luaL_error(L, usage);
    } else {
        // FUN_1407fdcd0
        text = static_cast<CSimpleFontString*>(Self(L, CSimpleFontString::GetObjectType(), &index, usage));
    }
    button->SetFontString(text);
    return Finish(L, button, "SetFontString", 0);
}

// ref: FUN_140873470
int GetText(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local text = self:GetText()");
    const char* text = nullptr;
    if (button->m_text != nullptr && button->m_text->m_text != nullptr && button->m_text->m_text[0] != '\0') {
        text = button->m_text->m_text;
    }
    RaisePendingError(L, button, "GetText");
    return PushStringOrNil(L, text);
}

// ref: FUN_1408888c0
int SetText(lua_State* L) {
    const char* usage = "Usage: self:SetText([text])";
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    const char* text = "";
    if (Present(L, index)) {
        if (!lua_isstring(L, index)) {
            luaL_argerror(L, index, usage);
        } else {
            text = lua_tolstring(L, index, nullptr);
        }
    }
    button->SetText(text);
    return Finish(L, button, "SetText", 0);
}

// ref: FUN_1424c8ff0: self for the methods shared with other button kinds.
CSimpleButton* SharedSelf(lua_State* L) {
    const char* error = nullptr;
    auto button = static_cast<CSimpleButton*>(
        FrameScript_Object::GetObjectFromLua(&error, L, CSimpleButton::GetObjectType(), 1, false, false));
    if (error != nullptr) {
        luaL_error(L, error);
        return nullptr;
    }
    return button;
}

// ref: FUN_1424c91d0
int SetFormattedText(lua_State* L) {
    CSimpleButton* button = SharedSelf(L);
    if (button != nullptr) {
        char buffer[0x1000];
        button->SetText(FrameScript_FormatString(L, 2, buffer, sizeof(buffer)));
    }
    return 0;
}

// The font object argument: a font's name or a font object.
CSimpleFont* FontArgument(lua_State* L) {
    int32_t type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        return CSimpleFont::GetFont(lua_tolstring(L, 2, nullptr), false);
    }
    if (type != LUA_TTABLE) {
        return nullptr;
    }
    const char* error = nullptr;
    return static_cast<CSimpleFont*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFont::GetObjectType(), 2, false, false));
}

// FUN_1424c90d0 and its siblings.
int SetFontObjectFor(lua_State* L, CSimpleFont* CSimpleButton::*field, const char* method) {
    CSimpleButton* button = SharedSelf(L);
    if (button == nullptr) {
        return 0;
    }
    CSimpleFont* font = FontArgument(L);
    if (font != nullptr && font->IsObjectType(CSimpleFont::GetObjectType())) {
        button->*field = font;
        button->UpdateTextState(static_cast<int32_t>(button->m_state << 28) >> 28);
        return 0;
    }
    FrameScript_ObjectError(button, method, "Usage: (\"fontname\" or fontObject)");
    return 0;
}

// ref: FUN_1424c90d0
int SetDisabledFontObject(lua_State* L) {
    return SetFontObjectFor(L, &CSimpleButton::m_disabledFont, "SetDisabledFontObject");
}

// ref: FUN_1424c9240
int SetHighlightFontObject(lua_State* L) {
    return SetFontObjectFor(L, &CSimpleButton::m_highlightFont, "SetHighlightFontObject");
}

// ref: FUN_1424c9340
int SetNormalFontObject(lua_State* L) {
    return SetFontObjectFor(L, &CSimpleButton::m_normalFont, "SetNormalFontObject");
}

int PushFont(lua_State* L, CSimpleFont* font) {
    return PushObject(L, font);
}

// ref: FUN_14086ac60
int GetDisabledFontObject(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local font = self:GetDisabledFontObject()");
    CSimpleFont* font = button->m_disabledFont;
    RaisePendingError(L, button, "GetDisabledFontObject");
    return PushFont(L, font);
}

// ref: FUN_14086d530
int GetHighlightFontObject(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local font = self:GetHighlightFontObject()");
    CSimpleFont* font = button->m_highlightFont;
    RaisePendingError(L, button, "GetHighlightFontObject");
    return PushFont(L, font);
}

// ref: FUN_14086fdb0
int GetNormalFontObject(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local font = self:GetNormalFontObject()");
    CSimpleFont* font = button->m_normalFont;
    RaisePendingError(L, button, "GetNormalFontObject");
    return PushFont(L, font);
}

// ref: FUN_140874210
int GetTextHeight(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local height = self:GetTextHeight()");
    float height = button->GetTextHeight();
    RaisePendingError(L, button, "GetTextHeight");
    return PushUIUnits(L, height);
}

// ref: FUN_140874f90
int GetTextWidth(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local width = self:GetTextWidth()");
    float width = button->GetTextWidth();
    RaisePendingError(L, button, "GetTextWidth");
    return PushUIUnits(L, width);
}

// ref: FUN_1408718b0
int GetPushedTextOffset(lua_State* L) {
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, "Usage: local offsetX, offsetY = self:GetPushedTextOffset()");
    float x = button->m_pushedTextOffset[0];
    float y = button->m_pushedTextOffset[1];
    RaisePendingError(L, button, "GetPushedTextOffset");
    return PushUIUnits(L, x) + PushUIUnits(L, y);
}

// ref: FUN_140886ca0
int SetPushedTextOffset(lua_State* L) {
    const char* usage = "Usage: self:SetPushedTextOffset(offsetX, offsetY)";
    int32_t index = 1;
    CSimpleButton* button = ButtonSelf(L, &index, usage);
    float x = RequiredUIUnits(L, &index, usage);
    float y = RequiredUIUnits(L, &index, usage);
    if ((button->m_state & 0xf) != CSimpleButton::BUTTON_PUSHED) {
        button->m_pushedTextOffset[0] = x;
        button->m_pushedTextOffset[1] = y;
    }
    return Finish(L, button, "SetPushedTextOffset", 0);
}

// DAT_142cf77e0
const luaL_Reg s_buttonMethods[] = {
    { "ClearDisabledTexture", ClearDisabledTexture },
    { "ClearHighlightTexture", ClearHighlightTexture },
    { "ClearNormalTexture", ClearNormalTexture },
    { "ClearPushedTexture", ClearPushedTexture },
    { "Click", Click },
    { "Disable", Disable },
    { "Enable", Enable },
    { "GetButtonState", GetButtonState },
    { "GetDisabledFontObject", GetDisabledFontObject },
    { "GetDisabledTexture", GetDisabledTexture },
    { "GetFontString", GetFontString },
    { "GetHighlightFontObject", GetHighlightFontObject },
    { "GetHighlightTexture", GetHighlightTexture },
    { "GetMotionScriptsWhileDisabled", GetMotionScriptsWhileDisabled },
    { "GetNormalFontObject", GetNormalFontObject },
    { "GetNormalTexture", GetNormalTexture },
    { "GetPushedTextOffset", GetPushedTextOffset },
    { "GetPushedTexture", GetPushedTexture },
    { "GetText", GetText },
    { "GetTextHeight", GetTextHeight },
    { "GetTextWidth", GetTextWidth },
    { "IsEnabled", IsEnabled },
    { "LockHighlight", LockHighlight },
    { "RegisterForClicks", RegisterForClicks },
    { "RegisterForMouse", RegisterForMouse },
    { "SetButtonState", SetButtonState },
    { "SetDisabledAtlas", SetDisabledAtlas },
    { "SetDisabledFontObject", SetDisabledFontObject },
    { "SetDisabledTexture", SetDisabledTexture },
    { "SetEnabled", SetEnabled },
    { "SetFontString", SetFontString },
    { "SetFormattedText", SetFormattedText },
    { "SetHighlightAtlas", SetHighlightAtlas },
    { "SetHighlightFontObject", SetHighlightFontObject },
    { "SetHighlightLocked", SetHighlightLocked },
    { "SetHighlightTexture", SetHighlightTexture },
    { "SetMotionScriptsWhileDisabled", SetMotionScriptsWhileDisabled },
    { "SetNormalAtlas", SetNormalAtlas },
    { "SetNormalFontObject", SetNormalFontObject },
    { "SetNormalTexture", SetNormalTexture },
    { "SetPushedAtlas", SetPushedAtlas },
    { "SetPushedTextOffset", SetPushedTextOffset },
    { "SetPushedTexture", SetPushedTexture },
    { "SetText", SetText },
    { "UnlockHighlight", UnlockHighlight },
};
static_assert(sizeof(s_buttonMethods) / sizeof(s_buttonMethods[0]) == 0x2d, "the button table has 0x2d methods");

}

// ref: FUN_1408a8460
void CSimpleButton_FillScriptMethods(lua_State* L) {
    CSimpleFrame_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_buttonMethods, sizeof(s_buttonMethods) / sizeof(s_buttonMethods[0]));
}
