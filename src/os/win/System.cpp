#include "os/Os.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <windows.h>

namespace {

// DAT_142620900: Win32 priority for each OS_THREAD_PRIORITY level. Entry 0 is never read:
// level 0 means "keep the current priority".
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

// DAT_1430a14f0
bool s_isFirstInstance;

}

// True when the process address space reaches above 2^48. Startup reports error 0x8510006b
// when it does.
// ref: FUN_141236c40
bool OsHasOversizedAddressSpace() {
    SYSTEM_INFO info = {};
    GetSystemInfo(&info);
    return reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress) > 0xFFFFFFFFFFFFull;
}

// ref: FUN_140263610
void OsSetCurrentThreadPriority(int32_t level) {
    HANDLE thread = GetCurrentThread();
    int32_t priority = level == OS_THREAD_PRIORITY_CURRENT
        ? GetThreadPriority(GetCurrentThread())
        : s_threadPriorities[level];
    SetThreadPriority(thread, priority);
}

// A named event doubles as the instance check: if it already exists, another client is
// running. The reference keeps the handle open only when it created the event.
// ref: FUN_141237720
void OsCheckSingleInstance(const char* name) {
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, name);
    DWORD error = GetLastError();
    if (event) {
        if (error != ERROR_ALREADY_EXISTS) {
            s_isFirstInstance = true;
            return;
        }
        CloseHandle(event);
    }
    s_isFirstInstance = false;
}

bool OsIsFirstInstance() {
    return s_isFirstInstance;
}

// Detects the CPU's features and writes them to Logs\cpu.log.
// ref: FUN_141237360
void OsLogCpuInfo() {
    FROST_UNIMPLEMENTED();
}

// Installs the unhandled-exception filter (0x1402113a0) and clears the process's
// user-mode exception policy bit through SetProcessUserModeExceptionPolicy.
// ref: FUN_140257940
void OsInitializeExceptionHandling() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141239330
uint32_t OsGetUserLocaleCode() {
    wchar_t wide[0x55];
    if (GetUserDefaultLocaleName(wide, 0x55) == 0) {
        return 0x53556E65;  // enUS
    }
    char name[0x58];
    UnicodeConvertUtf16ToUtf8(name, 0x55, reinterpret_cast<const uint16_t*>(wide), 0x55, nullptr, nullptr);
    uint64_t length = std::strlen(name);
    if (length == 5) {
        // "en-US" -> "enUS"
        std::memmove(name + 2, name + 3, length - 2);
    } else {
        const char* dash = std::strchr(name, '-');
        if (!dash || dash - name < 2) {
            return 0x53556E65;
        }
        name[2] = dash[1];
        name[3] = dash[2];
    }
    return uint32_t(uint8_t(name[3])) << 24 | uint32_t(uint8_t(name[2])) << 16 | uint32_t(uint8_t(name[1])) << 8
        | uint32_t(uint8_t(name[0]));
}
