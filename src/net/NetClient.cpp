#include "util/FrostLog.hpp"
#include "net/NetClient.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "net/NetInternal.hpp"
#include "net/WowConnection.hpp"
#include "os/Os.hpp"
#include "os/Timer.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Fnv.hpp"
#include <cstring>
#include <intrin.h>
#include <malloc.h>
#include <new>
#include <windows.h>
#include "zlib.h"

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_1420be010(unsigned long long, unsigned long long, unsigned long long, unsigned long long,
    unsigned long long);
unsigned long long FUN_1420cd4e0(unsigned long long);
unsigned long long FUN_1406d88d0(unsigned long long, unsigned long long);
unsigned long long FUN_1412357c0(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_141235030(unsigned long long, unsigned long long, unsigned long long, unsigned long long,
    unsigned long long);
unsigned long long FUN_141235ae0(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_141235960(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_141235c10(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_141235d50(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_141234a10(unsigned long long);
unsigned long long FUN_141234900(unsigned long long, unsigned long long, unsigned long long, unsigned long long,
    unsigned long long);
unsigned long long FUN_141232760(unsigned long long);
unsigned long long FUN_141233180(unsigned long long);
unsigned long long FUN_141233710(unsigned long long);
unsigned long long FUN_1412333f0(unsigned long long);
unsigned long long FUN_141233090(unsigned long long);
unsigned long long FUN_141232c30(unsigned long long);
unsigned long long FUN_1412332a0(unsigned long long);
}

const char s_tagWowConnectionArray[] = ".?AV?$TSGrowableArray@PEAVWowConnection@@$07@@";

uint32_t g_netBytesSent;                // DAT_1430a1450
uint32_t g_netBytesReceived;            // DAT_1430a145c
uint32_t g_netStartTime;                // DAT_1430a1468

namespace {

const char s_netClientFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\Net\\NetClient\\NetClient.cpp";
const char s_tagWowConnectionFixed[] = ".?AV?$TSFixedArray@PEAVWowConnection@@$07@@";
const char s_tagHeldMessages[] =
    ".?AV?$TSList@UHELDMESSAGE@NetClient@@V?$TSGetLink@UHELDMESSAGE@NetClient@@@@$07@@";

CVar* s_useIPv6;                        // DAT_1430a1440
CVar* s_disableServerNagle;             // DAT_1430a1448

// The server key (0x1427237b0, 0x100 bytes) and its exponent (0x1427237a0, 4 bytes): the
// reference's own data, read where it lies.
const uintptr_t CONNECTKEY_MODULUS = 0x1427237b0;
const uintptr_t CONNECTKEY_EXPONENT = 0x1427237a0;
const uintptr_t CONNECTKEY_VTABLE = 0x142723a10;

// The inflate streams' allocator.
// ref: FUN_141235dd0
voidpf InflateAlloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    return SMemAlloc(uint64_t(items * size), 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x739, 0);
}

// ref: 0x141235e30 (reached only through the pointer)
void InflateFree(voidpf opaque, voidpf address) {
    (void)opaque;
    SMemFree(address, s_netClientFile, 0x73f, 0);
}

// The server key's constructor (the modulus and exponent read into big numbers).
// translated: FUN_1420be010
void ConnectKeyConstruct(NetClient::CONNECTKEY* key) {
    __try {
        FUN_1420be010(reinterpret_cast<uintptr_t>(key), CONNECTKEY_MODULUS, 0x100, CONNECTKEY_EXPONENT, 4);
    } __except (GenFault(0x1420be010ull, GetExceptionInformation())) {
    }
}

// A big number's storage freed.
// translated: FUN_1420cd4e0
void BigNumFree(void* number) {
    __try {
        FUN_1420cd4e0(reinterpret_cast<uintptr_t>(number));
    } __except (GenFault(0x1420cd4e0ull, GetExceptionInformation())) {
    }
}

void InflateStreamDelete(z_stream_s* stream) {
    inflateEnd(stream);
    ::operator delete(stream, 0x58);
}

// The session's message handlers, by what they do. Not ported yet: they run translated.

// Answers the server's opening challenge.
// translated: FUN_141232760
void HandleServerChallenge(NETMESSAGEARGS* args) {
    __try {
        FUN_141232760(reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x141232760ull, GetExceptionInformation())) {
    }
}

// The switch to the encrypted mode ("mode change out of order", "server check failed").
// translated: FUN_141233180
void HandleEnterEncryptedMode(NETMESSAGEARGS* args) {
    __try {
        FUN_141233180(reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x141233180ull, GetExceptionInformation())) {
    }
}

// Answers a ping with the time.
// translated: FUN_141233710
void HandlePing(NETMESSAGEARGS* args) {
    __try {
        FUN_141233710(reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x141233710ull, GetExceptionInformation())) {
    }
}

// Releases the held messages once the server resumes the connection.
// translated: FUN_1412333f0
void HandleResumeComms(NETMESSAGEARGS* args) {
    __try {
        FUN_1412333f0(reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x1412333f0ull, GetExceptionInformation())) {
    }
}

// Lets a secondary connection go.
// translated: FUN_141233090
void HandleSuspendComms(NETMESSAGEARGS* args) {
    __try {
        FUN_141233090(reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x141233090ull, GetExceptionInformation())) {
    }
}

// Opens a connection to another address the server names.
// translated: FUN_141232c30
void HandleConnectTo(NETMESSAGEARGS* args) {
    __try {
        FUN_141232c30(reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x141232c30ull, GetExceptionInformation())) {
    }
}

// The latency from a pong ("Received pong with old sequence").
// translated: FUN_1412332a0
void HandlePong(NETMESSAGEARGS* args) {
    __try {
        FUN_1412332a0(reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x1412332a0ull, GetExceptionInformation())) {
    }
}

// The server restarts compression: the connection's inflate stream is reset.
// ref: 0x1412333b0 (reached only through the pointer)
void HandleResetCompressionContext(NETMESSAGEARGS* args) {
    NetClient* client = args->m_client;
    int32_t index = 0;
    while (index < 4 && client->m_serverConnection[index] != args->m_connection) {
        index++;
    }
    // an unknown connection reads one past the array, as the reference does
    auto streams = reinterpret_cast<z_stream_s**>(reinterpret_cast<uint8_t*>(client) + 0x1b0);
    inflateReset(streams[index]);
}

}

// translated: FUN_1406d88d0
void NetReasonMessageInit(NETREASONMESSAGE* message, uint32_t reason) {
    __try {
        FUN_1406d88d0(reinterpret_cast<uintptr_t>(message), reason);
    } __except (GenFault(0x1406d88d0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_141231cc0
NetClient::NetClient() {
    this->m_netCritSect.Initialize();
    for (CONNSTATE& state : this->m_connState) {
        state.m_flags &= 0xf8;
        std::memset(state.m_key, 0, sizeof(state.m_key));
        std::memset(state.m_secret, 0, sizeof(state.m_secret));
    }
    this->m_deleteLock.Initialize();
    ConnectKeyConstruct(&this->m_connectKey);
    this->m_netState = NS_UNINITIALIZED;
    this->m_netEventQueue = nullptr;
    for (int32_t i = 0; i < 4; i++) {
        this->m_serverConnection[i] = nullptr;
        this->m_inflateStream[i] = nullptr;
    }
    this->m_suppressDisconnect[0] = 0;
    this->m_suppressDisconnect[1] = 1;
    _InterlockedExchange(reinterpret_cast<volatile long*>(&this->m_pendingEvents), 0);
    this->m_unk0ac = 0;
    for (CONNSTATE& state : this->m_connState) {
        state.m_flags &= 0xf8;
        std::memset(state.m_key, 0, sizeof(state.m_key));
        std::memset(state.m_secret, 0, sizeof(state.m_secret));
    }
    for (PINGINFO& ping : this->m_ping) {
        ping.m_serial = 0;
        ping.m_lastPingTicks = 0;
        std::memset(ping.m_latency, 0, sizeof(ping.m_latency));
        ping.m_latencyNext = 0;
        ping.m_latencyCount = 0;
    }
    this->m_unk170 = 0;
    _InterlockedExchange(reinterpret_cast<volatile long*>(&this->m_bytesPending), 0);
    this->m_connectTime = 0;
    this->m_connectTimeout = 0;

    NetMessageHandler(0x14309e118) = reinterpret_cast<void*>(&HandleServerChallenge);
    NetMessageHandler(0x14309e1c0) = reinterpret_cast<void*>(&HandleEnterEncryptedMode);
    NetMessageHandler(0x14309e218) = reinterpret_cast<void*>(&HandlePing);
    NetMessageHandler(0x14309e270) = reinterpret_cast<void*>(&HandleResumeComms);
    NetMessageHandler(0x14309e2c0) = reinterpret_cast<void*>(&HandleSuspendComms);
    NetMessageHandler(0x14309e328) = reinterpret_cast<void*>(&HandleConnectTo);
    NetMessageHandler(0x14309e398) = reinterpret_cast<void*>(&HandlePong);
    NetMessageHandler(0x14309e400) = reinterpret_cast<void*>(&HandleResetCompressionContext);

    s_useIPv6 = CVarRegister("useIPv6", "Enable the usage of IPv6 sockets", 1, "0", static_cast<CVARCALLBACK>(nullptr), 6,
        false, nullptr, false);
    s_disableServerNagle = CVarRegister("disableServerNagle", "Disable server-side nagle algorithm", 1, "1",
        static_cast<CVARCALLBACK>(nullptr), 6, false, nullptr, false);
}

// ref: FUN_1412320d0
NetClient::~NetClient() {
    this->NetClient::Destroy();
    // the server key's destructor: the exponent, then the modulus
    this->m_connectKey.m_vtable = reinterpret_cast<const void*>(CONNECTKEY_VTABLE);
    BigNumFree(this->m_connectKey.m_exponent);
    BigNumFree(this->m_connectKey.m_modulus);
    if (this->m_deleteList.m_data != nullptr) {
        SMemFree(this->m_deleteList.m_data, s_tagWowConnectionFixed, -2, 0);
    }
    this->m_deleteLock.Destroy();
    for (int32_t i = 3; i >= 0; i--) {
        SecureZeroMemory(this->m_connState[i].m_secret, sizeof(this->m_connState[i].m_secret));
    }
    // the held message lists empty themselves (their destructors), then the lock goes; the
    // lists are empty after Destroy, so the lock going first changes nothing
    this->m_netCritSect.Destroy();
}

// ref: FUN_1412322b0 (the deleting destructor: the compiler's, through slot 0x00)

// Records the connect and posts it for the primary connection; a secondary one is logged.
// translated: FUN_141235ae0
void NetClient::WCConnected(WowConnection* connection, uint64_t a3, uint64_t a4, uint32_t timeStamp) {
    __try {
        FUN_141235ae0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(connection), a3,
            (a4 & 0xffffffff00000000ull) | timeStamp);
    } __except (GenFault(0x141235ae0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_141235960
void NetClient::WCDisconnected(WowConnection* connection, uint64_t a3, uint64_t a4) {
    __try {
        FUN_141235960(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(connection), a3, a4);
    } __except (GenFault(0x141235960ull, GetExceptionInformation())) {
    }
}

// translated: FUN_141235c10
void NetClient::WCCantConnect(WowConnection* connection, uint64_t a3, uint64_t a4) {
    __try {
        FUN_141235c10(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(connection), a3, a4);
    } __except (GenFault(0x141235c10ull, GetExceptionInformation())) {
    }
}

// translated: FUN_141235d50
void NetClient::WCDataReady(WowConnection* connection, uint32_t timeStamp, void* data) {
    __try {
        FUN_141235d50(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(connection), timeStamp,
            reinterpret_cast<uintptr_t>(data));
    } __except (GenFault(0x141235d50ull, GetExceptionInformation())) {
    }
}

// Slots 0x28 and 0x30 are empty (_guard_check_icall); 0x38 returns false (0x1402ca290).
void NetClient::Slot28() {
}

void NetClient::Slot30() {
}

bool NetClient::Slot38() {
    return false;
}

// The event queue and the primary connection, then the session is ready to connect.
// ref: FUN_141234d00
bool NetClient::Initialize() {
    void* memory = SMemAlloc(sizeof(NETEVENTQUEUE), 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x160, 0);
    this->m_netEventQueue = new (memory) NETEVENTQUEUE(this);
    memory = SMemAlloc(sizeof(WowConnection), 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x162, 0);
    WowConnection* connection = WowConnectionConstruct(memory, this, nullptr, 0, 0);
    this->m_serverConnection[0] = connection;
    connection->m_connectTimeout = this->m_connectTimeout;
    this->ResetInflateStream(0);
    this->m_netState = NS_INITIALIZED;
    g_netStartTime = OsGetAsyncTimeMs();
    return true;
}

// The held messages dropped and, if a session was up, the session ended and its parts freed;
// then every connection waiting for deletion is closed and released.
// ref: FUN_141233e40
void NetClient::Destroy() {
    this->m_netCritSect.Enter();
    for (HELDMESSAGELIST& list : this->m_heldMessages) {
        while (HELDMESSAGE* message = list.Head()) {
            message->m_data.~CDataStore();
            message->m_link.Unlink();
            SMemFree(message, s_tagHeldMessages, -2, 0);
        }
    }
    if (this->m_netState == NS_UNINITIALIZED) {
        this->m_netCritSect.Leave();
        return;
    }
    this->Disconnect(0xd);
    this->ReleaseConnection(this->m_serverConnection[0]);
    this->m_serverConnection[0] = nullptr;
    for (z_stream_s*& stream : this->m_inflateStream) {
        if (z_stream_s* old = stream) {
            stream = nullptr;
            InflateStreamDelete(old);
        }
    }
    if (NETEVENTQUEUE* queue = this->m_netEventQueue) {
        queue->~NETEVENTQUEUE();
        ::operator delete(queue, sizeof(NETEVENTQUEUE));
    }
    this->m_netEventQueue = nullptr;
    this->m_netState = NS_UNINITIALIZED;
    uint32_t now = OsGetAsyncTimeMs();
    ConsoleWriteFormat(0, "Client net stats: %lu bytes sent, %lu bytes received, %lu msec elapsed", g_netBytesSent,
        g_netBytesReceived, now - g_netStartTime);
    this->m_netCritSect.Leave();
    this->DeleteConnections(true);
}

// The session ended with reason 0xd and an event posted for it.
// ref: FUN_141233de0
bool NetClient::Close() {
    if (this->m_netState == NS_UNINITIALIZED) {
        return false;
    }
    this->Disconnect(0xd);
    this->m_netEventQueue->AddEvent(NETEVENT_CLOSED, nullptr, this, nullptr, 0);
    return true;
}

// A connected session sends the reason and closes; one not yet connected gets a fresh primary
// connection, ready to connect again.
// ref: FUN_141234230
void NetClient::Disconnect(uint32_t reason) {
    ConsoleWriteFormat(0, "Disconnecting for reason %d", reason);
    FrostLog("net", "NetClient::Disconnect(%u)", reason);
    FrostLogStack(0, 16);
    this->m_netCritSect.Enter();
    this->CloseSecondaryConnections(static_cast<uint8_t>(reason));
    if (this->m_netState == NS_CONNECTED) {
        this->m_netState = NS_DISCONNECTING;
        WowConnection* connection = this->m_serverConnection[0];
        NETREASONMESSAGE message;
        NetReasonMessageInit(&message, reason & 0xff);
        this->Send(&message, connection);
        WowConnectionDisconnect(this->m_serverConnection[0]);
    } else if (this->m_netState != NS_DISCONNECTING) {
        WowConnection* connection = this->m_serverConnection[0];
        NETREASONMESSAGE message;
        NetReasonMessageInit(&message, reason & 0xff);
        this->Send(&message, connection);
        this->ReleaseConnection(this->m_serverConnection[0]);
        z_stream_s* stream = this->m_inflateStream[0];
        this->m_inflateStream[0] = nullptr;
        InflateStreamDelete(stream);
        this->m_netEventQueue->Clear();
        void* memory = SMemAlloc(sizeof(WowConnection), 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x1fd, 0);
        void* pipeMemory = SMemAlloc(0x2208, 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x1fd, 0);
        PacketPipe* pipe = PacketPipeConstruct(pipeMemory, false);
        this->m_serverConnection[0] = WowConnectionConstruct(memory, this, pipe, 0, 0);
        this->ResetInflateStream(0);
        this->m_netState = NS_INITIALIZED;
    }
    this->m_netCritSect.Leave();
}

// Pings each connection every 30 seconds.
// translated: FUN_141234a10
void NetClient::Poll() {
    __try {
        FUN_141234a10(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x141234a10ull, GetExceptionInformation())) {
    }
}

// Counts the bytes and, while connected, dispatches the message.
// translated: FUN_141234900
bool NetClient::HandleData(uint32_t a2, uint32_t a3, void* data, int32_t bytes) {
    // (FROST_HEAP_VALIDATE=1: the heap checked around each message, with its first bytes)
    uint64_t head = 0;
    if (data != nullptr && bytes >= 8) {
        std::memcpy(&head, data, 8);
    }
    MemoryHeapCheck("before a message", head, static_cast<uint64_t>(bytes), a2);
    bool result = this->HandleDataTranslated(a2, a3, data, bytes);
    MemoryHeapCheck("after a message", head, static_cast<uint64_t>(bytes), a2);
    return result;
}

bool NetClient::HandleDataTranslated(uint32_t a2, uint32_t a3, void* data, int32_t bytes) {
    __try {
        return (FUN_141234900(reinterpret_cast<uintptr_t>(this), a2, a3, reinterpret_cast<uintptr_t>(data),
                    static_cast<uint32_t>(bytes)) & 0xff) != 0;
    } __except (GenFault(0x141234900ull, GetExceptionInformation())) {
        return true;
    }
}

// ref: FUN_1412348e0
void NetClient::HandleConnect() {
    this->m_netState = NS_CONNECTED;
    ConsoleWriteFormat(0, "NetClient::HandleConnect()\n");
}

// ref: FUN_1412349f0
void NetClient::HandleDisconnect() {
    this->m_netState = NS_INITIALIZED;
    ConsoleWriteFormat(0, "NetClient::HandleDisconnect()\n");
}

// ref: FUN_1412348c0
void NetClient::HandleCantConnect() {
    this->m_netState = NS_INITIALIZED;
    ConsoleWriteFormat(0, "NetClient::HandleCantConnect()\n");
}

// The primary connection gets a fresh packet pipe and connects; secondary connections close.
// An IPv6 address the connection had to fall back from turns useIPv6 off.
// ref: FUN_141233a70
void NetClient::Connect(const OSNETADDRESS* address, uint16_t port) {
    this->m_netCritSect.Enter();
    if (this->m_netState != NS_INITIALIZED) {
        ErrorDisplayFormatted(0x85100086, nullptr, 0, "Expected (m_netState == NS_INITIALIZED), got %d", this->m_netState);
    } else {
        this->m_suppressDisconnect[0] = 0;
        this->m_suppressDisconnect[1] = 1;
        this->m_netState = NS_CONNECTING;
        this->m_serverConnection[0]->m_connectTimeout = this->m_connectTimeout;
        WowConnectionSetPipe(this->m_serverConnection[0], nullptr);
        WowConnection* connection = this->m_serverConnection[0];
        void* memory = SMemAlloc(0x2208, 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x1c8, 0);
        WowConnectionSetPipe(connection, PacketPipeConstruct(memory, false));
        WowConnectionConnect(this->m_serverConnection[0], address, port, -1);
        if (address->m_family == 2 && this->m_serverConnection[0]->m_ipv6Failed) {
            if (CVar* useIPv6 = CVarLookup(t2util::ConstFnvHashStringI("useIPv6"))) {
                CVarSetInt(useIPv6, 0, true, false, false, true);
            }
        }
        this->CloseSecondaryConnections(0xc);
    }
    this->m_netCritSect.Leave();
}

// ref: FUN_141233840
void NetClient::CloseSecondaryConnections(uint8_t reason) {
    this->m_netCritSect.Enter();
    for (int32_t i = 1; i < 4; i++) {
        WowConnection* connection = this->m_serverConnection[i];
        if (connection == nullptr) {
            continue;
        }
        NETREASONMESSAGE message;
        NetReasonMessageInit(&message, reason);
        this->Send(&message, connection);
        this->ReleaseConnection(this->m_serverConnection[i]);
        z_stream_s* stream = this->m_inflateStream[i];
        this->m_inflateStream[i] = nullptr;
        InflateStreamDelete(stream);
        this->m_serverConnection[i] = nullptr;
    }
    this->m_netCritSect.Leave();
}

// A raw-deflate stream for a connection, made the first time and reset after.
// ref: FUN_141234c30
void NetClient::ResetInflateStream(int32_t index) {
    if (z_stream_s* stream = this->m_inflateStream[index]) {
        inflateReset(stream);
        return;
    }
    auto stream = static_cast<z_stream_s*>(
        SMemAlloc(sizeof(z_stream), 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x748, 0));
    stream->zalloc = &InflateAlloc;
    stream->zfree = &InflateFree;
    stream->opaque = nullptr;
    stream->avail_in = 0;
    stream->next_in = nullptr;
    inflateInit2(stream, -15);
    this->m_inflateStream[index] = stream;
}

// A compressed packet: {uncompressed size, its adler32, the compressed bytes' adler32, the
// compressed bytes}. Both checksums start from 0x9827d8f1. A packet that fails a check or
// does not inflate exactly closes the connection with reason 0x11 (3 for a bad size).
// ref: FUN_141233c00
void NetClient::HandleCompressedData(WowConnection* connection, const uint32_t* data, int32_t bytes) {
    uint32_t reason = 3;
    uint8_t* heapBuffer = nullptr;
    if (bytes > 0xb && data[0] < 0x8000000) {
        uint32_t size = data[0];
        uint32_t uncompressedSum = data[1];
        uint32_t compressedSum = data[2];
        uint8_t* buffer;
        if (size < 0x1000) {
            buffer = static_cast<uint8_t*>(_alloca((size + 0xf) & ~0xfu));
        } else {
            buffer = static_cast<uint8_t*>(SMemAlloc(size, 3, s_netClientFile, SStrHashFnv1a(s_netClientFile), 0x6dd, 0));
            heapBuffer = buffer;
        }
        reason = 0x11;
        auto compressed = reinterpret_cast<const Bytef*>(data + 3);
        if (adler32(0x9827d8f1, compressed, static_cast<uInt>(bytes - 0xc)) == compressedSum) {
            int32_t index = 0;
            while (index < 4 && this->m_serverConnection[index] != connection) {
                index++;
            }
            // an unknown connection reads one past the array, as the reference does
            auto streams = reinterpret_cast<z_stream_s**>(reinterpret_cast<uint8_t*>(this) + 0x1b0);
            z_stream_s* stream = streams[index];
            stream->next_in = const_cast<Bytef*>(compressed);
            stream->avail_in = static_cast<uInt>(bytes - 0xc);
            stream->next_out = buffer;
            stream->avail_out = size;
            if (inflate(stream, Z_SYNC_FLUSH) == Z_OK && stream->avail_in == 0 && stream->avail_out == 0
                && adler32(0x9827d8f1, buffer, size) == uncompressedSum) {
                this->HandlePacket(connection, 0, buffer, size);
                SMemFree(heapBuffer, s_netClientFile, 0x704, 0);
                return;
            }
        }
    }
    // the inflate buffer is not freed on this path, as in the reference
    NETREASONMESSAGE message;
    NetReasonMessageInit(&message, reason);
    this->Send(&message, connection);
    WowConnectionDisconnect(connection);
}

// Queues a connection for deletion (DeleteConnections).
// ref: FUN_141234f90
void NetClient::ReleaseConnection(WowConnection* connection) {
    this->m_deleteLock.Enter();
    this->m_deleteList.Add(connection, static_cast<const char*>(_ReturnAddress()));
    this->m_deleteLock.Leave();
}

// The queued connections taken over, each detached from this client, closed (at once, or by
// an orderly disconnect) and released.
// ref: FUN_1412325a0
void NetClient::DeleteConnections(bool closeNow) {
    TSGrowableArray<WowConnection*, s_tagWowConnectionArray> connections;
    this->m_deleteLock.Enter();
    uint32_t count = this->m_deleteList.m_count;
    if (count != 0) {
        connections.Reserve(count, static_cast<const char*>(_ReturnAddress()));
        for (uint32_t i = 0; i < count; i++) {
            connections.m_data[i] = this->m_deleteList.m_data[i];
        }
    }
    if (this->m_deleteList.m_data != nullptr) {
        SMemFree(this->m_deleteList.m_data, s_tagWowConnectionFixed, -2, 0);
    }
    this->m_deleteList.m_alloc = 0;
    this->m_deleteList.m_count = 0;
    this->m_deleteList.m_data = nullptr;
    this->m_deleteLock.Leave();
    for (uint32_t i = 0; i < count; i++) {
        WowConnection* connection = connections.m_data[i];
        WowConnectionSetResponse(connection, nullptr, false);
        if (closeNow) {
            if (connection->m_connState == 2 || connection->m_connState == 5) {
                WowConnectionClose(connection);
            }
        } else {
            WowConnectionDisconnect(connection);
        }
        WowConnectionRelease(connection);
    }
    if (connections.m_data != nullptr) {
        SMemFree(connections.m_data, s_tagWowConnectionFixed, -2, 0);
    }
}

// translated: FUN_1412357c0
void NetClient::Send(void* message, WowConnection* connection) {
    __try {
        FUN_1412357c0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(message),
            reinterpret_cast<uintptr_t>(connection));
    } __except (GenFault(0x1412357c0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_141235030
void NetClient::HandlePacket(WowConnection* connection, uint32_t timeStamp, const void* data, uint32_t bytes) {
    __try {
        FUN_141235030(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(connection), timeStamp,
            reinterpret_cast<uintptr_t>(data), bytes);
    } __except (GenFault(0x141235030ull, GetExceptionInformation())) {
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1406d88d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141232760=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141232c30=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141233090=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141233180=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412332a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412333f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141233710=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141234900=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141234a10=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141235030=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412357c0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141235960=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141235ae0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141235c10=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141235d50=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420be010=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1420cd4e0=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a1440 = (void*)&s_useIPv6;
extern "C" void* const gen_p_1430a1448 = (void*)&s_disableServerNagle;
// ---- end of generated globals ----
