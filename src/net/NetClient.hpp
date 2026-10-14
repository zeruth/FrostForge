#ifndef NET_NET_CLIENT_HPP
#define NET_NET_CLIENT_HPP

// WoW/Source/Net/NetClient/NetClient.cpp: the client end of a server session (0x368 bytes,
// vtable 0x142723aa8). It owns up to four WowConnections (the first is the session's own),
// their inflate streams, an event queue the network threads post into, and the list of
// connections waiting to be deleted. RealmConnection derives from it, ClientServices from that.
//
// The reference's slot order is kept: translated code calls through this vtable. Slots not
// ported yet forward to the reference's own code, translated (src/gen).

#include "base/CDataStore.hpp"
#include "storm/array/TSGrowableArray.hpp"
#include "storm/list/TSList.hpp"
#include "storm/thread/SCritSect.hpp"
#include <cstddef>
#include <cstdint>

struct OSNETADDRESS;
struct WowConnection;
struct z_stream_s;
class NETEVENTQUEUE;

// NS_*: m_netState.
enum NETSTATE : int32_t {
    NS_UNINITIALIZED    = 0,
    NS_INITIALIZED      = 2,
    NS_CONNECTING       = 4,
    NS_CONNECTED        = 5,
    NS_DISCONNECTING    = 6,
};

extern const char s_tagWowConnectionArray[];   // ".?AV?$TSGrowableArray@PEAVWowConnection@@$07@@"

class NetClient {
    public:
    // A message held back until the client can take it (TSList<NetClient::HELDMESSAGE>).
    struct HELDMESSAGE {
        TSLink<HELDMESSAGE> m_link;     // 0x00
        uint64_t m_unk10;               // 0x10
        CDataStore m_data;              // 0x18
    };
    typedef TSList<HELDMESSAGE, TSGetLink<HELDMESSAGE>, 8> HELDMESSAGELIST;

    // Ping bookkeeping per connection (0x58).
    struct PINGINFO {
        int32_t m_serial;               // 0x00: the last ping sent
        uint32_t m_pad04;
        uint64_t m_lastPingTicks;       // 0x08
        uint32_t m_latency[16];         // 0x10
        uint32_t m_latencyNext;         // 0x50
        uint32_t m_latencyCount;        // 0x54
    };

    // Per connection: its mode bits and 0x30 bytes of key state (0x31, packed). The last 0x10
    // bytes are wiped when the client is destroyed.
    #pragma pack(push, 1)
    struct CONNSTATE {
        uint8_t m_flags;                // 0x00: bit 0 handshake seen, bits 1-2 cleared on reset
        uint8_t m_key[0x20];            // 0x01
        uint8_t m_secret[0x10];         // 0x21
    };
    #pragma pack(pop)

    // The server key a connect-to redirect is checked against (vtable 0x142723a10, 0x50):
    // a 2048-bit RSA modulus and its exponent, as big numbers. Built and freed by the
    // reference's code, translated.
    struct CONNECTKEY {
        const void* m_vtable;           // 0x00
        uint32_t m_bytes;               // 0x08
        int32_t m_bits;                 // 0x0c
        uint8_t m_modulus[0x20];        // 0x10
        uint8_t m_exponent[0x20];       // 0x30
    };

    uint8_t m_unk008[0x68];             // 0x008: not touched by the constructor
    SCritSect m_netCritSect;            // 0x070
    NETSTATE m_netState;                // 0x098
    uint32_t m_pad09c;
    NETEVENTQUEUE* m_netEventQueue;     // 0x0a0
    volatile int32_t m_pendingEvents;   // 0x0a8: events queued, not yet handled
    uint8_t m_unk0ac;                   // 0x0ac
    uint8_t m_pad0ad[3];
    PINGINFO m_ping[2];                 // 0x0b0
    uint8_t m_unk160[0x10];             // 0x160
    uint32_t m_unk170;                  // 0x170
    volatile int32_t m_bytesPending;    // 0x174: bytes received, not yet handled
    uint32_t m_connectTime;             // 0x178: the time stamp of the last connect
    uint32_t m_connectTimeout;          // 0x17c: handed to each new WowConnection
    uint8_t m_unk180[0x10];             // 0x180
    WowConnection* m_serverConnection[4];   // 0x190
    z_stream_s* m_inflateStream[4];     // 0x1b0
    HELDMESSAGELIST m_heldMessages[2];  // 0x1d0
    uint32_t m_unk200[2];               // 0x200
    uint8_t m_suppressDisconnect[2];    // 0x208
    CONNSTATE m_connState[4];           // 0x20a
    uint8_t m_pad2ce[2];
    SCritSect m_deleteLock;             // 0x2d0
    TSGrowableArray<WowConnection*, s_tagWowConnectionArray> m_deleteList;  // 0x2f8
    CONNECTKEY m_connectKey;            // 0x318

    NetClient();
    virtual ~NetClient();                                                   // 0x00
    // WowConnectionResponse
    virtual void WCConnected(WowConnection* connection, uint64_t a3, uint64_t a4, uint32_t timeStamp);  // 0x08
    virtual void WCDisconnected(WowConnection* connection, uint64_t a3, uint64_t a4);    // 0x10
    virtual void WCCantConnect(WowConnection* connection, uint64_t a3, uint64_t a4);     // 0x18
    virtual void WCDataReady(WowConnection* connection, uint32_t timeStamp, void* data); // 0x20
    virtual void Slot28();                                                  // 0x28
    virtual void Slot30();                                                  // 0x30
    virtual bool Slot38();                                                  // 0x38
    // NetClient
    virtual bool Initialize();                                              // 0x40
    virtual void Destroy();                                                 // 0x48
    virtual bool Close();                                                   // 0x50
    virtual void Disconnect(uint32_t reason);                               // 0x58
    virtual void Poll();                                                    // 0x60
    virtual bool HandleData(uint32_t a2, uint32_t a3, void* data, int32_t bytes);  // 0x68
    virtual void HandleConnect();                                           // 0x70
    virtual void HandleDisconnect();                                        // 0x78
    virtual void HandleCantConnect();                                       // 0x80
    virtual bool WriteAuthData(CDataStore* store) = 0;                      // 0x88

    void Connect(const OSNETADDRESS* address, uint16_t port);
    // The secondary connections (1-3) told the reason, queued for deletion, their streams freed.
    void CloseSecondaryConnections(uint8_t reason);
    void ResetInflateStream(int32_t index);
    void HandleCompressedData(WowConnection* connection, const uint32_t* data, int32_t bytes);
    void ReleaseConnection(WowConnection* connection);
    void DeleteConnections(bool closeNow);
    // PacketPipe.h, not ported yet: the message written and sent, and a received packet handled.
    void Send(void* message, WowConnection* connection);
    void HandlePacket(WowConnection* connection, uint32_t timeStamp, const void* data, uint32_t bytes);
};
static_assert(offsetof(NetClient, m_netCritSect) == 0x70, "NetClient layout");
static_assert(offsetof(NetClient, m_netState) == 0x98, "NetClient layout");
static_assert(offsetof(NetClient, m_netEventQueue) == 0xa0, "NetClient layout");
static_assert(offsetof(NetClient, m_pendingEvents) == 0xa8, "NetClient layout");
static_assert(offsetof(NetClient, m_ping) == 0xb0, "NetClient layout");
static_assert(offsetof(NetClient, m_bytesPending) == 0x174, "NetClient layout");
static_assert(offsetof(NetClient, m_connectTimeout) == 0x17c, "NetClient layout");
static_assert(offsetof(NetClient, m_serverConnection) == 0x190, "NetClient layout");
static_assert(offsetof(NetClient, m_inflateStream) == 0x1b0, "NetClient layout");
static_assert(offsetof(NetClient, m_heldMessages) == 0x1d0, "NetClient layout");
static_assert(offsetof(NetClient, m_suppressDisconnect) == 0x208, "NetClient layout");
static_assert(offsetof(NetClient, m_connState) == 0x20a, "NetClient layout");
static_assert(offsetof(NetClient, m_deleteLock) == 0x2d0, "NetClient layout");
static_assert(offsetof(NetClient, m_deleteList) == 0x2f8, "NetClient layout");
static_assert(offsetof(NetClient, m_connectKey) == 0x318, "NetClient layout");
static_assert(sizeof(NetClient) == 0x368, "NetClient is 0x368 bytes");

// What the generated message dispatch hands a message handler: the session the message
// arrived on, its connection, and the decoded message from +0x20 (its layout is the message's).
struct NETMESSAGEARGS {
    void* m_unk00;
    NetClient* m_client;                // 0x08
    WowConnection* m_connection;        // 0x10
    uint64_t m_unk18;
    uint8_t m_message[8];               // 0x20

    template <class T>
    T& Message(size_t offset = 0) const {
        return *reinterpret_cast<T*>(const_cast<uint8_t*>(this->m_message) + offset);
    }
};

// A slot of the generated message dispatch (0x140727d90 and its siblings), which is not
// ported: its handlers are the reference's own data, set where they lie.
inline void*& NetMessageHandler(uintptr_t address) {
    return *reinterpret_cast<void**>(address);
}

// The session's traffic, counted across connections.
extern uint32_t g_netBytesSent;         // DAT_1430a1450
extern uint32_t g_netBytesReceived;     // DAT_1430a145c
extern uint32_t g_netStartTime;         // DAT_1430a1468

// A disconnect-reason message (0x30 bytes), as the session sends one before it closes a
// connection. Built by the reference's code, translated.
struct NETREASONMESSAGE {
    uint8_t m_bytes[0x30];
};
void NetReasonMessageInit(NETREASONMESSAGE* message, uint32_t reason);

#endif
