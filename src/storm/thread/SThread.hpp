#ifndef STORM_THREAD_STHREAD_HPP
#define STORM_THREAD_STHREAD_HPP

// Storm/Source/W32/SThread.cpp: named threads, tracked in a process-wide list while they
// run, and the small synchronization-object wrappers around their Win32 handles.

#include <windows.h>
#include <cstdint>

typedef uint32_t (*STHREADPROC)(void* param);

// _beginthreadex with the thread named (for the debugger and SetThreadDescription) and tracked
// while it runs. `reserved` is not used.
uintptr_t SCreateThread(STHREADPROC proc, void* param, uint32_t* threadId, void* reserved, const char* name,
    uint32_t stackSize);

// A Win32 handle that is closed with its owner.
class SSyncObject {
    public:
    HANDLE m_opaqueData;

    SSyncObject();
    void Close();
    uint32_t Wait(uint32_t timeout);
};

class SEvent : public SSyncObject {
    public:
    SEvent(bool manualReset, bool initialState);
    void Set();
    void Reset();
};

class SThread : public SSyncObject {
    public:
    // The name is dropped on the way down (the reference passes null for it), so these
    // threads all run unnamed.
    static bool Create(STHREADPROC proc, void* param, SThread* thread, const char* name, uint32_t stackSize);
};

#endif
