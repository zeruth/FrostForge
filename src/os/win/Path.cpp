#include "storm/ErrorReport.hpp"
#include "os/Os.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <malloc.h>
#include <windows.h>

namespace {

// DAT_142620be0: the "no error" code OsGetCurrentDirectory returns (static value 0).
const uint32_t s_osNoError = 0;

}

// ref: FUN_1402620a0
bool OsPathIsAbsolute(const char* path) {
    char c = path[0];
    if (c == '/' || c == '\\') {
        return true;
    }
    if (c != '\0' && path[1] == ':') {
        if (path[2] == '/' || path[2] == '\\') {
            return true;
        }
    }
    return false;
}

// Win32 error for a path-layer failure: 1 -> ERROR_INSUFFICIENT_BUFFER, 2 ->
// ERROR_INVALID_PARAMETER.
// ref: FUN_140267900
uint32_t OsErrorFromCode(int32_t code) {
    if (code == 0) {
        return 0;
    }
    if (code == 1) {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (code == 2) {
        return ERROR_INVALID_PARAMETER;
    }
    return 1;
}

// Copies at most destSize - 1 characters; returns the source's length.
// ref: FUN_140261bf0
uint64_t OsStrLCopy(char* dest, uint64_t destSize, const char* source) {
    uint64_t length = std::strlen(source);
    if (destSize != 0) {
        uint64_t count = length;
        if (destSize - 1 < length) {
            count = destSize - 1;
        }
        std::memcpy(dest, source, count);
        dest[count] = '\0';
    }
    return length;
}

// Appends within destSize; returns the length the result would have had.
// ref: FUN_140262380
uint64_t OsStrLCat(char* dest, uint64_t destSize, const char* source) {
    uint64_t length = std::strlen(source);
    uint64_t remaining = destSize;
    char* p = dest;
    for (;;) {
        if (remaining == 0) {
            return destSize + length;
        }
        if (*p == '\0') {
            uint64_t offset = static_cast<uint64_t>(p - dest);
            if (offset < destSize) {
                uint64_t count = (destSize - offset) - 1;
                if (length < count) {
                    count = length;
                }
                std::memcpy(p, source, count);
                p[count] = '\0';
            }
            return offset + length;
        }
        remaining--;
        p++;
    }
}

// Rewrites a path with '/' separators, dropping "." components and empty ones, resolving
// ".." against components already written (keeping leading ones), preserving a leading
// root, and lower-casing a drive letter. dest may be source.
// ref: FUN_1402620d0
void OsPathNormalize(char* dest, uint64_t destSize, const char* source) {
    int32_t written = 0;  // ordinary components written
    int32_t ups = 0;      // ".." components kept (nothing left to resolve them against)
    char* limit = dest + destSize - 1;
    char* out = dest;
    const char* scan = source;
    const char* segment = source;

    for (;;) {
        const char* cur = scan;
        char c = *cur;
        scan = cur + 1;
        if (c != '/' && c != '\\' && c != '\0') {
            continue;
        }

        bool dotdot = false;
        uint64_t length = static_cast<uint64_t>(cur - segment);
        if (length == 1) {
            if (*segment != '.') {
                goto copy;
            }
            length = 0;
            goto empty;
        }
        if (length == 2) {
            if (segment[0] != '.' || segment[1] != '.') {
                goto copy;
            }
            if (written != 0) {
                length = 0;
                do {
                    if (out <= dest) {
                        break;
                    }
                    out--;
                } while (*out != '/');
                written--;
            }
            dotdot = true;
        }
        if (length != 0) {
            goto copy;
        }

    empty:
        if (out == dest || (out == dest + 1 && *dest == '/') || c == '\0') {
            goto copy;
        }
        goto next;

    copy:
        if ((segment == source && cur == source && c != '\0')
            || (segment == source + 1 && cur == source + 1 && out == dest + 1 && *dest == '/' && c != '\0')) {
            // a leading root ("/" or the second slash of "//")
            segment = scan;
            if (out < limit) {
                *out++ = '/';
            }
            continue;
        }
        if ((written != 0 || ups != 0) && out < limit) {
            *out++ = '/';
        }
        if (length != 0) {
            for (uint64_t i = 0; i < length; i++) {
                if (out < limit) {
                    *out++ = segment[i];
                }
            }
            if (dotdot) {
                ups++;
            } else {
                written++;
            }
        }

    next:
        segment = scan;
        if (c == '\0') {
            *out = '\0';
            char drive = *dest;
            if (drive != '\0' && dest[1] == ':' && dest[2] == '/') {
                if (static_cast<uint32_t>(drive - 'A') < 26) {
                    drive += ' ';
                }
                *dest = drive;
            }
            return;
        }
    }
}

// base + "/" + relative (relative replaces base when absolute), normalized.
// ref: FUN_140261d70
void OsPathJoin(char* dest, uint64_t destSize, const char* base, const char* relative) {
    if (base && base != dest) {
        OsStrLCopy(dest, destSize, base);
    }

    char* end = dest + std::strlen(dest);
    if (dest < end) {
        if (end[-1] == '\\') {
            end[-1] = '/';
        } else if (end[-1] != '/') {
            uint64_t remaining = destSize;
            for (char* p = dest; remaining != 0; remaining--, p++) {
                if (*p == '\0') {
                    if (static_cast<uint64_t>(p - dest) < destSize) {
                        uint64_t count = (destSize - static_cast<uint64_t>(p - dest)) != 1;
                        std::memcpy(p, "/", count);
                        p[count] = '\0';
                    }
                    break;
                }
            }
        }
    }

    if (relative && *relative != '\0') {
        if (OsPathIsAbsolute(relative)) {
            *dest = '\0';
        }
        OsStrLCat(dest, destSize, relative);
    }

    // the reference inlines OsPathNormalize's body here, working in place
    OsPathNormalize(dest, destSize, dest);
}

// UTF-8 current directory, normalized. Returns 0 or a Win32 error.
// ref: FUN_140261620
uint32_t OsGetCurrentDirectory(char* buffer, uint64_t size) {
    wchar_t* wide = static_cast<wchar_t*>(_alloca((size * 2 + 0xF) & ~uint64_t(0xF)));
    DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(size), wide);
    if (length == 0) {
        return GetLastError();
    }
    if (length <= size && size != 0) {
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, static_cast<int32_t>(size), nullptr, nullptr)) {
            OsPathNormalize(buffer, size, buffer);
            return s_osNoError;
        }
    }
    return OsErrorFromCode(1);
}

// Copies a path converting '/' to '\\', at most destSize - 1 characters.
// ref: FUN_1402139f0
void OsPathToNative(const char* source, char* dest, int32_t destSize) {
    if (!dest || destSize <= 0) {
        return;
    }
    if (!source) {
        *dest = '\0';
        return;
    }
    for (;;) {
        char* at = dest;
        char c = *source++;
        destSize--;
        if (c == '\0') {
            *at = '\0';
            return;
        }
        if (c == '/') {
            c = '\\';
        }
        *at = c;
        dest = at + 1;
        if (destSize == 0) {
            *at = '\0';
            return;
        }
    }
}

// The executable's directory: argv[0] as recorded for the error reporter, without its file
// name. Copies at most size - 1 characters, always terminated.
// ref: FUN_140211520
void OsGetExecutableDirectory(char* buffer, uint64_t size) {
    const char* source = g_errorReportCommand;
    char* dest = buffer;
    if (size != 0) {
        while (--size != 0) {
            char c = *source++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
        }
        if (size == 0) {
            *dest = '\0';
        }
    }
    OsPathStripFilename(buffer);
}

// ref: FUN_140214150
void OsPathStripFilename(char* path) {
    (void)path;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1402151f0
bool OsSetCurrentDirectory(const char* path) {
    wchar_t wide[0x104];
    UnicodeConvertUtf8ToUtf16(wide, 0x104, path, 0x7FFFFFFF, nullptr, nullptr);
    return SetCurrentDirectoryW(wide) != 0;
}

// Takes a process snapshot, finds this process's parent, and copies the parent's executable
// name into the buffer (empty when not found).
// ref: FUN_141236f00
void OsGetParentProcessName(char* buffer) {
    *buffer = '\0';
    FROST_UNIMPLEMENTED();
}

// ref: FUN_140263440
uint32_t OsGetCurrentThreadId() {
    return GetCurrentThreadId();
}

// Deletes a file; the path is UTF-8.
// ref: FUN_1402148e0
bool OsDeleteFile(const char* path) {
    wchar_t wide[0x10c];
    UnicodeConvertUtf8ToUtf16(wide, 0x104, path, 0x7fffffff, nullptr, nullptr);
    return DeleteFileW(wide) != 0;
}

// Creates a directory; with createParents, every missing directory above it first (from the
// end of the path's root on). An existing directory counts as success unless failIfExists.
// ref: FUN_1402146d0
bool OsCreateDirectory(const char* path, bool createParents, bool failIfExists) {
    wchar_t wide[0x108];

    if (createParents) {
        char copy[0x110];
        char* out = copy;
        const char* in = path;
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

        uint32_t root = OsPathRootLength(copy);
        for (char* separator = std::strpbrk(copy + root, "/\\"); separator; separator = std::strpbrk(separator + 1, "/\\")) {
            char saved = *separator;
            *separator = '\0';
            UnicodeConvertUtf8ToUtf16(wide, 0x104, copy, 0x7FFFFFFF, nullptr, nullptr);
            if (!CreateDirectoryW(wide, nullptr) && (failIfExists || GetLastError() != ERROR_ALREADY_EXISTS)) {
                return false;
            }
            *separator = saved;
        }
    }

    UnicodeConvertUtf8ToUtf16(wide, 0x104, path, 0x7FFFFFFF, nullptr, nullptr);
    if (CreateDirectoryW(wide, nullptr)) {
        return true;
    }
    if (failIfExists) {
        return false;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// The length of a path's root: "C:\" or "C:/" is 3, "C:" 2, and a UNC path runs through
// the separator after its share name (all of it if there is none). Paths shorter than two
// characters, and relative ones, have none.
// ref: FUN_140213d80
uint32_t OsPathRootLength(const char* path) {
    uint64_t length = std::strlen(path);
    if (static_cast<uint32_t>(length) < 2) {
        return 0;
    }
    if (path[1] == ':') {
        if (path[2] == '\\') {
            return 3;
        }
        return (path[2] == '/') + 2;
    }
    if (path[0] == '\\' && path[1] == '\\') {
        const char* p = path + 2;
        for (int32_t i = 0; i < 2; i++) {
            if (p) {
                p = std::strchr(p, '\\') + 1;
            }
        }
        if (p) {
            return static_cast<uint32_t>(static_cast<int32_t>(p - path));
        }
        return static_cast<uint32_t>(length);
    }
    return 0;
}

// ref: FUN_140261c60
uint64_t OsStrLCopyN(char* dest, uint64_t destSize, const char* source, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        if (source[i] == '\0') {
            count = i;
            break;
        }
    }
    if (destSize != 0) {
        uint64_t copy = destSize - 1 < count ? destSize - 1 : count;
        std::memcpy(dest, source, copy);
        dest[copy] = '\0';
    }
    return count;
}

// ref: FUN_140261fd0
void OsPathGetDirectory(char* dest, uint64_t destSize, const char* path) {
    const char* separator = nullptr;
    for (const char* p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            separator = p;
        }
    }
    if (separator != nullptr) {
        OsStrLCopyN(dest, destSize, path, static_cast<uint64_t>(separator - path));
        return;
    }
    if (destSize != 0) {
        *dest = '\0';
    }
}

// ref: FUN_140262010
const char* OsPathGetFile(const char* path) {
    const char* separator = nullptr;
    if (*path != '\0') {
        for (const char* p = path; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\') {
                separator = p;
            }
        }
        if (separator != nullptr) {
            return separator + 1;
        }
    }
    return path;
}

// ref: FUN_1402140d0
void OsPathTruncateToDirectory(char* path) {
    char* cut = path;
    if (*path != '\0') {
        for (char* p = path; *p != '\0';) {
            char c = *p++;
            if (c == '/' || c == '\\') {
                cut = p;
            }
        }
        if (cut != path) {
            uint32_t root = OsPathRootLength(path);
            if (path + root <= cut) {
                *cut = '\0';
                return;
            }
            path[root] = '\0';
            return;
        }
    }
    *cut = '\0';
}

// The working directory in UTF-8, truncated to `size`. False when Windows fails.
// ref: FUN_140214d30
bool OsGetWorkingDirectory(uint64_t size, char* buffer) {
    wchar_t wide[0x108];
    if (GetCurrentDirectoryW(static_cast<DWORD>(size), wide) == 0) {
        return false;
    }
    int32_t bytes;
    UnicodeConvertUtf16ToUtf8(buffer, static_cast<uint32_t>(size), reinterpret_cast<const uint16_t*>(wide), 0x7FFFFFFF,
        &bytes, nullptr);
    uint32_t last = static_cast<uint32_t>(size) - 1;
    if (static_cast<uint32_t>(bytes) - 1 <= last) {
        last = static_cast<uint32_t>(bytes) - 1;
    }
    buffer[last] = '\0';
    return true;
}

// Writes base + relative into dest, adding a backslash between them unless base already
// ends in a separator. An empty base copies relative alone.
// ref: FUN_140213e60
void OsPathCombine(char* dest, uint64_t destSize, const char* base, const char* relative) {
    if (*base == '\0') {
        if (destSize != 0) {
            for (;;) {
                if (--destSize == 0) {
                    *dest = '\0';
                    return;
                }
                char c = *relative++;
                *dest++ = c;
                if (c == '\0') {
                    return;
                }
            }
        }
        return;
    }

    if (base != dest && destSize != 0) {
        char* out = dest;
        for (uint64_t left = destSize;;) {
            if (--left == 0) {
                *out = '\0';
                break;
            }
            char c = *base++;
            *out++ = c;
            if (c == '\0') {
                break;
            }
        }
    }
    char* end = dest + std::strlen(dest);
    bool separated = end[-1] == '/' || end[-1] == '\\';
    SStrPrintf(end, static_cast<uint64_t>(dest + destSize - end), "%s%s", separated ? "" : "\\", relative);
}

// ref: FUN_140213af0
const char* OsPathGetExtension(const char* path) {
    const char* extension = nullptr;
    for (;;) {
        char c = *path;
        if (c == '\0') {
            return extension ? extension : path;
        }
        if (c == '.') {
            extension = path + 1;
        } else if (c == '/' || c == '\\') {
            extension = nullptr;
        }
        path++;
    }
}

// ref: FUN_140213b30
const char* OsPathGetDotExtension(const char* path) {
    const char* dot = nullptr;
    for (char c; (c = *path) != '\0'; path++) {
        if (c == '.') {
            dot = path;
        } else if (c == '/' || c == '\\') {
            dot = nullptr;
        }
    }
    return dot ? dot : path;
}

// ref: FUN_140213f40
void OsPathChangeExtension(char* dest, uint64_t destSize, const char* path, const char* extension) {
    const char* dot = nullptr;
    const char* end = path;
    for (char c; (c = *end) != '\0'; end++) {
        if (c == '.') {
            dot = end;
        } else if (c == '/' || c == '\\') {
            dot = nullptr;
        }
    }
    uint32_t stem = static_cast<uint32_t>((dot ? dot : end) - path);
    if (stem < destSize) {
        if (path != dest) {
            std::memmove(dest, path, stem);
        }
        if (*extension == '\0') {
            dest[stem] = '\0';
            return;
        }
        dest[stem] = '.';
        dest += stem + 1;
        if (destSize - stem == 1) {
            return;
        }
        for (uint64_t left = destSize - stem - 2; left != 0; left--) {
            char c = *extension++;
            *dest++ = c;
            if (c == '\0') {
                return;
            }
        }
    }
    *dest = '\0';
}

// ref: FUN_140214360
bool OsGetFullPath(const char* path, char* dest, uint64_t destSize) {
    DWORD error;
    if (destSize == 0) {
        error = ERROR_INSUFFICIENT_BUFFER;
    } else {
        error = ERROR_NO_UNICODE_TRANSLATION;
        int32_t units = UnicodeCountUtf8ToUtf16(path, 0x7FFFFFFF, nullptr);
        if (units > 0) {
            auto wide = static_cast<wchar_t*>(_alloca(uint64_t(units) * 2));
            if (UnicodeConvertUtf8ToUtf16(wide, static_cast<uint32_t>(units), path, 0x7FFFFFFF, nullptr, nullptr) == 0) {
                uint32_t size = destSize < 0x7FFF ? static_cast<uint32_t>(destSize) : 0x7FFF;
                auto full = static_cast<wchar_t*>(_alloca(uint64_t(size) * 2));
                DWORD length = GetFullPathNameW(wide, size, full, nullptr);
                *dest = '\0';
                if (length == 0) {
                    if (size != 0) {
                        return false;
                    }
                    error = ERROR_INSUFFICIENT_BUFFER;
                } else if (length < size) {
                    uint32_t limit = destSize < 0xFFFFFFFF ? static_cast<uint32_t>(destSize) : 0xFFFFFFFF;
                    if (UnicodeConvertUtf16ToUtf8(dest, limit, reinterpret_cast<const uint16_t*>(full), length + 1,
                            nullptr, nullptr) == 0) {
                        return true;
                    }
                } else {
                    error = ERROR_INSUFFICIENT_BUFFER;
                }
            }
        }
    }
    SetLastError(error);
    return false;
}

// ref: FUN_1402144d0
char* OsGetErrorText(uint32_t error, char* dest, uint32_t destSize) {
    wchar_t* text = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&text), 1, nullptr);
    static wchar_t s_empty[1] = { 0 };  // DAT_142ddce88
    const wchar_t* source = length != 0 ? text : s_empty;
    int32_t written = 0;
    UnicodeConvertUtf16ToUtf8(dest, destSize, reinterpret_cast<const uint16_t*>(source), (length != 0 ? length : 0) + 1,
        &written, nullptr);
    if (length != 0) {
        LocalFree(text);
    }
    if (static_cast<uint32_t>(written - 1) > 2 && dest[written - 3] == '\r' && dest[written - 2] == '\n') {
        dest[written - 3] = '\0';
    }
    return dest;
}

// ref: FUN_1402145b0
char* OsGetLastErrorText(char* dest, uint32_t destSize) {
    return OsGetErrorText(GetLastError(), dest, destSize);
}
