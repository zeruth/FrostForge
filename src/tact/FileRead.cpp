#include "tact/FileRead.hpp"
#include "tact/Codec.hpp"
#include "tact/Common.hpp"
#include "tact/Container.hpp"
#include "tact/DownloadQueue.hpp"
#include "tact/ReadQueue.hpp"
#include "tact/TaskScheduler.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <new>

namespace tact {

namespace {

const char s_readFile[] =
    "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\async_state\\async_file_read_state.cpp";

// The truncation result: 0xe when the caller asked past the end of the file, else success.
uint8_t TruncationResult(FileReadState* state) {
    return state->m_truncated ? 0xe : 0;
}

// The read queue's callback.
// ref: FUN_1420e38d0
void ReadDone(void* context, uint8_t result) {
    auto state = static_cast<FileReadState*>(context);
    AsyncStateScope scope(state);
    state->OnRead(result);
}

// ref: FUN_1420e2ae0
void StepTask(void* argument) {
    auto state = static_cast<FileReadState*>(argument);
    AsyncStateScope scope(state);
    state->Step();
}

// The states the patch, repair, download and block-patch paths go through are not ported
// (streaming is off, see DownloadQueueStart): a read that reaches one fails.
uint8_t Unported(FileReadState* state, const char* what) {
    (void)state;
    (void)what;
    FROST_UNIMPLEMENTED();
    return 7;
}

}

// ref: FUN_1420e2590
FileReadState::FileReadState(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context)
    : AsyncState(instance, opId, callback, context) {
    this->m_state = FR_IDLE;
    this->m_link[0] = &this->m_link;
    this->m_link[1] = &this->m_link;
    std::memset(&this->m_key, 0, sizeof(this->m_key));
    this->m_dest = nullptr;
    this->m_truncated = 0;
    this->m_offset = 0;
    this->m_size = 0;
    this->m_skipped = 0;
    this->m_priority = 1;
    this->m_flags = 0;
    this->m_localOnly = 0;
    this->m_codec = 0;
    this->m_deferredResult = 0;
    this->m_flag10 = 0;
    this->m_flag11 = 0;
    this->m_request = nullptr;
    this->m_bufferSize = 0;
    this->m_buffer = nullptr;
    this->m_header = nullptr;
    this->m_headerSize = 0;
    this->m_f0 = nullptr;
    this->m_eSpan[0] = 0;
    this->m_eSpan[1] = 0;
    this->m_cSpan[0] = 0;
    this->m_cSpan[1] = 0;
    this->m_corrupt.Init();
    this->m_patchCount = 0;
    this->m_patchPool.Init(GetBufferAllocator(), 0x38, 8, 8, 1);
    this->m_patchAllocator = GetBufferAllocator();
    this->m_patches = nullptr;
    this->m_patchUsed = 0;
    this->m_patchCapacity = 0;
    this->m_spanAllocator = GetBufferAllocator();
    this->m_spans = nullptr;
    this->m_spanCount = 0;
    this->m_spanCapacity = 0;
    this->m_downloadOp = 0;
    std::memset(this->m_download, 0, sizeof(this->m_download));
    this->m_downloadError = 0;
    this->m_patchReserved = 0;
    this->m_extra = 0;
}

// ref: FUN_1420e23b0
FileReadState::~FileReadState() {
    if (this->m_buffer && this->m_buffer != this->m_dest) {
        this->m_instance->m_scratch.Free(this->m_buffer);
    }
    this->m_buffer = nullptr;
    this->m_bufferSize = 0;
    if (this->m_patchReserved != 0) {
        // The patch reservation (FUN_1421023b0 on the download queue, +0x490) belongs to the
        // block patch path, which is not ported.
        FROST_UNIMPLEMENTED();
    }
    if (this->m_spans) {
        this->m_spanAllocator->deallocate(this->m_spans);
    }
    if (this->m_patches) {
        this->m_patchAllocator->deallocate(this->m_patches);
    }
    this->m_patchPool.Destroy();
    this->m_corrupt.Destroy();
    if (this->m_header) {
        this->m_header->Release();
    }
    auto link = reinterpret_cast<void**>(&this->m_link);
    static_cast<void**>(link[1])[0] = link[0];
    static_cast<void**>(link[0])[1] = link[1];
    link[1] = link;
    link[0] = link;
}

// Back to the instance's pool.
// ref: FUN_1420e31a0
void FileReadState::Destroy() {
    Instance* instance = this->m_instance;
    this->~FileReadState();
    AcquireSRWLockExclusive(&instance->m_fileReadPoolLock);
    instance->m_fileReadPool.Free(this);
    ReleaseSRWLockExclusive(&instance->m_fileReadPoolLock);
}

// The shared `return 0`.
// ref: FUN_1402ca290
uint8_t FileReadState::GetType() {
    return 0;
}

// The bytes the read produced.
// ref: FUN_1420e32e0
int32_t FileReadState::GetDetail() {
    if (this->m_result != 0 && this->m_result != 0xe) {
        return 0;
    }
    return static_cast<int32_t>(this->m_skipped + this->m_size);
}

// ref: FUN_1420e3650
void FileReadState::OnCancel() {
    switch (this->m_state) {
    case FR_IDLE:
    case FR_STARTED:
        this->m_state = FR_DONE;
        this->Complete(2);
        break;
    case FR_HEADER_PENDING:
    case FR_READING:
        if (this->m_instance->m_readQueue.Cancel(this->m_request)) {
            this->m_state = FR_DONE;
            this->m_request = nullptr;
            this->Complete(2);
        }
        break;
    case FR_PATCHING: {
        uint32_t i = 0;
        while (i < this->m_patchUsed) {
            auto patch = static_cast<uint8_t*>(this->m_patches[i]);
            if (!this->m_instance->m_readQueue.Cancel(*reinterpret_cast<READREQUEST**>(patch + 0x20))) {
                i++;
                continue;
            }
            this->m_patchPool.Free(patch);
            if (i < this->m_patchUsed - 1) {
                this->m_patches[i] = this->m_patches[this->m_patchUsed - 1];
            }
            this->m_patchUsed--;
        }
        if (this->m_patchCount == 0) {
            this->m_state = FR_DONE;
            this->Complete(2);
        }
        break;
    }
    case FR_CANCEL_DOWNLOAD:
        CancelOp(this->m_instance, this->m_downloadOp);
        break;
    case FR_FETCHING:
        // Cancelling the span downloads (FUN_1420ed000) belongs to the download path.
        FROST_UNIMPLEMENTED();
        break;
    default:
        break;
    }
}

// ref: FUN_1420e3300
void FileReadState::Start(const FILEKEY* key, uint8_t* dest, uint32_t offset, uint32_t size, uint8_t priority,
    uint32_t flags, bool defer, uint64_t extra) {
    if (defer) {
        this->m_state = FR_STARTED;
    }
    uint32_t fileSize = key->m_csize;
    if (fileSize < offset || (offset == fileSize && size != 0)) {
        char text[0x21];
        KeyToString(text, sizeof(text), key->m_ekey);
        blz::log(g_logCategory, 4, s_readFile, 0x13b, "Invalid read offset requested for file %s. Offset: %u FileSize: %u",
            text, offset, fileSize);
    } else {
        uint32_t take = size;
        if (size == 0 || fileSize < offset + size) {
            take = fileSize - offset;
        }
        this->m_key = *key;
        this->m_truncated = take < size;
        this->m_offset = offset;
        this->m_size = take;
        this->m_priority = priority;
        this->m_flags = flags;
        this->m_dest = dest;
        bool writable = this->m_instance->m_container->IsWritable();
        this->m_localOnly = (!writable || !DownloadQueueStreaming(this->m_instance) || (this->m_flags & 0x10) != 0) ? 1 : 0;
        this->m_extra = extra;
        if (this->m_key.m_eSpec && *this->m_key.m_eSpec) {
            this->m_codec = ESpecType(this->m_key.m_eSpec, nullptr);
        }
        if (this->m_codec == 0) {
            this->m_codec = 6;
        }
        bool whole = this->m_offset == 0 && this->m_size == this->m_key.m_csize;
        if (whole || this->m_codec == 6 || this->m_codec == 1) {
            if (defer) {
                this->m_deferredResult = 0;
                return;
            }
            if (!whole) {
                if (this->m_codec != 1) {
                    this->m_state = FR_HEADER;
                    this->ReadHeader();
                    return;
                }
                if ((this->m_flags >> 1 & 1) != 0) {
                    if (this->m_buffer && this->m_buffer != this->m_dest) {
                        this->m_instance->m_scratch.Free(this->m_buffer);
                    }
                    this->m_cSpan[0] = this->m_offset;
                    this->m_cSpan[1] = this->m_size;
                    this->m_eSpan[0] = this->m_offset;
                    this->m_eSpan[1] = this->m_size;
                    this->m_bufferSize = this->m_size;
                    this->m_buffer = this->m_dest;
                    this->m_state = FR_READ;
                    this->SubmitRead();
                    return;
                }
                char text[0x21];
                KeyToString(text, sizeof(text), this->m_key.m_ekey);
                blz::log(g_logCategory, 3, s_readFile, 0x1e9,
                    "Attempting uncompressed span read with verification enabled. This is not recommended. E-Key: %s",
                    text);
            }
            this->PrepareBuffer();
            this->m_state = FR_READ;
            this->SubmitRead();
            return;
        }
        char text[0x21];
        KeyToString(text, sizeof(text), this->m_key.m_ekey);
        blz::log(g_logCategory, 4, s_readFile, 0x15f, "Attempted span read on non-block encoded file. Key: %s", text);
    }
    if (!defer) {
        this->m_state = FR_DONE;
        this->Complete(3);
    } else {
        this->m_deferredResult = 3;
    }
}

// A whole read decodes into the destination; a short one into scratch the size of the file.
// ref: FUN_1420e44e0
void FileReadState::PrepareBuffer() {
    if (this->m_buffer && this->m_buffer != this->m_dest) {
        this->m_instance->m_scratch.Free(this->m_buffer);
    }
    uint32_t size = this->m_key.m_csize;
    this->m_cSpan[0] = 0;
    this->m_cSpan[1] = size;
    this->m_eSpan[0] = 0;
    this->m_eSpan[1] = this->m_key.m_esize;
    this->m_buffer = nullptr;
    this->m_bufferSize = size;
    if (this->m_size < size) {
        this->m_buffer = static_cast<uint8_t*>(this->m_instance->m_scratch.Allocate(size));
        return;
    }
    this->m_buffer = this->m_dest;
}

// ref: FUN_1420e4b20
bool FileReadState::SubmitRead() {
    if (this->m_size == 0) {
        this->m_state = FR_DONE;
        this->Complete(TruncationResult(this));
        return false;
    }
    uint8_t partial = (this->m_eSpan[0] == 0 && this->m_eSpan[1] == this->m_key.m_esize) ? 0 : 1;
    this->m_state = FR_READING;
    uint32_t flags = this->m_buffer == this->m_dest ? this->m_flags : this->m_flags & ~1u;
    ENCODINGHEADER* header = nullptr;
    if (this->m_header) {
        header = *reinterpret_cast<ENCODINGHEADER**>(reinterpret_cast<uint8_t*>(this->m_header) + 0x20);
    }
    uint64_t eSpan;
    uint64_t cSpan;
    std::memcpy(&eSpan, this->m_eSpan, 8);
    std::memcpy(&cSpan, this->m_cSpan, 8);
    this->m_instance->m_readQueue.Submit(this->m_key.m_ekey, this->m_key.m_eSpecFlags, this->m_key.m_eSpec,
        this->m_codec, header, &eSpan, &cSpan, this->m_buffer, partial, this->m_priority, flags, &ReadDone, this,
        &this->m_request, &this->m_corrupt);
    return false;
}

// A span of a block-encoded file needs the file's encoding header, from the header cache
// (+0xa28) or a header fetch.
// ref: FUN_1420e4c90
void FileReadState::ReadHeader() {
    // The header cache (FUN_1420db860) and the header fetch state (FUN_142107fc0 /
    // FUN_1421080b0) are not ported yet.
    FROST_UNIMPLEMENTED();
    this->m_state = FR_DONE;
    this->Complete(7);
}

// The header is known: the block cache may already hold the start of the span; the rest is
// read as the whole blocks covering it.
// ref: FUN_1420e4590
void FileReadState::OnHeader(uint8_t result) {
    if ((this->m_flags >> 3 & 1) != 0 && result != 0) {
        this->m_state = FR_DONE;
        this->Complete(result);
        return;
    }
    if (result == 0) {
        auto header = *reinterpret_cast<ENCODINGHEADER**>(reinterpret_cast<uint8_t*>(this->m_header) + 0x20);
        this->m_headerSize = EncodingHeaderSize(header->m_blockCount);
        if (this->m_buffer && this->m_buffer != this->m_dest) {
            this->m_instance->m_scratch.Free(this->m_buffer);
        }
        this->m_buffer = nullptr;
        this->m_bufferSize = 0;
        uint32_t cached = this->m_instance->m_blockCache.Read(this->m_key.m_ekey, this->m_dest, this->m_offset, this->m_size);
        if (cached == this->m_size) {
            this->m_state = FR_DONE;
            this->Complete(TruncationResult(this));
            return;
        }
        if (cached != 0) {
            this->m_offset += cached;
            this->m_skipped = cached;
            this->m_dest += cached;
            this->m_size -= cached;
        }
        uint32_t span[2] = { this->m_offset, this->m_size };
        if (EncodingHeaderMapSpan(header, span, this->m_eSpan, this->m_cSpan) == 0) {
            this->m_bufferSize = this->m_cSpan[1];
            if (this->m_cSpan[1] == this->m_size) {
                this->m_buffer = this->m_dest;
            } else {
                this->m_buffer = static_cast<uint8_t*>(this->m_instance->m_scratch.Allocate(this->m_cSpan[1]));
            }
            this->m_state = FR_READ;
            this->Schedule();
            return;
        }
        char key[0x21];
        char ekey[0x21];
        KeyToString(key, sizeof(key), this->m_key.m_ckey);
        KeyToString(ekey, sizeof(ekey), this->m_key.m_ekey);
        blz::log(g_logCategory, 4, s_readFile, 0x1bb, "Invalid C-Span (%u, %u) requested for file %s|%s.", this->m_offset,
            this->m_size, key, ekey);
        this->m_state = FR_DONE;
        this->Complete(3);
        return;
    }
    if (result == 6) {
        this->m_state = FR_BLOCK_PATCH;
        this->Schedule();
        return;
    }
    this->PrepareBuffer();
    if (result != 7) {
        // Marking the file corrupt (container slot 0xc8) belongs to the repair path.
        FROST_UNIMPLEMENTED();
    }
    this->m_state = FR_DOWNLOAD;
    this->Schedule();
}

// ref: FUN_1420e3950
void FileReadState::OnRead(uint8_t result) {
    this->m_request = nullptr;
    if (result != 0 && this->m_localOnly) {
        char key[0x21];
        char ekey[0x21];
        KeyToString(key, sizeof(key), this->m_key.m_ckey);
        KeyToString(ekey, sizeof(ekey), this->m_key.m_ekey);
        blz::log(g_logCategory, 4, s_readFile, 0x33a, "Read failed for file '%s|%s' while streaming is disabled. Error (%u): %s",
            key, ekey, result, ErrorString(result));
        switch (result) {
        case 2:
        case 10:
        case 0xb:
        case 0xd:
            this->m_state = FR_DONE;
            this->Complete(result);
            break;
        case 6:
            this->m_state = FR_BLOCK_PATCH;
            this->Schedule();
            break;
        case 8:
            // The container drops the corrupt file (slot 0xf8) on the repair path, not ported.
            FROST_UNIMPLEMENTED();
            this->m_state = FR_DONE;
            this->Complete(0xb);
            break;
        default:
            this->m_state = FR_DONE;
            this->Complete(7);
            break;
        }
        return;
    }
    if (result == 0) {
        if (this->m_buffer != this->m_dest) {
            this->m_state = FR_COPY;
            this->Schedule();
            return;
        }
        this->m_state = FR_DONE;
        this->Complete(TruncationResult(this));
        return;
    }
    if (result == 6) {
        this->m_state = FR_BLOCK_PATCH;
        this->Schedule();
        return;
    }
    if (result == 7) {
        if (this->m_header) {
            this->m_state = FR_PATCH;
            this->Schedule();
            return;
        }
    } else {
        if (result != 8) {
            this->m_state = FR_DONE;
            this->Complete(result);
            return;
        }
        StatsReport(&this->m_instance->m_cdnCallbacks, 0, "read_file.repair.start", 1, 1.0f);
        if (this->m_header) {
            this->m_state = FR_REPAIR;
            this->Schedule();
            return;
        }
    }
    if (this->m_codec == 1) {
        this->PrepareBuffer();
    }
    this->m_state = FR_DOWNLOAD;
    this->Schedule();
}

void FileReadState::Schedule() {
    this->m_instance->m_scheduler->Post(&StepTask, this, 0);
}

// The span went into scratch memory: it is copied out, and what was decoded past it is kept
// in the block cache.
// ref: FUN_1420e3020
uint8_t FileReadState::CopyOut() {
    this->m_state = FR_DONE;
    if (this->m_downloadError != 0) {
        char key[0x21];
        char ekey[0x21];
        KeyToString(key, sizeof(key), this->m_key.m_ckey);
        KeyToString(ekey, sizeof(ekey), this->m_key.m_ekey);
        blz::log(g_logCategory, 4, s_readFile, 0x619, "Failed to download missing data for target '%s|%s'. Error(%u) : %s",
            key, ekey, this->m_downloadError, ErrorString(this->m_downloadError));
        uint8_t error = this->m_downloadError;
        if (error != 0xb && error != 10 && error != 0xd && error != 2) {
            this->m_downloadError = 7;
            error = 7;
        }
        return error;
    }
    if (this->m_buffer != this->m_dest) {
        uint8_t* from = this->m_buffer + (this->m_offset - this->m_cSpan[0]);
        std::memcpy(this->m_dest, from, this->m_size);
        uint32_t rest = (this->m_cSpan[1] - this->m_size - this->m_offset) + this->m_cSpan[0];
        if (rest != 0) {
            this->m_instance->m_blockCache.Store(this->m_key.m_ekey, from + this->m_size, this->m_size + this->m_offset, rest);
        }
        this->m_instance->m_scratch.Free(this->m_buffer);
        this->m_buffer = nullptr;
    }
    return 0;
}

// ref: FUN_1420e2760
void FileReadState::Step() {
    if (this->m_cancelled) {
        this->m_state = FR_DONE;
        this->Complete(2);
        return;
    }
    uint8_t state = this->m_state;
    if (this->m_localOnly &&
        ((((state - 8) & 0xFA) == 0 && state != FR_CANCEL_DOWNLOAD) || state == FR_FETCH_ALL)) {
        this->m_state = FR_DONE;
        this->Complete(7);
        return;
    }
    uint8_t result = 1;
    bool waitForCallback = true;
    switch (state) {
    case FR_READ:
        result = this->SubmitRead() ? 1 : 0;
        break;
    case FR_PATCH:
        result = Unported(this, "patch (FUN_1420e5560)");
        break;
    case FR_REPAIR:
        result = Unported(this, "repair (FUN_1420e4930)");
        waitForCallback = false;
        break;
    case FR_DOWNLOAD:
        this->m_state = FR_DOWNLOADING;
        result = Unported(this, "download (FUN_1420df360)");
        break;
    case FR_BLOCK_PATCH:
        result = Unported(this, "block patch (FUN_1420e4e70)");
        break;
    case FR_FETCH_SPANS:
    case FR_FETCH_ALL:
        result = Unported(this, "span download (FUN_1420e57b0)");
        break;
    case FR_COPY:
        result = this->CopyOut();
        waitForCallback = false;
        break;
    default:
        this->m_state = FR_DONE;
        this->Complete(result);
        return;
    }
    if (result == 0) {
        if (waitForCallback) {
            return;
        }
        if (this->m_state != FR_DONE) {
            this->Schedule();
            return;
        }
        this->m_state = FR_DONE;
        result = TruncationResult(this);
    } else {
        this->m_state = FR_DONE;
    }
    this->Complete(result);
}

// ref: FUN_1420e2590
AsyncState* FileReadCreate(Instance* instance, uint64_t opId, OPCALLBACK callback, void* context) {
    AcquireSRWLockExclusive(&instance->m_fileReadPoolLock);
    void* memory = instance->m_fileReadPool.Allocate();
    ReleaseSRWLockExclusive(&instance->m_fileReadPoolLock);
    if (!memory) {
        return nullptr;
    }
    auto state = new (memory) FileReadState(instance, opId, callback, context);
    state->AddRef();
    return state;
}

// ref: FUN_1420e4910
void FileReadSetFlag10(AsyncState* state) {
    static_cast<FileReadState*>(state)->m_flag10 = 1;
}

// ref: FUN_1420e4920
void FileReadSetFlag11(AsyncState* state) {
    static_cast<FileReadState*>(state)->m_flag11 = 1;
}

// ref: FUN_1420e5210
void FileReadStart(AsyncState* state, FILEKEY* key, void* buffer, uint64_t offset, uint32_t size, uint32_t a5,
    uint32_t a6) {
    static_cast<FileReadState*>(state)->Start(key, static_cast<uint8_t*>(buffer), static_cast<uint32_t>(offset), size,
        static_cast<uint8_t>(a5), a6, false, 0);
}

// ref: FUN_1420e5250
void FileReadStartWhole(AsyncState* state, FILEKEY* key, void* buffer, uint32_t size, uint32_t a4, uint32_t a5) {
    auto read = static_cast<FileReadState*>(state);
    if (size < key->m_csize) {
        char text[0x21];
        KeyToString(text, sizeof(text), key->m_ekey);
        blz::log(g_logCategory, 4, s_readFile, 0x66,
            "Attempted full file read of file %s with buffer smaller than cSize. Expected %u, but got %u.", text,
            key->m_csize, size);
        read->m_state = FR_DONE;
        read->Complete(4);
        return;
    }
    read->Start(key, static_cast<uint8_t*>(buffer), 0, key->m_csize, static_cast<uint8_t>(a4), a5, false, 0);
}

// ref: FUN_1420e5470
void FileReadStartByName(AsyncState* state, const char* path, void* buffer, uint32_t size, uint8_t priority,
    uint32_t flags) {
    auto read = static_cast<FileReadState*>(state);
    FILEKEY key;
    uint8_t result = read->m_instance->m_fileManifest.Lookup(path, &key, 1);
    if (result != 0) {
        blz::log(g_logCategory, 4, s_readFile, 0x26, "Unable to lookup file with path %s. Error(%u) : %s", path, result,
            ErrorString(result));
        read->m_state = FR_DONE;
        read->Complete(result);
        return;
    }
    FileReadStartWhole(state, &key, buffer, size, priority, flags);
}

// ref: FUN_1420d3870
OPHANDLE* CreateFileRead(Instance* instance, OPHANDLE* out, OPCALLBACK callback, void* context) {
    OPREGISTRY& ops = instance->m_ops;
    AcquireSRWLockExclusive(&ops.m_lock);
    uint64_t id = ops.m_nextId++;
    AsyncState* state = FileReadCreate(ops.m_instance, id, callback, context);
    ops.Register(state);
    int64_t* lock = SpinLockExclusive(&state->m_lock);
    out->m_state = state;
    out->m_lock = lock;
    if (state) {
        state->AddRef();
    }
    if (state) {
        state->Release();
    }
    ReleaseSRWLockExclusive(&ops.m_lock);
    return out;
}

}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420df360=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420e4930=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420e4e70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420e5560=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420e57b0=GenForwardMissing")
// ---- end of link fallbacks ----
