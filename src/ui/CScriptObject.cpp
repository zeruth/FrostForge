#include "ui/CScriptObject.hpp"

#include <cstring>
#include <new>

#include "framescript/FrameScript.hpp"
#include "framescript/Taint.hpp"
#include "storm/String.hpp"

extern "C" {
#include "lua.h"
}

CStringAllocator CScriptObject::s_nameAllocator;

namespace {

// The bounded copy the reference inlines: at most size - 1 characters, always terminated.
void CopyBounded(char* dest, const char* source, uint64_t size) {
    if (size == 0) {
        return;
    }
    for (uint64_t left = size;;) {
        if (--left == 0) {
            *dest = '\0';
            return;
        }
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
    }
}

}

// ref: FUN_14062bb80
CScriptObject::CScriptObject() {
    this->m_name = nullptr;
}

// ref: FUN_14062bbb0
CScriptObject::~CScriptObject() {
    if (!ClientUI_IsTearingDown()) {
        this->Unregister(this->m_name);
    }
}

// ref: FUN_140607470
const char* CScriptObject::GetName() {
    return this->m_name;
}

// ref: FUN_140607f90
bool CScriptObject::IsObjectTypeName(const char* name) {
    return _stricmp(name, "Object") == 0 || _stricmp(name, "FrameScript_Object") == 0;
}

// ref: FUN_140607f00
bool CScriptObject::IsObjectType(int32_t type) {
    return type == CScriptObject::GetObjectType() || type == FrameScript_Object::GetObjectType();
}

// ref: FUN_14062c230
const char* CScriptObject::GetObjectTypeName() {
    return "Object";
}

// ref: FUN_14062c220
uint32_t CScriptObject::GetObjectSize() {
    return 0x18;
}

// ref: FUN_14062c620
void CScriptObject::GetSourceLocation(blz::string_header* text) {
    blz::string_assign(text, "UnknownFile", 11);
}

// ref: FUN_140607480
int32_t CScriptObject::GetObjectType() {
    // 0x142e97a9c, numbered on first use
    static int32_t s_type = ++FrameScript_Object::s_objectTypeCount;
    return s_type;
}

// Each '.'-separated step is "$parent" or a key of the current object's table holding a
// scripted object; a missing key stops at the object reached so far.
// ref: FUN_14062c450
CScriptObject* CScriptObject::GetObjectByPath(const char* path) {
    if (path == nullptr || *path == '\0') {
        return nullptr;
    }
    const char separator[2] = {'.', '\0'};
    const char* cursor = path;
    lua_State* L = FrameScript_GetContext();
    char token[0x400];
    SStrTokenize(&cursor, token, sizeof(token), separator, nullptr);
    CScriptObject* object = this;
    while (true) {
        if (token[0] == '\0') {
            return object;
        }
        CScriptObject* next;
        if (_strnicmp(token, "$parent", 7) == 0) {
            next = object->GetParent();
            SStrTokenize(&cursor, token, sizeof(token), separator, nullptr);
            if (next == nullptr) {
                return nullptr;
            }
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, object->GetLuaRef());
            lua_pushstring(L, token);
            lua_rawget(L, -2);
            next = static_cast<CScriptObject*>(
                FrameScript_Object::GetObjectFromLua(nullptr, L, CScriptObject::GetObjectType(), -1, true, true));
            lua_settop(L, -3);
            if (next == nullptr) {
                return object;
            }
            SStrTokenize(&cursor, token, sizeof(token), separator, nullptr);
        }
        object = next;
        bool reachable = !(next->m_flags & FSOBJ_LOCKDOWN) && (next->m_flags & FSOBJ_ACCESSIBLE);
        if (!reachable && !this->IsInaccessible()) {
            return nullptr;
        }
    }
}

void* CScriptObject::Unknown88() {
    return nullptr;
}

void CScriptObject::Unknown90() {
}

void CScriptObject::Unknown98() {
}

// ref: FUN_14062bc50
void CScriptObject::CreateName(const char* name, char* buffer, uint32_t size) {
    if (_strnicmp(name, "$parent", 7) != 0) {
        CopyBounded(buffer, name, size);
        return;
    }
    CopyBounded(buffer, "Top", size);
    for (CScriptObject* parent = this->GetParent(); parent != nullptr; parent = parent->GetParent()) {
        const char* parentName = parent->GetName();
        if (parentName != nullptr && *parentName != '\0') {
            CopyBounded(buffer, parentName, size);
            break;
        }
    }
    // the rest of the name after "$parent", appended within the buffer
    uint64_t length = 0;
    while (length < size && buffer[length] != '\0') {
        length++;
    }
    uint64_t left = size - length;
    if (left == 0) {
        return;
    }
    char* dest = buffer + length;
    for (const char* rest = name + 7; *rest != '\0'; rest++) {
        if (left != 1) {
            *dest++ = *rest;
            left--;
        }
    }
    *dest = '\0';
}

// ref: FUN_14062c6c0
void CScriptObject::SetName(const char* name) {
    this->Unregister(this->m_name);
    const char* interned = nullptr;
    this->m_name = nullptr;
    if (name != nullptr && *name != '\0') {
        char buffer[0x400];
        this->CreateName(name, buffer, sizeof(buffer));
        interned = s_nameAllocator.Allocate(buffer);
        this->m_name = interned;
    }
    this->Register(interned);
}

// ref: FUN_14062c740
void CScriptObject::SetParentKey(const char* key) {
    lua_State* L = FrameScript_GetContext();
    CScriptObject* parent = this->GetParent();
    if (parent == nullptr) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, parent->GetLuaRef());
    lua_pushstring(L, key);
    lua_rawgeti(L, LUA_REGISTRYINDEX, this->GetLuaRef());
    lua_settable(L, -3);
    lua_settop(L, -2);
}

// ref: FUN_14062c240
const char* CScriptObject::GetParentKey() {
    CScriptObject* parent = this->GetParent();
    if (parent == nullptr || !(this->m_flags & FSOBJ_ACCESSIBLE) || (this->m_flags & FSOBJ_HIDDEN)) {
        return nullptr;
    }
    const char* found = nullptr;
    lua_State* L = FrameScript_GetContext();
    const char* previous = lua_taint;
    FrameScript_EnterTaint(L, nullptr);
    lua_rawgeti(L, LUA_REGISTRYINDEX, this->GetLuaRef());
    lua_rawgeti(L, LUA_REGISTRYINDEX, parent->GetLuaRef());
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        int32_t keyType = lua_type(L, -2);
        if (lua_rawequal(L, -1, -4) && keyType == LUA_TSTRING) {
            const char* key = lua_tolstring(L, -2, nullptr);
            if (found == nullptr
                || (static_cast<uint8_t>(*key - 'A') < 26 && static_cast<uint8_t>(*found - 'A') > 25)) {
                found = key;
            }
        }
        lua_settop(L, -2);
    }
    lua_settop(L, -3);
    FrameScript_LeaveTaint(L, previous);
    return found;
}

// ref: FUN_14062bde0
blz::string* CScriptObject::GetDebugName(blz::string* name, bool shortest) {
    const char* ownName = this->GetName();
    CScriptObject* parent = this->GetParent();
    if (ownName != nullptr && (!shortest || parent == nullptr)) {
        new (name) blz::string();
        name->construct(ownName, *ownName != '\0' ? std::strlen(ownName) : 0);
        return name;
    }
    const char* key = this->GetParentKey();
    blz::inline_string<0x20> address;
    const char* part = key;
    if (key == nullptr) {
        if (!(this->m_flags & FSOBJ_ACCESSIBLE) || (this->m_flags & FSOBJ_HIDDEN)) {
            blz::string_assign(address.header(), "inaccessible", 12);
        } else if (!this->IsRegistered()) {
            blz::string_format(address.header(), "%llx", this);
        } else {
            lua_State* L = FrameScript_GetContext();
            const char* previous = lua_taint;
            FrameScript_EnterTaint(L, nullptr);
            lua_rawgeti(L, LUA_REGISTRYINDEX, this->GetRawLuaRef());
            blz::string_format(address.header(), "%llx", lua_topointer(L, -1));
            lua_settop(L, -2);
            FrameScript_LeaveTaint(L, previous);
        }
        part = address.m_data;
    }
    if (parent != nullptr) {
        alignas(blz::string) uint8_t storage[sizeof(blz::string)];
        auto parentName = reinterpret_cast<blz::string*>(storage);
        parent->GetDebugName(parentName, shortest);
        if (parentName->m_size != 0) {
            const char* format = "%s.%s";
            if (key == nullptr && ownName != nullptr) {
                format = "%s, %s";
                part = ownName;
            }
            new (name) blz::string();
            blz::string_format(reinterpret_cast<blz::string_header*>(name), format, parentName->m_data, part);
            parentName->~string();
            return name;
        }
        parentName->~string();
    }
    new (name) blz::string();
    return name;
}

// ref: FUN_14062c6a0
bool CScriptObject::IsInaccessible() {
    return (this->m_flags & FSOBJ_LOCKDOWN) || !(this->m_flags & FSOBJ_ACCESSIBLE);
}
