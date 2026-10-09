#include "framescript/FrameScriptObject.hpp"

#include <cstdarg>
#include <cstring>
#include <string>

#include "base/Status.hpp"
#include "blz/Murmur.hpp"
#include "blz/String.hpp"
#include "blz/Vector.hpp"
#include "framescript/FrameScript.hpp"
#include "framescript/FrameScriptEvents.hpp"
#include "framescript/Taint.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/BinaryXML.hpp"
#include "ui/CSimpleFrame.hpp"
#include "ui/CSimpleTop.hpp"
#include "ui/FrameXML.hpp"

extern "C" {
#include "lauxlib.h"
#include "lblizzard.h"
#include "lua.h"
}

namespace {

// The value a secure mixin proxy's metatable is locked with.
constexpr lua_Integer SECURE_MIXIN_TAG = 0xa16d;

// 0x142cf5510
const char* const s_scriptNames[FRAMESCRIPT_NUM_SCRIPTS] = {
    "onclick", "preclick", "postclick", "ondoubleclick", "onload", "onfinished", "onplay",
    "onpause", "onstop", "onsizechanged", "onupdate", "onloop", "onshow", "onhide", "onenter",
    "onleave", "onmousedown", "onmouseup", "onmousewheel", "ondragstart", "ondragstop",
    "onreceivedrag", "onchar", "onhyperlinkclick", "onhyperlinkenter", "onhyperlinkleave",
    "onevent", "onkeydown", "onkeyup", "ongamepadbuttondown", "ongamepadbuttonup",
    "ongamepadstick", "onattributechanged", "onenable", "ondisable", "onenterpressed",
    "onescapepressed", "onscrollrangechanged", "onverticalscroll", "onhorizontalscroll",
    "onvaluechanged", "ontabpressed", "oneditfocuslost", "oneditfocusgained", "ontextchanged",
    "onmodelloaded", "onmodelcleared", "onmodelloading", "ondressmodel", "onmovieshowsubtitle",
    "onmoviehidesubtitle", "onmoviefinished", "ontooltipsetdefaultanchor", "ontooltipcleared",
    "ontooltipaddmoney", "ontooltipsetunit", "ontooltipsetitem", "ontooltipsetspell",
    "ontooltipsetquest", "ontooltipsetachievement", "ontooltipsetequipmentset",
    "ontooltipsetframestack", "onspacepressed", "ontextset", "oninputlanguagechanged",
    "oncharcomposition", "onminmaxchanged", "oncursorchanged", "onanimstarted",
    "onanimfinished", "oncolorselect", "onexternallink", "onbuttonupdate", "onarrowpressed",
    "onerror", "onpanfinished", "oncooldowndone", "onrequestnewsize", "onuimapchanged",
};

}

// ref: FUN_140642a70
uint32_t FrameScript_GetScriptIndex(const char* name) {
    if (name != nullptr) {
        char lower[0x80];
        char* dest = lower;
        uint64_t left = 0x7f;
        while (true) {
            char c = *name++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *dest = '\0';
                break;
            }
        }
        _strlwr(lower);
        for (uint32_t i = 0; i < FRAMESCRIPT_NUM_SCRIPTS; i++) {
            if (strcmp(lower, s_scriptNames[i]) == 0) {
                return i;
            }
        }
    }
    return FRAMESCRIPT_NUM_SCRIPTS + 1;
}

// ref: FUN_140644e90
void FrameScript_CreateSecureMixins(lua_State* L, const char* mixins, const char* owner, CStatus* status) {
    if (L == nullptr || mixins == nullptr) {
        return;
    }
    char name[0x400];
    name[0] = '\0';
    const char* cursor = mixins;
    do {
        SStrTokenize(&cursor, name, sizeof(name), " ,", nullptr);
        if (name[0] == '\0') {
            return;
        }
        lua_checkstack(L, 4);
        lua_pushlstring(L, name, strlen(name));
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_type(L, -1) == LUA_TTABLE) {
            int32_t pushed = lua_getmetatable(L, -1);
            int32_t top;
            if (pushed != 0) {
                lua_pushlstring(L, "__metatable", 11);
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1) && lua_tointeger(L, -1) == SECURE_MIXIN_TAG) {
                    // already a secure proxy
                    lua_settop(L, -3 - pushed);
                    continue;
                }
                lua_settop(L, -2 - pushed);
                lua_pushnil(L);
                lua_setmetatable(L, -2);
            }
            // _G[name] = setmetatable({}, { __index = original, __metatable = tag })
            lua_createtable(L, 0, 0);
            lua_pushlstring(L, "__index", 7);
            lua_pushvalue(L, -3);
            lua_settable(L, -3);
            lua_pushlstring(L, "__metatable", 11);
            lua_pushinteger(L, SECURE_MIXIN_TAG);
            lua_settable(L, -3);
            lua_createtable(L, 0, 0);
            lua_pushvalue(L, -2);
            lua_setmetatable(L, -2);
            lua_pushlstring(L, name, strlen(name));
            lua_insert(L, -2);
            lua_rawset(L, LUA_GLOBALSINDEX);
            top = -3;
            lua_settop(L, top);
        } else {
            lua_settop(L, -2);
            status->Add(1, "%s: Has bad mixin: %s", owner != nullptr ? owner : "", name);
        }
    } while (name[0] != '\0');
}

// ---- the handler storage ------------------------------------------------------------------

namespace {

const char s_scriptTag[] = ".?AUScript@FrameScript_Object@@";
const char s_scriptMapTag[] = ".?AU?$pair@QEBVFrameScript_Object@@V?$vector@UScript@FrameScript_Object@@V?$allocator@"
                              "UScript@FrameScript_Object@@@blz@@@blz@@@blz@@";
const char s_sourceFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\FrameScript\\FrameScript_Object.cpp";
const char s_scriptWrapper[] = "return function(self) %s end";

typedef blz::vector<FrameScript_Object::Script, s_scriptTag> ScriptVector;

// The bounded copy the reference inlines: at most size - 1 characters, always terminated.
void CopyString(char* dest, const char* source, uint64_t size) {
    for (uint64_t left = size - 1;; left--) {
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
        if (left == 1) {
            *dest = '\0';
            return;
        }
    }
}

// 0x142cf57b0: built by the static initializer FUN_1401dfb60 with 8 buckets, at most 3/4 full.
struct ScriptMap : blz::pointer_map<ScriptVector, s_scriptMapTag> {
    ScriptMap() {
        this->init(0.75f);
    }
};
ScriptMap s_scripts;

// The empty type description (0x1426cd550).
// 0x1426cd550: sixteen zero bytes
const SCRIPTLOCATION s_noLocation[2] = {};

// The pending script error an object's method leaves for its Lua wrapper (0x142cf5390).
blz::string s_scriptError;

// Calls `visit` with each of the object's scripts until it returns false.
// ref: FUN_14065e9a0
template <class Visit>
void ForEachScript(const FrameScript_Object* object, Visit visit) {
    ScriptVector* scripts = s_scripts.find(reinterpret_cast<uint64_t>(object));
    if (scripts == nullptr) {
        return;
    }
    for (FrameScript_Object::Script& script : *scripts) {
        if (!visit(&script)) {
            return;
        }
    }
}

// The script of an index and binding: its reference and taint (LAB_140644b10).
bool FindScript(const FrameScript_Object* object, uint8_t index, uint8_t binding, int32_t* ref, const char** taint) {
    bool found = false;
    ForEachScript(object, [&](FrameScript_Object::Script* script) {
        if (script->m_index != index || script->m_binding != binding) {
            return true;
        }
        *ref = script->m_ref;
        *taint = script->m_taint;
        found = true;
        return false;
    });
    return found;
}

// Sets, replaces or (with a zero reference) removes one script; returns the reference it had.
// ref: FUN_14065eca0
int32_t SetScriptRefStorage(const FrameScript_Object* object, uint8_t index, uint8_t binding, int32_t ref, const char* taint) {
    uint64_t key = reinterpret_cast<uint64_t>(object);
    ScriptVector* scripts = s_scripts.find(key);
    if (scripts != nullptr) {
        for (uint64_t i = 0; i < scripts->size(); i++) {
            FrameScript_Object::Script& script = (*scripts)[i];
            if (script.m_index == index && script.m_binding == binding) {
                int32_t old = script.m_ref;
                if (ref == 0) {
                    scripts->erase_unordered(i);
                    return old;
                }
                script.m_taint = taint;
                script.m_ref = ref;
                return old;
            }
        }
    }
    if (ref != 0) {
        scripts = s_scripts.find(key);
        if (scripts == nullptr) {
            // FUN_14065e690
            scripts = s_scripts.insert(key);
        }
        FrameScript_Object::Script script;
        script.m_taint = taint;
        script.m_ref = ref;
        script.m_index = index;
        script.m_binding = binding;
        scripts->push_back(script);
    }
    return 0;
}

}

// The name an object goes by in messages: its own, else its type's.
// ref: FUN_140628560
const char* GetDisplayName(FrameScript_Object* object) {
    const char* name = object->GetName();
    return name != nullptr ? name : object->GetObjectTypeName();
}

namespace {

// A blz::chained_hash_set<int64> (0x20): the tables WrapTable has visited.
struct VisitedNode {
    VisitedNode* m_next;
    int64_t m_key;
};

const char s_visitedNodeTag[] = ".?AU?$chained_hash_node@_J@blz@@";
const char s_visitedBucketTag[] = ".PEAU?$chained_hash_node@_J@blz@@";

struct VisitedSet {
    uint64_t m_bucketCount = 8;
    VisitedNode** m_buckets = nullptr;
    uint64_t m_size = 0;
    float m_maxLoad = 1.0f;

    ~VisitedSet() {
        if (this->m_buckets == nullptr) {
            return;
        }
        for (uint64_t i = 0; i < this->m_bucketCount; i++) {
            VisitedNode* node = this->m_buckets[i];
            while (node != nullptr) {
                VisitedNode* next = node->m_next;
                SMemFree(node, s_visitedNodeTag, -6, 0);
                node = next;
            }
        }
        SMemFree(this->m_buckets, s_visitedBucketTag, -6, 0);
    }

    // ref: FUN_140647090
    void Rehash(uint64_t count) {
        float needed = static_cast<float>(this->m_size) / this->m_maxLoad;
        uint64_t wanted = static_cast<uint64_t>(needed);
        if (static_cast<float>(this->m_size) > static_cast<float>(wanted) * this->m_maxLoad) {
            wanted++;
        }
        if (wanted < count) {
            wanted = count;
        }
        uint64_t buckets = 8;
        while (buckets < wanted) {
            buckets *= 2;
        }
        if (buckets == this->m_bucketCount && this->m_buckets != nullptr) {
            return;
        }
        auto table = static_cast<VisitedNode**>(
            SMemAlloc(buckets * 8, 0xb, s_visitedBucketTag, SStrHashFnv1a(s_visitedBucketTag), -6, 0));
        std::memset(table, 0, buckets * 8);
        if (this->m_buckets != nullptr) {
            for (uint64_t i = 0; i < this->m_bucketCount; i++) {
                VisitedNode* node = this->m_buckets[i];
                while (node != nullptr) {
                    VisitedNode* next = node->m_next;
                    uint64_t index = blz::fmix64(node->m_key) & (buckets - 1);
                    node->m_next = table[index];
                    table[index] = node;
                    node = next;
                }
            }
        }
        SMemFree(this->m_buckets, s_visitedBucketTag, -6, 0);
        this->m_buckets = table;
        this->m_bucketCount = buckets;
    }

    // True when the key was not there yet.
    // ref: FUN_140644c50
    bool Insert(int64_t key) {
        if (this->m_buckets == nullptr) {
            this->Rehash(this->m_bucketCount);
        }
        auto node = static_cast<VisitedNode*>(
            SMemAlloc(sizeof(VisitedNode), 0xb, s_visitedNodeTag, SStrHashFnv1a(s_visitedNodeTag), -6, 0));
        node->m_next = nullptr;
        node->m_key = key;
        VisitedNode** link = &this->m_buckets[blz::fmix64(key) & (this->m_bucketCount - 1)];
        while (*link != nullptr && (*link)->m_key != key) {
            link = &(*link)->m_next;
        }
        if (*link != nullptr) {
            SMemFree(node, s_visitedNodeTag, -6, 0);
            return false;
        }
        *link = node;
        this->m_size++;
        float load = static_cast<float>(this->m_size) / static_cast<float>(this->m_bucketCount);
        if (this->m_maxLoad <= load && load != this->m_maxLoad) {
            this->Rehash(this->m_bucketCount * 2);
        }
        return true;
    }
};

// A function that runs another under a taint: upvalue 1 the function, 2 the taint.
// ref: FUN_140646ff0
int TaintedCall(lua_State* L) {
    int32_t count = lua_gettop(L);
    auto taint = static_cast<const char*>(lua_touserdata(L, lua_upvalueindex(2)));
    lua_pushvalue(L, lua_upvalueindex(1));
    return FrameScript_CallWithTaint(L, taint, lua_gettop(L), 1, count);
}

// Replaces the function at `index` with one that runs it under `taint`.
// ref: FUN_140646d30
void WrapFunction(lua_State* L, int32_t index, const char* taint) {
    lua_checkstack(L, 2);
    lua_pushvalue(L, index);
    lua_pushlightuserdata(L, const_cast<char*>(taint));
    lua_pushcclosure(L, TaintedCall, 2);
    lua_replace(L, index);
}

// Wraps every function of a table and of the tables under it, ten levels deep, each table once.
// ref: FUN_140646ea0
void WrapTable(lua_State* L, int32_t index, const char* taint, int32_t depth, VisitedSet* visited) {
    if (depth > 10) {
        return;
    }
    lua_checkstack(L, 4);
    lua_pushvalue(L, index);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        int32_t type = lua_type(L, -1);
        if (type == LUA_TTABLE) {
            auto table = reinterpret_cast<int64_t>(lua_topointer(L, -1));
            if (table != 0 && visited->Insert(table)) {
                WrapTable(L, -1, taint, depth + 1, visited);
            }
            lua_settop(L, -2);
        } else if (type == LUA_TFUNCTION) {
            lua_pushvalue(L, -2);
            lua_insert(L, -3);
            WrapFunction(L, -1, taint);
            lua_settable(L, -4);
        } else {
            lua_settop(L, -2);
        }
    }
    lua_settop(L, -2);
}

// A secure mixin's method: self becomes the object's table, every function or table argument
// is wrapped to run under the caller's taint, and the method runs untainted.
// Upvalue 1 the method, 2 the object's table.
// ref: FUN_1406469b0
int SecureMixinCall(lua_State* L) {
    const char* previous = lua_taint;
    char* error = nullptr;
    FrameScript_EnterTaint(L, nullptr);
    int32_t count = lua_gettop(L);
    int32_t arg = 0;
    while (arg < count) {
        if (arg == 0) {
            lua_checkstack(L, 1);
            lua_pushvalue(L, lua_upvalueindex(2));
            lua_replace(L, 1);
            arg = 1;
            continue;
        }
        int32_t index = arg + 1;
        lua_settaint(L, index, nullptr);
        if (lua_type(L, index) == LUA_TFUNCTION) {
            WrapFunction(L, index, previous);
        } else if (lua_type(L, index) == LUA_TTABLE) {
            VisitedSet visited;
            WrapTable(L, index, previous, 0, &visited);
        }
        arg = index;
    }
    lua_checkstack(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    // FUN_14063e310: lua_pcall behind an anti-tamper return-address check
    if (lua_pcall(L, count, LUA_MULTRET, 0) != 0) {
        error = SStrDupA(lua_tolstring(L, -1, nullptr), nullptr, 0);
        lua_settop(L, -2);
    }
    FrameScript_LeaveTaint(L, previous);
    if (error != nullptr) {
        lua_pushlstring(L, error, strlen(error));
        SMemFree(error, nullptr, 0, 0);
        lua_error(L);
        return 0;
    }
    return lua_gettop(L);
}

// Runs upvalue 1 then upvalue 2 with the same arguments (an inherited handler and the
// element's own, in the order the element asked for).
// ref: FUN_140642420
int ChainedHandlerCall(lua_State* L) {
    int32_t count = lua_gettop(L);
    lua_checkstack(L, count);
    for (int32_t i = 1; i <= count; i++) {
        lua_pushvalue(L, i);
    }
    lua_checkstack(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, count + 1);
    lua_call(L, count, 0);
    lua_checkstack(L, 1);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, 1);
    lua_call(L, count, 0);
    return 0;
}

// A hooked handler: the original (upvalue 1) runs and its results are kept, then the hook
// (upvalue 2) runs under the current taint, its errors going to the error handler.
// ref: FUN_1406466d0
int HookedHandlerCall(lua_State* L) {
    int32_t count = lua_gettop(L);
    lua_checkstack(L, count);
    for (int32_t i = 1; i <= count; i++) {
        lua_pushvalue(L, i);
    }
    lua_checkstack(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, count + 1);
    lua_call(L, count, LUA_MULTRET);
    int32_t results = lua_gettop(L) - count;
    for (int32_t i = 0; i < results; i++) {
        lua_insert(L, 1);
    }
    lua_checkstack(L, 1);
    const char* previous = lua_taint;
    FrameScript_EnterTaint(L, lua_taint);
    lua_checkstack(L, 1);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, results + 1);
    lua_checkstack(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetErrorHandlerRef());
    lua_insert(L, results + 1);
    // FUN_14063e310: lua_pcall behind an anti-tamper return-address check
    int32_t status = lua_pcall(L, count, 0, results + 1);
    lua_settop(L, -2 - (status != 0 ? 1 : 0));
    FrameScript_LeaveTaint(L, previous);
    return results;
}

}

FrameScript_Object::Script* FrameScript_FindScripts(const FrameScript_Object* object, uint64_t* count) {
    ScriptVector* scripts = s_scripts.find(reinterpret_cast<uint64_t>(object));
    if (scripts == nullptr) {
        *count = 0;
        return nullptr;
    }
    *count = scripts->size();
    return scripts->data();
}

// Releases every script of an object and forgets it.
// ref: FUN_14065ea10
void FrameScript_ClearScripts(const FrameScript_Object* object) {
    uint64_t key = reinterpret_cast<uint64_t>(object);
    ScriptVector* scripts = s_scripts.find(key);
    if (scripts != nullptr) {
        for (FrameScript_Object::Script& script : *scripts) {
            if (script.m_ref != 0) {
                FrameScript_ReleaseFunction(script.m_ref);
            }
        }
    }
    scripts = s_scripts.find(key);
    if (scripts == nullptr) {
        return;
    }
    scripts->m_size = 0;
    scripts->~ScriptVector();
    s_scripts.erase(key);
}

// ---- errors -------------------------------------------------------------------------------

// ref: FUN_14062ce80
void FrameScript_ObjectError(FrameScript_Object* object, const char* method, const char* format, ...) {
    blz::inline_string<200> message;
    va_list args;
    va_start(args, format);
    int32_t length = SStrVSnprintf(nullptr, 0, format, args);
    va_end(args);
    uint64_t size = 0;
    if (length < 0) {
        message.m_data[0] = '\0';
    } else {
        blz::string_reserve(message.header(), static_cast<uint64_t>(length));
        va_start(args, format);
        SStrVSnprintf(message.m_data, static_cast<uint64_t>(length) + 1, format, args);
        va_end(args);
        message.m_data[length] = '\0';
        size = static_cast<uint64_t>(length);
    }
    message.m_size = size;
    auto error = reinterpret_cast<blz::string_header*>(&s_scriptError);
    if (method == nullptr) {
        blz::string_assign(error, message.m_data, size);
    } else {
        blz::string_format(error, "%s:%s(): %s", GetDisplayName(object), method, message.m_data);
    }
}

// ref: FUN_14062cd40
void FrameScript_ActionError(const char* format, ...) {
    auto error = reinterpret_cast<blz::string_header*>(&s_scriptError);
    va_list args;
    va_start(args, format);
    int32_t length = SStrVSnprintf(nullptr, 0, format, args);
    va_end(args);
    uint64_t size = 0;
    if (length < 0) {
        error->m_data[0] = '\0';
    } else {
        size = static_cast<uint64_t>(length);
        blz::string_reserve(error, size);
        va_start(args, format);
        SStrVSnprintf(error->m_data, size + 1, format, args);
        va_end(args);
        error->m_data[size] = '\0';
    }
    error->m_size = size;
    if (lua_taint != nullptr) {
        blz::string_append_format(error, "\nLua Taint: %s", lua_taint);
        return;
    }
    blz::string_append_format(error, "%s", "\nNo Lua Taint");
}

// ref: FUN_14062cc40
bool FrameScript_PushObjectError(lua_State* L, FrameScript_Object* object, const char* method) {
    const char* name = object != nullptr ? GetDisplayName(object) : nullptr;
    if (s_scriptError.m_size == 0) {
        return false;
    }
    char message[200];
    if (name == nullptr || method == nullptr) {
        CopyString(message, s_scriptError.m_data, sizeof(message));
    } else {
        SStrPrintf(message, sizeof(message), "%s:%s(): %s", name, method, s_scriptError.m_data);
    }
    s_scriptError.m_data[0] = '\0';
    s_scriptError.m_size = 0;
    lua_pushstring(L, message);
    return true;
}

// ---- FrameScript_Object -------------------------------------------------------------------

int32_t FrameScript_Object::s_objectTypeCount;

// ref: FUN_140641fd0
FrameScript_Object::FrameScript_Object() {
    this->m_luaRef = -2;
    this->m_flags = 0;
    uint32_t scoped = FrameXML_GetScopedState();
    this->m_flags = (scoped & 1) ? (this->m_flags | FSOBJ_FORBIDDEN) : (this->m_flags & ~FSOBJ_FORBIDDEN);
    scoped = FrameXML_GetScopedState();
    this->m_flags = (scoped & 2) ? (this->m_flags | FSOBJ_PROTECTED) : (this->m_flags & ~FSOBJ_PROTECTED);
    scoped = FrameXML_GetScopedState();
    this->m_flags = (scoped & 8) ? (this->m_flags | FSOBJ_LOCKDOWN) : (this->m_flags & ~FSOBJ_LOCKDOWN);
    scoped = FrameXML_GetScopedState();
    this->m_flags = (scoped & 0x10) ? (this->m_flags | FSOBJ_HIDDEN) : (this->m_flags & ~FSOBJ_HIDDEN);
    this->m_flags |= FSOBJ_ACCESSIBLE;
}

// ref: FUN_1406420b0
FrameScript_Object::~FrameScript_Object() {
    FrameScript_ClearScripts(this);
}

void FrameScript_Object::OnScriptsChanged(uint8_t index, bool hasScript) {
    (void)index;
    (void)hasScript;
}

bool FrameScript_Object::GetScriptData(uint8_t index, FRAMESCRIPT_SCRIPTDATA* data) {
    (void)index;
    (void)data;
    return false;
}

// ref: FUN_1406429a0
void FrameScript_Object::GetScriptCPUUsage(lua_State* L, double* time, int32_t* calls, bool includeChildren) {
    (void)includeChildren;
    // FUN_140644aa0
    ForEachScript(this, [&](Script* script) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, script->m_ref);
        double own;
        double total;
        int32_t count;
        lua_getclosureprofile(L, &own, &total, &count);
        lua_settop(L, -2);
        *time = total + *time;
        *calls = *calls + count;
        return true;
    });
}

// ref: FUN_140642b20
const SCRIPTLOCATION* FrameScript_Object::GetScriptLocation() {
    return s_noLocation;
}

void FrameScript_Object::SetSourceLocation(const SCRIPTLOCATION* location) {
    (void)location;
}

void FrameScript_Object::PreLoadXML(XMLOBJECT* node, CStatus* status) {
    (void)node;
    (void)status;
}

void FrameScript_Object::OnProtected() {
}

// ref: FUN_140608d00
bool FrameScript_Object::IsObjectTypeName(const char* name) {
    return _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140608c80
bool FrameScript_Object::IsObjectType(int32_t type) {
    return type == FrameScript_Object::GetObjectType();
}

// ref: FUN_140642700
const char* FrameScript_Object::GetObjectTypeName() {
    return "FrameScript_Object";
}

// ref: FUN_140642580
uint32_t FrameScript_Object::GetObjectSize() {
    return 0x10;
}

int32_t FrameScript_Object::GetObjectType() {
    // 0x142e97a94, numbered on first use
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// ref: FUN_140642110
int32_t FrameScript_Object::PushObject(lua_State* L, FrameScript_Object* object) {
    if (object != nullptr && (object->m_flags & FSOBJ_ACCESSIBLE) && !(object->m_flags & FSOBJ_HIDDEN)) {
        if (!(object->m_flags & FSOBJ_REGISTERED)) {
            object->Register(nullptr);
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, object->m_luaRef);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// ref: FUN_140642180
int32_t FrameScript_Object::CreateScriptMetaTable(lua_State* L, void (*fill)(lua_State*), bool forbidden) {
    lua_createtable(L, 0, 0);
    lua_pushlstring(L, "__index", 7);
    lua_createtable(L, 0, 0);
    fill(L);
    lua_settable(L, -3);
    if (forbidden) {
        lua_pushlstring(L, "__metatable", 11);
        lua_pushlstring(L, "Forbidden", 9);
        lua_settable(L, -3);
    }
    if (lua_type(L, -1) == LUA_TNIL) {
        lua_settop(L, -2);
        return -1;
    }
    // the reference is taken invisible to taint
    luaT_State saved;
    luaT_save(&saved);
    lua_taintTracking = 0;
    luaT_settaint(L, nullptr);
    lua_firstTaint = nullptr;
    int32_t ref = luaL_ref(L, LUA_REGISTRYINDEX);
    luaT_settaint(L, saved.taint);
    luaT_restore(&saved);
    return ref;
}

// ref: FUN_140642320
void FrameScript_Object::FillScriptMethodTable(lua_State* L, const void* methods, int32_t count) {
    auto entry = static_cast<const luaL_Reg*>(methods);
    for (int32_t i = 0; i < count; i++, entry++) {
        if (entry->name == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, entry->name, strlen(entry->name));
        }
        lua_pushcclosure(L, entry->func, 0);
        lua_settable(L, -3);
    }
}

// ref: FUN_140642590
FrameScript_Object* FrameScript_Object::GetObjectFromLua(const char** error, lua_State* L, int32_t type, int32_t index,
    bool allowForbidden, bool allowLockdown) {
    const char* ignored;
    if (error == nullptr) {
        error = &ignored;
    }
    if (lua_type(L, index) != LUA_TTABLE) {
        *error = "Attempt to find 'this' in non-table object (used '.' instead of ':' ?)";
        return nullptr;
    }
    lua_rawgeti(L, index, 0);
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
        auto object = static_cast<FrameScript_Object*>(lua_touserdata(L, -1));
        lua_settop(L, -2);
        if (object == nullptr) {
            *error = "Attempt to find 'this' in non-framescript object";
            return nullptr;
        }
        if (object->IsObjectType(type)) {
            if (!allowForbidden && ((object->m_flags & FSOBJ_FORBIDDEN) || (object->m_flags & FSOBJ_PROTECTED_METATABLE))
                && lua_taint != nullptr) {
                *error = "Attempt to access forbidden object from code tainted by an AddOn";
                return nullptr;
            }
            if (!allowLockdown && ((object->m_flags & FSOBJ_LOCKDOWN) || !(object->m_flags & FSOBJ_ACCESSIBLE))) {
                *error = "Attempt to access fully locked-down object.";
                return nullptr;
            }
            *error = nullptr;
            return object;
        }
    } else {
        lua_settop(L, -2);
    }
    *error = "Wrong object type for function";
    return nullptr;
}

// ref: FUN_1406420f0
bool FrameScript_Object::IsAccessible() {
    if (((this->m_flags & FSOBJ_FORBIDDEN) || (this->m_flags & FSOBJ_PROTECTED_METATABLE)) && lua_taint != nullptr) {
        return false;
    }
    return true;
}

// ref: FUN_140642b30
bool FrameScript_Object::IsRegistered() {
    return (this->m_flags & FSOBJ_REGISTERED) && this->m_luaRef != -2;
}

// ref: FUN_1406426c0
int32_t FrameScript_Object::GetLuaRef() {
    if (!(this->m_flags & FSOBJ_REGISTERED)) {
        this->Register(nullptr);
    }
    return this->m_luaRef;
}

// ref: FUN_1406426f0
int32_t FrameScript_Object::GetRawLuaRef() {
    return this->m_luaRef;
}

// The object's table is {[0] = the object}, with the class metatable; a name makes it a
// global (unless the scoped state hides it) and an entry of the current object's environment.
// ref: FUN_140644190
void FrameScript_Object::Register(const char* name) {
    if (!(this->m_flags & FSOBJ_ACCESSIBLE)) {
        return;
    }
    lua_State* L = FrameScript_GetContext();
    uint32_t scoped = FrameXML_GetScopedState();
    if (!(this->m_flags & FSOBJ_REGISTERED)) {
        lua_checkstack(L, 3);
        const char* previous = lua_taint;
        FrameScript_EnterTaint(L, nullptr);
        lua_createtable(L, 0, 0);
        lua_pushnumber(L, 0.0);
        lua_pushlightuserdata(L, this);
        lua_rawset(L, -3);
        lua_rawgeti(L, LUA_REGISTRYINDEX, this->GetScriptMetaTable());
        lua_setmetatable(L, -2);
        this->m_luaRef = luaL_ref(L, LUA_REGISTRYINDEX);
        FrameScript_LeaveTaint(L, previous);
    }
    this->m_flags |= FSOBJ_REGISTERED;
    if (name == nullptr) {
        return;
    }
    if (!(scoped & 0x10)) {
        lua_checkstack(L, 3);
        lua_pushlstring(L, name, strlen(name));
        lua_rawget(L, LUA_GLOBALSINDEX);
        int32_t type = lua_type(L, -1);
        lua_settop(L, -2);
        if (type == LUA_TNIL) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, this->m_luaRef);
            lua_pushlstring(L, name, strlen(name));
            lua_insert(L, -2);
            lua_rawset(L, LUA_GLOBALSINDEX);
        }
    }
    if (scoped & 4) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetCurrentObject());
        lua_pushlstring(L, name, strlen(name));
        lua_rawget(L, -2);
        int32_t type = lua_type(L, -1);
        lua_settop(L, -2);
        if (type == LUA_TNIL) {
            lua_pushlstring(L, name, strlen(name));
            lua_rawgeti(L, LUA_REGISTRYINDEX, this->m_luaRef);
            lua_rawset(L, -3);
        }
        lua_settop(L, -2);
    }
}

// ref: FUN_1406449a0
void FrameScript_Object::Unregister(const char* name) {
    lua_State* L = FrameScript_GetContext();
    if (this->m_flags & FSOBJ_REGISTERED) {
        this->m_flags &= ~FSOBJ_REGISTERED;
        lua_rawgeti(L, LUA_REGISTRYINDEX, this->m_luaRef);
        lua_pushnumber(L, 0.0);
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_settop(L, -2);
        luaL_unref(L, LUA_REGISTRYINDEX, this->m_luaRef);
        this->m_luaRef = -2;
    }
    if (name != nullptr) {
        lua_pushnil(L);
        lua_pushlstring(L, name, strlen(name));
        lua_insert(L, -2);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
}

// ref: FUN_1406444b0
void FrameScript_Object::SetProtected() {
    uint16_t flags = this->m_flags;
    this->m_flags = flags | FSOBJ_PROTECTED;
    if (flags & FSOBJ_PROTECTED_METATABLE) {
        return;
    }
    this->m_flags = flags | FSOBJ_PROTECTED | FSOBJ_PROTECTED_METATABLE;
    if ((flags & FSOBJ_REGISTERED) && this->m_luaRef != -2) {
        lua_State* L = FrameScript_GetContext();
        const char* previous = lua_taint;
        FrameScript_EnterTaint(L, nullptr);
        lua_rawgeti(L, LUA_REGISTRYINDEX, this->m_luaRef);
        if (lua_type(L, -1) != LUA_TNIL) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, this->GetScriptMetaTable());
            lua_setmetatable(L, -2);
        }
        lua_settop(L, -2);
        FrameScript_LeaveTaint(L, previous);
    }
    this->OnProtected();
}

// ref: FUN_140642b50
bool FrameScript_Object::HasScript(const char* name) {
    FRAMESCRIPT_SCRIPTDATA data = {s_scriptWrapper, -1};
    uint32_t index = FrameScript_GetScriptIndex(name);
    if (static_cast<uint8_t>(index) == FRAMESCRIPT_NUM_SCRIPTS + 1) {
        return false;
    }
    return this->GetScriptData(static_cast<uint8_t>(index), &data);
}

// `binding` is {binding, match}: with match clear any binding counts. The match byte is
// cleared on the way out.
// ref: FUN_140642ba0
bool FrameScript_Object::HasScriptBinding(uint8_t index, uint8_t* binding) {
    bool found = false;
    // LAB_140644b50
    ForEachScript(this, [&](Script* script) {
        if (script->m_index != index) {
            return true;
        }
        if (binding[1] != 0 && script->m_binding != binding[0]) {
            return true;
        }
        found = true;
        return false;
    });
    binding[1] = 0;
    return found;
}

// ref: FUN_140642c20
bool FrameScript_Object::HasScriptAny(uint8_t index) {
    uint8_t binding[2] = {0, 0};
    return this->HasScriptBinding(index, binding);
}

// The three bindings of a handler; an unset one has index 0x50 and binding 4.
// ref: FUN_140642910
FrameScript_Object::Script* FrameScript_Object::GetScripts(Script* scripts, uint8_t index) {
    for (uint32_t i = 0; i < 3; i++) {
        scripts[i].m_taint = nullptr;
        scripts[i].m_ref = 0;
        scripts[i].m_index = FRAMESCRIPT_NUM_SCRIPTS + 1;
        scripts[i].m_binding = 4;
    }
    // LAB_140644a80
    ForEachScript(this, [&](Script* script) {
        if (script->m_index == index) {
            scripts[script->m_binding] = *script;
        }
        return true;
    });
    return scripts;
}

// ref: FUN_140644890
int32_t FrameScript_Object::SetScriptRef(uint8_t index, uint8_t binding, int32_t ref, const char* taint) {
    int32_t old = SetScriptRefStorage(this, index, binding, ref, taint);
    this->OnScriptsChanged(index, this->HasScriptAny(index));
    return old;
}

// ref: FUN_140642710
int32_t FrameScript_Object::GetScript(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        FrameScript_ObjectError(this, "GetScript", "Usage: (\"frameScriptTypeName\"[, bindingType])");
        return 0;
    }
    FRAMESCRIPT_SCRIPTDATA data = {s_scriptWrapper, -1};
    const char* name = lua_tolstring(L, 2, nullptr);
    auto index = static_cast<uint8_t>(FrameScript_GetScriptIndex(name));
    if (!this->GetScriptData(index, &data)) {
        FrameScript_ObjectError(this, "GetScript", "Doesn't have a \"%s\" script", name != nullptr ? name : "");
        return 0;
    }
    uint8_t binding = 1;
    if (lua_isnumber(L, 3)) {
        binding = static_cast<uint8_t>(lua_tointeger(L, 3));
    }
    int32_t ref = -2;
    const char* taint = nullptr;
    if (!FindScript(this, index, binding, &ref, &taint)) {
        lua_pushnil(L);
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    }
    FrameScript_ReadTaint(L, taint);
    return 1;
}

// ref: FUN_140644630
int32_t FrameScript_Object::SetScript(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING || (lua_type(L, 3) != LUA_TFUNCTION && lua_type(L, 3) != LUA_TNIL)) {
        FrameScript_ObjectError(this, "SetScript", "Usage: (\"frameScriptTypeName\", function)");
        return 0;
    }
    FRAMESCRIPT_SCRIPTDATA data = {s_scriptWrapper, -1};
    const char* name = lua_tolstring(L, 2, nullptr);
    auto index = static_cast<uint8_t>(FrameScript_GetScriptIndex(name));
    if (!this->GetScriptData(index, &data)) {
        FrameScript_ObjectError(this, "GetScript", "Doesn't have a \"%s\" script", name != nullptr ? name : "");
        return 0;
    }
    int32_t ref = 0;
    if (lua_type(L, 3) == LUA_TFUNCTION) {
        lua_pushvalue(L, 3);
        int32_t made = luaL_ref(L, LUA_REGISTRYINDEX);
        if (made != -1) {
            ref = made;
        }
    }
    int32_t old = this->SetScriptRef(index, 1, ref, lua_taint);
    if (old != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, old);
    }
    if (ref != 0 && data.m_update != -1 && this->IsObjectType(CSimpleFrame::GetObjectType())) {
        auto frame = static_cast<CSimpleFrame*>(this);
        switch (data.m_update) {
            case 0:
            case 1:
            case 4:
            case 5:
            case 6:
                frame->EnableInputEvent(data.m_update);
                break;
            case 2:
            case 3:
                frame->SetMouseClickEnabled(true);
                frame->SetMouseMotionEnabled(true);
                break;
        }
    }
    return 0;
}

// ref: FUN_140642c40
int32_t FrameScript_Object::HookScript(lua_State* L) {
    if (lua_type(L, 2) != LUA_TSTRING || lua_type(L, 3) != LUA_TFUNCTION) {
        FrameScript_ObjectError(this, "HookScript", "Usage: (\"frameScriptTypeName\", function[, bindingType])");
        return 0;
    }
    const char* name = lua_tolstring(L, 2, nullptr);
    auto index = static_cast<uint8_t>(FrameScript_GetScriptIndex(name));
    int32_t hasBinding = lua_isnumber(L, 4);
    FRAMESCRIPT_SCRIPTDATA data = {s_scriptWrapper, -1};
    if (!this->GetScriptData(index, &data)) {
        FrameScript_ObjectError(this, "HookScript", "Doesn't have a \"%s\" script", name != nullptr ? name : "");
        return 0;
    }
    uint8_t binding = hasBinding ? static_cast<uint8_t>(lua_tointeger(L, 4)) : 1;
    uint8_t match[2] = {binding, 1};
    if (!this->HasScriptBinding(index, match)) {
        // nothing to hook: a plain handler is simply set
        bool set = false;
        if (binding == 1) {
            if (hasBinding) {
                lua_remove(L, 4);
            }
            this->SetScript(L);
            set = true;
        }
        lua_pushboolean(L, set);
        return 1;
    }
    const char* caller = lua_taint;
    FrameScript_EnterTaint(L, nullptr);
    int32_t ref = -2;
    const char* taint = nullptr;
    FindScript(this, index, binding, &ref, &taint);
    this->SetScriptRef(index, binding, 0, taint);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_settaint(L, -1, taint);
    const char* outer = lua_taint;
    FrameScript_EnterTaint(L, taint);
    lua_pushvalue(L, 3);
    lua_settaint(L, -1, caller);
    lua_pushcclosure(L, HookedHandlerCall, 2);
    lua_settaint(L, -1, taint);
    FrameScript_LeaveTaint(L, outer);
    int32_t hooked = luaL_ref(L, LUA_REGISTRYINDEX);
    this->SetScriptRef(index, binding, hooked, taint);
    FrameScript_LeaveTaint(L, caller);
    lua_pushboolean(L, 1);
    return 1;
}

// ref: FUN_1406440f0
bool FrameScript_Object::RegisterEvent(const char* name) {
    uint64_t length = *name == '\0' ? 0 : strlen(name);
    // FUN_1407bc570
    int32_t result = FrameScriptEvents::RegisterEvent(this, FrameScriptEvents::GetEventID(name, length));
    if (result != 2) {
        return result == 0;
    }
    FrameScript_ObjectError(this, "RegisterEvent", "Attempt to register unknown event \"%s\"", name);
    return false;
}

// ref: FUN_140644910
bool FrameScript_Object::UnregisterEvent(const char* name) {
    uint64_t length = *name == '\0' ? 0 : strlen(name);
    int32_t result = FrameScriptEvents::UnregisterEvent(this, FrameScriptEvents::GetEventID(name, length));
    if (result != 2) {
        return result == 0;
    }
    FrameScript_ObjectError(this, "UnregisterEvent", "Attempt to unregister unknown event \"%s\"", name);
    return false;
}

// A handler element: compiled from its body (once, the reference kept on the element), or a
// named global function, or a method of the object. An element that inherits a handler the
// object already has chains the two.
// ref: FUN_140643350
void FrameScript_Object::LoadScriptXML(lua_State* L, XMLSCRIPTHANDLER* handler, CStatus* status, int32_t own) {
    const char* elementName = handler->m_tag.m_text;
    const char* body = handler->m_body.m_text;
    FRAMESCRIPT_SCRIPTDATA data = {s_scriptWrapper, -1};
    auto index = static_cast<uint8_t>(FrameScript_GetScriptIndex(elementName));
    if (!this->GetScriptData(index, &data)) {
        status->Add(1, "Frame %s: Unknown script element %s", GetDisplayName(this), elementName);
        return;
    }
    uint8_t binding;
    if (own == 0) {
        binding = static_cast<uint8_t>(((handler->m_handlerFlags >> 12) & 1) * 2);
    } else {
        binding = 1;
    }
    bool compiled = false;
    int32_t ref = 0;
    if (handler->m_ref == nullptr && body != nullptr && *body != '\0') {
        blz::inline_string<0x1f4> where;
        ScriptLocationToString(&handler->m_location, where.header());
        char chunk[0x400];
        SStrPrintf(chunk, sizeof(chunk), "*%s_%s", OsPathGetFile(where.m_data), elementName);
        handler->m_ref = static_cast<int32_t*>(SMemAlloc(4, 1, s_sourceFile, SStrHashFnv1a(s_sourceFile), 0x272, 0));
        *handler->m_ref = FrameScript_CompileFunction(chunk, data.m_wrapper, body, status);
        compiled = true;
    }
    uint32_t flags = handler->m_handlerFlags;
    if (flags & 2) {
        // a method of the object
        if (!(this->m_flags & FSOBJ_REGISTERED)) {
            this->Register(nullptr);
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, this->m_luaRef);
        const char* method = handler->m_method.m_text;
        if (method == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, method, strlen(method));
        }
        lua_gettable(L, -2);
        lua_remove(L, -2);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
        if (ref == -1) {
            status->Add(1, "Frame %s: Unknown method %s in element %s", GetDisplayName(this), handler->m_method.m_text,
                elementName);
            ref = 0;
        }
    } else if (flags & 1) {
        // a global function, or one of the current object's environment
        const char* function = handler->m_function.m_text;
        if ((FrameXML_GetScopedState() & 4) && FrameScript_GetCurrentObject() != 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetCurrentObject());
            if (function == nullptr) {
                lua_pushnil(L);
            } else {
                lua_pushlstring(L, function, strlen(function));
            }
            lua_rawget(L, -2);
            lua_remove(L, -2);
        } else {
            if (function == nullptr) {
                lua_pushnil(L);
            } else {
                lua_pushlstring(L, function, strlen(function));
            }
            lua_rawget(L, LUA_GLOBALSINDEX);
        }
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
        if (ref == -1) {
            status->Add(1, "Frame %s: Unknown function %s in element %s", GetDisplayName(this), function, elementName);
            ref = 0;
        }
    } else if (handler->m_ref != nullptr) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, *handler->m_ref);
        if (!compiled) {
            // a body compiled for another object: only under the same environment
            lua_getfenv(L, -1);
            if ((FrameXML_GetScopedState() & 4) && FrameScript_GetCurrentObject() != 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetCurrentObject());
            } else {
                lua_pushvalue(L, LUA_GLOBALSINDEX);
            }
            if (!lua_rawequal(L, -1, -2)) {
                status->Add(2, "Frame %s: Attempt to reuse function %d with different environment",
                    GetDisplayName(this), 0);
                lua_settop(L, -4);
                return;
            }
            lua_settop(L, -3);
        }
        if ((FrameXML_GetScopedState() & 4) && FrameScript_GetCurrentObject() != 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetCurrentObject());
            lua_setfenv(L, -2);
        }
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (binding == 1 && ref != 0 && (flags & 0xc00) != 0) {
        uint8_t match[2] = {1, 1};
        if (this->HasScriptBinding(index, match)) {
            int32_t inherited = -2;
            const char* taint = nullptr;
            FindScript(this, index, 1, &inherited, &taint);
            lua_rawgeti(L, LUA_REGISTRYINDEX, inherited);
            luaL_unref(L, LUA_REGISTRYINDEX, inherited);
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (flags & 0x400) {
                lua_insert(L, -2);
            }
            lua_pushcclosure(L, ChainedHandlerCall, 2);
            const char* current = lua_taint;
            int32_t chained = luaL_ref(L, LUA_REGISTRYINDEX);
            this->SetScriptRef(index, 1, chained, current);
            return;
        }
    }
    int32_t old = this->SetScriptRef(index, binding, ref, lua_taint);
    if (old != 0) {
        FrameScript_ReleaseFunction(old);
    }
}

namespace {

// Loads a template into the object under the template's own taint.
void LoadTemplate(FrameScript_Object* object, XMLOBJECT* node, CStatus* status, const char* taint) {
    lua_State* L = FrameScript_GetContext();
    const char* previous = lua_taint;
    FrameScript_EnterTaint(L, taint);
    const char* objectTaint = lua_objectTaint;
    lua_objectTaint = taint;
    object->LoadXML(node, status, nullptr);
    lua_objectTaint = objectTaint;
    FrameScript_LeaveTaint(L, previous);
}

}

// The intrinsic the object was created as, then each template it inherits (each under its own
// taint), then mixins and key values under the scoped state the element was loaded with.
// ref: FUN_140643a20
void FrameScript_Object::LoadXML(XMLOBJECT* node, CStatus* status, const char* inherits) {
    this->PreLoadXML(node, status);
    char name[0x80];
    if (inherits != nullptr) {
        CopyString(name, inherits, sizeof(name));
        const char* taint = nullptr;
        bool busy = true;
        XMLOBJECT* intrinsic = FrameXML_AcquireIntrinsic(name, &taint, &busy);
        if (intrinsic != nullptr && !busy) {
            if (CSimpleFrame::s_loading == nullptr || taint != nullptr || CSimpleTop::s_instance->IsSecureLoad()) {
                LoadTemplate(this, intrinsic, status, taint);
            } else {
                this->LoadXML(intrinsic, status, nullptr);
            }
        }
        if (intrinsic != nullptr && !busy) {
            FrameXML_ReleaseIntrinsic(name);
        }
    }
    if (node->m_attributes & 0x100) {
        const char* cursor = node->m_inherits.m_text;
        char token[0x400];
        do {
            SStrTokenize(&cursor, token, sizeof(token), " ,", nullptr);
            if (token[0] == '\0') {
                break;
            }
            CopyString(name, token, sizeof(name));
            const char* taint = nullptr;
            bool busy = true;
            XMLOBJECT* parent = FrameXML_AcquireVirtual(name, &taint, &busy);
            if (parent != nullptr && !busy) {
                LoadTemplate(this, parent, status, taint);
            }
            if (parent != nullptr && !busy) {
                FrameXML_ReleaseVirtual(name);
            }
        } while (token[0] != '\0');
    }
    uint32_t scoped = FrameXML_GetScopedState();
    int32_t current = FrameScript_GetCurrentObject();
    bool restore = false;
    if ((node->m_flags & 0x1f) != 0 && node->m_scriptState != 0) {
        restore = true;
        FrameXML_SetScopedState(node->m_flags & 0x1f);
        FrameScript_SetCurrentObject(static_cast<int32_t>(node->m_scriptState));
    }
    if (node->m_attributes & 0x800) {
        FrameXML_ApplyMixins(this, node->m_mixin.m_text, status, false);
    }
    if (node->m_attributes & 0x1000) {
        FrameXML_ApplyMixins(this, node->m_secureMixin.m_text, status, true);
    }
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type == XMLTYPE_KEYVALUES) {
            FrameXML_LoadKeyValues(this, child, status);
        }
    }
    if (restore) {
        FrameScript_SetCurrentObject(current);
        FrameXML_SetScopedState(scoped);
    }
}

// ref: FUN_140645200
int32_t FrameScript_CallWithTaint(lua_State* L, const char* taint, int32_t function, int32_t first, int32_t count) {
    char* error = nullptr;
    if (!lua_checkstack(L, count + 1)) {
        return luaL_error(L, "stack overflow");
    }
    int32_t top = lua_gettop(L);
    lua_pushvalue(L, function);
    const char* previous = lua_taint;
    FrameScript_EnterTaint(L, taint);
    for (int32_t i = 0; i < count; i++) {
        lua_pushvalue(L, first + i);
    }
    // FUN_14063e310: lua_pcall behind an anti-tamper return-address check
    if (lua_pcall(L, count, LUA_MULTRET, 0) != 0) {
        error = SStrDupA(lua_tolstring(L, -1, nullptr), nullptr, 0);
        lua_settop(L, -2);
    }
    FrameScript_LeaveTaint(L, previous);
    if (error == nullptr) {
        return lua_gettop(L) - top;
    }
    lua_pushlstring(L, error, strlen(error));
    SMemFree(error, nullptr, 0, 0);
    return lua_error(L);
}

void FrameScript_WrapFunctionWithTaint(lua_State* L, int32_t index, const char* taint) {
    WrapFunction(L, index, taint);
}

// ref: FUN_140643f70
int32_t FrameScript_PushInteger(int32_t value) {
    lua_pushinteger(FrameScript_GetContext(), value);
    return 1;
}

// ref: FUN_140643fa0
int32_t FrameScript_PushFloat(float value) {
    lua_pushnumber(FrameScript_GetContext(), static_cast<double>(value));
    return 1;
}

// ref: FUN_140643fd0
int32_t FrameScript_PushNumber(double value) {
    lua_pushnumber(FrameScript_GetContext(), value);
    return 1;
}

// ref: FUN_140644000
int32_t FrameScript_PushString(const char* value) {
    lua_State* L = FrameScript_GetContext();
    if (value == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, value, strlen(value));
    return 1;
}

// ref: FUN_140644080
int32_t FrameScript_PushLString(const char* value, uint64_t length) {
    lua_pushlstring(FrameScript_GetContext(), value, length);
    return 1;
}

// ref: FUN_140644050
int32_t FrameScript_PushRef(int32_t ref) {
    lua_rawgeti(FrameScript_GetContext(), LUA_REGISTRYINDEX, ref);
    return 1;
}

// ref: FUN_1406440b0
int32_t FrameScript_PushBoolean(bool value) {
    lua_pushboolean(FrameScript_GetContext(), value);
    return 1;
}

// ---- mixins and key values ------------------------------------------------------------

// Copies each named mixin table's fields into the object's table (from the current object's
// environment when the scoped state says so). A secure mixin is read through its proxy, and
// its Lua functions are wrapped to run untainted with self as the object.
// ref: FUN_140668030
void FrameXML_ApplyMixins(FrameScript_Object* object, const char* mixins, CStatus* status, bool secure) {
    lua_State* L = FrameScript_GetContext();
    lua_rawgeti(L, LUA_REGISTRYINDEX, object->GetLuaRef());
    if ((FrameXML_GetScopedState() & 4) && FrameScript_GetCurrentObject() != 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetCurrentObject());
    } else {
        lua_pushvalue(L, LUA_GLOBALSINDEX);
    }
    const char* cursor = mixins;
    if (secure) {
        FrameScript_CreateSecureMixins(L, mixins, GetDisplayName(object), status);
    }
    char name[0x400];
    do {
        SStrTokenize(&cursor, name, sizeof(name), " ,", nullptr);
        if (name[0] == '\0') {
            break;
        }
        lua_pushstring(L, name);
        lua_rawget(L, -2);
        if (lua_type(L, -1) != LUA_TTABLE) {
            lua_settop(L, -2);
            status->Add(1, "%s: Has bad mixin: %s", GetDisplayName(object), name);
            break;
        }
        if (secure) {
            bool proxied = false;
            if (lua_getmetatable(L, -1) != 0) {
                lua_replace(L, -2);
                lua_pushstring(L, "__index");
                lua_rawget(L, -2);
                if (lua_type(L, -1) == LUA_TTABLE) {
                    lua_replace(L, -2);
                    proxied = true;
                }
            }
            if (!proxied) {
                lua_settop(L, -2);
                status->Add(1, "%s: Has improperly initialized secure mixin: %s", GetDisplayName(object), name);
                break;
            }
        }
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (secure && lua_taint == nullptr && lua_type(L, -1) == LUA_TFUNCTION && !lua_iscfunction(L, -1)) {
                lua_pushvalue(L, -5);
                lua_pushcclosure(L, SecureMixinCall, 2);
            }
            lua_pushvalue(L, -2);
            lua_insert(L, -3);
            lua_rawset(L, -6);
        }
        lua_settop(L, -2);
    } while (name[0] != '\0');
    lua_settop(L, -3);
}

// Pushes `value` as `type` ("nil", "boolean", "number", "global" -- a dotted path -- or a
// string), stamped with the current taint.
// ref: FUN_140668760
void FrameXML_PushTypedValue(lua_State* L, const char* type, const char* value) {
    if (_stricmp(type, "nil") == 0) {
        lua_pushnil(L);
    } else if (_stricmp(type, "boolean") == 0) {
        bool result = false;
        if (value != nullptr) {
            switch (*value) {
                case '0':
                case 'F':
                case 'N':
                case 'f':
                case 'n':
                    break;
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                case 'T':
                case 'Y':
                case 't':
                case 'y':
                    result = true;
                    break;
                default:
                    if (_stricmp(value, "off") != 0 && _stricmp(value, "disabled") != 0
                        && (_stricmp(value, "on") == 0 || _stricmp(value, "enabled") == 0)) {
                        result = true;
                    }
                    break;
            }
        }
        lua_pushboolean(L, result);
    } else if (_stricmp(type, "number") == 0) {
        lua_pushnumber(L, static_cast<double>(SStrToFloat(value)));
    } else if (_stricmp(type, "global") == 0) {
        const char* cursor = value;
        if (FrameXML_GetScopedState() & 4) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetCurrentObject());
        } else {
            lua_pushvalue(L, LUA_GLOBALSINDEX);
        }
        char field[200];
        do {
            SStrTokenize(&cursor, field, sizeof(field), ".", nullptr);
            lua_pushstring(L, field);
            lua_rawget(L, -2);
            lua_remove(L, -2);
            if (lua_type(L, -1) == LUA_TNIL) {
                break;
            }
        } while (*cursor != '\0');
    } else {
        lua_pushstring(L, value);
    }
    lua_settaint(L, -1, lua_taint);
}

// Sets each <KeyValue> as a field of the object's table.
// ref: FUN_140667c50
void FrameXML_LoadKeyValues(FrameScript_Object* object, XMLOBJECT* node, CStatus* status) {
    lua_State* L = FrameScript_GetContext();
    for (XMLOBJECT* child = node->m_child; child != nullptr; child = child->m_next) {
        if (child->m_type != XMLTYPE_KEYVALUE) {
            status->Add(1, "%s: Unknown KeyValues element %s", GetDisplayName(object), child->m_tag.m_text);
            continue;
        }
        auto keyValue = static_cast<XMLKEYVALUE*>(child);
        uint32_t flags = keyValue->m_keyValueFlags;
        if (!(flags & 1)) {
            status->Add(1, "%s: KeyValue without key", GetDisplayName(object));
            continue;
        }
        const char* key = keyValue->m_key.m_text;
        const char* keyType = (flags & 2) ? keyValue->m_keyType.m_text : "string";
        const char* valueType = (flags & 8) ? keyValue->m_valueType.m_text : "string";
        if (!(flags & 4)) {
            status->Add(1, "%s: KeyValue with key %s missing value", GetDisplayName(object), key);
            continue;
        }
        const char* value = keyValue->m_value.m_text;
        lua_rawgeti(L, LUA_REGISTRYINDEX, object->GetLuaRef());
        FrameXML_PushTypedValue(L, keyType, key);
        int32_t top;
        if (lua_type(L, -1) == LUA_TNIL) {
            status->Add(1, "%s: KeyValue with key evaluated to nil is not allowed.", GetDisplayName(object));
            top = -3;
        } else if (lua_isnumber(L, -1) && lua_tointeger(L, -1) == 0) {
            status->Add(1, "%s: KeyValue with key evaluated to 0 is not allowed.", GetDisplayName(object));
            top = -3;
        } else {
            FrameXML_PushTypedValue(L, valueType, value);
            lua_rawset(L, -3);
            top = -2;
        }
        lua_settop(L, top);
    }
}

uint32_t LocaleGetPluralRule();
uint32_t FrameXML_GetScopedState();

namespace {

// The last string FrameScript_GetText found (DAT_142e9a068).
const char* s_globalText = "";

}

// ref: FUN_141650790
int32_t FrameScript_GetPluralForm(uint32_t rule, int32_t count) {
    if (rule == 0) {
        return count != 1;
    }
    if (rule == 1) {
        return 1 < count;
    }
    if (rule == 2) {
        int32_t hundreds = count % 100;
        if (hundreds != 11 && hundreds != 12 && hundreds != 13 && hundreds != 14) {
            int32_t tens = count % 10;
            if (tens == 1) {
                return 0;
            }
            if (tens == 2 || tens == 3 || tens == 4) {
                return 1;
            }
        }
        return 2;
    }
    return 0;
}

// ref: FUN_14063cc10
bool FrameScript_GetGlobalString(const char* name, const char** value, bool scoped) {
    lua_State* L = FrameScript_GetContext();
    if (scoped && (FrameXML_GetScopedState() & 4) != 0 && FrameScript_GetCurrentObject() != 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, FrameScript_GetCurrentObject());
        if (name == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, name, strlen(name));
        }
        lua_gettable(L, -2);
        lua_remove(L, -2);
    } else {
        if (name == nullptr) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, name, strlen(name));
        }
        lua_gettable(L, LUA_GLOBALSINDEX);
    }
    bool found = lua_isstring(L, -1) != 0;
    if (found) {
        *value = lua_tostring(L, -1);
    }
    lua_settop(L, -2);
    return found;
}

// ref: FUN_140641860
const char* FrameScript_GetText(const char* name, int32_t count, int32_t gender, bool scoped) {
    s_globalText = "";
    int32_t form = FrameScript_GetPluralForm(LocaleGetPluralRule(), count);
    char plural[4] = {};
    if (static_cast<uint32_t>(form - 1) < 9) {
        plural[0] = '_';
        plural[1] = 'P';
        plural[2] = static_cast<char>('0' + form);
    }
    const char* female = gender == 3 ? "_FEMALE" : "";
    std::string buffer;
    const char* key = nullptr;
    auto build = [&](const char* first, const char* second) {
        buffer.assign(name);
        buffer.append(first);
        buffer.append(second);
        key = buffer.c_str();
    };
    build(plural, female);
    if (FrameScript_GetGlobalString(key, &s_globalText, scoped)) {
        return s_globalText;
    }
    build(plural, "");
    if (FrameScript_GetGlobalString(key, &s_globalText, scoped)) {
        return s_globalText;
    }
    build("", female);
    if (FrameScript_GetGlobalString(key, &s_globalText, scoped)) {
        return s_globalText;
    }
    FrameScript_GetGlobalString(name, &s_globalText, scoped);
    return s_globalText;
}
