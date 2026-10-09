#include "framescript/FrameScript.hpp"

#include <cstring>
#include <new>
#include <vector>

#include <windows.h>

#include "blz/Random.hpp"
#include "framescript/LuaMemPool.hpp"
#include "os/Timer.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
#include "lualib.h"
}

// The global functions FrameScript adds (FrameScriptLibrary.cpp, 0x1426cda00) and its own
// math.random (0x1426cd9e0).
extern const luaL_Reg g_frameScriptGlobals[];
extern const luaL_Reg g_frameScriptMath[];

namespace {

lua_State* s_L;                     // 0x142e99e18
LMEMPOOL* s_pool;                   // 0x142e99e10
DWORD s_threadId;                   // 0x142e99e20
int32_t s_initCount;                // 0x142e99f30
bool s_profiling;                   // 0x142e99dd5
bool s_inErrorHandler;              // 0x142e99dd6
int32_t s_errorCount;               // 0x142e99f34
FRAMESCRIPTSTATEHOOK s_stateHook;   // 0x142e99e28

// Registry references: the built-in error handler, whether a script replaced it, the shared
// environment table, and the table FrameScript_Execute's flag 2 reads.
int32_t s_errorHandlerRef;          // 0x142cf54ec
int32_t s_userErrorHandler = -1;    // 0x142cf54fc
int32_t s_environmentRef = -1;      // 0x142cf5500
int32_t s_objectTableRef;           // 0x142cf5504
int32_t s_currentObjectRef;         // 0x142e9a078

// The keystream math.random draws from, keyed from the system's entropy (0x142e9a0a0).
ARC4STATE s_random;

// 0x142e99f60: eight 16-byte slots filled with 0x09 on each start; their reader is not yet
// identified.
uint8_t s_tabSlots[8][16];

// Script profiling: the tick scale, and a 1000-entry call stack of 0x20 bytes each plus
// bookkeeping (0x7d20 bytes) when profiling.
struct PROFILEFRAME {
    void* m_function;
    int32_t m_depth;
    int32_t m_pad;
    uint64_t m_start;
    uint64_t m_child;
};
struct PROFILESTACK {
    PROFILEFRAME m_frames[1000];    // 0x0000
    void* m_unk7d00;                // 0x7d00
    void* m_unk7d08;                // 0x7d08
    int32_t m_unk7d10;              // 0x7d10
    int32_t m_depth;                // 0x7d14
    void* m_top;                    // 0x7d18
};
static_assert(sizeof(PROFILESTACK) == 0x7d20, "PROFILESTACK is 0x7d20 bytes");
uint64_t s_profileTicks;            // 0x142e99f40
double s_profileTickMs;             // 0x142e99f48
bool s_profileEnabled;              // 0x142e99f38
PROFILESTACK* s_profileStack;       // 0x142e99f50

// The Lua warnings the UI shows (0x1447aefc0).
std::vector<LUAWARNING> s_warnings;

// The source-file name registry script positions are resolved through (0x142e9a238). This
// build reads it but nothing ever sets it, so every position is "UnknownFile".
class SCRIPTFILENAMES {
    public:
    virtual ~SCRIPTFILENAMES() {}
    virtual void Register(uint32_t* id, const char* name) = 0;
    virtual void GetName(uint32_t id, const char** name) = 0;
};
SCRIPTFILENAMES* s_fileNames;

const char s_compatLua[] =
    "-------------------------------------------------------------------\n"
    "-- Table library\n"
    "local tab = table\n"
    "foreach = tab.foreach\n"
    "foreachi = tab.foreachi\n"
    "getn = tab.getn\n"
    "tinsert = tab.insert\n"
    "tremove = tab.remove\n"
    "sort = tab.sort\n"
    "wipe = tab.wipe\n"
    "\n"
    "-------------------------------------------------------------------\n"
    "-- math library\n"
    "local math = math\n"
    "abs = math.abs\n"
    "acos = function (x) return math.deg(math.acos(x)) end\n"
    "asin = function (x) return math.deg(math.asin(x)) end\n"
    "atan = function (x) return math.deg(math.atan(x)) end\n"
    "atan2 = function (x,y) return math.deg(math.atan2(x,y)) end\n"
    "ceil = math.ceil\n"
    "cos = function (x) return math.cos(math.rad(x)) end\n"
    "deg = math.deg\n"
    "exp = math.exp\n"
    "floor = math.floor\n"
    "frexp = math.frexp\n"
    "ldexp = math.ldexp\n"
    "log = math.log\n"
    "log10 = math.log10\n"
    "max = math.max\n"
    "min = math.min\n"
    "mod = math.fmod\n"
    "PI = math.pi\n"
    "--??? pow = math.pow\n"
    "rad = math.rad\n"
    "random = math.random\n"
    "sin = function (x) return math.sin(math.rad(x)) end\n"
    "sqrt = math.sqrt\n"
    "tan = function (x) return math.tan(math.rad(x)) end\n"
    "\n"
    "-------------------------------------------------------------------\n"
    "-- string library\n"
    "local str = string\n"
    "strbyte = str.byte\n"
    "strchar = str.char\n"
    "strfind = str.find\n"
    "format = str.format\n"
    "gmatch = str.gmatch\n"
    "gsub = str.gsub\n"
    "strlen = str.len\n"
    "strlower = str.lower\n"
    "strmatch = str.match\n"
    "strrep = str.rep\n"
    "strrev = str.reverse\n"
    "strsub = str.sub\n"
    "strupper = str.upper\n"
    "-------------------------------------------------------------------\n"
    "-- Add custom string functions to the string table\n"
    "str.trim = strtrim\n"
    "str.split = strsplit\n"
    "str.join = strjoin\n";

// The name of the object whose method is running `level` frames up, for error messages:
// a method's self is a table whose [0] holds the C++ object.
// ref: FUN_14063ca80
const char* GetMethodObjectName(lua_State* L, int32_t level) {
    lua_Debug ar;
    if (!lua_getstack(L, level, &ar)) {
        return nullptr;
    }
    lua_getinfo(L, "n", &ar);
    if (*ar.name != '*' && strcmp(ar.namewhat, "method") != 0) {
        return nullptr;
    }
    const char* name = nullptr;
    if (lua_getlocal(L, &ar, 1)) {
        if (lua_type(L, -1) == LUA_TTABLE) {
            lua_rawgeti(L, -1, 0);
            if (lua_type(L, -1) == LUA_TLIGHTUSERDATA && lua_touserdata(L, -1)) {
                // The object's GetName (slot 1), else its debug name (slot 12): CScriptObject is
                // not ported yet.
                FROST_UNIMPLEMENTED();
            }
            lua_settop(L, -2);
        }
        lua_settop(L, -2);
    }
    return name;
}

// The default error handler: an error inside an object's method has its "*:" stand-in
// replaced by the object's name, and a handler installed with seterrorhandler is called
// with the message.
// ref: FUN_14063cdd0
int ErrorHandler(lua_State* L) {
    s_errorCount++;
    if (!lua_isstring(L, -1)) {
        lua_pushlstring(L, "UNKNOWN ERROR", 13);
        lua_insert(L, -1);
    }
    const char* message = lua_tolstring(L, -1, nullptr);
    // FUN_140255c60, a debug print compiled out of this build ("FrameScript_HandleError: %s\n")
    const char* star = strstr(message, "*:");
    const char* object = GetMethodObjectName(L, 1);
    if (star && object) {
        lua_pushlstring(L, message, star - message);
        lua_pushlstring(L, object, strlen(object));
        const char* rest = star + 1;
        if (rest == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, rest, strlen(rest));
        }
        lua_concat(L, 3);
        lua_replace(L, -2);
    }
    if (s_userErrorHandler != -1) {
        s_inErrorHandler = true;
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_userErrorHandler);
        lua_insert(L, -2);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_errorHandlerRef);
        lua_insert(L, -3);
        int status = lua_pcall(L, 1, 1, -3);
        lua_settop(L, -2 - (status != 0 ? 1 : 0));
        s_inErrorHandler = false;
    }
    return 1;
}

// The pool behind every Lua allocation.
// ref: FUN_14063ec00
void* LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    return LuaMemPoolRealloc(static_cast<LMEMPOOL*>(ud), ptr, osize, nsize);
}

// ref: FUN_14065df00
int LuaPanic(lua_State* L) {
    const char* message = lua_tolstring(L, -1, nullptr);
    ErrorDisplayFormatted(0x85100086, nullptr, 0, "LUA PANIC: unprotected error in call to Lua API (%s)", message);
    return 0;
}

// Script profiling's clock and call stack.
// ref: FUN_14063edd0
void ProfileInitialize(bool profiling) {
    s_profileTicks = 0;
    s_profileTickMs = 1000.0 / static_cast<double>(OsTimerGetFrequency());
    s_profileEnabled = profiling;
    if (!profiling) {
        return;
    }
    static const char file[] = "D:\\BuildServer\\A\\work\\package_cache\\blz\\3.11.13\\include\\blz\\mem_unique_ptr.h";
    auto stack = static_cast<PROFILESTACK*>(SMemAlloc(sizeof(PROFILESTACK), 0xB, file, SStrHashFnv1a(file), 0x20E, 0));
    if (stack) {
        memset(stack, 0, 32000);
        stack->m_depth = 0;
        for (auto& frame : stack->m_frames) {
            frame.m_function = nullptr;
            frame.m_depth = 0;
            frame.m_start = 0;
            frame.m_child = 0;
        }
        stack->m_unk7d00 = nullptr;
        stack->m_unk7d08 = nullptr;
        stack->m_unk7d10 = 0;
        stack->m_top = nullptr;
    }
    PROFILESTACK* old = s_profileStack;
    s_profileStack = stack;
    if (old) {
        ::operator delete(old, sizeof(PROFILESTACK));
    }
}

// ref: FUN_14063ec10
void ProfileShutdown() {
    PROFILESTACK* stack = s_profileStack;
    s_profileStack = nullptr;
    if (stack) {
        ::operator delete(stack, sizeof(PROFILESTACK));
    }
}

// ref: FUN_14063f1a0
void CreateObjectTable() {
    lua_State* L = FrameScript_GetContext();
    lua_createtable(L, 0, 0);
    s_objectTableRef = luaL_ref(L, LUA_REGISTRYINDEX);
    memset(s_tabSlots, 0x09, sizeof(s_tabSlots));
}

// The libraries, FrameScript's globals, and compat.lua.
// ref: FUN_14065d630
void RegisterLibraries(lua_State* L) {
    ENTROPYSOURCE entropy;
    uint8_t key[16];
    EntropySourceInit(&entropy);
    EntropySourceFill(&entropy, key, sizeof(key));
    EntropySourceRelease(&entropy);
    ARC4Init(&s_random, key, sizeof(key));
    ARC4Skip(&s_random, 0x400);

    lua_atpanic(L, LuaPanic);
    luaopen_base(L);
    lua_settop(L, -3);
    luaopen_string(L);
    lua_settop(L, -2);
    luaopen_table(L);
    lua_settop(L, -2);
    luaopen_math(L);
    lua_settop(L, -2);
    luaopen_bit(L);
    lua_settop(L, -2);
    luaL_register(L, "_G", g_frameScriptGlobals);
    lua_settop(L, -2);
    luaL_register(L, "math", g_frameScriptMath);
    lua_settop(L, -2);
    FrameScript_Execute(s_compatLua, "compat.lua", nullptr);
}

// Drops every registered event listener (FrameScriptEvents).
// ref: FUN_140647840
void ClearEventListeners() {
    FROST_UNIMPLEMENTED();
}

}  // namespace

// ref: FUN_14060b970
void CScriptStatus::SetFile(const uint32_t* file) {
    this->m_location.m_file = *file;
    this->m_location.m_line = 1;
}

// ref: FUN_140607380
void* CScriptStatus::GetLocation() {
    return &this->m_location;
}

// Records messages only when enabled; with a known position they read "file:line message".
// ref: FUN_1406673a0
void CScriptStatus::AddV(int32_t severity, const char* format, va_list args) {
    if (!this->m_warn) {
        return;
    }
    if (this->m_location.m_file == 0 || this->m_location.m_line == 0) {
        CStatus::AddV(severity, format, args);
        return;
    }
    // FUN_140660750: the position as "file:line"
    const char* name = nullptr;
    if (s_fileNames) {
        s_fileNames->GetName(this->m_location.m_file, &name);
    } else {
        name = "UnknownFile";
    }
    if (!name) {
        name = "UnknownFile";
    }
    blz::inline_string<0x30C> where;
    blz::string_format(where.header(), "%s:%d", name, this->m_location.m_line);
    blz::inline_string<0x200> prefixed;
    blz::string_format(prefixed.header(), "%s %s", where.m_data, format);
    CStatus::AddV(severity, prefixed.m_data, args);
}

// ref: FUN_1406672f0
CScriptStatus::~CScriptStatus() {
    if (this->m_warn) {
        for (auto entry = this->m_list.m_next; entry != reinterpret_cast<STATUSENTRY*>(&this->m_list); entry = entry->m_next) {
            if (entry->m_severity > 0) {
                LuaWarningAdd(0, entry->m_text.m_data);
            }
        }
    }
}

// ref: FUN_140604170
CLoggedStatus::~CLoggedStatus() {
    if (this->m_log) {
        for (auto entry = this->m_list.m_next; entry != reinterpret_cast<STATUSENTRY*>(&this->m_list); entry = entry->m_next) {
            SLogWrite(this->m_log, "%s", entry->m_text.m_data);
        }
        SLogClose(this->m_log);
    }
}

// ref: FUN_1424d3b40
void LuaWarningAdd(int32_t type, const char* text) {
    if (s_warnings.size() >= 100) {
        return;
    }
    s_warnings.emplace_back();
    LUAWARNING& warning = s_warnings.back();
    warning.m_text.assign(text);
    warning.m_type = type;
}

void FrameScript_SetStateHook(FRAMESCRIPTSTATEHOOK hook) {
    s_stateHook = hook;
}

// ref: FUN_14063c960
lua_State* FrameScript_GetContext() {
    return s_L;
}

// ref: FUN_14063d070
bool FrameScript_IsInitialized() {
    return s_L != nullptr;
}

// ref: FUN_14063d080
void FrameScript_Initialize(bool profiling) {
    s_threadId = GetCurrentThreadId();
    s_pool = LuaMemPoolCreate();
    s_L = lua_newstate(LuaAlloc, s_pool, profiling);
    s_initCount++;
    s_profiling = profiling;
    RegisterLibraries(s_L);
    ProfileInitialize(profiling);
    CreateObjectTable();
    s_userErrorHandler = -1;
    lua_pushcclosure(s_L, ErrorHandler, 0);
    s_errorHandlerRef = luaL_ref(s_L, LUA_REGISTRYINDEX);
    s_environmentRef = -1;
    lua_gc(s_L, LUA_GCSETSTEPMUL, 110);
    if (s_stateHook) {
        s_stateHook(s_L);
    }
    lua_setscripttimeout(200, 19000);
}

// ref: FUN_14063c8e0
void FrameScript_Reinitialize(bool profiling) {
    if (!s_L) {
        return;
    }
    lua_close(s_L);
    s_L = nullptr;
    s_profiling = false;
    LuaMemPoolDestroy(s_pool);
    s_pool = nullptr;
    ClearEventListeners();
    ProfileShutdown();
    if (s_stateHook) {
        s_stateHook(nullptr);
    }
    FrameScript_Initialize(profiling);
}

// ref: FUN_140638d90
bool FrameScript_ExecuteBuffer(const char* source, uint64_t size, const char* name, CStatus* status,
    const char* arg, uint32_t flags) {
    lua_State* L = s_L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_errorHandlerRef);
    if (luaL_loadbuffer(L, source, size, name) != 0) {
        const char* message = lua_tolstring(L, -1, nullptr);
        status->Add(2, "%s", message);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            lua_settop(L, -2);
        }
        return false;
    }
    int32_t nargs = 0;
    if (flags != 0) {
        nargs = 3;
        lua_checkstack(L, 3);
        if ((flags & 1) == 0 || arg == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, arg, strlen(arg));
        }
        if ((flags & 2) == 0 || s_currentObjectRef == 0) {
            lua_pushnil(L);
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s_currentObjectRef);
        }
        if ((flags & 4) == 0) {
            lua_pushnil(L);
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, s_environmentRef);
        }
    }
    // FUN_14063e310: lua_pcall behind an anti-tamper return-address check
    if (lua_pcall(L, nargs, 0, -2 - nargs) == 0) {
        lua_settop(L, -2);
        return true;
    }
    const char* message = lua_tolstring(L, -1, nullptr);
    status->Add(2, "%s", message);
    lua_settop(L, -3);
    return false;
}

// ref: FUN_14063c090
void FrameScript_Execute(const char* source, const char* name, const char* taint) {
    lua_State* L = s_L;
    const char* previous = lua_taint;
    lua_pushjmptaint(L, lua_taint, lua_taintTracking);
    lua_taintTracking++;
    if (lua_taintTracking && !lua_taintSuspended) {
        lua_taint = taint;
        if (taint && !lua_firstTaint) {
            lua_firstTaint = taint;
            if (lua_captureFirstTaint) {
                luaT_capturefirsttaint(L);
            }
        }
    }
    CScriptStatus status;
    FrameScript_ExecuteBuffer(source, strlen(source), name, &status, nullptr, 0);
    if (lua_taintTracking && !lua_taintSuspended) {
        lua_taint = previous;
        if (previous && !lua_firstTaint) {
            lua_firstTaint = previous;
            if (lua_captureFirstTaint) {
                luaT_capturefirsttaint(L);
            }
        }
    }
    lua_taintTracking--;
    lua_popjmptaint(L);
    if (lua_taintTracking < 1) {
        lua_taintTracking = 0;
        lua_firstTaint = nullptr;
    }
}

// ref: FUN_14063e170
void FrameScript_ReleaseFunction(int32_t ref) {
    luaL_unref(s_L, LUA_REGISTRYINDEX, ref);
}

// ref: FUN_14063cbc0
int32_t FrameScript_GetErrorHandlerRef() {
    return s_errorHandlerRef;
}

// ref: FUN_14063cbd0
int32_t FrameScript_GetUserErrorHandler() {
    return s_userErrorHandler;
}

// ref: FUN_14063e190
void FrameScript_SetUserErrorHandler(int32_t ref) {
    s_userErrorHandler = ref;
}

// ref: FUN_14063ed90
double FrameScript_GetProfileTickMs() {
    return s_profileTickMs;
}

ARC4STATE* FrameScript_GetRandom() {
    return &s_random;
}
