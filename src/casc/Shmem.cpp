#include "casc/Shmem.hpp"
#include "casc/Dynamic.hpp"
#include "blz/Log.hpp"
#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <windows.h>

namespace casc {

namespace {

const char s_blockFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\shmem_control_block.cpp";
const char s_win32File[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\shmem_control.win32.cpp";
const char s_pidFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\pid_tracking.cpp";
const char s_driveFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\drive_type.win32.cpp";

const uint32_t HEADER_TAG = 0x2AB8;

// The size classes of free spans (the table at 0x1428f4230): 0x40, 0x60, 0x80, 0xc0, ...
uint64_t SizeClassLimit(uint32_t index) {
    uint64_t base = (index & 1) ? 0x60 : 0x40;
    return base << (index >> 1);
}

uint32_t SizeClass(uint64_t size) {
    uint32_t index = 0;
    while (index < 0x34 && size >= SizeClassLimit(index)) {
        index++;
    }
    return index - 1;
}

uint64_t ReadOffset40(const uint8_t* p) {
    return uint64_t(p[0]) << 32 | uint64_t(p[1]) << 24 | uint64_t(p[2]) << 16 | uint64_t(p[3]) << 8 | p[4];
}

void WriteBE40(uint8_t* p, uint64_t value) {
    p[0] = static_cast<uint8_t>(value >> 32);
    p[1] = static_cast<uint8_t>(value >> 24);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 8);
    p[4] = static_cast<uint8_t>(value);
}

// "Global\" + the container path.
// ref: FUN_1421581b0
int32_t MutexName(char* out, uint32_t size, const char* path) {
    (void)size;
    uint64_t length = std::strlen(path);
    uint32_t total = static_cast<uint32_t>(length) + 7;
    if (total > 0xFF) {
        blz::log(g_logCategory, 4, s_blockFile, 0x32, "Container path '%s' is too long to support.", path);
        return ERR_PATH_TOO_LONG;
    }
    std::memcpy(out, "Global\\", 7);
    std::memcpy(out + 7, path, length);
    out[total] = '\0';
    return ERR_SUCCESS;
}

// Opens (creating) the shmem file for reading and writing, shared.
// ref: FUN_142166bc0
uint32_t OpenShmemFile(OSFILE* file, const char* path) {
    auto wide = static_cast<wchar_t*>(_alloca((std::strlen(path) * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x3FFFFFFF);
    file->m_handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file->m_handle == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    return 0;
}

// ref: FUN_140265fc0
bool FileExists(const char* path) {
    auto wide = static_cast<wchar_t*>(_alloca((std::strlen(path) * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x3FFFFFFF);
    uint32_t attributes = GetFileAttributesW(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Grows a file with zeros, or truncates it, keeping the file pointer.
// ref: FUN_140266840
uint32_t SetFileSize(OSFILE* file, uint64_t size) {
    LARGE_INTEGER zero = {};
    LARGE_INTEGER position;
    SetFilePointerEx(file->m_handle, zero, &position, FILE_CURRENT);
    LARGE_INTEGER current;
    uint64_t length = GetFileSizeEx(file->m_handle, &current) ? static_cast<uint64_t>(current.QuadPart) : 0;
    uint32_t error = 0;
    if (length < size) {
        LARGE_INTEGER to;
        to.QuadPart = static_cast<LONGLONG>(length);
        error = SetFilePointerEx(file->m_handle, to, nullptr, FILE_BEGIN) ? 0 : GetLastError();
        if (error == 0) {
            uint64_t remaining = size - length;
            uint8_t zeros[0x1000];
            std::memset(zeros, 0, remaining < 0x1000 ? remaining : 0x1000);
            while (remaining != 0) {
                uint32_t chunk = remaining > 0xFFF ? 0x1000 : static_cast<uint32_t>(remaining);
                DWORD written;
                if (!WriteFile(file->m_handle, zeros, chunk, &written, nullptr)) {
                    error = GetLastError();
                    break;
                }
                remaining -= written;
            }
        }
    } else {
        LARGE_INTEGER to;
        to.QuadPart = static_cast<LONGLONG>(size);
        error = SetFilePointerEx(file->m_handle, to, nullptr, FILE_BEGIN) ? 0 : GetLastError();
        if (error == 0) {
            error = SetEndOfFile(file->m_handle) ? 0 : GetLastError();
        }
    }
    SetFilePointerEx(file->m_handle, position, nullptr, FILE_BEGIN);
    return error;
}

// ref: FUN_142368490
void MappingMove(OSMAPPING* dest, OSMAPPING* source) {
    if (dest->m_view) {
        OsMappingClose(dest);
    }
    *dest = *source;
    source->m_view = nullptr;
    source->m_size = 0;
    source->m_adjust = 0;
    source->m_mapping = nullptr;
    source->m_file = INVALID_HANDLE_VALUE;
}

// Maps the shmem file under the protection of an exclusive "<shmem>.lock" file (deleted on
// close, waited for up to five seconds). The first process to map it - the one whose delete
// of the old file succeeded - sizes it and writes the header.
// ref: FUN_142166ca0
int32_t ShmemMap(const char* path, uint32_t size, int32_t mode, const void* header, uint32_t headerSize,
    STATSINK* stats, SHMEMBINDING* binding) {
    OSFILE lock;
    OsFileInit(&lock);
    uint64_t length = std::strlen(path);
    char lockPath[0x30C + 8];
    std::memcpy(lockPath, path, length);
    std::memcpy(lockPath + length, ".lock", 6);
    wchar_t wideLock[0x30C];
    int32_t result;
    if (!MultiByteToWideChar(CP_UTF8, 0, lockPath, -1, wideLock, 0x30C)) {
        uint32_t error = GetLastError();
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_win32File, 0x52, "Failed to convert path '%s' to wide string. Error (%u): %s",
            lockPath, error, text);
        return ERR_UNSPECIFIED;
    }
    DeleteFileW(wideLock);
    lock.m_handle = CreateFileW(wideLock, GENERIC_READ, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lock.m_handle == INVALID_HANDLE_VALUE) {
        int64_t remaining = 5000000000;
        do {
            if (remaining == 0) {
                blz::log(g_logCategory, 4, s_win32File, 0x74, "Failed to create lock file due to timeout.");
                OsFileClose(&lock);
                return ERR_LOCKED;
            }
            int64_t step = remaining > 100000000 ? 100000000 : remaining;
            remaining -= step;
            Sleep(static_cast<DWORD>(step / 1000000));
            DeleteFileW(wideLock);
            lock.m_handle = CreateFileW(wideLock, GENERIC_READ, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        } while (lock.m_handle == INVALID_HANDLE_VALUE);
    }

    OSMAPPING mapping;
    OsMappingInit(&mapping);
    uint32_t attempt = 0;
    do {
        OsFileDelete(path);
        bool existed = FileExists(path);
        OSFILE file;
        OsFileInit(&file);
        uint32_t error = OpenShmemFile(&file, path);
        int32_t line;
        const char* message;
        if (error == 0) {
            if (!existed && (error = SetFileSize(&file, size)) != 0) {
                line = 0x96;
                message = "Failed to resize shmem control file '%s'. Error (%u): %s";
            } else {
                error = OsMappingOpen(&mapping, &file, true, 0, 0);
                if (error == 0) {
                    OsFileClose(&file);
                    if (attempt != 0) {
                        char name[0x100];
                        OsSnprintf(name, 0x80, "%s.%u", "dynamic_container.shmem_ctrl.bind.retry_succeeded", attempt);
                        StatsReport(stats, 0, name, 1, 1.0f);
                    }
                    if (mode == 4 || mode == 5) {
                        if (existed) {
                            blz::log(g_logCategory, 2, s_win32File, 0xd6,
                                "Failed to get exclusive access since we are not first to bind.");
                            result = ERR_LOCKED;
                            break;
                        }
                    } else if (existed) {
                        MappingMove(&binding->m_mapping, &mapping);
                        result = ERR_SUCCESS;
                        break;
                    }
                    std::memcpy(mapping.m_view, header, headerSize);
                    std::memset(static_cast<uint8_t*>(mapping.m_view) + headerSize, 0, mapping.m_size - headerSize);
                    binding->m_created = 1;
                    MappingMove(&binding->m_mapping, &mapping);
                    result = ERR_SUCCESS;
                    break;
                }
                line = 0xa1;
                message = "Failed to map shmem control file '%s'. Error (%u): %s";
            }
        } else {
            line = 0x8a;
            message = "Failed to open shmem control file '%s'. Error (%u): %s";
        }
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_win32File, line, message, path, error, text);
        result = error == 5 ? 0x14 : (error != 0x70 ? 8 : 7);
        OsFileClose(&file);
    } while (++attempt < 10);
    OsMappingClose(&mapping);
    OsFileClose(&lock);
    return result;
}

// Lays out the header for the version asked for and maps "<path>/shmem".
// ref: FUN_142157f20
int32_t ShmemCreate(const char* path, int32_t mode, STATSINK* stats, SHMEMBINDING* binding, SHMEMVERSION* version) {
    char file[0x30C];
    OsPathJoin(file, sizeof(file), path, "shmem");
    uint8_t buffer[0x258];
    auto header = reinterpret_cast<SHMEMHEADER*>(buffer);
    if (version->m_version == 4) {
        std::memset(buffer, 0, sizeof(buffer));
        header->m_version = 4;
        header->m_headerSize = 0x150;
        int32_t error = MutexName(header->m_mutexName, 0x100, path);
        if (error != 0) {
            return error;
        }
        header->m_freeSpaceFormat = HEADER_TAG;
        header->m_freeSpaceOffset = 0x150;
        return ShmemMap(file, 0x2C10, mode, buffer, 0x150, stats, binding);
    }
    if (version->m_version == 5) {
        std::memset(buffer, 0, sizeof(buffer));
        header->m_flags = version->m_flags & ~1u;
        header->m_headerSize = 0x154;
        version->m_flags = header->m_flags;
        uint32_t headerSize = header->m_flags != 0 ? 600 : 0x154;
        header->m_version = 5;
        if (mode == 4 || mode == 5) {
            header->m_flags |= 1;
        }
        int32_t error = MutexName(header->m_mutexName, 0x100, path);
        if (error != 0) {
            return error;
        }
        header->m_freeSpaceFormat = HEADER_TAG;
        header->m_freeSpaceOffset = (headerSize + 0xFFF) & 0xFFFFF000;
        uint32_t size = (header->m_freeSpaceOffset + 0x3AB7) & 0xFFFFF000;
        if (((version->m_flags >> 1) & 1) != 0 && (version->m_extraTables & 1) != 0) {
            header->m_tableSizes[0] = 0x228;
            header->m_tableOffsets[0] = size;
            header->m_tableFlags |= 1;
            size = (size + 0x1227) & 0xFFFFF000;
        }
        return ShmemMap(file, size, mode, buffer, headerSize, stats, binding);
    }
    return ERR_INVALID_PARAM;
}

}

// GetDriveType as casc's drive types: 0 unknown, 1 remote, 2 fixed, 3 removable, 4 RAM.
// ref: FUN_14215cf20
int32_t DriveType(const char* path) {
    wchar_t wide[0x30C];
    std::memset(wide, 0, sizeof(wide));
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x30C)) {
        return 0;
    }
    wchar_t full[0x30C];
    DWORD length = GetFullPathNameW(wide, 0x30C, full, nullptr);
    if (length - 1 >= 0x30B) {
        blz::log(g_logCategory, 4, s_driveFile, 0x1a, "Failed to get full path name for path '%s'.", path);
        return 0;
    }
    wchar_t volume[0x30C];
    if (!GetVolumePathNameW(full, volume, 0x30C)) {
        uint32_t error = GetLastError();
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_driveFile, 0x25, "Failed to get volume name for path '%s'. Error (%u): %s", path,
            error, text);
        return 0;
    }
    switch (GetDriveTypeW(volume)) {
        case DRIVE_REMOVABLE:
        case DRIVE_CDROM:
            return 3;
        case DRIVE_FIXED:
            return 2;
        case DRIVE_REMOTE:
            return 1;
        case DRIVE_RAMDISK:
            return 4;
        default:
            return 0;
    }
}

// Maps the shmem file and checks its header; another process holding it exclusively, or a
// header not yet written, asks the caller to retry.
// ref: FUN_1421582c0 (reached through FUN_142158270, which asks for version 5 with the process table)
int32_t ShmemBind(blz::allocator* allocator, const char* path, int32_t mode, STATSINK* stats, SHMEMBINDING* binding,
    bool* retry) {
    (void)allocator;
    SHMEMVERSION version = { 5, 2, 1 };
    *retry = false;
    int32_t drive = DriveType(path);
    if (drive == 1) {
        blz::log(g_logCategory, 4, s_blockFile, 0x11c, "Invalid drive type for shared memory at '%s'", path);
        return ERR_INVALID_DRIVE_TYPE;
    }
    if (drive == 0) {
        blz::log(g_logCategory, 3, s_blockFile, 0x123, "Unable to determine drive type for '%s'.", path);
    }
    int32_t error = ShmemCreate(path, mode, stats, binding, &version);
    if (error != 0) {
        return error;
    }
    auto header = static_cast<SHMEMHEADER*>(binding->m_mapping.m_view);
    if (static_cast<uint32_t>(mode - 4) < 2) {
        binding->m_header = header;
        binding->m_freeSpace = reinterpret_cast<FREESPACETABLE*>(reinterpret_cast<uint8_t*>(header) + header->m_freeSpaceOffset);
        return ERR_SUCCESS;
    }
    uint32_t headerVersion = header->m_version;
    int32_t line;
    if (headerVersion < 4) {
        line = 0x13f;
    } else if (headerVersion < 6) {
        if (header->m_freeSpaceFormat == HEADER_TAG) {
            if (headerVersion == 5 && (header->m_flags & 1) != 0) {
                blz::log(g_logCategory, 2, s_blockFile, 0x156,
                    "Unable to bind container while another process has exclusive access.");
            } else {
                if (header->m_freeSpaceOffset != 0 && header->m_mutexName[0] != '\0') {
                    binding->m_header = header;
                    binding->m_freeSpace =
                        reinterpret_cast<FREESPACETABLE*>(reinterpret_cast<uint8_t*>(header) + header->m_freeSpaceOffset);
                    return ERR_SUCCESS;
                }
                blz::log(g_logCategory, 3, s_blockFile, 0x15e, "Detected invalid shared memory initialization.");
                *retry = true;
            }
            error = ERR_LOCKED;
        } else {
            blz::log(g_logCategory, 4, s_blockFile, 0x14d, "Detected unsupported free space table format.");
            error = ERR_UNSUPPORTED;
        }
        OsMappingClose(&binding->m_mapping);
        binding->m_header = nullptr;
        binding->m_freeSpace = nullptr;
        return error;
    } else {
        line = 0x145;
    }
    blz::log(g_logCategory, 4, s_blockFile, line, "Unsupported shmem protocol version. '%u'", headerVersion);
    OsMappingClose(&binding->m_mapping);
    binding->m_header = nullptr;
    binding->m_freeSpace = nullptr;
    return ERR_UNSUPPORTED;
}

// ref: FUN_142158520
void ShmemUnbind(SHMEMBINDING* binding) {
    OsMappingClose(&binding->m_mapping);
    binding->m_header = nullptr;
    binding->m_freeSpace = nullptr;
}

// ref: FUN_142157dd0
bool PidTableValid(PIDTABLE* table) {
    return table->m_state - 1 < 2;
}

// ref: FUN_142157d40
void PidTableInit(PIDTABLE* table, uint32_t size) {
    uint32_t capacity = (size - 0x28) >> 3;
    table->m_capacity = capacity;
    table->m_pidOffset = 0x28 - 0x1c;
    table->m_capacity2 = capacity;
    table->m_modeOffset = static_cast<int32_t>(capacity * 4 + 4);
    std::memset(table->Pids(), 0, uint64_t(capacity) * 4);
    table->m_state = 1;
}

// Recounts after a process died while writing the table.
// ref: FUN_142157eb0
void PidTableRecount(PIDTABLE* table) {
    table->Pids()[table->m_lastSlot] = 0;
    uint32_t count = 0;
    uint32_t writers = 0;
    for (uint32_t i = 0; i < table->m_capacity; i++) {
        if (table->Pids()[i] != 0) {
            count++;
            if (table->Modes()[i] != 2) {
                writers++;
            }
        }
    }
    table->m_count = count;
    table->m_writers = writers;
    table->m_state = 1;
}

// ref: FUN_142157c20
int32_t PidTableAdd(PIDTABLE* table, SHMEMBINDING* binding, int32_t mode) {
    if (table->m_state == 2) {
        PidTableRecount(table);
    }
    int32_t line;
    if (table->m_count < table->m_capacity) {
        uint32_t pid = OsGetProcessId();
        uint32_t slot = 0;
        while (slot < table->m_capacity && table->Pids()[slot] != 0) {
            slot++;
        }
        if (slot != table->m_capacity) {
            table->m_lastSlot = slot;
            table->m_state = 2;
            table->m_count++;
            table->Pids()[slot] = pid;
            table->Modes()[slot] = mode;
            if (mode != 2) {
                table->m_writers++;
            }
            table->m_generation++;
            binding->m_slot = slot;
            binding->m_generation = table->m_generation;
            table->m_state = 1;
            return ERR_SUCCESS;
        }
        line = 0x56;
    } else {
        line = 0x47;
    }
    blz::log(g_logCategory, 4, s_pidFile, line, "The maximum number of casc process has been bound to the current container.");
    return ERR_LOCKED;
}

// ref: FUN_142157de0
void PidTableRemove(PIDTABLE* table, SHMEMBINDING* binding, int32_t mode) {
    if (table->m_state == 2) {
        PidTableRecount(table);
    }
    uint32_t slot = binding->m_slot;
    if (OsGetProcessId() != table->Pids()[slot]) {
        blz::log(g_logCategory, 4, s_pidFile, 0x9a, "Trying to remove a pid for which we do not own.");
        return;
    }
    table->m_lastSlot = slot;
    table->m_state = 2;
    table->Pids()[slot] = 0;
    table->m_count--;
    if (mode != 2) {
        table->m_writers--;
    }
    table->m_state = 1;
}

// Walks the gaps between sorted used spans, cut at 1 GB archive boundaries, and counts
// (pass 0) or records (pass 1) those of at least 0x40 bytes by size class.
// ref: FUN_14215d630
void FreeSpaceFill(FREESPACETABLE* table, uint8_t** spans, uint32_t count, bool emit, int32_t* classes) {
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* span = spans[i];
        uint64_t offset = ReadOffset40(span);
        uint32_t size;
        std::memcpy(&size, span + 5, 4);
        uint64_t start;
        while ((start = cursor) < offset) {
            cursor = (start + 0x40000000) & 0xFFFFFFFFC0000000ull;
            if (cursor <= start || offset < cursor) {
                cursor = offset;
            }
            uint64_t gap = cursor - start;
            if (gap > 0x3F) {
                uint32_t sizeClass = SizeClass(gap);
                if (!emit) {
                    classes[sizeClass]++;
                } else if (classes[sizeClass] != 0) {
                    classes[sizeClass]--;
                    WriteBE40(table->m_sizes[table->m_count], gap);
                    WriteBE40(table->m_offsets[table->m_count], start);
                    table->m_count++;
                }
            }
        }
        cursor = size + offset;
        if (size + offset <= start) {
            cursor = start;
        }
    }
}

// Sorts the used spans, counts the free ones by size class, keeps the largest classes up to
// the table's capacity, and records them.
// ref: FUN_14215d3a0
void FreeSpaceRebuild(FREESPACETABLE* table, uint8_t** spans, uint32_t count) {
    // FUN_140576a60 is std::sort over the span pointers with a 9-byte memcmp (0x14215da10).
    std::sort(spans, spans + count, [](const uint8_t* a, const uint8_t* b) {
        return std::memcmp(a, b, 9) < 0;
    });
    int32_t classes[0x34];
    std::memset(classes, 0, sizeof(classes));
    FreeSpaceFill(table, spans, count, false, classes);
    uint32_t budget = FREESPACE_MAX;
    for (int32_t i = 0x33; i >= 0; i--) {
        uint32_t take = static_cast<uint32_t>(classes[i]) < budget ? static_cast<uint32_t>(classes[i]) : budget;
        budget -= take;
        classes[i] = static_cast<int32_t>(take);
    }
    table->m_state = 0;
    FreeSpaceFill(table, spans, count, true, classes);
    table->m_state = 1;
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_140265fc0 = (void*)&casc::FileExists;
extern "C" void* const gen_f_140266840 = (void*)&casc::SetFileSize;
extern "C" void* const gen_f_1421581b0 = (void*)&casc::MutexName;
extern "C" void* const gen_f_142166bc0 = (void*)&casc::OpenShmemFile;
extern "C" void* const gen_f_142368490 = (void*)&casc::MappingMove;
// ---- end of generated globals ----
