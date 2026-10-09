#ifndef FRAMESCRIPT_FRAME_SCRIPT_OBJECT_HPP
#define FRAMESCRIPT_FRAME_SCRIPT_OBJECT_HPP

// FrameScript objects (Engine/Source/FrameScript/FrameScript_Object.cpp): the base of every
// object Lua can see -- its registry table, protection flags, the script handlers it carries,
// and the XML that sets them.

#include <cstdint>

struct lua_State;
struct XMLOBJECT;
struct XMLSCRIPTHANDLER;
class CStatus;

// The handler names, lower case: "onclick", "preclick", ..., "onuimapchanged".
constexpr uint32_t FRAMESCRIPT_NUM_SCRIPTS = 0x4f;
// The index of a handler name (any case), or FRAMESCRIPT_NUM_SCRIPTS + 1 if unknown.
uint32_t FrameScript_GetScriptIndex(const char* name);

// What GetScriptData reports about a handler an object accepts: the wrapper a body is
// compiled in, and the frame update the handler needs when set (-1 for none).
struct FRAMESCRIPT_SCRIPTDATA {
    const char* m_wrapper;
    int32_t m_update;
};

// The empty description FrameScript_Object returns from slot 6 (0x1426cd550, 16 zero bytes).
struct FRAMESCRIPT_TYPEINFO {
    const void* m_data;
    uint64_t m_count;
};

// The flags of FrameScript_Object::m_flags.
enum : uint16_t {
    FSOBJ_FORBIDDEN = 0x1,
    FSOBJ_PROTECTED = 0x2,
    FSOBJ_PROTECTED_METATABLE = 0x4,    // the metatable has been switched to the protected one
    FSOBJ_LOCKDOWN = 0x8,
    FSOBJ_ACCESSIBLE = 0x20,
    FSOBJ_REGISTERED = 0x40,
    FSOBJ_HIDDEN = 0x100,               // never handed to scripts nor given a global name
};

class FrameScript_Object {
    public:
    // One handler set on an object (0x10).
    struct Script {
        const char* m_taint;        // 0x00
        int32_t m_ref;              // 0x08 the function's registry reference
        uint8_t m_index;            // 0x0C FrameScript_GetScriptIndex
        uint8_t m_binding;          // 0x0D 0 the handler, 1 a pre-hook, 2 a post-hook
    };

    // The counter the per-class object type ids come from (0x142e979f0).
    static int32_t s_objectTypeCount;

    int32_t m_luaRef;               // 0x08 the registry reference of its table (-2 none)
    uint16_t m_flags;               // 0x0C

    FrameScript_Object();
    virtual ~FrameScript_Object();
    virtual const char* GetName() = 0;
    // The registry reference of the metatable its table gets.
    virtual int32_t GetScriptMetaTable() = 0;
    virtual void OnScriptsChanged(uint8_t index, bool hasScript);
    // Whether it accepts the handler, and how.
    virtual bool GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data);
    // Adds the profile times and call counts of its handlers.
    virtual void GetScriptCPUUsage(lua_State* L, double* time, int32_t* calls);
    virtual const FRAMESCRIPT_TYPEINFO* GetTypeInfo();
    virtual void Unknown38();
    virtual void PreLoadXML(XMLOBJECT* node, CStatus* status);
    virtual void OnProtected();
    virtual bool IsObjectTypeName(const char* name);
    virtual bool IsObjectType(int32_t type);
    virtual const char* GetObjectTypeName();
    virtual uint32_t GetObjectSize();

    // This class's type id (0x142e97a94).
    static int32_t GetObjectType();

    // Pushes the object's table, registering it first; nil for no object, an inaccessible
    // one, or one hidden from scripts.
    static int32_t PushObject(lua_State* L, FrameScript_Object* object);
    // Builds a metatable whose __index is a table `fill` populates, optionally locked, and
    // returns its registry reference (-1 if it could not be made).
    static int32_t CreateScriptMetaTable(lua_State* L, void (*fill)(lua_State*), bool forbidden);
    // Adds `count` {name, function} pairs to the table at the top.
    static void FillScriptMethodTable(lua_State* L, const void* methods, int32_t count);
    // The object a method was called on (self at `index`), or null with `*error` set.
    static FrameScript_Object* GetObjectFromLua(const char** error, lua_State* L, int32_t type, int32_t index,
        bool allowForbidden, bool allowLockdown);

    bool IsAccessible();
    bool IsRegistered();
    int32_t GetLuaRef();
    int32_t GetRawLuaRef();
    void Register(const char* name);
    void Unregister(const char* name);
    void SetProtected();

    // Handlers.
    bool HasScript(const char* name);
    bool HasScriptBinding(uint8_t index, uint8_t* binding);
    bool HasScriptAny(uint8_t index);
    Script* GetScripts(Script* scripts, uint8_t index);
    int32_t SetScriptRef(uint8_t index, uint8_t binding, int32_t ref, const char* taint);
    void LoadScriptXML(lua_State* L, XMLSCRIPTHANDLER* handler, CStatus* status, int32_t hook);
    void LoadXML(XMLOBJECT* node, CStatus* status, const char* inherits);

    // The Lua methods every scripted object shares (each returns its result count).
    int32_t GetScript(lua_State* L);
    int32_t SetScript(lua_State* L);
    int32_t HookScript(lua_State* L);
    bool RegisterEvent(const char* name);
    bool UnregisterEvent(const char* name);
};

// The handler storage: one vector of scripts per object, in a map keyed by the object.
FrameScript_Object::Script* FrameScript_FindScripts(const FrameScript_Object* object, uint64_t* count);
void FrameScript_ClearScripts(const FrameScript_Object* object);

// The script error an object's method reports: "<name>:<method>(): <message>" into the
// pending error buffer, which the method's Lua wrapper raises.
void FrameScript_ObjectError(FrameScript_Object* object, const char* method, const char* format, ...);
// A restricted-action failure, with the current taint appended.
void FrameScript_ActionError(const char* format, ...);
// Pushes the pending error (named after the object and method) and clears it; false if none.
bool FrameScript_PushObjectError(lua_State* L, FrameScript_Object* object, const char* method);

// Pushes a value with the FrameScript state (each returns 1).
int32_t FrameScript_PushInteger(int32_t value);
int32_t FrameScript_PushFloat(float value);
int32_t FrameScript_PushNumber(double value);
int32_t FrameScript_PushString(const char* value);
int32_t FrameScript_PushRef(int32_t ref);
int32_t FrameScript_PushBoolean(bool value);

// Calls the function at `function` with `count` arguments from `first` under `taint`, then
// leaves its results on the stack; an error is re-raised after the taint is restored.
int32_t FrameScript_CallWithTaint(lua_State* L, const char* taint, int32_t function, int32_t first, int32_t count);
// Wraps every function of the table at `index` (and of the tables it holds, ten deep) so it
// runs under `taint`.
void FrameScript_WrapFunctionWithTaint(lua_State* L, int32_t index, const char* taint);

// Turns each named global mixin table (a list separated by spaces or commas) into a read-only
// proxy: the global becomes an empty table whose metatable reads through to the original and
// is locked with the secure-mixin tag. A name that is not a table is reported against `owner`.
void FrameScript_CreateSecureMixins(lua_State* L, const char* mixins, const char* owner, CStatus* status);

#endif
