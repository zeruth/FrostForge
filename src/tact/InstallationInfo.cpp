#include "tact/InstallationInfo.hpp"
#include "os/Os.hpp"
#include <cstddef>
#include <new>

namespace tact {

namespace {

const char s_buildInfoFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\psv\\psv_build_info.cpp";
const char s_installationInfoFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\installation_info.cpp";

// PTR_1428e6050: the .build.info columns.
const PSVFIELD s_buildInfoFields[15] = {
    { "Branch", 8, 8, offsetof(BUILDINFOENTRY, m_branch), 0, &ParseString, nullptr, 4, 3, 0 },
    { "Active", 1, 1, offsetof(BUILDINFOENTRY, m_active), 0, &ParseBool, nullptr, 0, 1, 1 },
    { "Build Key", 16, 1, offsetof(BUILDINFOENTRY, m_buildKey), 0, &ParseHexKey, nullptr, 0, 2, 16 },
    { "CDN Key", 16, 1, offsetof(BUILDINFOENTRY, m_cdnKey), 0, &ParseHexKey, nullptr, 1, 2, 16 },
    { "Install Key", 16, 1, offsetof(BUILDINFOENTRY, m_installKey), 0, &ParseHexKey, nullptr, 1, 2, 16 },
    { "CDN Path", 8, 8, offsetof(BUILDINFOENTRY, m_cdnPath), 0, &ParseString, nullptr, 5, 3, 0 },
    { "CDN Hosts", 8, 8, offsetof(BUILDINFOENTRY, m_cdnHosts), offsetof(BUILDINFOENTRY, m_cdnHostCount), &ParseString,
        nullptr, 7, 3, 0 },
    { "CDN Servers", 8, 8, offsetof(BUILDINFOENTRY, m_cdnServers), offsetof(BUILDINFOENTRY, m_cdnServerCount),
        &ParseString, nullptr, 7, 3, 0 },
    { "Tags", 8, 8, offsetof(BUILDINFOENTRY, m_tags), 0, &ParseString, nullptr, 5, 3, 0 },
    { "Armadillo", 8, 8, offsetof(BUILDINFOENTRY, m_armadillo), 0, &ParseString, nullptr, 5, 3, 0 },
    { "Last Activated", 8, 8, offsetof(BUILDINFOENTRY, m_lastActivated), 0, &ParseString, nullptr, 5, 3, 0 },
    { "Version", 8, 8, offsetof(BUILDINFOENTRY, m_version), 0, &ParseString, nullptr, 5, 3, 0 },
    { "Keyring", 16, 1, offsetof(BUILDINFOENTRY, m_keyring), 0, &ParseHexKey, nullptr, 1, 2, 16 },
    { "Product", 8, 8, offsetof(BUILDINFOENTRY, m_product), 0, &ParseString, nullptr, 5, 3, 0 },
    { "BGDL Key", 16, 1, offsetof(BUILDINFOENTRY, m_bgdlKey), 0, &ParseHexKey, nullptr, 1, 2, 16 },
};

// PTR_1428e9d30: the .flavor.info column.
const PSVFIELD s_flavorInfoFields[1] = {
    { "Product Flavor", 8, 8, 0, 0, &ParseString, nullptr, 4, 3, 0 },
};

typedef PsvManifestReader<BUILDINFOENTRY, 15> BuildInfoReader;    // vtable 0x1428e6528
typedef PsvManifestReader<FLAVORINFOENTRY, 1> FlavorInfoReader;   // vtable 0x1428e9d88

template <class Reader>
PSVMANIFEST* ParseManifest(const char* text, uint32_t length, const PSVFIELD* fields) {
    blz::allocator* allocator = GetAllocator();
    blz::allocator* readerAllocator = GetAllocator();
    void* memory = allocator->reallocate(nullptr, sizeof(Reader), 8);
    Reader* reader = memory ? new (memory) Reader(readerAllocator, fields) : nullptr;
    if (!reader->Parse(text, length)) {
        blz::allocator* owner = GetAllocator();
        if (reader) {
            reader->~Reader();
            owner->deallocate(reader);
        }
        return nullptr;
    }
    return &reader->m_result;
}

}

// ref: FUN_1420da320
void BUILDINFOENTRY::Init(BUILDINFOENTRY* entry) {
    std::memset(entry, 0, sizeof(*entry));
    entry->m_product = "";
    entry->m_branch = "default";
    entry->m_cdnPath = "";
    entry->m_tags = "";
    entry->m_armadillo = "";
    entry->m_lastActivated = "";
    entry->m_version = "";
}

// ref: FUN_1420fa970
void FLAVORINFOENTRY::Init(FLAVORINFOENTRY* entry) {
    entry->m_flavor = "";
}

// ref: FUN_1420da8f0
PSVMANIFEST* ParseBuildInfo(const char* text, uint32_t length) {
    return ParseManifest<BuildInfoReader>(text, length, s_buildInfoFields);
}

// ref: FUN_1420fae20
PSVMANIFEST* ParseFlavorInfo(const char* text, uint32_t length) {
    return ParseManifest<FlavorInfoReader>(text, length, s_flavorInfoFields);
}

// ref: FUN_1420daa70
BUILDINFOENTRY* BuildInfoGetActive(PSVMANIFEST* buildInfo, const char* product) {
    uint32_t count = buildInfo->m_entryCount;
    auto entries = static_cast<BUILDINFOENTRY*>(buildInfo->m_entries);
    if (count == 0) {
        blz::log(g_logCategory, 3, s_buildInfoFile, 0x32, "Build info has no entries, cannot get an active entry.");
        return nullptr;
    }
    if (!product || *product == '\0') {
        const char* first = entries[0].m_product ? entries[0].m_product : "";
        BUILDINFOENTRY* active = nullptr;
        for (uint32_t i = 0; i < count; i++) {
            const char* name = entries[i].m_product ? entries[i].m_product : "";
            if (std::strcmp(name, first) != 0) {
                blz::log(g_logCategory, 3, s_buildInfoFile, 0x45,
                    "Build info has multiple products, but no product was specified.");
                return nullptr;
            }
            if (entries[i].m_active) {
                active = &entries[i];
            }
        }
        if (active) {
            return active;
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            const char* name = entries[i].m_product ? entries[i].m_product : "";
            if (entries[i].m_active && std::strcmp(name, product) == 0) {
                return &entries[i];
            }
        }
    }
    blz::log(g_logCategory, 3, s_buildInfoFile, 0x5D, "No build info entry found for product: %s", product ? product : "");
    return nullptr;
}

// ref: FUN_1420d9d70
uint32_t ReadFileToArray(const char* path, Array<char>* array) {
    OSFILE file;
    OsFileInit(&file);
    uint32_t error = OsFileOpen(&file, path, 2);
    if (error == 0) {
        uint64_t size = OsFileGetSize(&file);
        if (size == 0) {
            error = OsErrorFromCode(2);
        } else if (size < 0x100000000ull) {
            uint32_t bytes = static_cast<uint32_t>(size);
            if (array->m_count <= bytes && array->m_capacity < bytes) {
                array->Reserve(bytes);
            }
            array->m_count = bytes;
            uint32_t read = 0;
            error = OsFileRead(&file, array->m_data, bytes, &read);
            if (error == 0 && read != array->m_count) {
                error = OsErrorFromCode(1);
            }
        } else {
            error = OsErrorFromCode(1);
        }
    }
    OsFileClose(&file);
    return error;
}

// ref: FUN_1420d9e40
INSTALLATIONINFO* InstallationInfoRead(const char* path) {
    blz::allocator* allocator = GetAllocator();
    auto info = static_cast<INSTALLATIONINFO*>(allocator->reallocate(nullptr, sizeof(INSTALLATIONINFO), 8));
    if (info) {
        info->m_buildInfo = nullptr;
        info->m_flavorInfo = nullptr;
        info->m_active = nullptr;
    }
    if (!path || *path == '\0') {
        return info;
    }

    OSFILESTAT stat;
    uint32_t error = OsFileStat(path, &stat);
    if (error != 0) {
        char message[0x100];
        OsErrorString(message, sizeof(message), error);
        blz::log(g_logCategory, 4, s_installationInfoFile, 0x35, "Unable to stat install path: '%s'. Error (%u): %s", path,
            error, message);
        return info;
    }

    Array<char> buffer;
    buffer.Init(GetBufferAllocator());
    if (stat.m_flags & 1) {
        char file[0x30C];
        OsPathJoin(file, sizeof(file), path, ".flavor.info");
        if (ReadFileToArray(file, &buffer) == 0) {
            info->m_flavorInfo = ParseFlavorInfo(buffer.m_data, buffer.m_count);
        }
        if (info->m_flavorInfo) {
            OsPathJoin(file, sizeof(file), path, "..");
            path = file;
        }
        OsPathJoin(file, sizeof(file), path, ".build.info");
        if (ReadFileToArray(file, &buffer) == 0) {
            info->m_buildInfo = ParseBuildInfo(buffer.m_data, buffer.m_count);
        }
        if (info->m_buildInfo) {
            const char* flavor = nullptr;
            if (info->m_flavorInfo && info->m_flavorInfo->m_entryCount != 0) {
                flavor = static_cast<FLAVORINFOENTRY*>(info->m_flavorInfo->m_entries)[0].m_flavor;
            }
            info->m_active = BuildInfoGetActive(info->m_buildInfo, flavor);
        }
    } else {
        if (ReadFileToArray(path, &buffer) == 0) {
            info->m_buildInfo = ParseBuildInfo(buffer.m_data, buffer.m_count);
        }
        if (info->m_buildInfo) {
            info->m_active = BuildInfoGetActive(info->m_buildInfo, nullptr);
        }
    }
    buffer.Free();
    return info;
}

// ref: FUN_1420da1d0
void InstallationInfoFree(INSTALLATIONINFO* info) {
    if (info) {
        PsvManifestFree(info->m_buildInfo);
        PsvManifestFree(info->m_flavorInfo);
        GetAllocator()->deallocate(info);
    }
}

}
