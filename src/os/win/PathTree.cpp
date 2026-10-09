#include "os/Os.hpp"
#include <cstring>
#include <malloc.h>
#include <windows.h>

// Works on the path in UTF-16. Recursively, each separator (not the first of a UNC path, and
// not a trailing one) ends a prefix that is created unless it is already a directory; the
// first three characters of an absolute path ("C:/") are skipped. ERROR_ALREADY_EXISTS on
// the last component only counts as success when what exists is a directory.
// ref: FUN_1402613a0
uint32_t OsCreateDirectoryTree(const char* path, bool recursive) {
    uint64_t length = std::strlen(path);
    auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x3FFFFFFF);
    if (recursive) {
        wchar_t* at = OsPathIsAbsolute(path) ? wide + 3 : wide;
        bool unc = std::strncmp(path, "\\\\", 2) == 0 || std::strncmp(path, "//", 2) == 0;
        bool first = true;
        for (wchar_t c = *at; c != 0; c = *++at) {
            if ((c == L'/' || c == L'\\') && at[1] != 0) {
                if (!first || !unc) {
                    *at = 0;
                    uint32_t attributes = GetFileAttributesW(wide);
                    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                        if (!CreateDirectoryW(wide, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                            return GetLastError();
                        }
                    }
                    *at = c;
                }
                first = false;
            }
        }
    }
    if (!CreateDirectoryW(wide, nullptr)) {
        uint32_t error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return error;
        }
        uint32_t attributes = GetFileAttributesW(wide);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return ERROR_ALREADY_EXISTS;
        }
    }
    return 0;
}
