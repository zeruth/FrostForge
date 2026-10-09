/*
** Blizzard's additions to Lua 5.1.4: taint and script profiling.
** Ported from WowClassic 3.4.3.54261; see BLIZZARD.md.
*/

#ifndef lblizzard_h
#define lblizzard_h

#include "lua.h"


/* The taint state (0x142e207c0..0x142e20800). */
LUA_API const char *lua_taint;           /* the current taint */
LUA_API int lua_taintTracking;           /* taint is tracked */
LUA_API int lua_taintSuspended;          /* propagation is suspended */
LUA_API const char *lua_closureTaint;    /* what a Lua closure made without one gets */
LUA_API const char *lua_objectTaint;     /* what objects made while tainted get, when set */
LUA_API const char *lua_firstTaint;      /* the first taint seen, latched */
LUA_API int lua_captureFirstTaint;       /* record where the first taint came from */
LUA_API int lua_decimalConversion;       /* swap '.' and ',' in numbers (lua_convertdecimal) */
LUA_API lua_Debug lua_firstTaintSource;  /* where the first taint came from (0x1447cf900) */

/* The taint log's hook (0x142e207e0, set by FUN_1419998b0 while taint.log is open): a global
** read (2) or written (1) with a tainted value, or a call blocked while tainted (3, 4). */
typedef void (*lua_TaintHook) (lua_State *L, int what, const char *name, const char *taint);
LUA_API lua_TaintHook lua_taintHook;

/* "script ran too long": limits in milliseconds on tainted execution (0x142e20808..). The
** first applies while luai_usefirsttimeout() says so, the second otherwise; the check one is
** the smaller of the two that are set. */
LUA_API unsigned int lua_scriptTimeoutFirst;
LUA_API unsigned int lua_scriptTimeoutSecond;
LUA_API unsigned int lua_scriptTimeoutCheck;
LUA_API void lua_setscripttimeout (unsigned int first, unsigned int second);
LUA_API void lua_getscripttimeout (unsigned int *first, unsigned int *second);


/* A closure's profile counters (0x20 bytes), when the state profiles scripts. */
typedef struct ClosureProfile {
  int calls;
  int depth;     /* calls out of it still running */
  long long start;  /* the tick its outermost pending call out began */
  long long time;      /* ticks spent in it, callees excluded */
  long long subtime;   /* ticks spent in it, callees included */
} ClosureProfile;


/* The client's high-resolution timer frequency, for profile times (OsTimerGetFrequency). */
LUAI_FUNC long long luai_timerfrequency (void);
/* The client's high-resolution timer now (OsTimerGetTicks). */
LUAI_FUNC long long luai_timerticks (void);

/* The client's millisecond tick count (FUN_141237e30, GetTickCount). */
LUAI_FUNC unsigned int luai_tickcount (void);
/* Which script timeout applies now (FUN_14192fa50: a flag on the UI root). */
LUAI_FUNC int luai_usefirsttimeout (void);

/* Makes `t' the current taint and latches it as the first. */
LUAI_FUNC void luaT_settaint (lua_State *L, const char *t);

/* Records the function running when the first taint was seen (FUN_1402b2360). */
LUAI_FUNC void luaT_capturefirsttaint (lua_State *L);

/* A value or object read while tracking: its taint becomes the current one. */
#define luaT_read(L,t) \
  { const char *t_ = (t); \
    if (t_ != NULL && lua_taintTracking && !lua_taintSuspended) luaT_settaint(L, t_); }

/* What a new object is stamped with. */
#define luaT_newtaint() \
  ((lua_taint != NULL && lua_objectTaint != NULL) ? lua_objectTaint : lua_taint)

/* A value copied onto the stack keeps its taint and taints the reader; an untainted one takes
** the current taint. */
#define setobj2s_read(L,obj1,obj2) \
  { const TValue *o2_=(obj2); TValue *o1_=(obj1); \
    o1_->value = o2_->value; o1_->tt=o2_->tt; o1_->taint = o2_->taint; \
    if (o1_->taint == NULL) o1_->taint = lua_taint; else luaT_read(L, o1_->taint); }

/* A value copied anywhere else keeps its taint, untainted or not, and taints the reader. */
#define setobj_read(L,obj1,obj2) \
  { const TValue *o2_=(obj2); TValue *o1_=(obj1); \
    o1_->value = o2_->value; o1_->tt=o2_->tt; o1_->taint = o2_->taint; \
    luaT_read(L, o1_->taint); }

/* Running a tainted closure: its taint becomes current and stays so, propagation suspended
** until it returns or calls out. An untainted closure leaves propagation on. */
#define luaT_enter(L,t) \
  { const char *e_ = (t); lua_taintSuspended = 0; \
    if (e_ != NULL) { if (lua_taintTracking) luaT_settaint(L, e_); lua_taintSuspended = 1; } }

#endif
