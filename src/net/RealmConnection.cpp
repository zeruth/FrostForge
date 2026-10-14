#include "net/RealmConnection.hpp"
#include "os/Timer.hpp"
#include "storm/Memory.hpp"
#include <cstring>
#include <initializer_list>
#include <windows.h>

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_1414f7f60(unsigned long long);
unsigned long long FUN_1414f7650(unsigned long long, unsigned long long);
unsigned long long FUN_1414f7840(unsigned long long, unsigned long long);
unsigned long long FUN_1414f7fe0(unsigned long long, unsigned long long);
unsigned long long FUN_1414f77b0(unsigned long long, unsigned long long);
unsigned long long FUN_1414f7ec0(unsigned long long, unsigned long long);
unsigned long long FUN_1414f7300(unsigned long long);
unsigned long long FUN_14077b7c0(unsigned long long);
}

namespace {

const char s_tagRealmConnectionList[] = ".?AV?$TSList@UREALMCONNECTIONNODE@RealmConnection@@V?$TSGetLink@"
                                        "UREALMCONNECTIONNODE@RealmConnection@@@@$07@@";
const char s_tagVirtualRealms[] = ".?AV?$TSFixedArray@UJamVirtualRealmInfo@@$07@@";

void (*s_realmNameCallback)(const void*, const void*, const char*, uint32_t);  // DAT_1430affc8
const REALMAUTHDATA* (*s_authDataCallback)();                                 // DAT_1430affd0
void (*s_statusCallback)(uint32_t);                                           // DAT_1430affd8
void (*s_authValueCallback)(uint32_t);                                        // DAT_1430affe0
void (*s_serverTimeCallback)(int64_t);                                        // DAT_1430affe8
void (*s_messageCallback)(const void*);                                       // DAT_1430afff0

// The lock over the list of live realm connections, made and deleted by static initializers
// (0x1401ed680, and its atexit 0x1425eca91).
struct REALMCONNECTIONLOCK {
    SCritSect m_critSect;

    REALMCONNECTIONLOCK() {
        this->m_critSect.Initialize();
    }

    ~REALMCONNECTIONLOCK() {
        this->m_critSect.Destroy();
    }
};

REALMCONNECTIONLOCK s_realmConnectionLock;  // DAT_1430affa0
TSList<RealmConnection::REALMCONNECTIONNODE, TSGetLink<RealmConnection::REALMCONNECTIONNODE>, 8> s_realmConnections;  // DAT_142d08138

// The handlers RealmConnection installs in the generated message dispatch. Each takes the
// session from the message and calls the matching virtual.

// ref: 0x1414f8770 (reached only through the pointer)
void OnMessageForCallback(NETMESSAGEARGS* args) {
    if (s_messageCallback) {
        s_messageCallback(args->m_message);
    }
}

// ref: 0x1414f8090 (reached only through the pointer)
void OnAuthFailure(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleAuthFailure(args);
}

// ref: 0x1414f80a0 (reached only through the pointer)
void OnAuthResponse(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleAuthResponse(args);
}

// ref: 0x1414f8140 (reached only through the pointer)
void OnStatusCode(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleStatusCode(args);
}

// ref: 0x1414f8220 (reached only through the pointer)
void OnWaitQueueUpdate(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleWaitQueueUpdate(args);
}

// ref: 0x1414f8210 (reached only through the pointer)
void OnWaitQueueFinish(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleWaitQueueFinish(args);
}

// ref: 0x1414f80d0 (reached only through the pointer)
void OnCharacterCreate(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleCharacterCreate(args->Message<uint8_t>(0), args->m_message + 8);
}

// ref: 0x1414f80b0 (reached only through the pointer)
void OnCharacterLoginFailed(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleCharacterLoginFailed(*args->Message<uint8_t*>(0));
}

// ref: 0x1414f8190 (reached only through the pointer)
void OnLogoutComplete(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleLogoutComplete();
}

// ref: 0x1414f8180 (reached only through the pointer)
void OnLogoutAbortAck(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleLogoutAbortAck();
}

// ref: 0x1414f81a0 (reached only through the pointer)
void OnLogoutResponse(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleLogoutResponse(args->Message<int32_t>(0),
        args->Message<uint8_t>(4) != 0);
}

// ref: 0x1414f80f0 (reached only through the pointer)
void OnCharacterDelete(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleCharacterDelete(*args->Message<uint8_t*>(0));
}

// ref: 0x1414f8110 (reached only through the pointer)
void OnCharEnum(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleCharEnum(args);
}

// ref: 0x1414f81f0 (reached only through the pointer)
void OnUpdateExpansionLevel(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleUpdateExpansionLevel(args);
}

// ref: 0x1414f8200 (reached only through the pointer)
void OnUpdateGameTimeState(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleUpdateGameTimeState(args);
}

// ref: 0x1414f8120 (reached only through the pointer)
void OnRealmQueryResponse(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleRealmQueryResponse(args);
}

// A message the realm connection ignores (_guard_check_icall).
void OnIgnoredMessage(NETMESSAGEARGS* args) {
    (void)args;
}

// ref: 0x1414f81d0 (reached only through the pointer)
void OnServerTime(NETMESSAGEARGS* args) {
    if (s_serverTimeCallback) {
        s_serverTimeCallback(*args->Message<int64_t*>(0));
    }
}

// ref: 0x1414f8150 (reached only through the pointer)
void OnLobbyMatchmakerPartyInfo(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleLobbyMatchmakerPartyInfo(args);
}

// ref: 0x1414f8160 (reached only through the pointer)
void OnLobbyMatchmakerPartyInviteRejected(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleLobbyMatchmakerPartyInviteRejected(args);
}

// ref: 0x1414f8170 (reached only through the pointer)
void OnClientLobbyMatchmakerReceiveInvite(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleClientLobbyMatchmakerReceiveInvite(args);
}

// ref: 0x1414f81c0 (reached only through the pointer)
void OnClientNewDataBuild(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleClientNewDataBuild(args);
}

// ref: 0x1414f8130 (reached only through the pointer)
void OnOptionalTime(NETMESSAGEARGS* args) {
    static_cast<RealmConnection*>(args->m_client)->HandleOptionalTime(args);
}

// TSFixedArray<CharacterTemplateSet>'s storage freed with each set's own.
// translated: FUN_1414f7300
void CharacterTemplatesFree(void* templates) {
    __try {
        FUN_1414f7300(reinterpret_cast<uintptr_t>(templates));
    } __except (GenFault(0x1414f7300ull, GetExceptionInformation())) {
    }
}

// blz::vector<JamRaceClassAvailability>'s destructor.
// translated: FUN_14077b7c0
void RaceClassAvailabilityFree(void* vector) {
    __try {
        FUN_14077b7c0(reinterpret_cast<uintptr_t>(vector));
    } __except (GenFault(0x14077b7c0ull, GetExceptionInformation())) {
    }
}

}

// ref: FUN_1406d0290
GAMETIMESTATE::GAMETIMESTATE() {
    this->m_unk00 = 0;
    this->m_unk08 = 0;
    this->m_unk0a = 0;
    this->m_unk0c = 0;
}

// ref: FUN_1414f7080
RealmConnection::RealmConnection() {
    this->m_loggedIn = 0;
    this->m_queuePosition = 0;
    this->m_queueFlag = 0;
    this->m_expansion = 0;
    this->m_maxExpansion = 0;
    this->m_unk38a = 0;
    this->m_unk38c = 0;
    this->m_unk390 = 0;
    this->m_virtualRealms.m_count = 0;
    this->m_virtualRealms.m_data = nullptr;
    this->m_virtualRealms.m_alloc = 0;
    this->m_virtualRealms.m_chunk = 0;
    this->m_raceClassAvailability.m_data = nullptr;
    this->m_raceClassAvailability.m_size = 0;
    this->m_raceClassAvailability.m_capacity = 0;
    this->m_characterTemplates.m_count = 0;
    this->m_characterTemplates.m_data = nullptr;
    this->m_characterTemplates.m_alloc = 0;
    this->m_characterTemplates.m_chunk = 0;
    this->m_unk3f0 = 0;
    this->m_nextPingTime = OsGetUnixTime();
    this->m_unk408 = 0;
    this->m_unk400 = 0;
    this->NetClient::Initialize();

    NetMessageHandler(0x14309e9b8) = reinterpret_cast<void*>(&OnMessageForCallback);
    NetMessageHandler(0x14309ebc0) = reinterpret_cast<void*>(&OnAuthFailure);
    NetMessageHandler(0x14309ece8) = reinterpret_cast<void*>(&OnAuthResponse);
    NetMessageHandler(0x14309ee68) = reinterpret_cast<void*>(&OnStatusCode);
    NetMessageHandler(0x14309ed70) = reinterpret_cast<void*>(&OnWaitQueueUpdate);
    NetMessageHandler(0x14309ee00) = reinterpret_cast<void*>(&OnWaitQueueFinish);
    NetMessageHandler(0x14309d5a0) = reinterpret_cast<void*>(&OnCharacterCreate);
    NetMessageHandler(0x14309d7d0) = reinterpret_cast<void*>(&OnCharacterLoginFailed);
    NetMessageHandler(0x14309ebc8) = reinterpret_cast<void*>(&OnLogoutComplete);
    NetMessageHandler(0x14309ec38) = reinterpret_cast<void*>(&OnLogoutAbortAck);
    NetMessageHandler(0x14309eb58) = reinterpret_cast<void*>(&OnLogoutResponse);
    NetMessageHandler(0x14309d630) = reinterpret_cast<void*>(&OnCharacterDelete);
    NetMessageHandler(0x14309da90) = reinterpret_cast<void*>(&OnCharEnum);
    NetMessageHandler(0x14309f098) = reinterpret_cast<void*>(&OnUpdateExpansionLevel);
    NetMessageHandler(0x14309fba0) = reinterpret_cast<void*>(&OnUpdateGameTimeState);
    NetMessageHandler(0x14309f570) = reinterpret_cast<void*>(&OnRealmQueryResponse);
    NetMessageHandler(0x14309f628) = reinterpret_cast<void*>(&OnIgnoredMessage);
    NetMessageHandler(0x14309e4c8) = reinterpret_cast<void*>(&OnServerTime);
    NetMessageHandler(0x14309ecc8) = reinterpret_cast<void*>(&OnLobbyMatchmakerPartyInfo);
    NetMessageHandler(0x14309ed60) = reinterpret_cast<void*>(&OnLobbyMatchmakerPartyInviteRejected);
    NetMessageHandler(0x14309ee08) = reinterpret_cast<void*>(&OnClientLobbyMatchmakerReceiveInvite);
    NetMessageHandler(0x14309d638) = reinterpret_cast<void*>(&OnClientNewDataBuild);
    NetMessageHandler(0x14309d6d0) = reinterpret_cast<void*>(&OnOptionalTime);

    s_realmConnectionLock.m_critSect.Enter();
    auto node = static_cast<REALMCONNECTIONNODE*>(SMemAlloc(sizeof(REALMCONNECTIONNODE), 8, s_tagRealmConnectionList, 0,
        -2, SMEM_FLAG_ZEROMEMORY));
    node->m_link.m_prevlink = nullptr;
    node->m_link.m_next = nullptr;
    node->m_connection = nullptr;
    s_realmConnections.LinkToTail(node);
    node->m_connection = this;
    s_realmConnectionLock.m_critSect.Leave();
}

// Out of the list of live connections, the handlers taken back (not all the constructor set,
// and two it never set, as in the reference), the session destroyed, the realm data freed.
// ref: FUN_1414f73c0
RealmConnection::~RealmConnection() {
    s_realmConnectionLock.m_critSect.Enter();
    for (REALMCONNECTIONNODE* node = s_realmConnections.Head(); node != nullptr; node = s_realmConnections.Next(node)) {
        if (node->m_connection == this) {
            node->m_link.Unlink();
            SMemFree(node, s_tagRealmConnectionList, -2, 0);
            break;
        }
    }
    for (uintptr_t slot : { 0x14309e9b8ull, 0x14309ebc0ull, 0x14309ece8ull, 0x14309ed70ull, 0x14309ee00ull,
             0x14309d5a0ull, 0x14309d7d0ull, 0x14309ebc8ull, 0x14309ec38ull, 0x14309eb58ull, 0x14309d630ull,
             0x14309d198ull, 0x14309da90ull, 0x14309f098ull, 0x14309fba0ull, 0x14309e5a0ull, 0x14309e4c8ull,
             0x14309ecc8ull, 0x14309ed60ull }) {
        NetMessageHandler(slot) = nullptr;
    }
    this->NetClient::Destroy();
    s_realmConnectionLock.m_critSect.Leave();
    CharacterTemplatesFree(&this->m_characterTemplates);
    this->m_characterTemplates.m_alloc = 0;
    this->m_characterTemplates.m_count = 0;
    this->m_characterTemplates.m_data = nullptr;
    CharacterTemplatesFree(&this->m_characterTemplates);
    RaceClassAvailabilityFree(&this->m_raceClassAvailability);
    if (this->m_virtualRealms.m_data != nullptr) {
        SMemFree(this->m_virtualRealms.m_data, s_tagVirtualRealms, -2, 0);
    }
}

// The session's ping, every 90 seconds once logged in.
// translated: FUN_1414f7f60
void RealmConnection::Poll() {
    __try {
        FUN_1414f7f60(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x1414f7f60ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1414f7f50
void RealmConnection::HandleConnect() {
    this->m_loggedIn = 0;
    this->NetClient::HandleConnect();
}

// The auth data the auth-data callback hands out, written to the store.
// translated: FUN_1414f7650
bool RealmConnection::WriteAuthData(CDataStore* store) {
    __try {
        return (FUN_1414f7650(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(store)) & 0xff) != 0;
    } __except (GenFault(0x1414f7650ull, GetExceptionInformation())) {
        return false;
    }
}

// The realm data from an auth response (virtual realms, templates, availability, expansion).
// translated: FUN_1414f7840
void RealmConnection::HandleAuthResponse(const NETMESSAGEARGS* args) {
    __try {
        FUN_1414f7840(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x1414f7840ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1414f7fc0
void RealmConnection::HandleStatusCode(const NETMESSAGEARGS* args) {
    if (s_statusCallback) {
        s_statusCallback(*args->Message<uint32_t*>(0));
    }
}

// ref: FUN_1414f8070
void RealmConnection::HandleWaitQueueUpdate(const NETMESSAGEARGS* args) {
    if (this->m_loggedIn) {
        this->m_queuePosition = args->Message<uint32_t>(0);
        this->m_queueFlag = args->Message<uint8_t>(8);
    }
}

// The expansion levels (checked against what the account may play) and the race/class
// availability.
// translated: FUN_1414f7fe0
void RealmConnection::HandleUpdateExpansionLevel(const NETMESSAGEARGS* args) {
    __try {
        FUN_1414f7fe0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x1414f7fe0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1414f8060
void RealmConnection::HandleUpdateGameTimeState(const NETMESSAGEARGS* args) {
    std::memcpy(&this->m_gameTimeState, args->m_message, sizeof(GAMETIMESTATE));
}

// Slots 0xc8-0xe8 are empty in RealmConnection (_guard_check_icall); ClientServices fills them.
void RealmConnection::HandleLobbyMatchmakerPartyInfo(const NETMESSAGEARGS* args) {
    (void)args;
}

void RealmConnection::HandleLobbyMatchmakerPartyInviteRejected(const NETMESSAGEARGS* args) {
    (void)args;
}

void RealmConnection::HandleClientLobbyMatchmakerReceiveInvite(const NETMESSAGEARGS* args) {
    (void)args;
}

void RealmConnection::HandleClientNewDataBuild(const NETMESSAGEARGS* args) {
    (void)args;
}

void RealmConnection::HandleOptionalTime(const NETMESSAGEARGS* args) {
    (void)args;
}

// translated: FUN_1414f77b0
void RealmConnection::HandleAuthFailure(const NETMESSAGEARGS* args) {
    __try {
        FUN_1414f77b0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x1414f77b0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_1414f7ec0
void RealmConnection::HandleRealmQueryResponse(const NETMESSAGEARGS* args) {
    __try {
        FUN_1414f7ec0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x1414f7ec0ull, GetExceptionInformation())) {
    }
}

// ref: FUN_1414f8790
void RealmConnectionSetRealmNameCallback(void (*callback)(const void* realm, const void* name, const char* suffix,
    uint32_t id)) {
    s_realmNameCallback = callback;
}

// ref: FUN_1414f8930
void RealmConnectionSetStatusCallback(void (*callback)(uint32_t code)) {
    s_statusCallback = callback;
}

// ref: FUN_1414f8940
void RealmConnectionSetAuthDataCallback(const REALMAUTHDATA* (*callback)()) {
    s_authDataCallback = callback;
}

// ref: FUN_1414f8950
void RealmConnectionSetMessageCallback(void (*callback)(const void* message)) {
    s_messageCallback = callback;
}

// ref: FUN_1414f8960
void RealmConnectionSetServerTimeCallback(void (*callback)(int64_t serverTime)) {
    s_serverTimeCallback = callback;
}

// ref: FUN_1414f8970
void RealmConnectionSetAuthValueCallback(void (*callback)(uint32_t value)) {
    s_authValueCallback = callback;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430affc8 = (void*)&s_realmNameCallback;
extern "C" void* const gen_p_1430affd0 = (void*)&s_authDataCallback;
// ---- end of generated globals ----
