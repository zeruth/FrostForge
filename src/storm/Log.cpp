#include "storm/Log.hpp"
#include "os/Os.hpp"
#include "storm/String.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

namespace {

// One log (0x10138 bytes, VirtualAlloc'd): its handle, the bucket chain, the file, and a 64K
// line buffer written out at 48K, on close, and when the process ends.
struct LOGRECORD {
    uint64_t m_handle;              // 0x000
    LOGRECORD* m_next;              // 0x008
    char m_filename[0x108];         // 0x010
    void* m_file;                   // 0x118: INVALID_HANDLE_VALUE until opened
    uint32_t m_flags;               // 0x120: 1 open now, 2 no file, 4 append
    uint32_t m_length;              // 0x124
    uint32_t m_lineStart;           // 0x128: where the last line began
    int32_t m_indent;               // 0x12c
    uint32_t m_timestamps;          // 0x130
    char m_buffer[0x10004];         // 0x134
};
static_assert(sizeof(LOGRECORD) == 0x10138, "LOGRECORD is 0x10138 bytes");

uint32_t s_logInitialized;          // DAT_142e1d854: a reference count
CRITICAL_SECTION s_buckets[4];      // DAT_142e1d660 (0x28 apart)
LOGRECORD* s_bucketHeads[4];        // DAT_142e1d700
uint64_t s_lastHandle;              // DAT_142e1d720
CRITICAL_SECTION s_directoryLock;   // DAT_142e1d728
char s_directory[0x104];            // DAT_142e1d750: where relative logs go (empty: the exe's folder)
char s_timestamp[0x40];             // DAT_142e1d860
uint64_t s_timestampLength;         // DAT_142e1d8a0
DWORD s_timestampTick;              // DAT_142e1d8a8
uint32_t s_debugOutput;             // DAT_142e1d8b8: also send each line to OutputDebugString
uint32_t s_8b0;                     // DAT_142e1d8b0
uint32_t s_8c0[4];                  // DAT_142e1d8c0..8d0

void CopyBounded(char* dest, const char* source, int64_t limit) {
    for (int64_t left = limit;;) {
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
        if (--left == 0) {
            *dest = '\0';
            return;
        }
    }
}

void AppendBounded(char* buffer, uint64_t size, const char* text) {
    char* end = buffer;
    uint64_t room = size;
    while (room != 0 && *end != '\0') {
        end++;
        room--;
    }
    room = static_cast<uint64_t>(buffer + size - end);
    if (room != 0) {
        for (; *text != '\0'; text++) {
            if (room != 1) {
                *end++ = *text;
                room--;
            }
        }
        *end = '\0';
    }
}

// The length of a path's root: "\\server\share\", "C:\", "C:", "/", or none.
uint32_t RootLength(const char* path) {
    if (path[0] == '/') {
        return 1;
    }
    uint64_t length = std::strlen(path);
    if (length > 1) {
        if (path[1] == ':') {
            return (path[2] == '\\') + 2;
        }
        if (path[0] == '\\' && path[1] == '\\') {
            const char* p = path + 2;
            for (int32_t i = 0; i < 2; i++) {
                if (p != nullptr) {
                    p = std::strchr(p, '\\');
                    if (p != nullptr) {
                        p++;
                    }
                }
            }
            return p == nullptr ? static_cast<uint32_t>(length) : static_cast<uint32_t>(p - path);
        }
    }
    return 0;
}

// A relative log name goes under the log folder (or the executable's).
// ref: FUN_14025a690
const char* ResolvePath(char* dest, uint32_t size, const char* filename) {
    if (filename == nullptr || *filename == '\0' || filename[1] == ':' || std::strchr(filename, '\\') != nullptr) {
        return filename;
    }
    EnterCriticalSection(&s_directoryLock);
    if (s_directory[0] == '\0') {
        GetModuleFileNameA(GetModuleHandleA(nullptr), dest, size);
        char* slash = std::strrchr(dest, '\\');
        if (slash != nullptr) {
            *slash = '\0';
        }
        AppendBounded(dest, size, "\\");
        AppendBounded(dest, size, filename);
    } else {
        CopyBounded(dest, s_directory, int64_t(size) - 1);
        uint64_t length = std::strlen(s_directory);
        char* out = dest + length;
        for (uint64_t left = size - length; left != 0;) {
            if (--left == 0) {
                *out = '\0';
                break;
            }
            char c = *filename++;
            *out++ = c;
            if (c == '\0') {
                break;
            }
        }
    }
    LeaveCriticalSection(&s_directoryLock);
    return dest;
}

// Creates every folder on a file's path.
// ref: FUN_14025a1a0
void CreatePathFolders(const char* filename) {
    char path[0x110];
    CopyBounded(path, filename, 0x103);
    char* back = std::strrchr(path, '\\');
    char* forward = std::strrchr(path, '/');
    char* last = forward < back ? back : forward;
    if (last != nullptr) {
        uint32_t root = RootLength(path);
        if (last < path + root) {
            path[root] = '\0';
        } else {
            last[1] = '\0';
        }
    }
    uint32_t root = RootLength(path);
    for (char* p = std::strchr(path + root, '/'); p != nullptr; p = std::strchr(p + 1, '/')) {
        *p = '\\';
    }
    for (char* p = std::strchr(path + root, '\\'); p != nullptr; p = std::strchr(p + 1, '\\')) {
        *p = '\0';
        CreateDirectoryA(path, nullptr);
        *p = '\\';
    }
    CreateDirectoryA(path, nullptr);
}

// ref: FUN_14025a450
bool OpenLogFile(const char* filename, void** file, uint32_t flags) {
    if (filename == nullptr || *filename == '\0') {
        *file = INVALID_HANDLE_VALUE;
        return false;
    }
    char path[0x110];
    const char* resolved = ResolvePath(path, 0x104, filename);
    CreatePathFolders(resolved);
    *file = OsCreateFile(resolved, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, ((flags >> 2) & 1) * 2 + 2,
        FILE_ATTRIBUTE_NORMAL);
    if (*file != INVALID_HANDLE_VALUE && (flags & 4) != 0) {
        OsSetFilePointer(*file, 0, FILE_END);
    }
    return *file != INVALID_HANDLE_VALUE;
}

// ref: FUN_14025a400
void FlushRecord(LOGRECORD* record) {
    if (record->m_length != 0) {
        uint32_t written;
        OsWriteFile(record->m_file, record->m_buffer, record->m_length, &written);
        OsFlushFile(record->m_file);
        record->m_length = 0;
        record->m_lineStart = 0;
    }
}

// The line's time stamp (or as many spaces).
// ref: FUN_14025a580
void WriteTimestamp(LOGRECORD* record, int32_t stamp) {
    if (record->m_timestamps == 0) {
        return;
    }
    DWORD tick = GetTickCount();
    if (tick != s_timestampTick) {
        s_timestampTick = tick;
        SYSTEMTIME now;
        GetLocalTime(&now);
        wsprintfA(s_timestamp, "%u/%u %02u:%02u:%02u.%03u  ", now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds);
        s_timestampLength = std::strlen(s_timestamp);
    }
    char* out = record->m_buffer + record->m_length;
    if (stamp == 0) {
        std::memset(out, ' ', s_timestampLength);
        record->m_buffer[record->m_length + s_timestampLength] = '\0';
    } else {
        std::memcpy(out, s_timestamp, s_timestampLength + 1);
    }
    record->m_length += static_cast<uint32_t>(s_timestampLength);
}

// ref: FUN_14025a520
void WriteIndent(LOGRECORD* record) {
    int32_t indent = record->m_indent;
    if (indent > 0) {
        if (indent > 0x7f) {
            indent = 0x80;
        }
        std::memset(record->m_buffer + record->m_length, ' ', indent);
        record->m_buffer[record->m_length + indent] = '\0';
        record->m_length += indent;
    }
}

LOGRECORD* FindRecord(uint64_t handle, uint32_t bucket) {
    for (LOGRECORD* record = s_bucketHeads[bucket]; record != nullptr; record = record->m_next) {
        if (record->m_handle == handle) {
            return record;
        }
    }
    return nullptr;
}

void UnlinkRecord(LOGRECORD* record, uint32_t bucket) {
    LOGRECORD** link = &s_bucketHeads[bucket];
    for (LOGRECORD* node = *link; node != nullptr; node = node->m_next) {
        if (node == record) {
            *link = node->m_next;
            VirtualFree(node, 0, MEM_RELEASE);
            return;
        }
        link = &node->m_next;
    }
}

// Closes every log at exit.
// ref: FUN_14025ad40
void LogShutdown() {
    if (s_logInitialized != 1) {
        return;
    }
    // FUN_14025b080 runs first: the flushing of the profile logs, not ported.
    for (uint32_t bucket = 0; bucket < 4; bucket++) {
        EnterCriticalSection(&s_buckets[bucket]);
        while (s_bucketHeads[bucket] != nullptr) {
            LOGRECORD* record = s_bucketHeads[bucket];
            char name[0x110];
            CopyBounded(name, record->m_filename, 0x103);
            SLogClose(reinterpret_cast<HSLOG>(record->m_handle));
            // FUN_140259620("HSLOG", name): the leak report, compiled empty in release.
        }
        LeaveCriticalSection(&s_buckets[bucket]);
    }
    s_logInitialized = 0;
    for (CRITICAL_SECTION& lock : s_buckets) {
        DeleteCriticalSection(&lock);
    }
    DeleteCriticalSection(&s_directoryLock);
}

// Storm's initialization runs these from the CRT initializers (0x14024d100 via 0x1401ca4f0).
struct LOGSTARTUP {
    LOGSTARTUP() {
        SLogResetDebugOutput();
        SLogInitialize();
    }
} s_logStartup;

}

// ref: FUN_14025b650
void SLogResetDebugOutput() {
    s_8b0 = 0;
    s_debugOutput = 0;
    s_8c0[0] = s_8c0[1] = s_8c0[2] = s_8c0[3] = 0;
}

// ref: FUN_14025b260
void SLogInitialize() {
    if (s_logInitialized != 0) {
        s_logInitialized++;
        return;
    }
    for (CRITICAL_SECTION& lock : s_buckets) {
        InitializeCriticalSection(&lock);
    }
    InitializeCriticalSection(&s_directoryLock);
    s_logInitialized = 1;
    std::atexit(&LogShutdown);
}

// ref: FUN_14025a990
bool SLogCreate(const char* filename, uint32_t flags, HSLOG* log) {
    *log = nullptr;
    const char* name = (flags & 2) == 0 ? filename : "";
    uint32_t mode = (flags & 2) == 0 ? flags : (flags & ~1u);
    void* file = INVALID_HANDLE_VALUE;
    if ((mode & 1) != 0) {
        if (name == nullptr || *name == '\0') {
            return false;
        }
        char path[0x110];
        const char* resolved = ResolvePath(path, 0x104, name);
        CreatePathFolders(resolved);
        file = OsCreateFile(resolved, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, ((mode >> 2) & 1) * 2 + 2,
            FILE_ATTRIBUTE_NORMAL);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        if ((mode & 4) != 0) {
            OsSetFilePointer(file, 0, FILE_END);
        }
    }
    uint64_t handle = ++s_lastHandle;
    *log = reinterpret_cast<HSLOG>(handle);
    if (handle != 0) {
        uint32_t bucket = static_cast<uint32_t>(handle) & 3;
        EnterCriticalSection(&s_buckets[bucket]);
        LOGRECORD* record = FindRecord(handle, bucket);
        if (record == nullptr) {
            LOGRECORD** link = &s_bucketHeads[bucket];
            while (*link != nullptr) {
                link = &(*link)->m_next;
            }
            record = static_cast<LOGRECORD*>(VirtualAlloc(nullptr, sizeof(LOGRECORD), MEM_COMMIT, PAGE_READWRITE));
            *link = record;
            if (record == nullptr) {
                LeaveCriticalSection(&s_buckets[bucket]);
                *log = nullptr;
                return false;
            }
            record->m_handle = handle;
            record->m_next = nullptr;
            record->m_filename[0] = '\0';
            record->m_file = INVALID_HANDLE_VALUE;
            record->m_length = 0;
            record->m_lineStart = 0;
        }
        record->m_file = file;
        CopyBounded(record->m_filename, name, 0x103);
        record->m_flags = mode;
        record->m_timestamps = 1;
        record->m_indent = 0;
        LeaveCriticalSection(&s_buckets[bucket]);
        return true;
    }
    *log = nullptr;
    return false;
}

// ref: FUN_14025a880
void SLogClose(HSLOG log) {
    if (s_logInitialized == 0 || log == nullptr) {
        return;
    }
    uint64_t handle = reinterpret_cast<uint64_t>(log);
    uint32_t bucket = static_cast<uint32_t>(handle) & 3;
    EnterCriticalSection(&s_buckets[bucket]);
    LOGRECORD* record = FindRecord(handle, bucket);
    if (record != nullptr) {
        if (record->m_file != INVALID_HANDLE_VALUE) {
            FlushRecord(record);
            OsCloseFile(record->m_file);
        }
        UnlinkRecord(record, bucket);
    }
    LeaveCriticalSection(&s_buckets[bucket]);
}

// ref: FUN_14025afa0
void SLogFlush(HSLOG log) {
    if (log == nullptr) {
        return;
    }
    uint64_t handle = reinterpret_cast<uint64_t>(log);
    uint32_t bucket = static_cast<uint32_t>(handle) & 3;
    EnterCriticalSection(&s_buckets[bucket]);
    LOGRECORD* record = FindRecord(handle, bucket);
    if (record != nullptr && record->m_file != INVALID_HANDLE_VALUE) {
        FlushRecord(record);
    }
    LeaveCriticalSection(&s_buckets[bucket]);
}

// ref: FUN_14025b2e0
int32_t SLogWriteV(HSLOG log, const char* format, va_list args) {
    if (log == nullptr) {
        return 0;
    }
    uint64_t handle = reinterpret_cast<uint64_t>(log);
    uint32_t bucket = static_cast<uint32_t>(handle) & 3;
    EnterCriticalSection(&s_buckets[bucket]);
    LOGRECORD* record = FindRecord(handle, bucket);
    if (record != nullptr) {
        if (record->m_file == INVALID_HANDLE_VALUE && !OpenLogFile(record->m_filename, &record->m_file, record->m_flags)) {
            // A log whose file will not open forgets its name, so it is not tried again.
            record->m_filename[0] = '\0';
        } else {
            WriteTimestamp(record, 1);
            WriteIndent(record);
            int32_t result = static_cast<int32_t>(SStrVPrintfCount(record->m_buffer + record->m_length,
                0x10000 - record->m_length, format, args));
            record->m_length += static_cast<uint32_t>(std::strlen(record->m_buffer + record->m_length));
            record->m_buffer[record->m_length] = '\r';
            record->m_buffer[record->m_length + 1] = '\n';
            record->m_buffer[record->m_length + 2] = '\0';  // DAT_142617950
            record->m_length += 2;
            if (s_debugOutput != 0) {
                OutputDebugStringA(record->m_buffer + record->m_lineStart);
            }
            record->m_lineStart = record->m_length;
            if (record->m_length > 0xbfff) {
                FlushRecord(record);
            }
            LeaveCriticalSection(&s_buckets[bucket]);
            return result;
        }
    }
    LeaveCriticalSection(&s_buckets[bucket]);
    return 0;
}

// ref: FUN_14025b4a0
int32_t SLogWrite(HSLOG log, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int32_t result = SLogWriteV(log, format, args);
    va_end(args);
    return result;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142e1d8c0 = (void*)&s_8c0;
// ---- end of generated globals ----
