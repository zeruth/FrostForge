#include "tact/AsyncState.hpp"
#include <windows.h>

namespace tact {

namespace {

const char* const s_errorStrings[] = {
    "success",
    "unknown failure",
    "operation cancelled",
    "invalid parameter",
    "buffer too small",
    "not supported",
    "not found",
    "not available",
    "data error",
    "already exists",
    "disk full",
    "needs repair",
    "operation would block",
    "missing encryption key",
    "results are truncated",
    "invalid drive type",
    "container locked",
    "cdn hosts exhausted",
    "needs update",
    "would not pipeline",
    "bad armadillo key",
    "insufficient quality",
    "path too long",
};

}

// The table at 0x142d3e5c0 is indexed unchecked.
// ref: FUN_1420d9030
const char* ErrorString(uint8_t error) {
    return s_errorStrings[error];
}

// Spins 1000 times, then yields with Sleep(0) and allows 100000 more spins before the next yield.
// ref: FUN_1420d4540
int64_t* SpinLockExclusive(int64_t* lock) {
    int64_t self = static_cast<int64_t>(GetCurrentThreadId()) + 1;
    int64_t seen = InterlockedCompareExchange64(lock, self, 0);
    if (seen == 0) {
        return lock;
    }
    if (seen == self) {
        return nullptr;
    }
    int32_t spins = 1000;
    while (true) {
        int32_t previous = spins;
        if (InterlockedCompareExchange64(lock, self, 0) == 0) {
            break;
        }
        spins = previous - 1;
        if (spins < 1) {
            Sleep(0);
            spins = previous + 99999;
        }
    }
    return lock;
}

// ref: FUN_1420d44a0
int64_t* SpinLockShared(int64_t* lock) {
    int64_t seen = InterlockedCompareExchange64(lock, -1, 0);
    if (seen == 0) {
        return lock;
    }
    if (seen > 0) {
        if (seen == static_cast<int64_t>(GetCurrentThreadId()) + 1) {
            return nullptr;
        }
        goto wait_for_writer;
    }
    while (true) {
        // Readers hold it: join them.
        while (true) {
            int64_t current = InterlockedCompareExchange64(lock, seen - 1, seen);
            if (current == seen) {
                return lock;
            }
            seen = current;
            if (seen >= 1) {
                break;
            }
        }
    wait_for_writer: {
        int32_t spins = 1000;
        do {
            int32_t next = spins - 1;
            if (next < 1) {
                Sleep(0);
                next = spins + 99999;
            }
            spins = next;
            seen = InterlockedCompareExchange64(lock, -1, 0);
            if (seen == 0) {
                return lock;
            }
        } while (seen > 0);
    }
    }
}

// ref: FUN_1420dbc20
AsyncState::AsyncState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context) {
    this->m_instance = instance;
    this->m_refCount = 0;
    this->m_context = context;
    this->m_opId = opId;
    this->m_callback = callback;
    this->m_result = ERR_UNKNOWN;
    this->m_pending = 1;
    this->m_cancelled = 0;
    this->m_lock = 0;
}

// ref: FUN_1420d4160
AsyncState::~AsyncState() {
}

// The base's +0x18 is the shared `return 0`.
// ref: FUN_14022c3f0
int32_t AsyncState::GetDetail() {
    return 0;
}

void AsyncState::OnStart() {
}

void AsyncState::OnCancel() {
}

void AsyncState::AddRef() {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&this->m_refCount));
}

void AsyncState::Release() {
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(&this->m_refCount)) == 0) {
        this->Destroy();
    }
}

// ref: FUN_1420dbc70
void AsyncState::Complete(uint8_t result) {
    this->m_result = result;
    this->m_pending = 0;
    if (this->m_callback) {
        this->m_callback(this->m_context, this->m_instance, this->m_opId);
    }
}

// ref: FUN_1420dbc60
void AsyncState::Cancel() {
    this->m_cancelled = 1;
    this->OnCancel();
}

AsyncStateScope::AsyncStateScope(AsyncState* state) {
    this->m_state = state;
    this->m_lock = SpinLockExclusive(&state->m_lock);
    if (state) {
        state->AddRef();
    }
}

AsyncStateScope::~AsyncStateScope() {
    if (this->m_lock) {
        *this->m_lock = 0;
    }
    if (this->m_state) {
        this->m_state->Release();
    }
}

}
