#ifndef UI_C_SCRIPT_OBJECT_SCRIPT_HPP
#define UI_C_SCRIPT_OBJECT_SCRIPT_HPP

// The Lua method tables of the UI object classes, each filled into the table a class's script
// metatable indexes (FrameScript_Object::CreateScriptMetaTable's fill function).

struct lua_State;

// FrameScript_Object's and CScriptObject's methods (GetName, GetParent, ...).
void CScriptObject_FillScriptMethods(lua_State* L);
// CScriptRegion's and CLayoutFrame's methods, after the object ones (Show, SetPoint, ...).
void CScriptRegion_FillScriptMethods(lua_State* L);
// CAnimatableObject's methods (CreateAnimationGroup, ...).
void CAnimatableObject_FillScriptMethods(lua_State* L);
// CSimpleFrame's methods, after all of the above (CreateTexture, RegisterEvent, ...).
void CSimpleFrame_FillScriptMethods(lua_State* L);

#endif
