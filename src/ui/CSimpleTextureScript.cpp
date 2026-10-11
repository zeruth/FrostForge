// The Lua methods of regions (CSimpleRegion's table 0x142cf9550), textures (0x142cf9cb0) and
// their masks (0x142cf9c70); the MaskTexture's own metatables; and frame:CreateTexture's
// region factory.

#include "ui/CSimpleTexture.hpp"

#include <cstring>

#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "texture/Texture.hpp"
#include "ui/CScriptObjectScript.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/TextureAtlas.hpp"
#include "ui/UITypes.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

void CScriptRegion_FillScriptMethods(lua_State* L);
void CAnimatableObject_FillScriptMethods(lua_State* L);

// A number clamped to 0..1 (FUN_140a54b40; ui/CSimpleFontStringScript.cpp).
float RequiredUnit(lua_State* L, int32_t* index, const char* usage);

namespace {

using namespace ScriptMethod;

const float TEXELS_PER_SCREEN = 1024.0f;
const float UI_WIDTH = 0.8f;

CSimpleRegion* RegionSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleRegion*>(Self(L, CScriptRegion::GetObjectType(), index, usage));
}

CSimpleTexture* TextureSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleTexture*>(Self(L, CSimpleTexture::GetObjectType(), index, usage));
}

// The texture for the methods that report through the object error (FUN_140642590 with the
// texture type, and FUN_14062cd40 on a lookup error).
CSimpleTexture* TextureFromLua(lua_State* L) {
    const char* error = nullptr;
    auto texture = static_cast<CSimpleTexture*>(
        FrameScript_Object::GetObjectFromLua(&error, L, CSimpleTexture::GetObjectType(), 1, false, false));
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    return texture;
}

// Three numbers 0..1 as 0xffRRGGBB (FUN_140a548f0).
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

// An optional alpha 0..1 into {value, present} (the template's inline reader).
// ref: FUN_1407cbb50
void OptionalAlphaByte(lua_State* L, int32_t* index, const char* usage, uint8_t* alpha) {
    if (lua_type(L, *index) < 1) {
        (*index)++;
        return;
    }
    auto valid = [&]() {
        if (lua_type(L, *index) != LUA_TNUMBER) {
            return false;
        }
        float value = static_cast<float>(lua_tonumber(L, *index));
        return !(value < 0.0f) && !(1.0f < value);
    };
    if (!valid()) {
        luaL_argerror(L, *index, usage);
        return;
    }
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return;
    }
    alpha[0] = valid() ? static_cast<uint8_t>(static_cast<int32_t>(static_cast<float>(lua_tonumber(L, *index)) * 255.0f + 0.5f)) : 0;
    (*index)++;
    if (alpha[1] == 0) {
        alpha[1] = 1;
    }
}

// A vertex index, 1-based in Lua (FUN_1408c5f10).
uint32_t RequiredIndex(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return 0;
    }
    double value = lua_tonumber(L, *index);
    if (!lua_isnumber(L, *index) || value != value || value < 0.0 || 4294967295.0 < value) {
        lua_pushfstring(L, "outside of expected range %u to %u", 0u, 0xffffffffu);
        luaL_argerror(L, *index, lua_pushfstring(L, "%s - %s", lua_tostring(L, -1), usage));
        return 0;
    }
    (*index)++;
    return static_cast<uint32_t>(static_cast<int32_t>(value - 1.0));
}

// An orientation name (FUN_140a54cf0).
uint32_t RequiredOrientation(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return 0;
    }
    uint32_t orientation = 0;
    if (lua_type(L, *index) != LUA_TSTRING || !StringToOrientation(lua_tolstring(L, *index, nullptr), &orientation)) {
        luaL_argerror(L, *index, usage);
        return 0;
    }
    (*index)++;
    return orientation;
}

// A ColorMixin table {r, g, b, a} as ARGB (FUN_140a53de0 with FUN_140ae5710).
uint32_t RequiredColorTable(lua_State* L, int32_t* index, const char* usage) {
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return 0;
    }
    int32_t at = *index;
    auto has = [&](const char* field) {
        lua_getfield(L, at, field);
        bool present = lua_type(L, -1) != LUA_TNIL;
        lua_settop(L, -2);
        return present;
    };
    if (lua_type(L, at) != LUA_TTABLE || !has("a") || !has("r") || !has("g") || !has("b")) {
        luaL_argerror(L, at, usage);
        return 0;
    }
    auto channel = [&](const char* field) {
        lua_getfield(L, at, field);
        float value = static_cast<float>(lua_tonumber(L, -1));
        lua_settop(L, -2);
        if (value < 0.0f) {
            value = 0.0f;
        } else if (1.0f <= value) {
            value = 1.0f;
        }
        return static_cast<uint32_t>(static_cast<int64_t>(value * 255.0f + 0.5f)) & 0xff;
    };
    uint32_t b = channel("b");
    uint32_t g = channel("g");
    uint32_t r = channel("r");
    uint32_t a = channel("a");
    (*index)++;
    return a << 24 | r << 16 | g << 8 | b;
}

// An optional {x, y} table (FUN_140a53f80).
bool OptionalPoint(lua_State* L, int32_t* index, const char* usage, float* point) {
    int32_t at = *index;
    if (lua_type(L, at) < 1) {
        (*index)++;
        return false;
    }
    auto has = [&](const char* field) {
        lua_getfield(L, at, field);
        bool present = lua_type(L, -1) != LUA_TNIL;
        lua_settop(L, -2);
        return present;
    };
    if (lua_type(L, at) != LUA_TTABLE || !has("x") || !has("y")) {
        luaL_argerror(L, at, usage);
        return false;
    }
    lua_getfield(L, at, "x");
    point[0] = static_cast<float>(lua_tonumber(L, -1));
    lua_settop(L, -2);
    lua_getfield(L, at, "y");
    point[1] = static_cast<float>(lua_tonumber(L, -1));
    lua_settop(L, -2);
    (*index)++;
    return true;
}

// A filter mode name (FUN_1424df470).
bool StringToFilterMode(const char* string, uint8_t* mode) {
    static const struct {
        uint8_t m_mode;
        const char* m_name;
    } s_modes[] = { { 1, "LINEAR" }, { 2, "TRILINEAR" }, { 0, "NEAREST" } };
    for (const auto& entry : s_modes) {
        if (_stricmp(entry.m_name, string) == 0) {
            *mode = entry.m_mode;
            return true;
        }
    }
    return false;
}

// A FileDataID pushed as a number, nil for none (FUN_14065f6a0).
void PushFileDataId(lua_State* L, int32_t fileDataId) {
    if (fileDataId == 0) {
        lua_pushnil(L);
        return;
    }
    lua_pushnumber(L, static_cast<double>(fileDataId));
}

// ---- CSimpleRegion -------------------------------------------------------------------------

// ref: FUN_140a90700
int GetAlpha(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local alpha = self:GetAlpha()");
    uint8_t alpha = region->GetAlpha();
    RaisePendingError(L, region, "GetAlpha");
    return PushAlpha(L, alpha);
}

// ref: FUN_140a914d0
int GetDrawLayer(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local layer, sublayer = self:GetDrawLayer()");
    uint32_t layer = 2;
    int8_t subLevel = 0;
    region->GetDrawLayer(&layer, &subLevel);
    RaisePendingError(L, region, "GetDrawLayer");
    int32_t count = PushStringOrNil(L, DrawLayerToString(layer));
    return count + PushInt(L, subLevel);
}

// ref: FUN_140a92310
int GetEffectiveScale(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local effectiveScale = self:GetEffectiveScale()");
    float scale = region->m_layoutScale;
    RaisePendingError(L, region, "GetEffectiveScale");
    return PushFloat(L, scale);
}

// ref: FUN_140a930b0
int GetScale(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local scale = self:GetScale()");
    float scale = region->m_scale;
    RaisePendingError(L, region, "GetScale");
    return PushFloat(L, scale);
}

// ref: FUN_140a93e50
int GetVertexColor(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local r, g, b, a = self:GetVertexColor()");
    uint32_t argb = 0;
    region->GetVertexColor(&argb);
    RaisePendingError(L, region, "GetVertexColor");
    PushFloat(L, static_cast<float>((argb >> 16) & 0xff) * (1.0f / 255.0f));
    PushFloat(L, static_cast<float>((argb >> 8) & 0xff) * (1.0f / 255.0f));
    PushFloat(L, static_cast<float>(argb & 0xff) * (1.0f / 255.0f));
    PushFloat(L, static_cast<float>(argb >> 24) * (1.0f / 255.0f));
    return 4;
}

// ref: FUN_140a95870
int IsIgnoringParentAlpha(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isIgnoring = self:IsIgnoringParentAlpha()");
    bool ignoring = ((region->m_regionFlags >> 1) & 1) != 0;
    RaisePendingError(L, region, "IsIgnoringParentAlpha");
    return PushBool(L, ignoring);
}

// ref: FUN_140a96610
int IsIgnoringParentScale(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isIgnoring = self:IsIgnoringParentScale()");
    bool ignoring = ((region->m_regionFlags >> 2) & 1) != 0;
    RaisePendingError(L, region, "IsIgnoringParentScale");
    return PushBool(L, ignoring);
}

// ref: FUN_140a973b0
int IsObjectLoaded(lua_State* L) {
    int32_t index = 1;
    auto region = RegionSelf(L, &index, "Usage: local isLoaded = self:IsObjectLoaded()");
    bool loaded = region->IsLayoutReady();
    RaisePendingError(L, region, "IsObjectLoaded");
    return PushBool(L, loaded);
}

// ref: FUN_140a98150
int SetAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetAlpha(alpha)";
    auto region = RegionSelf(L, &index, usage);
    uint8_t alpha = RequiredAlpha(L, &index, usage);
    region->SetAlpha(alpha);
    RaisePendingError(L, region, "SetAlpha");
    return 0;
}

// ref: FUN_140a99010
int SetDrawLayer(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetDrawLayer(layer [, sublevel])";
    auto region = RegionSelf(L, &index, usage);
    uint32_t layer = RequiredDrawLayer(L, &index, usage);
    bool present = false;
    int8_t subLevel = OptionalSubLevel(L, &index, usage, &present);
    region->SetDrawLayer(static_cast<uint8_t>(layer), present ? subLevel : 0);
    RaisePendingError(L, region, "SetDrawLayer");
    return 0;
}

// ref: FUN_140a99fd0
int SetIgnoreParentAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetIgnoreParentAlpha(ignore)";
    auto region = RegionSelf(L, &index, usage);
    bool ignore = RequiredBool(L, &index, usage);
    if (static_cast<uint8_t>(ignore) != ((region->m_regionFlags >> 1) & 1)) {
        region->m_regionFlags = static_cast<uint8_t>((region->m_regionFlags & 0xfd) | (ignore ? 2 : 0));
        region->UpdateEffectiveAlpha();
    }
    RaisePendingError(L, region, "SetIgnoreParentAlpha");
    return 0;
}

// ref: FUN_140a9add0
int SetIgnoreParentScale(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetIgnoreParentScale(ignore)";
    auto region = RegionSelf(L, &index, usage);
    bool ignore = RequiredBool(L, &index, usage);
    region->SetIgnoreParentScale(ignore);
    RaisePendingError(L, region, "SetIgnoreParentScale");
    return 0;
}

// ref: FUN_140a9bba0
int SetScale(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetScale(scale)";
    auto region = RegionSelf(L, &index, usage);
    float scale = RequiredFinite(L, &index, usage);
    if (!CanChangeProtected(region)) {
        return 0;
    }
    region->SetScale(scale);
    RaisePendingError(L, region, "SetScale");
    return 0;
}

struct SETVERTEXCOLORCONTEXT {
    CSimpleRegion* m_region;
    uint32_t m_rgb;
    uint8_t m_alpha[2];
};

// ref: FUN_140a569b0
int32_t SetVertexColorBody(lua_State* L, void* context) {
    auto ctx = static_cast<SETVERTEXCOLORCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetVertexColor(color [, a])";
    ctx->m_region = RegionSelf(L, &index, usage);
    ctx->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, ctx->m_alpha);
    ctx->m_region->SetVertexColor(ctx->m_rgb, ctx->m_alpha);
    RaisePendingError(L, ctx->m_region, "SetVertexColor");
    return 0;
}

// ref: FUN_140a9c970
int SetVertexColor(lua_State* L) {
    SETVERTEXCOLORCONTEXT context = {};
    return ProtectedCall(L, SetVertexColorBody, &context);
}

// 0x142cf9550
const luaL_Reg s_regionMethods[] = {
    { "GetAlpha", GetAlpha },
    { "GetDrawLayer", GetDrawLayer },
    { "GetEffectiveScale", GetEffectiveScale },
    { "GetScale", GetScale },
    { "GetVertexColor", GetVertexColor },
    { "IsIgnoringParentAlpha", IsIgnoringParentAlpha },
    { "IsIgnoringParentScale", IsIgnoringParentScale },
    { "IsObjectLoaded", IsObjectLoaded },
    { "SetAlpha", SetAlpha },
    { "SetDrawLayer", SetDrawLayer },
    { "SetIgnoreParentAlpha", SetIgnoreParentAlpha },
    { "SetIgnoreParentScale", SetIgnoreParentScale },
    { "SetScale", SetScale },
    { "SetVertexColor", SetVertexColor },
};

// ---- CSimpleTexture --------------------------------------------------------------------------

// ref: FUN_140ac7520
int GetAtlas(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local atlas = self:GetAtlas()");
    RaisePendingError(L, texture, "GetAtlas");
    // FUN_1407d2c90: the atlas element's name, nil without one
    const char* name = texture->m_atlas != 0 ? TextureAtlasGetName(&texture->m_atlas) : nullptr;
    if (name != nullptr && *name != '\0') {
        lua_pushstring(L, name);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ref: FUN_140ac8300
int GetBlendMode(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local blendMode = self:GetBlendMode()");
    uint8_t mode = texture->m_blendMode;
    RaisePendingError(L, texture, "GetBlendMode");
    return PushStringOrNil(L, BlendModeToString(mode));
}

// ref: FUN_140ac90f0
int GetDesaturation(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local desaturation = self:GetDesaturation()");
    float desaturation = texture->m_desaturation;
    RaisePendingError(L, texture, "GetDesaturation");
    return PushFloat(L, desaturation);
}

// ref: FUN_140ac9e90
int GetHorizTile(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local tiling = self:GetHorizTile()");
    bool tiling = ((texture->m_textureFlags >> 2) & 1) != 0;
    RaisePendingError(L, texture, "GetHorizTile");
    return PushBool(L, tiling);
}

// ref: FUN_140acac30
int GetRotation(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local radians, normalizedRotationPoint = self:GetRotation()");
    float radians = texture->m_rotation;
    float x = texture->m_rotationCenter[0];
    float y = texture->m_rotationCenter[1];
    RaisePendingError(L, texture, "GetRotation");
    PushFloat(L, radians);
    ScriptMethod::PushFromMixin(L, "Vector2DMixin");
    lua_pushnumber(L, x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, y);
    lua_setfield(L, -2, "y");
    return 2;
}

// ref: FUN_1424cb380
int GetTexCoord(lua_State* L) {
    CSimpleTexture* texture = TextureFromLua(L);
    if (texture == nullptr) {
        return 0;
    }
    float texCoords[8] = {};
    texture->GetTexCoords(texCoords);
    for (float value : texCoords) {
        lua_pushnumber(L, value);
    }
    return 8;
}

// ref: FUN_140acc790
int GetTexelSnappingBias(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local bias = self:GetTexelSnappingBias()");
    float bias = texture->m_snappingBias;
    RaisePendingError(L, texture, "GetTexelSnappingBias");
    return PushFloat(L, bias);
}

// The FileDataID first, the path when it has none.
// ref: FUN_1424cb530
int GetTexture(lua_State* L) {
    CSimpleTexture* texture = TextureFromLua(L);
    if (texture != nullptr) {
        int32_t fileDataId = texture->GetFileDataId();
        if (fileDataId != 0) {
            PushFileDataId(L, fileDataId);
            return 1;
        }
        char name[0x30c];
        if (texture->GetTextureName(name, sizeof(name))) {
            lua_pushstring(L, name);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// ref: FUN_140ace1e0
int GetTextureFileID(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local textureFile = self:GetTextureFileID()");
    int32_t fileDataId = texture->GetFileDataId();
    RaisePendingError(L, texture, "GetTextureFileID");
    if (fileDataId != 0) {
        lua_pushnumber(L, static_cast<double>(fileDataId));
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// The path first, the FileDataID when it has none.
// ref: FUN_1424cb460
int GetTextureFilePath(lua_State* L) {
    CSimpleTexture* texture = TextureFromLua(L);
    if (texture != nullptr) {
        char name[0x30c];
        if (texture->GetTextureName(name, sizeof(name))) {
            lua_pushstring(L, name);
            return 1;
        }
        int32_t fileDataId = texture->GetFileDataId();
        if (fileDataId != 0) {
            PushFileDataId(L, fileDataId);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// ref: FUN_140acfc70
int GetVertTile(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local tiling = self:GetVertTile()");
    bool tiling = ((texture->m_textureFlags >> 3) & 1) != 0;
    RaisePendingError(L, texture, "GetVertTile");
    return PushBool(L, tiling);
}

// ref: FUN_140ad0a10
int GetVertexOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local offsetX, offsetY = self:GetVertexOffset(vertexIndex)";
    auto texture = TextureSelf(L, &index, usage);
    uint32_t vertex = RequiredIndex(L, &index, usage);
    float x = 0.0f;
    float y = 0.0f;
    texture->GetVertexOffset(vertex, &x, &y);
    RaisePendingError(L, texture, "GetVertexOffset");
    lua_pushnumber(L, (1.0f * TEXELS_PER_SCREEN * x) / UI_WIDTH);
    lua_pushnumber(L, (1.0f * TEXELS_PER_SCREEN * y) / UI_WIDTH);
    return 2;
}

// ref: FUN_140ad1890
int IsBlockingLoadRequested(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local blocking = self:IsBlockingLoadRequested()");
    bool blocking = (texture->m_textureFlags & 1) != 0;
    RaisePendingError(L, texture, "IsBlockingLoadRequested");
    return PushBool(L, blocking);
}

// ref: FUN_140ad2630
int IsDesaturated(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local desaturated = self:IsDesaturated()");
    bool desaturated = 0.0f < texture->m_desaturation;
    RaisePendingError(L, texture, "IsDesaturated");
    return PushBool(L, desaturated);
}

// ref: FUN_140ad33d0
int IsSnappingToPixelGrid(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local snap = self:IsSnappingToPixelGrid()");
    bool snap = ((texture->m_textureFlags >> 0x11) & 1) != 0;
    RaisePendingError(L, texture, "IsSnappingToPixelGrid");
    return PushBool(L, snap);
}

// ("atlasName"[, useAtlasSize, filterMode, resetTexCoords]); true when it took.
// ref: FUN_1424cb610
int SetAtlas(lua_State* L) {
    CSimpleTexture* texture = TextureFromLua(L);
    if (texture == nullptr) {
        return 0;
    }
    if (lua_type(L, 2) == LUA_TNIL) {
        texture->SetTextureByName(nullptr, 0, 0, 1, true, false);
        return 0;
    }
    if (!lua_isstring(L, 2)) {
        FrameScript_ObjectError(texture, "SetAtlas", "Usage: (\"atlasName\"[, useAtlasSize, filterMode, resetTexCoords])");
        return 0;
    }
    const char* name = lua_tolstring(L, 2, nullptr);
    bool useAtlasSize = lua_toboolean(L, 3) != 0;
    bool resetTexCoords = lua_toboolean(L, 5) != 0;
    TEXTUREATLASSCALE scale = { CSimpleTop::s_instance->GetRegionLayoutVersion(texture), 1 };
    int32_t atlas = 0;
    TextureAtlasFindByName(&atlas, name, &scale);
    if (atlas == 0) {
        return 0;
    }
    uint8_t filter = 1;
    if (lua_isstring(L, 4)) {
        StringToFilterMode(lua_tolstring(L, 4, nullptr), &filter);
    }
    uint32_t flags = texture->m_textureFlags;
    if (!texture->SetAtlas(&atlas, filter, resetTexCoords, static_cast<uint8_t>((flags >> 4) & 7),
            static_cast<uint8_t>((flags >> 7) & 7))) {
        return 0;
    }
    if (useAtlasSize) {
        float size[2] = {};
        TextureAtlasGetSize(&atlas, size);
        float toUi = 1.0f / (1.0f * TEXELS_PER_SCREEN);
        texture->SetSize(toUi * size[0] * UI_WIDTH, toUi * size[1] * UI_WIDTH);
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ref: FUN_140ad4e20
int SetBlendMode(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetBlendMode(blendMode)";
    auto texture = TextureSelf(L, &index, usage);
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    }
    uint8_t mode = 2;
    if (lua_type(L, index) != LUA_TSTRING || !StringToBlendMode(lua_tolstring(L, index, nullptr), &mode)) {
        luaL_argerror(L, index, usage);
        mode = 2;
    }
    texture->SetBlendMode(mode);
    RaisePendingError(L, texture, "SetBlendMode");
    return 0;
}

// ref: FUN_140ad5cd0
int SetBlockingLoadsRequested(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: self:SetBlockingLoadsRequested([blocking])");
    bool blocking = OptionalBool(L, &index);
    texture->m_textureFlags = (texture->m_textureFlags & 0xfffffffe) | (blocking ? 1 : 0);
    RaisePendingError(L, texture, "SetBlockingLoadsRequested");
    return 0;
}

struct SETCOLORTEXTURECONTEXT {
    CSimpleTexture* m_texture;
    uint32_t m_rgb;
    uint8_t m_alpha[2];
};

// ref: FUN_140a5ad60
int32_t SetColorTextureBody(lua_State* L, void* context) {
    auto ctx = static_cast<SETCOLORTEXTURECONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetColorTexture(color [, a])";
    ctx->m_texture = TextureSelf(L, &index, usage);
    ctx->m_rgb = RequiredRGB(L, &index, usage);
    OptionalAlphaByte(L, &index, usage, ctx->m_alpha);
    // FUN_140634970: the alpha byte, or opaque without one
    uint32_t argb = (ctx->m_rgb & 0x00ffffff) | (static_cast<uint32_t>(ctx->m_alpha[1] != 0 ? ctx->m_alpha[0] : 0xff) << 24);
    ctx->m_texture->SetColorTexture(&argb);
    RaisePendingError(L, ctx->m_texture, "SetColorTexture");
    return 0;
}

// ref: FUN_140ad6aa0
int SetColorTexture(lua_State* L) {
    SETCOLORTEXTURECONTEXT context = {};
    return ProtectedCall(L, SetColorTextureBody, &context);
}

// ref: FUN_140ad77d0
int SetDesaturated(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: self:SetDesaturated([desaturated])");
    bool desaturated = OptionalBool(L, &index);
    texture->SetDesaturation(desaturated ? 1.0f : 0.0f);
    RaisePendingError(L, texture, "SetDesaturated");
    return 0;
}

// ref: FUN_140ad85b0
int SetDesaturation(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetDesaturation(desaturation)";
    auto texture = TextureSelf(L, &index, usage);
    float desaturation = RequiredUnit(L, &index, usage);
    texture->SetDesaturation(desaturation);
    RaisePendingError(L, texture, "SetDesaturation");
    return 0;
}

// ref: FUN_140ad9380
int SetGradient(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetGradient(orientation, minColor, maxColor)";
    auto texture = TextureSelf(L, &index, usage);
    uint32_t orientation = RequiredOrientation(L, &index, usage);
    uint32_t minColor = RequiredColorTable(L, &index, usage);
    uint32_t maxColor = RequiredColorTable(L, &index, usage);
    texture->SetGradient(static_cast<int32_t>(orientation), &minColor, &maxColor);
    RaisePendingError(L, texture, "SetGradient");
    return 0;
}

// ref: FUN_140ada1a0
int SetHorizTile(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: self:SetHorizTile([tiling])");
    bool tiling = OptionalBool(L, &index);
    texture->m_textureFlags = (texture->m_textureFlags & 0xfffffffb) | (tiling ? 4 : 0);
    RaisePendingError(L, texture, "SetHorizTile");
    return 0;
}

// ref: FUN_140adaf70
int SetMask(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetMask(file)";
    auto texture = TextureSelf(L, &index, usage);
    const char* file = RequiredString(L, &index, usage);
    texture->SetMaskFile(file);
    RaisePendingError(L, texture, "SetMask");
    return 0;
}

struct SETROTATIONCONTEXT {
    CSimpleTexture* m_texture;
    float m_radians;
    float m_center[2];
    uint8_t m_hasCenter;
};

// ref: FUN_140a5bb60
int32_t SetRotationBody(lua_State* L, void* context) {
    auto ctx = static_cast<SETROTATIONCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetRotation(radians [, normalizedRotationPoint])";
    ctx->m_texture = TextureSelf(L, &index, usage);
    ctx->m_radians = RequiredFinite(L, &index, usage);
    ctx->m_hasCenter = OptionalPoint(L, &index, usage, ctx->m_center) ? 1 : 0;
    ctx->m_texture->SetRotation(ctx->m_radians, ctx->m_hasCenter != 0 ? ctx->m_center : nullptr);
    RaisePendingError(L, ctx->m_texture, "SetRotation");
    return 0;
}

// ref: FUN_140adbdc0
int SetRotation(lua_State* L) {
    SETROTATIONCONTEXT context = {};
    return ProtectedCall(L, SetRotationBody, &context);
}

// ref: FUN_140adcaf0
int SetSnapToPixelGrid(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: self:SetSnapToPixelGrid([snap])");
    bool snap = OptionalBool(L, &index);
    texture->m_textureFlags = (texture->m_textureFlags & 0xfffdffff) | (snap ? 0x20000u : 0);
    RaisePendingError(L, texture, "SetSnapToPixelGrid");
    return 0;
}

// (minX, maxX, minY, maxY) or the four corners.
// ref: FUN_1424cb850
int SetTexCoord(lua_State* L) {
    CSimpleTexture* texture = TextureFromLua(L);
    if (texture == nullptr) {
        return 0;
    }
    if (texture->HasMaskFile()) {
        FrameScript_ObjectError(texture, "SetTextCoord", "Cannot set tex coords when texture has mask.");
        return 0;
    }
    int32_t count = lua_gettop(L);
    const char* error;
    if (count == 5) {
        // FUN_1424d42d0: left, right, top, bottom into {top, left, bottom, right}
        float rect[4];
        rect[1] = static_cast<float>(lua_tonumber(L, 2));
        rect[3] = static_cast<float>(lua_tonumber(L, 3));
        rect[0] = static_cast<float>(lua_tonumber(L, 4));
        rect[2] = static_cast<float>(lua_tonumber(L, 5));
        bool inRange = true;
        for (float value : rect) {
            if (value < -10000.0f || 10000.0f < value) {
                inRange = false;
            }
        }
        if (inRange) {
            texture->SetTexCoordsRect(rect);
            return 0;
        }
        error = "TexCoord out of range";
    } else if (count == 9) {
        float texCoords[8] = {};
        bool inRange = true;
        for (int32_t i = 0; i < 8; i++) {
            texCoords[i] = static_cast<float>(lua_tonumber(L, 2 + i));
            if (texCoords[i] < -10000.0f || 10000.0f < texCoords[i]) {
                inRange = false;
                break;
            }
        }
        if (inRange) {
            texture->SetTexCoords(texCoords);
            return 0;
        }
        error = "TexCoord out of range";
    } else {
        error = "Usage (minX, maxX, minY, maxY) or (ULx, ULy, LLx, LLy, URx, URy, LRx, LRy)";
    }
    FrameScript_ObjectError(texture, "SetTextCoord", error);
    return 0;
}

// ref: FUN_140ade570
int SetTexelSnappingBias(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetTexelSnappingBias(bias)";
    auto texture = TextureSelf(L, &index, usage);
    float bias = RequiredUnit(L, &index, usage);
    texture->SetSnappingBias(bias);
    RaisePendingError(L, texture, "SetTexelSnappingBias");
    return 0;
}

// (file or FileDataID[, wrapModeHorizontal, wrapModeVertical, filterMode]); whether it took.
// ref: FUN_1424cba70
int SetTexture(lua_State* L) {
    CSimpleTexture* texture = TextureFromLua(L);
    if (texture == nullptr) {
        return 0;
    }
    uint8_t wrapH = 0;
    uint8_t wrapV = 0;
    uint8_t filter = 1;
    if (lua_isstring(L, 3)) {
        StringToWrapMode(lua_tolstring(L, 3, nullptr), &wrapH);
    } else {
        wrapH = lua_toboolean(L, 3) != 0;
    }
    if (lua_isstring(L, 4)) {
        StringToWrapMode(lua_tolstring(L, 4, nullptr), &wrapV);
    } else {
        wrapV = lua_toboolean(L, 4) != 0;
    }
    if (lua_isstring(L, 5)) {
        StringToFilterMode(lua_tolstring(L, 5, nullptr), &filter);
    }
    bool loaded = true;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        loaded = texture->SetTextureById(static_cast<int32_t>(lua_tointeger(L, 2)), wrapH, wrapV, filter, true, false);
    } else if (lua_isstring(L, 2)) {
        loaded = texture->SetTextureByName(lua_tolstring(L, 2, nullptr), wrapH, wrapV, filter, true, false);
    } else {
        texture->SetTextureByName(nullptr, 0, 0, 1, true, false);
    }
    lua_pushboolean(L, loaded);
    return 1;
}

// ref: FUN_140adfff0
int SetVertTile(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: self:SetVertTile([tiling])");
    bool tiling = OptionalBool(L, &index);
    texture->m_textureFlags = (texture->m_textureFlags & 0xfffffff7) | (tiling ? 8 : 0);
    RaisePendingError(L, texture, "SetVertTile");
    return 0;
}

// ref: FUN_140ae0dc0
int SetVertexOffset(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetVertexOffset(vertexIndex, offsetX, offsetY)";
    auto texture = TextureSelf(L, &index, usage);
    uint32_t vertex = RequiredIndex(L, &index, usage);
    float toUi = (1.0f / 1024.0f) / 1.0f * UI_WIDTH;
    float x = RequiredFinite(L, &index, usage) * toUi;
    float y = RequiredFinite(L, &index, usage) * toUi;
    texture->SetVertexOffset(vertex, &x, &y);
    RaisePendingError(L, texture, "SetVertexOffset");
    return 0;
}

// 0x142cf9cb0
const luaL_Reg s_textureMethods[] = {
    { "GetAtlas", GetAtlas },
    { "GetBlendMode", GetBlendMode },
    { "GetDesaturation", GetDesaturation },
    { "GetHorizTile", GetHorizTile },
    { "GetRotation", GetRotation },
    { "GetTexCoord", GetTexCoord },
    { "GetTexelSnappingBias", GetTexelSnappingBias },
    { "GetTexture", GetTexture },
    { "GetTextureFileID", GetTextureFileID },
    { "GetTextureFilePath", GetTextureFilePath },
    { "GetVertTile", GetVertTile },
    { "GetVertexOffset", GetVertexOffset },
    { "IsBlockingLoadRequested", IsBlockingLoadRequested },
    { "IsDesaturated", IsDesaturated },
    { "IsSnappingToPixelGrid", IsSnappingToPixelGrid },
    { "SetAtlas", SetAtlas },
    { "SetBlendMode", SetBlendMode },
    { "SetBlockingLoadsRequested", SetBlockingLoadsRequested },
    { "SetColorTexture", SetColorTexture },
    { "SetDesaturated", SetDesaturated },
    { "SetDesaturation", SetDesaturation },
    { "SetGradient", SetGradient },
    { "SetHorizTile", SetHorizTile },
    { "SetMask", SetMask },
    { "SetRotation", SetRotation },
    { "SetSnapToPixelGrid", SetSnapToPixelGrid },
    { "SetTexCoord", SetTexCoord },
    { "SetTexelSnappingBias", SetTexelSnappingBias },
    { "SetTexture", SetTexture },
    { "SetVertTile", SetVertTile },
    { "SetVertexOffset", SetVertexOffset },
};

// ---- the masks -------------------------------------------------------------------------------

CSimpleMaskTexture* MaskArgument(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleMaskTexture*>(Self(L, CSimpleMaskTexture::GetObjectType(), index, usage));
}

// ref: FUN_140ae1de0
int AddMaskTexture(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:AddMaskTexture(mask)";
    auto texture = TextureSelf(L, &index, usage);
    CSimpleMaskTexture* mask = MaskArgument(L, &index, usage);
    texture->AddMaskTexture(mask);
    RaisePendingError(L, texture, "AddMaskTexture");
    return 0;
}

// ref: FUN_140ae2c60
int GetMaskTexture(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local mask = self:GetMaskTexture(index)";
    auto texture = TextureSelf(L, &index, usage);
    uint32_t which = RequiredIndex(L, &index, usage);
    CSimpleMaskTexture* mask = texture->GetMaskTexture(which);
    RaisePendingError(L, texture, "GetMaskTexture");
    return PushObject(L, reinterpret_cast<FrameScript_Object*>(mask));
}

// ref: FUN_140ae3aa0
int GetNumMaskTextures(lua_State* L) {
    int32_t index = 1;
    auto texture = TextureSelf(L, &index, "Usage: local count = self:GetNumMaskTextures()");
    uint64_t count = texture->GetNumMaskTextures();
    RaisePendingError(L, texture, "GetNumMaskTextures");
    lua_pushnumber(L, static_cast<double>(count));
    return 1;
}

// ref: FUN_140ae4890
int RemoveMaskTexture(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:RemoveMaskTexture(mask)";
    auto texture = TextureSelf(L, &index, usage);
    CSimpleMaskTexture* mask = MaskArgument(L, &index, usage);
    texture->RemoveMaskTexture(mask);
    RaisePendingError(L, texture, "RemoveMaskTexture");
    return 0;
}

// 0x142cf9c70
const luaL_Reg s_maskMethods[] = {
    { "AddMaskTexture", AddMaskTexture },
    { "GetMaskTexture", GetMaskTexture },
    { "GetNumMaskTextures", GetNumMaskTextures },
    { "RemoveMaskTexture", RemoveMaskTexture },
};

}

// The Region methods (0x142cf9550) every drawable region has.
// ref: FUN_140ae5a20
void CSimpleRegion_FillScriptMethods(lua_State* L) {
    FrameScript_Object::FillScriptMethodTable(L, s_regionMethods, sizeof(s_regionMethods) / sizeof(s_regionMethods[0]));
}

// ref: FUN_140ae5a40
void CSimpleTexture_FillBaseScriptMethods(lua_State* L) {
    CScriptRegion_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_regionMethods, sizeof(s_regionMethods) / sizeof(s_regionMethods[0]));
    FrameScript_Object::FillScriptMethodTable(L, s_textureMethods, sizeof(s_textureMethods) / sizeof(s_textureMethods[0]));
}

// ref: FUN_140ae59b0
void CSimpleTexture_FillScriptMethods(lua_State* L) {
    CAnimatableObject_FillScriptMethods(L);
    CSimpleTexture_FillBaseScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_maskMethods, sizeof(s_maskMethods) / sizeof(s_maskMethods[0]));
}

// The MaskTexture's methods are the Texture's (FUN_140a510b0).
// ref: FUN_140a510b0
void CSimpleMaskTexture_FillScriptMethods(lua_State* L) {
    CAnimatableObject_FillScriptMethods(L);
    CSimpleTexture_FillBaseScriptMethods(L);
}

// ref: FUN_1408a7520
bool TextureSourceFromLua(lua_State* L, int32_t index, TEXTURESOURCE* source) {
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

// ref: FUN_1407ff990
TEXTURESOURCE TextureSourceArgument(lua_State* L, int32_t* index, const char* usage) {
    TEXTURESOURCE source = TEXTURESOURCE::FromFileId(0);
    if (lua_type(L, *index) < 1) {
        luaL_error(L, usage);
        return source;
    }
    if (!TextureSourceFromLua(L, *index, &source)) {
        luaL_argerror(L, *index, usage);
        return source;
    }
    (*index)++;
    return source;
}
