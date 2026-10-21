// The frame system's script globals (0x142d5e590, registered by the glue and the game alike):
// CreateFrame and CreateFont, GetText, EnumerateFrames, DoesTemplateExist,
// GetFramesRegisteredForEvent, the keyboard focus and the class metatables.

#include <cstring>

#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptEvents.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CSimpleFont.hpp"
#include "ui/CSimpleFontString.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "ui/XMLTree.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

// The edit box holding the keyboard focus (0x142e97b30); CSimpleEditBox sets it.
CSimpleFrame* g_editBoxFocus;  // DAT_142e97b30

// The metatables of classes not ported yet: EditBox (DAT_142d5e4a8) and Button (DAT_142d4e470).
int32_t g_editBoxMetaTable = -2;
int32_t g_buttonMetaTable = -2;

namespace {

// What CreateFrame is asked for (0x38).
struct CREATEFRAMEINFO {
    SCRIPTLOCATION m_location;      // 0x00 where the script called from
    int32_t m_id;                   // 0x08
    uint8_t m_hasId;                // 0x0c
    uint8_t m_pad0d[3];
    const char* m_frameType;        // 0x10
    const char* m_name;             // 0x18
    const char* m_inherits;         // 0x20 comma-separated templates
    CSimpleFrame* m_parent;         // 0x28
    uint8_t m_forbidden;            // 0x30
    uint8_t m_pad31[7];
};
static_assert(sizeof(CREATEFRAMEINFO) == 0x38, "CREATEFRAMEINFO is 0x38 bytes");

// The location of a script position (FUN_140660800): the file's id through the registry when
// there is one, and the line.
// ref: FUN_140660800
void ScriptLocationFromSource(SCRIPTLOCATION* location, const char* source, int32_t line) {
    SCRIPTLOCATION found = {};
    ScriptLocationFromFile(&found, source, line);
    location->m_file = found.m_file;
    location->m_line = static_cast<int16_t>(line < 1 ? 0 : line);
}

// The frame of type `info->m_frameType`, made through an XML element built for it with its name,
// parent, templates and id, and loaded as FrameXML would (FUN_140614c20). 0 success, 1 a template
// is missing, 2 one inherits itself, 3 the type is unknown, 4 tainted code asked for a forbidden
// frame or a protected template, 5 the new frame cannot be used by the caller.
// ref: FUN_140614c20
int32_t CreateFrameFromInfo(CSimpleFrame** out, const CREATEFRAMEINFO* info) {
    if (info->m_frameType == nullptr || *info->m_frameType == '\0') {
        return 3;
    }
    if (info->m_forbidden != 0 && lua_taint != nullptr) {
        return 4;
    }
    uint32_t state = FrameXML_GetScopedState();
    int32_t object = FrameScript_GetCurrentObject();
    uint32_t scoped = state;

    if (info->m_inherits != nullptr) {
        const char* cursor = info->m_inherits;
        while (true) {
            char token[0x400];
            SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
            if (token[0] == '\0') {
                break;
            }
            char name[0x80];
            uint32_t i = 0;
            for (; i + 1 < sizeof(name) && token[i] != '\0'; i++) {
                name[i] = token[i];
            }
            name[i] = '\0';
            const char* taint = nullptr;
            bool busy = true;
            XMLOBJECT* node = FrameXML_AcquireVirtual(name, &taint, &busy);
            int32_t status = 0;
            if (node == nullptr) {
                status = 1;
            } else if (busy) {
                status = 2;
            } else if (lua_taint != nullptr && (node->m_flags & 9) != 0) {
                status = 4;
            }
            if (status != 0) {
                if (node != nullptr && !busy) {
                    FrameXML_ReleaseVirtual(name);
                }
                return status;
            }
            scoped |= node->m_flags & 0x1f;
            FrameXML_ReleaseVirtual(name);
        }
    }

    // A template scoped to its caller's environment runs with the calling function's.
    int32_t environment = 0;
    if ((scoped & 4) != 0 && (state & 4) == 0) {
        lua_State* L = FrameScript_GetContext();
        lua_Debug ar;
        int32_t level = 0;
        bool found = false;
        while (lua_getstack(L, level, &ar)) {
            lua_getinfo(L, "f", &ar);
            if (lua_type(L, -1) == LUA_TFUNCTION && !lua_iscfunction(L, -1)) {
                lua_getfenv(L, -1);
                lua_remove(L, -2);
                found = true;
                break;
            }
            lua_settop(L, -2);
            level++;
        }
        if (!found) {
            lua_pushvalue(L, LUA_GLOBALSINDEX);
        }
        environment = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    CScriptStatus status;
    status.m_warn = true;
    XMLNode node;
    node.m_flags = 0;
    node.m_attributeCount = 0;
    node.m_attributes = nullptr;
    node.m_attributeCapacity = 0;
    node.m_attributeGranularity = 0;
    node.m_location = {};
    node.Init(nullptr, info->m_frameType);
    if (info->m_name != nullptr) {
        node.SetAttribute("name", info->m_name);
    }
    if (info->m_parent != nullptr) {
        node.SetAttribute("parent", "");
    }
    if (info->m_inherits != nullptr) {
        node.SetAttribute("inherits", info->m_inherits);
    }
    if (info->m_location.m_file != 0 && info->m_location.m_line != 0) {
        *node.GetLocation() = info->m_location;
    }
    if (info->m_hasId != 0) {
        char id[0x20];
        SStrPrintf(id, sizeof(id), "%d", info->m_id);
        node.SetAttribute("id", id);
    }
    XMLOBJECT* element = XMLObjectCreateFrame(&node);
    node.Destroy();

    CSimpleFrame* frame = nullptr;
    if (element != nullptr) {
        uint32_t frameState = scoped | (element->m_flags & 0x1f);
        if (info->m_forbidden != 0) {
            frameState |= 1;
        }
        FrameXML_SetScopedState(frameState);
        if (environment == 0) {
            frame = FrameXML_CreateFrame(element, info->m_parent, &status);
            FrameXML_SetScopedState(state);
        } else {
            FrameScript_SetCurrentObject(environment);
            frame = FrameXML_CreateFrame(element, info->m_parent, &status);
            FrameXML_SetScopedState(state);
            FrameScript_SetCurrentObject(object);
            luaL_unref(FrameScript_GetContext(), LUA_REGISTRYINDEX, environment);
        }
        XMLObjectFree(element, false);
    } else if (environment != 0) {
        luaL_unref(FrameScript_GetContext(), LUA_REGISTRYINDEX, environment);
    }

    *out = frame;
    if (frame == nullptr) {
        return 3;
    }
    if (!frame->CanQueryRect()) {
        frame->Destroy();
        *out = nullptr;
        return 5;
    }
    if (info->m_hasId != 0) {
        frame->m_id = info->m_id;
    }
    return 0;
}

// The CreateFrame behind both script functions (FUN_1424db710).
// ref: FUN_1424db710
int32_t ScriptCreateFrame(lua_State* L, bool forbidden) {
    const char* usage = "Usage: CreateFrame(\"frameType\" [, \"name\"] [, parent] [, \"template\"] [, id])";
    if (!lua_isstring(L, 1) || (lua_type(L, 3) != LUA_TNONE && lua_type(L, 3) != LUA_TNIL && lua_type(L, 3) != LUA_TTABLE)) {
        luaL_error(L, usage);
        return 0;
    }
    CREATEFRAMEINFO info = {};
    info.m_forbidden = forbidden ? 1 : 0;
    info.m_frameType = lua_tostring(L, 1);
    info.m_name = lua_tostring(L, 2);
    if (lua_type(L, 3) == LUA_TTABLE) {
        const char* error = nullptr;
        info.m_parent = static_cast<CSimpleFrame*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFrame::GetObjectType(), 3, false, false));
        if (error != nullptr) {
            luaL_error(L, error);
            info.m_parent = nullptr;
        }
    }
    info.m_inherits = lua_tostring(L, 4);
    if (lua_isnumber(L, 5)) {
        info.m_id = static_cast<int32_t>(lua_tointeger(L, 5));
        if (info.m_hasId == 0) {
            info.m_hasId = 1;
        }
    }
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
        ScriptLocationFromSource(&info.m_location, ar.source, ar.currentline);
    }

    CSimpleFrame* frame = nullptr;
    switch (CreateFrameFromInfo(&frame, &info)) {
    case 0:
        if (frame != nullptr) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, frame->GetLuaRef());
            return 1;
        }
        break;
    case 1:
        luaL_error(L, "CreateFrame(): Couldn't find inherited node \"%s\"", info.m_inherits);
        return 0;
    case 2:
        luaL_error(L, "CreateFrame(): Recursively inherited node \"%s\"", info.m_inherits);
        return 0;
    case 3:
        luaL_error(L, "CreateFrame: Unknown frame type '%s'", info.m_frameType);
        return 0;
    case 4:
        luaL_error(L, "CreateFrame(): Cannot create a forbidden frame from a tainted context.");
        return 0;
    case 5:
        luaL_error(L, "CreateFrame: Can't create protected '%s' now", info.m_frameType);
        return 0;
    default:
        break;
    }
    luaL_error(L, "CreateFrame(): Unknown error, unable to create frame");
    return 0;
}

}

// ref: FUN_1424d9990
int Lua_CreateFrame(lua_State* L) {
    return ScriptCreateFrame(L, false);
}

// ref: FUN_1424d9a00
int Lua_CreateForbiddenFrame(lua_State* L) {
    return ScriptCreateFrame(L, true);
}

// ("name"): the named font object, made when there is none.
// ref: FUN_1424d8c90
int Lua_CreateFont(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: CreateFont(\"name\")");
        return 0;
    }
    CSimpleFont* font = CSimpleFont::GetFont(lua_tostring(L, 1), true);
    lua_rawgeti(L, LUA_REGISTRYINDEX, font->GetLuaRef());
    return 1;
}

// ("token" [, gender] [, ordinal])
// ref: FUN_1424d6b90
int Lua_GetText(lua_State* L) {
    const char* token = lua_tostring(L, 1);
    if (token == nullptr) {
        luaL_error(L, "Usage: GetText(\"token\" [,gender] [,ordinal])");
    }
    int32_t gender = 0;
    int32_t ordinal = -1;
    if (lua_isnumber(L, 2)) {
        gender = static_cast<int32_t>(lua_tointeger(L, 2));
    }
    if (lua_isnumber(L, 3)) {
        ordinal = static_cast<int32_t>(lua_tointeger(L, 3));
    }
    const char* text = FrameScript_GetText(token, ordinal, gender, false);
    if (text == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, text, strlen(text));
    }
    return 1;
}

// ([frame]): the next frame scripts may see after `frame`, or the first.
// ref: FUN_1424d7920
int Lua_EnumerateFrames(lua_State* L) {
    CSimpleFrame* frame;
    if (lua_type(L, 1) == LUA_TTABLE) {
        lua_rawgeti(L, 1, 0);
        auto current = static_cast<CSimpleFrame*>(lua_touserdata(L, -1));
        lua_settop(L, -2);
        if (current == nullptr) {
            luaL_error(L, "EnumerateFrames: Couldn't find 'this' in current object");
            return 0;
        }
        if (!current->IsObjectType(CSimpleFrame::GetObjectType())) {
            luaL_error(L, "EnumerateFrames: Wrong current object type, expected frame");
            return 0;
        }
        frame = CSimpleTop::s_instance->m_rootFrames.Next(current);
    } else {
        frame = CSimpleTop::s_instance->m_rootFrames.Head();
    }
    for (; frame != nullptr; frame = CSimpleTop::s_instance->m_rootFrames.Next(frame)) {
        uint16_t flags = frame->FrameScript_Object::m_flags;
        if ((flags & 5) == 0 && (flags & 8) == 0 && (flags & 0x20) != 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, frame->GetLuaRef());
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// ("template"): whether a virtual node by that name exists.
// ref: FUN_1424d9a70
int Lua_DoesTemplateExist(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: DoesTemplateExist(\"template\")");
        return 0;
    }
    lua_pushboolean(L, FrameXML_HasVirtual(lua_tostring(L, 1)));
    return 1;
}

// ("event"): the objects registered for an event that scripts may see.
// ref: FUN_1424da750
int Lua_GetFramesRegisteredForEvent(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: GetFramesRegisteredForEvent(\"event\")");
        return 0;
    }
    size_t length = 0;
    const char* name = lua_tolstring(L, 1, &length);
    // FUN_1407bc570
    uint64_t event = FrameScriptEvents::GetEventID(name, length);
    lua_settop(L, 0);
    blz::vector<FrameScript_Object*, FrameScriptEvents::s_objectTag> objects;
    FrameScriptEvents::GetListeners(&objects, event);
    for (FrameScript_Object* object : objects) {
        uint16_t flags = object->m_flags;
        if ((flags & 0x20) != 0 && ((flags >> 8) & 1) == 0) {
            lua_checkstack(L, 1);
            lua_rawgeti(L, LUA_REGISTRYINDEX, object->GetLuaRef());
        }
    }
    return lua_gettop(L);
}

// ref: FUN_1424d4590
int Lua_GetCurrentKeyBoardFocus(lua_State* L) {
    if (g_editBoxFocus == nullptr) {
        lua_pushnil(L);
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_editBoxFocus->GetLuaRef());
    }
    return 1;
}

// ref: FUN_1424d6b10
int Lua_GetFontStringMetatable(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, CSimpleFontString::s_metaTable);
    return 1;
}

// ref: FUN_1424d6b30
int Lua_GetEditBoxMetatable(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_editBoxMetaTable);
    return 1;
}

// ref: FUN_1424d6b50
int Lua_GetFrameMetatable(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, CSimpleFrame::s_metaTable);
    return 1;
}

// ref: FUN_1424d6b70
int Lua_GetButtonMetatable(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_buttonMetaTable);
    return 1;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_140614c20 = (void*)&CreateFrameFromInfo;
// ---- end of generated globals ----
