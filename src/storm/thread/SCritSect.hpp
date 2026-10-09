#ifndef STORM_THREAD_SCRITSECT_HPP
#define STORM_THREAD_SCRITSECT_HPP

#include <windows.h>

// A CRITICAL_SECTION behind three out-of-line wrappers. Objects of this type are statics that
// their owner initializes explicitly (no constructor runs).
class SCritSect {
    public:
    CRITICAL_SECTION m_opaqueData;

    bool Initialize();
    void Enter();
    void Leave();
};

#endif
