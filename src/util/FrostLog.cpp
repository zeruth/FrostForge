#include "util/FrostLog.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <windows.h>
#include <dbghelp.h>

namespace {

std::recursive_mutex s_lock;
FILE* s_file = nullptr;
bool s_opened = false;
bool s_symbols = false;

FILE* LogFile() {
    if (!s_opened) {
        s_opened = true;
        CreateDirectoryA("Logs", nullptr);
        s_file = std::fopen("Logs\\FrostLite.log", "w");
    }
    return s_file;
}

void WriteLine(const char* text) {
    SYSTEMTIME time;
    GetLocalTime(&time);
    char line[4096];
    std::snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u  %s\n", time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, text);
    if (FILE* file = LogFile()) {
        std::fputs(line, file);
        std::fflush(file);
    }
    std::fputs(line, stderr);
    std::fflush(stderr);
}

}

void FrostLog(const char* category, const char* format, ...) {
    char message[3800];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    char text[4000];
    std::snprintf(text, sizeof(text), "[%s] %s", category, message);
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    WriteLine(text);
}

void FrostLogStack(int skip, int count) {
    void* frames[64];
    if (count > 64) {
        count = 64;
    }
    // This function and FrostLogStack's caller's own frame are skipped as well.
    USHORT captured = CaptureStackBackTrace(static_cast<DWORD>(skip + 2), static_cast<DWORD>(count), frames, nullptr);
    std::lock_guard<std::recursive_mutex> guard(s_lock);
    HANDLE process = GetCurrentProcess();
    if (!s_symbols) {
        s_symbols = true;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
    }
    for (USHORT i = 0; i < captured; i++) {
        auto address = reinterpret_cast<DWORD64>(frames[i]);
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        char text[600];
        if (SymFromAddr(process, address, &displacement, symbol)) {
            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisplacement = 0;
            const char* file = "";
            unsigned lineNumber = 0;
            if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line)) {
                file = line.FileName;
                const char* src = std::strstr(file, "\\src\\");
                if (src != nullptr) {
                    file = src + 1;
                }
                lineNumber = line.LineNumber;
            }
            std::snprintf(text, sizeof(text), "    at %s  %s:%u", symbol->Name, file, lineNumber);
        } else {
            std::snprintf(text, sizeof(text), "    at 0x%llx", static_cast<unsigned long long>(address));
        }
        WriteLine(text);
    }
}
