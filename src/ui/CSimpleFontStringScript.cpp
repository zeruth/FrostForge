// The Lua methods of font strings (0x142cf8350), with the Region, object and animation methods
// they share with textures.

#include "ui/CSimpleFontString.hpp"
#include "ui/UIScriptArgs.hpp"

#include <cstring>

#include "base/Coordinate.hpp"
#include "fileservices/AsyncFile.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "gxu/GxuFontPrivate.hpp"
#include "storm/Memory.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

void CScriptRegion_FillScriptMethods(lua_State* L);
void CSimpleRegion_FillScriptMethods(lua_State* L);
void CAnimatableObject_FillScriptMethods(lua_State* L);

namespace {

using namespace ScriptMethod;

const char s_renderScriptFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Frame\\CSimpleRenderScript.cpp";
const char s_boundsTag[] = ".?AUuiBoundsRect@@";

CSimpleFontString* StringSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleFontString*>(Self(L, CSimpleFontString::GetObjectType(), index, usage));
}

// The font string for the methods that report through the object error (FUN_140642590 with
// the font string type, FUN_141a10160, and FUN_14062cd40 on a lookup error).
CSimpleFontString* StringFromLua(lua_State* L) {
    const char* error = nullptr;
    auto string = static_cast<CSimpleFontString*>(
        FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFontString::GetObjectType(), 1, false, false));
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    return string;
}

}

// Three numbers 0..1 as 0xffRRGGBB (FUN_140a50ab0).
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

// A number clamped to 0..1 (FUN_140a54b40).
float RequiredUnit(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return 0.0f;
    }
    if (lua_type(L, *index) != LUA_TNUMBER) {
        luaL_argerror(L, *index, usage);
        return 0.0f;
    }
    float value = static_cast<float>(lua_tonumber(L, *index));
    if (value < 0.0f) {
        value = 0.0f;
    } else if (1.0f < value) {
        value = 1.0f;
    }
    (*index)++;
    return value;
}

// An optional alpha 0..1 into {value, present}: CSimpleTextureScript.cpp's (FUN_1407cbb50).
void OptionalAlphaByte(lua_State* L, int32_t* index, const char* usage, uint8_t* alpha);

// A justification name (FUN_1408c6380).
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
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    }
    (*index)++;
    return justify;
}

// Two raw numbers (the shadow offset reader).
void RequiredPair(lua_State* L, int32_t* index, const char* usage, float* values) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return;
    }
    if (!lua_isnumber(L, *index) || !lua_isnumber(L, *index + 1)) {
        luaL_argerror(L, *index, usage);
        return;
    }
    values[0] = static_cast<float>(lua_tonumber(L, *index));
    values[1] = static_cast<float>(lua_tonumber(L, *index + 1));
    *index += 2;
}

// A colour as r, g, b, a (FUN_140a50e10).
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

// A justification as its name (FUN_1408c6e70).
// (not FUN_1407cb130: that takes a context holding the lua_State, not the lua_State, so translated
// callers handed this their context; untagged 2026-10-21, they run its translation)
int32_t PushJustify(lua_State* L, uint32_t justify) {
    return ScriptMethod::PushStringOrNil(L, JustifyToString(justify));
}

namespace {

// Sets a string flag the font object no longer decides.
void SetOwnFlag(CSimpleFontString* string, uint32_t flag, bool on) {
    string->m_inheritedFlags &= ~flag;
    string->SetStringFlag(flag, on);
}

// ---- the methods ---------------------------------------------------------------------------

struct SPANCONTEXT {
    CSimpleFontString* m_string;
    uint32_t m_start;
    uint32_t m_end;
    uiBoundsRect* m_rects;
    uint64_t m_count;
    bool m_found;
};

// ref: FUN_1408d03f0
int32_t CalculateScreenAreaFromCharacterSpanBody(lua_State* L, void* context) {
    auto ctx = static_cast<SPANCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local areas = self:CalculateScreenAreaFromCharacterSpan(leftIndex, rightIndex)";
    ctx->m_string = StringSelf(L, &index, usage);
    ctx->m_start = OptionalIndex(L, &index, usage);
    ctx->m_end = OptionalIndex(L, &index, usage);
    ctx->m_found = ctx->m_string->CalculateScreenAreaFromCharacterSpan(ctx->m_start, ctx->m_end, &ctx->m_rects, &ctx->m_count);
    RaisePendingError(L, ctx->m_string, "CalculateScreenAreaFromCharacterSpan");
    if (!ctx->m_found) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int32_t>(ctx->m_count), 0);
    int32_t table = lua_gettop(L);
    for (uint64_t i = 0; i < ctx->m_count; i++) {
        lua_pushnumber(L, static_cast<double>(i + 1));
        lua_createtable(L, 0, 4);
        int32_t area = lua_gettop(L);
        PushUIUnits(L, ctx->m_rects[i].m_left);
        lua_setfield(L, area, "left");
        PushUIUnits(L, ctx->m_rects[i].m_bottom);
        lua_setfield(L, area, "bottom");
        PushUIUnits(L, ctx->m_rects[i].m_width);
        lua_setfield(L, area, "width");
        PushUIUnits(L, ctx->m_rects[i].m_height);
        lua_setfield(L, area, "height");
        lua_settable(L, table);
    }
    return 1;
}

// ref: FUN_1409483e0
int CalculateScreenAreaFromCharacterSpan(lua_State* L) {
    SPANCONTEXT context = {};
    int results = ProtectedCall(L, CalculateScreenAreaFromCharacterSpanBody, &context);
    if (context.m_rects != nullptr) {
        SMemFree(context.m_rects, s_boundsTag, -6, 0);
    }
    return results;
}

// ref: FUN_140949140
int CanNonSpaceWrap(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local wrap = self:CanNonSpaceWrap()");
    bool wrap = ((string->m_stringFlags >> 12) & 1) != 0;
    RaisePendingError(L, string, "CanNonSpaceWrap");
    return PushBool(L, wrap);
}

// ref: FUN_140949ec0
int CanWordWrap(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local wrap = self:CanWordWrap()");
    bool wrap = ((string->m_stringFlags >> 6) & 1) == 0;
    RaisePendingError(L, string, "CanWordWrap");
    return PushBool(L, wrap);
}

// ref: FUN_14094ac40
int FindCharacterIndexAtCoordinate(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local characterIndex, inside = self:FindCharacterIndexAtCoordinate(x, y)";
    auto string = StringSelf(L, &index, usage);
    float x = RequiredUIUnits(L, &index, usage);
    float y = RequiredUIUnits(L, &index, usage);
    int32_t character = 0;
    bool inside = false;
    if (!string->FindCharacterIndexAtCoordinate(x, y, &character, &inside)) {
        return 0;
    }
    RaisePendingError(L, string, "FindCharacterIndexAtCoordinate");
    lua_pushinteger(L, character + 1);
    return 1 + PushBool(L, inside);
}

// ref: FUN_14094c7c0
int GetFieldSize(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local fieldSize = self:GetFieldSize()");
    RaisePendingError(L, string, "GetFieldSize");
    return PushInt(L, 0x1fff);
}

// The font file (or its FileDataID), the height and the flags.
// ref: FUN_1424ca490
int GetFont(lua_State* L) {
    CSimpleFontString* string = StringFromLua(L);
    if (string == nullptr) {
        return 0;
    }
    const char* name = string->GetFontName();
    if (name == nullptr || *name == '\0') {
        int32_t fileDataId = string->GetFontFileDataId();
        if (fileDataId == 0) {
            lua_pushnil(L);
        } else {
            lua_pushnumber(L, static_cast<double>(fileDataId));
        }
    } else {
        lua_pushstring(L, name);
    }
    const COORDINATESPACE& space = g_coordinateSpace;
    float height = string->GetFontHeight(false);
    lua_pushnumber(L, static_cast<double>(space.m_aspectCompensation * height * (1024.0f / space.m_diagonalWidth)));
    PushStringOrNil(L, FontFlagsToString(string->GetFontFlags()));
    return 3;
}

// ref: FUN_14094e1e0
int GetFontObject(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local font = self:GetFontObject()");
    CSimpleFont* font = string->m_fontObject;
    RaisePendingError(L, string, "GetFontObject");
    return PushObject(L, font);
}

// ref: FUN_14094ef60
int GetIndentedWordWrap(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local wrap = self:GetIndentedWordWrap()");
    bool wrap = ((string->m_stringFlags >> 0x11) & 1) != 0;
    RaisePendingError(L, string, "GetIndentedWordWrap");
    return PushBool(L, wrap);
}

// ref: FUN_14094fce0
int GetJustifyH(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local justifyH = self:GetJustifyH()");
    uint32_t justify = string->m_stringFlags & 7;
    RaisePendingError(L, string, "GetJustifyH");
    return PushJustify(L, justify);
}

// ref: FUN_140950a60
int GetJustifyV(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local justifyH = self:GetJustifyV()");
    uint32_t justify = string->m_stringFlags & 0x38;
    RaisePendingError(L, string, "GetJustifyV");
    return PushJustify(L, justify);
}

// ref: FUN_1409517e0
int GetLineHeight(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local lineHeight = self:GetLineHeight()");
    float height = string->GetEffectiveFontHeight();
    RaisePendingError(L, string, "GetLineHeight");
    return PushUIUnits(L, height);
}

// ref: FUN_140952560
int GetMaxLines(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local maxLines = self:GetMaxLines()");
    uint32_t lines = string->m_maxLines & 0xffffff;
    RaisePendingError(L, string, "GetMaxLines");
    return PushUInt(L, lines);
}

// ref: FUN_1409532e0
int GetNumLines(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local numLines = self:GetNumLines()");
    uint32_t lines = string->GetNumLines();
    RaisePendingError(L, string, "GetNumLines");
    return PushUInt(L, lines);
}

// ref: FUN_140954060
int GetRotation(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local radians = self:GetRotation()");
    float rotation = string->m_rotation;
    RaisePendingError(L, string, "GetRotation");
    return PushFloat(L, rotation);
}

// ref: FUN_140954de0
int GetShadowColor(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local r, g, b, a = self:GetShadowColor()");
    RaisePendingError(L, string, "GetShadowColor");
    return PushColor(L, string->m_shadowColor);
}

// ref: FUN_140955b60
int GetShadowOffset(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local x, y = self:GetShadowOffset()");
    RaisePendingError(L, string, "GetShadowOffset");
    PushFloat(L, string->m_shadowOffset[0]);
    PushFloat(L, string->m_shadowOffset[1]);
    return 2;
}

// ref: FUN_1409568e0
int GetSpacing(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local spacing = self:GetSpacing()");
    float spacing = string->m_spacing;
    RaisePendingError(L, string, "GetSpacing");
    return PushUIUnits(L, spacing);
}

// ref: FUN_140957660
int GetStringHeight(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local height = self:GetStringHeight()");
    float height = string->GetStringHeight();
    RaisePendingError(L, string, "GetStringHeight");
    return PushUIUnits(L, height);
}

// ref: FUN_1409583e0
int GetStringWidth(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local width = self:GetStringWidth()");
    float width = string->GetStringWidth();
    RaisePendingError(L, string, "GetStringWidth");
    return PushUIUnits(L, width);
}

// ref: FUN_140959160
int GetText(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local text = self:GetText()");
    const char* text = string->m_text;
    if (text == nullptr || *text == '\0') {
        text = nullptr;
    }
    RaisePendingError(L, string, "GetText");
    return PushStringOrNil(L, text);
}

// ref: FUN_140959ef0
int GetTextColor(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local r, g, b, a = self:GetTextColor()");
    uint32_t argb = 0;
    if (!string->GetVertexColor(&argb)) {
        return 0;
    }
    RaisePendingError(L, string, "GetTextColor");
    return PushColor(L, argb);
}

// ref: FUN_14095b8f0
int GetTextScale(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local textScale = self:GetTextScale()");
    float scale = string->m_fontScale;
    RaisePendingError(L, string, "GetTextScale");
    return PushFloat(L, scale);
}

// ref: FUN_14095c670
int GetUnboundedStringWidth(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local width = self:GetUnboundedStringWidth()");
    float width = string->GetTextWidth(nullptr, 0);
    RaisePendingError(L, string, "GetUnboundedStringWidth");
    return PushUIUnits(L, width);
}

// ref: FUN_14095d400
int GetWrappedWidth(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local width = self:GetWrappedWidth()");
    float width = string->GetUnboundedStringWidth();
    RaisePendingError(L, string, "GetWrappedWidth");
    return PushUIUnits(L, width);
}

// ref: FUN_14095e180
int IsTruncated(lua_State* L) {
    int32_t index = 1;
    auto string = StringSelf(L, &index, "Usage: local isTruncated = self:IsTruncated()");
    bool truncated = string->IsTruncated();
    RaisePendingError(L, string, "IsTruncated");
    return PushBool(L, truncated);
}

// ref: FUN_14095ef00
int SetAlphaGradient(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local isWithinText = self:SetAlphaGradient(start, length)";
    auto string = StringSelf(L, &index, usage);
    int32_t start = RequiredInt32(L, &index, usage);
    int32_t length = RequiredInt32(L, &index, usage);
    bool within = string->SetAlphaGradient(static_cast<int16_t>(start), length);
    RaisePendingError(L, string, "SetAlphaGradient");
    return PushBool(L, within);
}

// ref: FUN_14095fce0
int SetFixedColor(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetFixedColor(fixedColor)";
    auto string = StringSelf(L, &index, usage);
    bool fixed = RequiredBool(L, &index, usage);
    SetOwnFlag(string, CSimpleFontString::FONTSTRING_NO_COLOR_CODES, fixed);
    RaisePendingError(L, string, "SetFixedColor");
    return 0;
}

// ("font", fontHeight [, flags]); a font not yet on disk is fetched first when tainted code
// asks, and taken once the string next lays out.
// ref: FUN_1424ca7b0
int SetFont(lua_State* L) {
    CSimpleFontString* string = StringFromLua(L);
    if (string == nullptr) {
        return 0;
    }
    if ((!lua_isnumber(L, 2) && !lua_isstring(L, 2)) || !lua_isnumber(L, 3)) {
        FrameScript_ObjectError(string, "SetFont", "Arguments: (\"font\", fontHeight [, flags])");
        return 0;
    }
    float height = CSimpleFont_ConvertHeight(static_cast<float>(lua_tonumber(L, 3)));
    if (!(1.1920929e-07f < height)) {
        FrameScript_ObjectError(string, "SetFont", "Invalid fontHeight: %f, height must be > 0", static_cast<double>(height));
        return 0;
    }
    uint8_t flags = 0;
    if (lua_isstring(L, 4)) {
        StringToFontFlags(&flags, lua_tostring(L, 4));
    }
    bool secure = lua_taint == nullptr;
    bool set;
    if (!lua_isnumber(L, 2)) {
        const char* path = lua_tostring(L, 2);
        if (*path == '\0') {
            return 0;
        }
        if (!secure && !GxuFontFaceExistsByName(path)) {
            AsyncFilePrefetchByName(path);
            if (string->m_fontRequest == nullptr) {
                string->m_fontRequest = static_cast<FONTSTRINGREQUEST*>(SMemAlloc(sizeof(FONTSTRINGREQUEST), 0x1f, s_renderScriptFile, 0, 0x11f, 0));
            }
            FONTSTRINGREQUEST* request = string->m_fontRequest;
            request->m_fontFlags = flags;
            request->m_height = height;
            uint32_t i = 0;
            for (; i < 0x103 && path[i] != '\0'; i++) {
                request->m_path[i] = path[i];
            }
            request->m_path[i] = '\0';
            request->m_fileDataId = 0;
            return 0;
        }
        set = string->SetFont(path, height, flags, 0);
    } else {
        int32_t fileDataId = static_cast<int32_t>(lua_tointeger(L, 2));
        if (fileDataId == 0) {
            return 0;
        }
        if (!secure && !GxuFontFaceExistsById(static_cast<uint32_t>(fileDataId))) {
            AsyncFilePrefetchById(fileDataId);
            if (string->m_fontRequest == nullptr) {
                string->m_fontRequest = static_cast<FONTSTRINGREQUEST*>(SMemAlloc(sizeof(FONTSTRINGREQUEST), 0x1f, s_renderScriptFile, 0, 0x13a, 0));
            }
            FONTSTRINGREQUEST* request = string->m_fontRequest;
            request->m_fontFlags = flags;
            request->m_height = height;
            request->m_fileDataId = fileDataId;
            request->m_path[0] = '\0';
            return 0;
        }
        set = string->SetFont(fileDataId, height, flags, 0);
    }
    if (set) {
        string->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FONT;
    }
    lua_pushboolean(L, set);
    return 1;
}

// (font or "font" or nil)
// ref: FUN_1424ca5e0
int SetFontObject(lua_State* L) {
    CSimpleFontString* string = StringFromLua(L);
    if (string == nullptr) {
        return 0;
    }
    CSimpleFont* font = nullptr;
    const char* error = nullptr;
    if (lua_type(L, 2) == LUA_TTABLE) {
        font = static_cast<CSimpleFont*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFont::GetObjectType(), 2, false, false));
        if (font == nullptr && lua_type(L, 2) != LUA_TNIL) {
            FrameScript_ObjectError(string, "SetFontObject", "Arguments: (font or \"font\" or nil)");
            return 0;
        }
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        font = CSimpleFont::GetFont(lua_tostring(L, 2), false);
        if (font == nullptr) {
            FrameScript_ObjectError(string, "SetFontObject", "Couldn't find font named %s", lua_tostring(L, 2));
            return 0;
        }
    } else if (lua_type(L, 2) != LUA_TNIL) {
        FrameScript_ObjectError(string, "SetFontObject", "Arguments: (font or \"font\" or nil)");
        return 0;
    }
    string->SetFontObject(font);
    return 0;
}

// ref: FUN_1424cab50
int SetFormattedText(lua_State* L) {
    CSimpleFontString* string = StringFromLua(L);
    if (string == nullptr) {
        return 0;
    }
    if (!string->m_font && string->m_fontRequest == nullptr) {
        FrameScript_ObjectError(string, "SetFormattedText", "Font not set");
        return 0;
    }
    char buffer[0x1000];
    string->SetText(FrameScript_FormatString(L, 2, buffer, sizeof(buffer)), true);
    return 0;
}

// ref: FUN_1409630f0
int SetIndentedWordWrap(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetIndentedWordWrap(wrap)";
    auto string = StringSelf(L, &index, usage);
    bool wrap = RequiredBool(L, &index, usage);
    SetOwnFlag(string, CSimpleFontString::FONTSTRING_INDENTED_WORD_WRAP, wrap);
    RaisePendingError(L, string, "SetIndentedWordWrap");
    return 0;
}

// ref: FUN_140963ef0
int SetJustifyH(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetJustifyH(justifyH)";
    auto string = StringSelf(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    string->m_inheritedFlags &= ~CSimpleFontString::FONTSTRING_JUSTIFY_H;
    string->SetJustifyH(justify & 7);
    string->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    RaisePendingError(L, string, "SetJustifyH");
    return 0;
}

// ref: FUN_140964ce0
int SetJustifyV(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetJustifyV(justifyV)";
    auto string = StringSelf(L, &index, usage);
    uint32_t justify = RequiredJustify(L, &index, usage);
    string->m_inheritedFlags &= ~CSimpleFontString::FONTSTRING_JUSTIFY_V;
    string->SetJustifyV(justify & 0x38);
    string->m_fontableFlags &= ~CSimpleFontAttributes::CHANGED_FLAGS;
    RaisePendingError(L, string, "SetJustifyV");
    return 0;
}

// ref: FUN_140965ad0
int SetMaxLines(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetMaxLines(maxLines)";
    auto string = StringSelf(L, &index, usage);
    int32_t lines = RequiredInt32(L, &index, usage);
    string->m_maxLines = (string->m_maxLines & 0xff000000) | (static_cast<uint32_t>(lines) & 0xffffff);
    RaisePendingError(L, string, "SetMaxLines");
    return 0;
}

// ref: FUN_1409668d0
int SetNonSpaceWrap(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetNonSpaceWrap(wrap)";
    auto string = StringSelf(L, &index, usage);
    bool wrap = RequiredBool(L, &index, usage);
    SetOwnFlag(string, CSimpleFontString::FONTSTRING_NON_SPACE_WRAP, wrap);
    RaisePendingError(L, string, "SetNonSpaceWrap");
    return 0;
}

// ref: FUN_1409676d0
int SetRotation(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetRotation(radians)";
    auto string = StringSelf(L, &index, usage);
    float radians = RequiredFinite(L, &index, usage);
    string->SetRotation(radians);
    RaisePendingError(L, string, "SetRotation");
    return 0;
}

struct COLORCONTEXT {
    CSimpleFontString* m_string;
    uint32_t m_rgb;
    uint8_t m_alpha[2];
};

// ref: FUN_1408d12c0
int32_t SetShadowColorBody(lua_State* L, void* context) {
    auto ctx = static_cast<COLORCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetShadowColor(color [, a])";
    ctx->m_string = StringSelf(L, &index, usage);
    ctx->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, ctx->m_alpha);
    ctx->m_string->SetShadowColor(&ctx->m_rgb, ctx->m_alpha);
    RaisePendingError(L, ctx->m_string, "SetShadowColor");
    return 0;
}

// ref: FUN_1409684c0
int SetShadowColor(lua_State* L) {
    COLORCONTEXT context = {};
    return ProtectedCall(L, SetShadowColorBody, &context);
}

// ref: FUN_1409691f0
int SetShadowOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetShadowOffset(offset)";
    auto string = StringSelf(L, &index, usage);
    float offset[2] = {};
    RequiredPair(L, &index, usage, offset);
    string->SetShadowOffset(offset);
    RaisePendingError(L, string, "SetShadowOffset");
    return 0;
}

// ref: FUN_14096a0d0
int SetSpacing(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetSpacing(spacing)";
    auto string = StringSelf(L, &index, usage);
    float spacing = RequiredUIUnits(L, &index, usage);
    string->SetSpacingFromFont(spacing);
    RaisePendingError(L, string, "SetSpacing");
    return 0;
}

// ref: FUN_14096af20
int SetText(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetText([text])";
    auto string = StringSelf(L, &index, usage);
    const char* text = "";
    if (0 < lua_type(L, index)) {
        if (!lua_isstring(L, index)) {
            luaL_argerror(L, index, usage);
        } else {
            text = lua_tostring(L, index);
        }
    }
    string->SetTextChecked(text);
    RaisePendingError(L, string, "SetText");
    return 0;
}

// ref: FUN_1408d2130
int32_t SetTextColorBody(lua_State* L, void* context) {
    auto ctx = static_cast<COLORCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetTextColor(color [, a])";
    ctx->m_string = StringSelf(L, &index, usage);
    ctx->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, ctx->m_alpha);
    ctx->m_string->SetVertexColor(ctx->m_rgb, ctx->m_alpha);
    RaisePendingError(L, ctx->m_string, "SetTextColor");
    return 0;
}

// ref: FUN_14096bd10
int SetTextColor(lua_State* L) {
    COLORCONTEXT context = {};
    return ProtectedCall(L, SetTextColorBody, &context);
}

// ref: FUN_14096ca40
int SetTextHeight(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetTextHeight(height)";
    auto string = StringSelf(L, &index, usage);
    float height = RequiredUIUnits(L, &index, usage);
    string->SetFontHeight(height);
    RaisePendingError(L, string, "SetTextHeight");
    return 0;
}

// ref: FUN_14096d890
int SetTextScale(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetTextScale(textScale)";
    auto string = StringSelf(L, &index, usage);
    float scale = RequiredFinite(L, &index, usage);
    string->SetFontScale(scale);
    RaisePendingError(L, string, "SetTextScale");
    return 0;
}

// ref: FUN_14096e680
int SetWordWrap(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetWordWrap(wrap)";
    auto string = StringSelf(L, &index, usage);
    bool wrap = RequiredBool(L, &index, usage);
    SetOwnFlag(string, CSimpleFontString::FONTSTRING_NO_WORD_WRAP, !wrap);
    RaisePendingError(L, string, "SetWordWrap");
    return 0;
}

// 0x142cf8350
const luaL_Reg s_fontStringMethods[] = {
    { "CalculateScreenAreaFromCharacterSpan", CalculateScreenAreaFromCharacterSpan },
    { "CanNonSpaceWrap", CanNonSpaceWrap },
    { "CanWordWrap", CanWordWrap },
    { "FindCharacterIndexAtCoordinate", FindCharacterIndexAtCoordinate },
    { "GetFieldSize", GetFieldSize },
    { "GetFont", GetFont },
    { "GetFontObject", GetFontObject },
    { "GetIndentedWordWrap", GetIndentedWordWrap },
    { "GetJustifyH", GetJustifyH },
    { "GetJustifyV", GetJustifyV },
    { "GetLineHeight", GetLineHeight },
    { "GetMaxLines", GetMaxLines },
    { "GetNumLines", GetNumLines },
    { "GetRotation", GetRotation },
    { "GetShadowColor", GetShadowColor },
    { "GetShadowOffset", GetShadowOffset },
    { "GetSpacing", GetSpacing },
    { "GetStringHeight", GetStringHeight },
    { "GetStringWidth", GetStringWidth },
    { "GetText", GetText },
    { "GetTextColor", GetTextColor },
    { "GetTextScale", GetTextScale },
    { "GetUnboundedStringWidth", GetUnboundedStringWidth },
    { "GetWrappedWidth", GetWrappedWidth },
    { "IsTruncated", IsTruncated },
    { "SetAlphaGradient", SetAlphaGradient },
    { "SetFixedColor", SetFixedColor },
    { "SetFont", SetFont },
    { "SetFontObject", SetFontObject },
    { "SetFormattedText", SetFormattedText },
    { "SetIndentedWordWrap", SetIndentedWordWrap },
    { "SetJustifyH", SetJustifyH },
    { "SetJustifyV", SetJustifyV },
    { "SetMaxLines", SetMaxLines },
    { "SetNonSpaceWrap", SetNonSpaceWrap },
    { "SetRotation", SetRotation },
    { "SetShadowColor", SetShadowColor },
    { "SetShadowOffset", SetShadowOffset },
    { "SetSpacing", SetSpacing },
    { "SetText", SetText },
    { "SetTextColor", SetTextColor },
    { "SetTextHeight", SetTextHeight },
    { "SetTextScale", SetTextScale },
    { "SetWordWrap", SetWordWrap },
};
static_assert(sizeof(s_fontStringMethods) / sizeof(s_fontStringMethods[0]) == 0x2c, "0x2c FontString methods");

}

// ref: FUN_140a50fb0
void CSimpleFontString_FillScriptMethods(lua_State* L) {
    CScriptRegion_FillScriptMethods(L);
    CSimpleRegion_FillScriptMethods(L);
    CAnimatableObject_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_fontStringMethods, 0x2c);
}
