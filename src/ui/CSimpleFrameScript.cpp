// The Lua methods of frames (CSimpleFrame's table 0x142cf8610) and of the animation host a
// frame carries (CAnimatableObject's table 0x142cf7ae0).

#include "ui/CScriptObjectScript.hpp"

#include <cstring>

#include "framescript/FrameScriptEvents.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "framescript/ScriptMethod.hpp"
#include "framescript/Taint.hpp"
#include "ui/CAnimatableObject.hpp"
#include "ui/CLayoutFrame.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleRegion.hpp"
#include "ui/CSimpleTexture.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"
#include "framescript/FrameScript.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

namespace {

// The bounded copy the reference inlines.
void CopyBounded(char* dest, const char* source, uint64_t size) {
    uint64_t i = 0;
    for (; i + 1 < size && source[i] != '\0'; i++) {
        dest[i] = source[i];
    }
    dest[i] = '\0';
}

using namespace ScriptMethod;

// ref: FUN_1407feb40
CSimpleFrame* FrameSelf(lua_State* L, int32_t* index, const char* usage) {
    return static_cast<CSimpleFrame*>(Self(L, CSimpleFrame::GetObjectType(), index, usage));
}

CLayoutFrame* Layout(CSimpleFrame* frame) {
    return static_cast<CLayoutFrame*>(frame);
}

// A frame argument (self) for the attribute methods, which report through luaL_error.
// ref: FUN_1424cbca0
CSimpleFrame* FrameFromLua(lua_State* L, bool allowForbidden) {
    const char* error = nullptr;
    auto frame = static_cast<CSimpleFrame*>(
        FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFrame::GetObjectType(), 1, allowForbidden, false));
    if (error != nullptr) {
        luaL_error(L, error);
        return nullptr;
    }
    return frame;
}

// A frame-flag reader: the flag pushed as a bool.
int32_t PushFrameFlag(lua_State* L, const char* usage, const char* method, uint32_t flag) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool set = (frame->m_frameFlags & flag) != 0;
    RaisePendingError(L, frame, method);
    return PushBool(L, set);
}

template <class T>
struct OPTIONALARG {
    T m_value = T();
    bool m_set = false;
};

// ---- the region factories --------------------------------------------------------------

// CSimpleFontString and CSimpleLine are not ported yet.

// FUN_1424c9a80
// The texture-like regions frame:CreateTexture and frame:CreateMaskTexture make: the template
// looked up first, the region made in its layer and sublevel, named, loaded from the template,
// and caught up with an animation its frame is playing.
template <class T>
CSimpleRegion* CreateTextureLikeRegion(CSimpleFrame* frame, const OPTIONALARG<const char*>* name,
    const OPTIONALARG<uint32_t>* layer, const OPTIONALARG<const char*>* inherits, const OPTIONALARG<int8_t>* subLevel) {
    char templateName[0x80] = {};
    XMLOBJECT* node = nullptr;
    bool busy = false;
    const char* taint = nullptr;
    T* region = nullptr;
    if (inherits->m_set && inherits->m_value != nullptr) {
        busy = true;
        CopyBounded(templateName, inherits->m_value, sizeof(templateName));
        node = FrameXML_AcquireVirtual(templateName, &taint, &busy);
        if (node == nullptr) {
            FrameScript_ActionError("Couldn't find inherited node \"%s\"", templateName);
            return nullptr;
        }
        if (busy) {
            FrameScript_ActionError("Recursively inherited node \"%s\"", templateName);
            return nullptr;
        }
    }
    if (subLevel->m_set && static_cast<uint8_t>(subLevel->m_value + 8) >= 0x10) {
        FrameScript_ActionError("Sublevel must be between %d and %d", -8, 7);
    } else {
        region = new T(frame, static_cast<uint8_t>(layer->m_set ? layer->m_value : 2),
            static_cast<int8_t>(subLevel->m_set ? subLevel->m_value : 0), true);
        if (name->m_set && name->m_value != nullptr && *name->m_value != '\0') {
            region->SetName(name->m_value);
        }
        if (node != nullptr) {
            CScriptStatus status;
            region->FrameScript_Object::LoadXML(node, &status, nullptr);
            region->LoadXML(node, &status, nullptr);
            region->PostLoadXML(node, &status);
        }
        CAnimatableObject* animatable = region;
        if (animatable->IsPlaying()) {
            animatable->OnAnimStarted(animatable->IsAnimatingAlpha(), animatable->IsAnimatingTransform(),
                animatable->IsAnimatingRectOffset());
        }
    }
    if (node != nullptr && !busy) {
        FrameXML_ReleaseVirtual(templateName);
    }
    return region;
}

CSimpleRegion* CreateTextureRegion(CSimpleFrame* frame, const OPTIONALARG<const char*>* name, const OPTIONALARG<uint32_t>* layer,
    const OPTIONALARG<const char*>* inherits, const OPTIONALARG<int8_t>* subLevel) {
    return CreateTextureLikeRegion<CSimpleTexture>(frame, name, layer, inherits, subLevel);
}

// FUN_1424c9d50
CSimpleRegion* CreateFontStringRegion(CSimpleFrame* frame, const OPTIONALARG<const char*>* name, const OPTIONALARG<uint32_t>* layer,
    const OPTIONALARG<const char*>* inherits) {
    (void)frame;
    (void)name;
    (void)layer;
    (void)inherits;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// FUN_1424c94b0
CSimpleRegion* CreateLineRegion(CSimpleFrame* frame, const OPTIONALARG<const char*>* name, const OPTIONALARG<uint32_t>* layer,
    const OPTIONALARG<const char*>* inherits, const OPTIONALARG<int8_t>* subLevel) {
    (void)frame;
    (void)name;
    (void)layer;
    (void)inherits;
    (void)subLevel;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// FUN_1424c9780
CSimpleRegion* CreateMaskTextureRegion(CSimpleFrame* frame, const OPTIONALARG<const char*>* name, const OPTIONALARG<uint32_t>* layer,
    const OPTIONALARG<const char*>* inherits, const OPTIONALARG<int8_t>* subLevel) {
    return CreateTextureLikeRegion<CSimpleMaskTexture>(frame, name, layer, inherits, subLevel);
}

struct CREATEREGIONCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    OPTIONALARG<const char*> m_name;
    OPTIONALARG<uint32_t> m_layer;
    OPTIONALARG<const char*> m_inherits;
    OPTIONALARG<int8_t> m_subLevel;
    CSimpleRegion* m_region = nullptr;
};

// The [name, drawLayer, templateName] arguments every factory method starts with.
void ReadRegionArguments(lua_State* L, int32_t* index, const char* usage, CREATEREGIONCONTEXT* ctx) {
    ctx->m_frame = FrameSelf(L, index, usage);
    ctx->m_name.m_value = OptionalString(L, index, usage, &ctx->m_name.m_set);
    ctx->m_layer.m_value = OptionalDrawLayer(L, index, usage, &ctx->m_layer.m_set);
    ctx->m_inherits.m_value = OptionalString(L, index, usage, &ctx->m_inherits.m_set);
}

// ref: FUN_1408d4cd0
int32_t CreateFontStringBody(lua_State* L, void* context) {
    auto ctx = static_cast<CREATEREGIONCONTEXT*>(context);
    int32_t index = 1;
    ReadRegionArguments(L, &index, "Usage: local line = self:CreateFontString([name, drawLayer, templateName])", ctx);
    ctx->m_region = CreateFontStringRegion(ctx->m_frame, &ctx->m_name, &ctx->m_layer, &ctx->m_inherits);
    RaisePendingError(L, ctx->m_frame, "CreateFontString");
    return PushObject(L, ctx->m_region);
}

// ref: FUN_1409830d0
int CreateFontString(lua_State* L) {
    CREATEREGIONCONTEXT context;
    return ProtectedCall(L, CreateFontStringBody, &context);
}

// ref: FUN_1408d5b90
int32_t CreateLineBody(lua_State* L, void* context) {
    auto ctx = static_cast<CREATEREGIONCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local line = self:CreateLine([name, drawLayer, templateName, subLevel])";
    ReadRegionArguments(L, &index, usage, ctx);
    ctx->m_subLevel.m_value = OptionalSubLevel(L, &index, usage, &ctx->m_subLevel.m_set);
    ctx->m_region = CreateLineRegion(ctx->m_frame, &ctx->m_name, &ctx->m_layer, &ctx->m_inherits, &ctx->m_subLevel);
    RaisePendingError(L, ctx->m_frame, "CreateLine");
    return PushObject(L, ctx->m_region);
}

// ref: FUN_140983e00
int CreateLine(lua_State* L) {
    CREATEREGIONCONTEXT context;
    return ProtectedCall(L, CreateLineBody, &context);
}

// ref: FUN_1408d6ad0
int32_t CreateMaskTextureBody(lua_State* L, void* context) {
    auto ctx = static_cast<CREATEREGIONCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local maskTexture = self:CreateMaskTexture([name, drawLayer, templateName, subLevel])";
    ReadRegionArguments(L, &index, usage, ctx);
    ctx->m_subLevel.m_value = OptionalSubLevel(L, &index, usage, &ctx->m_subLevel.m_set);
    ctx->m_region = CreateMaskTextureRegion(ctx->m_frame, &ctx->m_name, &ctx->m_layer, &ctx->m_inherits, &ctx->m_subLevel);
    RaisePendingError(L, ctx->m_frame, "CreateMaskTexture");
    return PushObject(L, ctx->m_region);
}

// ref: FUN_140984b50
int CreateMaskTexture(lua_State* L) {
    CREATEREGIONCONTEXT context;
    return ProtectedCall(L, CreateMaskTextureBody, &context);
}

// ref: FUN_1408d7a10
int32_t CreateTextureBody(lua_State* L, void* context) {
    auto ctx = static_cast<CREATEREGIONCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local texture = self:CreateTexture([name, drawLayer, templateName, subLevel])";
    ReadRegionArguments(L, &index, usage, ctx);
    ctx->m_subLevel.m_value = OptionalSubLevel(L, &index, usage, &ctx->m_subLevel.m_set);
    ctx->m_region = CreateTextureRegion(ctx->m_frame, &ctx->m_name, &ctx->m_layer, &ctx->m_inherits, &ctx->m_subLevel);
    RaisePendingError(L, ctx->m_frame, "CreateTexture");
    return PushObject(L, ctx->m_region);
}

// ref: FUN_1409858a0
int CreateTexture(lua_State* L) {
    CREATEREGIONCONTEXT context;
    return ProtectedCall(L, CreateTextureBody, &context);
}

// ---- the frame methods -----------------------------------------------------------------

// ref: FUN_1409815a0
int AbortDrag(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:AbortDrag()");
    frame->CancelDrag();
    RaisePendingError(L, frame, "AbortDrag");
    return 0;
}

// ref: FUN_140982330
int CanChangeAttribute(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local canChangeAttributes = self:CanChangeAttribute()");
    bool canChange = frame->CanQueryRect();
    RaisePendingError(L, frame, "CanChangeAttribute");
    return PushBool(L, canChange);
}

// ref: FUN_1409865f0
int DesaturateHierarchy(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:DesaturateHierarchy(desaturation [, excludeRoot])";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    float desaturation = RequiredFinite(L, &index, usage);
    bool excludeRoot = OptionalBool(L, &index);
    frame->DesaturateHierarchy(desaturation, excludeRoot);
    RaisePendingError(L, frame, "DesaturateHierarchy");
    return 0;
}

// ref: FUN_140987440
int DisableDrawLayer(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:DisableDrawLayer(layer)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    uint32_t layer = RequiredDrawLayer(L, &index, usage);
    frame->DisableDrawLayer(static_cast<uint8_t>(layer));
    RaisePendingError(L, frame, "DisableDrawLayer");
    return 0;
}

// ref: FUN_1409882b0
int DoesClipChildren(lua_State* L) {
    return PushFrameFlag(L, "Usage: local clipsChildren = self:DoesClipChildren()", "DoesClipChildren", CSimpleFrame::FRAME_CLIP_CHILDREN);
}

// ref: FUN_140989050
int EnableDrawLayer(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:EnableDrawLayer(layer)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    uint32_t layer = RequiredDrawLayer(L, &index, usage);
    frame->EnableDrawLayer(static_cast<uint8_t>(layer));
    RaisePendingError(L, frame, "EnableDrawLayer");
    return 0;
}

// ref: FUN_140989ec0
int EnableGamePadButton(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:EnableGamePadButton([enable])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->EnableGamePadButton(enable);
    RaisePendingError(L, frame, "EnableGamePadButton");
    return 0;
}

// ref: FUN_14098ac90
int EnableGamePadStick(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:EnableGamePadStick([enable])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->EnableGamePadStick(enable);
    RaisePendingError(L, frame, "EnableGamePadStick");
    return 0;
}

// ref: FUN_14098ba60
int EnableKeyboard(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:EnableKeyboard([enable])");
    bool enable = OptionalBool(L, &index);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->EnableKeyboard(enable);
    RaisePendingError(L, frame, "EnableKeyboard");
    return 0;
}

// Runs the function an attribute holds with the arguments after its name, under the taint
// it was set with: true and its results, or false when the attribute is not a function.
// ref: FUN_1424cbd60
int ExecuteAttributeBody(lua_State* L) {
    CSimpleFrame* frame = FrameFromLua(L, true);
    if (frame == nullptr) {
        return 0;
    }
    if (!lua_isstring(L, 2)) {
        FrameScript_ActionError("Arguments: (\"name\" [, ...])");
        return 0;
    }
    int32_t ref = -1;
    const char* taint = nullptr;
    if (frame->GetAttribute(lua_tolstring(L, 2, nullptr), &ref, &taint)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_type(L, -1) == LUA_TFUNCTION) {
            lua_remove(L, 1);
            lua_remove(L, 1);
            int32_t function = lua_gettop(L);
            int32_t count = lua_gettop(L) - 1;
            lua_pushboolean(L, 1);
            return FrameScript_CallWithTaint(L, taint, function, function - count, count);
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

// ref: FUN_14098c830
int ExecuteAttribute(lua_State* L) {
    int results = ExecuteAttributeBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_14098d4e0
int GetAlpha(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local alpha = self:GetAlpha()");
    uint8_t alpha = frame->GetAlpha();
    RaisePendingError(L, frame, "GetAlpha");
    return PushAlpha(L, alpha);
}

// Appends at most `*room` bytes of `text` to `*cursor`.
void AppendBounded(char** cursor, uint64_t* room, const char* text, uint64_t length) {
    if (length == 0) {
        return;
    }
    uint64_t count = length < *room ? length : *room;
    std::memcpy(*cursor, text, count);
    *cursor += count;
    *room -= count;
}

// An attribute by name; with (prefix, name, suffix) the most specific of prefix..name..suffix,
// *name..suffix, prefix..name*, *name* and plain name.
// ref: FUN_1424cbec0
int GetAttributeBody(lua_State* L) {
    CSimpleFrame* frame = FrameFromLua(L, true);
    if (frame == nullptr) {
        return 0;
    }
    int32_t ref;
    const char* taint;
    const char* lookup;
    if (lua_gettop(L) == 4 && lua_isstring(L, 3)) {
        size_t prefixLength;
        size_t nameLength;
        size_t suffixLength;
        const char* prefix = lua_tolstring(L, 2, &prefixLength);
        const char* name = lua_tolstring(L, 3, &nameLength);
        const char* suffix = lua_tolstring(L, 4, &suffixLength);
        char key[0x100];
        char* cursor = key;
        uint64_t room = 0xff;
        AppendBounded(&cursor, &room, prefix, prefixLength);
        AppendBounded(&cursor, &room, name, nameLength);
        AppendBounded(&cursor, &room, suffix, suffixLength);
        *cursor = '\0';
        if (frame->GetAttribute(key, &ref, &taint)) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            return 1;
        }
        key[0] = '*';
        cursor = key + 1;
        room = 0xfe;
        AppendBounded(&cursor, &room, name, nameLength);
        AppendBounded(&cursor, &room, suffix, suffixLength);
        *cursor = '\0';
        if (frame->GetAttribute(key, &ref, &taint)) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            return 1;
        }
        cursor = key;
        room = 0xfe;
        AppendBounded(&cursor, &room, prefix, prefixLength);
        AppendBounded(&cursor, &room, name, nameLength);
        cursor[0] = '*';
        cursor[1] = '\0';
        if (frame->GetAttribute(key, &ref, &taint)) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            return 1;
        }
        key[0] = '*';
        cursor = key + 1;
        room = 0xfd;
        AppendBounded(&cursor, &room, name, nameLength);
        cursor[0] = '*';
        cursor[1] = '\0';
        if (frame->GetAttribute(key, &ref, &taint)) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            return 1;
        }
        lookup = name;
    } else {
        if (!lua_isstring(L, 2)) {
            FrameScript_ActionError("Arguments: (\"name\")");
            return 0;
        }
        lookup = lua_tolstring(L, 2, nullptr);
    }
    if (frame->GetAttribute(lookup, &ref, &taint)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ref: FUN_14098e280
int GetAttribute(lua_State* L) {
    int results = GetAttributeBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

int32_t PushUIRect(lua_State* L, float a, float b, float c, float d) {
    int32_t results = PushUIUnits(L, a);
    results += PushUIUnits(L, b);
    results += PushUIUnits(L, c);
    return results + PushUIUnits(L, d);
}

// ref: FUN_14098ef30
int GetBoundsRect(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local left, bottom, width, height = self:GetBoundsRect()");
    float left = 0.0f;
    float bottom = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    frame->GetBoundsRectLua(&left, &bottom, &width, &height);
    RaisePendingError(L, frame, "GetBoundsRect");
    return PushUIRect(L, left, bottom, width, height);
}

struct CHILDRENCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    SIMPLEFRAMELIST m_children;
};

// ref: FUN_1408d88f0
int32_t GetChildrenBody(lua_State* L, void* context) {
    auto ctx = static_cast<CHILDRENCONTEXT*>(context);
    int32_t index = 1;
    ctx->m_frame = FrameSelf(L, &index, "Usage: local (scriptObject)* = self:GetChildren()");
    SIMPLEFRAMELIST children;
    ctx->m_frame->GetScriptChildren(&children);
    ctx->m_children.clear();
    for (uint64_t i = 0; i < children.m_size; i++) {
        ctx->m_children.push_back(children.m_data[i]);
    }
    RaisePendingError(L, ctx->m_frame, "GetChildren");
    int32_t results = 0;
    for (uint64_t i = 0; i < ctx->m_children.m_size; i++) {
        results += PushObject(L, ctx->m_children.m_data[i]);
    }
    return results;
}

// ref: FUN_14098fe30
int GetChildren(lua_State* L) {
    CHILDRENCONTEXT context;
    return ProtectedCall(L, GetChildrenBody, &context);
}

// ref: FUN_140990cc0
int GetClampRectInsets(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local left, right, top, bottom = self:GetClampRectInsets()");
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    Layout(frame)->GetClampRectInsets(&left, &right, &top, &bottom);
    RaisePendingError(L, frame, "GetClampRectInsets");
    return PushUIRect(L, left, right, top, bottom);
}

// ref: FUN_140991bc0
int GetDontSavePosition(lua_State* L) {
    return PushFrameFlag(L, "Usage: local dontSave = self:GetDontSavePosition()", "GetDontSavePosition", CSimpleFrame::FRAME_DONT_SAVE_POSITION);
}

// ref: FUN_140992960
int GetEffectiveAlpha(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local effectiveAlpha = self:GetEffectiveAlpha()");
    uint8_t alpha = frame->GetEffectiveAlpha();
    RaisePendingError(L, frame, "GetEffectiveAlpha");
    return PushAlpha(L, alpha);
}

// ref: FUN_140993700
int GetEffectiveScale(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local effectiveScale = self:GetEffectiveScale()");
    float scale = Layout(frame)->m_layoutScale;
    RaisePendingError(L, frame, "GetEffectiveScale");
    return PushFloat(L, scale);
}

// ref: FUN_1409944a0
int GetEffectivelyFlattensRenderLayers(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local flatten = self:GetEffectivelyFlattensRenderLayers()");
    bool flatten = (frame->m_frameFlags & CSimpleFrame::FRAME_OWN_BATCH) != 0 || (frame->m_flags2 & CSimpleFrame::FRAME2_FRAME_BUFFER) != 0;
    RaisePendingError(L, frame, "GetEffectivelyFlattensRenderLayers");
    return PushBool(L, flatten);
}

// ref: FUN_140995250
int GetFlattensRenderLayers(lua_State* L) {
    return PushFrameFlag(L, "Usage: local flatten = self:GetFlattensRenderLayers()", "GetFlattensRenderLayers", CSimpleFrame::FRAME_FLATTEN_RENDER_LAYERS);
}

// ref: FUN_140995ff0
int GetFrameLevel(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local frameLevel = self:GetFrameLevel()");
    uint16_t level = frame->m_level;
    RaisePendingError(L, frame, "GetFrameLevel");
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(level));
    return 1;
}

// ref: FUN_140996da0
int GetFrameStrata(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local strata = self:GetFrameStrata()");
    uint32_t flags = frame->m_flags2;
    RaisePendingError(L, frame, "GetFrameStrata");
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    const char* strata = FrameStrataToString((flags >> 7) & 0xf);
    if (strata == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, strata, std::strlen(strata));
    return 1;
}

// ref: FUN_140997ba0
int GetHitRectInsets(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local left, right, top, bottom = self:GetHitRectInsets()");
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    frame->GetHitRectInsetsLua(&left, &right, &top, &bottom);
    RaisePendingError(L, frame, "GetHitRectInsets");
    return PushUIRect(L, left, right, top, bottom);
}

// ref: FUN_140998aa0
int GetHyperlinksEnabled(lua_State* L) {
    return PushFrameFlag(L, "Usage: local enabled = self:GetHyperlinksEnabled()", "GetHyperlinksEnabled", CSimpleFrame::FRAME_HYPERLINKS_ENABLED);
}

// ref: FUN_140999840
int GetID(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local id = self:GetID()");
    int32_t id = frame->m_id;
    RaisePendingError(L, frame, "GetID");
    return PushInt(L, id);
}

// ref: FUN_14099a5e0
int GetNumChildren(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local numChildren = self:GetNumChildren()");
    uint32_t count = static_cast<uint32_t>(frame->GetNumChildren());
    RaisePendingError(L, frame, "GetNumChildren");
    return PushUInt(L, count);
}

// ref: FUN_14099b380
int GetNumRegions(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local numRegions = self:GetNumRegions()");
    uint32_t count = static_cast<uint32_t>(frame->GetNumRegions());
    RaisePendingError(L, frame, "GetNumRegions");
    return PushUInt(L, count);
}

// ref: FUN_14099c120
int GetPropagateKeyboardInput(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local propagate = self:GetPropagateKeyboardInput()");
    bool propagate = (frame->m_frameFlags & CSimpleFrame::FRAME_PROPAGATE_KEYBOARD) != 0 && !Layout(frame)->IsProtected();
    RaisePendingError(L, frame, "GetPropagateKeyboardInput");
    return PushBool(L, propagate);
}

struct REGIONSCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    SIMPLEREGIONLIST m_regions;
};

// ref: FUN_1408d9890
int32_t GetRegionsBody(lua_State* L, void* context) {
    auto ctx = static_cast<REGIONSCONTEXT*>(context);
    int32_t index = 1;
    ctx->m_frame = FrameSelf(L, &index, "Usage: local (scriptObject)* = self:GetRegions()");
    SIMPLEREGIONLIST regions;
    ctx->m_frame->GetScriptRegions(&regions);
    ctx->m_regions.clear();
    for (uint64_t i = 0; i < regions.m_size; i++) {
        ctx->m_regions.push_back(regions.m_data[i]);
    }
    RaisePendingError(L, ctx->m_frame, "GetRegions");
    int32_t results = 0;
    for (uint64_t i = 0; i < ctx->m_regions.m_size; i++) {
        results += PushObject(L, ctx->m_regions.m_data[i]);
    }
    return results;
}

// ref: FUN_14099ced0
int GetRegions(lua_State* L) {
    REGIONSCONTEXT context;
    return ProtectedCall(L, GetRegionsBody, &context);
}

// ref: FUN_14099dd60
int GetResizeBounds(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local minWidth, minHeight, maxWidth, maxHeight = self:GetResizeBounds()");
    float minWidth = 0.0f;
    float minHeight = 0.0f;
    float maxWidth = 0.0f;
    float maxHeight = 0.0f;
    Layout(frame)->GetResizeBounds(&minWidth, &minHeight, &maxWidth, &maxHeight);
    RaisePendingError(L, frame, "GetResizeBounds");
    return PushUIRect(L, minWidth, minHeight, maxWidth, maxHeight);
}

// ref: FUN_14099ec60
int GetScale(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local frameScale = self:GetScale()");
    float scale = frame->m_scale;
    RaisePendingError(L, frame, "GetScale");
    return PushFloat(L, scale);
}

// ref: FUN_14099fa00
int HasFixedFrameLevel(lua_State* L) {
    return PushFrameFlag(L, "Usage: local isFixed = self:HasFixedFrameLevel()", "HasFixedFrameLevel", CSimpleFrame::FRAME_FIXED_LEVEL);
}

// ref: FUN_1409a07a0
int HasFixedFrameStrata(lua_State* L) {
    return PushFrameFlag(L, "Usage: local isFixed = self:HasFixedFrameStrata()", "HasFixedFrameStrata", CSimpleFrame::FRAME_FIXED_STRATA);
}

// ref: FUN_1409a1540
int Hide(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:Hide()");
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->Hide();
    RaisePendingError(L, frame, "Hide");
    return 0;
}

// ref: FUN_1409a22d0
int InterceptStartDrag(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:InterceptStartDrag(delegate)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    CSimpleFrame* delegate = FrameSelf(L, &index, usage);
    frame->InterceptStartDrag(delegate);
    RaisePendingError(L, frame, "InterceptStartDrag");
    return 0;
}

// ref: FUN_1409a3150
int IsClampedToScreen(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local clampedToScreen = self:IsClampedToScreen()");
    bool clamped = (Layout(frame)->m_flags & 0x8) != 0;
    RaisePendingError(L, frame, "IsClampedToScreen");
    return PushBool(L, clamped);
}

// The units a unit event is filtered on push as their tokens ("player", ...).
// FUN_1407d2c40
int32_t PushUnitToken(lua_State* L, uint16_t unit) {
    (void)L;
    (void)unit;
    // The unit token names (FUN_1424348d0) belong to the unit system, which is not ported.
    FROST_UNIMPLEMENTED();
    return 0;
}

struct EVENTREGISTEREDCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    const char* m_event = nullptr;
    bool m_registered = false;
    uint16_t m_units[4] = {};
    uint64_t m_unitCount = 0;
    bool m_hasUnits = false;
};

// Whether the object listens to an event, and the units it filters it on.
// ref: FUN_140643100
void IsEventRegisteredWithUnits(FrameScript_Object* object, const char* name, bool* registered, EVENTREGISTEREDCONTEXT* ctx) {
    uint64_t length = *name == '\0' ? 0 : std::strlen(name);
    uint64_t id = FrameScriptEvents::GetEventID(name, length);
    *registered = FrameScriptEvents::IsEventRegistered(object, id);
    uint64_t units = 0;
    if (*registered && FrameScriptEvents::GetEventUnits(object, id, &units)) {
        for (int32_t i = 0; i < 4; i++) {
            auto unit = static_cast<uint16_t>(units >> (i * 16));
            if (unit != 0) {
                ctx->m_hasUnits = true;
                ctx->m_units[ctx->m_unitCount++] = unit;
            }
        }
    }
}

// ref: FUN_1408da890
int32_t IsEventRegisteredBody(lua_State* L, void* context) {
    auto ctx = static_cast<EVENTREGISTEREDCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local isRegistered, (units)* = self:IsEventRegistered(eventName)";
    ctx->m_frame = FrameSelf(L, &index, usage);
    ctx->m_event = RequiredString(L, &index, usage);
    IsEventRegisteredWithUnits(ctx->m_frame, ctx->m_event, &ctx->m_registered, ctx);
    RaisePendingError(L, ctx->m_frame, "IsEventRegistered");
    int32_t results = PushBool(L, ctx->m_registered);
    if (!ctx->m_hasUnits) {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
            return results;
        }
        lua_pushnil(L);
        return results + 1;
    }
    for (uint64_t i = 0; i < ctx->m_unitCount; i++) {
        if (!lua_checkstack(L, 1)) {
            luaL_error(L, "stack overflow");
            continue;
        }
        results += PushUnitToken(L, ctx->m_units[i]);
    }
    return results;
}

// ref: FUN_1409a3ef0
int IsEventRegistered(lua_State* L) {
    EVENTREGISTEREDCONTEXT context;
    return ProtectedCall(L, IsEventRegisteredBody, &context);
}

// ref: FUN_1409a4c50
int IsGamePadButtonEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local enabled = self:IsGamePadButtonEnabled()");
    bool enabled = frame->IsGamePadButtonEnabled();
    RaisePendingError(L, frame, "IsGamePadButtonEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_1409a59f0
int IsGamePadStickEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local enabled = self:IsGamePadStickEnabled()");
    bool enabled = frame->IsGamePadStickEnabled();
    RaisePendingError(L, frame, "IsGamePadStickEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_1409a6790
int IsIgnoringParentAlpha(lua_State* L) {
    return PushFrameFlag(L, "Usage: local ignore = self:IsIgnoringParentAlpha()", "IsIgnoringParentAlpha", CSimpleFrame::FRAME_IGNORE_PARENT_ALPHA);
}

// ref: FUN_1409a7530
int IsIgnoringParentScale(lua_State* L) {
    return PushFrameFlag(L, "Usage: local ignore = self:IsIgnoringParentScale()", "IsIgnoringParentScale", CSimpleFrame::FRAME_IGNORE_PARENT_SCALE);
}

// ref: FUN_1409a82d0
int IsKeyboardEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local enabled = self:IsKeyboardEnabled()");
    bool enabled = frame->IsKeyboardEnabled();
    RaisePendingError(L, frame, "IsKeyboardEnabled");
    return PushBool(L, enabled);
}

// ref: FUN_1409a9070
int IsMovable(lua_State* L) {
    return PushFrameFlag(L, "Usage: local isMovable = self:IsMovable()", "IsMovable", CSimpleFrame::FRAME_MOVABLE);
}

// ref: FUN_1409a9e10
int IsObjectLoaded(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local isLoaded = self:IsObjectLoaded()");
    bool loaded = Layout(frame)->IsLayoutReady();
    RaisePendingError(L, frame, "IsObjectLoaded");
    return PushBool(L, loaded);
}

// ref: FUN_1409aabb0
int IsResizable(lua_State* L) {
    return PushFrameFlag(L, "Usage: local resizable = self:IsResizable()", "IsResizable", CSimpleFrame::FRAME_RESIZABLE);
}

// ref: FUN_1409ab950
int IsShown(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local isShown = self:IsShown()");
    bool shown = (Layout(frame)->m_layoutState & CLayoutFrame::LAYOUT_SHOWN) != 0;
    RaisePendingError(L, frame, "IsShown");
    return PushBool(L, shown);
}

// ref: FUN_1409ac6f0
int IsToplevel(lua_State* L) {
    return PushFrameFlag(L, "Usage: local isTopLevel = self:IsToplevel()", "IsToplevel", CSimpleFrame::FRAME_TOPLEVEL);
}

// ref: FUN_1409ad490
int IsUserPlaced(lua_State* L) {
    return PushFrameFlag(L, "Usage: local isUserPlaced = self:IsUserPlaced()", "IsUserPlaced", CSimpleFrame::FRAME_USER_PLACED);
}

// ref: FUN_1409ae230
int IsVisible(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: local isVisible = self:IsVisible()");
    bool visible = (Layout(frame)->m_layoutState & CLayoutFrame::LAYOUT_VISIBLE) != 0;
    RaisePendingError(L, frame, "IsVisible");
    return PushBool(L, visible);
}

// ref: FUN_1409aefd0
int Lower(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:Lower()");
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->Lower();
    RaisePendingError(L, frame, "Lower");
    return 0;
}

// ref: FUN_1409afd60
int Raise(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:Raise()");
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->Raise();
    RaisePendingError(L, frame, "Raise");
    return 0;
}

// ref: FUN_1409b0af0
int RegisterAllEvents(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:RegisterAllEvents()");
    FrameScriptEvents::RegisterAllEvents(frame);
    RaisePendingError(L, frame, "RegisterAllEvents");
    return 0;
}

// ref: FUN_1409b1880
int RegisterEvent(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local registered = self:RegisterEvent(eventName)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    const char* name = RequiredString(L, &index, usage);
    bool registered = frame->RegisterEvent(name);
    RaisePendingError(L, frame, "RegisterEvent");
    return PushBool(L, registered);
}

struct DRAGBUTTONSCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    uint32_t m_buttons[32] = {};
    uint64_t m_count = 0;
};

// ref: FUN_1408db6a0
int32_t RegisterForDragBody(lua_State* L, void* context) {
    auto ctx = static_cast<DRAGBUTTONSCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:RegisterForDrag(buttons)";
    ctx->m_frame = FrameSelf(L, &index, usage);
    MouseButtons(L, &index, usage, ctx->m_buttons, 32, &ctx->m_count);
    ctx->m_frame->RegisterForDrag(ctx->m_buttons, ctx->m_count);
    RaisePendingError(L, ctx->m_frame, "RegisterForDrag");
    return 0;
}

// ref: FUN_1409b2690
int RegisterForDrag(lua_State* L) {
    DRAGBUTTONSCONTEXT context;
    return ProtectedCall(L, RegisterForDragBody, &context);
}

// The unit a token names (FUN_14065f830 then FUN_14065f4a0); 0 when none.
uint16_t UnitFromToken(const char* token) {
    (void)token;
    // The unit tokens belong to the unit system, which is not ported; without one the
    // registration below falls back to the unfiltered event.
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1424cc260
int RegisterUnitEventBody(lua_State* L) {
    const char* error = nullptr;
    auto frame = static_cast<CSimpleFrame*>(FrameScript_Object::GetObjectFromLua(&error, L, CSimpleFrame::GetObjectType(), 1, false, false));
    if (error != nullptr) {
        FrameScript_ActionError(error);
    }
    if (frame == nullptr) {
        return 0;
    }
    if (!lua_isstring(L, 2)) {
        FrameScript_ObjectError(frame, "RegisterUnitEvent", "Usage: (\"event\", \"unit1\" [,\"unit2\", \"unit3\", \"unit4\"])");
        return 0;
    }
    size_t length;
    const char* name = lua_tolstring(L, 2, &length);
    uint64_t id = FrameScriptEvents::GetEventID(name, length);
    uint64_t units = 0;
    int32_t unitCount = 0;
    int32_t count = lua_gettop(L) - 2;
    if (count > 4) {
        count = 4;
    }
    for (int32_t i = 0; i < count; i++) {
        if (!lua_isstring(L, 3 + i)) {
            break;
        }
        uint16_t unit = UnitFromToken(lua_tolstring(L, 3 + i, nullptr));
        if (unit != 0) {
            units |= static_cast<uint64_t>(unit) << (unitCount * 16);
            unitCount++;
        }
    }
    int32_t result;
    if (unitCount > 0) {
        // FUN_1406482a0, the registration with a unit filter, comes with the unit system.
        FROST_UNIMPLEMENTED();
        result = FrameScriptEvents::RegisterEvent(frame, id);
    } else {
        result = FrameScriptEvents::RegisterEvent(frame, id);
    }
    if (result == 2) {
        FrameScript_ObjectError(frame, "RegisterUnitEvent", "Attempt to register unknown event \"%s\"", lua_tolstring(L, 2, nullptr));
        return 0;
    }
    lua_pushboolean(L, result == 0);
    return 1;
}

// ref: FUN_1409b33f0
int RegisterUnitEvent(lua_State* L) {
    int results = RegisterUnitEventBody(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_1409b40a0
int RotateTextures(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:RotateTextures(radians [, x, y])";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    float radians = RequiredFinite(L, &index, usage);
    float x = OptionalFinite(L, &index, usage, 0.5f);
    float y = OptionalFinite(L, &index, usage, 0.5f);
    frame->RotateTextures(radians, x, y);
    RaisePendingError(L, frame, "RotateTextures");
    return 0;
}

// ref: FUN_1409b4fa0
int SetAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetAlpha(alpha)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    uint8_t alpha = RequiredAlpha(L, &index, usage);
    frame->SetAlpha(alpha);
    RaisePendingError(L, frame, "SetAlpha");
    return 0;
}

// Sets an attribute to the value at 3, the value referenced untainted and the setter's taint
// kept with it; `quiet` skips OnAttributeChanged.
// ref: FUN_1424cc4d0
int SetAttributeBody(lua_State* L, bool quiet) {
    CSimpleFrame* frame = FrameFromLua(L, true);
    if (frame == nullptr) {
        return 0;
    }
    if (!frame->CanQueryRect()) {
        if (CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
            CSimpleTop::s_instance->m_actionBlockedHook(frame);
        }
        return 0;
    }
    lua_settop(L, 3);
    if (lua_isstring(L, 2) && lua_type(L, 3) != LUA_TNONE) {
        const char* name = lua_tolstring(L, 2, nullptr);
        int32_t ref;
        const char* taint;
        if (frame->GetAttribute(name, &ref, &taint)) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
        }
        const char* previous = lua_taint;
        FrameScript_EnterTaint(L, nullptr);
        lua_pushvalue(L, 3);
        int32_t value = luaL_ref(L, LUA_REGISTRYINDEX);
        FrameScript_LeaveTaint(L, previous);
        frame->SetAttribute(name, value, previous, quiet);
        return 0;
    }
    FrameScript_ActionError("Arguments: (\"name\", value)");
    return 0;
}

// ref: FUN_1424cc6d0
int SetAttributeHandled(lua_State* L) {
    return SetAttributeBody(L, false);
}

// ref: FUN_1409b5e60
int SetAttribute(lua_State* L) {
    int results = SetAttributeHandled(L);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_1409b6b10
int SetAttributeNoHandler(lua_State* L) {
    int results = SetAttributeBody(L, true);
    RaisePendingError(L, nullptr, nullptr);
    return results;
}

// ref: FUN_1409b77c0
int SetClampRectInsets(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetClampRectInsets(left, right, top, bottom)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    float left = RequiredUIUnits(L, &index, usage);
    float right = RequiredUIUnits(L, &index, usage);
    float top = RequiredUIUnits(L, &index, usage);
    float bottom = RequiredUIUnits(L, &index, usage);
    frame->SetClampRectInsets(left, right, top, bottom);
    RaisePendingError(L, frame, "SetClampRectInsets");
    return 0;
}

// ref: FUN_1409b87f0
int SetClampedToScreen(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetClampedToScreen(clampedToScreen)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool clamped = RequiredBool(L, &index, usage);
    Layout(frame)->SetClampedToScreen(clamped);
    RaisePendingError(L, frame, "SetClampedToScreen");
    return 0;
}

// ref: FUN_1409b95c0
int SetClipsChildren(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetClipsChildren(clipsChildren)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool clips = RequiredBool(L, &index, usage);
    if (((frame->m_frameFlags & CSimpleFrame::FRAME_CLIP_CHILDREN) != 0) != clips) {
        frame->SetFrameFlag(CSimpleFrame::FRAME_CLIP_CHILDREN, clips);
        frame->UpdateOwnBatch();
    }
    RaisePendingError(L, frame, "SetClipsChildren");
    return 0;
}

// A frame-flag setter: the required bool into the flag.
int SetFrameFlagMethod(lua_State* L, const char* usage, const char* method, uint32_t flag) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool set = RequiredBool(L, &index, usage);
    frame->SetFrameFlag(flag, set);
    RaisePendingError(L, frame, method);
    return 0;
}

// ref: FUN_1409ba3b0
int SetDontSavePosition(lua_State* L) {
    return SetFrameFlagMethod(L, "Usage: self:SetDontSavePosition(dontSave)", "SetDontSavePosition", CSimpleFrame::FRAME_DONT_SAVE_POSITION);
}

// ref: FUN_1409bb190
int SetDrawLayerEnabled(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetDrawLayerEnabled(layer [, isEnabled])";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    uint32_t layer = RequiredDrawLayer(L, &index, usage);
    bool enabled = OptionalBool(L, &index);
    frame->SetDrawLayerEnabled(static_cast<uint8_t>(layer), enabled);
    RaisePendingError(L, frame, "SetDrawLayerEnabled");
    return 0;
}

// ref: FUN_1409bc050
int SetFixedFrameLevel(lua_State* L) {
    return SetFrameFlagMethod(L, "Usage: self:SetFixedFrameLevel(isFixed)", "SetFixedFrameLevel", CSimpleFrame::FRAME_FIXED_LEVEL);
}

// ref: FUN_1409bce30
int SetFixedFrameStrata(lua_State* L) {
    return SetFrameFlagMethod(L, "Usage: self:SetFixedFrameStrata(isFixed)", "SetFixedFrameStrata", CSimpleFrame::FRAME_FIXED_STRATA);
}

// ref: FUN_1409bdc10
int SetFlattensRenderLayers(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetFlattensRenderLayers(flatten)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool flatten = RequiredBool(L, &index, usage);
    frame->SetFlattenRenderLayers(flatten);
    RaisePendingError(L, frame, "SetFlattensRenderLayers");
    return 0;
}

// ref: FUN_1409be9e0
int SetFrameLevel(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetFrameLevel(frameLevel)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    uint16_t level = RequiredUInt16(L, &index, usage);
    frame->SetFrameLevelChecked(level);
    RaisePendingError(L, frame, "SetFrameLevel");
    return 0;
}

// ref: FUN_1409bf880
int SetFrameStrata(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetFrameStrata(strata)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    uint32_t strata = RequiredFrameStrata(L, &index, usage);
    frame->SetFrameStrataChecked(strata);
    RaisePendingError(L, frame, "SetFrameStrata");
    return 0;
}

// ref: FUN_1409c0730
int SetHitRectInsets(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetHitRectInsets(left, right, top, bottom)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    float left = RequiredUIUnits(L, &index, usage);
    float right = RequiredUIUnits(L, &index, usage);
    float top = RequiredUIUnits(L, &index, usage);
    float bottom = RequiredUIUnits(L, &index, usage);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->SetHitRectInsets(left, right, top, bottom);
    RaisePendingError(L, frame, "SetHitRectInsets");
    return 0;
}

// ref: FUN_1409c1750
int SetHyperlinksEnabled(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:SetHyperlinksEnabled([enabled])");
    bool enabled = OptionalBool(L, &index);
    frame->SetHyperlinksEnabled(enabled);
    RaisePendingError(L, frame, "SetHyperlinksEnabled");
    return 0;
}

// ref: FUN_1409c2520
int SetID(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetID(id)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    int32_t id = RequiredInt32(L, &index, usage);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->m_id = id;
    RaisePendingError(L, frame, "SetID");
    return 0;
}

// ref: FUN_1409c32f0
int SetIgnoreParentAlpha(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetIgnoreParentAlpha(ignore)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool ignore = RequiredBool(L, &index, usage);
    frame->SetIgnoreParentAlpha(ignore);
    RaisePendingError(L, frame, "SetIgnoreParentAlpha");
    return 0;
}

// ref: FUN_1409c40c0
int SetIgnoreParentScale(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetIgnoreParentScale(ignore)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool ignore = RequiredBool(L, &index, usage);
    frame->SetIgnoreParentScale(ignore);
    RaisePendingError(L, frame, "SetIgnoreParentScale");
    return 0;
}

// ref: FUN_1409c4e90
int SetIsFrameBuffer(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetIsFrameBuffer(isFrameBuffer)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool frameBuffer = RequiredBool(L, &index, usage);
    frame->SetIsFrameBuffer(frameBuffer);
    RaisePendingError(L, frame, "SetIsFrameBuffer");
    return 0;
}

// ref: FUN_1409c5c60
int SetMovable(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetMovable(movable)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool movable = RequiredBool(L, &index, usage);
    frame->SetMovable(movable);
    RaisePendingError(L, frame, "SetMovable");
    return 0;
}

// ref: FUN_1409c6a30
int SetPropagateKeyboardInput(lua_State* L) {
    return SetFrameFlagMethod(L, "Usage: self:SetPropagateKeyboardInput(propagate)", "SetPropagateKeyboardInput", CSimpleFrame::FRAME_PROPAGATE_KEYBOARD);
}

// ref: FUN_1409c7830
int SetResizable(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetResizable(resizable)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool resizable = RequiredBool(L, &index, usage);
    frame->SetResizable(resizable);
    RaisePendingError(L, frame, "SetResizable");
    return 0;
}

struct RESIZEBOUNDSCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    float m_minWidth = 0.0f;
    float m_minHeight = 0.0f;
    CLayoutFrame::OPTIONALBOUND m_maxWidth = {};
    CLayoutFrame::OPTIONALBOUND m_maxHeight = {};
};

// ref: FUN_1408dc370
int32_t SetResizeBoundsBody(lua_State* L, void* context) {
    auto ctx = static_cast<RESIZEBOUNDSCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:SetResizeBounds(minWidth, minHeight [, maxWidth, maxHeight])";
    ctx->m_frame = FrameSelf(L, &index, usage);
    ctx->m_minWidth = RequiredUIUnits(L, &index, usage);
    ctx->m_minHeight = RequiredUIUnits(L, &index, usage);
    ctx->m_maxWidth.m_value = OptionalUIUnits(L, &index, usage, &ctx->m_maxWidth.m_set);
    ctx->m_maxHeight.m_value = OptionalUIUnits(L, &index, usage, &ctx->m_maxHeight.m_set);
    Layout(ctx->m_frame)->SetResizeBounds(ctx->m_minWidth, ctx->m_minHeight, &ctx->m_maxWidth, &ctx->m_maxHeight);
    RaisePendingError(L, ctx->m_frame, "SetResizeBounds");
    return 0;
}

// ref: FUN_1409c8600
int SetResizeBounds(lua_State* L) {
    RESIZEBOUNDSCONTEXT context;
    return ProtectedCall(L, SetResizeBoundsBody, &context);
}

// ref: FUN_1409c9330
int SetScale(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetScale(scale)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    float scale = RequiredFinite(L, &index, usage);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->SetScale(scale, false);
    RaisePendingError(L, frame, "SetScale");
    return 0;
}

// ref: FUN_1409ca140
int SetShown(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:SetShown([shown])");
    bool shown = OptionalBool(L, &index);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->SetShown(shown);
    RaisePendingError(L, frame, "SetShown");
    return 0;
}

// ref: FUN_1409caf10
int SetToplevel(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetToplevel(topLevel)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool toplevel = RequiredBool(L, &index, usage);
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    if (((frame->m_frameFlags & CSimpleFrame::FRAME_TOPLEVEL) != 0) != toplevel) {
        frame->SetFrameFlag(CSimpleFrame::FRAME_TOPLEVEL, toplevel);
        frame->UpdateOwnBatch();
    }
    RaisePendingError(L, frame, "SetToplevel");
    return 0;
}

// ref: FUN_1409cbd00
int SetUserPlaced(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: self:SetUserPlaced(userPlaced)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    bool placed = RequiredBool(L, &index, usage);
    frame->SetUserPlaced(placed);
    RaisePendingError(L, frame, "SetUserPlaced");
    return 0;
}

// ref: FUN_1409ccad0
int Show(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:Show()");
    if (!CanChangeProtected(frame)) {
        return 0;
    }
    frame->Show();
    RaisePendingError(L, frame, "Show");
    return 0;
}

// ref: FUN_1409cd860
int StartMoving(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:StartMoving([alwaysStartFromMouse])");
    bool alwaysStartFromMouse = OptionalBool(L, &index);
    frame->StartMoving(alwaysStartFromMouse);
    RaisePendingError(L, frame, "StartMoving");
    return 0;
}

struct STARTSIZINGCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    int32_t m_point = 0;
    bool m_pointSet = false;
    bool m_alwaysStartFromMouse = false;
};

// ref: FUN_1408dd2d0
int32_t StartSizingBody(lua_State* L, void* context) {
    auto ctx = static_cast<STARTSIZINGCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: self:StartSizing([resizePoint, alwaysStartFromMouse])";
    ctx->m_frame = FrameSelf(L, &index, usage);
    ctx->m_point = static_cast<int32_t>(OptionalFramePoint(L, &index, usage, &ctx->m_pointSet));
    ctx->m_alwaysStartFromMouse = OptionalBool(L, &index);
    // The optional point as the reference passes it: the value, then its set flag at +4.
    struct {
        int32_t m_value;
        bool m_set;
    } point = {ctx->m_point, ctx->m_pointSet};
    ctx->m_frame->StartSizingChecked(&point.m_value, ctx->m_alwaysStartFromMouse);
    RaisePendingError(L, ctx->m_frame, "StartSizing");
    return 0;
}

// ref: FUN_1409ce630
int StartSizing(lua_State* L) {
    STARTSIZINGCONTEXT context;
    return ProtectedCall(L, StartSizingBody, &context);
}

// ref: FUN_1409cf360
int StopMovingOrSizing(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:StopMovingOrSizing()");
    frame->StopMovingOrSizing();
    RaisePendingError(L, frame, "StopMovingOrSizing");
    return 0;
}

// ref: FUN_1409d00f0
int UnregisterAllEvents(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:UnregisterAllEvents()");
    FrameScriptEvents::UnregisterAllEvents(frame);
    RaisePendingError(L, frame, "UnregisterAllEvents");
    return 0;
}

// ref: FUN_1409d0e80
int UnregisterEvent(lua_State* L) {
    int32_t index = 1;
    const char* usage = "Usage: local registered = self:UnregisterEvent(eventName)";
    CSimpleFrame* frame = FrameSelf(L, &index, usage);
    const char* name = RequiredString(L, &index, usage);
    bool unregistered = frame->UnregisterEvent(name);
    RaisePendingError(L, frame, "UnregisterEvent");
    return PushBool(L, unregistered);
}

// ---- CAnimatableObject -----------------------------------------------------------------

struct CREATEANIMGROUPCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    OPTIONALARG<const char*> m_name;
    OPTIONALARG<const char*> m_inherits;
    FrameScript_Object* m_group = nullptr;
};

// ref: FUN_1408029c0
int32_t CreateAnimationGroupBody(lua_State* L, void* context) {
    auto ctx = static_cast<CREATEANIMGROUPCONTEXT*>(context);
    int32_t index = 1;
    const char* usage = "Usage: local group = self:CreateAnimationGroup([name, templateName])";
    ctx->m_frame = FrameSelf(L, &index, usage);
    ctx->m_name.m_value = OptionalString(L, &index, usage, &ctx->m_name.m_set);
    ctx->m_inherits.m_value = OptionalString(L, &index, usage, &ctx->m_inherits.m_set);
    ctx->m_group = static_cast<CAnimatableObject*>(ctx->m_frame)->CreateAnimationGroup(
        ctx->m_name.m_set ? ctx->m_name.m_value : nullptr, ctx->m_inherits.m_set ? ctx->m_inherits.m_value : nullptr);
    RaisePendingError(L, ctx->m_frame, "CreateAnimationGroup");
    return PushObject(L, ctx->m_group);
}

// ref: FUN_1407eef90
int CreateAnimationGroup(lua_State* L) {
    CREATEANIMGROUPCONTEXT context;
    return ProtectedCall(L, CreateAnimationGroupBody, &context);
}

struct ANIMGROUPSCONTEXT {
    CSimpleFrame* m_frame = nullptr;
    ANIMGROUPLIST m_groups;
};

// ref: FUN_140806e10
int32_t GetAnimationGroupsBody(lua_State* L, void* context) {
    auto ctx = static_cast<ANIMGROUPSCONTEXT*>(context);
    int32_t index = 1;
    ctx->m_frame = FrameSelf(L, &index, "Usage: local (scriptObject)* = self:GetAnimationGroups()");
    static_cast<CAnimatableObject*>(ctx->m_frame)->GetScriptAnimationGroups(&ctx->m_groups);
    RaisePendingError(L, ctx->m_frame, "GetAnimationGroups");
    // FUN_140800d80
    int32_t results = 0;
    for (uint64_t i = 0; i < ctx->m_groups.m_size; i++) {
        results += PushObject(L, ctx->m_groups.m_data[i]);
    }
    return results;
}

// ref: FUN_1407f31b0
int GetAnimationGroups(lua_State* L) {
    ANIMGROUPSCONTEXT context;
    return ProtectedCall(L, GetAnimationGroupsBody, &context);
}

// ref: FUN_1407f74a0
int StopAnimating(lua_State* L) {
    int32_t index = 1;
    CSimpleFrame* frame = FrameSelf(L, &index, "Usage: self:StopAnimating()");
    static_cast<CAnimatableObject*>(frame)->StopAnimating();
    RaisePendingError(L, frame, "StopAnimating");
    return 0;
}

// 0x142cf7ae0
const luaL_Reg s_animatableMethods[] = {
    {"CreateAnimationGroup", CreateAnimationGroup},
    {"GetAnimationGroups", GetAnimationGroups},
    {"StopAnimating", StopAnimating},
};

// 0x142cf8610
const luaL_Reg s_frameMethods[] = {
    {"AbortDrag", AbortDrag},
    {"CanChangeAttribute", CanChangeAttribute},
    {"CreateFontString", CreateFontString},
    {"CreateLine", CreateLine},
    {"CreateMaskTexture", CreateMaskTexture},
    {"CreateTexture", CreateTexture},
    {"DesaturateHierarchy", DesaturateHierarchy},
    {"DisableDrawLayer", DisableDrawLayer},
    {"DoesClipChildren", DoesClipChildren},
    {"EnableDrawLayer", EnableDrawLayer},
    {"EnableGamePadButton", EnableGamePadButton},
    {"EnableGamePadStick", EnableGamePadStick},
    {"EnableKeyboard", EnableKeyboard},
    {"ExecuteAttribute", ExecuteAttribute},
    {"GetAlpha", GetAlpha},
    {"GetAttribute", GetAttribute},
    {"GetBoundsRect", GetBoundsRect},
    {"GetChildren", GetChildren},
    {"GetClampRectInsets", GetClampRectInsets},
    {"GetDontSavePosition", GetDontSavePosition},
    {"GetEffectiveAlpha", GetEffectiveAlpha},
    {"GetEffectiveScale", GetEffectiveScale},
    {"GetEffectivelyFlattensRenderLayers", GetEffectivelyFlattensRenderLayers},
    {"GetFlattensRenderLayers", GetFlattensRenderLayers},
    {"GetFrameLevel", GetFrameLevel},
    {"GetFrameStrata", GetFrameStrata},
    {"GetHitRectInsets", GetHitRectInsets},
    {"GetHyperlinksEnabled", GetHyperlinksEnabled},
    {"GetID", GetID},
    {"GetNumChildren", GetNumChildren},
    {"GetNumRegions", GetNumRegions},
    {"GetPropagateKeyboardInput", GetPropagateKeyboardInput},
    {"GetRegions", GetRegions},
    {"GetResizeBounds", GetResizeBounds},
    {"GetScale", GetScale},
    {"HasFixedFrameLevel", HasFixedFrameLevel},
    {"HasFixedFrameStrata", HasFixedFrameStrata},
    {"Hide", Hide},
    {"InterceptStartDrag", InterceptStartDrag},
    {"IsClampedToScreen", IsClampedToScreen},
    {"IsEventRegistered", IsEventRegistered},
    {"IsGamePadButtonEnabled", IsGamePadButtonEnabled},
    {"IsGamePadStickEnabled", IsGamePadStickEnabled},
    {"IsIgnoringParentAlpha", IsIgnoringParentAlpha},
    {"IsIgnoringParentScale", IsIgnoringParentScale},
    {"IsKeyboardEnabled", IsKeyboardEnabled},
    {"IsMovable", IsMovable},
    {"IsObjectLoaded", IsObjectLoaded},
    {"IsResizable", IsResizable},
    {"IsShown", IsShown},
    {"IsToplevel", IsToplevel},
    {"IsUserPlaced", IsUserPlaced},
    {"IsVisible", IsVisible},
    {"Lower", Lower},
    {"Raise", Raise},
    {"RegisterAllEvents", RegisterAllEvents},
    {"RegisterEvent", RegisterEvent},
    {"RegisterForDrag", RegisterForDrag},
    {"RegisterUnitEvent", RegisterUnitEvent},
    {"RotateTextures", RotateTextures},
    {"SetAlpha", SetAlpha},
    {"SetAttribute", SetAttribute},
    {"SetAttributeNoHandler", SetAttributeNoHandler},
    {"SetClampRectInsets", SetClampRectInsets},
    {"SetClampedToScreen", SetClampedToScreen},
    {"SetClipsChildren", SetClipsChildren},
    {"SetDontSavePosition", SetDontSavePosition},
    {"SetDrawLayerEnabled", SetDrawLayerEnabled},
    {"SetFixedFrameLevel", SetFixedFrameLevel},
    {"SetFixedFrameStrata", SetFixedFrameStrata},
    {"SetFlattensRenderLayers", SetFlattensRenderLayers},
    {"SetFrameLevel", SetFrameLevel},
    {"SetFrameStrata", SetFrameStrata},
    {"SetHitRectInsets", SetHitRectInsets},
    {"SetHyperlinksEnabled", SetHyperlinksEnabled},
    {"SetID", SetID},
    {"SetIgnoreParentAlpha", SetIgnoreParentAlpha},
    {"SetIgnoreParentScale", SetIgnoreParentScale},
    {"SetIsFrameBuffer", SetIsFrameBuffer},
    {"SetMovable", SetMovable},
    {"SetPropagateKeyboardInput", SetPropagateKeyboardInput},
    {"SetResizable", SetResizable},
    {"SetResizeBounds", SetResizeBounds},
    {"SetScale", SetScale},
    {"SetShown", SetShown},
    {"SetToplevel", SetToplevel},
    {"SetUserPlaced", SetUserPlaced},
    {"Show", Show},
    {"StartMoving", StartMoving},
    {"StartSizing", StartSizing},
    {"StopMovingOrSizing", StopMovingOrSizing},
    {"UnregisterAllEvents", UnregisterAllEvents},
    {"UnregisterEvent", UnregisterEvent},
};

}

// ref: FUN_1408a8630
void CAnimatableObject_FillScriptMethods(lua_State* L) {
    FrameScript_Object::FillScriptMethodTable(L, s_animatableMethods, sizeof(s_animatableMethods) / sizeof(s_animatableMethods[0]));
}

// ref: FUN_140a50ff0
void CSimpleFrame_FillScriptMethods(lua_State* L) {
    CScriptRegion_FillScriptMethods(L);
    CAnimatableObject_FillScriptMethods(L);
    FrameScript_Object::FillScriptMethodTable(L, s_frameMethods, sizeof(s_frameMethods) / sizeof(s_frameMethods[0]));
}
