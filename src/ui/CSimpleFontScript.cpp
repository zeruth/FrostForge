// The Lua methods of font objects (the object methods 0x142cf8be0 and the Font methods
// 0x142cf8200). A font is passed as itself or by name.

#include "ui/CSimpleFont.hpp"

#include "base/Coordinate.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

void CScriptObject_FillObjectScriptMethods(lua_State* L);

// The file, height and flags (FUN_14062baa0).
// ref: FUN_14062baa0
bool FontSetFont(CSimpleFont* font, const char* path, float height, const uint8_t* flags) {
    if (!(0.0f < height)) {
        FrameScript_ActionError("Invalid fontHeight: %f, height must be > 0", static_cast<double>(height));
        return false;
    }
    if (!font->m_attributes.SetFont(path, height, flags)) {
        return false;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FONT;
    font->UpdateFontables();
    return true;
}

namespace {

using namespace ScriptMethod;

// A font at `index`: the object, or the named font (FUN_140a508c0).
CSimpleFont* FontAt(lua_State* L, int32_t index) {
    int32_t type = lua_type(L, index);
    if (type == LUA_TSTRING) {
        return CSimpleFont::GetFont(lua_tostring(L, index), false);
    }
    if (type == LUA_TTABLE) {
        const char* error = nullptr;
        return static_cast<CSimpleFont*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFont::GetObjectType(), index, false, false));
    }
    return nullptr;
}

// Self for the Font methods: the font, or the usage error.
// ref: FUN_1408c6090
CSimpleFont* FontSelf(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return nullptr;
    }
    CSimpleFont* font = FontAt(L, *index);
    if (font == nullptr) {
        luaL_argerror(L, *index, usage);
        return nullptr;
    }
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return nullptr;
    }
    (*index)++;
    return font;
}

// The font for the methods that report through the object error (FUN_1424ca050).
CSimpleFont* FontFromLua(lua_State* L, int32_t index) {
    const char* error = nullptr;
    auto font = static_cast<CSimpleFont*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFont::GetObjectType(), index, false, false));
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    return font;
}

uint32_t RequiredRGB(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return 0xff000000;
    }
    if (!lua_isnumber(L, *index) || !lua_isnumber(L, *index + 1) || !lua_isnumber(L, *index + 2)) {
        luaL_argerror(L, *index, usage);
        return 0xff000000;
    }
    if (!lua_checkstack(L, 3)) {
        luaL_error(L, "stack overflow");
        return 0xff000000;
    }
    auto channel = [](double value) {
        float f = static_cast<float>(value);
        if (f < 0.0f) {
            f = 0.0f;
        } else if (1.0f <= f) {
            f = 1.0f;
        }
        return static_cast<uint32_t>(static_cast<int64_t>(f * 255.0f + 0.5f)) & 0xff;
    };
    uint32_t b = channel(lua_tonumber(L, *index + 2));
    uint32_t g = channel(lua_tonumber(L, *index + 1));
    uint32_t r = channel(lua_tonumber(L, *index));
    *index += 3;
    return ((r | 0xffffff00u) << 8 | g) << 8 | b;
}

// The alpha byte a colour gets: the optional fourth number, or opaque.
uint8_t OptionalAlpha(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        (*index)++;
        return 0xff;
    }
    if (lua_type(L, *index) != LUA_TNUMBER) {
        luaL_argerror(L, *index, usage);
        return 0xff;
    }
    float value = static_cast<float>(lua_tonumber(L, *index));
    if (value < 0.0f || 1.0f < value) {
        luaL_argerror(L, *index, usage);
        return 0xff;
    }
    (*index)++;
    return static_cast<uint8_t>(static_cast<int32_t>(value * 255.0f + 0.5f));
}

uint32_t RequiredJustify(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return 1;
    }
    uint32_t justify = 0;
    if (lua_type(L, *index) != LUA_TSTRING || !StringToJustify(lua_tostring(L, *index), &justify)) {
        luaL_argerror(L, *index, usage);
        return 1;
    }
    (*index)++;
    return justify;
}

// ref: FUN_1408c6c60
int32_t PushColor(lua_State* L, uint32_t argb) {
    if (!lua_checkstack(L, 4)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    const float scale = 1.0f / 255.0f;
    lua_pushnumber(L, static_cast<double>(static_cast<float>((argb >> 16) & 0xff) * scale));
    lua_pushnumber(L, static_cast<double>(static_cast<float>((argb >> 8) & 0xff) * scale));
    lua_pushnumber(L, static_cast<double>(static_cast<float>(argb & 0xff) * scale));
    lua_pushnumber(L, static_cast<double>(static_cast<float>(argb >> 24) * scale));
    return 4;
}

// Sets the string flags the attributes carry, as a change when they differ.
void SetFlags(CSimpleFont* font, uint32_t value) {
    CSimpleFontAttributes& attributes = font->m_attributes;
    if (value != attributes.m_stringFlags || (attributes.m_flags & CSimpleFontAttributes::HAS_FLAGS) == 0) {
        attributes.m_stringFlags = value;
        attributes.m_flags |= CSimpleFontAttributes::HAS_FLAGS | CSimpleFontAttributes::CHANGED_FLAGS;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
}

// (font or "font"): takes the other font's settings.
// ref: FUN_1424ca130
int CopyFontObject(lua_State* L) {
    CSimpleFont* font = FontFromLua(L, 1);
    if (font == nullptr) {
        return 0;
    }
    CSimpleFont* source = nullptr;
    if (lua_type(L, 2) == LUA_TTABLE) {
        source = FontFromLua(L, 2);
        if (source == nullptr) {
            return 0;
        }
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        source = CSimpleFont::GetFont(lua_tostring(L, 2), false);
        if (source == nullptr || (source->m_flags & FSOBJ_ACCESSIBLE) == 0 || (source->m_flags & FSOBJ_HIDDEN) != 0) {
            FrameScript_ObjectError(font, "CopyFontObject", "Couldn't find font named %s", lua_tostring(L, 2));
            return 0;
        }
    } else {
        FrameScript_ObjectError(font, "CopyFontObject", "Usage: (font or \"font\")");
        return 0;
    }
    *font = *source;
    return 0;
}

// ref: FUN_140970120
int GetAlpha(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local alpha = self:GetAlpha()");
    uint8_t alpha = static_cast<uint8_t>(font->m_attributes.m_color >> 24);
    RaisePendingError(L, font, "GetAlpha");
    return PushAlpha(L, alpha);
}

// ref: FUN_140970eb0
int GetFont(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local fontFile, height, flags = self:GetFont()");
    const char* path = "";
    float height = 0.0f;
    uint8_t flags = 0;
    font->GetFontInfo(&path, &height, &flags);
    RaisePendingError(L, font, "GetFont");
    PushStringOrNil(L, path);
    const COORDINATESPACE& space = g_coordinateSpace;
    lua_pushnumber(L, static_cast<double>((space.m_aspectCompensation * 1024.0f * height) / space.m_diagonalWidth));
    PushStringOrNil(L, FontFlagsToString(flags));
    return 3;
}

// ref: FUN_140971d30
int GetFontObject(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local font = self:GetFontObject()");
    CSimpleFont* inherited = font->m_fontObject;
    RaisePendingError(L, font, "GetFontObject");
    return PushObject(L, inherited);
}

// ref: FUN_140972ac0
int GetIndentedWordWrap(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local wordWrap = self:GetIndentedWordWrap()");
    bool wrap = ((font->m_attributes.m_stringFlags >> 0x11) & 1) != 0;
    RaisePendingError(L, font, "GetIndentedWordWrap");
    return PushBool(L, wrap);
}

// ref: FUN_140973850
int GetJustifyH(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local justifyH = self:GetJustifyH()");
    uint32_t justify = font->m_attributes.m_stringFlags & 7;
    RaisePendingError(L, font, "GetJustifyH");
    return PushStringOrNil(L, JustifyToString(justify));
}

// ref: FUN_1409745e0
int GetJustifyV(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local justifyV = self:GetJustifyV()");
    uint32_t justify = font->m_attributes.m_stringFlags & 0x38;
    RaisePendingError(L, font, "GetJustifyV");
    return PushStringOrNil(L, JustifyToString(justify));
}

// ref: FUN_140975370
int GetShadowColor(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local r, g, b, a = self:GetShadowColor()");
    RaisePendingError(L, font, "GetShadowColor");
    return PushColor(L, font->m_attributes.m_shadowColor);
}

// ref: FUN_140976110
int GetShadowOffset(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local x, y = self:GetShadowOffset()");
    RaisePendingError(L, font, "GetShadowOffset");
    PushFloat(L, font->m_attributes.m_shadowOffset[0]);
    PushFloat(L, font->m_attributes.m_shadowOffset[1]);
    return 2;
}

// ref: FUN_140976eb0
int GetSpacing(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local spacing = self:GetSpacing()");
    float spacing = font->m_attributes.m_spacing;
    RaisePendingError(L, font, "GetSpacing");
    return PushUIUnits(L, spacing);
}

// ref: FUN_140977c40
int GetTextColor(lua_State* L) {
    int32_t index = 1;
    auto font = FontSelf(L, &index, "Usage: local r, g, b, a = self:GetTextColor()");
    RaisePendingError(L, font, "GetTextColor");
    return PushColor(L, font->m_attributes.m_color);
}

// ref: FUN_1409789e0
int SetAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetAlpha(alpha)";
    auto font = FontSelf(L, &index, usage);
    uint8_t alpha = RequiredAlpha(L, &index, usage);
    CSimpleFontAttributes& attributes = font->m_attributes;
    if (alpha != static_cast<uint8_t>(attributes.m_color >> 24) || (attributes.m_flags & CSimpleFontAttributes::HAS_COLOR) == 0) {
        attributes.m_color = (attributes.m_color & 0xffffff) | (static_cast<uint32_t>(alpha) << 24);
        attributes.m_flags |= CSimpleFontAttributes::HAS_COLOR | CSimpleFontAttributes::CHANGED_COLOR;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_COLOR;
    font->UpdateFontables();
    RaisePendingError(L, font, "SetAlpha");
    return 0;
}

// ref: FUN_1409798b0
int SetFont(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetFont(fontFile, height, flags)";
    auto font = FontSelf(L, &index, usage);
    const char* path = RequiredString(L, &index, usage);
    float height = 0.0f;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (lua_type(L, index) != LUA_TNUMBER) {
        luaL_argerror(L, index, usage);
    } else {
        height = CSimpleFont_ConvertHeight(static_cast<float>(lua_tonumber(L, index)));
        index++;
    }
    uint8_t flags = 0;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (lua_type(L, index) != LUA_TSTRING || !IsValidFontFlags(lua_tostring(L, index))) {
        luaL_argerror(L, index, usage);
    } else {
        StringToFontFlags(&flags, lua_tostring(L, index));
        index++;
    }
    FontSetFont(font, path, height, &flags);
    RaisePendingError(L, font, "SetFont");
    return 0;
}

// (font or "font" or nil); a font cannot inherit from itself.
// ref: FUN_1424ca250
int SetFontObject(lua_State* L) {
    CSimpleFont* font = FontFromLua(L, 1);
    if (font == nullptr) {
        return 0;
    }
    CSimpleFont* inherited = nullptr;
    if (lua_type(L, 2) == LUA_TTABLE) {
        inherited = FontFromLua(L, 2);
        if (inherited == nullptr) {
            return 0;
        }
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        inherited = CSimpleFont::GetFont(lua_tostring(L, 2), false);
        if (inherited == nullptr) {
            FrameScript_ObjectError(font, "SetFontObject", "Couldn't find font named %s", lua_tostring(L, 2));
            return 0;
        }
    } else if (lua_type(L, 2) != LUA_TNIL) {
        FrameScript_ObjectError(font, "SetFontObject", "Usage: (font or \"font\" or nil)");
        return 0;
    }
    for (CSimpleFont* check = inherited; check != nullptr; check = check->m_fontObject) {
        if (check == font) {
            FrameScript_ObjectError(font, "SetFontObject", "Can't create a font object loop");
            return 0;
        }
    }
    font->CSimpleFontable::SetFontObject(inherited);
    return 0;
}

// ref: FUN_14097b3c0
int SetIndentedWordWrap(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetIndentedWordWrap(wordWrap)";
    auto font = FontSelf(L, &index, usage);
    bool wrap = RequiredBool(L, &index, usage);
    uint32_t flags = font->m_attributes.m_stringFlags;
    SetFlags(font, wrap ? (flags | 0x20000) : (flags & ~0x20000u));
    font->UpdateFontables();
    RaisePendingError(L, font, "SetIndentedWordWrap");
    return 0;
}

// ref: FUN_14097c1c0
int SetJustifyH(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetJustifyH(justifyH)";
    auto font = FontSelf(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    SetFlags(font, (font->m_attributes.m_stringFlags & ~7u) | (justify & 7));
    font->UpdateFontables();
    RaisePendingError(L, font, "SetJustifyH");
    return 0;
}

// ref: FUN_14097cfb0
int SetJustifyV(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetJustifyV(justifyV)";
    auto font = FontSelf(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    SetFlags(font, (font->m_attributes.m_stringFlags & ~0x38u) | (justify & 0x38));
    font->UpdateFontables();
    RaisePendingError(L, font, "SetJustifyV");
    return 0;
}

// ref: FUN_1408d2fa0
int32_t SetShadowColorBody(lua_State* L, void* context) {
    (void)context;
    int32_t index = 1;
    const char* usage = "Usage: self:SetShadowColor(color [, a])";
    auto font = FontSelf(L, &index, usage);
    uint32_t color = RequiredRGB(L, &index, usage);
    color = (color & 0xffffff) | (static_cast<uint32_t>(OptionalAlpha(L, &index, usage)) << 24);
    CSimpleFontAttributes& attributes = font->m_attributes;
    if (color != attributes.m_shadowColor || (attributes.m_flags & CSimpleFontAttributes::HAS_SHADOW) == 0) {
        attributes.m_shadowColor = color;
        attributes.m_flags |= CSimpleFontAttributes::HAS_SHADOW | CSimpleFontAttributes::CHANGED_SHADOW;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
    font->UpdateFontables();
    RaisePendingError(L, font, "SetShadowColor");
    return 0;
}

// ref: FUN_14097dda0
int SetShadowColor(lua_State* L) {
    return ProtectedCall(L, SetShadowColorBody, nullptr);
}

// ref: FUN_14097ead0
int SetShadowOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetShadowOffset(offset)";
    auto font = FontSelf(L, &index, usage);
    float x = 0.0f;
    float y = 0.0f;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (!lua_isnumber(L, index) || !lua_isnumber(L, index + 1)) {
        luaL_argerror(L, index, usage);
    } else {
        x = static_cast<float>(lua_tonumber(L, index));
        y = static_cast<float>(lua_tonumber(L, index + 1));
    }
    CSimpleFontAttributes& attributes = font->m_attributes;
    if (x != attributes.m_shadowOffset[0] || y != attributes.m_shadowOffset[1]
        || (attributes.m_flags & CSimpleFontAttributes::HAS_SHADOW) == 0) {
        attributes.m_shadowOffset[0] = x;
        attributes.m_shadowOffset[1] = y;
        attributes.m_flags |= CSimpleFontAttributes::HAS_SHADOW | CSimpleFontAttributes::CHANGED_SHADOW;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SHADOW;
    font->UpdateFontables();
    RaisePendingError(L, font, "SetShadowOffset");
    return 0;
}

// ref: FUN_14097f9d0
int SetSpacing(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetSpacing(spacing)";
    auto font = FontSelf(L, &index, usage);
    float spacing = RequiredUIUnits(L, &index, usage);
    CSimpleFontAttributes& attributes = font->m_attributes;
    float delta = spacing - attributes.m_spacing;
    if (2.3841858e-07f <= (delta < 0.0f ? -delta : delta) || (attributes.m_flags & CSimpleFontAttributes::HAS_SPACING) == 0) {
        attributes.m_spacing = spacing;
        attributes.m_flags |= CSimpleFontAttributes::HAS_SPACING | CSimpleFontAttributes::CHANGED_SPACING;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_SPACING;
    font->UpdateFontables();
    RaisePendingError(L, font, "SetSpacing");
    return 0;
}

// ref: FUN_1408d3e40
int32_t SetTextColorBody(lua_State* L, void* context) {
    (void)context;
    int32_t index = 1;
    const char* usage = "Usage: self:SetTextColor(color [, a])";
    auto font = FontSelf(L, &index, usage);
    uint32_t color = RequiredRGB(L, &index, usage);
    color = (color & 0xffffff) | (static_cast<uint32_t>(OptionalAlpha(L, &index, usage)) << 24);
    CSimpleFontAttributes& attributes = font->m_attributes;
    if (color != attributes.m_color || (attributes.m_flags & CSimpleFontAttributes::HAS_COLOR) == 0) {
        attributes.m_color = color;
        attributes.m_flags |= CSimpleFontAttributes::HAS_COLOR | CSimpleFontAttributes::CHANGED_COLOR;
    }
    font->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_COLOR;
    font->UpdateFontables();
    RaisePendingError(L, font, "SetTextColor");
    return 0;
}

// ref: FUN_140980870
int SetTextColor(lua_State* L) {
    return ProtectedCall(L, SetTextColorBody, nullptr);
}

// 0x142cf8200
const luaL_Reg s_fontMethods[] = {
    { "CopyFontObject", CopyFontObject },
    { "GetAlpha", GetAlpha },
    { "GetFont", GetFont },
    { "GetFontObject", GetFontObject },
    { "GetIndentedWordWrap", GetIndentedWordWrap },
    { "GetJustifyH", GetJustifyH },
    { "GetJustifyV", GetJustifyV },
    { "GetShadowColor", GetShadowColor },
    { "GetShadowOffset", GetShadowOffset },
    { "GetSpacing", GetSpacing },
    { "GetTextColor", GetTextColor },
    { "SetAlpha", SetAlpha },
    { "SetFont", SetFont },
    { "SetFontObject", SetFontObject },
    { "SetIndentedWordWrap", SetIndentedWordWrap },
    { "SetJustifyH", SetJustifyH },
    { "SetJustifyV", SetJustifyV },
    { "SetShadowColor", SetShadowColor },
    { "SetShadowOffset", SetShadowOffset },
    { "SetSpacing", SetSpacing },
    { "SetTextColor", SetTextColor },
};
static_assert(sizeof(s_fontMethods) / sizeof(s_fontMethods[0]) == 0x15, "0x15 Font methods");

}

// ref: FUN_140a50f80
void CSimpleFont_FillScriptMethods(lua_State* L) {
    CScriptObject_FillObjectScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_fontMethods, 0x15);
}
