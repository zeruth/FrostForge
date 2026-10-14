// The packages' thread layer (0x140263xxx..0x140268xxx): threads with a start handshake, a
// parking lot (wait until a predicate holds, woken by address), a nanosecond clock and the
// processor topology.

#include "os/Os.hpp"
#include <cstdlib>
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

// ref: FUN_140266c80
int64_t OsGetPerformanceCounter() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

// ref: FUN_140266bf0
int64_t OsTicksToNanoseconds(uint64_t ticks) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);  // PTR_FUN_142cc8ad0
    uint64_t f = static_cast<uint64_t>(frequency.QuadPart);
    return static_cast<int64_t>((ticks % f) * 1000000000 / f + (ticks / f) * 1000000000);
}

// ref: FUN_140263700
void OsSleepPrecise(int64_t nanoseconds) {
    int64_t start = OsTicksToNanoseconds(OsGetPerformanceCounter());
    uint64_t milliseconds = static_cast<uint64_t>(nanoseconds / 1000000);
    if ((milliseconds & 0xffffffffull) != milliseconds) {
        Sleep(INFINITE);
        return;
    }
    if (static_cast<uint32_t>(milliseconds) > 1) {
        Sleep(static_cast<uint32_t>(milliseconds) - 1);
    }
    while (OsTicksToNanoseconds(OsGetPerformanceCounter()) - start < nanoseconds) {
    }
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

namespace {

// A 64-bit RDRAND value, retried up to five times; false when none came.
bool ReadRandom(uint64_t* value) {
    for (int32_t attempt = 0; attempt < 5; attempt++) {
        unsigned long long out;
        if (_rdrand64_step(&out) != 0) {
            *value = out;
            return true;
        }
    }
    return false;
}

// Whether RDRAND works: three more values that all repeat the first mean the instruction is
// broken (some AMD parts return the same value every time).
bool RandomWorks() {
    uint64_t first;
    if (!ReadRandom(&first)) {
        return true;
    }
    for (int32_t i = 0; i < 3; i++) {
        uint64_t next;
        if (!ReadRandom(&next) || next != first) {
            return true;
        }
    }
    return false;
}

}

// The processor's features from CPUID into the flag bytes at 0x20..0x25 and 0x2a (SSE through
// AVX-512, the bit-manipulation, crypto and RDRAND extensions, AVX only where the OS saves the
// registers), its vendor (0x2b), its rated clock (leaf 0x16), its brand string (0x6b) and, under a
// hypervisor, the hypervisor's vendor (0xab).
// ref: FUN_1402652c0
void OsCpuDetectFeatures(OSCPUINFO* info) {
    auto bytes = reinterpret_cast<uint8_t*>(info);
    int regs[4];
    __cpuid(regs, 0);
    uint32_t maxLeaf = static_cast<uint32_t>(regs[0]);
    std::memcpy(bytes + 0x2b, &regs[1], 4);  // ebx
    std::memcpy(bytes + 0x2f, &regs[3], 4);  // edx
    std::memcpy(bytes + 0x33, &regs[2], 4);  // ecx
    bytes[0x37] = 0;
    bool avx = false;
    bool avx512 = false;
    if (maxLeaf >= 2) {
        __cpuid(regs, 1);
        uint32_t edx = static_cast<uint32_t>(regs[3]);
        uint32_t ecx = static_cast<uint32_t>(regs[2]);
        if ((edx >> 25 & 1) != 0) bytes[0x20] |= 2;      // SSE
        if ((edx >> 26 & 1) != 0) bytes[0x20] |= 4;      // SSE2
        if ((ecx & 1) != 0) bytes[0x20] |= 8;            // SSE3
        if ((ecx >> 9 & 1) != 0) bytes[0x20] |= 0x10;    // SSSE3
        if ((ecx >> 19 & 1) != 0) bytes[0x20] |= 0x20;   // SSE4.1
        if ((ecx >> 20 & 1) != 0) bytes[0x20] |= 0x40;   // SSE4.2
        if ((ecx >> 13 & 1) != 0) bytes[0x25] |= 8;      // CMPXCHG16B
        if ((ecx >> 22 & 1) != 0) bytes[0x23] |= 8;      // MOVBE
        if ((ecx >> 23 & 1) != 0) bytes[0x23] |= 0x10;   // POPCNT
        if ((ecx >> 29 & 1) != 0) bytes[0x24] |= 2;      // F16C
        if ((ecx >> 25 & 1) != 0) bytes[0x24] |= 8;      // AES
        if ((ecx & 2) != 0) bytes[0x24] |= 0x10;         // PCLMULQDQ
        if ((ecx >> 30 & 1) != 0) {                      // RDRAND
            bytes[0x25] = static_cast<uint8_t>((bytes[0x25] & 0xfd) | (RandomWorks() ? 2 : 0));
        }
        if ((ecx >> 31 & 1) != 0) bytes[0x2a] |= 1;      // under a hypervisor
        if ((ecx >> 27 & 1) != 0) {                      // OSXSAVE
            uint64_t xcr0 = _xgetbv(0);
            avx = (xcr0 & 6) == 6;
            avx512 = (xcr0 & 0xe6) == 0xe6;
            if (avx && (ecx >> 28 & 1) != 0) {
                bytes[0x20] |= 0x80;                     // AVX
                if ((ecx >> 12 & 1) != 0) bytes[0x24] |= 4;  // FMA
            }
        }
    }
    if (maxLeaf > 6) {
        __cpuidex(regs, 7, 0);
        uint32_t subleaves = static_cast<uint32_t>(regs[0]);
        uint32_t ebx = static_cast<uint32_t>(regs[1]);
        uint32_t ecx = static_cast<uint32_t>(regs[2]);
        uint32_t edx = static_cast<uint32_t>(regs[3]);
        if ((ebx & 8) != 0) bytes[0x23] |= 0x20;         // BMI1
        if ((ebx >> 8 & 1) != 0) bytes[0x23] |= 0x40;    // BMI2
        if ((ebx >> 19 & 1) != 0) bytes[0x24] |= 1;      // ADX
        if ((ebx >> 18 & 1) != 0) bytes[0x25] |= 4;      // RDSEED
        if ((ebx >> 29 & 1) != 0) bytes[0x24] |= 0x20;   // SHA
        if ((ecx >> 8 & 1) != 0) bytes[0x24] |= 0x40;    // GFNI
        if ((edx >> 15 & 1) != 0) bytes[0x2a] |= 2;      // hybrid
        if (avx) {
            if ((ebx & 0x20) != 0) bytes[0x21] |= 1;     // AVX2
            if ((ecx >> 9 & 1) != 0) bytes[0x24] |= 0x80;    // VAES
            if ((ecx >> 10 & 1) != 0) bytes[0x25] |= 1;      // VPCLMULQDQ
            if (avx512) {
                if ((ebx >> 16 & 1) != 0) bytes[0x21] |= 4;      // AVX512F
                if ((ebx >> 17 & 1) != 0) bytes[0x21] |= 8;      // AVX512DQ
                if ((ebx >> 21 & 1) != 0) bytes[0x21] |= 0x10;   // AVX512IFMA
                if ((ebx >> 26 & 1) != 0) bytes[0x21] |= 0x20;   // AVX512PF
                if ((ebx >> 27 & 1) != 0) bytes[0x21] |= 0x40;   // AVX512ER
                if ((ebx >> 28 & 1) != 0) bytes[0x21] |= 0x80;   // AVX512CD
                if ((ebx >> 30 & 1) != 0) bytes[0x22] |= 1;      // AVX512BW
                if ((ebx >> 31 & 1) != 0) bytes[0x22] |= 2;      // AVX512VL
                if ((ecx & 2) != 0) bytes[0x22] |= 4;            // AVX512VBMI
                if ((ecx & 0x40) != 0) bytes[0x22] |= 8;         // AVX512VBMI2
                if ((ecx >> 11 & 1) != 0) bytes[0x22] |= 0x10;   // AVX512VNNI
                if ((ecx >> 12 & 1) != 0) bytes[0x22] |= 0x20;   // AVX512BITALG
                if ((ecx >> 14 & 1) != 0) bytes[0x22] |= 0x40;   // AVX512VPOPCNTDQ
                if ((edx & 4) != 0) bytes[0x22] |= 0x80;         // AVX5124VNNIW
                if ((edx & 8) != 0) bytes[0x23] |= 1;            // AVX5124FMAPS
                if ((edx >> 8 & 1) != 0) bytes[0x23] |= 2;       // AVX512VP2INTERSECT
            }
        }
        if (subleaves != 0) {
            __cpuidex(regs, 7, 1);
            uint32_t eax = static_cast<uint32_t>(regs[0]);
            if (avx) {
                if ((eax & 0x10) != 0) bytes[0x21] |= 2;                 // AVX-VNNI
                if (avx512 && (eax & 0x20) != 0) bytes[0x23] |= 4;       // AVX512BF16
            }
        }
    }
    if (maxLeaf > 0x15) {
        __cpuid(regs, 0x16);
        info->m_maxFrequency = static_cast<uint64_t>(static_cast<uint32_t>(regs[1])) * 1000000;
    }
    __cpuid(regs, static_cast<int>(0x80000000));
    uint32_t maxExtended = static_cast<uint32_t>(regs[0]);
    if (maxExtended > 0x80000000) {
        __cpuid(regs, static_cast<int>(0x80000001));
        uint32_t ecx = static_cast<uint32_t>(regs[2]);
        if ((ecx & 1) != 0) bytes[0x25] |= 0x10;         // LAHF/SAHF
        if ((ecx & 0x20) != 0) bytes[0x23] |= 0x80;      // LZCNT
        if ((ecx >> 8 & 1) != 0) bytes[0x25] |= 0x20;    // PREFETCHW
    }
    if (maxExtended > 0x80000003) {
        for (uint32_t i = 0; i < 3; i++) {
            __cpuid(regs, static_cast<int>(0x80000002 + i));
            std::memcpy(bytes + 0x6b + i * 0x10, regs, 0x10);
        }
        bytes[0x9b] = 0;
    }
    if ((bytes[0x2a] & 1) != 0) {
        bytes[0xb7] = 0;
        __cpuid(regs, 0x40000000);
        std::memcpy(bytes + 0xab, &regs[1], 4);
        std::memcpy(bytes + 0xaf, &regs[2], 4);
        std::memcpy(bytes + 0xb3, &regs[3], 4);
    }
}

// The rated clock from the power manager: the first processor's maximum, in Hz.
// ref: FUN_1402651d0
bool OsCpuFrequencyFromPower(uint64_t* frequency) {
    HMODULE library = LoadLibraryW(L"powrprof.dll");
    if (library == nullptr) {
        return false;
    }
    typedef LONG(WINAPI * CALLPOWER)(int level, void* in, ULONG inSize, void* out, ULONG outSize);
    auto call = reinterpret_cast<CALLPOWER>(GetProcAddress(library, "CallNtPowerInformation"));
    bool result = false;
    if (call != nullptr) {
        SYSTEM_INFO system = {};
        GetSystemInfo(&system);
        // PROCESSOR_POWER_INFORMATION: Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState,
        // CurrentIdleState
        uint32_t size = system.dwNumberOfProcessors * 0x18;
        auto processors = static_cast<uint32_t*>(_alloca((static_cast<uint64_t>(size) + 0xf) & ~0xfull));
        if (call(0xb, nullptr, 0, processors, size) == 0) {
            *frequency = static_cast<uint64_t>(processors[1]) * 1000000;
            result = true;
        }
    }
    FreeLibrary(library);
    return result;
}

// The machine's architecture (IsWow64Process2's native machine, else GetNativeSystemInfo),
// its word-size class, and when CPUID gave no clock, one read off the brand string ("3.60GHz")
// or else from the power manager.
// ref: FUN_140265790
void OsCpuDetectArchitecture(OSCPUINFO* info) {
    info->m_architecture = 0;
    typedef BOOL(WINAPI * ISWOW64PROCESS2)(HANDLE process, USHORT* processMachine, USHORT* nativeMachine);
    auto isWow64Process2 =
        reinterpret_cast<ISWOW64PROCESS2>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    USHORT processMachine = 0;
    USHORT nativeMachine = 0;
    if (isWow64Process2 != nullptr && isWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) != 0) {
        if (nativeMachine == IMAGE_FILE_MACHINE_I386) {
            info->m_architecture = 1;
        } else if (nativeMachine == IMAGE_FILE_MACHINE_AMD64) {
            info->m_architecture = 2;
        } else if (nativeMachine == IMAGE_FILE_MACHINE_ARMNT) {
            info->m_architecture = 3;
        } else if (nativeMachine == IMAGE_FILE_MACHINE_ARM64) {
            info->m_architecture = 4;
        }
    }
    if (info->m_architecture == 0) {
        SYSTEM_INFO system;
        GetNativeSystemInfo(&system);
        if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
            info->m_architecture = 1;
        } else if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
            info->m_architecture = 2;
        } else if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM) {
            info->m_architecture = 3;
        } else if (system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
            info->m_architecture = 4;
        }
    }
    uint32_t architecture = info->m_architecture;
    info->m_unknown18 = architecture == 1 || architecture == 2 ? 1
        : architecture == 3 || architecture == 4                ? 2
        : architecture == 5                                     ? 3
                                                                : 0;
    if (info->m_maxFrequency == 0) {
        const char* brand = reinterpret_cast<const char*>(info) + 0x6b;
        uint64_t length = strnlen(brand, 0x40);
        const char* end = brand + length - 1;
        if (brand < end && *end == 'z' && brand < end - 1 && end[-1] == 'H' && brand < end - 2) {
            double scale = 0.0;
            if (end[-2] == 'G') {
                scale = 1000000000.0;  // DAT_142620ec8
            } else if (end[-2] == 'M') {
                scale = 1000000.0;  // DAT_142620ec0
            } else if (end[-2] == 'T') {
                scale = 1000000000000.0;  // DAT_142620ed0
            }
            if (scale != 0.0) {
                const char* number = end - 3;
                while (brand < number && (static_cast<uint8_t>(*number - '0') < 10 || *number == '.')) {
                    number--;
                }
                double value = std::strtod(number, nullptr);
                if (value != 0.0) {
                    info->m_maxFrequency = static_cast<uint64_t>(value * scale);
                }
            }
        }
    }
    if (info->m_maxFrequency == 0) {
        OsCpuFrequencyFromPower(&info->m_maxFrequency);
    }
}

// The process-wide CPU description, filled once: the defaults (one package, core and thread,
// x86-64, "?" for the vendor and brand), the CPUID features, the architecture and clock, the
// topology.
// ref: FUN_140265ca0
const OSCPUINFO* OsGetCpuInfo() {
    static OSCPUINFO s_info = [] {
        OSCPUINFO info = {};
        info.m_packages = 1;
        info.m_cores = 1;
        info.m_logical = 1;
        info.m_unknown18 = 1;
        info.m_architecture = 2;
        reinterpret_cast<uint8_t*>(&info)[0x2b] = '?';
        reinterpret_cast<uint8_t*>(&info)[0x6b] = '?';
        OsCpuDetectFeatures(&info);
        reinterpret_cast<uint8_t*>(&info)[0x20] |= 1;
        OsCpuDetectArchitecture(&info);
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
