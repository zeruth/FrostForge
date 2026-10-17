#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptEvents.hpp"
#include "framescript/FrameScriptObject.hpp"

#include <cstring>
#include <new>
#include <vector>

#include <windows.h>

#include "blz/Random.hpp"
#include "cas/CAS.hpp"
#include "framescript/LuaMemPool.hpp"
#include "framescript/ScriptAPI.hpp"
#include "os/Timer.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/FrostLog.hpp"
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
bool s_currentObjectScoped;         // 0x142e9a07c set when a scoped modifier captured the object

// The keystream math.random draws from, keyed from the system's entropy (0x142e9a0a0).
ARC4STATE s_random;

// 0x142e99f60: eight 16-byte slots filled with 0x09 on each start; their reader is not yet
// identified.
uint8_t s_tabSlots[8][16];

// Script profiling: the tick scale, and a 1000-entry call stack of 0x20 bytes each plus
// bookkeeping (0x7d20 bytes) when profiling.
// One frame's event time (as FUN_14063eae0 fills it): the total, the handler calls, and the
// slowest handler's object name and time.
struct PROFILEFRAME {
    double m_total;                 // 0x00 ms
    int32_t m_calls;                // 0x08
    int32_t m_pad;
    const char* m_slowest;          // 0x10 the slowest object's GetName()
    double m_max;                   // 0x18 ms
};
struct PROFILESTACK {
    PROFILEFRAME m_frames[1000];    // 0x0000 a ring, by event scope
    uint32_t m_frame;               // 0x7d00 the newest scope's frame
    uint32_t m_frameCount;          // 0x7d04 frames used, up to 1000
    double m_totalMs;               // 0x7d08
    int32_t m_calls;                // 0x7d10
    int32_t m_depth;                // 0x7d14
    PROFILEEVENTSCOPE* m_top;       // 0x7d18 the innermost event scope
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
    lua_getinfo(L, "Sln", &ar);
    // a chunk named "*..." is an object's script; otherwise only a method call qualifies
    if (*ar.source != '*' && strcmp(ar.namewhat, "method") != 0) {
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
    // FUN_140255c60, a debug print compiled out of this build ("FrameScript_HandleError: %s\n").
    // FROST: the error and its Lua stack go to FrostLite.log instead.
    FrostLog("lua", "%s", message);
    lua_Debug frame;
    for (int level = 1; level < 24 && lua_getstack(L, level, &frame); level++) {
        lua_getinfo(L, "Sln", &frame);
        FrostLog("lua", "    at %s:%d %s", frame.short_src, frame.currentline,
            frame.name != nullptr ? frame.name : (frame.what != nullptr ? frame.what : "?"));
    }
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
            frame.m_total = 0.0;
            frame.m_calls = 0;
            frame.m_slowest = nullptr;
            frame.m_max = 0.0;
        }
        stack->m_frame = 0;
        stack->m_frameCount = 0;
        stack->m_totalMs = 0.0;
        stack->m_calls = 0;
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


}  // namespace

// An event's profiling scope (FUN_14063ea00): when profiling, it becomes the innermost scope and
// takes the next frame of the ring, clearing it once the ring is full.
// ref: FUN_14063ea00
PROFILEEVENTSCOPE* ProfileEventEnter(PROFILEEVENTSCOPE* scope, uint64_t id) {
    scope->m_id = id;
    PROFILESTACK* stack = s_profileStack;
    if (stack == nullptr) {
        scope->m_previous = nullptr;
        return scope;
    }
    uint32_t count = stack->m_frameCount;
    PROFILEEVENTSCOPE* previous = stack->m_top;
    stack->m_top = scope;
    if (999 < count) {
        uint32_t frame = stack->m_frame + 1;
        stack->m_frame = frame;
        stack->m_frames[frame % 1000] = PROFILEFRAME{};
        scope->m_frame = stack->m_frame;
        scope->m_previous = previous;
        return scope;
    }
    stack->m_frame = count;
    stack->m_frameCount = count + 1;
    scope->m_frame = stack->m_frame;
    scope->m_previous = previous;
    return scope;
}

// ref: FUN_14063ebd0
void ProfileEventLeave(PROFILEEVENTSCOPE* scope) {
    if (s_profileStack != nullptr && scope == s_profileStack->m_top) {
        s_profileStack->m_top = scope->m_previous;
    }
}

// A handler's profiling scope within an event (FUN_14063e980): its start, the event and the
// object it runs for.
// ref: FUN_14063e980
PROFILEHANDLERSCOPE* ProfileHandlerEnter(PROFILEHANDLERSCOPE* scope, uint64_t id, FrameScript_Object* object) {
    scope->m_start = s_profileStack == nullptr ? 0 : OsTimerGetTicks();
    scope->m_id = id;
    scope->m_object = object;
    if (s_profileStack != nullptr && s_profileStack->m_top != nullptr) {
        scope->m_event = s_profileStack->m_top;
    }
    return scope;
}

// The handler's time into the totals and into its event's frame, keeping the slowest handler
// (FUN_14063eae0).
// ref: FUN_14063eae0
void ProfileHandlerLeave(PROFILEHANDLERSCOPE* scope) {
    PROFILESTACK* stack = s_profileStack;
    if (stack == nullptr || stack->m_top == nullptr) {
        return;
    }
    uint64_t start = scope->m_start;
    uint64_t now = OsTimerGetTicks();
    stack->m_calls++;
    s_profileTicks += now - start;
    double ms = static_cast<double>(now - start) * s_profileTickMs;
    stack->m_totalMs = ms + stack->m_totalMs;
    uint32_t frame = scope->m_event->m_frame;
    if (frame <= stack->m_frame && (stack->m_frameCount < 1000 || stack->m_frame - 1000 <= frame)) {
        PROFILEFRAME* entry = &stack->m_frames[frame % 1000];
        if (entry->m_max < ms) {
            entry->m_slowest = scope->m_object->GetName();
            entry->m_max = ms;
        }
        entry->m_calls++;
        entry->m_total = ms + entry->m_total;
    }
}

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
    // FROST: warnings and errors are copied to FrostLite.log as they happen.
    if (severity > 0) {
        char text[2048];
        va_list copy;
        va_copy(copy, args);
        vsnprintf(text, sizeof(text), format, copy);
        va_end(copy);
        if (this->m_location.m_file != 0 && this->m_location.m_line != 0) {
            blz::inline_string<0x30C> where;
            ScriptLocationToString(&this->m_location, where.header());
            FrostLog("script", "%s %s", where.m_data, text);
        } else {
            FrostLog("script", "%s", text);
        }
    }
    if (this->m_location.m_file == 0 || this->m_location.m_line == 0) {
        CStatus::AddV(severity, format, args);
        return;
    }
    blz::inline_string<0x30C> where;
    ScriptLocationToString(&this->m_location, where.header());
    blz::inline_string<0x200> prefixed;
    blz::string_format(prefixed.header(), "%s %s", where.m_data, format);
    CStatus::AddV(severity, prefixed.m_data, args);
}

// A system message: formatted, then dropped (the output is compiled out of this client).
// ref: FUN_140667590
static void SysMessageOutput(int32_t type, const char* format, ...) {
    (void)type;
    // FUN_141283d60
    char text[0x100];
    va_list args;
    va_start(args, format);
    SStrVPrintf(text, sizeof(text), format, args);
    va_end(args);
}

// ref: FUN_140667240
CScriptStatus::CScriptStatus(const char* logName, bool append, const char* file) {
    if (logName != nullptr && *logName != '\0') {
        if (!SLogCreate(logName, uint32_t(append) << 2, &this->m_log)) {
            SysMessageOutput(1, "Cannot create WOWClient log file \"%s\"!", logName);
        }
    }
    ScriptLocationFromFile(&this->m_location, file, 1);
    this->m_warn = true;
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
    FrameScriptEvents::Clear();
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

// ref: FUN_14063a2a0
bool FrameScript_ExecuteFile(const char* path, const char* arg, CScriptStatus* status, uint32_t flags, bool localFiles) {
    char collapsed[0x104];
    if (strstr(path, "..") != nullptr) {
        // "dir/../" pairs are collapsed out of the path
        char* dest = collapsed;
        uint64_t left = 0x103;
        while (true) {
            char c = *path++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *dest = '\0';
                break;
            }
        }
        char* up = strstr(collapsed, "..");
        while (up != nullptr && (up[-1] == '\\' || up[-1] == '/') && (up[2] == '\\' || up[2] == '/')) {
            char* start = up - 2;
            while (collapsed <= start && *start != '\\' && *start != '/') {
                start--;
            }
            const char* from = up + 3;
            char* to = start + 1;
            uint64_t room = collapsed + sizeof(collapsed) - to;
            if (room != 0) {
                while (true) {
                    char c = *from++;
                    *to = c;
                    if (c == '\0') {
                        break;
                    }
                    to++;
                    if (--room == 0) {
                        *to = '\0';
                        break;
                    }
                }
            }
            up = strstr(collapsed, "..");
        }
        path = collapsed;
    }
    char name[0x105];
    SStrPrintf(name, sizeof(name), "@%s", path);
    uint8_t* data;
    uint32_t size;
    if (!cas::CASFileLoadByName(path, &data, &size, localFiles, 0x20)) {
        status->Add(2, "Error loading %s", path);
        return false;
    }
    if (size == 0) {
        status->Add(2, "File %s is empty.", path);
        return false;
    }
    SCRIPTLOCATION saved = status->m_location;
    SCRIPTLOCATION location;
    status->m_location = *ScriptLocationFromFile(&location, path, 1);
    bool result = FrameScript_ExecuteBuffer(reinterpret_cast<const char*>(data), size, name, status, arg, flags);
    cas::CASFileFree(data);
    status->m_location = saved;
    return result;
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

// ref: FUN_1406466c0
int32_t FrameScript_GetCurrentObject() {
    return s_currentObjectRef;
}

// ref: FUN_140646d10
void FrameScript_SetCurrentObject(int32_t ref) {
    s_currentObjectRef = ref;
}

// ref: FUN_140646d20
void FrameScript_SetCurrentObjectScoped(bool scoped) {
    s_currentObjectScoped = scoped;
}

// ref: FUN_1406606d0
SCRIPTLOCATION* ScriptLocationFromFile(SCRIPTLOCATION* location, const char* file, int32_t line) {
    location->m_file = 0;
    location->m_line = 0;
    uint32_t id;
    // The file-name registry (0x142e9a238) is never installed in this client.
    id = 0;
    location->m_file = id;
    location->m_line = static_cast<uint16_t>(line < 1 ? 0 : line);
    (void)file;
    return location;
}

// ref: FUN_140660750
void ScriptLocationToString(const SCRIPTLOCATION* location, blz::string_header* text) {
    const char* name = nullptr;
    if (s_fileNames == nullptr) {
        name = "UnknownFile";
    } else {
        s_fileNames->GetName(location->m_file, &name);
    }
    blz::string_format(text, "%s:%d", name != nullptr ? name : "UnknownFile", location->m_line);
}

// Runs a handler: the function `ref` with `object`'s table as self and the `count` values on
// top of the stack as arguments, under `taint` and the error handler. The arguments are
// popped.
// ref: FUN_14063b820
void FrameScript_RunScript(int32_t ref, FrameScript_Object* object, int32_t count, const char* taint) {
    lua_State* L = s_L;
    int32_t top = lua_gettop(L);
    const char* previous = lua_taint;
    lua_pushjmptaint(L, lua_taint, lua_taintTracking);
    lua_taintTracking++;
    if (lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, taint);
    }
    lua_checkstack(L, count + 3);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_errorHandlerRef);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    int32_t args = count;
    if (object != nullptr) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, object->GetLuaRef());
        args = count + 1;
    }
    for (int32_t i = 0; i < count; i++) {
        lua_pushvalue(L, i + (1 - count) + top);
    }
    // FUN_14063e310: lua_pcall behind an anti-tamper return-address check
    if (lua_pcall(L, args, 0, -2 - args) != 0) {
        lua_settop(L, -2);
    }
    lua_settop(L, -2);
    if (lua_taintTracking && !lua_taintSuspended) {
        luaT_settaint(L, previous);
    }
    lua_taintTracking--;
    lua_popjmptaint(L);
    if (lua_taintTracking < 1) {
        lua_taintTracking = 0;
        lua_firstTaint = nullptr;
    }
    lua_settop(L, -1 - count);
}

// Compiles `body` inside `wrapper` (a format with one %s) as `name` and runs it under the
// error handler; the function it returns is referenced in the registry. -1 on an error, which
// is reported into `status`.
// ref: FUN_140638a40
int32_t FrameScript_CompileFunction(const char* name, const char* wrapper, const char* body, CStatus* status) {
    lua_State* L = s_L;
    uint64_t size = strlen(wrapper) - 1 + strlen(body) + 2;
    char* source = static_cast<char*>(_alloca((size + 0xf) & ~0xfull));
    SStrPrintf(source, size, wrapper, body);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_errorHandlerRef);
    if (luaL_loadbuffer(L, source, strlen(source), name) != 0) {
        status->Add(2, "%s", lua_tolstring(L, -1, nullptr));
        // FUN_14063e310: lua_pcall behind an anti-tamper return-address check
        if (lua_pcall(L, 1, 0, 0) != 0) {
            lua_settop(L, -2);
        }
        return -1;
    }
    if (lua_pcall(L, 0, 1, -2) != 0) {
        status->Add(2, "%s", lua_tolstring(L, -1, nullptr));
        lua_settop(L, -3);
        return -1;
    }
    int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, -2);
    return ref;
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

// ref: FUN_140641c60
char* FrameScript_FormatString(lua_State* L, int32_t index, char* buffer, uint32_t size) {
    size_t length = 0;
    const char* format = luaL_checklstring(L, index, &length);
    const char* end = format + length;
    char* out = buffer;
    int32_t arg = index;
    if (format >= end) {
        *buffer = '\0';
        return buffer;
    }
    const char* next = format;
    do {
        if (size < 2) {
            break;
        }
        next = format + 1;
        if (*format != '%') {
            *out++ = *format;
            size--;
            format = next;
            continue;
        }
        char c = *next;
        if (c == '%') {
            *out++ = '%';
            next = format + 2;
            size--;
            format = next;
            continue;
        }
        char spec[0x80];
        spec[0] = '%';
        if (static_cast<uint8_t>(c - '0') < 10 && format[2] == '$') {
            arg = c - '1' + index;
            next = format + 3;
        }
        arg++;
        const char* s = next;
        while (static_cast<uint8_t>(*s) <= 0x30 && ((0x1280900000000ull >> (static_cast<uint8_t>(*s) & 0x3f)) & 1) != 0) {
            s++;
        }
        while ('/' < *s && *s < ':') {
            s++;
        }
        if (*s == '.') {
            s++;
        }
        if (*s == '-') {
            s++;
        }
        while ('/' < *s && *s < ':') {
            s++;
        }
        size_t width = static_cast<size_t>(s - next);
        if (0x7d < width) {
            luaL_error(L, "invalid format (width or precision too long)");
        }
        std::memcpy(spec + 1, next, width + 1);
        spec[width + 2] = '\0';
        char conversion = *s;
        next = s + 1;
        int32_t written = 0;
        bool decimal = false;
        switch (conversion) {
        case 'E':
        case 'G':
        case 'e':
        case 'f':
        case 'g':
            written = static_cast<int32_t>(SStrPrintf(out, size, spec, luaL_checknumber(L, arg)));
            break;
        case 'F':
            for (char* p = spec; *p != '\0'; p++) {
                if (*p == 'F') {
                    *p = 'f';
                }
            }
            written = static_cast<int32_t>(SStrPrintf(out, size, spec, luaL_checknumber(L, arg)));
            decimal = true;
            break;
        case 'X':
        case 'o':
        case 'u':
        case 'x':
            written = static_cast<int32_t>(SStrPrintf(out, size, spec, static_cast<int32_t>(luaL_checkinteger(L, arg))));
            break;
        case 'c':
            *out++ = static_cast<char>(luaL_checkinteger(L, arg));
            size--;
            break;
        case 'd':
        case 'i':
            written = static_cast<int32_t>(SStrPrintf(out, size, spec, static_cast<int32_t>(luaL_checkinteger(L, arg))));
            decimal = true;
            break;
        case 's': {
            size_t ignored = 0;
            written = static_cast<int32_t>(SStrPrintf(out, size, spec, luaL_checklstring(L, arg, &ignored)));
            break;
        }
        default:
            luaL_error(L, "invalid option in `format'");
            break;
        }
        if (0 < written) {
            if (decimal) {
                lua_convertdecimal(out);
            }
            out += written;
            size -= static_cast<uint32_t>(written);
        }
        format = next;
    } while (next < end);
    *out = '\0';
    return buffer;
}

// ref: FUN_14063daa0
void FrameScript_RegisterFunction(const char* name, lua_CFunction function) {
    lua_State* L = FrameScript_GetContext();
    lua_pushcclosure(L, function, 0);
    if (name == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, name, strlen(name));
    }
    lua_insert(L, -2);
    lua_settable(L, LUA_GLOBALSINDEX);
}

// ref: FUN_14063d2b0
void FrameScript_RegisterNamespace(const SCRIPTFUNCTION* functions, uint32_t count, const char* space) {
    lua_State* L = FrameScript_GetContext();
    if (space == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, space, strlen(space));
    }
    lua_gettable(L, LUA_GLOBALSINDEX);
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_settop(L, -2);
        lua_createtable(L, 0, 0);
        if (space == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, space, strlen(space));
        }
        lua_pushvalue(L, -2);
        lua_settable(L, LUA_GLOBALSINDEX);
    }
    for (uint32_t i = 0; i < count; i++) {
        if (functions[i].m_name == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, functions[i].m_name, strlen(functions[i].m_name));
        }
        lua_pushcclosure(L, functions[i].m_function, 0);
        lua_settable(L, -3);
    }
    lua_settop(L, -2);
}

// ref: FUN_14063e1b0
void FrameScript_SetGlobalNumber(const char* name, int32_t value) {
    lua_State* L = FrameScript_GetContext();
    lua_pushnumber(L, static_cast<double>(value));
    if (name == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, name, strlen(name));
    }
    lua_insert(L, -2);
    lua_settable(L, LUA_GLOBALSINDEX);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cf5504 = (void*)&s_objectTableRef;
extern "C" void* const gen_p_142e99e18 = (void*)&s_L;
extern "C" void* const gen_p_142e99e20 = (void*)&s_threadId;
extern "C" void* const gen_p_142e99e28 = (void*)&s_stateHook;
extern "C" void* const gen_p_142e99f30 = (void*)&s_initCount;
extern "C" void* const gen_p_142e9a07c = (void*)&s_currentObjectScoped;
// ---- end of generated globals ----
