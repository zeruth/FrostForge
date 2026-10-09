#ifndef TACT_FILE_HANDLE_HPP
#define TACT_FILE_HANDLE_HPP

// pkg/tact/3.6.39/src/file_handle.cpp: a file looked up once by path, its keys kept for the
// reads that follow.

#include "tact/Instance.hpp"
#include <cstddef>
#include <cstdint>

namespace tact {

// 0x50 bytes, from the buffer allocator.
struct FILEHANDLE {
    Instance* m_instance;           // 0x00
    FILEKEY m_key;                  // 0x08
    struct RefCounted {
        virtual ~RefCounted();
        virtual void Destroy();     // 0x08
        volatile long m_refs;       // 0x08
    }* m_owner;                     // 0x48: released with the handle
};
static_assert(offsetof(FILEHANDLE, m_owner) == 0x48, "FILEHANDLE layout");
static_assert(sizeof(FILEHANDLE) == 0x50, "FILEHANDLE is 0x50 bytes");

// Looks `path` up in the file manifest; null (and `error` set) when it is not there.
FILEHANDLE* FileHandleOpen(Instance* instance, const char* path, uint8_t* error);
void FileHandleClose(FILEHANDLE* handle);

uint8_t ImmediateReadByHandle(FILEHANDLE* handle, void* dest, uint32_t offset, uint32_t size, uint32_t* read,
    uint32_t flags);

// The async span reads (an operation id, DAT_1428e4ca0 when refused).
uint64_t InstanceStartSpanReadByHandle(FILEHANDLE* handle, void* dest, uint32_t offset, uint32_t size,
    OPCALLBACK callback, void* context, uint8_t priority, uint32_t flags);
uint64_t InstanceStartSpanReadByName(Instance* instance, const char* path, void* dest, uint32_t offset, uint32_t size,
    OPCALLBACK callback, void* context, uint8_t priority, uint32_t flags);
uint64_t InstanceMakeSpanResident(Instance* instance, const char* path, uint32_t offset, uint32_t size,
    uint64_t a5, OPCALLBACK callback, void* context);
uint64_t InstanceCheckResidency(Instance* instance, const char* path, uint32_t offset, uint32_t size);

}

#endif
