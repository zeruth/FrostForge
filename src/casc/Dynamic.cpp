#include "casc/Dynamic.hpp"
#include "blz/Hex.hpp"
#include "blz/Log.hpp"
#include "casc/DynamicStorage.hpp"
#include "casc/SegmentHeader.hpp"
#include "casc/Shmem.hpp"
#include "os/Os.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

namespace casc {

namespace {

const char s_dynamicFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic.cpp";
const char s_indexFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\container_index.cpp";

const char* const s_errorStrings[] = {
    "Success",
    "UnspecifiedError",
    "NotReserved",
    "NotResident",
    "InvalidSpan",
    "Unsupported",
    "NotOpened",
    "DiskFull",
    "IOError",
    "NeedsRepair",
    "InvalidParam",
    "CascLocked",
    "FileExists",
    "InvalidDriveType",
    "CascFull",
    "Canceled",
    "ArchiveFull",
    "AlreadyOpened",
    "NoIndexFound",
    "PathTooLong",
    "PermissionDenied",
    "WouldBlock",
    "FileTooBig",
    "WouldNotPipeline",
    "SharingViolation",
};

// Remote drives cannot hold a writable container.
// ref: FUN_14214b060
bool DriveSupported(const char* path, int32_t mode) {
    if (!path) {
        return false;
    }
    if (mode != 2) {
        int32_t type = DriveType(path);
        if (type == 1) {
            return false;
        }
        if (type == 0) {
            blz::log(g_logCategory, 3, s_indexFile, 0x2a5, "Unable to determine drive type for '%s'.", path);
        }
    }
    return true;
}

// Creating a container over an existing one starts it empty: the index files, the archives and
// shmem go.
// ref: FUN_14213d970
int32_t TruncateContainer(const char* directory, blz::allocator* allocator) {
    (void)allocator;
    OSDIRITER iter;
    OsDirInit(&iter);
    char reason[0x100];
    uint32_t error = OsDirOpen(&iter, directory);
    if (error != 0) {
        OsErrorString(reason, sizeof(reason), error);
        blz::log(g_logCategory, 4, s_dynamicFile, 0x72, "Error opening directory '%s'. (reason: %s)", directory, reason);
        OsDirClose(&iter);
        return 8;
    }
    int32_t result = 0;
    char stem[0x30C];
    std::memset(stem, 0, sizeof(stem));
    OSDIRENTRY entry;
    while (OsDirNext(&iter, &entry)) {
        if (entry.m_directory) {
            continue;
        }
        uint64_t length = std::strlen(entry.m_name);
        if (length > 0x30B) {
            length = 0x30B;
        }
        std::memcpy(stem, entry.m_name, length);
        stem[length] = '\0';
        OsPathStripExtension(stem);
        bool remove = std::strcmp(OsPathExtension(entry.m_name), ".idx") == 0 || std::strcmp(stem, "data") == 0 ||
            std::strcmp(stem, "shmem") == 0;
        if (!remove) {
            continue;
        }
        char path[0x30C];
        std::memset(path, 0, sizeof(path));
        OsPathJoin(path, sizeof(path), directory, entry.m_name);
        error = OsFileDelete(path);
        if (error != 0) {
            OsErrorString(reason, sizeof(reason), error);
            blz::log(g_logCategory, 4, s_dynamicFile, 0x91, "Error removing file '%s'. (reason: %s)", path, reason);
            result = 8;
            break;
        }
    }
    OsDirClose(&iter);
    return result;
}

// After a read came up short, the part of the key's span past it is marked not resident.
// ref: FUN_14213ded0
void HandleTruncatedRead(CONTAINERINDEX* index, const uint8_t key[16], const SPAN* span, uint32_t read) {
    char text[0x21];
    blz::hex_encode(text, sizeof(text), key, 16);  // FUN_1420d8f40
    blz::log(g_logCategory, 4, s_dynamicFile, 0x141, "Truncated read for key '%s'. Requested: %u Read: %u.", text,
        span->m_size, read);
    SPAN stored;
    int32_t result = index->Lookup(key, 0, &stored);
    const char* message;
    int32_t line;
    if (result == 0) {
        SPAN rest;
        rest.m_offset = uint64_t(read) + span->m_offset;
        rest.m_size = stored.m_size - static_cast<uint32_t>(rest.m_offset);
        rest.m_pad = 0;
        result = index->UpdateResidency(key, 0, &rest, 0);
        if (result == 0) {
            return;
        }
        message = "Failed to update residency after truncated read.";
        line = 0x153;
    } else {
        message = "Failed to get allocated span after truncated read.";
        line = 0x147;
    }
    blz::log(g_logCategory, 4, s_dynamicFile, line, message);
}

// The accessor's truncated-read callback.
// ref: FUN_14213dfe0
void OnTruncatedRead(void* context, const uint8_t* key, const void* span, uint32_t read) {
    auto container = static_cast<DYNAMICCONTAINER*>(context);
    if (container && container->m_index.m_bound) {
        StatsReport(&container->m_stats, 0, "dynamic_container.read_truncated.count", 1, 1.0f);
        HandleTruncatedRead(&container->m_index, key, static_cast<const SPAN*>(span), read);
    }
}

// Writes an archive's 16 reconstruction headers (one per index bucket) at its start.
// ref: FUN_14213ede0
int32_t WriteHeaders(void* context, uint32_t archive, const uint8_t* keys) {
    auto container = static_cast<DYNAMICCONTAINER*>(context);
    if (!container || !container->m_index.m_bound) {
        return 6;
    }
    uint8_t headers[0x1e0];
    for (uint32_t i = 0; i < 16; i++) {
        SPAN span;
        span.m_offset = uint64_t(i * 0x1e) + uint64_t(archive) * 0x40000000;
        span.m_size = 0x1e;
        span.m_pad = 0;
        if (!SegmentHeaderSerialize(headers + i * 0x1e, 0x1e, keys + i * 0x10, &span, 1)) {
            blz::log(g_logCategory, 4, s_dynamicFile, 0xb0, "Failed to serialize reconstruction header.");
            return 1;
        }
    }
    return container->m_storage->Write(headers, uint64_t(archive) * 0x40000000, 0x1e0);
}

// Binds the index, then checks each archive's reconstruction headers against it.
// ref: FUN_14213e9d0
int32_t OpenIndex(DYNAMICCONTAINER* container, uint64_t maxSize, int32_t mode, uint8_t reclaim, int32_t* processes,
    uint8_t freeze, uint8_t noPidTable) {
    blz::id_set* frozen = freeze ? &container->m_frozen : nullptr;
    int32_t result = container->m_index.Bind(container->m_path.c_str(), mode, maxSize, reclaim, &WriteHeaders, container,
        processes, frozen, noPidTable);
    if (result != 0) {
        blz::log(g_logCategory, 4, s_dynamicFile, 0x17d, "failed to bind Container index at '%s': %d (%s)",
            container->m_path.c_str(), result, ErrorString(result));
        return result;
    }
    container->m_storage->Freeze(freeze ? &container->m_frozen : nullptr);
    uint8_t present[0x3ff];
    int32_t count = 0;
    if (!ScanArchives(container->m_path.c_str(), container->m_allocator, present, &count)) {
        return 8;
    }
    if (count != 0) {
        uint8_t headers[0x1e0];
        for (uint32_t archive = 0; archive < 0x3ff; archive++) {
            if (!present[archive]) {
                continue;
            }
            uint32_t read = 0;
            int32_t status = container->m_storage->Read(headers, uint64_t(archive) * 0x40000000, 0x1e0, &read);
            if (status == 2) {
                blz::log(g_logCategory, 3, s_dynamicFile, 0x1aa,
                    "Archive file %u is missing. Container will initialize, but files may be missing.", archive);
            } else if (status != 0) {
                blz::log(g_logCategory, 4, s_dynamicFile, 0x1ae, "Failed to read archive '%u', failing validation.",
                    archive);
                return 8;
            }
            if (read < 0x1e0) {
                std::memset(headers + read, 0, 0x1e0 - read);
            }
            for (uint32_t i = 0; i < 16; i++) {
                uint64_t offset = uint64_t(i * 0x1e) + uint64_t(archive) * 0x40000000;
                SEGMENTHEADER header;
                if (SegmentHeaderParse(headers + i * 0x1e, 0x1e, offset, &header) && header.m_flag == 1) {
                    SPAN span;
                    if (container->m_index.Lookup(header.m_key, 1, &span) != 0) {
                        blz::log(g_logCategory, 4, s_dynamicFile, 0x1c8,
                            "Missing segment header for archive '%u', repair needed.", archive);
                        return 9;
                    }
                    if (span.m_offset != offset) {
                        blz::log(g_logCategory, 4, s_dynamicFile, 0x1ce,
                            "Segment header mismatch for archive '%u', repair needed.", archive);
                        return 9;
                    }
                }
            }
        }
    }
    return 0;
}

// A read found the archive gone: the key goes.
// ref: FUN_14213de20
void HandleMissingArchive(CONTAINERINDEX* index, const uint8_t key[16], const SPAN* span) {
    blz::log(g_logCategory, 4, s_dynamicFile, 0x131, "Missing archive detected for offset '%llu'", span->m_offset);
    if (index->RemoveKeys(key, 1, 0) != 0) {
        char text[0x21];
        blz::hex_encode(text, sizeof(text), key, 16);  // FUN_1420d8f40
        blz::log(g_logCategory, 4, s_dynamicFile, 0x138, "Failed to delete key %s after missing archive detected.", text);
    }
}

// Whether the buffered blocks can be trusted, from who else writes to the container.
void CheckBuffers(DYNAMICCONTAINER* container) {
    if ((static_cast<uint32_t>(container->m_mode - 2) & 0xFFFFFFFC) == 0 && container->m_mode != 3) {
        return;
    }
    ARCHIVEFILEACCESSOR& accessor = container->m_storage->m_accessor;
    switch (container->m_index.CheckWriters()) {
    case 0:
    case 5:
        accessor.ShutdownBuffers();
        break;
    case 4:
        accessor.InvalidateBuffers();
        accessor.EnableBuffers();
        break;
    case 3:
        accessor.EnableBuffers();
        break;
    default:
        break;
    }
}

// ref: FUN_14213cb30
void ContainerDelete(blz::allocator* allocator, DYNAMICCONTAINER* container) {
    if (!container) {
        return;
    }
    container->m_frozen.destroy();
    container->m_pool.Destroy();
    container->m_b20.destroy();
    container->m_index.Destroy();
    container->m_path.Destroy();
    allocator->deallocate(container);
}

void StorageDelete(blz::allocator* allocator, DYNAMICSTORAGE* storage) {
    if (storage) {
        storage->~DYNAMICSTORAGE();
        allocator->deallocate(storage);
    }
}

}

// The table at 0x142d3e6c0 is indexed unchecked.
// ref: FUN_14213caf0
const char* ErrorString(int32_t error) {
    return s_errorStrings[static_cast<uint32_t>(error)];
}

// ref: FUN_14213fc30
int32_t DynamicOpen(DYNAMICCONTAINER** out, const char* directory, int32_t mode, OPENPARAMS* params) {
    uint32_t archiveCount = static_cast<uint32_t>(params->m_archiveCount);
    blz::allocator* allocator = params->m_allocator;
    uint32_t bufferCount = 0;
    uint8_t lru;
    if (!params->m_noPidTable) {
        lru = params->m_lru;
        bufferCount = static_cast<uint32_t>(params->m_bufferCount);
    } else {
        lru = 0;
    }
    *out = nullptr;
    if (mode == 4) {
        return 5;
    }
    if (archiveCount - 1 > 0x3fe) {
        return 10;
    }
    uint32_t bufferSize = static_cast<uint32_t>(params->m_bufferSize);
    if (bufferCount != 0) {
        if (bufferCount == 1) {
            blz::log(g_logCategory, 4, s_dynamicFile, 0x31b,
                "Aligned cached buffer count of 1 isn't valid, it must be 2 or greater");
            return 10;
        }
        if (bufferSize == 0) {
            blz::log(g_logCategory, 4, s_dynamicFile, 0x321, "Aligned cached buffer size of 0 isn't valid");
            return 10;
        }
        if ((bufferSize & (bufferSize - 1)) != 0) {
            blz::log(g_logCategory, 4, s_dynamicFile, 0x327, "Aligned cached buffer size must be a power of 2 (%u isn't)",
                bufferSize);
            return 10;
        }
    }
    if (!DriveSupported(directory, mode)) {
        blz::log(g_logCategory, 4, s_dynamicFile, 0x32e,
            "Drive containing path '%s' is of unsupported type and cannot house dynamic containers.", directory);
        return 0xd;
    }
    if (!OsDirExists(directory) && static_cast<uint32_t>(mode - 2) < 2) {
        blz::log(g_logCategory, 4, s_dynamicFile, 0x334, "missing directory '%s' for existing container", directory);
        return 0x12;
    }
    if (!allocator) {
        allocator = blz::default_allocator();
    }
    if (OsDirExists(directory) && mode == 1 && TruncateContainer(directory, allocator) != 0) {
        blz::log(g_logCategory, 4, s_dynamicFile, 0x33e, "Error truncating existing container at '%s'.", directory);
        return 8;
    }

    auto container = static_cast<DYNAMICCONTAINER*>(allocator->allocate(sizeof(DYNAMICCONTAINER), 8));
    if (container) {
        container->m_allocator = allocator;
        container->m_path.Init(allocator);
        container->m_mode = 0;
        container->m_lruActive = 0;
        container->m_freezeArchives = 0;
        container->m_stats.m_owner = container;
        container->m_stats.m_report = nullptr;
        container->m_stats.m_context = nullptr;
        container->m_stats.m_18 = nullptr;
        container->m_stats.m_20 = nullptr;
        container->m_index.Construct(allocator, &container->m_stats);
        container->m_storage = nullptr;
        container->m_writer = nullptr;
        container->m_lruTable = nullptr;
        container->m_b20.init(allocator);
        container->m_pool.Init(allocator, 0x50, 8, 0x10, 1);
        InitializeSRWLock(&container->m_poolLock);
        container->m_frozen.init(allocator);
        InitializeSRWLock(&container->m_lock);
    }
    *out = container;
    container->m_stats.m_report = reinterpret_cast<void (*)(void*, void*, const char*)>(params->m_callbackA);
    container->m_stats.m_context = params->m_callbackAContext;
    container->m_stats.m_18 = reinterpret_cast<void*>(params->m_callbackB);
    container->m_stats.m_20 = params->m_callbackBContext;
    container->m_path.Assign(directory, static_cast<uint32_t>(std::strlen(directory)));
    container->m_mode = mode;
    container->m_freezeArchives = params->m_freezeArchives;

    DYNAMICSTORAGE* storage = nullptr;
    void* memory = allocator->allocate(sizeof(DYNAMICSTORAGE), 8);
    if (memory) {
        storage = new (memory) DYNAMICSTORAGE(allocator, directory, static_cast<uint32_t>(params->m_archiveCacheSize), 0x1e,
            mode == 2, static_cast<aio::CONTEXT*>(params->m_ioContext), params->m_20, container->m_freezeArchives,
            bufferCount, bufferSize, &OnTruncatedRead, container);
    }
    container->m_storage = storage;
    storage->SetOwner(container);

    int32_t processes = 0;
    int32_t result = OpenIndex(container, uint64_t(archiveCount) << 30, mode, params->m_reclaimFreeSpace, &processes,
        container->m_freezeArchives, params->m_noPidTable);
    if (result != 0) {
        StorageDelete(allocator, container->m_storage);
        ContainerDelete(allocator, container);
        *out = nullptr;
        return result;
    }
    if (!lru) {
        return 0;
    }
    if (mode == 2) {
        blz::log(g_logCategory, 4, s_dynamicFile, 0x1de,
            "LRU subsystem cannot be used with a read only container or in maintenance mode.");
        result = 5;
    } else if (!container->m_index.HasPidTable()) {
        blz::log(g_logCategory, 4, s_dynamicFile, 0x1e5, "Cannot use the LRU subsystem without a pid table.");
        result = 5;
    } else {
        // The LRU table (FUN_14214f8e0, dynamic/lru.cpp) is not ported; nothing the client
        // opens asks for it (tact passes CONTAINERPARAMS a7 = 0).
        container->m_lruActive = 1;
        FROST_UNIMPLEMENTED();
        result = 5;
    }
    StorageDelete(allocator, container->m_storage);
    ContainerDelete(allocator, container);
    *out = nullptr;
    return result;
}

// Only a bound container is torn down.
// ref: FUN_14213f150
void DynamicClose(DYNAMICCONTAINER** handle) {
    DYNAMICCONTAINER* container = *handle;
    if (!container || !container->m_index.m_bound) {
        return;
    }
    if (container->m_lruTable) {
        // The LRU flush (FUN_1421500d0) and teardown (FUN_14214fa50) are not ported; DynamicOpen
        // never creates the table.
        FROST_UNIMPLEMENTED();
    }
    if (container->m_storage) {
        StorageDelete(container->m_allocator, container->m_storage);
    }
    if (container->m_writer) {
        // The writer (destroyed by FUN_1421521f0) belongs to the write path, which is not ported.
        FROST_UNIMPLEMENTED();
    }
    ContainerDelete(container->m_allocator, container);
    *handle = nullptr;
}

// Whether a key's data (past its 0x1e-byte header) is all resident.
// ref: FUN_14213fa20
int32_t DynamicContains(DYNAMICCONTAINER* container, const uint8_t key[16]) {
    if (!container || !container->m_index.m_bound) {
        return 6;
    }
    SPAN request;
    request.m_offset = 0x1e;
    request.m_size = 0xFFFFFFFF;
    request.m_pad = 0;
    return container->m_index.QuerySpan(key, 0, &request);
}

// Whether a range of a key's data ({offset, size} past its header) is resident.
// ref: FUN_14213fa80
int32_t DynamicGetSpan(DYNAMICCONTAINER* container, const uint8_t key[16], uint64_t span[2]) {
    if (!container || !container->m_index.m_bound) {
        return 6;
    }
    SPAN request;
    request.m_offset = span[0] + 0x1e;
    request.m_size = static_cast<uint32_t>(span[1]);
    request.m_pad = static_cast<uint32_t>(span[1] >> 32);
    return container->m_index.QuerySpan(key, 0, &request);
}

// ref: FUN_142140930
int32_t DynamicRead(DYNAMICCONTAINER* container, const uint8_t key[16], const uint64_t span[2], void* dest,
    uint32_t* read) {
    if (!container || !container->m_index.m_bound) {
        return 6;
    }
    // The LRU table (+0xb10) is never created (see DynamicOpen), so its lock and touch are skipped.
    SPAN request;
    request.m_offset = span[0] + 0x1e;
    request.m_size = static_cast<uint32_t>(span[1]);
    request.m_pad = 0;
    SPAN resolved;
    SPAN stored;
    int32_t result = container->m_index.Resolve(key, 0, &request, &resolved, &stored);
    if (result != 0) {
        return result;
    }
    CheckBuffers(container);
    if (resolved.m_size == 0) {
        *read = 0;
        return 0;
    }
    result = container->m_storage->Read(dest, resolved.m_offset, resolved.m_size, read);
    if (result == 2) {
        HandleMissingArchive(&container->m_index, key, &resolved);
    } else if (result == 0 && *read < resolved.m_size) {
        StatsReport(&container->m_stats, 0, "dynamic_container.read_truncated.count", 1, 1.0f);
        SPAN truncated;
        truncated.m_offset = span[0] + 0x1e;
        truncated.m_size = resolved.m_size;
        truncated.m_pad = 0;
        HandleTruncatedRead(&container->m_index, key, &truncated, *read);
    }
    return result;
}

// ref: FUN_142140b90
int32_t DynamicReadAsync(DYNAMICCONTAINER* container, const uint8_t key[16], const uint64_t span[2], void* dest,
    uint64_t* outKey, void (*callback)(void* context, void* owner, uint64_t key), void* context, uint32_t flags) {
    *outKey = 0;
    if (!container || !container->m_index.m_bound) {
        return 6;
    }
    if (static_cast<uint32_t>(span[1]) == 0) {
        return 0;
    }
    SPAN request;
    request.m_offset = span[0] + 0x1e;
    request.m_size = static_cast<uint32_t>(span[1]);
    request.m_pad = 0;
    SPAN resolved;
    SPAN stored;
    int32_t result = container->m_index.Resolve(key, 0, &request, &resolved, &stored);
    if (result != 0) {
        return result;
    }
    CheckBuffers(container);
    uint64_t requestKey = NewRequestKey(0);
    request.m_offset = span[0] + 0x1e;
    request.m_size = resolved.m_size;
    result = container->m_storage->ReadAsync(key, &request, dest, resolved.m_offset, resolved.m_size, requestKey, outKey,
        callback, context, flags);
    if (result == 2) {
        HandleMissingArchive(&container->m_index, key, &resolved);
    }
    return result;
}

// Keys tagged in the top bit belong to the LRU's own requests (+0xb20), never made here.
// ref: FUN_14213ef00
int32_t DynamicGetResult(DYNAMICCONTAINER* container, uint64_t key, uint32_t* read) {
    if ((key >> 63) == 0) {
        bool found = false;
        int32_t result = container->m_storage->GetResult(key, read, &found);
        if (found) {
            return result;
        }
        return container->m_storage->GetResult(key, read, nullptr);
    }
    // The LRU request path (the 0x50-byte requests in the pool at +0xb50) is not ported.
    FROST_UNIMPLEMENTED();
    return 10;
}

// Part of a key's data ({offset past its header, size}) is no longer resident.
// ref: FUN_142141230
int32_t DynamicMarkNonResident(DYNAMICCONTAINER* container, const uint8_t key[16], const uint64_t span[2]) {
    if (!container || !container->m_index.m_bound) {
        return 6;
    }
    if (container->m_mode == 2) {
        return 5;
    }
    SPAN request;
    request.m_offset = span[0] + 0x1e;
    request.m_size = static_cast<uint32_t>(span[1]);
    request.m_pad = static_cast<uint32_t>(span[1] >> 32);
    int32_t result = container->m_index.UpdateResidency(key, 0, &request, 0);
    if (static_cast<uint32_t>(result - 2) < 2) {
        result = 0;
    }
    return result;
}

// ref: FUN_142140e60
int32_t DynamicRemove(DYNAMICCONTAINER* container, const uint8_t key[16]) {
    (void)container;
    (void)key;
    // The key removal (FUN_14214bbe0 -> FUN_14214b870 / FUN_14214b260) is not ported yet.
    FROST_UNIMPLEMENTED();
    return ERR_NOT_OPENED;
}

extern "C" unsigned long long FUN_14212ba10(unsigned long long, unsigned long long);
// translated: FUN_14212ba10
void WriteRepairMarker(const char* directory, int32_t* reason) {
    __try {
        FUN_14212ba10((unsigned long long)directory, (unsigned long long)reason);
    } __except (GenFault(0x14212ba10ull, GetExceptionInformation())) {
    }
}

}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1420d8f40=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14212ba10=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_14213de20 = (void*)&casc::HandleMissingArchive;
// ---- end of generated globals ----
