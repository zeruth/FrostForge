#ifndef FRAMESCRIPT_FRAMESCRIPT_HPP
#define FRAMESCRIPT_FRAMESCRIPT_HPP

// FrameScript: the client's Lua host. One Lua state for the whole UI (glue or game), its
// allocator, the error handler scripts can replace, and running chunks under a taint.

#include "base/Status.hpp"
#include "blz/String.hpp"
#include "storm/Log.hpp"
#include <cstdint>

struct lua_State;

// A position in a loaded file: the file's id with the source tracker and a line.
struct SCRIPTLOCATION {
    uint32_t m_file;
    uint16_t m_line;
};

// A CStatus that copies its messages to a log file when destroyed (0x30, vtable 0x1426c9248).
class CLoggedStatus : public CStatus {
    public:
    HSLOG m_log = nullptr;          // 0x28

    ~CLoggedStatus() override;
};

// What script loading reports into: each message is prefixed with the current file and line
// when one is set, and on destruction errors can be echoed as Lua warnings (0x40, vtable
// 0x1426c92a8).
class CScriptStatus : public CLoggedStatus {
    public:
    SCRIPTLOCATION m_location = {}; // 0x30
    bool m_warn = false;            // 0x38 echo messages of severity > 0 as Lua warnings

    CScriptStatus() = default;
    // Logs to `logName` (appending when asked), positioned at line 1 of `file`, with
    // warnings echoed.
    CScriptStatus(const char* logName, bool append, const char* file);
    ~CScriptStatus() override;
    // Sets the file messages are prefixed with, at line 1.
    void SetFile(const uint32_t* file) override;
    void* GetLocation() override;
    void AddV(int32_t severity, const char* format, va_list args) override;
};

// A queued Lua warning for the UI to show (0x30).
struct LUAWARNING {
    blz::string m_text;
    int32_t m_type;
};

// Queues a warning (at most 100 are kept).
void LuaWarningAdd(int32_t type, const char* text);

// Called with the new Lua state after FrameScript creates one, and with null when it goes.
typedef void (*FRAMESCRIPTSTATEHOOK)(lua_State* L);
void FrameScript_SetStateHook(FRAMESCRIPTSTATEHOOK hook);

void FrameScript_Initialize(bool profiling);
// Tears the state down and builds a fresh one (a UI reload).
void FrameScript_Reinitialize(bool profiling);
lua_State* FrameScript_GetContext();
bool FrameScript_IsInitialized();

// Compiles `size` bytes of `source` as `name` and runs it under the error handler. `flags`
// selects up to three arguments: 1 the string `arg`, 2 the current object's table, 4 the
// shared environment table. Errors are reported into `status`.
bool FrameScript_ExecuteBuffer(const char* source, uint64_t size, const char* name, CStatus* status,
    const char* arg, uint32_t flags);
// Loads and runs a Lua file (as "@path"), positioned at its first line.
bool FrameScript_ExecuteFile(const char* path, const char* arg, CScriptStatus* status, uint32_t flags, bool localFiles);
// Runs a NUL-terminated chunk with `taint` current.
void FrameScript_Execute(const char* source, const char* name, const char* taint);

struct ARC4STATE;

// The registry reference of the object whose script is running (0 for none), which flag 2
// of FrameScript_ExecuteBuffer passes; and the flag FrameXML sets when a scoped modifier
// captured it.
int32_t FrameScript_GetCurrentObject();
void FrameScript_SetCurrentObject(int32_t ref);
void FrameScript_SetCurrentObjectScoped(bool scoped);
// The position of `line` in a named file (the file id is always 0: no registry is set).
SCRIPTLOCATION* ScriptLocationFromFile(SCRIPTLOCATION* location, const char* file, int32_t line);
// The position as "file:line" ("UnknownFile" without a file-name registry).
void ScriptLocationToString(const SCRIPTLOCATION* location, blz::string_header* text);
class FrameScript_Object;
// Runs a handler: `ref` with `object` as self and the `count` values on top of the stack as
// arguments, under `taint` and the error handler; the arguments are popped.
void FrameScript_RunScript(int32_t ref, FrameScript_Object* object, int32_t count, const char* taint);
// Compiles `body` inside `wrapper` (a format with one %s) as chunk `name` and runs it; the
// function that returns is referenced in the registry. -1 on an error, reported to `status`.
int32_t FrameScript_CompileFunction(const char* name, const char* wrapper, const char* body, CStatus* status);
// Drops a function's registry reference.
void FrameScript_ReleaseFunction(int32_t ref);
// The registry reference of FrameScript's own error handler.
int32_t FrameScript_GetErrorHandlerRef();
// The registry reference of the handler a script set with seterrorhandler, or -1.
int32_t FrameScript_GetUserErrorHandler();
void FrameScript_SetUserErrorHandler(int32_t ref);
// Milliseconds per profile tick.
double FrameScript_GetProfileTickMs();
// The keystream math.random draws from.
ARC4STATE* FrameScript_GetRandom();
// A localized string from the Lua string tables (GlobalStrings): the plural form for `count`
// and the _FEMALE form for gender 3 are tried first; "" when there is none. `scoped` looks in
// the running object's table first while FrameXML's scoped state asks for it.
const char* FrameScript_GetText(const char* name, int32_t count, int32_t gender, bool scoped = false);
// The global string named `name`, or the running object's field while scoped (FUN_14063cc10).
bool FrameScript_GetGlobalString(const char* name, const char** value, bool scoped);
// The plural form of `count` under a locale's plural rule: 0 singular, 1 plural, 2 the
// Slavic genitive plural (FUN_141650790).
int32_t FrameScript_GetPluralForm(uint32_t rule, int32_t count);
struct lua_State;
struct SCRIPTFUNCTION;
typedef int (*lua_CFunction)(lua_State* L);
// A global function (FUN_14063daa0).
void FrameScript_RegisterFunction(const char* name, lua_CFunction function);
// `count` functions into the global table `space`, made when missing (FUN_14063d2b0).
void FrameScript_RegisterNamespace(const SCRIPTFUNCTION* functions, uint32_t count, const char* space);
// A global number (FUN_14063e1b0).
void FrameScript_SetGlobalNumber(const char* name, int32_t value);
// string.format over the arguments from `index` (the format) on, into a buffer of `size`
// bytes; %d, %i and %F numbers get the locale's decimal mark.
char* FrameScript_FormatString(lua_State* L, int32_t index, char* buffer, uint32_t size);

#endif
