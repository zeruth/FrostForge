#include "casc/DynamicStorage.hpp"
#include "blz/Log.hpp"
#include "blz/Parse.hpp"
#include "casc/Common.hpp"
#include "casc/Dynamic.hpp"
#include <cstring>

namespace casc {

namespace {

const char s_storageFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic_storage.cpp";

int32_t Lower(char c) {
    int32_t value = c;
    return static_cast<uint32_t>(value - 'A') <= 0x19 ? value + 0x20 : value;
}

bool IsDigit(char c) {
    return static_cast<uint8_t>(c - '0') < 10;
}

}

// ref: FUN_142146c30
DYNAMICSTORAGE::DYNAMICSTORAGE(blz::allocator* allocator, const char* directory, uint64_t cacheCapacity, uint8_t shift,
    uint8_t readOnly, aio::CONTEXT* aio, uint64_t a8, uint8_t a9, uint32_t bufferCount, uint32_t bufferSize,
    TRUNCATEDREAD truncated, void* context)
    : m_archives(allocator, cacheCapacity) {
    this->m_allocator = allocator;
    this->m_accessor.Construct(allocator, bufferCount, bufferSize, 1, aio, a8, a9, truncated, context);
    this->m_shift = shift;
    this->m_mask = (uint64_t(1) << (shift & 0x3F)) - 1;
    this->m_readOnly = readOnly;
    this->m_directory = nullptr;
    this->m_frozen = nullptr;
    this->m_list.m_next = &this->m_list;
    this->m_list.m_prev = &this->m_list;
    if (directory) {
        uint64_t size = std::strlen(directory) + 1;
        if (size > 1) {
            auto copy = static_cast<char*>(allocator->allocate(size, 1));
            if (copy) {
                std::memset(copy, 0, size);
            }
            this->m_directory = copy;
            std::memcpy(copy, directory, size);
            for (uint64_t at = size - 2; at != 0; at--) {
                char c = this->m_directory[at];
                if (c != '/' && c != '\\') {
                    break;
                }
                this->m_directory[at] = '\0';
            }
        }
    }
    InitializeSRWLock(&this->m_lock);
}

// ref: FUN_142146dd0
DYNAMICSTORAGE::~DYNAMICSTORAGE() {
    this->m_archives.Clear();
    this->m_allocator->deallocate(this->m_directory);
    LISTLINK* node = this->m_list.m_next;
    while (node != &this->m_list) {
        LISTLINK* next = node->m_next;
        node->m_next = node;
        node->m_prev = node;
        node = next;
    }
    this->m_list.m_prev = &this->m_list;
    this->m_list.m_next = &this->m_list;
    // m_archives' destructor (FUN_142157260) runs here, then the accessor's (FUN_142143970).
}

// The accessor hands this to read callbacks.
// ref: FUN_142147cf0 (through FUN_14031d390)
void DYNAMICSTORAGE::SetOwner(void* owner) {
    this->m_accessor.m_owner = owner;
}

// Opens each frozen archive read-only with writers denied: one another process has open for
// writing is not frozen after all. Archives are opened that way from then on.
// ref: FUN_1421477e0
int32_t DYNAMICSTORAGE::Freeze(blz::id_set* frozen) {
    if (!frozen || frozen->m_count == 0) {
        return 0;
    }
    blz::allocator* allocator = this->m_allocator;
    this->m_archives.m_exclusiveRead = 1;
    uint64_t inlineStore[8];
    uint64_t* thawed = inlineStore;
    uint32_t count = 0;
    uint32_t capacity = 0x80000008;
    bool onStack = true;
    int32_t result = 0;
    for (uint32_t i = 0; i < frozen->m_count; i++) {
        uint64_t archive = frozen->m_entries[i];
        int32_t error = 0;
        ARCHIVEFILE* file = this->m_archives.Open(archive, this->m_directory, 2, &error, 0, nullptr, nullptr);
        if (!file) {
            result = error;
            if (error != 0x18) {
                if (!onStack) {
                    allocator->deallocate(thawed);
                }
                return result;
            }
            blz::log(g_logCategory, 3, s_storageFile, 0x36d,
                "Frozen archive %u is opened for writing in another process, unfreezing.", static_cast<uint32_t>(archive));
            uint32_t limit = capacity & 0x7FFFFFFF;
            if (count == limit && limit < count + 1) {
                uint32_t grown = limit + (capacity >> 1 & 0x3FFFFFFF);
                if (grown < count + 1) {
                    grown = count + 1;
                }
                if (grown < 8) {
                    grown = 8;
                }
                auto data = static_cast<uint64_t*>(allocator->allocate(uint64_t(grown) * 8, 8));
                std::memcpy(data, thawed, uint64_t(count) * 8);
                if (!onStack) {
                    allocator->deallocate(thawed);
                }
                thawed = data;
                capacity = grown & 0x7FFFFFFF;
                onStack = false;
            }
            thawed[count] = archive;
            count++;
        } else {
            file->Release();
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        frozen->remove(thawed[i]);
    }
    this->m_frozen = frozen;
    if (!onStack) {
        allocator->deallocate(thawed);
    }
    return 0;
}

// ref: FUN_142147a40
bool DYNAMICSTORAGE::IsFrozen(uint64_t archive) {
    if (!this->m_frozen) {
        return false;
    }
    return this->m_frozen->contains(archive);
}

// Notes which data.### files the directory holds ("data." in any case, then three or four
// digits).
// ref: FUN_142147490
bool ScanArchives(const char* directory, blz::allocator* allocator, uint8_t present[0x3ff], int32_t* count) {
    (void)allocator;
    std::memset(present, 0, 0x3ff);
    *count = 0;
    OSDIRITER iter;
    OsDirInit(&iter);
    uint32_t error = OsDirOpen(&iter, directory);
    if (error != 0) {
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_storageFile, 0x23f, "Failed to open directory %s. Error (%u): %s", directory, error, text);
        OsDirClose(&iter);
        return false;
    }
    OSDIRENTRY entry;
    while (OsDirNext(&iter, &entry)) {
        if (entry.m_directory) {
            continue;
        }
        const char* name = entry.m_name;
        static const char s_prefix[] = "data.";
        bool matches = true;
        for (uint32_t i = 0; i < 5; i++) {
            int32_t a = Lower(name[i]);
            int32_t b = Lower(s_prefix[i]);
            if (a != b) {
                matches = false;
                break;
            }
            if (a == 0) {
                break;
            }
        }
        if (!matches) {
            continue;
        }
        if (!IsDigit(name[5]) || !IsDigit(name[6]) || !IsDigit(name[7]) ||
            (name[8] != '\0' && (!IsDigit(name[8]) || name[9] != '\0'))) {
            continue;
        }
        uint32_t archive;
        bool overflow;
        blz::parse_uint32(name + 5, std::strlen(name + 5), &archive, &overflow);
        if (archive < 0x3ff) {
            if (!present[archive]) {
                present[archive] = 1;
                (*count)++;
                continue;
            }
            blz::log(g_logCategory, 3, s_storageFile, 0x264, "Detected archive segment %u multiple times.", archive);
        } else {
            blz::log(g_logCategory, 3, s_storageFile, 599, "Ignoring out of range archive file: %s.", name);
        }
    }
    OsDirClose(&iter);
    return true;
}

// ref: FUN_142147ae0
int32_t DYNAMICSTORAGE::Read(void* dest, uint64_t offset, uint32_t size, uint32_t* read) {
    *read = 0;
    uint64_t archive = offset >> (this->m_shift & 0x3F);
    uint32_t first = static_cast<uint32_t>(archive);
    uint32_t last = first;
    if (size != 0) {
        last = static_cast<uint32_t>((uint64_t(size) - 1 + offset) >> (this->m_shift & 0x3F));
    }
    if (first > 0x3fe || last != first) {
        blz::log(g_logCategory, 4, s_storageFile, 0x2b5, "requested span is outside segment boundaries");
        return 10;
    }
    if (size == 0) {
        return 0;
    }
    int32_t error = 0;
    int32_t mode = 2;
    if (!this->m_readOnly && !this->IsFrozen(archive & 0xFFFFFFFF)) {
        mode = 1;
    }
    int32_t result = 0;
    ARCHIVEFILE* file = this->m_archives.Open(archive & 0xFFFFFFFF, this->m_directory, mode, &error, 0, nullptr, nullptr);
    if (!file) {
        *read = 0;
        result = error;
    } else {
        uint64_t local = this->m_mask & offset;
        int32_t status = this->m_accessor.Read(&file, local, size, dest, read);
        if (status != 0) {
            blz::log(g_logCategory, 4, s_storageFile, 0x69,
                "Failed to read %u bytes from container archive segment %u at offset %llu. Error (%u): %s", size, first,
                local, status, ErrorString(status));
            result = status;
        }
    }
    return result;
}

// The archive is opened for aio (kind 2) and the read handed to the accessor.
// ref: FUN_142147f60
int32_t DYNAMICSTORAGE::ReadAsync(const uint8_t ekey[16], const void* span, void* dest, uint64_t offset, uint32_t size,
    uint64_t requestKey, uint64_t* outKey, READCALLBACK callback, void* context, uint32_t flags) {
    *outKey = 0;
    uint64_t archive = offset >> (this->m_shift & 0x3F);
    uint32_t first = static_cast<uint32_t>(archive);
    uint32_t last = first;
    if (size != 0) {
        last = static_cast<uint32_t>((uint64_t(size) - 1 + offset) >> (this->m_shift & 0x3F));
    }
    if (first > 0x3fe || last != first) {
        blz::log(g_logCategory, 4, s_storageFile, 0x2b5, "requested span is outside segment boundaries");
        return 10;
    }
    if (size == 0) {
        return 0;
    }
    int32_t error = 0;
    int32_t mode = 2;
    if (!this->m_readOnly && !this->IsFrozen(archive & 0xFFFFFFFF)) {
        mode = 1;
    }
    ARCHIVEFILE* file = this->m_archives.Open(archive & 0xFFFFFFFF, this->m_directory, mode, &error, 2,
        this->m_accessor.m_aio, nullptr);
    if (!file) {
        return error;
    }
    // The accessor takes the reference over on every path.
    ARCHIVEFILE* held = file;
    return this->m_accessor.ReadAsync(ekey, span, &held, this->m_mask & offset, size, dest, &requestKey, outKey,
        callback, context, flags);
}

// ref: FUN_142147090
int32_t DYNAMICSTORAGE::GetResult(uint64_t key, uint32_t* read, bool* found) {
    return this->m_accessor.GetResult(key, read, found);
}

// ref: FUN_142148260
int32_t DYNAMICSTORAGE::Write(const void* data, uint64_t offset, uint32_t size) {
    if (this->m_readOnly) {
        return 5;
    }
    uint64_t archive = offset >> (this->m_shift & 0x3F);
    uint32_t first = static_cast<uint32_t>(archive);
    uint32_t last = first;
    if (size != 0) {
        last = static_cast<uint32_t>((uint64_t(size) - 1 + offset) >> (this->m_shift & 0x3F));
    }
    if (first >= 0x3ff || last != first) {
        blz::log(g_logCategory, 4, s_storageFile, 0x2b5, "requested span is outside segment boundaries");
        return 10;
    }
    if (size == 0) {
        return 0;
    }
    int32_t error = 0;
    ARCHIVEFILE* file = this->m_archives.Open(archive & 0xFFFFFFFF, this->m_directory, 1, &error, 0, nullptr, nullptr);
    int32_t result = error;
    if (file) {
        uint64_t local = this->m_mask & offset;
        uint32_t written = 0;
        uint32_t status = OsFileWriteAt(&file->m_file, data, size, local, &written);
        if (status == 0) {
            ARCHIVEFILE* held = file;
            file = nullptr;
            this->m_accessor.Invalidate(&held, local, written);
            result = written != size ? 8 : 0;
        } else {
            result = ErrorFromOs(status);
        }
    }
    if (file) {
        file->Release();
    }
    return result;
}

}
