#ifndef NET_REALM_CONNECTION_HPP
#define NET_REALM_CONNECTION_HPP

// RealmConnection (0x410 bytes, vtable 0x142740330): the NetClient that speaks the realm
// protocol. It installs the realm messages' handlers in the generated dispatch, each of
// which calls one of its virtuals (0x90 on), and keeps every live RealmConnection in a list.
// The realm data it holds (virtual realms, race/class availability, character templates) is
// filled by the auth response. ClientServices derives from it.

#include "net/NetClient.hpp"
#include <cstddef>
#include <cstdint>

// A server's game time state (0x10, 4-aligned: the realm's +0x374).
#pragma pack(push, 4)
struct GAMETIMESTATE {
    uint64_t m_unk00;                   // 0x00
    uint16_t m_unk08;                   // 0x08
    uint8_t m_unk0a;                    // 0x0a
    uint8_t m_pad0b;
    int32_t m_unk0c;                    // 0x0c

    GAMETIMESTATE();
};
#pragma pack(pop)
static_assert(sizeof(GAMETIMESTATE) == 0x10, "GAMETIMESTATE is 0x10 bytes");

class RealmConnection : public NetClient {
    public:
    // TSList<RealmConnection::REALMCONNECTIONNODE>: one per live RealmConnection (0x18).
    struct REALMCONNECTIONNODE {
        TSLink<REALMCONNECTIONNODE> m_link; // 0x00
        RealmConnection* m_connection;      // 0x10
    };

    uint8_t m_loggedIn;                 // 0x368: an auth response was accepted
    uint8_t m_pad369[3];
    uint32_t m_queuePosition;           // 0x36c
    uint32_t m_queueFlag;               // 0x370
    GAMETIMESTATE m_gameTimeState;      // 0x374
    uint32_t m_pad384;
    uint8_t m_expansion;                // 0x388
    uint8_t m_maxExpansion;             // 0x389
    uint8_t m_unk38a;                   // 0x38a
    uint8_t m_pad38b;
    uint32_t m_unk38c;                  // 0x38c
    uint32_t m_unk390;                  // 0x390
    uint32_t m_pad394;
    // TSGrowableArray<JamVirtualRealmInfo> (0x20)
    struct { uint32_t m_count; uint32_t m_pad; void* m_data; uint32_t m_alloc; uint32_t m_pad2; uint32_t m_chunk; uint32_t m_pad3; } m_virtualRealms;  // 0x398
    // blz::vector<JamRaceClassAvailability> (elements 0x60)
    struct { void* m_data; uint64_t m_size; uint64_t m_capacity; } m_raceClassAvailability;  // 0x3b8
    // TSGrowableArray<CharacterTemplateSet> (elements 0x90)
    struct { uint32_t m_count; uint32_t m_pad; void* m_data; uint32_t m_alloc; uint32_t m_pad2; uint32_t m_chunk; uint32_t m_pad3; } m_characterTemplates;  // 0x3d0
    uint8_t m_unk3f0;                   // 0x3f0
    uint8_t m_pad3f1[7];
    int64_t m_nextPingTime;             // 0x3f8: unix seconds
    uint32_t m_unk400;                  // 0x400
    uint32_t m_pad404;
    uint64_t m_unk408;                  // 0x408

    RealmConnection();
    ~RealmConnection() override;                                            // 0x00
    void Poll() override;                                                   // 0x60
    void HandleConnect() override;                                          // 0x70
    bool WriteAuthData(CDataStore* store) override;                         // 0x88
    virtual void HandleAuthResponse(const NETMESSAGEARGS* args);            // 0x90
    virtual void HandleStatusCode(const NETMESSAGEARGS* args);              // 0x98
    virtual void HandleWaitQueueUpdate(const NETMESSAGEARGS* args);         // 0xa0
    virtual void HandleWaitQueueFinish(const NETMESSAGEARGS* args) = 0;     // 0xa8
    virtual void HandleCharEnum(const NETMESSAGEARGS* args) = 0;            // 0xb0
    virtual void HandleUpdateExpansionLevel(const NETMESSAGEARGS* args);    // 0xb8
    virtual void HandleUpdateGameTimeState(const NETMESSAGEARGS* args);     // 0xc0
    virtual void HandleLobbyMatchmakerPartyInfo(const NETMESSAGEARGS* args);            // 0xc8
    virtual void HandleLobbyMatchmakerPartyInviteRejected(const NETMESSAGEARGS* args);  // 0xd0
    virtual void HandleClientLobbyMatchmakerReceiveInvite(const NETMESSAGEARGS* args);  // 0xd8
    virtual void HandleClientNewDataBuild(const NETMESSAGEARGS* args);      // 0xe0
    virtual void HandleOptionalTime(const NETMESSAGEARGS* args);            // 0xe8
    virtual void HandleCharacterCreate(uint8_t result, const void* a3) = 0; // 0xf0
    virtual void HandleCharacterDelete(uint8_t result) = 0;                 // 0xf8
    virtual void HandleCharacterLoginFailed(uint32_t code) = 0;             // 0x100
    virtual void HandleLogoutComplete() = 0;                                // 0x108
    virtual void HandleLogoutResponse(int32_t result, bool instant) = 0;    // 0x110
    virtual void HandleLogoutAbortAck() = 0;                                // 0x118

    // An auth response with an error code, made into a failed HandleAuthResponse.
    void HandleAuthFailure(const NETMESSAGEARGS* args);
    // Each realm in a realm query response reported through the realm-name callback.
    void HandleRealmQueryResponse(const NETMESSAGEARGS* args);
};
static_assert(offsetof(RealmConnection, m_loggedIn) == 0x368, "RealmConnection layout");
static_assert(offsetof(RealmConnection, m_gameTimeState) == 0x374, "RealmConnection layout");
static_assert(offsetof(RealmConnection, m_expansion) == 0x388, "RealmConnection layout");
static_assert(offsetof(RealmConnection, m_unk38c) == 0x38c, "RealmConnection layout");
static_assert(offsetof(RealmConnection, m_virtualRealms) == 0x398, "RealmConnection layout");
static_assert(offsetof(RealmConnection, m_raceClassAvailability) == 0x3b8, "RealmConnection layout");
static_assert(offsetof(RealmConnection, m_characterTemplates) == 0x3d0, "RealmConnection layout");
static_assert(offsetof(RealmConnection, m_nextPingTime) == 0x3f8, "RealmConnection layout");
static_assert(sizeof(RealmConnection) == 0x410, "RealmConnection is 0x410 bytes");

// The callbacks other subsystems give the realm connection.
// The auth data a session sends (WriteAuthData): a pointer and a length.
struct REALMAUTHDATA {
    const void* m_data;
    uint32_t m_size;
};
void RealmConnectionSetRealmNameCallback(void (*callback)(const void* realm, const void* name, const char* suffix,
    uint32_t id));
void RealmConnectionSetStatusCallback(void (*callback)(uint32_t code));
void RealmConnectionSetAuthDataCallback(const REALMAUTHDATA* (*callback)());
void RealmConnectionSetMessageCallback(void (*callback)(const void* message));
void RealmConnectionSetServerTimeCallback(void (*callback)(int64_t serverTime));
void RealmConnectionSetAuthValueCallback(void (*callback)(uint32_t value));

#endif
