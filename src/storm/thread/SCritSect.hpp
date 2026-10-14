#ifndef STORM_THREAD_SCRITSECT_HPP
#define STORM_THREAD_SCRITSECT_HPP

#include <windows.h>

// A CRITICAL_SECTION behind out-of-line wrappers. Objects of this type are initialized and
// deleted explicitly by their owner (no constructor or destructor runs).
class SCritSect {
    public:
    CRITICAL_SECTION m_opaqueData;

    bool Initialize();
    void Destroy();
    void Enter();
    void Leave();
};

#endif
