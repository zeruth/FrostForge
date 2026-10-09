#include "tact/ReadQueue.hpp"
#include "tact/Codec.hpp"
#include "tact/Common.hpp"
#include "tact/Instance.hpp"
#include "tact/TaskScheduler.hpp"
#include "tact/U32Set.hpp"
#include <cstring>

namespace tact {

namespace {

const char s_queueFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\read_queue.cpp";

// The read queue's per-thread batch depth (byte 0x2291 of the reference's TLS block): while
// a thread batches, submissions queue without taking the lock or dispatching.
thread_local uint8_t s_batchDepth;

void LinkInit(QUEUELINK* link) {
    link->m_next = link;
    link->m_prev = link;
}

void LinkUnlink(QUEUELINK* link) {
    link->m_prev->m_next = link->m_next;
    link->m_next->m_prev = link->m_prev;
    link->m_prev = link;
    link->m_next = link;
}

// At the tail of a list.
void LinkAppend(QUEUELINK* head, QUEUELINK* link) {
    link->m_prev->m_next = link->m_next;
    link->m_next->m_prev = link->m_prev;
    link->m_prev = head->m_prev;
    link->m_next = head;
    head->m_prev = link;
    link->m_prev->m_next = link;
}

READREQUEST* RequestOf(QUEUELINK* link) {
    return reinterpret_cast<READREQUEST*>(reinterpret_cast<uint8_t*>(link) - offsetof(READREQUEST, m_link));
}

void FreeBuffer(READREQUEST* request) {
    if (request->m_buffer && request->m_buffer != request->m_dest) {
        request->m_queue->m_instance->m_scratch.Free(request->m_buffer);
        request->m_buffer = nullptr;
    }
}

// The decode task.
// ref: FUN_1420dc8b0
void DecodeTask(void* argument) {
    auto request = static_cast<READREQUEST*>(argument);
    uint8_t result = ReadRequestDecode(request);
    READQUEUE* queue = request->m_queue;
    READDONE callback = request->m_callback;
    void* context = request->m_context;
    FreeBuffer(request);
    if (callback) {
        callback(context, result);
    }
    EnterCriticalSection(&queue->m_lock);
    LinkUnlink(&request->m_link);
    queue->m_requests.Free(request);
    LeaveCriticalSection(&queue->m_lock);
}

// The container finished reading a request's encoded span: unless it failed or nothing needs
// decoding, the decode goes to the scheduler and the next request is started.
// ref: FUN_1420dcfd0
void OnRead(READFUNCTOR* functor, uint8_t result, uint32_t read) {
    auto request = reinterpret_cast<READREQUEST*>(reinterpret_cast<uint8_t*>(functor) - offsetof(READREQUEST, m_done));
    READQUEUE* queue = request->m_queue;
    uint8_t error = queue->m_instance->m_container->FinishRead(result, request->m_ekey, read, request->m_eSize);
    if (error != 0) {
        queue->Fail(request, error);
        return;
    }
    if (request->m_buffer == request->m_dest && (request->m_flags & 2) != 0) {
        queue->Fail(request, 0);
        return;
    }
    queue->m_instance->m_scheduler->Post(&DecodeTask, request, 0);
    _InterlockedDecrement(reinterpret_cast<volatile long*>(&queue->m_inFlight));
    EnterCriticalSection(&queue->m_lock);
    queue->Pump();
    LeaveCriticalSection(&queue->m_lock);
}

}

// ref: FUN_1420dc6c0
void READQUEUE::Init(Instance* instance) {
    this->m_instance = instance;
    for (auto& list : this->m_waiting) {
        LinkInit(&list);
    }
    this->m_requests.Init(GetAllocator(), 0x88, 8, 0x20, 2);
    this->m_maxInFlight = 0x10;
    this->m_inFlight = 0;
    this->m_dispatching = 0;
    InitializeCriticalSection(&this->m_lock);
}

// ref: FUN_1420dc750
void READQUEUE::Destroy() {
    DeleteCriticalSection(&this->m_lock);
    this->m_requests.Destroy();
    for (int32_t i = 2; i >= 0; i--) {
        QUEUELINK* head = &this->m_waiting[i];
        QUEUELINK* node = head->m_next;
        while (node != head) {
            QUEUELINK* next = node->m_next;
            LinkInit(node);
            node = next;
        }
        LinkInit(head);
    }
}

// How many reads may be in flight at once.
// ref: FUN_1420dc9d0
void READQUEUE::SetDepth(uint32_t depth) {
    this->m_maxInFlight = depth;
}

// ref: FUN_1420dd140
void READQUEUE::BeginBatch() {
    s_batchDepth++;
    if (s_batchDepth == 1) {
        EnterCriticalSection(&this->m_lock);
    }
}

// ref: FUN_1420dc980
void READQUEUE::EndBatch() {
    s_batchDepth--;
    if (s_batchDepth == 0) {
        this->Pump();
        LeaveCriticalSection(&this->m_lock);
    }
}

// Starts the read at once when there is room and nothing of its priority or higher waits;
// otherwise it waits in its list.
// ref: FUN_1420dcda0
void READQUEUE::Submit(const uint8_t ekey[16], uint8_t keySize, const char* eSpec, uint32_t codec, ENCODINGHEADER* header,
    const uint64_t* eSpan, const uint64_t* cSpan, uint8_t* dest, uint8_t partial, uint8_t priority, uint32_t flags,
    READDONE callback, void* context, READREQUEST** out, U32SET* corrupt) {
    if (out) {
        *out = nullptr;
    }
    if (corrupt) {
        U32SET fresh;
        fresh.Init();
        corrupt->Destroy();
        corrupt->Assign(&fresh);
        fresh.Destroy();
    }
    bool batched = s_batchDepth != 0;
    if (!batched) {
        EnterCriticalSection(&this->m_lock);
    }
    auto request = static_cast<READREQUEST*>(this->m_requests.Allocate());
    if (request) {
        request->m_queue = this;
        request->m_eSpec = eSpec;
        request->m_keySize = keySize;
        request->m_22 = 1;
        std::memcpy(request->m_ekey, ekey, 16);
        request->m_partial = partial;
        request->m_codec = codec;
        request->m_header = header;
        request->m_priority = priority;
        std::memcpy(&request->m_eOffset, eSpan, 8);
        std::memcpy(&request->m_cOffset, cSpan, 8);
        request->m_dest = dest;
        request->m_callback = callback;
        request->m_context = context;
        LinkInit(&request->m_link);
        request->m_flags = flags;
        request->m_corrupt = corrupt;
        request->m_buffer = nullptr;
        request->m_done.m_function = nullptr;
    }
    if (out) {
        *out = request;
    }
    bool waiting = batched || this->m_maxInFlight <= this->m_inFlight;
    if (!waiting && priority < 3) {
        for (uint32_t i = priority; i < 3; i++) {
            if (this->m_waiting[i].m_next != &this->m_waiting[i]) {
                waiting = true;
                break;
            }
        }
    }
    if (waiting) {
        LinkAppend(&this->m_waiting[priority], &request->m_link);
    } else {
        this->Dispatch(request);
    }
    if (!batched) {
        LeaveCriticalSection(&this->m_lock);
    }
}

// A request still waiting is dropped; one already reading cannot be.
// ref: FUN_1420dd0a0
bool READQUEUE::Cancel(READREQUEST* request) {
    if (!request) {
        return false;
    }
    EnterCriticalSection(&this->m_lock);
    bool cancelled = false;
    if (request->m_link.m_next != &request->m_link || request->m_link.m_prev != &request->m_link) {
        LinkUnlink(&request->m_link);
        this->m_requests.Free(request);
        cancelled = true;
    }
    LeaveCriticalSection(&this->m_lock);
    return cancelled;
}

// Called with the lock held; drops it while the container is asked. Unencoded whole reads go
// straight into the destination, everything else into scratch memory.
// ref: FUN_1420dd180
void READQUEUE::Dispatch(READREQUEST* request) {
    LeaveCriticalSection(&this->m_lock);
    if (!request->m_buffer) {
        bool direct = (request->m_flags & 2) != 0 || ((request->m_flags & 1) == 0 && !request->m_partial);
        if (request->m_codec == 1 && direct) {
            request->m_buffer = request->m_dest;
        } else {
            request->m_buffer = static_cast<uint8_t*>(this->m_instance->m_scratch.Allocate(request->m_eSize));
        }
    }
    request->m_done.m_function = &OnRead;
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&this->m_inFlight));
    bool wouldBlock = false;
    uint8_t ekey[16];
    std::memcpy(ekey, request->m_ekey, 16);
    uint8_t error = this->m_instance->m_container->ReadAsync(ekey, request->m_buffer, request->m_eOffset,
        request->m_eSize, &request->m_done, &wouldBlock);
    if (!wouldBlock) {
        if (error != 0) {
            this->Fail(request, error);
        }
        EnterCriticalSection(&this->m_lock);
        return;
    }
    FreeBuffer(request);
    _InterlockedDecrement(reinterpret_cast<volatile long*>(&this->m_inFlight));
    EnterCriticalSection(&this->m_lock);
    LinkAppend(&this->m_waiting[request->m_priority], &request->m_link);
    this->Pump();
}

// Starts waiting requests, most urgent first, while there is room.
// ref: FUN_1420dccb0
void READQUEUE::Pump() {
    if (this->m_dispatching) {
        return;
    }
    for (;;) {
        if (this->m_maxInFlight <= this->m_inFlight) {
            return;
        }
        READREQUEST* request = nullptr;
        int32_t list = 2;
        for (; list >= 0; list--) {
            QUEUELINK* head = &this->m_waiting[list];
            QUEUELINK* first = head->m_next;
            if (first != head) {
                LinkUnlink(first);
                request = RequestOf(first);
                break;
            }
        }
        if (!request) {
            return;
        }
        this->m_dispatching = 1;
        this->Dispatch(request);
        this->m_dispatching = 0;
        QUEUELINK* head = &this->m_waiting[list];
        READREQUEST* front = head->m_next == head ? nullptr : RequestOf(head->m_next);
        if (front == request) {
            return;
        }
    }
}

// ref: FUN_1420dc7c0
void READQUEUE::Fail(READREQUEST* request, uint8_t error) {
    READDONE callback = request->m_callback;
    void* context = request->m_context;
    FreeBuffer(request);
    _InterlockedDecrement(reinterpret_cast<volatile long*>(&this->m_inFlight));
    EnterCriticalSection(&this->m_lock);
    this->Pump();
    LeaveCriticalSection(&this->m_lock);
    if (callback) {
        callback(context, error);
    }
    EnterCriticalSection(&this->m_lock);
    LinkUnlink(&request->m_link);
    this->m_requests.Free(request);
    LeaveCriticalSection(&this->m_lock);
}

// Checks or decodes the encoded span into the destination.
// ref: FUN_1420dc9e0
uint8_t ReadRequestDecode(READREQUEST* request) {
    Instance* instance = request->m_queue->m_instance;
    uint8_t keySize = instance->m_container->GetKeySize();
    if (request->m_keySize < keySize) {
        keySize = request->m_keySize;
    }
    uint8_t noHashCheck = static_cast<uint8_t>(request->m_flags >> 1 & 1);
    uint8_t error = 0;
    if (request->m_codec == 1) {
        if (!noHashCheck && !CodecVerifyHash(request->m_ekey, keySize, request->m_buffer, request->m_eSize)) {
            error = 8;
            char text[0x21];
            KeyToString(text, sizeof(text), request->m_ekey);
            blz::log(g_logCategory, 4, s_queueFile, 0x220,
                "Unencoded data failed hash validation during file read. E-Key: %s", text);
        }
        if (request->m_buffer != request->m_dest && error == 0) {
            std::memcpy(request->m_dest, request->m_buffer, request->m_cSize);
            return 0;
        }
    } else if (!request->m_partial) {
        error = CodecDecode(request->m_dest, request->m_cSize, request->m_buffer, request->m_eSize, request->m_ekey,
            request->m_eSpec, keySize, static_cast<uint8_t>(request->m_flags & 1),
            reinterpret_cast<KEYGETTER>(instance->m_f58), instance->m_f60, noHashCheck ^ 1, &instance->m_scratch,
            static_cast<DECODERHOOK*>(instance->m_memoryLimit), request->m_corrupt);
        if (error != 0) {
            char text[0x21];
            KeyToString(text, sizeof(text), request->m_ekey);
            blz::log(g_logCategory, 4, s_queueFile, 0x255, "Decode failed for file read. E-Key: %s", text);
        }
    } else {
        error = CodecDecodeSpan(request->m_dest, &request->m_cOffset, request->m_buffer, &request->m_eOffset,
            request->m_header, request->m_corrupt, static_cast<uint8_t>(request->m_flags & 1),
            reinterpret_cast<KEYGETTER>(instance->m_f58), instance->m_f60, noHashCheck ^ 1, &instance->m_scratch,
            static_cast<DECODERHOOK*>(instance->m_memoryLimit));
        if (error != 0) {
            char text[0x21];
            KeyToString(text, sizeof(text), request->m_ekey);
            blz::log(g_logCategory, 4, s_queueFile, 0x23d, "Decode failed for span read. Span: { %u, %u }, E-Key: %s",
                request->m_cOffset, request->m_cSize, text);
        }
    }
    const char* stat;
    if (error == 8) {
        stat = "read_file.decode.corrupt";
    } else if (error != 0) {
        stat = "read_file.decode.failure";
    } else {
        return 0;
    }
    StatsReport(&instance->m_cdnCallbacks, 0, stat, 1, 1.0f);
    return error;
}

}
