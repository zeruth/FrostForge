#ifndef FRAMESCRIPT_SCRIPT_METHOD_HPP
#define FRAMESCRIPT_SCRIPT_METHOD_HPP

// The pieces the reference's script-method template expands into for each Lua method of a
// UI object: reading self and the arguments (with the template's usage and range errors),
// pushing the results, and raising the error a method left pending. Each method of the
// reference inlines its own copy; the standalone functions carry their addresses.

#include <cstdint>

struct lua_State;
class FrameScript_Object;
class CScriptRegion;

namespace ScriptMethod {

// Self at *index (advanced), of a class with `type`, or the usage error.
// (FUN_1407feb40 and its per-class copies 0x140a54510, 0x1408c51d0)
FrameScript_Object* Self(lua_State* L, int32_t type, int32_t* index, const char* usage);

// Arguments; each advances *index past what it read.
bool OptionalBool(lua_State* L, int32_t* index);
bool RequiredBool(lua_State* L, int32_t* index, const char* usage);
const char* RequiredString(lua_State* L, int32_t* index, const char* usage);
const char* OptionalString(lua_State* L, int32_t* index, const char* usage, bool* present);
// A length in UI units (1024 to the screen height), as layout units.
float RequiredUIUnits(lua_State* L, int32_t* index, const char* usage);
float OptionalUIUnits(lua_State* L, int32_t* index, const char* usage, bool* present);
// A finite number (FUN_1408a77d0; the error is FUN_140800e30).
float RequiredFinite(lua_State* L, int32_t* index, const char* usage);
float OptionalFinite(lua_State* L, int32_t* index, const char* usage, float fallback);
// An integer within the type's range ("outside of expected range ...").
int32_t RequiredInt32(lua_State* L, int32_t* index, const char* usage);
uint16_t RequiredUInt16(lua_State* L, int32_t* index, const char* usage);
uint32_t RequiredUInt32(lua_State* L, int32_t* index, const char* usage);
uint32_t OptionalIndex(lua_State* L, int32_t* index, const char* usage);
int8_t OptionalSubLevel(lua_State* L, int32_t* index, const char* usage, bool* present);
// Named values; an unknown name is the usage error, a missing optional one the default.
uint32_t RequiredDrawLayer(lua_State* L, int32_t* index, const char* usage);
uint32_t OptionalDrawLayer(lua_State* L, int32_t* index, const char* usage, bool* present);
uint32_t RequiredFrameStrata(lua_State* L, int32_t* index, const char* usage);
uint32_t RequiredFramePoint(lua_State* L, int32_t* index, const char* usage);
uint32_t OptionalFramePoint(lua_State* L, int32_t* index, const char* usage, bool* present);
// An alpha from 0 to 1, as 0..255.
uint8_t RequiredAlpha(lua_State* L, int32_t* index, const char* usage);
// The mouse button names from *index to the end of the arguments, OR'd.
uint32_t MouseButtons(lua_State* L, int32_t* index, const char* usage, uint32_t* buttons, uint64_t capacity, uint64_t* count);

// A number in UI units as layout units: v / 1024 / aspect compensation * diagonal width.
float ToLayoutUnits(double value);

// Results; each returns the values it pushed.
int32_t PushBool(lua_State* L, bool value);
int32_t PushStringOrNil(lua_State* L, const char* value);
int32_t PushLString(lua_State* L, const char* value, uint64_t length);
int32_t PushAlpha(lua_State* L, uint8_t alpha);
int32_t PushUIUnits(lua_State* L, float value);
int32_t PushFloat(lua_State* L, float value);
int32_t PushInt(lua_State* L, int32_t value);
int32_t PushUInt(lua_State* L, uint32_t value);
int32_t PushObject(lua_State* L, FrameScript_Object* object);
int32_t PushFramePoint(lua_State* L, uint32_t point);

// The error the method left pending, raised.
void RaisePendingError(lua_State* L, FrameScript_Object* object, const char* method);

// Whether a protected region may be changed by the code running now; when it may not, the
// action-blocked hook hears of it. (FUN_1424c4ec0)
bool CanChangeProtected(CScriptRegion* region);

// Whether tainted code may take restricted action `action` now; a refusal is reported as a
// blocked action. Untainted code may always. (FUN_1419985c0)
bool IsActionPermitted(uint32_t action);

// A table made from a named mixin: the global's fields copied in.
void PushFromMixin(lua_State* L, const char* mixin);

// Runs `body` as a protected Lua call with the arguments on the stack (FUN_1407d0960): an
// error is copied out, the body's locals unwound, and the error raised again. Returns the
// results left on the stack.
typedef int32_t (*BODY)(lua_State* L, void* context);
int32_t ProtectedCall(lua_State* L, BODY body, void* context);
// Makes the trampoline ProtectedCall runs bodies through (FUN_1407d20b0), and forgets it when
// the state goes (FUN_1407e9330).
void CreateTrampoline(lua_State* L);
void ReleaseTrampoline();

}

#endif
