#include "tact/DownloadQueue.hpp"
#include "tact/Common.hpp"
#include "tact/Instance.hpp"
#include "blz/Log.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <windows.h>

namespace tact {

namespace {

const char s_queueFile[] = "D:\\BuildServer\\A\\work\\package_cache\\tact\\3.6.39\\src\\download\\download_queue.cpp";
const char s_noTag[] = "";

// The list of operation ids a cancel task carries (0x18 bytes).
struct CANCELLIST {
    Instance* m_instance;
    uint64_t* m_ids;
    uint32_t m_count;
};

// ref: FUN_1420ecf00
void CancelTask(void* argument) {
    auto list = static_cast<CANCELLIST*>(argument);
    for (uint32_t i = 0; i < list->m_count; i++) {
        OPHANDLE handle;
        list->m_instance->m_ops.Find(&handle, list->m_ids[i]);
        if (!handle.m_state) {
            if (handle.m_lock) {
                *handle.m_lock = 0;
            }
        } else {
            handle.m_state->Cancel();
            if (handle.m_lock) {
                *handle.m_lock = 0;
            }
            handle.m_state->Release();
        }
    }
    GetBufferAllocator()->deallocate(list->m_ids);
    GetBufferAllocator()->deallocate(list);
}

// The index set's fetch task; streaming only.
// ref: 0x1420dfdb0
void IndexSetFetchTask(void* argument) {
    (void)argument;
    FROST_UNIMPLEMENTED();
}

}

// Prepares the queue for an initialization: streaming is enabled only with a read/write
// container, connections, a CDN host and a CDN configuration key, and an armadillo key that
// loads; the callback then gets the result.
// ref: FUN_1420e6730
void DownloadQueueStart(uint8_t* queue, INSTANCEPARAMS* params, RESULTCALLBACK callback, void* context) {
    EnterCriticalSection(&FieldAt<CRITICAL_SECTION>(queue, 0x540));
    uint8_t error = ERR_SUCCESS;
    FieldAt<uint8_t>(queue, 0x574) = 0;
    blz::allocator* allocator = GetAllocator();
    auto downloader = FieldAt<TaskSchedulerBase*>(queue, 0x08);
    if (downloader) {
        downloader->~TaskSchedulerBase();
        allocator->deallocate(downloader);
    }
    FieldAt<RESULTCALLBACK>(queue, 0x578) = callback;
    FieldAt<void*>(queue, 0x08) = nullptr;
    FieldAt<uint8_t>(queue, 0x576) = 0;
    FieldAt<void*>(queue, 0x580) = context;

    static const uint8_t s_zeroKey[16] = {};
    bool disabled = params->m_streaming == 1 || params->m_74 == 0 || params->m_mode == 2 ||
        params->m_cdnHostCount == 0 || std::memcmp(params->m_cdnKey, s_zeroKey, 16) == 0;
    // DIVERGED: FrostLite has no network layer (docs/stubs.md). Where the reference would load
    // the armadillo key and create the CDN downloader (0x14210deb0 / 0x142110c40) and start its
    // state (0x142104c80), the queue stays without streaming, exactly as when the parameters
    // disable it.
    (void)disabled;
    blz::log(g_logCategory, 2, s_queueFile, 0x70, "Streaming is disabled due to instance parameters.");
    LeaveCriticalSection(&FieldAt<CRITICAL_SECTION>(queue, 0x540));
    FieldAt<RESULTCALLBACK>(queue, 0x578)(FieldAt<void*>(queue, 0x580), error);
}

bool DownloadQueueStreaming(Instance* instance) {
    return FieldAt<uint8_t>(instance->m_downloadQueue, 0x574) != 0;
}

// ref: FUN_1420df8b0
void IndexSetReset(uint8_t* set) {
    EnterCriticalSection(&FieldAt<CRITICAL_SECTION>(set, 0x00));
    FieldAt<uint64_t>(set, 0x2a8) = 0;
    FieldAt<uint64_t>(set, 0x2b0) = 0;
    FieldAt<uint64_t>(set, 0x2b8) = 0;
    FieldAt<uint64_t>(set, 0x2c0) = 0;
    LeaveCriticalSection(&FieldAt<CRITICAL_SECTION>(set, 0x00));
}

// ref: FUN_1420e1f60
void IndexSetFetch(uint8_t* set, Instance* instance, const uint8_t key[16], RESULTCALLBACK callback, void* context,
    uint32_t type) {
    EnterCriticalSection(&FieldAt<CRITICAL_SECTION>(set, 0x00));
    FieldAt<Instance*>(set, 0x28) = instance;
    FieldAt<void*>(set, 0x2b0) = context;
    std::memcpy(set + 0x50, key, 16);
    FieldAt<uint32_t>(set, 0x2a0) = type;
    FieldAt<RESULTCALLBACK>(set, 0x2a8) = callback;
    instance->m_scheduler->Post(&IndexSetFetchTask, set, 0);
    LeaveCriticalSection(&FieldAt<CRITICAL_SECTION>(set, 0x00));
}

// Sums the sizes of the CDN archives not yet in the container; streaming only.
// ref: FUN_1420dfe20
void IndexSetMeasure(uint8_t* set, SIZECALLBACK callback, void* context) {
    (void)set;
    (void)callback;
    (void)context;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1420df530
void IndexSetCancel(uint8_t* set) {
    EnterCriticalSection(&FieldAt<CRITICAL_SECTION>(set, 0x00));
    uint64_t opId = FieldAt<uint64_t>(set, 0x308);
    FieldAt<uint8_t>(set, 0x299) = 1;
    LeaveCriticalSection(&FieldAt<CRITICAL_SECTION>(set, 0x00));
    if (opId != 0) {
        CancelOp(FieldAt<Instance*>(set, 0x28), opId);
    }
}

// ref: FUN_1420ed100
void CancelOp(Instance* instance, uint64_t opId) {
    auto list = static_cast<CANCELLIST*>(PackageAlloc(GetBufferAllocator(), sizeof(CANCELLIST), 8, false, s_noTag, 0));
    if (list) {
        list->m_instance = instance;
        list->m_count = 1;
        list->m_ids = static_cast<uint64_t*>(
            PackageAlloc(GetBufferAllocator(), uint64_t(list->m_count) * 8, 8, false, s_noTag, 0));
        list->m_ids[0] = opId;
    }
    instance->m_scheduler->Post(&CancelTask, list, 1);
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1420ecf00 = (void*)&tact::CancelTask;
// ---- end of generated globals ----
