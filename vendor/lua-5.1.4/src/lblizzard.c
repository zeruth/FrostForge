/*
** Blizzard's additions to Lua 5.1.4: taint and script profiling.
** Ported from WowClassic 3.4.3.54261; see BLIZZARD.md.
*/

#include <stddef.h>
#include <string.h>

#define lblizzard_c
#define LUA_CORE

#include "lua.h"

#include "lblizzard.h"
#include "lobject.h"
#include "lstate.h"


const char *lua_taint;           /* 0x142e207c0 */
int lua_taintTracking;           /* 0x142e207c8 */
int lua_taintSuspended;          /* 0x142e207cc */
const char *lua_closureTaint;    /* 0x142e207d0 */
const char *lua_objectTaint;     /* 0x142e207d8 */
const char *lua_firstTaint;      /* 0x142e207e8 */
int lua_captureFirstTaint;       /* 0x142e207f0 */
int lua_decimalConversion;       /* 0x142e207f8 */
lua_Debug lua_firstTaintSource;  /* 0x1447cf900 */
lua_TaintHook lua_taintHook;     /* 0x142e207e0 */
unsigned int lua_scriptTimeoutFirst;   /* 0x142e20808 */
unsigned int lua_scriptTimeoutSecond;  /* 0x142e2080c */
unsigned int lua_scriptTimeoutCheck;   /* 0x142e20810 */

/* The strings lua_firstTaintSource points at, kept past the frame (0x1447cf980..). */
static char firstTaintName[0x400];
static char firstTaintNameWhat[0x400];
static char firstTaintWhat[0x400];
static char firstTaintSourceText[0x400];


/* The layouts the rest of the client relies on (BLIZZARD.md). */
#define LAYOUT(name, cond) typedef char layout_##name[(cond) ? 1 : -1]
LAYOUT(tvalue, sizeof(TValue) == 0x18 && offsetof(TValue, taint) == 0x10);
LAYOUT(header, offsetof(Table, flags) == 0x12);
LAYOUT(state, sizeof(lua_State) == 0xe0 && offsetof(lua_State, status) == 0x20 &&
    offsetof(lua_State, top) == 0x28 && offsetof(lua_State, l_gt) == 0x90 &&
    offsetof(lua_State, errfunc) == 0xd8);
LAYOUT(global, sizeof(global_State) == 0x1d8 && offsetof(global_State, currentwhite) == 0x21 &&
    offsetof(global_State, l_registry) == 0xb0 && offsetof(global_State, uvhead) == 0xd0 &&
    offsetof(global_State, mt) == 0x108);
LAYOUT(cclosure, offsetof(CClosure, f) == 0x30 && offsetof(CClosure, upvalue) == 0x38);
LAYOUT(lclosure, offsetof(LClosure, p) == 0x30 && offsetof(LClosure, upvals) == 0x38);
LAYOUT(upval, sizeof(UpVal) == 0x38 && offsetof(UpVal, u) == 0x20);
LAYOUT(table, sizeof(Table) == 0x48 && offsetof(Table, sizearray) == 0x40);
LAYOUT(profile, sizeof(ClosureProfile) == 0x20);


/*
** The reference also writes every new current taint into an obfuscated per-thread copy
** (lua_State +0x18, FUN_1402bd260) that anti-tamper checks compare with lua_taint. That copy is
** not ported (Aegis).
*/
void luaT_settaint (lua_State *L, const char *t) {
  lua_taint = t;
  if (t != NULL && lua_firstTaint == NULL) {
    lua_firstTaint = t;
    if (lua_captureFirstTaint)
      luaT_capturefirsttaint(L);
  }
}


/* ref: FUN_1402b7f60 */
LUA_API void lua_setscripttimeout (unsigned int first, unsigned int second) {
  lua_scriptTimeoutFirst = first;
  lua_scriptTimeoutSecond = second;
  if (first != 0 && second != 0)
    lua_scriptTimeoutCheck = (first < second) ? first : second;
  else
    lua_scriptTimeoutCheck = (first < second) ? second : first;
}


/* ref: FUN_1402b7f40 */
LUA_API void lua_getscripttimeout (unsigned int *first, unsigned int *second) {
  if (first != NULL)
    *first = lua_scriptTimeoutFirst;
  if (second != NULL)
    *second = lua_scriptTimeoutSecond;
}


/* Records the function running when the first taint was seen.
** ref: FUN_1402b2360 */
void luaT_capturefirsttaint (lua_State *L) {
  memset(&lua_firstTaintSource, 0, sizeof(lua_firstTaintSource));
  if (lua_getstack(L, 0, &lua_firstTaintSource))
    lua_getinfo(L, "Snl", &lua_firstTaintSource);
  if (lua_firstTaintSource.name != NULL) {
    strncpy(firstTaintName, lua_firstTaintSource.name, sizeof(firstTaintName));
    lua_firstTaintSource.name = firstTaintName;
  }
  if (lua_firstTaintSource.namewhat != NULL) {
    strncpy(firstTaintNameWhat, lua_firstTaintSource.namewhat, sizeof(firstTaintNameWhat));
    lua_firstTaintSource.namewhat = firstTaintNameWhat;
  }
  if (lua_firstTaintSource.what != NULL) {
    strncpy(firstTaintWhat, lua_firstTaintSource.what, sizeof(firstTaintWhat));
    lua_firstTaintSource.what = firstTaintWhat;
  }
  if (lua_firstTaintSource.source != NULL) {
    strncpy(firstTaintSourceText, lua_firstTaintSource.source, sizeof(firstTaintSourceText));
    lua_firstTaintSource.source = firstTaintSourceText;
  }
}
