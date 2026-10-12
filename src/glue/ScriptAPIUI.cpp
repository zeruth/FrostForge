// The UI queries the glue reaches: the screen in UI units, a template's description
// (C_XMLUtil), an atlas member's description (C_Texture) and the game window's size
// (C_VideoOptions).

#include "client/ConsoleDevice.hpp"
#include "client/GxWindow.hpp"
#include "gx/Gx.hpp"
#include "base/Coordinate.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/ScriptMethod.hpp"
#include "glue/ScriptAPIHandlers.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CLayoutFrame.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/FrameXML.hpp"
#include "ui/TextureAtlas.hpp"
#include <cstring>
#include <optional>
#include <windows.h>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

// UIParent once the game's FrameXML has made it (DAT_1431e7d48); the glue has none.
CSimpleFrame* g_uiParent;

namespace {

// The screen in UI units (DAT_142ccb9f4, DAT_142ccb9f8), the aspect compensation
// (DAT_142ccb9fc) and the UI's 1024 units across (DAT_14261a9c4).
const float& UI_WIDTH = g_coordinateSpace.m_diagonalWidth;
const float& UI_HEIGHT = g_coordinateSpace.m_diagonalHeight;
const float& UI_ASPECT = g_coordinateSpace.m_aspectCompensation;
constexpr float UI_UNITS = 1024.0f;

// UIParent's scale, 1 without it.
// ref: FUN_14191f540
float UiParentScale() {
    if (g_uiParent != nullptr) {
        return static_cast<CLayoutFrame*>(g_uiParent)->m_layoutScale;
    }
    return 1.0f;
}

struct XMLTEMPLATEKEYVALUE {
    const char* m_key;
    const char* m_keyType;
    const char* m_type;
    const char* m_value;
};

struct XMLTEMPLATEINFO {
    const char* m_type;             // the element the template is
    float m_width;
    float m_height;
    std::vector<XMLTEMPLATEKEYVALUE> m_keyValues;
    std::optional<const char*> m_inherits;
};

// The <KeyValue>s directly under a <KeyValues> that carry both a key and a value.
// ref: FUN_140665440
void TemplateGetKeyValues(XMLOBJECT* keyValues, std::vector<XMLTEMPLATEKEYVALUE>* out) {
    for (XMLOBJECT* child = keyValues->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type != XMLTYPE_KEYVALUE) {
            continue;
        }
        auto keyValue = static_cast<XMLKEYVALUE*>(child);
        uint32_t flags = keyValue->m_keyValueFlags;
        if ((flags & 5) != 5) {
            continue;
        }
        XMLTEMPLATEKEYVALUE entry;
        entry.m_key = keyValue->m_key.m_text;
        entry.m_value = keyValue->m_value.m_text;
        entry.m_keyType = (flags & 2) != 0 ? keyValue->m_keyType.m_text : "string";
        entry.m_type = (flags & 8) != 0 ? keyValue->m_valueType.m_text : "string";
        out->push_back(entry);
    }
}

// A template's <Size>, or the size the templates it inherits give it (later ones win).
// ref: FUN_1406651d0
void TemplateGetSize(XMLOBJECT* node, float* width, float* height) {
    auto size = static_cast<XMLDIMENSION*>(XMLObjectFindChild(node, XMLTYPE_SIZE));
    if (size != nullptr) {
        CScriptStatus status;
        float x = 0.0f;
        float y = 0.0f;
        if (XMLDimensionGet(size, &x, &y, &status)) {
            *width = x;
            *height = y;
        }
        return;
    }
    const char* cursor = node->m_inherits.m_text;
    if (cursor == nullptr) {
        return;
    }
    char token[0x100];
    for (;;) {
        SStrTokenize(&cursor, token, sizeof(token), ", \t", nullptr);
        if (token[0] == '\0') {
            return;
        }
        const char* taint = nullptr;
        bool busy = false;
        XMLOBJECT* parent = FrameXML_AcquireVirtual(token, &taint, &busy);
        if (parent != nullptr) {
            TemplateGetSize(parent, width, height);
            FrameXML_ReleaseVirtual(token);
        }
    }
}

// ref: FUN_140663640
bool FrameXML_GetTemplateInfo(const char* name, XMLTEMPLATEINFO* info) {
    const char* taint = nullptr;
    bool busy = false;
    XMLOBJECT* node = FrameXML_AcquireVirtual(name, &taint, &busy);
    if (node == nullptr) {
        return false;
    }
    info->m_type = node->m_tag.m_text;
    if (node->m_inherits.m_text != nullptr) {
        info->m_inherits = node->m_inherits.m_text;
    }
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_KEYVALUES) {
            TemplateGetKeyValues(child, &info->m_keyValues);
        }
    }
    float width = 0.0f;
    float height = 0.0f;
    TemplateGetSize(node, &width, &height);
    info->m_width = (UI_ASPECT * UI_UNITS * width) / UI_WIDTH;
    info->m_height = (UI_ASPECT * UI_UNITS * height) / UI_WIDTH;
    FrameXML_ReleaseVirtual(name);
    return true;
}

void PushOptionalString(lua_State* L, const char* text) {
    if (text == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, text);
    }
}

const char* NameArgument(lua_State* L, const char* usage) {
    if (lua_gettop(L) < 1) {
        luaL_error(L, usage);
    } else if (!lua_isstring(L, 1)) {
        luaL_argerror(L, 1, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        return lua_tolstring(L, 1, nullptr);
    }
    return nullptr;
}

// ref: FUN_14115a4b0
int32_t GetTemplateInfoBody(lua_State* L, void*) {
    const char* name = NameArgument(L, "Usage: local info = C_XMLUtil.GetTemplateInfo(name)");
    XMLTEMPLATEINFO info = {};
    if (!FrameXML_GetTemplateInfo(name, &info)) {
        return 0;
    }
    lua_createtable(L, 0, 5);
    int32_t table = lua_gettop(L);
    PushOptionalString(L, info.m_type);
    lua_setfield(L, table, "type");
    lua_pushnumber(L, info.m_width);
    lua_setfield(L, table, "width");
    lua_pushnumber(L, info.m_height);
    lua_setfield(L, table, "height");
    // FUN_14115a160
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_createtable(L, static_cast<int32_t>(info.m_keyValues.size()), 0);
    int32_t list = lua_gettop(L);
    int32_t index = 0;
    for (const XMLTEMPLATEKEYVALUE& entry : info.m_keyValues) {
        index++;
        lua_pushinteger(L, index);
        lua_createtable(L, 0, 4);
        int32_t row = lua_gettop(L);
        PushOptionalString(L, entry.m_key);
        lua_setfield(L, row, "key");
        PushOptionalString(L, entry.m_keyType);
        lua_setfield(L, row, "keyType");
        PushOptionalString(L, entry.m_type);
        lua_setfield(L, row, "type");
        PushOptionalString(L, entry.m_value);
        lua_setfield(L, row, "value");
        lua_settable(L, list);
    }
    lua_setfield(L, table, "keyValues");
    // FUN_14115a2d0
    PushOptionalString(L, info.m_inherits.has_value() ? *info.m_inherits : nullptr);
    lua_setfield(L, table, "inherits");
    return 1;
}

struct ATLASINFO {
    double m_width;
    double m_height;
    double m_left;
    double m_right;
    double m_top;
    double m_bottom;
    bool m_tilesHorizontally;
    bool m_tilesVertically;
    std::optional<int32_t> m_file;
    std::optional<std::string> m_filename;
};

// ref: FUN_141cf1ef0
bool TextureAtlasGetInfo(const int32_t* member, ATLASINFO* info) {
    if (*member == 0) {
        return false;
    }
    char path[0x104];
    if (TextureAtlasGetPath(member, path, sizeof(path))) {
        info->m_filename = path;
    }
    int32_t file = TextureAtlasGetFileDataId(member);
    if (file != 0) {
        info->m_file = file;
    }
    static const float s_defaultTexCoords[8] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
    float texCoords[8] = {};
    TextureAtlasMapTexCoords(member, texCoords, s_defaultTexCoords);
    float size[2] = {};
    TextureAtlasGetSize(member, size);
    info->m_width = size[0];
    info->m_height = size[1];
    info->m_left = texCoords[0];
    info->m_right = texCoords[6];
    info->m_top = texCoords[1];
    info->m_bottom = texCoords[7];
    info->m_tilesHorizontally = TextureAtlasTilesHorizontally(member);
    info->m_tilesVertically = TextureAtlasTilesVertically(member);
    return true;
}

// ref: FUN_140e5ae70
int32_t GetAtlasInfoBody(lua_State* L, void*) {
    const char* usage = "Usage: local info = C_Texture.GetAtlasInfo(atlas)";
    int32_t member = 0;
    if (lua_gettop(L) < 1) {
        luaL_error(L, usage);
    } else if (!lua_isstring(L, 1)) {
        luaL_argerror(L, 1, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        // FUN_1407d1140
        TEXTUREATLASSCALE scale = { 0, 0 };
        TextureAtlasFindByName(&member, lua_tolstring(L, 1, nullptr), &scale);
    }
    ATLASINFO info = {};
    if (!TextureAtlasGetInfo(&member, &info)) {
        return 0;
    }
    // FUN_140e6a150
    lua_createtable(L, 0, 10);
    int32_t table = lua_gettop(L);
    lua_pushnumber(L, info.m_width);
    lua_setfield(L, table, "width");
    lua_pushnumber(L, info.m_height);
    lua_setfield(L, table, "height");
    lua_pushnumber(L, info.m_left);
    lua_setfield(L, table, "leftTexCoord");
    lua_pushnumber(L, info.m_right);
    lua_setfield(L, table, "rightTexCoord");
    lua_pushnumber(L, info.m_top);
    lua_setfield(L, table, "topTexCoord");
    lua_pushnumber(L, info.m_bottom);
    lua_setfield(L, table, "bottomTexCoord");
    lua_pushboolean(L, info.m_tilesHorizontally);
    lua_setfield(L, table, "tilesHorizontally");
    lua_pushboolean(L, info.m_tilesVertically);
    lua_setfield(L, table, "tilesVertically");
    if (info.m_file.has_value()) {
        lua_pushnumber(L, static_cast<double>(*info.m_file));
    } else {
        lua_pushnil(L);
    }
    lua_setfield(L, table, "file");
    PushOptionalString(L, info.m_filename.has_value() ? info.m_filename->c_str() : nullptr);
    lua_setfield(L, table, "filename");
    return 1;
}

}

// ref: FUN_141d053c0
int Lua_GetScreenWidth(lua_State* L) {
    float scale = UiParentScale();
    lua_pushnumber(L, static_cast<double>((UI_ASPECT * UI_UNITS * (UI_WIDTH / scale)) / UI_WIDTH));
    return 1;
}

// ref: FUN_141d05420
int Lua_GetScreenHeight(lua_State* L) {
    float scale = UiParentScale();
    lua_pushnumber(L, static_cast<double>((UI_ASPECT * UI_UNITS * (UI_HEIGHT / scale)) / UI_WIDTH));
    return 1;
}

// ref: FUN_14115c120
int Lua_C_XMLUtil_GetTemplateInfo(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetTemplateInfoBody, nullptr);
}

// ref: FUN_140ea7200
int Lua_C_Texture_GetAtlasInfo(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetAtlasInfoBody, nullptr);
}

// The window's back buffer size, as a Vector2DMixin (FUN_1419f6010, FUN_1407caed0). The
// monitor and fullscreen arguments are not read.
// ref: FUN_140b6d590
int Lua_C_VideoOptions_GetCurrentGameWindowSize(lua_State* L) {
    GXSIZE size;
    WindowGetResolution(&size);
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    ScriptMethod::PushFromMixin(L, "Vector2DMixin");
    int32_t table = lua_gettop(L);
    lua_pushnumber(L, static_cast<double>(static_cast<float>(size.m_width)));
    lua_setfield(L, table, "x");
    lua_pushnumber(L, static_cast<double>(static_cast<float>(size.m_height)));
    lua_setfield(L, table, "y");
    return 1;
}

// ref: FUN_1419f4be0
int Lua_GetMonitorCount(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(GxMonitorCount()));
    return 1;
}

// The name a monitor would have; the reference's lookup (FUN_14196fd00) is the shared empty
// body, so nothing is returned.
// ref: FUN_1419f4cb0
int Lua_GetMonitorName(lua_State* L) {
    if (!lua_isnumber(L, 1)) {
        return 0;
    }
    uint32_t index = static_cast<uint32_t>(static_cast<int64_t>(lua_tonumber(L, 1))) - 1;
    if (index > GxMonitorCount()) {
        return 0;
    }
    return 0;
}

// ref: FUN_1419f4c10
int Lua_GetMonitorAspectRatio(lua_State* L) {
    uint32_t index = 0;
    if (lua_isnumber(L, 1)) {
        index = static_cast<uint32_t>(static_cast<int64_t>(lua_tonumber(L, 1))) - 1;
    }
    GXSIZE size = {};
    uint32_t bitsPerPixel = 0;
    // FUN_142290bc0
    if (!GxMonitorGetDesktopSize(index, &size, &bitsPerPixel)) {
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(static_cast<float>(size.m_width) / static_cast<float>(size.m_height)));
    return 1;
}

namespace {

// C2Vector.
struct WINDOWSIZEVECTOR {
    float x;
    float y;
};

// The sizes GetGameWindowSizes last handed out (DAT_1431f6680).
blz::vector<GXSIZE, s_gxSizeTag> s_gameWindowSizes;

// The window sizes for a monitor, windowed or fullscreen: its fullscreen sizes, as vectors.
// ref: FUN_1419f60b0
std::vector<WINDOWSIZEVECTOR> GameWindowSizes(uint32_t monitor, bool fullscreen) {
    GetFullscreenSizes(&s_gameWindowSizes, monitor, fullscreen);
    std::vector<WINDOWSIZEVECTOR> sizes;
    sizes.reserve(0x1e);
    for (uint64_t i = 0; i < s_gameWindowSizes.size(); i++) {
        sizes.push_back({static_cast<float>(s_gameWindowSizes[i].m_width),
            static_cast<float>(s_gameWindowSizes[i].m_height)});
    }
    return sizes;
}

// ref: FUN_140b4c160
int32_t GetGameWindowSizesBody(lua_State* L, void*) {
    const char* usage = "Usage: local sizes = C_VideoOptions.GetGameWindowSizes(monitor, fullscreen)";
    uint32_t monitor = 0;
    int32_t index = 1;
    // FUN_140a54180: an integer in range
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else {
        double value = lua_isnumber(L, index) ? lua_tonumber(L, index) : -1.0;
        if (!lua_isnumber(L, index) || value != value || value < 0.0 || value > 4294967295.0) {
            luaL_argerror(L, index, usage);
        } else if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else {
            monitor = static_cast<uint32_t>(static_cast<int64_t>(value));
            index++;
        }
    }
    bool fullscreen = false;
    if (lua_type(L, index) < 1) {
        luaL_error(L, usage);
    } else if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    } else {
        fullscreen = lua_toboolean(L, index) != 0;
    }
    std::vector<WINDOWSIZEVECTOR> sizes = GameWindowSizes(monitor, fullscreen);
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 1;
    }
    // FUN_1407d0e40
    lua_createtable(L, static_cast<int32_t>(sizes.size()), 0);
    int32_t table = lua_gettop(L);
    for (uint64_t i = 0; i < sizes.size(); i++) {
        lua_pushnumber(L, static_cast<double>(i + 1));
        ScriptMethod::PushFromMixin(L, "Vector2DMixin");
        int32_t entry = lua_gettop(L);
        lua_pushnumber(L, static_cast<double>(sizes[i].x));
        lua_setfield(L, entry, "x");
        lua_pushnumber(L, static_cast<double>(sizes[i].y));
        lua_setfield(L, entry, "y");
        lua_settable(L, table);
    }
    return 1;
}

}  // namespace

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
// translated: FUN_140b1c450
unsigned long long FUN_140b1c450(unsigned long long L, unsigned long long details);
// translated: FUN_140ba20c0
unsigned long long FUN_140ba20c0(unsigned long long details);
}

namespace {

// The body GetGxAdapterInfo runs protected: the reference's own (FUN_140b4caf0 through its
// thunk), translated, filling the adapter details it is handed.
int32_t GetGxAdapterInfoBody(lua_State* L, void* details) {
    __try {
        return static_cast<int32_t>(FUN_140b1c450(reinterpret_cast<unsigned long long>(L),
                                                  reinterpret_cast<unsigned long long>(details)));
    } __except (GenFault(0x140b4caf0ull, GetExceptionInformation())) {
        return 0;
    }
}

}  // namespace

// The graphics adapters, for the settings' adapter list: the body run protected with a
// GxAdapterInfoDetails vector (0x18 bytes) it fills, which is freed after. (Ported from the
// instructions: the decompilation of this handler is damaged by its anti-tamper check.)
// ref: FUN_140b6f0f0
int Lua_C_VideoOptions_GetGxAdapterInfo(lua_State* L) {
    alignas(8) uint64_t details[3] = {};
    int results = ScriptMethod::ProtectedCall(L, GetGxAdapterInfoBody, details);
    __try {
        FUN_140ba20c0(reinterpret_cast<unsigned long long>(details));
    } __except (GenFault(0x140ba20c0ull, GetExceptionInformation())) {
    }
    return results;
}

// The default window size for a monitor, as a Vector2D: its default fullscreen size
// (FUN_1419f6050), pushed as FUN_1407caed0 pushes a C2Vector. (Ported from the instructions:
// the decompilation of this handler is damaged by its anti-tamper check.)
// ref: FUN_140b6dc00
int Lua_C_VideoOptions_GetDefaultGameWindowSize(lua_State* L) {
    const char* usage = "Usage: local size = C_VideoOptions.GetDefaultGameWindowSize(monitor)";
    uint32_t monitor = 0;
    // FUN_140a54180: an integer in range
    if (lua_type(L, 1) < 1) {
        luaL_error(L, usage);
    } else {
        double value = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : -1.0;
        if (!lua_isnumber(L, 1) || value != value || value < 0.0 || value > 4294967295.0) {
            luaL_argerror(L, 1, usage);
        } else if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
        } else {
            monitor = static_cast<uint32_t>(static_cast<int64_t>(value));
        }
    }
    GXSIZE size = DefaultFullscreenSize(GxSelectGpuToUse(), monitor);
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    ScriptMethod::PushFromMixin(L, "Vector2DMixin");
    int32_t entry = lua_gettop(L);
    lua_pushnumber(L, static_cast<double>(static_cast<float>(size.m_width)));
    lua_setfield(L, entry, "x");
    lua_pushnumber(L, static_cast<double>(static_cast<float>(size.m_height)));
    lua_setfield(L, entry, "y");
    return 1;
}

// ref: FUN_140b6e390
int Lua_C_VideoOptions_GetGameWindowSizes(lua_State* L) {
    return ScriptMethod::ProtectedCall(L, GetGameWindowSizesBody, nullptr);
}
