#include "casc/IndexTables.hpp"
#include "casc/Dynamic.hpp"
#include "casc/Shmem.hpp"
#include "blz/Hex.hpp"
#include "blz/Log.hpp"
#include <cstring>
#include <malloc.h>
#include <windows.h>

namespace casc {

namespace {

const char s_tablesFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\index_tables.cpp";
const char s_hexDigits[] = "0123456789aAbBcCdDeEfF";

uint32_t HexValue(char c) {
    if (static_cast<uint8_t>(c - '0') < 10) {
        return static_cast<uint32_t>(c - '0');
    }
    if (static_cast<uint8_t>(c - 'a') < 6) {
        return static_cast<uint32_t>(c - 'a' + 10);
    }
    if (static_cast<uint8_t>(c - 'A') < 6) {
        return static_cast<uint32_t>(c - 'A' + 10);
    }
    return 0;
}

int32_t Lower(int32_t c) {
    return static_cast<uint32_t>(c - 'A') < 0x1A ? c + 0x20 : c;
}

void TablePath(char* out, const char* directory, uint32_t bucket, uint32_t version) {
    char name[16];
    IndexFileName(name, bucket, version);
    OsPathJoin(out, 0x30C, directory, name);
}

// ref: FUN_1421593c0
void LogRefreshOpenError(const char* path, uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_tablesFile, 0xd4, "Failed to open refresh target table file '%s'. Error (%u): %s", path,
        error, text);
}

// Deletes a table file that is older than the one in use (or a leftover "_" file).
// ref: FUN_1421585f0
void DeleteStale(INDEXTABLES* tables, char* path, uint32_t* error, const char* name) {
    if (tables->m_mode == 2) {
        return;
    }
    OsPathJoin(path, 0x30C, tables->m_path, name);
    *error = OsFileDelete(path);
    if (*error != 0) {
        char text[0x100];
        OsErrorString(text, sizeof(text), *error);
        blz::log(g_logCategory, 3, s_tablesFile, 0x1e3, "Failed to delete table file '%s'. Error (%u): %s", tables->m_path,
            *error, text);
    }
}

// "0<bucket>tempfile.idx", created for writing.
// ref: FUN_142159440
int32_t CreateTemp(const char* directory, uint32_t bucket, char* path, uint64_t size, OSFILE* file) {
    char name[0x20];
    name[0] = '0';
    name[1] = "0123456789abcdef"[bucket & 0xF];  // FUN_142366220
    std::memcpy(name + 2, "tempfile", 8);
    std::memcpy(name + 10, ".idx", 4);
    name[14] = '\0';
    OsPathJoin(path, size, directory, name);
    uint32_t error = OsFileOpen(file, path, 5, 0);
    if (error == 0) {
        return 0;
    }
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_tablesFile, 0x48, "Failed to create flush target table file '%s'. Error (%u): %s", path,
        error, text);
    return ErrorFromOs(error);
}

// MoveFileExW from one UTF-8 path to another.
// ref: FUN_140266710
uint32_t RenameFile(const char* from, const char* to) {
    auto wideTo = static_cast<wchar_t*>(_alloca((std::strlen(to) * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, to, -1, wideTo, 0x3FFFFFFF);
    auto wideFrom = static_cast<wchar_t*>(_alloca((std::strlen(from) * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, from, -1, wideFrom, 0x3FFFFFFF);
    if (!MoveFileExW(wideFrom, wideTo, 0)) {
        return GetLastError();
    }
    return 0;
}

// Syncs the written table and renames it to the next version (three tries). The result of
// the last failed try is what is returned, even when a later one succeeds.
// ref: FUN_1421595a0
int32_t Commit(const char* directory, uint32_t bucket, uint32_t version, const char* temp, OSFILE* file,
    uint32_t* committed) {
    uint32_t error = FlushFileBuffers(file->m_handle) ? 0 : GetLastError();  // FUN_140266060
    if (error != 0) {
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_tablesFile, 0x86, "Failed to sync file '%s' to disk. Error (%u): %s", temp, error,
            text);
        return ErrorFromOs(error);
    }
    int32_t result = 0;
    for (uint32_t attempt = 0; attempt < 3; attempt++) {
        version++;
        *committed = version;
        char path[0x30C];
        TablePath(path, directory, bucket, version);
        error = RenameFile(temp, path);
        if (error == 0) {
            return result;
        }
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_tablesFile, 0xa0, "Failed to rename flush target from '%s' to '%s'. Error (%u): %s",
            temp, path, error, text);
        result = ErrorFromOs(error);
    }
    blz::log(g_logCategory, 4, s_tablesFile, 0xac, "Exhausted flush retry attempts for table '%u'.", bucket);
    return result;
}

}

// ref: FUN_142158550
void INDEXTABLES::Init(blz::allocator* allocator) {
    this->m_allocator = allocator;
    this->m_path = nullptr;
    this->m_mode = 2;
    for (auto& table : this->m_tables) {
        table.m_version = 0;
        OsFileInit(&table.m_file);
    }
}

// ref: FUN_1421585b0
void INDEXTABLES::Destroy() {
    for (int32_t i = 15; i >= 0; i--) {
        OsFileClose(&this->m_tables[i].m_file);
    }
}

// ref: FUN_1421589a0
OSFILE* INDEXTABLES::File(uint32_t bucket) {
    return &this->m_tables[bucket].m_file;
}

// ref: FUN_142158c30
int32_t INDEXTABLES::Open(uint32_t bucket, uint32_t version, bool existing) {
    char path[0x30C];
    TablePath(path, this->m_path, bucket, version);
    int32_t mode = existing ? 3 : 1;
    if (this->m_mode == 2) {
        mode = 2;
    }
    uint32_t error = OsFileOpen(&this->m_tables[bucket].m_file, path, mode, 0);
    if (error == 0) {
        this->m_tables[bucket].m_version = version;
        return 0;
    }
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_tablesFile, 0x1b0, "Failed to open backing table file '%s'. Error (%u): %s", path, error,
        text);
    return ErrorFromOs(error);
}

// Finds the newest version of every table in the directory, deleting older ones.
// ref: FUN_142158f70
int32_t INDEXTABLES::Scan() {
    OSDIRITER iter;
    OsDirInit(&iter);
    uint32_t error = OsDirOpen(&iter, this->m_path);
    char path[0x30C];
    int32_t result = 0;
    if (error != 0) {
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_tablesFile, 0x1d0, "Failed to open directory '%s'. Error (%u): %s", this->m_path, error,
            text);
        result = ErrorFromOs(error);
        OsDirClose(&iter);
        return result;
    }
    OSDIRENTRY entry;
    while (OsDirNext(&iter, &entry)) {
        if (entry.m_directory) {
            continue;
        }
        const char* name = entry.m_name;
        uint32_t hex = 0;
        while (name[hex] != '\0' && std::strchr(s_hexDigits, name[hex]) != nullptr) {
            hex++;
        }
        if (hex != 10 || name[10] != '.') {
            continue;
        }
        bool underscore = name[11] == '_';
        const char* extension = name + (underscore ? 12 : 11);
        const char* expected = "idx";
        bool match = true;
        for (uint32_t i = 0;; i++) {
            int32_t a = Lower(extension[i]);
            int32_t b = Lower(expected[i]);
            if (a != b) {
                match = false;
                break;
            }
            if (a == 0) {
                break;
            }
        }
        if (!match) {
            continue;
        }
        uint32_t bucket = HexValue(name[0]) * 16 + HexValue(name[1]);
        if (bucket >= 0x10) {
            continue;
        }
        uint8_t bytes[4] = {};
        if (blz::hex_decode(bytes, 4, name + 2, 8) != 4) {
            continue;
        }
        uint32_t version = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
        TABLE& table = this->m_tables[bucket];
        if (version < table.m_version || underscore) {
            uint32_t deleteError = 0;
            DeleteStale(this, path, &deleteError, name);
            continue;
        }
        if (table.m_version != 0) {
            OsFileClose(&table.m_file);
            if (this->m_mode != 2) {
                char stale[0x30C];
                TablePath(stale, this->m_path, bucket, table.m_version);
                uint32_t deleteError = OsFileDelete(stale);
                if (deleteError != 0) {
                    char text[0x100];
                    OsErrorString(text, sizeof(text), deleteError);
                    blz::log(g_logCategory, 3, s_tablesFile, 0x1e3, "Failed to delete table file '%s'. Error (%u): %s",
                        this->m_path, deleteError, text);
                }
            }
        }
        result = this->Open(bucket, version, true);
        if (result != 0) {
            blz::log(g_logCategory, 4, s_tablesFile, 0x213, "First-To-Bind Index failed to open backing table file.");
            OsDirClose(&iter);
            return result;
        }
    }
    OsDirClose(&iter);
    return 0;
}

// Opens the tables at the versions the shmem header records, or - for the first process to
// bind - scans for them and creates the missing ones, then records them in the header.
// ref: FUN_1421589b0
int32_t INDEXTABLES::Bind(const char* path, int32_t mode, SHMEMBINDING* binding) {
    this->m_path = path;
    this->m_mode = mode;
    uint32_t* versions = binding->m_header->m_tableVersions;
    bool complete = true;
    for (uint32_t i = 0; i < 16; i++) {
        if (versions[i] == 0) {
            complete = false;
            break;
        }
    }
    if (complete) {
        uint32_t bucket = 0;
        int32_t result = 0;
        for (; bucket < 16; bucket++) {
            result = this->Open(bucket, versions[bucket], true);
            if (result != 0) {
                break;
            }
        }
        if (bucket == 16) {
            return 0;
        }
        blz::log(g_logCategory, 4, s_tablesFile, 0x22c, "Subsequent Index failed to open existing table file.");
        if (result != ERR_NOT_OPENED) {
            return result;
        }
    }
    blz::log(g_logCategory, 2, s_tablesFile, 0xef, "Scanning for KMT sequence numbers.");
    this->Scan();
    int32_t missing = 0x10;
    for (uint32_t bucket = 0; bucket < 16; bucket++) {
        if (this->m_tables[bucket].m_version == 0) {
            if (static_cast<uint32_t>(this->m_mode - 2) > 1) {
                int32_t result = this->Open(bucket, 1, false);
                if (result != 0) {
                    blz::log(g_logCategory, 4, s_tablesFile, 0x247, "First-To-Bind Index failed to create backing table file.");
                    return result;
                }
            }
        } else {
            missing--;
        }
    }
    if (missing != 0) {
        if (missing == 0x10) {
            if (static_cast<uint32_t>(this->m_mode - 2) < 2) {
                return ERR_NO_INDEX_FOUND;
            }
        } else if (this->m_mode != 4) {
            return ERR_NEEDS_REPAIR;
        }
    }
    for (uint32_t bucket = 0; bucket < 16; bucket++) {
        versions[bucket] = this->m_tables[bucket].m_version;
    }
    return 0;
}

// Moves to a newer version of a table another process wrote.
// ref: FUN_142158d70
int32_t INDEXTABLES::Refresh(uint32_t bucket, uint32_t version, KEYMAPPINGTABLE* table) {
    TABLE& entry = this->m_tables[bucket];
    if (entry.m_version == version) {
        return 0;
    }
    if (version < entry.m_version) {
        blz::log(g_logCategory, 4, s_tablesFile, 0x176, "Invalid refresh sequence number. Current '%u' Target '%u'",
            entry.m_version, version);
        return ERR_UNSPECIFIED;
    }
    char path[0x30C];
    TablePath(path, this->m_path, bucket, version);
    int32_t result = 0;
    OSFILE file;
    OsFileInit(&file);
    uint32_t error = OsFileOpen(&file, path, (this->m_mode != 2) + 2, 0);
    if (error == 0) {
        entry.m_version = version;
        OsFileMove(&entry.m_file, &file);
        int32_t bound = table->Bind(&entry.m_file, this->m_mode, bucket, table->m_maxSize);
        if (bound != 0) {
            blz::log(g_logCategory, 4, s_tablesFile, 399, "Cannot bind to new backing file '%s'. Error (%u): %s", path, bound,
                ErrorString(bound));
        }
    } else {
        LogRefreshOpenError(path, error);
        result = ErrorFromOs(error);
    }
    OsFileClose(&file);
    return result;
}

// Writes the table out as a new version with its updates folded in.
// ref: FUN_1421586a0
int32_t INDEXTABLES::Flush(uint32_t bucket, KEYMAPPINGTABLE* table, uint32_t* version, KMTRECORD* records,
    uint32_t count) {
    OSFILE file;
    OsFileInit(&file);
    char path[0x30C];
    int32_t result = CreateTemp(this->m_path, bucket, path, sizeof(path), &file);
    if (result == 0) {
        result = count == 0 ? table->Write(&file, nullptr, 0) : table->Write(&file, records, count);
        if (result == 0) {
            TABLE& entry = this->m_tables[bucket];
            result = Commit(this->m_path, bucket, entry.m_version, path, &file, version);
            if (result == 0) {
                entry.m_version = *version;
                OsFileMove(&entry.m_file, &file);
                result = table->Bind(&entry.m_file, this->m_mode, bucket, table->m_maxSize);
                if (result != 0) {
                    blz::log(g_logCategory, 4, s_tablesFile, 0x135, "Failed to bind table %u after flushing. Error (%u): %s",
                        bucket, result, ErrorString(result));
                }
            }
        }
    }
    OsFileClose(&file);
    return result;
}

}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140266060=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142366220=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_140266710 = (void*)&casc::RenameFile;
extern "C" void* const gen_f_142159440 = (void*)&casc::CreateTemp;
extern "C" void* const gen_f_1421595a0 = (void*)&casc::Commit;
// ---- end of generated globals ----
