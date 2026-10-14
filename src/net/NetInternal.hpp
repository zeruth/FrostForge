#ifndef NET_NET_INTERNAL_HPP
#define NET_NET_INTERNAL_HPP

// WoW/Source/Net/NetInternal.cpp and NetInternal.h: the queue a NetClient's connection events
// wait in until the main thread handles them. The network threads add (connected, can't
// connect, disconnected, ...); the client's poll takes them off.

#include "storm/list/TSList.hpp"
#include "storm/thread/SCritSect.hpp"
#include <cstdint>

class NetClient;
class TObjectAllocHeap;
struct WowConnection;

// What a queued event reports (the values NetClient's connection callbacks post).
enum NETEVENTID : uint32_t {
    NETEVENT_CONNECTED      = 0x13,
    NETEVENT_CANTCONNECT    = 0x14,
    NETEVENT_DISCONNECTED   = 0x15,
    NETEVENT_CLOSED         = 0x16,
};

// One queued event (0x130 bytes, from the NETEVENTQUEUENODE object heap). Up to 0x100 bytes of
// data are held inline; more go to the heap.
struct NETEVENTQUEUENODE {
    TSLink<NETEVENTQUEUENODE> m_link;   // 0x000
    uint32_t m_eventId;                 // 0x010
    uint32_t m_timeStamp;               // 0x014
    void* m_data;                       // 0x018: when m_dataSize > 0x100
    uint8_t m_inline[0x100];            // 0x020
    uint32_t m_dataSize;                // 0x120
    uint8_t m_pad124[0xc];              // 0x124: the allocator's index in the last 4 bytes

    void SetData(const void* data, uint32_t bytes);
};
static_assert(sizeof(NETEVENTQUEUENODE) == 0x130, "NETEVENTQUEUENODE is 0x130 bytes");

typedef TSList<NETEVENTQUEUENODE, TSGetLink<NETEVENTQUEUENODE>, 8> NETEVENTLIST;

// A NetClient's event queue (0x48 bytes).
class NETEVENTQUEUE {
    public:
    NetClient* m_client;                // 0x00
    SCritSect m_critSect;               // 0x08
    NETEVENTLIST m_eventList;           // 0x30

    explicit NETEVENTQUEUE(NetClient* client);
    ~NETEVENTQUEUE();
    void AddEvent(NETEVENTID eventId, WowConnection* connection, NetClient* client, const void* data,
        uint32_t bytes);
    // Every queued event dropped, and the ones taken off for handling with them.
    void Clear();
};
static_assert(sizeof(NETEVENTQUEUE) == 0x48, "NETEVENTQUEUE is 0x48 bytes");

// Frees every node of an event list back to the node heap.
void NetEventListClear(NETEVENTLIST* list);
// Frees one node: its heap data, its link, its slot in the node heap.
void NetEventNodeDelete(TObjectAllocHeap* heap, NETEVENTQUEUENODE* node);

#endif
