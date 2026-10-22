// The packages' file and directory helpers beyond open/read/close (0x140261320..0x140266ec0):
// a directory iterator, deletion, positioned reads and writes, the allocation granularity.

#include "os/Os.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include <cstdarg>
#include <cstring>
#include <malloc.h>
#include <windows.h>

namespace {

// DAT_142620be0: the packages' "no error" code.
const uint32_t s_osNoError = 0;

int64_t s_allocationGranularity = -1;  // DAT_142cc8ad8

wchar_t* WidePath(const char* path, wchar_t* buffer) {
    MultiByteToWideChar(CP_UTF8, 0, path, -1, buffer, 0x3FFFFFFF);
    return buffer;
}

}

// Generic results as OS error codes: 1 ERROR_INSUFFICIENT_BUFFER, 2 ERROR_INVALID_PARAMETER.
// ref: FUN_140267900
uint32_t OsErrorFromResult(int32_t result) {
    if (result == 0) {
        return 0;
    }
    if (result != 1) {
        if (result != 2) {
            return 1;
        }
        return ERROR_INVALID_PARAMETER;
    }
    return ERROR_INSUFFICIENT_BUFFER;
}

// ref: FUN_140261320
void OsDirInit(OSDIRITER* iter) {
    std::memset(iter->m_find, 0, sizeof(iter->m_find));
    iter->m_handle = INVALID_HANDLE_VALUE;
    std::memset(iter->m_name, 0, sizeof(iter->m_name));
    iter->m_empty = 0;
}

// Starts iterating `path` (a missing pattern means every entry). A directory that exists but
// has no entries is not an error.
// ref: FUN_1402618b0
uint32_t OsDirOpen(OSDIRITER* iter, const char* path) {
    iter->m_handle = INVALID_HANDLE_VALUE;
    iter->m_empty = 0;
    uint64_t length = std::strlen(path);
    char last = length == 0 ? '\0' : path[length - 1];
    int32_t count = MultiByteToWideChar(CP_UTF8, 0, path, static_cast<int32_t>(length), nullptr, 0);
    if (count > 0x7FFF) {
        return OsErrorFromResult(2);
    }
    auto wide = static_cast<wchar_t*>(_alloca((uint64_t(count + 3) * 2 + 0xF) & ~uint64_t(0xF)));
    count = MultiByteToWideChar(CP_UTF8, 0, path, static_cast<int32_t>(length), wide, count + 3);
    if (last != '\\' && last != '/') {
        wide[count++] = L'\\';
    }
    wide[count] = L'*';
    wide[count + 1] = 0;
    HANDLE handle = FindFirstFileW(wide, reinterpret_cast<WIN32_FIND_DATAW*>(iter->m_find));
    iter->m_handle = handle;
    iter->m_name[0] = '\0';
    if (handle == INVALID_HANDLE_VALUE) {
        uint32_t error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            return error;
        }
        if (!OsDirExists(path)) {
            return ERROR_FILE_NOT_FOUND;
        }
        iter->m_empty = 1;
    }
    return s_osNoError;
}

// The next entry other than "." and "..": its UTF-8 name and whether it is a directory.
// ref: FUN_1402617e0
bool OsDirNext(OSDIRITER* iter, OSDIRENTRY* entry) {
    if (iter->m_empty) {
        return false;
    }
    auto find = reinterpret_cast<WIN32_FIND_DATAW*>(iter->m_find);
    do {
        if (iter->m_name[0] != '\0') {
            if (!FindNextFileW(iter->m_handle, find)) {
                return false;
            }
        }
        if (!WideCharToMultiByte(CP_UTF8, 0, find->cFileName, -1, iter->m_name, sizeof(iter->m_name), nullptr, nullptr)) {
            return false;
        }
    } while (iter->m_name[0] == '.' &&
        (iter->m_name[1] == '\0' || (iter->m_name[1] == '.' && iter->m_name[2] == '\0')));
    entry->m_name = iter->m_name;
    entry->m_directory = (find->dwFileAttributes >> 4) & 1;
    return true;
}

// ref: FUN_140261370
void OsDirClose(OSDIRITER* iter) {
    if (iter->m_handle != INVALID_HANDLE_VALUE) {
        FindClose(iter->m_handle);
        iter->m_handle = INVALID_HANDLE_VALUE;
    }
}

// The last '.' of the last component (not a leading one), or the end of the string.
// ref: FUN_140262050
const char* OsPathExtension(const char* path) {
    const char* dot = nullptr;
    bool start = true;
    for (; *path != '\0'; path++) {
        char c = *path;
        if (c == '/' || c == '\\') {
            dot = nullptr;
            start = true;
        } else if (c != '.' || start) {
            start = false;
        } else {
            start = false;
            dot = path;
        }
    }
    return dot ? dot : path;
}

// ref: FUN_140262270
void OsPathStripExtension(char* path) {
    *const_cast<char*>(OsPathExtension(path)) = '\0';
}

// Deletes a file, clearing a read-only attribute if that is what stopped it.
// ref: FUN_140266640
uint32_t OsFileDelete(const char* path) {
    auto wide = WidePath(path, static_cast<wchar_t*>(_alloca((std::strlen(path) * 2 + 2 + 0xF) & ~uint64_t(0xF))));
    if (!DeleteFileW(wide)) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(wide)) {
                return s_osNoError;
            }
        }
        return GetLastError();
    }
    return s_osNoError;
}

// ref: FUN_140261580
bool OsDirExists(const char* path) {
    auto wide = WidePath(path, static_cast<wchar_t*>(_alloca((std::strlen(path) * 2 + 2 + 0xF) & ~uint64_t(0xF))));
    uint32_t attributes = GetFileAttributesW(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ref: FUN_1402149a0
bool OsFileExists(const char* path) {
    if (path != nullptr && *path != '\0') {
        wchar_t wide[0x10c];
        UnicodeConvertUtf8ToUtf16(wide, 0x104, path, 0x7FFFFFFF, nullptr, nullptr);
        uint32_t attributes = GetFileAttributesW(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return true;
        }
    }
    return false;
}

// A synchronous read at an offset; the end of the file is not an error.
// ref: FUN_1402665c0
uint32_t OsFileReadAt(OSFILE* file, void* buffer, uint32_t bytes, uint64_t offset, uint32_t* read) {
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<uint32_t>(offset);
    overlapped.OffsetHigh = static_cast<uint32_t>(offset >> 32);
    DWORD done;
    if (!ReadFile(file->m_handle, buffer, bytes, &done, &overlapped)) {
        if (GetLastError() != ERROR_HANDLE_EOF) {
            *read = 0;
            return GetLastError();
        }
    }
    *read = done;
    return s_osNoError;
}

// ref: FUN_140266a50
uint32_t OsFileWrite(OSFILE* file, const void* buffer, uint32_t bytes, uint32_t* written) {
    DWORD done;
    if (!WriteFile(file->m_handle, buffer, bytes, &done, nullptr)) {
        *written = 0;
        return GetLastError();
    }
    *written = done;
    return s_osNoError;
}

// ref: FUN_140266aa0
uint32_t OsFileWriteAt(OSFILE* file, const void* buffer, uint32_t bytes, uint64_t offset, uint32_t* written) {
    OVERLAPPED overlapped = {};
    overlapped.Offset = static_cast<uint32_t>(offset);
    overlapped.OffsetHigh = static_cast<uint32_t>(offset >> 32);
    DWORD done;
    if (!WriteFile(file->m_handle, buffer, bytes, &done, &overlapped)) {
        *written = 0;
        return GetLastError();
    }
    *written = done;
    return s_osNoError;
}

// ref: FUN_1402661f0
uint64_t OsFileTell(OSFILE* file) {
    LARGE_INTEGER position;
    LARGE_INTEGER zero = {};
    SetFilePointerEx(file->m_handle, zero, &position, FILE_CURRENT);
    return static_cast<uint64_t>(position.QuadPart);
}

// Takes over `source`'s handle, closing the one held.
// ref: FUN_140265f70
OSFILE* OsFileMove(OSFILE* dest, OSFILE* source) {
    if (dest->m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(dest->m_handle);
        dest->m_handle = INVALID_HANDLE_VALUE;
    }
    void* handle = source->m_handle;
    source->m_handle = INVALID_HANDLE_VALUE;
    dest->m_handle = handle;
    return dest;
}

// ref: FUN_1402636c0
void OsSleepNanoseconds(int64_t nanoseconds) {
    uint64_t ms = static_cast<uint64_t>(nanoseconds) / 1000000;
    if ((ms & 0xFFFFFFFF) != ms) {
        ms = 0xFFFFFFFF;
    }
    Sleep(static_cast<DWORD>(ms));
}

// ref: FUN_140255c70
uint32_t OsGetProcessId() {
    return GetCurrentProcessId();
}

// ref: FUN_140266ec0
uint64_t OsGetAllocationGranularity() {
    if (s_allocationGranularity < 0) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        s_allocationGranularity = info.dwAllocationGranularity;
    }
    return static_cast<uint64_t>(s_allocationGranularity);
}

// snprintf over Storm's printf engine (SStrVSnprintf, 0x140265e00).
// ref: 0x14155a060
int32_t OsSnprintf(char* dest, uint64_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int32_t length = SStrVSnprintf(dest, size, format, args);
    va_end(args);
    return length;
}

// A file mapping (0x28 bytes): the view (offset into the mapped range), its length, the offset
// the view had to be moved back by for alignment, the mapping and a duplicate of the file
// handle.
// ref: FUN_142368460
void OsMappingInit(OSMAPPING* mapping) {
    mapping->m_file = INVALID_HANDLE_VALUE;
    mapping->m_view = nullptr;
    mapping->m_size = 0;
    mapping->m_adjust = 0;
    mapping->m_mapping = nullptr;
}

// Maps [offset, offset + size) of the file (size 0 for the rest of it), writable if asked.
// ref: FUN_142368630
uint32_t OsMappingOpen(OSMAPPING* mapping, OSFILE* file, bool writable, uint64_t offset, uint64_t size) {
    if (mapping->m_view || mapping->m_size || mapping->m_mapping || mapping->m_file != INVALID_HANDLE_VALUE ||
        file->m_handle == INVALID_HANDLE_VALUE) {
        return OsErrorFromResult(2);
    }
    uint64_t fileSize = OsFileGetSize(file);
    if (fileSize == 0 || size > fileSize || offset > fileSize - size || offset == fileSize) {
        return OsErrorFromResult(2);
    }
    if (size == 0) {
        size = fileSize - offset;
    }
    uint64_t granularity = OsGetAllocationGranularity();
    mapping->m_size = size;
    uint64_t aligned = offset & ~(granularity - 1);
    size += offset - aligned;
    mapping->m_adjust = offset - aligned;
    mapping->m_mapping = CreateFileMappingW(file->m_handle, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY,
        static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), nullptr);
    if (mapping->m_mapping) {
        void* view = MapViewOfFile(mapping->m_mapping, writable ? FILE_MAP_WRITE : FILE_MAP_READ,
            static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned), size);
        mapping->m_view = view;
        if (view) {
            mapping->m_view = static_cast<uint8_t*>(view) + mapping->m_adjust;
            DuplicateHandle(GetCurrentProcess(), file->m_handle, GetCurrentProcess(), &mapping->m_file, 0, FALSE,
                DUPLICATE_SAME_ACCESS);
            return s_osNoError;
        }
        CloseHandle(mapping->m_mapping);
        mapping->m_mapping = nullptr;
        mapping->m_size = 0;
        mapping->m_adjust = 0;
    }
    return GetLastError();
}

// ref: FUN_142368500
uint32_t OsMappingClose(OSMAPPING* mapping) {
    uint32_t error = 0;
    if (mapping->m_view) {
        if (!UnmapViewOfFile(static_cast<uint8_t*>(mapping->m_view) - mapping->m_adjust)) {
            error = GetLastError();
        }
    }
    if (mapping->m_mapping) {
        if (!CloseHandle(mapping->m_mapping) && error == 0) {
            error = GetLastError();
        }
    }
    if (mapping->m_file != INVALID_HANDLE_VALUE) {
        if (!CloseHandle(mapping->m_file) && error == 0) {
            error = GetLastError();
        }
    }
    mapping->m_view = nullptr;
    mapping->m_size = 0;
    mapping->m_adjust = 0;
    mapping->m_mapping = nullptr;
    mapping->m_file = INVALID_HANDLE_VALUE;
    return error;
}

// ref: FUN_1402651b0
void OsTerminateProcess() {
    TerminateProcess(GetCurrentProcess(), 1);
}

// ref: FUN_140255c90
void OsSetProcessAffinity(uint32_t mask) {
    SetProcessAffinityMask(GetCurrentProcess(), mask);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cc8ad8 = (void*)&s_allocationGranularity;
// ---- end of generated globals ----
