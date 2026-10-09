#include "client/CrashReport.hpp"
#include "os/Os.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <windows.h>

namespace {

// DAT_142dda590 / DAT_142ddc610: files the crash reporter attaches, 0x104 characters each,
// room for 32 entries.
char s_files[32][0x104];
uint32_t s_fileCount;

}

// Appends a path to the crash report's attachment list, truncated to 0x103 characters.
// The reference does not check the count against the list's 32 entries; nor does this.
// ref: FUN_140212e10
void CrashReportAddFile(const char* path) {
    char* dest = s_files[s_fileCount++];
    for (int32_t remaining = 0x103; remaining != 0; remaining--) {
        char c = *path++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
    }
    *dest = '\0';
}

// Records argv[0] (made absolute against the current directory when it is relative) and
// the whole command line, as UTF-8, into g_errorReportCommand and g_errorReportParams. Only argv is read; the other three
// arguments are passed (1, 0, 0) and unused.
// ref: FUN_1402115d0
void CrashReportRecordCommandLine(char** argv, int32_t a2, int32_t a3, int32_t a4) {
    (void)a2;
    (void)a3;
    (void)a4;

    if (!OsPathIsAbsolute(argv[0])) {
        char directory[0x110];
        char joined[0x110];
        OsGetCurrentDirectory(directory, 0x104);
        OsPathJoin(joined, 0x104, directory, argv[0]);
        OsPathToNative(joined, g_errorReportCommand, 0x104);
    } else {
        uint64_t length = std::strlen(argv[0]);
        if (length > 0x7FFF) {
            length = 0x7FFF;
        }
        std::memcpy(g_errorReportCommand, argv[0], length);
        g_errorReportCommand[length] = '\0';
    }

    g_errorReportParams[0] = '\0';
    const wchar_t* commandLine = GetCommandLineW();
    if (!commandLine) {
        commandLine = L"";
    }

    char utf8[0x800];
    int32_t written;
    UnicodeConvertUtf16ToUtf8(utf8, 0x800, reinterpret_cast<const uint16_t*>(commandLine), 0x7FFFFFFF, &written,
        nullptr);
    uint64_t length = std::strlen(utf8);
    if (length > 0x7FFF) {
        length = 0x7FFF;
    }
    std::memcpy(g_errorReportParams, utf8, length);
    g_errorReportParams[length] = '\0';
}

// Fills the reporter's project fields ("World of Warcraft", "WrathClassic", "Retail",
// "Client", the t2jira.corp.blizzard.net tracker) and its paths: the install directory,
// Utils\WindowsExceptionHandler.dll, Utils\WowWindowsExceptionHandler.dll, Errors and
// BlizzardError.exe.
// ref: FUN_140217470
void CrashReportConfigure() {
    FROST_UNIMPLEMENTED();
}
