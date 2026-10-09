#ifndef UI_C_SCRIPT_OBJECT_HPP
#define UI_C_SCRIPT_OBJECT_HPP

// CScriptObject (Engine/Source/Frame/CScriptObject.cpp): a FrameScript object with a name, a
// place in a parent hierarchy, and the "$parent" naming and parentKey lookups the XML uses
// (vtable 0x1426cb4a0, 0x18 bytes).

#include <cstdint>

#include "base/StringAllocator.hpp"
#include "blz/String.hpp"
#include "framescript/FrameScriptObject.hpp"

class CAnimatableObject;

// Receives the lines DumpHierarchy writes: the text, its length, the caller's context.
typedef void (*HIERARCHYDUMPFUNC)(const char* text, int32_t length, void* context);

class CScriptObject : public FrameScript_Object {
    public:
    // Where object names are kept (0x142e99c30); cleared when the UI root goes.
    static CStringAllocator s_nameAllocator;

    const char* m_name = nullptr;   // 0x10

    CScriptObject();
    ~CScriptObject() override;
    const char* GetName() override;
    bool IsObjectTypeName(const char* name) override;
    bool IsObjectType(int32_t type) override;
    const char* GetObjectTypeName() override;
    uint32_t GetObjectSize() override;
    // The file and line the object was defined at, as text.
    virtual void GetSourceLocation(blz::string_header* text);
    virtual CScriptObject* GetParent() = 0;
    // The object a dotted path ("$parent.Child.key") leads to from this one.
    virtual CScriptObject* GetObjectByPath(const char* path);
    // The animation host of the object, if it can animate.
    virtual CAnimatableObject* GetAnimatableObject();
    // Writes a line for the object (indented by `indent` dots) and each object under it.
    virtual void DumpHierarchy(uint32_t indent, HIERARCHYDUMPFUNC dump, void* context);
    virtual void Unknown98(void* argument);

    // This class's type id (0x142e97a9c).
    static int32_t GetObjectType();

    // A name with "$parent" replaced by the nearest named ancestor's name ("Top" for none).
    void CreateName(const char* name, char* buffer, uint32_t size);
    // Names the object (and its global); an empty name unnames it.
    void SetName(const char* name);
    // Makes the object `key` in its parent's table.
    void SetParentKey(const char* key);
    // The key its parent's table holds it under (an upper-case one preferred), or null.
    const char* GetParentKey();
    // A readable path for messages: "Parent.key", or the address when nothing better exists;
    // `name` is raw storage the string is constructed in.
    blz::string* GetDebugName(blz::string* name, bool shortest);
    // Locked down or otherwise not visible to scripts.
    bool IsInaccessible();
};
static_assert(sizeof(CScriptObject) == 0x18, "CScriptObject is 0x18 bytes");

// Whether the UI's script state is going away (the glue or game UI is not running), so objects
// leave their globals alone as they are destroyed.
// ref: FUN_14196fed0
bool ClientUI_IsTearingDown();

#endif
