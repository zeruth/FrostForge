#ifndef UI_DEV_LUA_HPP
#define UI_DEV_LUA_HPP

// A development aid (DevLua.cpp): runs the file FROST_LUA names in the UI's Lua state when it
// changes. Called once per UI update; nothing without the variable.
void DevLuaPoll();

#endif
