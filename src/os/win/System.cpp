#include "os/Os.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <windows.h>
#include <malloc.h>
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

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
extern "C" unsigned long long FUN_141237360();
// translated: FUN_141237360
void OsLogCpuInfo() {
    __try {
        FUN_141237360();
    } __except (GenFault(0x141237360ull, GetExceptionInformation())) {
    }
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

namespace {

// ref: FUN_140266d20
OSVERSION OsReadVersion() {
    RTL_OSVERSIONINFOEXW info;
    std::memset(reinterpret_cast<uint8_t*>(&info) + 4, 0, 0x118);
    info.dwOSVersionInfoSize = 0x11c;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    typedef LONG(WINAPI * RTLGETVERSION)(RTL_OSVERSIONINFOEXW*);
    auto getVersion = reinterpret_cast<RTLGETVERSION>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (getVersion == nullptr) {
#pragma warning(suppress : 4996)
        GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&info));
    } else {
        getVersion(&info);
    }
    OSVERSION version;
    version.m_server = 0;
    std::memset(version.m_extra, 0, sizeof(version.m_extra));
    version.m_major = info.dwMajorVersion;
    version.m_minor = info.dwMinorVersion;
    version.m_servicePack = info.wServicePackMajor;
    version.m_build = info.dwBuildNumber;
    version.m_platform = 10;
    WideCharToMultiByte(CP_UTF8, 0, info.szCSDVersion, -1, version.m_extra, 0x80, nullptr, nullptr);
    version.m_server = info.wProductType != VER_NT_WORKSTATION;
    static const char* const s_wineExports[] = { "wine_get_version", "wine_get_host_version",
        "wine_nt_to_unix_file_name" };
    for (const char* name : s_wineExports) {
        if (GetProcAddress(ntdll, name) != nullptr) {
            version.m_wine = 1;
            return version;
        }
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Wine", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        version.m_wine = 1;
    } else {
        version.m_wine = 0;
    }
    return version;
}

}

// ref: FUN_140263b70
const OSVERSION* OsGetVersion() {
    static OSVERSION s_version = OsReadVersion();  // DAT_142e1d900
    return &s_version;
}

// ref: FUN_140263c50
void OsFormatVersion(char* dest, uint64_t size, const OSVERSION* version) {
    // "{} {}.{}.{} ({})" and its siblings through bc's {fmt}-style formatter (FUN_14026a3e0),
    // which is not ported; the text is left empty.
    (void)version;
    FROST_UNIMPLEMENTED();
    if (size != 0) {
        dest[0] = '\0';
    }
}

// ref: FUN_140266fd0
void OsGetPowerStatus(OSPOWERSTATUS* status) {
    SYSTEM_POWER_STATUS power;
    if (!GetSystemPowerStatus(&power)) {
        status->m_acFlags &= 0xfc;
        status->m_batteryFlags &= 0xfc;
        status->m_percentFlags &= 0xfc;
        status->m_lifeFlags &= 0xfe;
        status->m_lifeFlags &= 0xfd;
        return;
    }
    status->m_onAc = power.ACLineStatus != 0;
    status->m_batteryPercent = power.BatteryLifePercent;
    status->m_acFlags = (power.ACLineStatus != 0xff ? 2 : 0) | (status->m_acFlags & 0xfd) | 1;
    status->m_batteryPresent = power.BatteryFlag != 0x80;
    status->m_batteryFlags = (power.BatteryFlag != 0xff ? 2 : 0) | (status->m_batteryFlags & 0xfd) | 1;
    status->m_percentFlags = (power.BatteryLifePercent != 0xff ? 2 : 0) | (status->m_percentFlags & 0xfd) | 1;
    status->m_batteryLifeNs = static_cast<uint64_t>(power.BatteryLifeTime) * 1000000000;
    status->m_lifeFlags = (power.BatteryLifeTime != 0xffffffff ? 2 : 0) | (status->m_lifeFlags & 0xfd) | 1;
}

// ref: FUN_141236ff0
uint64_t OsGetPhysicalMemory() {
    MEMORYSTATUSEX status;
    status.dwLength = 0x40;
    GlobalMemoryStatusEx(&status);
    return status.ullTotalPhys;
}

// ref: FUN_140267270
bool OsIsRemoteSession() {
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
}

// Shows a message box (UTF-8 text). The low nibble of `style` picks the buttons (1 OK, 2 OK and
// Cancel, 4 Yes and No, 8 Retry and Cancel), 0x10000 / 0x20000 / 0x40000 the icon. Returns 0 for
// OK or Yes, 1 for No, 2 otherwise.
// ref: FUN_141237fb0
uint32_t OsMessageBox(void* owner, uint32_t style, const char* text, const char* caption) {
    uint32_t length = static_cast<uint32_t>(UnicodeCountUtf8ToUtf16(text, 0x7fffffff, nullptr));
    auto wideText = static_cast<wchar_t*>(_alloca((static_cast<size_t>(length) * 2 + 0xf) & ~static_cast<size_t>(0xf)));
    UnicodeConvertUtf8ToUtf16(wideText, length, text, 0x7fffffff, nullptr, nullptr);
    const wchar_t* wideCaption = L"";  // DAT_1426178b8
    if (caption != nullptr) {
        uint32_t captionLength = static_cast<uint32_t>(UnicodeCountUtf8ToUtf16(caption, 0x7fffffff, nullptr));
        auto converted = static_cast<wchar_t*>(_alloca((static_cast<size_t>(captionLength) * 2 + 0xf) & ~static_cast<size_t>(0xf)));
        UnicodeConvertUtf8ToUtf16(converted, captionLength, caption, 0x7fffffff, nullptr, nullptr);
        wideCaption = converted;
    }
    uint32_t buttons;
    switch (style & 0xf) {
        case 1: buttons = MB_OK; break;
        case 2: buttons = MB_OKCANCEL; break;
        case 4: buttons = MB_YESNO; break;
        case 8: buttons = MB_RETRYCANCEL; break;
        default: buttons = 0; break;
    }
    switch (style & 0xf0000) {
        case 0x10000: buttons |= MB_ICONINFORMATION; break;
        case 0x20000: buttons |= MB_ICONWARNING; break;
        case 0x40000:
        case 0x80000: buttons |= MB_ICONERROR; break;
    }
    int result = MessageBoxW(static_cast<HWND>(owner), wideText, wideCaption, buttons);
    if (result == IDOK || result == IDYES) {
        return 0;
    }
    return result == IDNO ? 1 : 2;
}
