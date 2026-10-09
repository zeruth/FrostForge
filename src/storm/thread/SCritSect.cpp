#include "storm/thread/SCritSect.hpp"

// ref: FUN_1402637c0
bool SCritSect::Initialize() {
    InitializeCriticalSection(&this->m_opaqueData);
    return true;
}

// ref: FUN_1402637b0
void SCritSect::Enter() {
    EnterCriticalSection(&this->m_opaqueData);
}

// ref: FUN_1402637f0
void SCritSect::Leave() {
    LeaveCriticalSection(&this->m_opaqueData);
}
