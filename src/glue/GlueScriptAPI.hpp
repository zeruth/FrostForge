#ifndef GLUE_GLUE_SCRIPT_API_HPP
#define GLUE_GLUE_SCRIPT_API_HPP

struct lua_State;

// Registers the glue's script API: namespaces, Enum tables, globals and class metatables
// (FUN_140afd040).
void GlueRegisterScriptAPI(lua_State* L);

#endif
