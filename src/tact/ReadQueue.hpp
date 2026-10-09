#ifndef TACT_READ_QUEUE_HPP
#define TACT_READ_QUEUE_HPP

// pkg/tact/3.6.39/src/read_queue.cpp: reads from the container, at most 16 in flight. A
// request reads its encoded span asynchronously into a scratch buffer (or straight into the
// destination when nothing needs decoding), then a task decodes it into the destination and
// calls back. Requests wait in three lists by priority, 2 the most urgent.

#include "blz/ObjectPool.hpp"
#include "tact/Container.hpp"
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace tact {

class Instance;
struct ENCODINGHEADER;
struct U32SET;
struct READQUEUE;

struct QUEUELINK {
    QUEUELINK* m_next;
    QUEUELINK* m_prev;
};

typedef void (*READDONE)(void* context, uint8_t result);

// One read (0x88 bytes).
struct READREQUEST {
    READQUEUE* m_queue;             // 0x00
    const char* m_eSpec;            // 0x08
    uint8_t m_ekey[16];             // 0x10
    uint8_t m_keySize;              // 0x20: how much of the e-key a hash check compares
    uint8_t m_partial;              // 0x21: a span of a block-encoded file
    uint8_t m_22;                   // 0x22
    uint8_t m_priority;             // 0x23
    uint32_t m_codec;               // 0x24: the e-spec type
    ENCODINGHEADER* m_header;       // 0x28: the file's block table, for span reads
    uint32_t m_eOffset;             // 0x30
    uint32_t m_eSize;               // 0x34
    uint32_t m_cOffset;             // 0x38
    uint32_t m_cSize;               // 0x3c
    uint8_t* m_dest;                // 0x40
    READDONE m_callback;            // 0x48
    void* m_context;                // 0x50
    U32SET* m_corrupt;              // 0x58
    uint8_t* m_buffer;              // 0x60: what the encoded span is read into
    QUEUELINK m_link;               // 0x68
    uint32_t m_flags;               // 0x78: 1 streamed, 2 no decode
    READFUNCTOR m_done;             // 0x80
};
static_assert(offsetof(READREQUEST, m_link) == 0x68, "READREQUEST layout");
static_assert(sizeof(READREQUEST) == 0x88, "READREQUEST is 0x88 bytes");

// 0x108 bytes, at instance+0x378.
struct READQUEUE {
    Instance* m_instance;           // 0x00
    QUEUELINK m_waiting[3];         // 0x08
    blz::object_pool m_requests;    // 0x38: 0x88-byte requests
    CRITICAL_SECTION m_lock;        // 0xd0
    uint32_t m_maxInFlight;         // 0xf8
    volatile uint32_t m_inFlight;   // 0xfc
    uint8_t m_dispatching;          // 0x100

    void Init(Instance* instance);
    void Destroy();
    void SetDepth(uint32_t depth);

    void Submit(const uint8_t ekey[16], uint8_t keySize, const char* eSpec, uint32_t codec, ENCODINGHEADER* header,
        const uint64_t* eSpan, const uint64_t* cSpan, uint8_t* dest, uint8_t partial, uint8_t priority, uint32_t flags,
        READDONE callback, void* context, READREQUEST** out, U32SET* corrupt);
    bool Cancel(READREQUEST* request);
    void Dispatch(READREQUEST* request);
    void Pump();
    void Fail(READREQUEST* request, uint8_t error);
    void BeginBatch();
    void EndBatch();
};
static_assert(offsetof(READQUEUE, m_requests) == 0x38, "READQUEUE layout");
static_assert(offsetof(READQUEUE, m_lock) == 0xd0, "READQUEUE layout");
static_assert(sizeof(READQUEUE) == 0x108, "READQUEUE is 0x108 bytes");

uint8_t ReadRequestDecode(READREQUEST* request);

}

#endif
