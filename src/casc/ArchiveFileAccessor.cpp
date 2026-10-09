#include "casc/ArchiveFileAccessor.hpp"
#include "blz/Log.hpp"
#include "blz/Sync.hpp"
#include "casc/Common.hpp"
#include <cstring>

namespace casc {

namespace {

const char s_accessorFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\archive_file_accessor.cpp";

volatile int64_t s_requestCounter;  // DAT_143477850

// ref: FUN_142146b30
void LogArchiveReadError(uint64_t archive, uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_accessorFile, 0x3c, "Failed to read archive %llu. Error (%u): %s", archive, error, text);
}

// ref: FUN_142146bb0
void LogReadError(uint32_t size, uint64_t offset, uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_accessorFile, 0x34,
        "Failed to read %u bytes from container archive at offset %llu. Error (%u): %s", size, offset, error, text);
}

// Copies a piece out of the block its buffer now holds; what the read fell short of is zeroed.
// ref: FUN_142145250
void CompletePiece(ARCHIVEBUFFERS* buffers, READPIECE* piece, uint32_t read, uint8_t error) {
    (void)buffers;
    std::memcpy(piece->m_dest, piece->m_buffer->m_data + piece->m_bufferOffset, piece->m_size);
    uint32_t available = read - piece->m_bufferOffset;
    piece->m_buffer = nullptr;
    uint32_t size = piece->m_size;
    uint32_t done = size <= available ? size : available;
    piece->m_done = done;
    if (done < size) {
        std::memset(piece->m_dest + done, 0, size - done);
    }
    piece->m_error = error;
}

void ReleaseFile(ARCHIVEFILE* file) {
    if (file) {
        file->Release();
    }
}

// A growable array of keys with eight inline (the capacity's top bit marks the inline store).
struct KEYLIST {
    uint64_t* m_data;
    uint32_t m_count;
    uint32_t m_capacity;
    uint64_t m_inline[8];
};

void KeyListPush(KEYLIST* list, blz::allocator* allocator, uint64_t key) {
    uint32_t capacity = list->m_capacity & 0x7FFFFFFF;
    if (list->m_count == capacity && capacity < list->m_count + 1) {
        uint32_t grown = capacity + (list->m_capacity >> 1 & 0x3FFFFFFF);
        if (grown < list->m_count + 1) {
            grown = list->m_count + 1;
        }
        if (grown < 8) {
            grown = 8;
        }
        auto data = static_cast<uint64_t*>(allocator->allocate(uint64_t(grown) * 8, 8));
        std::memcpy(data, list->m_data, uint64_t(list->m_count) * 8);
        if (list->m_data != list->m_inline) {
            allocator->deallocate(list->m_data);
        }
        list->m_data = data;
        list->m_capacity = grown & 0x7FFFFFFF;
    }
    list->m_data[list->m_count] = key;
    list->m_count++;
}

}

// A process-wide id for a pending read; the top byte carries a tag.
// ref: FUN_142143340
uint64_t NewRequestKey(uint8_t tag) {
    int64_t previous = _InterlockedExchangeAdd64(&s_requestCounter, 1);
    return (static_cast<uint64_t>(previous) + 1 & 0xFFFFFFFFFFFFFFull) | static_cast<uint64_t>(tag) << 56;
}

// Splits a read into an unaligned head, whole blocks and an unaligned tail.
// ref: FUN_142145860
void BuildReadPlan(ARCHIVEBUFFERS* buffers, READPLAN* plan, ARCHIVEFILE* file, void* dest, uint64_t offset, uint32_t size,
    READCALLBACK callback, void* context) {
    auto out = static_cast<uint8_t*>(dest);
    ARCHIVEFILE* previous = plan->m_file;
    uint64_t block = ~(uint64_t(buffers->m_size) - 1) & offset;
    if (previous != file) {
        plan->m_file = file;
        if (file) {
            file->AddRef();
        }
        if (previous) {
            previous->Release();
        }
    }
    if (block < offset) {
        uint32_t into = static_cast<uint32_t>(offset) - static_cast<uint32_t>(block);
        plan->m_head.m_offset = block;
        uint32_t take = buffers->m_size - into;
        plan->m_head.m_dest = out;
        plan->m_head.m_bufferOffset = into;
        if (size < take) {
            take = size;
        }
        plan->m_head.m_size = take;
        out += take;
        block += buffers->m_size;
        size -= take;
    }
    if (buffers->m_size <= size) {
        plan->m_middleOffset = block;
        plan->m_middleDest = out;
        uint32_t whole = (0u - buffers->m_size) & size;
        plan->m_middleDone = 0;
        out += whole;
        plan->m_middleSize = whole;
        block += whole;
        size -= whole;
    }
    if (size != 0) {
        plan->m_tail.m_offset = block;
        plan->m_tail.m_size = size;
        plan->m_tail.m_dest = out;
        plan->m_tail.m_bufferOffset = 0;
    }
    plan->m_key = 0;
    plan->m_callback = callback;
    plan->m_callbackContext = context;
}

// The buffer holding (archive, block) with at least `needed` bytes (or being filled), and
// otherwise the least recently used idle one; *hit says the data is there now.
// ref: FUN_1421452e0
ARCHIVEBUFFER* FindBuffer(ARCHIVEBUFFERS* buffers, uint64_t archive, uint64_t offset, uint32_t needed, bool* hit,
    ARCHIVEBUFFER* exclude) {
    *hit = false;
    if (buffers->m_count == 0) {
        return nullptr;
    }
    ARCHIVEBUFFER* best = nullptr;
    uint64_t bestAge = 0;
    ARCHIVEBUFFER* chosen = nullptr;
    for (uint32_t i = 0; i < buffers->m_count; i++) {
        ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
        if (!buffer->m_invalid && buffer->m_archive == archive && buffer->m_offset == offset &&
            (needed <= buffer->m_valid || buffer->m_inFlight)) {
            *hit = !buffer->m_inFlight;
            chosen = buffer;
            break;
        }
        if (!buffer->m_inFlight && buffer != exclude) {
            uint64_t age = buffers->m_clock - buffer->m_lastUse;
            if (bestAge < age || !best) {
                best = buffer;
                bestAge = age;
            }
        }
        chosen = best;
    }
    if (chosen) {
        buffers->m_clock++;
        chosen->m_lastUse = buffers->m_clock;
    }
    return chosen;
}

// ref: FUN_1421436f0
void ARCHIVEFILEACCESSOR::Construct(blz::allocator* allocator, uint32_t bufferCount, uint32_t bufferSize,
    uint8_t directMiddle, aio::CONTEXT* aio, uint64_t a7, uint8_t a8, TRUNCATEDREAD truncated, void* context) {
    (void)a7;
    (void)a8;
    this->m_ownsAio = 0;
    this->m_waiting = 0;
    this->m_buffers = nullptr;
    this->m_aio = aio;
    this->m_truncated = truncated;
    this->m_truncatedContext = context;
    this->m_allocator = allocator;
    this->m_owner = nullptr;
    this->m_directMiddle = directMiddle;
    this->m_pending.init(allocator);
    this->m_planPool.Init(allocator, 0xc0, 8, 0x20, 1);
    InitializeSRWLock(&this->m_planPoolLock);
    this->m_requestPool.Init(allocator, 0x40, 8, 0x20, 1);
    InitializeSRWLock(&this->m_requestPoolLock);
    InitializeSRWLock(&this->m_pendingLock);
    blz::sync_semaphore_init(&this->m_drained, 0);
    if (bufferCount != 0 && bufferSize != 0) {
        auto buffers = static_cast<ARCHIVEBUFFERS*>(allocator->allocate(sizeof(ARCHIVEBUFFERS), 8));
        if (buffers) {
            buffers->m_allocator = allocator;
            buffers->m_count = bufferCount;
            buffers->m_size = bufferSize;
            buffers->m_clock = 0;
            buffers->m_shutdown = 0;
            buffers->m_buffers =
                static_cast<ARCHIVEBUFFER*>(allocator->allocate(uint64_t(bufferCount) * sizeof(ARCHIVEBUFFER), 0x10));
            for (uint32_t i = 0; i < bufferCount; i++) {
                ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
                buffer->m_archive = 0;
                buffer->m_offset = 0;
                buffer->m_data = nullptr;
                buffer->m_lastUse = 0;
                buffer->m_valid = 0;
                buffer->m_aioKey = 0;
                buffer->m_allocator = allocator;
                buffer->m_cached = 0;
                buffer->m_invalid = 0;
                buffer->m_inFlight = 0;
                buffer->m_waiters.init(allocator);
                buffer->m_data = static_cast<uint8_t*>(allocator->allocate(bufferSize, 0x1000));
                blz::sync_event_init(&buffer->m_ready, true, false);
            }
            InitializeSRWLock(&buffers->m_lock);
            blz::sync_semaphore_init(&buffers->m_available, 0);
        }
        this->m_buffers = buffers;
    }
    if (!this->m_aio) {
        aio::PARAMS params = {};
        params.m_affinity = ~uint64_t(0);
        params.m_priority = 4;
        params.m_0c = 1;
        params.m_stackSize = 0;
        params.m_18 = 0;
        params.m_20 = 0;
        params.m_buffered = directMiddle;
        params.m_allocator = allocator;
        uint8_t result = 0;
        this->m_aio = aio::ContextCreate(&params, &result);
        this->m_ownsAio = 1;
    }
}

// ref: FUN_142143970
void ARCHIVEFILEACCESSOR::Destroy() {
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    if (buffers) {
        aio::CONTEXT* aio = this->m_aio;
        for (uint32_t i = 0; i < buffers->m_count; i++) {
            ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
            if (buffer->m_aioKey != 0) {
                if (!aio::FileIsDone(aio, buffer->m_aioKey)) {
                    aio::FileCancel(aio, buffer->m_aioKey);
                }
                while (buffer->m_aioKey != 0) {
                    blz::sync_wait(&buffer->m_ready);
                }
            }
        }
    }
    if (this->m_aio && this->m_ownsAio) {
        aio::ContextDestroy(this->m_aio);
    }
    if (buffers) {
        blz::allocator* allocator = this->m_allocator;
        blz::sync_close(&buffers->m_available);
        for (uint32_t i = 0; i < buffers->m_count; i++) {
            ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
            blz::sync_close(&buffer->m_ready);
            buffer->m_allocator->deallocate(buffer->m_data);
            buffer->m_waiters.destroy();
        }
        buffers->m_allocator->deallocate(buffers->m_buffers);
        allocator->deallocate(buffers);
    }
    blz::sync_close(&this->m_drained);
    this->m_requestPool.Destroy();
    this->m_planPool.Destroy();
    this->m_pending.destroy();
}

// Reads straight from the file unless the buffers can serve it (an aligned read never goes
// through them). Takes over the file reference.
// ref: FUN_142144dd0
int32_t ARCHIVEFILEACCESSOR::Read(ARCHIVEFILE** file, uint64_t offset, uint32_t size, void* dest, uint32_t* read) {
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    bool direct = !buffers || buffers->m_shutdown;
    if (!direct) {
        uint32_t blockSize = buffers->m_size;
        direct = (~(uint64_t(blockSize) - 1) & offset) == offset && ((0u - blockSize) & size) == size;
    }
    bool fallback = false;
    bool ok = true;
    if (!direct) {
        ok = this->CachedRead(*file, dest, offset, size, &fallback, read);
        direct = fallback;
    }
    ARCHIVEFILE* held = *file;
    if (direct) {
        uint32_t error = OsFileReadAt(&held->m_file, dest, size, offset, read);
        if (error != 0) {
            LogReadError(size, offset, error);
            ReleaseFile(held);
            return 8;
        }
    } else if (!ok) {
        ReleaseFile(held);
        return 8;
    }
    ReleaseFile(held);
    return 0;
}

// ref: FUN_142145ff0
bool ARCHIVEFILEACCESSOR::CachedRead(ARCHIVEFILE* file, void* dest, uint64_t offset, uint32_t size, bool* fallback,
    uint32_t* read) {
    READPLAN plan;
    std::memset(&plan, 0, sizeof(plan));
    plan.m_spanOffset = ~uint64_t(0);
    plan.m_middleDone = 1;
    *fallback = false;
    BuildReadPlan(this->m_buffers, &plan, file, dest, offset, size, nullptr, nullptr);
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    AcquireSRWLockExclusive(&buffers->m_lock);
    bool result;
    if (this->m_buffers->m_shutdown) {
        *fallback = true;
        ReleaseSRWLockExclusive(&buffers->m_lock);
        result = false;
    } else {
        bool hit;
        if (plan.m_head.m_size != 0) {
            ARCHIVEBUFFER* buffer = FindBuffer(this->m_buffers, plan.m_file->m_key.m_archive, plan.m_head.m_offset,
                plan.m_head.m_bufferOffset + plan.m_head.m_size, &hit, nullptr);
            if (hit) {
                std::memcpy(plan.m_head.m_dest, buffer->m_data + plan.m_head.m_bufferOffset, plan.m_head.m_size);
                plan.m_head.m_done = plan.m_head.m_size;
            }
        }
        if (plan.m_tail.m_size != 0) {
            ARCHIVEBUFFER* buffer = FindBuffer(this->m_buffers, plan.m_file->m_key.m_archive, plan.m_tail.m_offset,
                plan.m_tail.m_bufferOffset + plan.m_tail.m_size, &hit, nullptr);
            if (hit) {
                std::memcpy(plan.m_tail.m_dest, buffer->m_data + plan.m_tail.m_bufferOffset, plan.m_tail.m_size);
                plan.m_tail.m_done = plan.m_tail.m_size;
            }
        }
        ReleaseSRWLockExclusive(&buffers->m_lock);
        if (plan.m_head.m_size == 0 || plan.m_head.m_done != 0 || this->FillPiece(&file->m_file, &plan, &plan.m_head)) {
            uint32_t middleRead = 0;
            bool failed = false;
            if (plan.m_middleSize != 0 && plan.m_head.m_done == plan.m_head.m_size) {
                uint32_t blockSize = this->m_buffers->m_size;
                if (!this->m_directMiddle &&
                    (reinterpret_cast<uint64_t>(plan.m_middleDest) & (uint64_t(blockSize) - 1)) != 0) {
                    // A misaligned destination takes the middle a block at a time through the buffers.
                    READPLAN blocks = plan;
                    if (blocks.m_file) {
                        blocks.m_file->AddRef();
                    }
                    for (uint32_t at = 0; at < plan.m_middleSize; at += this->m_buffers->m_size) {
                        blocks.m_head.m_dest = plan.m_middleDest + at;
                        blocks.m_head.m_offset = plan.m_middleOffset + at;
                        blocks.m_head.m_size = this->m_buffers->m_size;
                        blocks.m_head.m_done = 0;
                        blocks.m_head.m_buffer = nullptr;
                        blocks.m_head.m_bufferOffset = 0;
                        blocks.m_head.m_error = 0;
                        if (!this->FillPiece(&file->m_file, &blocks, &blocks.m_head)) {
                            *read = 0;
                            ReleaseFile(blocks.m_file);
                            failed = true;
                            break;
                        }
                        middleRead += this->m_buffers->m_size;
                    }
                    if (!failed) {
                        ReleaseFile(blocks.m_file);
                    }
                } else {
                    uint32_t error = OsFileReadAt(&file->m_file, plan.m_middleDest, plan.m_middleSize, plan.m_middleOffset,
                        &middleRead);
                    if (error != 0) {
                        LogArchiveReadError(file->m_key.m_archive, error);
                        *read = 0;
                        failed = true;
                    }
                }
            }
            if (failed) {
                result = false;
            } else if (plan.m_tail.m_size == 0 || middleRead != plan.m_middleSize || plan.m_tail.m_done != 0 ||
                this->FillPiece(&file->m_file, &plan, &plan.m_tail)) {
                if (plan.m_head.m_done < plan.m_head.m_size) {
                    *read = plan.m_head.m_done;
                } else if (middleRead < plan.m_middleSize) {
                    *read = middleRead + plan.m_head.m_done;
                } else {
                    *read = plan.m_tail.m_done + middleRead + plan.m_head.m_done;
                }
                result = true;
            } else {
                *read = 0;
                result = false;
            }
        } else {
            result = false;
            *read = 0;
        }
    }
    ReleaseFile(plan.m_file);
    return result;
}

// ref: FUN_142144d90
void ARCHIVEFILEACCESSOR::RemovePending(uint64_t key) {
    this->m_pending.remove(key);
    if (this->m_waiting && this->m_pending.m_count == 0) {
        this->m_waiting = 0;
        blz::sync_semaphore_release(&this->m_drained);
    }
}

// Serves one piece through a buffer: from memory when the block is there, otherwise by
// filling the least recently used buffer (or waiting for whoever is filling it).
// ref: FUN_1421453e0
bool ARCHIVEFILEACCESSOR::FillPiece(OSFILE* file, READPLAN* plan, READPIECE* piece) {
    uint64_t offset = piece->m_offset;
    uint64_t archive = plan->m_file->m_key.m_archive;
    ARCHIVEBUFFER* chosen;
    bool hit;
    SRWLOCK* cacheLock;
    for (;;) {
        cacheLock = &this->m_buffers->m_lock;
        AcquireSRWLockExclusive(cacheLock);
        ARCHIVEBUFFERS* buffers = this->m_buffers;
        uint64_t bestAge = 0;
        ARCHIVEBUFFER* best = nullptr;
        chosen = nullptr;
        hit = false;
        if (buffers->m_count != 0) {
            for (uint32_t i = 0; i < buffers->m_count; i++) {
                ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
                if (!buffer->m_invalid && buffer->m_archive == archive && buffer->m_offset == offset &&
                    (piece->m_size + piece->m_bufferOffset <= buffer->m_valid || buffer->m_inFlight)) {
                    hit = !buffer->m_inFlight;
                    best = buffer;
                    break;
                }
                if (!buffer->m_inFlight) {
                    uint64_t age = buffers->m_clock - buffer->m_lastUse;
                    if (bestAge < age || !best) {
                        best = buffer;
                        bestAge = age;
                    }
                }
            }
            chosen = best;
            if (chosen) {
                break;
            }
        }
        ReleaseSRWLockExclusive(cacheLock);
        blz::sync_wait(&this->m_buffers->m_available);
    }
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    buffers->m_clock++;
    chosen->m_lastUse = buffers->m_clock;
    if (hit) {
        std::memcpy(piece->m_dest, chosen->m_data + piece->m_bufferOffset, piece->m_size);
        piece->m_done = piece->m_size;
        ReleaseSRWLockExclusive(cacheLock);
        return true;
    }
    if (plan->m_key == 0) {
        plan->m_key = NewRequestKey(0);
    }
    AcquireSRWLockExclusive(&this->m_pendingLock);
    bool inserted;
    this->m_pending.assign(plan->m_key, plan, &inserted);
    ReleaseSRWLockExclusive(&this->m_pendingLock);
    piece->m_buffer = chosen;
    chosen->m_waiters.insert(plan->m_key);
    if (!chosen->m_inFlight) {
        chosen->m_archive = archive;
        chosen->m_offset = offset;
        chosen->m_valid = 0;
        chosen->m_cached = 1;
        chosen->m_inFlight = 1;
        blz::sync_event_reset(&chosen->m_ready);
        ReleaseSRWLockExclusive(&this->m_buffers->m_lock);
        uint32_t read = 0;
        uint32_t error = OsFileReadAt(file, chosen->m_data, this->m_buffers->m_size, chosen->m_offset, &read);
        if (error != 0) {
            LogArchiveReadError(archive, error);
            this->CompleteBuffer(chosen, 0, 7);
            AcquireSRWLockExclusive(&this->m_pendingLock);
            this->RemovePending(plan->m_key);
            ReleaseSRWLockExclusive(&this->m_pendingLock);
            AcquireSRWLockExclusive(&this->m_buffers->m_lock);
            ReleaseSRWLockExclusive(cacheLock);
            return false;
        }
        this->CompleteBuffer(chosen, read, 0);
        AcquireSRWLockExclusive(&this->m_buffers->m_lock);
    } else {
        ReleaseSRWLockExclusive(&this->m_buffers->m_lock);
        if (piece->m_buffer == chosen) {
            blz::sync_wait(&chosen->m_ready);
        }
        if (piece->m_error) {
            AcquireSRWLockExclusive(&this->m_pendingLock);
            this->RemovePending(plan->m_key);
            ReleaseSRWLockExclusive(&this->m_pendingLock);
            AcquireSRWLockExclusive(&this->m_buffers->m_lock);
            ReleaseSRWLockExclusive(cacheLock);
            return false;
        }
        AcquireSRWLockExclusive(&this->m_buffers->m_lock);
    }
    AcquireSRWLockExclusive(&this->m_pendingLock);
    this->RemovePending(plan->m_key);
    ReleaseSRWLockExclusive(&this->m_pendingLock);
    ReleaseSRWLockExclusive(cacheLock);
    return true;
}

// Hands a filled buffer to every plan waiting on it, then runs the callbacks of the plans that
// are now complete.
// ref: FUN_142145bf0
void ARCHIVEFILEACCESSOR::CompleteBuffer(ARCHIVEBUFFER* buffer, uint32_t read, uint8_t error) {
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    blz::allocator* allocator = this->m_allocator;
    KEYLIST ready;
    ready.m_data = ready.m_inline;
    ready.m_count = 0;
    ready.m_capacity = 0x80000008;
    AcquireSRWLockExclusive(&buffers->m_lock);
    AcquireSRWLockShared(&this->m_pendingLock);
    for (uint32_t i = 0; i < buffer->m_waiters.m_count; i++) {
        uint64_t key = buffer->m_waiters.m_entries[i];
        auto entry = this->m_pending.find(key);
        if (!entry) {
            continue;
        }
        READPLAN* plan = entry->m_value;
        if (plan && plan->m_callback && (!plan->m_head.m_buffer || plan->m_head.m_buffer == buffer) &&
            (!plan->m_tail.m_buffer || plan->m_tail.m_buffer == buffer) && plan->m_middleDone) {
            KeyListPush(&ready, allocator, plan->m_key);
        }
    }
    this->DeliverBuffer(buffer, read, error);
    ReleaseSRWLockShared(&this->m_pendingLock);
    ReleaseSRWLockExclusive(&buffers->m_lock);
    blz::sync_semaphore_release(&this->m_buffers->m_available);
    for (uint32_t i = 0; i < ready.m_count; i++) {
        AcquireSRWLockShared(&this->m_pendingLock);
        auto entry = this->m_pending.find(ready.m_data[i]);
        if (entry && entry->m_value) {
            READPLAN* plan = entry->m_value;
            READCALLBACK callback = plan->m_callback;
            void* context = plan->m_callbackContext;
            ReleaseSRWLockShared(&this->m_pendingLock);
            if (callback) {
                callback(context, this->m_owner, ready.m_data[i]);
            }
        } else {
            ReleaseSRWLockShared(&this->m_pendingLock);
        }
    }
    if (ready.m_data != ready.m_inline) {
        allocator->deallocate(ready.m_data);
    }
}

// ref: FUN_142144a10
void ARCHIVEFILEACCESSOR::DeliverBuffer(ARCHIVEBUFFER* buffer, uint32_t read, uint8_t error) {
    for (uint32_t i = 0; i < buffer->m_waiters.m_count; i++) {
        auto entry = this->m_pending.find(buffer->m_waiters.m_entries[i]);
        if (!entry) {
            continue;
        }
        READPLAN* plan = entry->m_value;
        if (plan->m_head.m_buffer == buffer) {
            CompletePiece(this->m_buffers, &plan->m_head, read, error);
        } else if (plan->m_tail.m_buffer == buffer) {
            CompletePiece(this->m_buffers, &plan->m_tail, read, error);
        }
    }
    buffer->m_valid = read;
    if (buffer->m_waiters.m_buckets) {
        for (uint32_t i = 0; i < buffer->m_waiters.m_bucketCount; i++) {
            buffer->m_waiters.m_buckets[i] = 0xFFFFFFFF;
        }
    }
    buffer->m_waiters.m_count = 0;
    buffer->m_inFlight = 0;
    buffer->m_aioKey = 0;
    if (buffer->m_invalid || error) {
        buffer->m_cached = 0;
        buffer->m_invalid = 0;
        buffer->m_archive = 0;
        buffer->m_offset = 0;
        buffer->m_valid = 0;
    }
    blz::sync_event_set(&buffer->m_ready);
}

// After a write: forgets the buffered blocks it overlaps (or marks them, if being filled).
// ref: FUN_1421459f0
void ARCHIVEFILEACCESSOR::Invalidate(ARCHIVEFILE** file, uint64_t offset, uint32_t size) {
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    ARCHIVEFILE* held = *file;
    if (buffers) {
        uint64_t archive = held->m_key.m_archive;
        AcquireSRWLockExclusive(&buffers->m_lock);
        for (uint32_t i = 0; i < buffers->m_count; i++) {
            ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
            if (buffer->m_cached && buffer->m_archive == archive && buffer->m_offset < offset + size &&
                offset < uint64_t(buffers->m_size) + buffer->m_offset) {
                if (!buffer->m_inFlight) {
                    buffer->m_archive = 0;
                    buffer->m_offset = 0;
                    buffer->m_valid = 0;
                    buffer->m_cached = 0;
                } else {
                    buffer->m_invalid = 1;
                }
            }
        }
        ReleaseSRWLockExclusive(&buffers->m_lock);
    }
    ReleaseFile(held);
}

// --- asynchronous reads --------------------------------------------------------------------

// ref: FUN_1421571b0
int32_t ErrorFromAio(uint8_t result) {
    switch (result) {
    case 0:
        return 0;
    case 1:
        return 10;
    case 2:
        return 5;
    case 3:
        return 0xf;
    case 4:
    case 6:
        return 8;
    case 5:
        return 0x14;
    default:
        return 1;
    }
}

// A pooled copy of a plan, taking over its file reference.
// ref: FUN_1421435d0
READPLAN* PlanCopy(blz::object_pool* pool, READPLAN* plan) {
    SRWLOCK* lock = reinterpret_cast<SRWLOCK*>(reinterpret_cast<uint8_t*>(pool) + 0x98);
    AcquireSRWLockExclusive(lock);
    auto copy = static_cast<READPLAN*>(pool->Allocate());
    ReleaseSRWLockExclusive(lock);
    if (!copy) {
        return nullptr;
    }
    *copy = *plan;
    plan->m_file = nullptr;
    return copy;
}

// ref: FUN_142144c30
void PlanFree(blz::object_pool* pool, READPLAN* plan) {
    ReleaseFile(plan->m_file);
    SRWLOCK* lock = reinterpret_cast<SRWLOCK*>(reinterpret_cast<uint8_t*>(pool) + 0x98);
    AcquireSRWLockExclusive(lock);
    pool->Free(plan);
    ReleaseSRWLockExclusive(lock);
}

// Another process writes to the archives: buffered blocks could go stale, so the buffers stop
// serving reads.
// ref: FUN_142144ca0
void ARCHIVEFILEACCESSOR::ShutdownBuffers() {
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    if (!buffers || buffers->m_shutdown) {
        return;
    }
    AcquireSRWLockExclusive(&buffers->m_lock);
    buffers->m_shutdown = 1;
    for (uint32_t i = 0; i < buffers->m_count; i++) {
        ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
        if (buffer->m_cached) {
            if (!buffer->m_inFlight) {
                buffer->m_archive = 0;
                buffer->m_offset = 0;
                buffer->m_valid = 0;
                buffer->m_cached = 0;
            } else {
                buffer->m_invalid = 1;
            }
        }
    }
    ReleaseSRWLockExclusive(&buffers->m_lock);
}

// ref: FUN_142145970
void ARCHIVEFILEACCESSOR::InvalidateBuffers() {
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    if (!buffers) {
        return;
    }
    AcquireSRWLockExclusive(&buffers->m_lock);
    for (uint32_t i = 0; i < buffers->m_count; i++) {
        ARCHIVEBUFFER* buffer = &buffers->m_buffers[i];
        if (buffer->m_cached) {
            if (!buffer->m_inFlight) {
                buffer->m_archive = 0;
                buffer->m_offset = 0;
                buffer->m_valid = 0;
                buffer->m_cached = 0;
            } else {
                buffer->m_invalid = 1;
            }
        }
    }
    ReleaseSRWLockExclusive(&buffers->m_lock);
}

// ref: FUN_142144d80
void ARCHIVEFILEACCESSOR::EnableBuffers() {
    if (this->m_buffers) {
        this->m_buffers->m_shutdown = 0;
    }
}

namespace {

// The aio completion of a plan's middle: once the buffered pieces are in too, the plan's
// callback runs.
// ref: FUN_142144090
void MiddleDone(void* context, aio::CONTEXT* aio, uint64_t key) {
    (void)aio;
    (void)key;
    auto plan = static_cast<READPLAN*>(context);
    ARCHIVEFILEACCESSOR* accessor = plan->m_owner;
    AcquireSRWLockExclusive(&accessor->m_pendingLock);
    plan->m_middleDone = 1;
    if (!plan->m_head.m_buffer && !plan->m_tail.m_buffer) {
        READCALLBACK callback = plan->m_callback;
        void* callbackContext = plan->m_callbackContext;
        uint64_t planKey = plan->m_key;
        ReleaseSRWLockExclusive(&accessor->m_pendingLock);
        if (callback) {
            callback(callbackContext, accessor->m_owner, planKey);
        }
        return;
    }
    ReleaseSRWLockExclusive(&accessor->m_pendingLock);
}

// The aio completion of a buffer fill.
// ref: FUN_142143bc0
void BufferDone(void* context, aio::CONTEXT* aio, uint64_t key) {
    (void)aio;
    auto accessor = static_cast<ARCHIVEFILEACCESSOR*>(context);
    uint32_t read = 0;
    uint8_t result = aio::FileResult(accessor->m_aio, key, &read);
    ARCHIVEBUFFERS* buffers = accessor->m_buffers;
    uint8_t error = result == aio::RESULT_END_OF_FILE ? 0 : result;
    AcquireSRWLockExclusive(&buffers->m_lock);
    ARCHIVEBUFFER* found = nullptr;
    for (uint32_t i = 0; i < buffers->m_count; i++) {
        if (buffers->m_buffers[i].m_aioKey == key) {
            found = &buffers->m_buffers[i];
            break;
        }
    }
    ReleaseSRWLockExclusive(&buffers->m_lock);
    accessor->CompleteBuffer(found, read, error);
}

}

// Ties a piece to its buffer: the plan joins the buffer's waiters (stored in the pending map
// first if it is not yet), and an idle buffer is claimed for the block. True when the caller
// must start the fill.
// ref: FUN_142145ae0
bool ARCHIVEFILEACCESSOR::ClaimFill(READPLAN* plan, READPIECE* piece, READPLAN** stored, uint64_t* requestKey) {
    READPLAN* current = *stored ? *stored : plan;
    uint64_t archive = current->m_file->m_key.m_archive;
    bool head = piece == &plan->m_head;
    uint64_t offset = head ? current->m_head.m_offset : current->m_tail.m_offset;
    ARCHIVEBUFFER* buffer = head ? current->m_head.m_buffer : current->m_tail.m_buffer;
    if (!buffer) {
        return false;
    }
    if (current->m_key == 0) {
        plan->m_key = *requestKey;
        *stored = PlanCopy(&this->m_planPool, plan);
        AcquireSRWLockExclusive(&this->m_pendingLock);
        bool inserted;
        this->m_pending.assign((*stored)->m_key, *stored, &inserted);
        ReleaseSRWLockExclusive(&this->m_pendingLock);
        current = *stored;
    }
    buffer->m_waiters.insert(current->m_key);
    if (buffer->m_inFlight) {
        return false;
    }
    buffer->m_archive = archive;
    buffer->m_offset = offset;
    buffer->m_valid = 0;
    buffer->m_cached = 1;
    buffer->m_inFlight = 1;
    blz::sync_event_reset(&buffer->m_ready);
    return true;
}

// Serves what it can of a plan's head and tail from the buffers and claims buffers for the
// rest; false when no buffer is free.
// ref: FUN_142146490
bool ARCHIVEFILEACCESSOR::ClaimBuffers(READPLAN* plan, bool* headNeedsRead, bool* tailNeedsRead, bool* fallback,
    READPLAN** stored, uint64_t* requestKey) {
    *headNeedsRead = false;
    *tailNeedsRead = false;
    *fallback = false;
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    AcquireSRWLockExclusive(&buffers->m_lock);
    bool result;
    if (this->m_buffers->m_shutdown) {
        *fallback = true;
        result = false;
    } else {
        bool hit;
        result = true;
        if (plan->m_head.m_size != 0) {
            ARCHIVEBUFFER* buffer = FindBuffer(this->m_buffers, plan->m_file->m_key.m_archive, plan->m_head.m_offset,
                plan->m_head.m_size + plan->m_head.m_bufferOffset, &hit, nullptr);
            plan->m_head.m_buffer = buffer;
            if (!buffer) {
                result = false;
            } else if (hit) {
                std::memcpy(plan->m_head.m_dest, buffer->m_data + plan->m_head.m_bufferOffset, plan->m_head.m_size);
                plan->m_head.m_done = plan->m_head.m_size;
                plan->m_head.m_buffer = nullptr;
            }
        }
        if (result && plan->m_tail.m_size != 0) {
            ARCHIVEBUFFER* buffer = FindBuffer(this->m_buffers, plan->m_file->m_key.m_archive, plan->m_tail.m_offset,
                plan->m_tail.m_bufferOffset + plan->m_tail.m_size, &hit, plan->m_head.m_buffer);
            plan->m_tail.m_buffer = buffer;
            if (!buffer) {
                result = false;
            } else if (hit) {
                std::memcpy(plan->m_tail.m_dest, buffer->m_data + plan->m_tail.m_bufferOffset, plan->m_tail.m_size);
                plan->m_tail.m_done = plan->m_tail.m_size;
                plan->m_tail.m_buffer = nullptr;
            }
        }
        if (result) {
            if (plan->m_head.m_size != 0) {
                *headNeedsRead = this->ClaimFill(plan, &plan->m_head, stored, requestKey);
            }
            if (plan->m_tail.m_size != 0) {
                *tailNeedsRead = this->ClaimFill(plan, &plan->m_tail, stored, requestKey);
            }
        }
    }
    ReleaseSRWLockExclusive(&buffers->m_lock);
    return result;
}

// An asynchronous read through the buffers: memory serves what it can, the buffers claimed for
// the head and tail are filled by aio, and the middle is read by aio straight into place.
// ref: FUN_142144380
int32_t ARCHIVEFILEACCESSOR::ReadBuffered(const uint8_t ekey[16], const void* span, ARCHIVEFILE* file, void* dest,
    uint64_t offset, uint32_t size, READCALLBACK callback, void* context, bool nonBlocking, bool* fallback,
    uint64_t* requestKey, uint64_t* outKey) {
    READPLAN plan;
    std::memset(&plan, 0, sizeof(plan));
    plan.m_middleDone = 1;
    plan.m_spanOffset = ~uint64_t(0);
    *outKey = 0;
    READPLAN* stored = nullptr;
    BuildReadPlan(this->m_buffers, &plan, file, dest, offset, size, callback, context);
    std::memcpy(plan.m_ekey, ekey, 16);
    std::memcpy(&plan.m_spanOffset, span, 16);
    int32_t result = 0;
    bool headNeedsRead;
    bool tailNeedsRead;
    bool claimed = this->ClaimBuffers(&plan, &headNeedsRead, &tailNeedsRead, fallback, &stored, requestKey);
    while (!claimed) {
        if (*fallback) {
            ReleaseFile(plan.m_file);
            return result;
        }
        if (nonBlocking) {
            ReleaseFile(plan.m_file);
            return 0x15;
        }
        blz::sync_wait(&this->m_buffers->m_available);
        claimed = this->ClaimBuffers(&plan, &headNeedsRead, &tailNeedsRead, fallback, &stored, requestKey);
    }
    if (!stored) {
        if (plan.m_middleSize == 0) {
            ReleaseFile(plan.m_file);
            return result;
        }
        plan.m_key = *requestKey;
        stored = PlanCopy(&this->m_planPool, &plan);
        AcquireSRWLockExclusive(&this->m_pendingLock);
        bool inserted;
        this->m_pending.assign(stored->m_key, stored, &inserted);
        ReleaseSRWLockExclusive(&this->m_pendingLock);
    } else if (!headNeedsRead && !tailNeedsRead && plan.m_middleSize == 0) {
        *outKey = plan.m_key;
        ReleaseFile(plan.m_file);
        return 0;
    }
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    uint64_t key = stored->m_key;
    AcquireSRWLockExclusive(&buffers->m_lock);
    AcquireSRWLockExclusive(&this->m_pendingLock);
    bool headFailed = false;
    if (headNeedsRead) {
        ARCHIVEBUFFER* buffer = stored->m_head.m_buffer;
        buffer->m_aioKey = aio::FileRead(this->m_aio, &file->m_asyncFile, buffer->m_data, buffer->m_offset,
            this->m_buffers->m_size, &BufferDone, this);
        headFailed = buffer->m_aioKey == 0;
    }
    bool middleFailed = false;
    if (stored->m_middleSize != 0) {
        stored->m_owner = this;
        aio::CALLBACK_ done = callback ? &MiddleDone : nullptr;
        stored->m_middleAio = aio::FileRead(this->m_aio, &file->m_asyncFile, stored->m_middleDest, stored->m_middleOffset,
            stored->m_middleSize, done, stored);
        middleFailed = stored->m_middleAio == 0;
    }
    bool tailFailed = false;
    if (tailNeedsRead) {
        ARCHIVEBUFFER* buffer = stored->m_tail.m_buffer;
        buffer->m_aioKey = aio::FileRead(this->m_aio, &file->m_asyncFile, buffer->m_data, buffer->m_offset,
            this->m_buffers->m_size, &BufferDone, this);
        tailFailed = buffer->m_aioKey == 0;
    }
    ReleaseSRWLockExclusive(&this->m_pendingLock);
    ReleaseSRWLockExclusive(&buffers->m_lock);
    if (headFailed) {
        this->CompleteBuffer(stored->m_head.m_buffer, 0, 7);
    }
    if (tailFailed) {
        this->CompleteBuffer(stored->m_tail.m_buffer, 0, 7);
    }
    if (middleFailed) {
        AcquireSRWLockExclusive(&this->m_pendingLock);
        this->RemovePending(key);
        ReleaseSRWLockExclusive(&this->m_pendingLock);
        PlanFree(&this->m_planPool, stored);
        result = 8;
    } else {
        *outKey = key;
    }
    ReleaseFile(plan.m_file);
    return result;
}

// Starts an asynchronous read; *outKey is what GetResult waits on. Takes over the file
// reference.
// ref: FUN_142146660
int32_t ARCHIVEFILEACCESSOR::ReadAsync(const uint8_t ekey[16], const void* span, ARCHIVEFILE** file, uint64_t offset,
    uint32_t size, void* dest, uint64_t* requestKey, uint64_t* outKey, READCALLBACK callback, void* context,
    uint32_t flags) {
    ARCHIVEBUFFERS* buffers = this->m_buffers;
    if (buffers && !buffers->m_shutdown && (flags & 2) == 0) {
        uint32_t blockSize = buffers->m_size;
        if ((~(uint64_t(blockSize) - 1) & offset) != offset || ((0u - blockSize) & size) != size) {
            bool fallback = false;
            int32_t result = this->ReadBuffered(ekey, span, *file, dest, offset, size, callback, context,
                (flags & 1) != 0, &fallback, requestKey, outKey);
            if (!fallback) {
                ReleaseFile(*file);
                return result;
            }
        }
    }
    AcquireSRWLockExclusive(&this->m_pendingLock);
    AcquireSRWLockExclusive(&this->m_planPoolLock);
    auto plan = static_cast<READPLAN*>(this->m_planPool.Allocate());
    ReleaseSRWLockExclusive(&this->m_planPoolLock);
    if (plan) {
        std::memset(plan, 0, sizeof(READPLAN));
        plan->m_middleDone = 1;
        plan->m_59 = 0;
        plan->m_spanOffset = ~uint64_t(0);
        plan->m_spanSize = 0;
    }
    plan->m_owner = this;
    aio::CALLBACK_ done = callback ? &MiddleDone : nullptr;
    uint64_t aioKey = aio::FileRead(this->m_aio, &(*file)->m_asyncFile, dest, offset, size, done, plan);
    if (aioKey == 0) {
        ReleaseFile(plan->m_file);
        AcquireSRWLockExclusive(&this->m_planPoolLock);
        this->m_planPool.Free(plan);
        ReleaseSRWLockExclusive(&this->m_planPoolLock);
        ReleaseSRWLockExclusive(&this->m_pendingLock);
        ReleaseFile(*file);
        return 8;
    }
    plan->m_key = *requestKey;
    ReleaseFile(plan->m_file);
    plan->m_file = *file;
    *file = nullptr;
    plan->m_middleAio = aioKey;
    plan->m_middleDone = 0;
    plan->m_middleOffset = offset;
    plan->m_middleSize = size;
    plan->m_middleDest = static_cast<uint8_t*>(dest);
    plan->m_callback = callback;
    plan->m_callbackContext = context;
    std::memcpy(plan->m_ekey, ekey, 16);
    std::memcpy(&plan->m_spanOffset, span, 16);
    bool inserted;
    this->m_pending.assign(plan->m_key, plan, &inserted);
    *outKey = *requestKey;
    ReleaseSRWLockExclusive(&this->m_pendingLock);
    ReleaseFile(*file);
    return 0;
}

// Waits for a plan's pieces and middle and totals what was read; a short read reports the
// truncation to the owner.
// ref: FUN_142144130
int32_t ARCHIVEFILEACCESSOR::WaitPlan(uint64_t key, uint32_t* read, bool* found) {
    int32_t result = 0;
    *read = 0;
    AcquireSRWLockShared(&this->m_pendingLock);
    auto entry = this->m_pending.find(key);
    if (!entry || !entry->m_value) {
        if (found) {
            *found = false;
        }
        ReleaseSRWLockShared(&this->m_pendingLock);
        return 10;
    }
    READPLAN* plan = entry->m_value;
    if (plan->m_key >> 63) {
        ReleaseSRWLockShared(&this->m_pendingLock);
        return 0;
    }
    uint64_t aioKey = plan->m_middleAio;
    if (aioKey != 0 && !this->m_aio) {
        ReleaseSRWLockShared(&this->m_pendingLock);
        return 10;
    }
    ARCHIVEBUFFER* headBuffer = plan->m_head.m_buffer;
    ARCHIVEBUFFER* tailBuffer = plan->m_tail.m_buffer;
    ReleaseSRWLockShared(&this->m_pendingLock);
    if (headBuffer) {
        blz::sync_wait(&headBuffer->m_ready);
    }
    if (tailBuffer) {
        blz::sync_wait(&tailBuffer->m_ready);
    }
    uint32_t middleRead = 0;
    if (aioKey != 0) {
        uint8_t status = aio::FileResult(this->m_aio, aioKey, &middleRead);
        if (status != aio::RESULT_SUCCESS && status != aio::RESULT_END_OF_FILE) {
            result = ErrorFromAio(status);
            if (result == 0xf) {
                return result;
            }
        }
    }
    AcquireSRWLockShared(&this->m_pendingLock);
    entry = this->m_pending.find(key);
    if (!entry || !entry->m_value) {
        ReleaseSRWLockShared(&this->m_pendingLock);
        return 10;
    }
    plan = entry->m_value;
    uint32_t headSize = plan->m_head.m_size;
    if (headSize != 0 && plan->m_head.m_error) {
        result = 8;
    }
    uint32_t total;
    if (plan->m_tail.m_size != 0 && plan->m_head.m_done == headSize && middleRead == plan->m_middleSize &&
        plan->m_tail.m_error) {
        total = plan->m_head.m_done;
        result = 8;
        if (plan->m_middleSize <= middleRead) {
            total += plan->m_tail.m_done;
        }
        total += middleRead;
    } else {
        total = plan->m_head.m_done;
        if (headSize <= total) {
            if (plan->m_middleSize <= middleRead) {
                total += plan->m_tail.m_done;
            }
            total += middleRead;
        }
    }
    *read = total;
    if (this->m_truncated && (plan->m_spanOffset != ~uint64_t(0) || plan->m_spanSize != 0) && total < plan->m_spanSize) {
        this->m_truncated(this->m_truncatedContext, plan->m_ekey, &plan->m_spanOffset, total);
    }
    ReleaseSRWLockShared(&this->m_pendingLock);
    return result;
}

// The result of an asynchronous read, which is then forgotten. Keys tagged in the top bit are
// writes (pooled 0x40-byte requests with their result kept in them).
// ref: FUN_142143e10
int32_t ARCHIVEFILEACCESSOR::GetResult(uint64_t key, uint32_t* read, bool* found) {
    if (found) {
        *found = true;
    }
    int32_t result;
    if ((key >> 63) == 1) {
        result = 0;
        *read = 0;
        AcquireSRWLockShared(&this->m_pendingLock);
        auto entry = this->m_pending.find(key);
        if (!entry || !entry->m_value) {
            if (found) {
                *found = false;
            }
            ReleaseSRWLockShared(&this->m_pendingLock);
            return 10;
        }
        READPLAN* request = entry->m_value;
        if (!(request->m_key >> 63)) {
            uint64_t aioKey = request->m_middleAio;
            if (aioKey != 0) {
                if (!this->m_aio) {
                    ReleaseSRWLockShared(&this->m_pendingLock);
                    return 10;
                }
                ReleaseSRWLockShared(&this->m_pendingLock);
                uint8_t status = aio::FileResult(this->m_aio, aioKey, read);
                if (status != aio::RESULT_SUCCESS && status != aio::RESULT_END_OF_FILE) {
                    result = ErrorFromAio(status);
                }
            } else {
                ReleaseSRWLockShared(&this->m_pendingLock);
            }
            if (result == 10) {
                return 10;
            }
        } else {
            ReleaseSRWLockShared(&this->m_pendingLock);
        }
    } else {
        result = this->WaitPlan(key, read, found);
        if (result == 10) {
            return 10;
        }
    }
    AcquireSRWLockExclusive(&this->m_pendingLock);
    auto entry = this->m_pending.find(key);
    if (entry) {
        READPLAN* value = entry->m_value;
        if (!(value->m_key >> 63)) {
            if ((key >> 63) == 0) {
                PlanFree(&this->m_planPool, value);
            } else {
                PlanFree(&this->m_requestPool, value);
            }
        } else {
            // A completed write keeps its result at +0x30 and the bytes at +0x34.
            auto request = reinterpret_cast<uint8_t*>(value);
            result = *reinterpret_cast<int32_t*>(request + 0x30);
            *read = *reinterpret_cast<uint32_t*>(request + 0x34);
            ReleaseFile(value->m_file);
            this->m_allocator->deallocate(value);
        }
        this->RemovePending(key);
        ReleaseSRWLockExclusive(&this->m_pendingLock);
        return result;
    }
    ReleaseSRWLockExclusive(&this->m_pendingLock);
    return 10;
}

}
