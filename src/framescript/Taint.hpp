#ifndef FRAMESCRIPT_TAINT_HPP
#define FRAMESCRIPT_TAINT_HPP

// The taint scope every FrameScript entry point inlines around foreign code: the taint state
// survives an error (lua_pushjmptaint), tracking nests, and the new taint becomes current;
// leaving puts the previous taint back and unwinds tracking.

extern "C" {
#include "lua.h"
#include "lblizzard.h"
}

inline void FrameScript_EnterTaint(lua_State* L, const char* taint) {
    lua_pushjmptaint(L, lua_taint, lua_taintTracking);
    lua_taintTracking++;
    if (lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, taint);
    }
}

inline void FrameScript_LeaveTaint(lua_State* L, const char* previous) {
    if (lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, previous);
    }
    lua_taintTracking--;
    lua_popjmptaint(L);
    if (lua_taintTracking < 1) {
        lua_taintTracking = 0;
        lua_firstTaint = nullptr;
    }
}

// The read of a value's taint: it becomes current while tracking.
inline void FrameScript_ReadTaint(lua_State* L, const char* taint) {
    if (taint != nullptr && lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, taint);
    }
}

#endif
