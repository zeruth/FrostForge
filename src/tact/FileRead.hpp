#ifndef TACT_FILE_READ_HPP
#define TACT_FILE_READ_HPP

// pkg/tact/3.6.39/src/async_state/async_file_read_state.cpp: reads a file by its keys from
// the container (or the CDN), decodes it (BLTE) and hands the content to the caller's buffer.
// Whole files are read through the read queue straight into the caller's buffer; a span of a
// block-encoded file needs its encoding header first (the header cache, +0xa28) and decodes
// whole blocks into scratch memory.

#include "blz/ObjectPool.hpp"
#include "tact/AsyncState.hpp"
#include "tact/Instance.hpp"
#include "tact/U32Set.hpp"
#include <cstddef>
#include <cstdint>

namespace tact {

struct READREQUEST;

// The read's states (+0x50).
enum {
    FR_IDLE = 0,
    FR_STARTED = 1,
    FR_HEADER = 2,
    FR_HEADER_PENDING = 3,
    FR_READ = 4,
    FR_READING = 5,
    FR_PATCH = 6,
    FR_PATCHING = 7,
    FR_REPAIR = 8,
    FR_DOWNLOAD = 9,
    FR_DOWNLOADING = 10,
    FR_BLOCK_PATCH = 11,
    FR_CANCEL_DOWNLOAD = 12,
    FR_FETCH_SPANS = 13,
    FR_FETCH_ALL = 14,
    FR_FETCHING = 15,
    FR_COPY = 16,
    FR_DONE = 17,
};

// 0x268 bytes, from the instance's file read pool (vtable 0x1428e6cb8).
class FileReadState : public AsyncState {
    public:
    void* m_link[2];                // 0x40
    uint8_t m_state;                // 0x50
    FILEKEY m_key;                  // 0x58
    uint8_t* m_dest;                // 0x98
    uint8_t m_truncated;            // 0xa0: the caller asked for more than the file holds
    uint32_t m_offset;              // 0xa4
    uint32_t m_size;                // 0xa8
    uint32_t m_skipped;             // 0xac: served from the block cache
    uint8_t m_priority;             // 0xb0
    uint32_t m_flags;               // 0xb4
    uint8_t m_localOnly;            // 0xb8: no streaming: a failed read is final
    uint32_t m_codec;               // 0xbc: the e-spec type, 6 when there is none
    uint8_t m_deferredResult;       // 0xc0
    uint8_t m_flag10;               // 0xc1
    uint8_t m_flag11;               // 0xc2
    READREQUEST* m_request;         // 0xc8
    uint32_t m_bufferSize;          // 0xd0
    uint8_t* m_buffer;              // 0xd8: the decode target, the destination or scratch
    AsyncState* m_header;           // 0xe0: the header cache entry (its +0x20 is the parsed header)
    uint32_t m_headerSize;          // 0xe8
    void* m_f0;                     // 0xf0
    uint32_t m_eSpan[2];            // 0xf8
    uint32_t m_cSpan[2];            // 0x100
    U32SET m_corrupt;               // 0x108
    uint32_t m_patchCount;          // 0x138
    blz::object_pool m_patchPool;   // 0x140: 0x38-byte patch requests
    blz::allocator* m_patchAllocator;  // 0x1d8
    void** m_patches;               // 0x1e0
    uint32_t m_patchUsed;           // 0x1e8
    uint32_t m_patchCapacity;       // 0x1ec
    blz::allocator* m_spanAllocator;// 0x1f0
    uint64_t* m_spans;              // 0x1f8
    uint32_t m_spanCount;           // 0x200
    uint32_t m_spanCapacity;        // 0x204
    uint64_t m_downloadOp;          // 0x208
    uint8_t m_download[0x48];       // 0x210
    uint8_t m_downloadError;        // 0x258
    uint32_t m_patchReserved;       // 0x25c
    uint64_t m_extra;               // 0x260

    FileReadState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);
    ~FileReadState() override;

    void Destroy() override;
    uint8_t GetType() override;
    int32_t GetDetail() override;
    void OnCancel() override;

    void Start(const FILEKEY* key, uint8_t* dest, uint32_t offset, uint32_t size, uint8_t priority, uint32_t flags,
        bool defer, uint64_t extra);
    void PrepareBuffer();
    bool SubmitRead();
    void ReadHeader();
    void OnHeader(uint8_t result);
    void OnRead(uint8_t result);
    uint8_t CopyOut();
    void Step();
    void Schedule();
};
static_assert(offsetof(FileReadState, m_state) == 0x50, "FileReadState layout");
static_assert(offsetof(FileReadState, m_key) == 0x58, "FileReadState layout");
static_assert(offsetof(FileReadState, m_request) == 0xc8, "FileReadState layout");
static_assert(offsetof(FileReadState, m_corrupt) == 0x108, "FileReadState layout");
static_assert(offsetof(FileReadState, m_patchPool) == 0x140, "FileReadState layout");
static_assert(offsetof(FileReadState, m_patchAllocator) == 0x1d8, "FileReadState layout");
static_assert(offsetof(FileReadState, m_download) == 0x210, "FileReadState layout");
static_assert(offsetof(FileReadState, m_extra) == 0x260, "FileReadState layout");
static_assert(sizeof(FileReadState) == 0x268, "FileReadState is 0x268 bytes");

// Takes a state from the instance's file read pool; the caller registers it
// (CreateFileRead).
AsyncState* FileReadCreate(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context);

void FileReadSetFlag10(AsyncState* state);
void FileReadSetFlag11(AsyncState* state);

// (key, destination, offset, size, priority, flags)
void FileReadStart(AsyncState* state, FILEKEY* key, void* buffer, uint64_t offset, uint32_t size, uint32_t a5,
    uint32_t a6);
// (key, destination, size, priority, flags): the whole file; the buffer must hold its c-size.
void FileReadStartWhole(AsyncState* state, FILEKEY* key, void* buffer, uint32_t size, uint32_t a4, uint32_t a5);

// (path, destination, size, priority, flags): the path is looked up in the file manifest.
void FileReadStartByName(AsyncState* state, const char* path, void* buffer, uint32_t size, uint8_t priority,
    uint32_t flags);

// Registers a new file read with the instance; the handle keeps a reference and the lock.
OPHANDLE* CreateFileRead(Instance* instance, OPHANDLE* out, OPCALLBACK callback, void* context);

}

#endif
