/*
** Blizzard's `bit' library (lua-wowext): bitwise operations on 32-bit integers.
** Ported from WowClassic 3.4.3.54261; see BLIZZARD.md.
*/


#define lbitlib_c
#define LUA_LIB

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


/* ref: FUN_14242b410 */
static int bit_bnot (lua_State *L) {
  lua_Number n = luaL_checknumber(L, 1);
  lua_pushnumber(L, (lua_Number)(unsigned int)~(unsigned int)(long long)n);
  return 1;
}


/* ref: FUN_14242b450 */
static int bit_band (lua_State *L) {
  int n = lua_gettop(L);
  int i;
  unsigned long long r = (unsigned long long)luaL_checknumber(L, 1);
  for (i = 2; i <= n; i++)
    r = (unsigned int)r & (unsigned int)(long long)luaL_checknumber(L, i);
  lua_pushnumber(L, (lua_Number)(r & 0xffffffff));
  return 1;
}


/* ref: FUN_14242b4e0 */
static int bit_bor (lua_State *L) {
  int n = lua_gettop(L);
  int i;
  unsigned long long r = (unsigned long long)luaL_checknumber(L, 1);
  for (i = 2; i <= n; i++)
    r = (unsigned int)r | (unsigned int)(long long)luaL_checknumber(L, i);
  lua_pushnumber(L, (lua_Number)(r & 0xffffffff));
  return 1;
}


/* ref: FUN_14242b570 */
static int bit_bxor (lua_State *L) {
  int n = lua_gettop(L);
  int i;
  unsigned long long r = (unsigned long long)luaL_checknumber(L, 1);
  for (i = 2; i <= n; i++)
    r = (unsigned int)r ^ (unsigned int)(long long)luaL_checknumber(L, i);
  lua_pushnumber(L, (lua_Number)(r & 0xffffffff));
  return 1;
}


/* ref: FUN_14242b600 */
static int bit_lshift (lua_State *L) {
  lua_Number s = luaL_checknumber(L, 2);
  lua_Number n = luaL_checknumber(L, 1);
  lua_pushnumber(L, (lua_Number)(unsigned int)((int)(long long)n << ((long long)s & 0x1f)));
  return 1;
}


/* ref: FUN_14242b660 */
static int bit_rshift (lua_State *L) {
  lua_Number s = luaL_checknumber(L, 2);
  lua_Number n = luaL_checknumber(L, 1);
  lua_pushnumber(L, (lua_Number)((unsigned int)(long long)n >> ((long long)s & 0x1f)));
  return 1;
}


/* Signed: the result can be negative. */
/* ref: FUN_14242b6c0 */
static int bit_arshift (lua_State *L) {
  lua_Number s = luaL_checknumber(L, 2);
  lua_Number n = luaL_checknumber(L, 1);
  lua_pushnumber(L, (lua_Number)((int)(long long)n >> ((long long)s & 0x1f)));
  return 1;
}


/* A divisor that truncates to zero returns 1 divided by the divisor as given. */
/* ref: FUN_14242b720 */
static int bit_mod (lua_State *L) {
  int d = (int)(long long)luaL_checknumber(L, 2);
  if (d == 0) {
    lua_pushnumber(L, 1.0 / lua_tonumber(L, 2));
    return 1;
  }
  d = (int)(long long)luaL_checknumber(L, 2);
  lua_pushnumber(L, (lua_Number)((int)(long long)luaL_checknumber(L, 1) % d));
  return 1;
}


static const luaL_Reg bitlib[] = {
  {"bnot", bit_bnot},
  {"band", bit_band},
  {"bor", bit_bor},
  {"bxor", bit_bxor},
  {"lshift", bit_lshift},
  {"rshift", bit_rshift},
  {"arshift", bit_arshift},
  {"mod", bit_mod},
  {NULL, NULL}
};


/* ref: FUN_14242b7b0 */
LUALIB_API int luaopen_bit (lua_State *L) {
  luaI_openlib(L, LUA_BITLIBNAME, bitlib, 0);
  return 1;
}
