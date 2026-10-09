#include "base/Handle.hpp"

#include "storm/thread/SCritSect.hpp"

namespace {

// The lock every handle's count changes under, made by a static initializer (0x14020bae0).
SCritSect& HandleLock() {
    static SCritSect* s_lock = [] {
        static SCritSect lock;
        lock.Initialize();
        return &lock;
    }();
    return *s_lock;
}

}

// ref: FUN_142431130
CHandleObject* HandleDuplicate(CHandleObject* handle) {
    HandleLock().Enter();
    handle->m_refcount++;
    HandleLock().Leave();
    return handle;
}

// ref: FUN_1424310f0
void HandleClose(CHandleObject* handle) {
    if (handle == nullptr) {
        return;
    }
    HandleLock().Enter();
    if (--handle->m_refcount == 0) {
        delete handle;
    }
    HandleLock().Leave();
}
