#include "os/Os.hpp"
#include "storm/String.hpp"

#include <windows.h>
#include <malloc.h>
#include <cstring>

// A named page-file mapping shared between processes. Modes: 0 create (fails when it exists),
// 1 open for writing, 2 create or open, 3 open for reading. Returns a Win32 error, 0 on
// success; `created` says whether this call made it.
// ref: FUN_1402641c0
uint32_t OsSharedMemoryOpen(OSSHAREDMEMORY* memory, const char* name, int32_t mode, uint64_t size, bool* created) {
    if (memory->m_data != nullptr || memory->m_size != 0 || memory->m_mapping != nullptr) {
        return OsErrorFromResult(2);
    }
    uint64_t length = std::strlen(name);
    if (length > 0x3f) {
        return OsErrorFromResult(2);
    }
    // No path separators: a backslash anywhere, or a slash, makes the name invalid.
    for (const char* c = name; *c != '\0'; c++) {
        if (*c == '\\' || *c == '/') {
            return OsErrorFromResult(2);
        }
    }
    char copy[0x40];
    std::memcpy(copy, name, length);
    copy[length] = '\0';
    auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 0x11) & ~static_cast<uint64_t>(0xf)));
    MultiByteToWideChar(CP_UTF8, 0, copy, -1, wide, 0x3fffffff);
    bool made = false;
    HANDLE mapping;
    if (mode == 0 || mode == 2) {
        if (size == 0) {
            return OsErrorFromResult(2);
        }
        made = true;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
            static_cast<DWORD>(size >> 32), static_cast<DWORD>(size), wide);
    } else if (mode == 1) {
        mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, wide);
    } else if (mode == 3) {
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, wide);
    } else {
        return OsErrorFromResult(2);
    }
    uint32_t error = GetLastError();
    if (mapping == nullptr) {
        return error;
    }
    if (error == ERROR_ALREADY_EXISTS) {
        if (mode == 0) {
            CloseHandle(mapping);
            return ERROR_ALREADY_EXISTS;
        }
        made = false;
    }
    memory->m_mapping = mapping;
    memory->m_data = MapViewOfFile(mapping, mode == 3 ? FILE_MAP_READ : FILE_MAP_WRITE, 0, 0, 0);
    if (memory->m_data == nullptr) {
        error = GetLastError();
        OsSharedMemoryClose(memory);
        return error;
    }
    if (!made) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(memory->m_data, &info, sizeof(info)) == 0) {
            error = GetLastError();
            OsSharedMemoryClose(memory);
            return error;
        }
        size = info.RegionSize;
    }
    memory->m_size = size;
    if (created != nullptr) {
        *created = made;
    }
    return 0;
}

// ref: FUN_140264150
uint32_t OsSharedMemoryClose(OSSHAREDMEMORY* memory) {
    uint32_t error = 0;
    if (memory->m_data != nullptr && !UnmapViewOfFile(memory->m_data)) {
        error = GetLastError();
    }
    if (memory->m_mapping != nullptr && !CloseHandle(memory->m_mapping) && error == 0) {
        error = GetLastError();
    }
    memory->m_data = nullptr;
    memory->m_size = 0;
    memory->m_mapping = nullptr;
    return error;
}

// "<name>.<id in hex>", opened with OsSharedMemoryOpen; true when that failed.
// ref: FUN_1412396f0
bool OsSharedMemoryOpenNamed(OSSHAREDMEMORY* memory, const char* name, uint32_t id, uint32_t size, int32_t mode) {
    char full[0x100];
    SStrPrintf(full, sizeof(full), "%s.%x", name, id);
    if (mode < 0 || mode > 3) {
        return true;
    }
    return OsSharedMemoryOpen(memory, full, mode, size, nullptr) != 0;
}
