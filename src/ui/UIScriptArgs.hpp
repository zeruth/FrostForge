#ifndef UI_UI_SCRIPT_ARGS_HPP
#define UI_UI_SCRIPT_ARGS_HPP

// Argument readers and result pushers the region and frame script methods share
// (ui/CSimpleFontStringScript.cpp).

#include <cstdint>

struct lua_State;

// Three numbers 0..1 as 0xffRRGGBB (FUN_140a50ab0).
uint32_t RequiredRGB(lua_State* L, int32_t* index, const char* usage);
// An optional alpha 0..1 into {value, present}.
void OptionalAlphaByte(lua_State* L, int32_t* index, const char* usage, uint8_t* alpha);
// A justification name (FUN_1408c6380).
uint32_t RequiredJustify(lua_State* L, int32_t* index, const char* usage);
// Two raw numbers.
void RequiredPair(lua_State* L, int32_t* index, const char* usage, float* values);
// A colour as r, g, b, a (FUN_140a50e10).
int32_t PushColor(lua_State* L, uint32_t argb);
// A justification as its name (FUN_1408c6e70).
int32_t PushJustify(lua_State* L, uint32_t justify);

#endif
