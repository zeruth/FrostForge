#include "net/NetInternal.hpp"
#include "base/ObjectAlloc.hpp"
#include "net/NetClient.hpp"
#include "os/Timer.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Fnv.hpp"
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <new>

namespace {

const char s_internalFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\Net\\NetInternal.cpp";
const char s_internalHeader[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\Net\\NetInternal.h";

TObjectAllocHeap* s_nodeHeap;           // DAT_1430a14a0: the NETEVENTQUEUENODE heap, made on first use

}

// The events taken off a queue for handling, and how many (the client's poll fills them).
NETEVENTLIST s_takenEvents;             // DAT_142cfb9c0
uint32_t s_takenEventCount;             // DAT_1430a14a8

namespace {

// Registered with atexit when the node heap is made.
// ref: FUN_1412363a0
void NetEventNodeHeapDestroy() {
    if (s_nodeHeap != nullptr) {
        delete s_nodeHeap;
    }
    s_nodeHeap = nullptr;
}

}

// ref: FUN_141235eb0
NETEVENTQUEUE::NETEVENTQUEUE(NetClient* client) {
    this->m_critSect.Initialize();
    this->m_client = client;
}

// ref: FUN_141235f00
NETEVENTQUEUE::~NETEVENTQUEUE() {
    NetEventListClear(&s_takenEvents);
    s_takenEventCount = 0;
    this->m_critSect.Enter();
    NetEventListClear(&this->m_eventList);
    this->m_critSect.Leave();
    // the reference deletes the lock after the (now empty) list's destructor; nothing is left
    // in the list for that order to matter
    this->m_critSect.Destroy();
}

// The connection is not recorded: an event stands for the client's primary connection.
// ref: FUN_141235f60
void NETEVENTQUEUE::AddEvent(NETEVENTID eventId, WowConnection* connection, NetClient* client, const void* data,
    uint32_t bytes) {
    (void)connection;
    this->m_critSect.Enter();
    if (s_nodeHeap == nullptr) {
        void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x26, s_internalFile, SStrHashFnv1a(s_internalFile), 0x9d, 0);
        s_nodeHeap = new (memory) TObjectAllocHeap("NETEVENTQUEUENODE", sizeof(NETEVENTQUEUENODE), 0x90, 0x26);
        atexit(&NetEventNodeHeapDestroy);
    }
    auto node = static_cast<NETEVENTQUEUENODE*>(s_nodeHeap->Alloc());
    node->m_link.m_prevlink = nullptr;
    node->m_link.m_next = nullptr;
    node->m_data = nullptr;
    node->m_dataSize = 0;
    this->m_eventList.LinkToTail(node);
    node->m_eventId = eventId;
    if (bytes != 0) {
        node->SetData(data, bytes);
    }
    node->m_timeStamp = OsPeekAsyncTimeMs();
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&client->m_pendingEvents));
    this->m_critSect.Leave();
}

// ref: FUN_1412361d0
void NETEVENTQUEUE::Clear() {
    NetEventListClear(&s_takenEvents);
    s_takenEventCount = 0;
    this->m_critSect.Enter();
    NetEventListClear(&this->m_eventList);
    this->m_critSect.Leave();
}

// ref: FUN_141236120
void NETEVENTQUEUENODE::SetData(const void* data, uint32_t bytes) {
    this->m_dataSize = bytes;
    if (bytes > 0x100) {
        this->m_data = SMemAlloc(bytes, 3, s_internalHeader, SStrHashFnv1a(s_internalHeader), 0x45, 0);
        std::memcpy(this->m_data, data, bytes);
        return;
    }
    std::memcpy(this->m_inline, data, bytes);
}

// ref: FUN_141236220
void NetEventListClear(NETEVENTLIST* list) {
    while (NETEVENTQUEUENODE* node = list->Head()) {
        NetEventNodeDelete(s_nodeHeap, node);
    }
}

// ref: FUN_141236300
void NetEventNodeDelete(TObjectAllocHeap* heap, NETEVENTQUEUENODE* node) {
    if (node->m_dataSize > 0x100) {
        SMemFree(node->m_data, s_internalHeader, 0x3e, 0);
    }
    node->m_link.Unlink();
    heap->Free(node);
}
