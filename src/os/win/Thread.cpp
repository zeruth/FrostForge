// The packages' thread layer (0x140263xxx..0x140268xxx): threads with a start handshake, a
// parking lot (wait until a predicate holds, woken by address), a nanosecond clock and the
// processor topology.

#include "os/Os.hpp"
#include <cstring>
#include <intrin.h>
#include <malloc.h>
#include <process.h>
#include <windows.h>

namespace {

// DAT_142cc8b40: the parking lot - 128 buckets, each a lock and a list of waiters, picked by
// a hash of the address waited on.
struct PARKWAITER {
    PARKWAITER* m_next;
    PARKWAITER* m_prev;
    const void* m_address;
    bool (*m_predicate)(void* context);
    void* m_context;
    CONDITION_VARIABLE* m_condition;
};

struct PARKBUCKET {
    SRWLOCK m_lock = SRWLOCK_INIT;
    PARKWAITER m_head;
};

PARKBUCKET s_park[128];

PARKBUCKET* BucketOf(const void* address) {
    uint64_t h = reinterpret_cast<uint64_t>(address);
    h = (h >> 33 ^ h) * 0xFF51AFD7ED558CCDull;
    h = (h >> 33 ^ h) * 0xC4CEB9FE1A85EC53ull;
    PARKBUCKET* bucket = &s_park[(static_cast<uint32_t>(h >> 33) ^ static_cast<uint32_t>(h)) & 0x7F];
    if (!bucket->m_head.m_next) {
        bucket->m_head.m_next = &bucket->m_head;
        bucket->m_head.m_prev = &bucket->m_head;
    }
    return bucket;
}

thread_local CONDITION_VARIABLE t_parkCondition = CONDITION_VARIABLE_INIT;

// DAT_142620900: the Win32 priority for each OS_THREAD_PRIORITY level.
const int32_t s_threadPriorities[] = {
    0,
    THREAD_PRIORITY_IDLE,
    THREAD_PRIORITY_LOWEST,
    THREAD_PRIORITY_BELOW_NORMAL,
    THREAD_PRIORITY_NORMAL,
    THREAD_PRIORITY_ABOVE_NORMAL,
    THREAD_PRIORITY_HIGHEST,
    THREAD_PRIORITY_TIME_CRITICAL,
};

struct THREADSTART {
    OSTHREAD** m_thread;
    bool (*m_test)(void* value);
};

// ref: 0x140263220
bool IsZero(void* value) {
    return value == nullptr;
}

// ref: 0x1402631d0
bool ThreadStarted(void* context) {
    auto start = static_cast<THREADSTART*>(context);
    return start->m_test((*start->m_thread)->m_function ? reinterpret_cast<void*>(1) : nullptr);
}

// The trampoline: takes the function, clears the slot (the start handshake) and runs it.
// ref: FUN_140263340
unsigned __stdcall ThreadEntry(void* parameter) {
    auto thread = static_cast<OSTHREAD*>(parameter);
    void* argument = thread->m_argument;
    auto function = thread->m_function;
    thread->m_function = nullptr;
    OsUnpark(&thread->m_function, true);
    function(argument);
    return 0;
}

}

// ref: FUN_140266c30
int64_t OsGetNanoseconds() {
    static int64_t s_frequency = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
    uint64_t f = static_cast<uint64_t>(s_frequency);
    return static_cast<int64_t>((ticks % f) * 1000000000 / f + (ticks / f) * 1000000000);
}

// Waits until `predicate(context)` holds, parked on `address` between checks (woken by
// OsUnpark); it spins 40 checks first. timeoutNs INT64_MAX waits forever. True when the
// predicate held or the waiter was woken, false on timeout.
// ref: FUN_140268930
bool OsParkWait(const void* address, bool (*predicate)(void* context), void* context, int64_t timeoutNs) {
    int64_t deadline = 0;
    if (timeoutNs != INT64_MAX) {
        deadline = timeoutNs + OsGetNanoseconds();
    }
    for (int32_t i = 0; i < 0x28; i++) {
        if (predicate(context)) {
            return true;
        }
        if (timeoutNs != INT64_MAX && deadline - OsGetNanoseconds() <= 0) {
            return false;
        }
    }

    PARKBUCKET* bucket = BucketOf(address);
    PARKWAITER waiter;
    waiter.m_address = address;
    waiter.m_predicate = predicate;
    waiter.m_context = context;
    waiter.m_condition = &t_parkCondition;
    AcquireSRWLockExclusive(&bucket->m_lock);
    waiter.m_prev = bucket->m_head.m_prev;
    waiter.m_next = &bucket->m_head;
    waiter.m_prev->m_next = &waiter;
    bucket->m_head.m_prev = &waiter;

    bool held = predicate(context);
    for (;;) {
        if (held) {
            waiter.m_prev->m_next = waiter.m_next;
            waiter.m_next->m_prev = waiter.m_prev;
            ReleaseSRWLockExclusive(&bucket->m_lock);
            return true;
        }
        if (timeoutNs == INT64_MAX) {
            SleepConditionVariableSRW(&t_parkCondition, &bucket->m_lock, INFINITE, 0);
        } else {
            int64_t left = deadline - OsGetNanoseconds();
            if (left < 1) {
                waiter.m_prev->m_next = waiter.m_next;
                waiter.m_next->m_prev = waiter.m_prev;
                ReleaseSRWLockExclusive(&bucket->m_lock);
                return false;
            }
            int64_t ms = left / 1000000;
            SleepConditionVariableSRW(&t_parkCondition, &bucket->m_lock, ms > 0xFFFFFFFF ? 0xFFFFFFFF : DWORD(ms), 0);
        }
        if (waiter.m_next == &waiter) {
            // woken: OsUnpark took us off the list
            ReleaseSRWLockExclusive(&bucket->m_lock);
            return true;
        }
        held = predicate(context);
    }
}

// Wakes the waiters parked on `address` whose predicate now holds (all of them, or the
// first). True when one was woken.
// ref: FUN_140268820
bool OsUnpark(const void* address, bool all) {
    PARKBUCKET* bucket = BucketOf(address);
    bool woken = false;
    AcquireSRWLockExclusive(&bucket->m_lock);
    PARKWAITER* node = bucket->m_head.m_next;
    while (node != &bucket->m_head) {
        PARKWAITER* next = node->m_next;
        if (node->m_address == address && node->m_predicate(node->m_context)) {
            woken = true;
            node->m_next->m_prev = node->m_prev;
            node->m_prev->m_next = node->m_next;
            node->m_next = node;
            node->m_prev = node;
            WakeConditionVariable(node->m_condition);
            if (!all) {
                break;
            }
        }
        node = next;
    }
    ReleaseSRWLockExclusive(&bucket->m_lock);
    return woken;
}

// Starts a thread running function(argument): created suspended, then resumed once the
// function is stored; the thread clears the slot when it picks the function up.
// ref: FUN_140263380
uint32_t OsThreadCreate(OSTHREAD* thread, void (*function)(void* argument), void* argument, uint32_t stackSize) {
    uintptr_t handle = _beginthreadex(nullptr, stackSize, &ThreadEntry, thread, CREATE_SUSPENDED, nullptr);
    if (handle == 0) {
        return static_cast<uint32_t>(*__doserrno());
    }
    thread->m_handle = reinterpret_cast<void*>(handle);
    OSTHREAD* self = thread;
    THREADSTART start = { &self, &IsZero };
    OsParkWait(&thread->m_function, &ThreadStarted, &start, INT64_MAX);
    thread->m_function = function;
    thread->m_argument = argument;
    ResumeThread(thread->m_handle);
    return 0;
}

// Waits until the thread has picked up its function.
// ref: FUN_1402631e0
void OsThreadWaitStarted(OSTHREAD* thread) {
    OSTHREAD* self = thread;
    THREADSTART start = { &self, &IsZero };
    OsParkWait(&thread->m_function, &ThreadStarted, &start, INT64_MAX);
}

// ref: FUN_140263490
void OsThreadJoin(OSTHREAD* thread) {
    WaitForSingleObject(thread->m_handle, INFINITE);
    CloseHandle(thread->m_handle);
    thread->m_handle = nullptr;
}

// ref: FUN_1402632b0 (reached through 0x140263660)
uint32_t OsThreadSetAffinity(OSTHREAD* thread, uint64_t mask) {
    DWORD_PTR process;
    DWORD_PTR system;
    uint64_t allowed = ~uint64_t(0);
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) {
        allowed = process;
    }
    if ((mask & allowed) == 0) {
        return OsErrorFromCode(2);
    }
    if (SetThreadAffinityMask(thread->m_handle, mask & allowed)) {
        return 0;
    }
    return GetLastError();
}

// Level 0 copies the calling thread's priority.
// ref: FUN_140263670
void OsThreadSetPriority(OSTHREAD* thread, int32_t level) {
    if (level == 0) {
        SetThreadPriority(thread->m_handle, GetThreadPriority(GetCurrentThread()));
        return;
    }
    SetThreadPriority(thread->m_handle, s_threadPriorities[level]);
}

namespace {

// The debugger's thread-naming exception (0x406d1388), raised only under a debugger.
// ref: FUN_140263230
void NameThreadForDebugger(const char* name) {
    if (!IsDebuggerPresent()) {
        return;
    }
#pragma pack(push, 8)
    struct {
        DWORD m_type;
        LPCSTR m_name;
        DWORD m_threadId;
        DWORD m_flags;
    } info = { 0x1000, name, 0xFFFFFFFF, 0 };
#pragma pack(pop)
    __try {
        RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), reinterpret_cast<ULONG_PTR*>(&info));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}

// Names the calling thread (SetThreadDescription when the system has it, and the debugger
// exception).
// ref: FUN_1402634e0
void OsThreadSetName(const char* name) {
    typedef HRESULT(WINAPI * SETDESCRIPTION)(HANDLE thread, PCWSTR description);
    static SETDESCRIPTION s_setDescription = reinterpret_cast<SETDESCRIPTION>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (s_setDescription) {
        uint64_t length = std::strlen(name);
        auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 2 + 0xF) & ~uint64_t(0xF)));
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 0x3FFFFFFF);
        s_setDescription(GetCurrentThread(), wide);
    }
    NameThreadForDebugger(name);
}

// The processor topology: packages, cores and logical processors (each at least 1).
// ref: FUN_140265a30
void OsGetProcessorTopology(OSCPUINFO* info) {
    DWORD length = 0;
    if (!GetLogicalProcessorInformationEx(RelationProcessorPackage, nullptr, &length)
        && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        auto buffer = static_cast<uint8_t*>(_alloca((length + 0xF) & ~DWORD(0xF)));
        if (GetLogicalProcessorInformationEx(RelationProcessorPackage,
                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer), &length)) {
            info->m_packages = 0;
            for (uint8_t* p = buffer; length > 7;) {
                auto entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (length < entry->Size) {
                    break;
                }
                if (entry->Size > 0x2F) {
                    info->m_packages++;
                }
                length -= entry->Size;
                p += entry->Size;
            }
        }
    }

    length = 0;
    bool first = true;
    uint8_t efficiency = 0;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length)
        && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        auto buffer = static_cast<uint8_t*>(_alloca((length + 0xF) & ~DWORD(0xF)));
        std::memset(buffer, 0, length);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer), &length)) {
            info->m_cores = 0;
            info->m_logical = 0;
            for (uint8_t* p = buffer; length > 7;) {
                auto entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(p);
                if (length < entry->Size) {
                    break;
                }
                if (entry->Size > 0x2F) {
                    info->m_cores++;
                    for (uint32_t g = 0; g < entry->Processor.GroupCount; g++) {
                        if (first) {
                            first = false;
                            efficiency = entry->Processor.EfficiencyClass;
                        } else if (entry->Processor.EfficiencyClass != efficiency) {
                            info->m_flags2A |= 2;  // mixed efficiency classes (hybrid CPU)
                        }
                        if (uint64_t(0x20 + g * 0x10 + 0x10) <= entry->Size) {
                            info->m_logical += static_cast<uint32_t>(__popcnt64(entry->Processor.GroupMask[g].Mask));
                        }
                    }
                }
                length -= entry->Size;
                p += entry->Size;
            }
        }
    }
    if (info->m_packages == 0) {
        info->m_packages = 1;
    }
    if (info->m_cores == 0) {
        info->m_cores = 1;
    }
    if (info->m_logical == 0) {
        info->m_logical = 1;
    }
}

// The process-wide CPU description, filled once. The feature detection (cpuid, 0x1402652c0)
// and the architecture (0x140265790) are not ported: only the topology.
// ref: FUN_140265ca0
const OSCPUINFO* OsGetCpuInfo() {
    static OSCPUINFO s_info = [] {
        OSCPUINFO info = {};
        OsGetProcessorTopology(&info);
        return info;
    }();
    return &s_info;
}

// ref: FUN_1412394b0
int64_t OsGetElapsedNanoseconds() {
    static int64_t s_base;      // DAT_1430a1540
    static double s_scale;      // DAT_1430a1548
    if (s_base == 0) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        s_scale = 1000000000.0 / static_cast<double>(frequency.QuadPart);
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        s_base = counter.QuadPart;
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    uint64_t ticks = static_cast<uint64_t>(counter.QuadPart - s_base);
    if (static_cast<int64_t>(ticks) >= 0) {
        return static_cast<int64_t>(static_cast<double>(static_cast<int64_t>(ticks)) * s_scale);
    }
    return static_cast<int64_t>(static_cast<double>(ticks) * s_scale);
}
