#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include "storm/String.hpp"
#include <cstring>
#include <malloc.h>

// Reads a whole file into a buffer from SMemAlloc (Engine/Source/Os/W32/OsFile.cpp). Used
// for SCmd's @response files.
// ref: FUN_140214ed0
bool OsFileReadAll(const char* path, uint8_t** data, uint32_t* size, int32_t a4, int32_t a5) {
    (void)path;
    (void)a4;
    (void)a5;
    *data = nullptr;
    *size = 0;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_140215280
void OsFileFree(void* data) {
    SMemFree(data, __FILE__, __LINE__, 0);
}

// CreateFileW on a UTF-8 path. INVALID_HANDLE_VALUE for no path, no access, or a creation
// disposition outside 1..5.
// ref: FUN_140214840
HANDLE OsCreateFile(const char* path, uint32_t access, uint32_t share, uint32_t disposition, uint32_t attributes) {
    if (!path || access == 0 || disposition - 1 > 4) {
        return INVALID_HANDLE_VALUE;
    }
    wchar_t wide[0x108];
    UnicodeConvertUtf8ToUtf16(wide, 0x104, path, 0x7FFFFFFF, nullptr, nullptr);
    return CreateFileW(wide, access, share, nullptr, disposition, attributes, nullptr);
}

// ref: FUN_140214620
void OsCloseFile(HANDLE file) {
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

// The file's size, or `fallback` when Windows cannot tell.
// ref: FUN_140214dc0
uint64_t OsGetFileSize(HANDLE file, uint64_t fallback) {
    if (file != INVALID_HANDLE_VALUE) {
        DWORD high;
        DWORD low = GetFileSize(file, &high);
        if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
            return fallback;
        }
        fallback = (uint64_t(high) << 32) | low;
    }
    return fallback;
}

// ref: 0x140215340
bool OsFindFilesInvokePointer(OSFILEINFO* info, void* param, void* storage) {
    typedef bool (*FINDCALLBACK)(OSFILEINFO* info, void* param);
    return (*static_cast<FINDCALLBACK*>(storage))(info, param);
}

// Calls `callback` for each file matching <directory of dir><pattern> (hidden ones only with
// includeHidden) until it returns true; returns whether one did. The callback is passed by
// value: this destroys it.
// ref: FUN_140214ad0
bool OsFindFiles(const char* dir, const char* pattern, blz::function_storage* callback, void* param, bool includeHidden) {
    char path[0x104];
    {
        char* out = path;
        const char* in = dir;
        for (int64_t left = 0x103;;) {
            char c = *in++;
            *out++ = c;
            if (c == '\0') {
                break;
            }
            if (--left == 0) {
                *out = '\0';
                break;
            }
        }
    }
    OsPathTruncateToDirectory(path);
    {
        char* end = path;
        uint64_t room = sizeof(path);
        while (room != 0 && *end != '\0') {
            end++;
            room--;
        }
        room = static_cast<uint64_t>(path + sizeof(path) - end);
        if (room != 0) {
            for (; *pattern != '\0'; pattern++) {
                if (room != 1) {
                    *end++ = *pattern;
                    room--;
                }
            }
            *end = '\0';
        }
    }

    wchar_t wide[0x108];
    UnicodeConvertUtf8ToUtf16(wide, 0x104, path, 0x7FFFFFFF, nullptr, nullptr);
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(wide, &data);
    bool found = false;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (includeHidden || !(data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) {
                OSFILEINFO info;
                int32_t bytes;
                UnicodeConvertUtf16ToUtf8(info.m_name, 0x104, reinterpret_cast<const uint16_t*>(data.cFileName),
                    0x7FFFFFFF, &bytes, nullptr);
                uint32_t last = static_cast<uint32_t>(bytes) - 1;
                if (last > 0x103) {
                    last = 0x103;
                }
                info.m_name[last] = '\0';
                info.m_size = data.nFileSizeLow;
                info.m_attributes = 0;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    info.m_attributes = OS_FILE_DIRECTORY;
                }
                if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                    info.m_attributes |= OS_FILE_READONLY;
                }
                if (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
                    info.m_attributes |= OS_FILE_HIDDEN;
                }
                info.m_directory = dir;
                typedef bool (*INVOKE)(OSFILEINFO* info, void* param, void* storage);
                if (reinterpret_cast<INVOKE>(callback->m_invoke)(&info, param, callback->m_storage)) {
                    found = true;
                    break;
                }
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    callback->m_manager(callback->m_storage, nullptr, 0);
    return found;
}

// ref: FUN_140215150
bool OsReadFile(HANDLE file, void* buffer, uint32_t bytes, uint32_t* read) {
    DWORD done;
    BOOL ok = ReadFile(file, buffer, bytes, &done, nullptr);
    if (read) {
        *read = done;
    }
    return ok != 0;
}

// ref: FUN_140265f30
void OsFileInit(OSFILE* file) {
    file->m_handle = INVALID_HANDLE_VALUE;
}

// Opens a UTF-8 path (flags: 2 write-through, 4 delete on close, 8 sequential scan).
// ref: FUN_1402663b0
uint32_t OsFileOpen(OSFILE* file, const char* path, int32_t mode, uint32_t flags) {
    DWORD access;
    DWORD disposition;
    switch (mode) {
    case 0:
    case 6:
        access = GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
        break;
    case 1:
    case 7:
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = OPEN_ALWAYS;
        break;
    case 2:
        access = GENERIC_READ;
        disposition = OPEN_EXISTING;
        break;
    case 3:
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = OPEN_EXISTING;
        break;
    case 4:
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        break;
    case 5:
        access = GENERIC_READ | GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        break;
    case 8:
        access = 4;
        disposition = OPEN_ALWAYS;
        break;
    default:
        return OsErrorFromCode(2);
    }

    DWORD attributes = ((flags & 2) << 28) | FILE_ATTRIBUTE_NORMAL;
    if (flags & 4) {
        attributes = ((flags & 2) << 28) | FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_NORMAL;
    } else if (flags & 8) {
        attributes = ((flags & 2) << 28) | FILE_FLAG_SEQUENTIAL_SCAN | FILE_ATTRIBUTE_NORMAL;
    }

    uint64_t length = std::strlen(path);
    auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x3FFFFFFF);
    file->m_handle = CreateFileW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        disposition, attributes, nullptr);
    if (file->m_handle == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    if (static_cast<uint32_t>(mode - 6) < 2) {
        SetFilePointer(file->m_handle, 0, nullptr, FILE_END);
    }
    return 0;
}

// ref: FUN_140266380
uint64_t OsFileGetSize(OSFILE* file) {
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file->m_handle, &size)) {
        size.QuadPart = 0;
    }
    return static_cast<uint64_t>(size.QuadPart);
}

// A broken pipe counts as the end of the data, not an error.
// ref: FUN_140266570
uint32_t OsFileRead(OSFILE* file, void* buffer, uint32_t bytes, uint32_t* read) {
    DWORD done;
    if (!ReadFile(file->m_handle, buffer, bytes, &done, nullptr)) {
        *read = 0;
        DWORD error = GetLastError();
        return error == ERROR_BROKEN_PIPE ? 0 : error;
    }
    *read = done;
    return 0;
}

// ref: FUN_140265f40
void OsFileClose(OSFILE* file) {
    if (file->m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(file->m_handle);
        file->m_handle = INVALID_HANDLE_VALUE;
    }
}

// FILETIME (100 ns since 1601) to nanoseconds since 1970.
// ref: FUN_14026b260
int64_t OsFileTimeToNanoseconds(int64_t filetime) {
    return static_cast<int64_t>(static_cast<uint64_t>(filetime) * 100 + 0x5E6684F4B3960000ull);
}

// ref: FUN_140266090
uint32_t OsFileStat(const char* path, OSFILESTAT* stat) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    uint64_t length = std::strlen(path);
    auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x3FFFFFFF);
    if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &data)) {
        return GetLastError();
    }
    uint32_t flags = 0;
    stat->m_flags = 0;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
        flags = 2;
        stat->m_flags = 2;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
        flags |= 4;
        stat->m_flags = flags;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        stat->m_size = 0;
        stat->m_flags = flags | 1;
    } else {
        stat->m_size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    auto ticks = [](FILETIME time) {
        return static_cast<int64_t>((uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime);
    };
    stat->m_created = OsFileTimeToNanoseconds(ticks(data.ftCreationTime));
    stat->m_accessed = OsFileTimeToNanoseconds(ticks(data.ftLastAccessTime));
    stat->m_written = OsFileTimeToNanoseconds(ticks(data.ftLastWriteTime));
    return 0;
}

namespace {

uint64_t CopyMessage(char* buffer, uint64_t size, const char* text, uint64_t length) {
    if (size != 0) {
        uint64_t n = size - 1 < length ? size - 1 : length;
        std::memcpy(buffer, text, n);
        buffer[n] = '\0';
    }
    return length;
}

// The system's text for an error code (UTF-8, a trailing "." dropped), or "Unknown Error: <n>".
// The number is formatted by 0x140268c40 / 0x140269830 (a decimal formatter, not ported);
// FrostLite formats it with Storm's printf.
// ref: FUN_140267930
uint64_t OsErrorStringSystem(char* buffer, uint64_t size, uint32_t error) {
    auto wide = static_cast<wchar_t*>(_alloca((size * 2 + 0xF) & ~uint64_t(0xF)));
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, wide,
        static_cast<DWORD>(size), nullptr);
    if (length != 0) {
        if (length > 2 && wide[length - 3] == L'.') {
            wide[length - 3] = L'\0';
            length -= 3;
        }
        int32_t written = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int32_t>(length), buffer,
            static_cast<int32_t>(size), nullptr, nullptr);
        if (size == 0) {
            return static_cast<uint64_t>(written);
        }
        uint64_t end = static_cast<uint64_t>(written) < size ? static_cast<uint64_t>(written) : size - 1;
        buffer[end] = '\0';
        return static_cast<uint64_t>(written);
    }
    char number[0x10];
    SStrPrintf(number, sizeof(number), "%u", error);
    uint64_t digits = std::strlen(number);
    CopyMessage(buffer, size, "Unknown Error: ", 0xF);
    if (size > 0xF) {
        CopyMessage(buffer + 0xF, size - 0xF, number, digits);
    }
    return digits + 0xF;
}

}

// The text for an error code from the file layer, truncated to `size`; returns its length.
// ref: FUN_140263fa0
uint64_t OsErrorString(char* buffer, uint64_t size, uint32_t error) {
    if (error == OsErrorFromCode(0)) {
        return CopyMessage(buffer, size, "Success", 7);
    }
    if (error == OsErrorFromCode(1)) {
        return CopyMessage(buffer, size, "Result too large", 0x10);
    }
    if (error == OsErrorFromCode(2)) {
        return CopyMessage(buffer, size, "Invalid argument", 0x10);
    }
    if (error == OsErrorFromCode(3)) {
        return CopyMessage(buffer, size, "Operation not supported", 0x17);
    }
    return OsErrorStringSystem(buffer, size, error);
}
