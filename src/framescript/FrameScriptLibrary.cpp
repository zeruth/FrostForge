// The global functions FrameScript adds to every Lua state (0x1426cda00): string helpers,
// the secure-execution API, the error handler, profiling, date/time, and debugging aids.

#include "framescript/FrameScript.hpp"

#include <cmath>
#include <cstring>
#include <ctime>
#include <cwchar>

#include "blz/Random.hpp"
#include "os/Timer.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
#include "lualib.h"
}

namespace {

const char s_taintForced[] = "*** TaintForced ***";

// The script-visible clock debugprofilestart reset (0x142e9a1a8).
uint64_t s_debugProfileStart;

// The pattern every FrameScript entry point wraps foreign code in: the taint state survives an
// error (lua_pushjmptaint), tracking nests, and `taint` becomes current. PopTaint undoes it.
void PushTaint(lua_State* L, const char* taint) {
    lua_pushjmptaint(L, lua_taint, lua_taintTracking);
    lua_taintTracking++;
    if (lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, taint);
    }
}

void PopTaint(lua_State* L, const char* taint) {
    if (lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, taint);
    }
    lua_taintTracking--;
    lua_popjmptaint(L);
    if (lua_taintTracking < 1) {
        lua_taintTracking = 0;
        lua_firstTaint = nullptr;
    }
}

bool InSet(const char* set, char c) {
    for (; *set != '\0'; set++) {
        if (*set == c) {
            return true;
        }
    }
    return false;
}

void PushString(lua_State* L, const char* s) {
    if (s == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, s, strlen(s));
    }
}

// strtrim(string [, chars]): `chars` (default " \t\r\n") removed from both ends.
// ref: FUN_140648ef0
int Script_strtrim(lua_State* L) {
    const char* s = luaL_checklstring(L, 1, nullptr);
    const char* chars = luaL_optlstring(L, 2, " \t\r\n", nullptr);
    uint32_t length = static_cast<uint32_t>(strlen(s));
    lua_settop(L, 1);
    if (length == 0 || *chars == '\0') {
        return 1;
    }
    if (!InSet(chars, s[0]) && !InSet(chars, s[length - 1])) {
        return 1;
    }
    while (InSet(chars, *s)) {
        s++;
        length--;
    }
    while (length != 0 && InSet(chars, s[length - 1])) {
        length--;
    }
    lua_pushlstring(L, s, length);
    return 1;
}

// strsplit(delimiters, string [, pieces]): the pieces between any of the delimiter
// characters, at most `pieces` of them (the last keeps the rest).
// ref: FUN_140649c90
int Script_strsplit(lua_State* L) {
    const char* delimiters = luaL_checklstring(L, 1, nullptr);
    const char* s = luaL_checklstring(L, 2, nullptr);
    int32_t limit = static_cast<int32_t>(luaL_optinteger(L, 3, 0));
    lua_settop(L, 0);
    int32_t count = 0;
    if (limit >= 0 && limit != 1) {
        for (const char* p = s; *p != '\0'; p++) {
            if (*delimiters != '\0' && InSet(delimiters, *p)) {
                count++;
                if (!lua_checkstack(L, count)) {
                    luaL_error(L, "strsplit(): Stack overflow");
                }
                lua_pushlstring(L, s, p - s);
                s = p + 1;
                if (count == limit - 1) {
                    break;
                }
            }
        }
    }
    if (!lua_checkstack(L, count + 1)) {
        luaL_error(L, "strsplit(): Stack overflow");
    }
    PushString(L, s);
    return count + 1;
}

// strsplittable(delimiters, string [, pieces]): strsplit's pieces in an array.
// ref: FUN_14064b020
int Script_strsplittable(lua_State* L) {
    const char* delimiters = luaL_checklstring(L, 1, nullptr);
    const char* s = luaL_checklstring(L, 2, nullptr);
    int32_t limit = static_cast<int32_t>(luaL_optinteger(L, 3, 0));
    lua_createtable(L, 0, 0);
    int32_t count = 0;
    if (limit >= 0 && limit != 1) {
        for (const char* p = s; *p != '\0'; p++) {
            if (*delimiters != '\0' && InSet(delimiters, *p)) {
                count++;
                lua_pushinteger(L, count);
                lua_pushlstring(L, s, p - s);
                lua_settable(L, -3);
                s = p + 1;
                if (count == limit - 1) {
                    break;
                }
            }
        }
    }
    lua_pushinteger(L, count + 1);
    PushString(L, s);
    lua_settable(L, -3);
    return 1;
}

// strjoin(separator, ...)
// ref: FUN_14064b7f0
int Script_strjoin(lua_State* L) {
    size_t separatorLength;
    const char* separator = luaL_checklstring(L, 1, &separatorLength);
    int32_t top = lua_gettop(L);
    int32_t count = top - 1;
    if (separatorLength == 0) {
        lua_concat(L, top);
        return 1;
    }
    if (count == 0) {
        lua_pushlstring(L, "", 0);
        return 1;
    }
    if (!lua_checkstack(L, count * 2)) {
        luaL_error(L, "strjoin(): Stack overflow");
    }
    for (int32_t i = 0, at = 3; i < count - 1; i++, at += 2) {
        lua_pushlstring(L, separator, separatorLength);
        lua_insert(L, at);
    }
    lua_concat(L, count * 2 - 1);
    return 1;
}

// ref: FUN_14064d130
int Script_strconcat(lua_State* L) {
    lua_concat(L, lua_gettop(L));
    return 1;
}

// ref: FUN_14065a530
int Script_strlenutf8(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: strlenutf8(string)");
        return 0;
    }
    lua_pushnumber(L, static_cast<double>(SStrLenUTF8(lua_tolstring(L, 1, nullptr))));
    return 1;
}

// ref: FUN_14065b220
int Script_strcmputf8i(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) {
        luaL_error(L, "Usage: strcmputf8i(string1, string2)");
        return 0;
    }
    const char* b = lua_tolstring(L, 2, nullptr);
    const char* a = lua_tolstring(L, 1, nullptr);
    lua_pushinteger(L, SStrCmpUTF8Fold(a, b, 0x7FFFFFFF));
    return 1;
}

// ref: FUN_14064d790
int Script_issecure(lua_State* L) {
    lua_pushboolean(L, lua_taint == nullptr);
    return 1;
}

// issecurevariable([table,] name): whether the variable's value is untainted, and its taint.
// ref: FUN_14064ddf0
int Script_issecurevariable(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_insert(L, 1);
    }
    if (!lua_isstring(L, 2)) {
        luaL_error(L, "Usage: issecurevariable([table,] \"variable\")");
        return 0;
    }
    const char* name = lua_tolstring(L, 2, nullptr);
    const char* previous = lua_taint;
    PushTaint(L, nullptr);
    PushString(L, name);
    lua_gettable(L, 1);
    const char* taint = lua_taint;
    lua_settop(L, -3);
    PopTaint(L, previous);
    lua_pushboolean(L, taint == nullptr);
    PushString(L, taint);
    return 2;
}

// ref: FUN_14064ec30
int Script_forceinsecure(lua_State* L) {
    if (lua_taint == nullptr && lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, s_taintForced);
    }
    return 0;
}

// Calls the function at 1 with the rest as arguments under the error handler; the results
// come back carrying `taint`, the caller's.
// ref: FUN_14065df40
int SecureCall(lua_State* L, const char* taint) {
    if (lua_gettop(L) == 0) {
        lua_pushnil(L);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetErrorHandlerRef());
    lua_insert(L, 1);
    if (lua_pcall(L, lua_gettop(L) - 2, LUA_MULTRET, 1) == 0) {
        lua_remove(L, 1);
    } else {
        lua_settop(L, -3);
    }
    int32_t count = lua_gettop(L);
    for (int32_t i = 1; i <= count; i++) {
        lua_settaint(L, i, taint);
    }
    return count;
}

// securecall(function or name, ...)
// ref: FUN_14064f2d0
int Script_securecall(lua_State* L) {
    const char* previous = lua_taint;
    PushTaint(L, lua_taint);
    if (lua_isstring(L, 1)) {
        PushString(L, lua_tolstring(L, 1, nullptr));
        lua_rawget(L, LUA_GLOBALSINDEX);
        lua_remove(L, 1);
        lua_insert(L, 1);
    }
    int count = SecureCall(L, previous);
    PopTaint(L, previous);
    return count;
}

// ref: FUN_14064fab0
int Script_securecallfunction(lua_State* L) {
    const char* previous = lua_taint;
    PushTaint(L, lua_taint);
    int count = SecureCall(L, previous);
    PopTaint(L, previous);
    return count;
}

struct EXECUTERANGE {
    int32_t m_extra;    // arguments after the callback
    int32_t m_args;     // what the callback gets: key, value and the extras
    lua_State** m_L;
};

// One step of secureexecuterange: the next pair, handed to the callback under the error
// handler's protection; false once the table is done.
// ref: FUN_14065d3d0
bool ExecuteRangeStep(EXECUTERANGE* range) {
    lua_State* L = *range->m_L;
    const char* previous = lua_taint;
    PushTaint(L, lua_taint);
    int more = lua_next(L, 1);
    if (more) {
        lua_pushvalue(L, 2);
        lua_pushvalue(L, -3);
        lua_pushvalue(L, -3);
        for (int32_t i = 0; i < range->m_extra; i++) {
            lua_pushvalue(L, i + 3);
        }
        if (lua_pcall(L, range->m_args, 0, 0) != 0) {
            lua_settop(L, -2);
        }
        lua_settop(L, -2);
    }
    PopTaint(L, previous);
    return more != 0;
}

// secureexecuterange(table, callback, ...)
// ref: FUN_140650240
int Script_secureexecuterange(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE || lua_type(L, 2) != LUA_TFUNCTION) {
        luaL_error(L, "Usage: secureexecuterange(table, callback)");
        return 0;
    }
    EXECUTERANGE range;
    range.m_L = &L;
    range.m_args = lua_gettop(L);
    range.m_extra = range.m_args - 2;
    lua_pushnil(L);
    while (ExecuteRangeStep(&range)) {
    }
    return 0;
}

// The closure hooksecurefunc installs: the original function (upvalue 1) runs first and its
// results are returned; the hook (upvalue 2) then runs with the same arguments under the
// error handler, its results dropped.
// ref: FUN_1406466d0
int SecureHook(lua_State* L) {
    int32_t args = lua_gettop(L);
    lua_checkstack(L, args);
    for (int32_t i = 1; i <= args; i++) {
        lua_pushvalue(L, i);
    }
    lua_checkstack(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, args + 1);
    lua_call(L, args, LUA_MULTRET);
    int32_t results = lua_gettop(L) - args;
    for (int32_t i = 0; i < results; i++) {
        lua_insert(L, 1);
    }
    lua_checkstack(L, 1);
    const char* previous = lua_taint;
    PushTaint(L, lua_taint);
    lua_checkstack(L, 1);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, results + 1);
    lua_checkstack(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetErrorHandlerRef());
    lua_insert(L, results + 1);
    int status = lua_pcall(L, args, 0, results + 1);
    lua_settop(L, -2 - (status != 0 ? 1 : 0));
    PopTaint(L, previous);
    return results;
}

// hooksecurefunc([table,] name, hook): replaces table[name] with a closure that calls the
// original and then `hook`, keeping the original's taint on the replacement.
// ref: FUN_140650ef0
int Script_hooksecurefunc(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        lua_insert(L, 1);
    }
    if (!lua_isstring(L, 2) || lua_type(L, 3) != LUA_TFUNCTION) {
        luaL_error(L, "Usage: hooksecurefunc([table,] \"function\", hookfunc)");
        return 0;
    }
    lua_settop(L, 3);
    const char* name = lua_tolstring(L, 2, nullptr);
    lua_remove(L, 2);
    const char* previous = lua_taint;
    PushTaint(L, nullptr);
    PushString(L, name);
    lua_gettable(L, 1);
    bool hooked;
    if (lua_type(L, -1) == LUA_TFUNCTION) {
        const char* original = lua_taint;
        PushTaint(L, lua_taint);
        lua_insert(L, 2);
        lua_settaint(L, 2, original);
        lua_settaint(L, 3, previous);
        lua_pushcclosure(L, SecureHook, 2);
        lua_settaint(L, 2, original);
        PopTaint(L, original);
        PushString(L, name);
        lua_insert(L, 2);
        lua_rawset(L, 1);
        hooked = true;
    } else {
        lua_settop(L, -3);
        hooked = false;
    }
    lua_settop(L, -2);
    PopTaint(L, previous);
    if (!hooked) {
        luaL_error(L, "hooksecurefunc(): %s is not a function", name);
    }
    return 0;
}

// ref: FUN_1406524e0
int Script_debugprofilestart(lua_State* L) {
    s_debugProfileStart = OsTimerGetTicks();
    return 0;
}

// Milliseconds since debugprofilestart.
// ref: FUN_140652b30
int Script_debugprofilestop(lua_State* L) {
    int64_t elapsed = static_cast<int64_t>(OsTimerGetTicks() - s_debugProfileStart);
    lua_pushnumber(L, FrameScript_GetProfileTickMs() * static_cast<double>(elapsed));
    return 1;
}

// ref: FUN_1406531b0
int Script_seterrorhandler(lua_State* L) {
    if (lua_type(L, 1) != LUA_TFUNCTION) {
        luaL_error(L, "Usage: seterrorhandler(errfunc)");
        return 0;
    }
    if (FrameScript_GetUserErrorHandler() != -1) {
        luaL_unref(L, LUA_REGISTRYINDEX, FrameScript_GetUserErrorHandler());
    }
    FrameScript_SetUserErrorHandler(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

// ref: FUN_140653e80
int Script_geterrorhandler(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetUserErrorHandler());
    return 1;
}

// A debug print compiled out of this build (FUN_140255c60).
// ref: FUN_1406544f0
int Script_addframetext(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        luaL_error(L, "Usage: addframetext(\"text\")");
        return 0;
    }
    lua_tolstring(L, 1, nullptr);
    return 0;
}

struct PCALLARGS {
    lua_State* L;
    int32_t m_args;
};

// pcall's result convention: true and the results, or false and the message.
// ref: FUN_14065d2e0
int ProtectedCall(PCALLARGS* call) {
    lua_State* L = call->L;
    if (lua_pcall(L, call->m_args, LUA_MULTRET, 0) != 0) {
        lua_pushboolean(L, 0);
        lua_insert(L, 1);
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    return lua_gettop(L);
}

// pcallwithenv(f, env, ...): calls a Lua function with `env` as its environment for the
// duration of the call.
// ref: FUN_1406551c0
int Script_pcallwithenv(lua_State* L) {
    if (lua_type(L, 1) != LUA_TFUNCTION || lua_type(L, 2) != LUA_TTABLE) {
        return luaL_error(L, "Usage: local success, ... = pcallwithenv(f, env, [args...])");
    }
    PCALLARGS call;
    call.L = L;
    call.m_args = lua_gettop(L) - 2;
    if (lua_iscfunction(L, 1)) {
        lua_remove(L, 2);
        return ProtectedCall(&call);
    }
    lua_getfenv(L, 1);
    if (luaL_getmetafield(L, -1, "__environment")) {
        luaL_error(L, "cannot use pcallwithenv on a function with a protected environment");
        return 0;
    }
    int32_t oldEnvironment = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_remove(L, 2);
    lua_setfenv(L, -2);
    int32_t function = luaL_ref(L, LUA_REGISTRYINDEX);
    int count = ProtectedCall(&call);
    lua_rawgeti(L, LUA_REGISTRYINDEX, function);
    luaL_unref(L, LUA_REGISTRYINDEX, function);
    lua_rawgeti(L, LUA_REGISTRYINDEX, oldEnvironment);
    luaL_unref(L, LUA_REGISTRYINDEX, oldEnvironment);
    lua_setfenv(L, -2);
    lua_settop(L, -2);
    return count;
}

void SetField(lua_State* L, const char* key, int32_t value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

// date([format [, time]]): os.date, formatted through the wide strftime so the result is UTF-8.
// ref: FUN_140655fb0
int Script_date(lua_State* L) {
    const char* format = luaL_optlstring(L, 1, "%c", nullptr);
    __time64_t t;
    if (lua_type(L, 2) < 1) {
        t = _time64(nullptr);
    } else {
        t = static_cast<__time64_t>(luaL_checknumber(L, 2));
    }
    tm* stm;
    if (*format == '!') {
        stm = _gmtime64(&t);
        format++;
    } else {
        stm = _localtime64(&t);
    }
    if (stm == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    if (format[0] == '*' && format[1] == 't' && format[2] == '\0') {
        lua_createtable(L, 0, 9);
        SetField(L, "sec", stm->tm_sec);
        SetField(L, "min", stm->tm_min);
        SetField(L, "hour", stm->tm_hour);
        SetField(L, "day", stm->tm_mday);
        SetField(L, "month", stm->tm_mon + 1);
        SetField(L, "year", stm->tm_year + 1900);
        SetField(L, "wday", stm->tm_wday + 1);
        SetField(L, "yday", stm->tm_yday + 1);
        if (stm->tm_isdst >= 0) {
            lua_pushboolean(L, stm->tm_isdst);
            lua_setfield(L, -2, "isdst");
        }
        return 1;
    }
    wchar_t wideFormat[256];
    wchar_t wideResult[256];
    char result[256];
    if (UnicodeConvertUtf8ToUtf16(wideFormat, 0x100, format, 0x7FFFFFFF, nullptr, nullptr) != 0
        || wcsftime(wideResult, 0x100, wideFormat, stm) == 0
        || UnicodeConvertUtf16ToUtf8(result, 0x100, reinterpret_cast<const uint16_t*>(wideResult), 0x7FFFFFFF, nullptr, nullptr) != 0) {
        return luaL_error(L, "'date' format too long");
    }
    lua_pushlstring(L, result, strlen(result));
    return 1;
}

int GetField(lua_State* L, const char* key, int32_t fallback) {
    lua_getfield(L, -1, key);
    int32_t value;
    if (lua_isnumber(L, -1)) {
        value = static_cast<int32_t>(lua_tointeger(L, -1));
    } else {
        if (fallback < 0) {
            return luaL_error(L, "field '%s' missing in date table", key);
        }
        value = fallback;
    }
    lua_settop(L, -2);
    return value;
}

// time([table]): os.time.
// ref: FUN_140656fd0
int Script_time(lua_State* L) {
    __time64_t t;
    if (lua_type(L, 1) < 1) {
        t = _time64(nullptr);
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_settop(L, 1);
        tm ts = {};
        ts.tm_sec = GetField(L, "sec", 0);
        ts.tm_min = GetField(L, "min", 0);
        ts.tm_hour = GetField(L, "hour", 12);
        ts.tm_mday = GetField(L, "day", -1);
        ts.tm_mon = GetField(L, "month", -1) - 1;
        ts.tm_year = GetField(L, "year", -1) - 1900;
        lua_getfield(L, -1, "isdst");
        ts.tm_isdst = lua_type(L, -1) == LUA_TNIL ? -1 : lua_toboolean(L, -1);
        lua_settop(L, -2);
        t = _mktime64(&ts);
    }
    if (t == -1) {
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, static_cast<double>(t));
    }
    return 1;
}

// ref: FUN_1406578d0
int Script_difftime(lua_State* L) {
    double later = luaL_optnumber(L, 2, 0);
    double earlier = luaL_checknumber(L, 1);
    lua_pushnumber(L, _difftime64(static_cast<__time64_t>(earlier), static_cast<__time64_t>(later)));
    return 1;
}

// debugstack([thread,] [start [, top [, bottom]]]): a traceback of `top` levels from
// `start`, and when it is cut the last `bottom` levels after "...".
// ref: FUN_140657f60
int Script_debugstack(lua_State* L) {
    if (lua_isingc(L)) {
        return 0;
    }
    bool firstPart = true;
    int32_t top = 12;
    int32_t bottom = 10;
    lua_State* L1 = L;
    int isThread = lua_type(L, 1) == LUA_TTHREAD;
    if (isThread) {
        L1 = lua_tothread(L, 1);
        lua_remove(L, 1);
    }
    uint32_t level = isThread ? 0 : 1;
    if (lua_isnumber(L, 1)) {
        level = static_cast<uint32_t>(lua_tonumber(L, 1));
        lua_remove(L, 1);
    }
    if (lua_isnumber(L, 1)) {
        top = static_cast<int32_t>(static_cast<int64_t>(lua_tonumber(L, 1)));
        lua_remove(L, 1);
    }
    if (lua_isnumber(L, 1)) {
        bottom = static_cast<int32_t>(static_cast<int64_t>(lua_tonumber(L, 1)));
        lua_remove(L, 1);
    }
    if (lua_gettop(L) == 0) {
        lua_pushlstring(L, "", 0);
    } else if (!lua_isstring(L, 1)) {
        return 1;
    }
    lua_Debug ar;
    uint32_t limit = top + level;
    while (lua_getstack(L1, level, &ar)) {
        uint32_t next = level + 1;
        if (limit < next && firstPart) {
            if (!lua_getstack(L1, next + bottom, &ar)) {
                firstPart = false;
            } else {
                lua_pushlstring(L, "...\n", 4);
                while (lua_getstack(L1, next + bottom, &ar)) {
                    next++;
                }
                firstPart = false;
                level = next;
            }
            continue;
        }
        lua_getinfo(L1, "Snl", &ar);
        if (ar.source == nullptr) {
            lua_pushfstring(L, "%s:", ar.short_src);
        } else {
            lua_pushfstring(L, "[string \"%s\"]:", ar.source);
        }
        if (ar.currentline > 0) {
            lua_pushfstring(L, "%d:", ar.currentline);
        }
        char kind = *ar.namewhat;
        if (kind == 'f' || kind == 'g' || kind == 'l' || kind == 'm') {
            lua_pushfstring(L, " in function `%s'", ar.name);
        } else if (*ar.what == 'm') {
            lua_pushfstring(L, " in main chunk");
        } else if (*ar.what == 'C' || *ar.what == 't') {
            lua_pushlstring(L, " ?", 2);
        } else {
            lua_pushfstring(L, " in function <%s:%d>", ar.short_src, ar.linedefined);
        }
        lua_pushlstring(L, "\n", 1);
        lua_concat(L, lua_gettop(L));
        level = next;
    }
    lua_concat(L, lua_gettop(L));
    return 1;
}

// The indent the local dump nests with: one space per level (0x142e99fe0).
char s_indent[128];

// Appends to a NUL-terminated buffer of `size` bytes, truncating.
void Append(char* buffer, uint32_t size, const char* text) {
    uint64_t room = size;
    char* end = buffer;
    while (room != 0 && *end != '\0') {
        room--;
        end++;
    }
    if (room == 0) {
        return;
    }
    while (*text != '\0') {
        if (room != 1) {
            *end++ = *text;
            room--;
        }
        text++;
    }
    *end = '\0';
}

// A number for the dump: "Infinite" for an infinity or NaN, an integer without decimals,
// otherwise six places. False when the value is not a number.
// ref: FUN_14063efb0
bool FormatNumber(lua_State* L, int32_t index, blz::inline_string<0x20>* out) {
    if (!lua_isnumber(L, index)) {
        return false;
    }
    double value = lua_tonumber(L, index);
    if (!std::isfinite(value)) {
        blz::string_assign(out->header(), "Infinite", 8);
        return true;
    }
    double whole;
    if (modf(value, &whole) == 0.0) {
        blz::string_format(out->header(), "%lld", static_cast<long long>(value));
    } else {
        blz::string_format(out->header(), "%.6f", value);
    }
    return true;
}

// Dumps the value on top as "<indent>name = value", tables one level deep.
// ref: FUN_14063f920
void DumpValue(lua_State* L, const char* name, uint32_t depth, char* buffer, uint32_t size) {
    char line[0x400];
    if (depth < 0x80) {
        if (static_cast<int32_t>(depth) > 0) {
            s_indent[depth - 1] = ' ';
        }
        s_indent[depth] = '\0';
    }
    switch (lua_type(L, -1)) {
        case LUA_TNIL:
            SStrPrintf(line, 0x400, "%s%s = nil\n", s_indent, name);
            Append(buffer, size, line);
            break;
        case LUA_TBOOLEAN:
            SStrPrintf(line, 0x400, "%s%s = %s\n", s_indent, name, lua_toboolean(L, -1) ? "true" : "false");
            Append(buffer, size, line);
            break;
        case LUA_TLIGHTUSERDATA:
        case LUA_TUSERDATA:
            SStrPrintf(line, 0x400, "%s%s = <userdata>\n", s_indent, name);
            Append(buffer, size, line);
            break;
        case LUA_TNUMBER: {
            blz::inline_string<0x20> number;
            FormatNumber(L, -1, &number);
            SStrPrintf(line, 0x400, "%s%s = %s\n", s_indent, name, number.m_data);
            Append(buffer, size, line);
            break;
        }
        case LUA_TSTRING:
            SStrPrintf(line, 0x400, "%s%s = \"%s\"\n", s_indent, name, lua_tolstring(L, -1, nullptr));
            Append(buffer, size, line);
            break;
        case LUA_TTABLE: {
            const char* label = "<table>";
            lua_rawgeti(L, -1, 0);
            void* object = lua_type(L, -1) == LUA_TLIGHTUSERDATA ? lua_touserdata(L, -1) : nullptr;
            lua_settop(L, -2);
            if (object) {
                // The object's GetName (slot 1), else its debug name (slot 12): CScriptObject is
                // not ported yet.
                FROST_UNIMPLEMENTED();
            }
            SStrPrintf(line, 0x400, "%s%s = %s {\n", s_indent, name, label);
            Append(buffer, size, line);
            if (depth == 0) {
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    blz::inline_string<0x20> key;
                    if (FormatNumber(L, -2, &key)) {
                        DumpValue(L, key.m_data, 1, buffer, size);
                    } else {
                        lua_pushvalue(L, -2);
                        const char* text = lua_tolstring(L, -1, nullptr);
                        blz::inline_string<0x3E8> copy;
                        blz::string_assign(copy.header(), text ? text : "", strlen(text ? text : ""));
                        lua_settop(L, -2);
                        DumpValue(L, copy.m_data, 1, buffer, size);
                    }
                    lua_settop(L, -2);
                }
            }
            SStrPrintf(line, 0x400, "%s}\n", s_indent, name, label);
            Append(buffer, size, line);
            break;
        }
        case LUA_TFUNCTION: {
            lua_Debug ar;
            lua_pushvalue(L, -1);
            lua_getinfo(L, ">nS", &ar);
            if (ar.name != nullptr && ar.source != nullptr) {
                SStrPrintf(line, 0x400, "%s%s = %s() defined %s:%d\n", s_indent, name, ar.name, ar.source, ar.linedefined);
            } else if (ar.source == nullptr) {
                SStrPrintf(line, 0x400, "%s%s = <function> defined %d\n", s_indent, name, ar.linedefined);
            } else {
                SStrPrintf(line, 0x400, "%s%s = <function> defined %s:%d\n", s_indent, name, ar.source, ar.linedefined);
            }
            Append(buffer, size, line);
            break;
        }
        default:
            SStrPrintf(line, 0x400, "%s%s = <%s>\n", s_indent, name, lua_typename(L, lua_type(L, -1)));
            Append(buffer, size, line);
            break;
    }
    if (depth < 0x80 && static_cast<int32_t>(depth) > 0) {
        s_indent[depth - 1] = '\0';
    }
}

// debuglocals([level]): the locals and upvalues of the function `level` frames up.
// ref: FUN_140658ed0
int Script_debuglocals(lua_State* L) {
    if (lua_isingc(L)) {
        return 0;
    }
    int32_t level = static_cast<int32_t>(lua_tonumber(L, 1));
    if (level < 1) {
        level = 1;
    }
    char buffer[0x1000] = {};
    lua_Debug ar;
    if (lua_getstack(L, level, &ar)) {
        int32_t n = 1;
        for (const char* local = lua_getlocal(L, &ar, n); local; local = lua_getlocal(L, &ar, ++n)) {
            DumpValue(L, local, 0, buffer, sizeof(buffer));
            lua_settop(L, -2);
        }
        lua_getinfo(L, "f", &ar);
        n = 1;
        for (const char* upvalue = lua_getupvalue(L, -1, n); upvalue; upvalue = lua_getupvalue(L, -1, ++n)) {
            DumpValue(L, upvalue, 0, buffer, sizeof(buffer));
            lua_settop(L, -2);
        }
        lua_settop(L, -2);
    }
    lua_pushlstring(L, buffer, strlen(buffer));
    return 1;
}

// scrub(...): the arguments with anything but booleans, numbers and strings made nil.
// ref: FUN_140659de0
int Script_scrub(lua_State* L) {
    int32_t count = lua_gettop(L);
    lua_checkstack(L, 1);
    for (int32_t i = 1; i <= count; i++) {
        int type = lua_type(L, i);
        if (type != LUA_TBOOLEAN && type != LUA_TNUMBER && type != LUA_TSTRING) {
            lua_pushnil(L);
            lua_replace(L, i);
        }
    }
    return count;
}

// math.random over the RC4 keystream: 53 random bits scaled to [0, 1).
// ref: FUN_14065bf30
int Script_securerandom(lua_State* L) {
    uint64_t bits = 0;
    ARC4Process(FrameScript_GetRandom(), reinterpret_cast<uint8_t*>(&bits), 8);
    double r = static_cast<double>(bits & 0x1FFFFFFFFFFFFF) * 0x1p-53;
    int32_t low;
    int32_t high;
    switch (lua_gettop(L)) {
        case 0:
            lua_pushnumber(L, r);
            return 1;
        case 1:
            low = 1;
            high = static_cast<int32_t>(luaL_checkinteger(L, 1));
            break;
        case 2:
            low = static_cast<int32_t>(luaL_checkinteger(L, 1));
            high = static_cast<int32_t>(luaL_checkinteger(L, 2));
            break;
        default:
            return luaL_error(L, "Usage: securerandom(); securerandom(highint); securerandom(lowint, highint)");
    }
    if (high < low) {
        luaL_argerror(L, 1, "interval is empty");
    }
    lua_pushnumber(L, floor(static_cast<double>(high - low + 1) * r) + static_cast<double>(low));
    return 1;
}

}  // namespace

extern const luaL_Reg g_frameScriptGlobals[] = {
    { "strtrim", Script_strtrim },
    { "strsplit", Script_strsplit },
    { "strsplittable", Script_strsplittable },
    { "strjoin", Script_strjoin },
    { "strconcat", Script_strconcat },
    { "strlenutf8", Script_strlenutf8 },
    { "strcmputf8i", Script_strcmputf8i },
    { "issecure", Script_issecure },
    { "issecurevariable", Script_issecurevariable },
    { "forceinsecure", Script_forceinsecure },
    { "securecall", Script_securecall },
    { "securecallfunction", Script_securecallfunction },
    { "secureexecuterange", Script_secureexecuterange },
    { "hooksecurefunc", Script_hooksecurefunc },
    { "debugprofilestart", Script_debugprofilestart },
    { "debugprofilestop", Script_debugprofilestop },
    { "seterrorhandler", Script_seterrorhandler },
    { "geterrorhandler", Script_geterrorhandler },
    { "addframetext", Script_addframetext },
    { "fastrandom", math_random },
    { "pcallwithenv", Script_pcallwithenv },
    { "date", Script_date },
    { "time", Script_time },
    { "difftime", Script_difftime },
    { "debugstack", Script_debugstack },
    { "debuglocals", Script_debuglocals },
    { "scrub", Script_scrub },
    { nullptr, nullptr },
};

extern const luaL_Reg g_frameScriptMath[] = {
    { "random", Script_securerandom },
    { nullptr, nullptr },
};
