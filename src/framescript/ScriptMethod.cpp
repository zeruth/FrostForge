#include "framescript/ScriptMethod.hpp"

#include <cmath>
#include <cstring>

#include "base/Coordinate.hpp"
#include "client/Locale.hpp"
#include "blz/String.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptObject.hpp"
#include "ui/CScriptRegion.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/UITypes.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

namespace ScriptMethod {

namespace {

// The registry reference of the trampoline closure.
int32_t s_trampolineRef = LUA_NOREF;  // 0x142cf70b4

const double FLOAT_MAX = 3.4028234663852886e+38;

bool Present(lua_State* L, int32_t index) {
    // FUN_1402b4a30: anything but none and nil
    return lua_type(L, index) >= 1;
}

void CheckStack(lua_State* L) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
    }
}

// The template's argument error: the usage, behind the range message when there is one.
// (FUN_1407cbb50 when `range` is empty)
void ArgError(lua_State* L, int32_t index, const char* usage, const blz::string_header* range) {
    const char* message = usage;
    if (range != nullptr && range->m_size != 0) {
        message = lua_pushfstring(L, "%s - %s", range->m_data, usage);
    }
    luaL_argerror(L, index, message);
}

bool IsFinite(double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return ((bits >> 52) & 0x7ff) != 0x7ff;
}

// FUN_1408a77d0
bool IsFiniteNumber(lua_State* L, int32_t index) {
    if (!lua_isnumber(L, index)) {
        return false;
    }
    double value = lua_tonumber(L, index);
    return IsFinite(value) && -FLOAT_MAX <= value && value <= FLOAT_MAX;
}

// FUN_140800e30
void FiniteError(lua_State* L, int32_t index, const char* usage) {
    blz::inline_string<0x18> range;
    double value = lua_tonumber(L, index);
    if (!IsFinite(value)) {
        blz::string_assign(range.header(), "must be a finite number", 0x17);
    } else {
        blz::string_format(range.header(), "outside of expected range %e to %e", -FLOAT_MAX, FLOAT_MAX);
    }
    ArgError(L, index, usage, range.header());
}

// A number within [low, high], or the range error formatted with `format`.
template <class T>
T RangedNumber(lua_State* L, int32_t index, const char* usage, double low, double high, const char* format, T formatLow, T formatHigh,
    double* value) {
    if (!lua_isnumber(L, index) || !IsFinite(lua_tonumber(L, index)) || lua_tonumber(L, index) < low || high < lua_tonumber(L, index)) {
        blz::inline_string<0x18> range;
        blz::string_format(range.header(), format, formatLow, formatHigh);
        ArgError(L, index, usage, range.header());
    }
    CheckStack(L);
    *value = lua_tonumber(L, index);
    return T();
}

// The report of a refused restricted action: the calling function's "file:line" sent
// as ADDON_ACTION_FORBIDDEN or ADDON_ACTION_BLOCKED (kind 0 or the others). (FUN_141999900)
void ReportBlockedAction(int32_t kind) {
    (void)kind;
    // The report goes through the event dispatcher (FUN_141273d80's hashed slots), which is
    // not ported; the refusal itself still stands.
    FROST_UNIMPLEMENTED();
}

// FUN_1407d2b20
int Trampoline(lua_State* L) {
    auto body = *static_cast<BODY*>(lua_touserdata(L, lua_upvalueindex(1)));
    void* context = lua_touserdata(L, lua_upvalueindex(2));
    return body(L, context);
}

}

float ToLayoutUnits(double value) {
    const COORDINATESPACE& space = g_coordinateSpace;
    return static_cast<float>(value) * (0.0009765625f / space.m_aspectCompensation) * space.m_diagonalWidth;
}

// ref: FUN_1419985c0
bool IsActionPermitted(uint32_t action) {
    CSimpleTop* top = CSimpleTop::s_instance;
    if (lua_taint != nullptr) {
        switch (action) {
        case 0x0: case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x10: case 0x11: case 0x14:
        case 0x1a: case 0x1e: case 0x1f: case 0x20: case 0x21: case 0x24: case 0x25: case 0x26:
        case 0x27: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x30:
            ReportBlockedAction(0);
            return false;
        case 0x6: case 0x7: case 0x8: case 0x9: case 0xf: case 0x12: case 0x13: case 0x15: case 0x17:
        case 0x19: case 0x1b: case 0x1c: case 0x22: case 0x29: case 0x36: case 0x38: case 0x3c: case 0x40:
            if (top == nullptr) {
                return true;
            }
            if (!top->m_inMouseScript) {
                ReportBlockedAction(2);
                return false;
            }
            break;
        case 0xb: case 0xc: case 0xd: case 0xe: case 0x16: case 0x18: case 0x1d: case 0x23: case 0x28:
            if (top == nullptr) {
                return true;
            }
            if (!top->IsSecureLoad()) {
                ReportBlockedAction(3);
                return false;
            }
            break;
        case 0x2a:
            if (top == nullptr) {
                return true;
            }
            if (!top->m_inMouseScript && RegionGet() != 5) {
                ReportBlockedAction(2);
                return false;
            }
            break;
        default:
            break;
        }
    }
    // A hardware event is spent by the actions that need one.
    if (top != nullptr && ((action < 0x1a && ((0x2000101u >> (action & 0x1f)) & 1) != 0) || (action == 0x2a && RegionGet() != 5))) {
        top->m_inMouseScript = 0;
    }
    return true;
}

// ref: FUN_140800cc0
FrameScript_Object* Self(lua_State* L, int32_t type, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
        return nullptr;
    }
    if (FrameScript_Object::GetObjectFromLua(nullptr, L, type, *index, false, false) == nullptr) {
        ArgError(L, *index, usage, nullptr);
        return nullptr;
    }
    CheckStack(L);
    FrameScript_Object* object = FrameScript_Object::GetObjectFromLua(nullptr, L, type, *index, false, false);
    (*index)++;
    return object;
}

bool OptionalBool(lua_State* L, int32_t* index) {
    bool value = false;
    if (Present(L, *index)) {
        CheckStack(L);
        value = lua_toboolean(L, *index) != 0;
    }
    (*index)++;
    return value;
}

bool RequiredBool(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    CheckStack(L);
    bool value = lua_toboolean(L, *index) != 0;
    (*index)++;
    return value;
}

const char* RequiredString(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    if (!lua_isstring(L, *index)) {
        ArgError(L, *index, usage, nullptr);
    }
    CheckStack(L);
    const char* value = lua_tolstring(L, *index, nullptr);
    (*index)++;
    return value;
}

const char* OptionalString(lua_State* L, int32_t* index, const char* usage, bool* present) {
    *present = false;
    if (!Present(L, *index)) {
        (*index)++;
        return nullptr;
    }
    if (!lua_isstring(L, *index)) {
        ArgError(L, *index, usage, nullptr);
    }
    CheckStack(L);
    const char* value = lua_tolstring(L, *index, nullptr);
    (*index)++;
    *present = true;
    return value;
}

float RequiredUIUnits(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    if (!lua_isnumber(L, *index)) {
        ArgError(L, *index, usage, nullptr);
    }
    CheckStack(L);
    float value = ToLayoutUnits(lua_tonumber(L, *index));
    (*index)++;
    return value;
}

float OptionalUIUnits(lua_State* L, int32_t* index, const char* usage, bool* present) {
    *present = false;
    if (!Present(L, *index)) {
        (*index)++;
        return 0.0f;
    }
    if (!lua_isnumber(L, *index)) {
        ArgError(L, *index, usage, nullptr);
    }
    CheckStack(L);
    float value = ToLayoutUnits(lua_tonumber(L, *index));
    (*index)++;
    *present = true;
    return value;
}

// ref: FUN_1408a77d0
float RequiredFinite(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    if (!IsFiniteNumber(L, *index)) {
        FiniteError(L, *index, usage);
    }
    CheckStack(L);
    float value = static_cast<float>(lua_tonumber(L, *index));
    (*index)++;
    return value;
}

float OptionalFinite(lua_State* L, int32_t* index, const char* usage, float fallback) {
    if (!Present(L, *index)) {
        (*index)++;
        return fallback;
    }
    if (!IsFiniteNumber(L, *index)) {
        FiniteError(L, *index, usage);
    }
    CheckStack(L);
    float value = static_cast<float>(lua_tonumber(L, *index));
    (*index)++;
    return value;
}

// FUN_1408c5050
// ref: FUN_1407d2350
int32_t RequiredInt32(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    double value;
    RangedNumber<int32_t>(L, *index, usage, -2147483648.0, 2147483647.0, "outside of expected range %i to %i",
        static_cast<int32_t>(0x80000000), 0x7fffffff, &value);
    (*index)++;
    return static_cast<int32_t>(value);
}

uint16_t RequiredUInt16(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    double value;
    RangedNumber<uint32_t>(L, *index, usage, 0.0, 65535.0, "outside of expected range %u to %u", 0u, 0xffffu, &value);
    (*index)++;
    return static_cast<uint16_t>(static_cast<int32_t>(value));
}

// FUN_1407d22e0, its error FUN_1407cbaa0.
uint32_t RequiredUInt32(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    double value;
    RangedNumber<uint32_t>(L, *index, usage, 0.0, 4294967295.0, "outside of expected range %u to %u", 0u, 0xffffffffu, &value);
    (*index)++;
    return static_cast<uint32_t>(static_cast<int64_t>(value));
}

// A 1-based index, as 0-based.
uint32_t OptionalIndex(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        (*index)++;
        return 0;
    }
    double value;
    RangedNumber<uint32_t>(L, *index, usage, 0.0, 4294967295.0, "outside of expected range %u to %u", 0u, 0xffffffffu, &value);
    (*index)++;
    return static_cast<uint32_t>(static_cast<int32_t>(value - 1.0));
}

// FUN_1407fe550
int8_t OptionalSubLevel(lua_State* L, int32_t* index, const char* usage, bool* present) {
    *present = false;
    if (!Present(L, *index)) {
        (*index)++;
        return 0;
    }
    double value;
    RangedNumber<int32_t>(L, *index, usage, -128.0, 127.0, "outside of expected range %i to %i", -128, 127, &value);
    (*index)++;
    *present = true;
    return static_cast<int8_t>(static_cast<int32_t>(value));
}

namespace {

// The named-value pattern: a string the parser accepts, or the usage error.
template <class Parse>
uint32_t NamedValue(lua_State* L, int32_t* index, const char* usage, Parse parse, uint32_t fallback) {
    uint32_t value = 0;
    if (lua_type(L, *index) != LUA_TSTRING || !parse(lua_tolstring(L, *index, nullptr), &value)) {
        ArgError(L, *index, usage, nullptr);
    }
    CheckStack(L);
    uint32_t parsed = fallback;
    if (lua_type(L, *index) == LUA_TSTRING && parse(lua_tolstring(L, *index, nullptr), &value)) {
        parsed = value;
    }
    (*index)++;
    return parsed;
}

}

uint32_t RequiredDrawLayer(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    return NamedValue(L, index, usage, StringToDrawLayer, DRAWLAYER_ARTWORK);
}

uint32_t OptionalDrawLayer(lua_State* L, int32_t* index, const char* usage, bool* present) {
    *present = false;
    if (!Present(L, *index)) {
        (*index)++;
        return DRAWLAYER_ARTWORK;
    }
    uint32_t layer = NamedValue(L, index, usage, StringToDrawLayer, DRAWLAYER_ARTWORK);
    *present = true;
    return layer;
}

uint32_t RequiredFrameStrata(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    return NamedValue(L, index, usage, StringToFrameStrata, FRAME_STRATA_MEDIUM);
}

// FUN_1407ff4b0
uint32_t RequiredFramePoint(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    return NamedValue(L, index, usage, StringToFramePoint, 0);
}

uint32_t OptionalFramePoint(lua_State* L, int32_t* index, const char* usage, bool* present) {
    *present = false;
    if (!Present(L, *index)) {
        (*index)++;
        return 0;
    }
    uint32_t point = NamedValue(L, index, usage, StringToFramePoint, 0);
    *present = true;
    return point;
}

uint8_t RequiredAlpha(lua_State* L, int32_t* index, const char* usage) {
    if (!Present(L, *index)) {
        luaL_error(L, usage);
    }
    auto valid = [&]() {
        if (lua_type(L, *index) != LUA_TNUMBER) {
            return false;
        }
        float value = static_cast<float>(lua_tonumber(L, *index));
        return !(value < 0.0f) && !(1.0f < value);
    };
    if (!valid()) {
        ArgError(L, *index, usage, nullptr);
    }
    CheckStack(L);
    uint8_t alpha = 0;
    if (valid()) {
        alpha = static_cast<uint8_t>(static_cast<int32_t>(static_cast<float>(lua_tonumber(L, *index)) * 255.0f + 0.5f));
    }
    (*index)++;
    return alpha;
}

// FUN_1408c4d20
uint32_t MouseButtons(lua_State* L, int32_t* index, const char* usage, uint32_t* buttons, uint64_t capacity, uint64_t* count) {
    uint32_t mask = 0;
    *count = 0;
    while (*index <= lua_gettop(L)) {
        uint32_t button = 0;
        if (!Present(L, *index)) {
            luaL_error(L, usage);
        } else if (lua_type(L, *index) != LUA_TSTRING) {
            ArgError(L, *index, usage, nullptr);
        } else {
            CheckStack(L);
            button = StringToMouseButton(lua_tolstring(L, *index, nullptr));
            (*index)++;
        }
        if (*count < capacity) {
            buttons[*count] = button;
            (*count)++;
        }
        mask |= button;
    }
    return mask;
}

// FUN_140800d20
int32_t PushBool(lua_State* L, bool value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushboolean(L, value);
    return 1;
}

// FUN_1407cb130
// ref: FUN_140800bf0
int32_t PushStringOrNil(lua_State* L, const char* value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    if (value == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, value, std::strlen(value));
    return 1;
}

// FUN_140800bf0
int32_t PushLString(lua_State* L, const char* value, uint64_t length) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushlstring(L, value, length);
    return 1;
}

// FUN_1408c6d60
int32_t PushAlpha(lua_State* L, uint8_t alpha) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(static_cast<float>(alpha) * 0.003921568859368563f));
    return 1;
}

// FUN_140800b70
int32_t PushUIUnits(lua_State* L, float value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    const COORDINATESPACE& space = g_coordinateSpace;
    lua_pushnumber(L, static_cast<double>((space.m_aspectCompensation * 1024.0f * value) / space.m_diagonalWidth));
    return 1;
}

// FUN_140800790
int32_t PushFloat(lua_State* L, float value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(value));
    return 1;
}

// FUN_1408006d0
int32_t PushInt(lua_State* L, int32_t value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(value));
    return 1;
}

// FUN_140800730
int32_t PushUInt(lua_State* L, uint32_t value) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(value));
    return 1;
}

// FUN_1408008a0
// ref: FUN_140800ae0
int32_t PushObject(lua_State* L, FrameScript_Object* object) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    if (object != nullptr) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, object->GetLuaRef());
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// FUN_140800a40
int32_t PushFramePoint(lua_State* L, uint32_t point) {
    if (!lua_checkstack(L, 1)) {
        luaL_error(L, "stack overflow");
        return 0;
    }
    const char* name = FramePointToString(point);
    if (name == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, name, std::strlen(name));
    return 1;
}

void RaisePendingError(lua_State* L, FrameScript_Object* object, const char* method) {
    if (FrameScript_PushObjectError(L, object, method)) {
        lua_error(L);
    }
}

// FUN_1424c4ec0
bool CanChangeProtected(CScriptRegion* region) {
    if (region == nullptr) {
        return false;
    }
    if (region->CanQueryRect()) {
        return true;
    }
    if (CSimpleTop::s_instance->m_actionBlockedHook != nullptr) {
        CSimpleTop::s_instance->m_actionBlockedHook(reinterpret_cast<CSimpleFrame*>(region));
    }
    return false;
}

// ref: FUN_1407d0a90
void PushFromMixin(lua_State* L, const char* mixin) {
    lua_createtable(L, 0, 2);
    lua_getfield(L, LUA_GLOBALSINDEX, mixin);
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_settable(L, -5);
        }
    }
    lua_settop(L, -2);
}

// FUN_1407d0960, with its callers' error handling
int32_t ProtectedCall(lua_State* L, BODY body, void* context) {
    int32_t count = lua_gettop(L);
    bool ok;
    char error[200] = {};
    if (!lua_checkstack(L, 2)) {
        lua_settop(L, 0);
        lua_pushlstring(L, "stack overflow", 0xe);
        ok = false;
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_trampolineRef);
        lua_pushlightuserdata(L, &body);
        lua_setupvalue(L, -2, 1);
        lua_pushlightuserdata(L, context);
        lua_setupvalue(L, -2, 2);
        lua_insert(L, lua_gettop(L) - count);
        ok = lua_pcall(L, count, LUA_MULTRET, 0) == 0;
    }
    if (!ok) {
        const char* message = lua_tolstring(L, -1, nullptr);
        if (message != nullptr) {
            std::strncpy(error, message, sizeof(error) - 1);
        }
    }
    if (error[0] == '\0') {
        return lua_gettop(L);
    }
    lua_pushlstring(L, error, std::strlen(error));
    return lua_error(L);
}

// ref: FUN_1407d20b0
void CreateTrampoline(lua_State* L) {
    lua_pushlightuserdata(L, nullptr);
    lua_pushlightuserdata(L, nullptr);
    lua_pushcclosure(L, Trampoline, 2);
    s_trampolineRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

// ref: FUN_1407e9330
void ReleaseTrampoline() {
    s_trampolineRef = -1;
}

}
