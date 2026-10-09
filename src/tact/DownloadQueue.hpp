#ifndef TACT_DOWNLOAD_QUEUE_HPP
#define TACT_DOWNLOAD_QUEUE_HPP

// pkg/tact/3.6.39/src/download: the download queue (instance+0x480) and its CDN index set
// (queue+0x1f0 = instance+0x670). FrostLite runs offline: streaming is never enabled, so only
// the paths a non-streaming instance takes are ported (see docs/stubs.md).

#include <cstdint>

namespace tact {

class Instance;
struct INSTANCEPARAMS;

typedef void (*RESULTCALLBACK)(void* context, uint8_t error);
typedef void (*SIZECALLBACK)(void* context, uint64_t done, uint64_t total);

// queue: instance->m_downloadQueue
void DownloadQueueStart(uint8_t* queue, INSTANCEPARAMS* params, RESULTCALLBACK callback, void* context);
bool DownloadQueueStreaming(Instance* instance);

// set: instance->m_downloadQueue + 0x1f0
void IndexSetReset(uint8_t* set);
void IndexSetFetch(uint8_t* set, Instance* instance, const uint8_t key[16], RESULTCALLBACK callback, void* context,
    uint32_t type);
void IndexSetMeasure(uint8_t* set, SIZECALLBACK callback, void* context);
void IndexSetCancel(uint8_t* set);

// Cancels an operation from a task on the instance's scheduler.
void CancelOp(Instance* instance, uint64_t opId);

}

#endif
