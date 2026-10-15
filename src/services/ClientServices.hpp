#ifndef SERVICES_CLIENT_SERVICES_HPP
#define SERVICES_CLIENT_SERVICES_HPP

#include "blz/String.hpp"
#include "blz/Vector.hpp"
#include "net/RealmConnection.hpp"
#include "os/Os.hpp"
#include <cstddef>
#include <cstdint>

struct CVar;

// WoW/Source/WowSvcs/WowSvcsClient/ClientServices.cpp: the client's connection to the login
// and realm services.

// How the connection's state is reported to its listeners (ClientConnectionState).
enum ClientConnectionState : int32_t {
    CLIENT_CONNECTION_CONNECTED     = 0,
    CLIENT_CONNECTION_DISCONNECTED  = 1,
};

extern const char s_tagAuthListeners[];         // ".P6AX_N@Z"
extern const char s_tagStateListeners[];        // ".P6AXW4ClientConnectionState@@@Z"

// What ClientServices reports to the login screen (the glue). The live one is a static object
// (DAT_142d098e8, vtable 0x1427b50e8).
class ClientServicesResponse {
    public:
    virtual void Slot00();                                                  // 0x00
    virtual void AuthResult(int32_t result);                                // 0x08
    virtual void Slot10();                                                  // 0x10
    virtual void WaitQueueUpdate(uint32_t position, uint32_t minutes, bool hasFcm);  // 0x18
    virtual void WaitQueueStatus(uint32_t position, uint32_t minutes, bool hasFcm);  // 0x20
    virtual void CharacterCreateResult(const char* result, const void* guid);      // 0x28
    virtual void CharacterDeleteResult(const char* result);                 // 0x30
    virtual void CharacterLoginFailed(const char* result);                  // 0x38
};

// The client's session (0x860 bytes, vtable 0x1427b5518).
class ClientServices : public RealmConnection {
    public:
    // A connect address and its port (SockAddr.h).
    struct SERVERADDRESS {
        OSNETADDRESS m_address;         // 0x00: SockAddr.h clears 0x18 bytes
        uint32_t m_pad14;
        uint16_t m_port;                // 0x18
        uint16_t m_pad1a;

        ~SERVERADDRESS();
    };

    void* m_distribution;               // 0x410: the Battlepay distribution state (0x40)
    uint8_t m_unk418[0x138];            // 0x418
    SERVERADDRESS m_address;            // 0x550
    uint32_t m_pad56c;
    SERVERADDRESS m_fallbackAddress;    // 0x570: tried when the first can't connect
    uint32_t m_createTime;              // 0x58c (FrameTime)
    // The realm being joined (0x1e0): set by a connection change, used to reconnect.
    struct REALMJOIN {
        uint8_t m_unk000[0x1a0];
        SERVERADDRESS m_address;        // 0x1a0
        uint32_t m_pad1bc;
        SERVERADDRESS m_fallbackAddress;    // 0x1c0
        uint32_t m_pad1dc;

        ~REALMJOIN();
    } m_join;                           // 0x590
    uint8_t m_reconnect;                // 0x770
    uint8_t m_sessionUp;                // 0x771
    uint8_t m_unk772;                   // 0x772
    uint8_t m_unk773;                   // 0x773
    uint32_t m_pad774;
    ClientServicesResponse* m_response;     // 0x778
    blz::vector<void (*)(bool), s_tagAuthListeners> m_authListeners;  // 0x780
    blz::vector<void (*)(ClientConnectionState), s_tagStateListeners> m_stateListeners;  // 0x798
    uint8_t m_charEnum[0xa0];           // 0x7b0: the last character list, while m_hasCharEnum
    uint8_t m_hasCharEnum;              // 0x850
    uint8_t m_pad851[7];
    bool (*m_statusCallback)();         // 0x858

    explicit ClientServices(ClientServicesResponse* response);
    ~ClientServices() override;                                             // 0x00
    void Disconnect(uint32_t reason) override;                              // 0x58
    void Poll() override;                                                   // 0x60
    void HandleConnect() override;                                          // 0x70
    void HandleDisconnect() override;                                       // 0x78
    void HandleCantConnect() override;                                      // 0x80
    void HandleAuthResponse(const NETMESSAGEARGS* args) override;           // 0x90
    void HandleWaitQueueUpdate(const NETMESSAGEARGS* args) override;        // 0xa0
    void HandleWaitQueueFinish(const NETMESSAGEARGS* args) override;        // 0xa8
    void HandleCharEnum(const NETMESSAGEARGS* args) override;               // 0xb0
    void HandleUpdateExpansionLevel(const NETMESSAGEARGS* args) override;   // 0xb8
    void HandleUpdateGameTimeState(const NETMESSAGEARGS* args) override;    // 0xc0
    void HandleLobbyMatchmakerPartyInfo(const NETMESSAGEARGS* args) override;           // 0xc8
    void HandleLobbyMatchmakerPartyInviteRejected(const NETMESSAGEARGS* args) override; // 0xd0
    void HandleClientLobbyMatchmakerReceiveInvite(const NETMESSAGEARGS* args) override; // 0xd8
    void HandleClientNewDataBuild(const NETMESSAGEARGS* args) override;     // 0xe0
    void HandleOptionalTime(const NETMESSAGEARGS* args) override;           // 0xe8
    void HandleCharacterCreate(uint8_t result, const void* a3) override;    // 0xf0
    void HandleCharacterDelete(uint8_t result) override;                    // 0xf8
    void HandleCharacterLoginFailed(uint32_t code) override;                // 0x100
    void HandleLogoutComplete() override;                                   // 0x108
    void HandleLogoutResponse(int32_t result, bool instant) override;       // 0x110
    void HandleLogoutAbortAck() override;                                   // 0x118
};
static_assert(offsetof(ClientServices, m_distribution) == 0x410, "ClientServices layout");
static_assert(offsetof(ClientServices, m_address) == 0x550, "ClientServices layout");
static_assert(offsetof(ClientServices, m_fallbackAddress) == 0x570, "ClientServices layout");
static_assert(offsetof(ClientServices, m_createTime) == 0x58c, "ClientServices layout");
static_assert(offsetof(ClientServices, m_join) == 0x590, "ClientServices layout");
static_assert(sizeof(ClientServices::REALMJOIN) == 0x1e0, "REALMJOIN is 0x1e0 bytes");
static_assert(offsetof(ClientServices, m_sessionUp) == 0x771, "ClientServices layout");
static_assert(offsetof(ClientServices, m_response) == 0x778, "ClientServices layout");
static_assert(offsetof(ClientServices, m_authListeners) == 0x780, "ClientServices layout");
static_assert(offsetof(ClientServices, m_stateListeners) == 0x798, "ClientServices layout");
static_assert(offsetof(ClientServices, m_charEnum) == 0x7b0, "ClientServices layout");
static_assert(offsetof(ClientServices, m_hasCharEnum) == 0x850, "ClientServices layout");
static_assert(offsetof(ClientServices, m_statusCallback) == 0x858, "ClientServices layout");
static_assert(sizeof(ClientServices) == 0x860, "ClientServices is 0x860 bytes");

// The server's clock against ours (the realm connection's server-time callback).
void ClientServicesSetServerTime(int64_t serverTime);

// Which build this is: 0 PTR, 1 beta, 2 live, 3 internal.
int32_t GetBuildType();
void SetBuildType(int32_t type);

void ClientServicesInitialize();
void* ClientServicesGet();
bool ClientServicesExists();
// Adds a listener of the connection's state (FUN_14156d0e0).
void ClientServicesAddStateListener(void* services, void (*listener)(ClientConnectionState));
void ClientServicesSetStatusCallback(void* services, bool (*callback)());
void ClientServicesShutdown();

// The session state bytes (DAT_1430d2589..8b). The connection that sets them is not ported,
// so they stay clear and the client behaves as at the glue.
// ref: FUN_141567e20
bool ClientServicesIsInWorld();
// ref: FUN_141567e50
bool ClientServicesIsSessionUp();

// Sets the portal CVar from `region` (unless REGION_NONE); when the portal is still empty,
// fills it from the system's region and returns true with that region in *systemRegion.
// Also registers serverAlert and auroraClientRequestSuffix.
bool ClientServicesSetupPortal(uint32_t region, uint32_t* systemRegion);

// The portal name for a region, by build type.
const char* PortalForRegion(uint32_t region);

// A portal as a Battle.net host and port: "us" -> "us.actual.battle.net", port 1119 unless
// given as "host:port".
struct PORTALHOST {
    blz::string m_host;
    uint16_t m_port;
};
PORTALHOST* PortalGetHost(PORTALHOST* result, const char* portal);

// The region a portal host stands for (regionOverride CVar first).
uint32_t RegionFromPortalHost(PORTALHOST* host);

#endif
