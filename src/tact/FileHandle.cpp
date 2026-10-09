#include "tact/FileHandle.hpp"
#include "tact/AsyncState.hpp"
#include "tact/Common.hpp"
#include "tact/Container.hpp"
#include "tact/ImmediateRead.hpp"
#include "util/Unimplemented.hpp"

namespace tact {

namespace {

const char s_handleFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\file_handle.cpp";
const char s_instanceFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\instance.cpp";

}

// ref: FUN_1420d8d60
FILEHANDLE* FileHandleOpen(Instance* instance, const char* path, uint8_t* error) {
    uint8_t scratch = 1;
    uint8_t* result = error != nullptr ? error : &scratch;
    if (instance == nullptr || !instance->m_initialized) {
        blz::log(g_logCategory, 4, s_handleFile, 0x1b, "Unable to open a file handle. Invalid tact instance.");
        *result = 3;
        return nullptr;
    }
    FILEKEY key;
    *result = instance->m_fileManifest.Lookup(path, &key, 1);
    if (*result != 0) {
        blz::log(g_logCategory, 4, s_handleFile, 0x25, "Unable to lookup file with path %s. Error(%u) : %s", path,
            *result, ErrorString(*result));
        return nullptr;
    }
    // FILEKEY +0x30 holds the e-key's length; the container may keep fewer bytes.
    uint8_t keySize = instance->m_container->GetKeySize();
    if (keySize < key.m_eSpecFlags) {
        key.m_eSpecFlags = keySize;
    }
    auto handle = static_cast<FILEHANDLE*>(PackageAlloc(GetBufferAllocator(), sizeof(FILEHANDLE), 8, false, "", 0));
    if (handle == nullptr) {
        return nullptr;
    }
    handle->m_instance = instance;
    handle->m_key = key;
    handle->m_owner = nullptr;
    return handle;
}

// ref: FUN_1420d8d10
void FileHandleClose(FILEHANDLE* handle) {
    blz::allocator* allocator = GetBufferAllocator();
    if (handle == nullptr) {
        return;
    }
    FILEHANDLE::RefCounted* owner = handle->m_owner;
    if (owner != nullptr && InterlockedDecrement(&owner->m_refs) == 0) {
        owner->Destroy();
    }
    PackageFree(allocator, handle);
}

// ref: FUN_1420d6e10
uint8_t ImmediateReadByHandle(FILEHANDLE* handle, void* dest, uint32_t offset, uint32_t size, uint32_t* read,
    uint32_t flags) {
    return ImmediateReadByKey(handle->m_instance, &handle->m_key, dest, offset, size, read, flags);
}

// The span read state (FUN_1420d3870 / FUN_1420e5320, and the already-done path through
// FUN_1420d3be0 / FUN_1420f7810 for flag 0x20) is not ported.
// ref: FUN_1420d4970
uint64_t InstanceStartSpanReadByHandle(FILEHANDLE* handle, void* dest, uint32_t offset, uint32_t size,
    OPCALLBACK callback, void* context, uint8_t priority, uint32_t flags) {
    (void)dest;
    (void)offset;
    (void)size;
    (void)callback;
    (void)context;
    (void)priority;
    (void)flags;
    if (handle == nullptr || handle->m_instance == nullptr) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x2ee, "Unable to start an async file span read. Invalid file handle.");
        return 0;
    }
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1420d4fd0
uint64_t InstanceStartSpanReadByName(Instance* instance, const char* path, void* dest, uint32_t offset, uint32_t size,
    OPCALLBACK callback, void* context, uint8_t priority, uint32_t flags) {
    (void)path;
    (void)dest;
    (void)offset;
    (void)size;
    (void)callback;
    (void)context;
    (void)priority;
    (void)flags;
    if (instance == nullptr || !instance->m_initialized) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x2a4, "Unable to start an async file span read. Invalid tact instance.");
        return 0;
    }
    FROST_UNIMPLEMENTED();
    return 0;
}

// The span-resident state (async_file_span_resident_state.cpp) is not ported.
// ref: FUN_1420d4ed0
uint64_t InstanceMakeSpanResident(Instance* instance, const char* path, uint32_t offset, uint32_t size,
    uint64_t a5, OPCALLBACK callback, void* context) {
    (void)path;
    (void)offset;
    (void)size;
    (void)a5;
    (void)callback;
    (void)context;
    if (instance == nullptr || !instance->m_initialized) {
        blz::log(g_logCategory, 4, s_instanceFile, 0x52d, "Unable to make file span resident. Invalid tact instance.");
        return 0;
    }
    FROST_UNIMPLEMENTED();
    return 0;
}

}
