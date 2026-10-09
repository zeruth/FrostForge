#ifndef BASE_HANDLE_HPP
#define BASE_HANDLE_HPP

// Reference-counted handle objects (HFACE and the like): a vtable and a count, changed under
// one lock (DAT_1447aa218); the last close deletes the object.

#include <cstdint>

class CHandleObject {
    public:
    virtual ~CHandleObject() = default;
    int32_t m_refcount = 0;     // 0x08
};

// Another reference to a handle (FUN_142431130).
CHandleObject* HandleDuplicate(CHandleObject* handle);
// Drops a reference, deleting the object with the last (FUN_1424310f0).
void HandleClose(CHandleObject* handle);

#endif
