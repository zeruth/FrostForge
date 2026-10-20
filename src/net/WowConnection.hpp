#ifndef NET_WOW_CONNECTION_HPP
#define NET_WOW_CONNECTION_HPP

// WoW/Source/WowConnection/WowConnection.cpp: one socket to a server (0x228 bytes), and the
// packet pipe that frames what crosses it. Not ported yet (docs/login-roadmap.md, Phase 5):
// the functions NetClient calls run translated, and only the members NetClient reads are
// named here.

#include <cstdint>

struct OSNETADDRESS;
class PacketPipe;

struct WowConnection {
    uint8_t m_unk000[0x90];
    int32_t m_connectTimeout;           // 0x090: NetClient copies its own (+0x17c) here
    uint8_t m_unk094[0x18];
    int32_t m_connState;                // 0x0ac: 2 connecting, 5 connected, 7 closing
    uint8_t m_unk0b0[0x176];
    uint8_t m_ipv6Failed;               // 0x226: an IPv6 connect that fell back
    uint8_t m_unk227;
};
static_assert(sizeof(WowConnection) == 0x228, "WowConnection is 0x228 bytes");

// new WowConnection(response, pipe, ...) on `memory` (0x228 bytes).
WowConnection* WowConnectionConstruct(void* memory, void* response, PacketPipe* pipe, uint64_t a4, uint64_t a5);
// Replaces the connection's packet pipe (null releases it).
void WowConnectionSetPipe(WowConnection* connection, PacketPipe* pipe);
bool WowConnectionConnect(WowConnection* connection, const OSNETADDRESS* address, uint16_t port, int32_t timeout);
void WowConnectionDisconnect(WowConnection* connection);
// Detaches the response object (NetClient) from the connection.
void WowConnectionSetResponse(WowConnection* connection, void* response, bool a3);
// Closes the socket at once (for a connection still connecting or connected).
void WowConnectionClose(WowConnection* connection);
// Drops a reference; the last one deletes the connection.
void WowConnectionRelease(WowConnection* connection);

// The client/server packet pipe (WoW/Source/WowServices/PacketPipe_CliSvr.cpp, 0x2208 bytes).
PacketPipe* PacketPipeConstruct(void* memory, bool a2);

// DIVERGED (docs/stubs.md): puts FrostForge's Ed25519 public key where the reference keeps the
// one SMSG_ENTER_ENCRYPTED_MODE is verified against (0x1427b4358, read by FUN_141561000). Called
// once the reference image is mapped.
void PacketPipeInstallServerKey();

#endif
