#include "services/ClientServices.hpp"
#include "client/Locale.hpp"
#include "client/LocaleTables.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "os/Os.hpp"
#include "os/Timer.hpp"
#include "storm/Log.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Fnv.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <malloc.h>
#include <new>
#include <windows.h>

extern "C" {
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
unsigned long long FUN_1425825f0(unsigned long long);
unsigned long long FUN_1425827c0(unsigned long long);
unsigned long long FUN_14169fb00(unsigned long long);
unsigned long long FUN_1416a1750(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_1416a16a0(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_141569970(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_1415699d0(unsigned long long, unsigned long long);
unsigned long long FUN_14116d920(unsigned long long);
unsigned long long FUN_14156b870(unsigned long long, unsigned long long);
unsigned long long FUN_14156c960(unsigned long long);
unsigned long long FUN_14156c7d0(unsigned long long);
unsigned long long FUN_14156c850(unsigned long long);
unsigned long long FUN_14156c2b0(unsigned long long);
unsigned long long FUN_14156c0b0(unsigned long long, unsigned long long);
unsigned long long FUN_14156cc20(unsigned long long, unsigned long long);
unsigned long long FUN_14156cbc0(unsigned long long, unsigned long long);
unsigned long long FUN_14156c3a0(unsigned long long, unsigned long long);
unsigned long long FUN_14156ca70(unsigned long long, unsigned long long);
unsigned long long FUN_14156cb00(unsigned long long, unsigned long long);
unsigned long long FUN_14156c650(unsigned long long, unsigned long long);
unsigned long long FUN_14156c680(unsigned long long, unsigned long long);
unsigned long long FUN_14156c6b0(unsigned long long, unsigned long long);
unsigned long long FUN_14156c6e0(unsigned long long, unsigned long long);
unsigned long long FUN_14156cb80(unsigned long long, unsigned long long);
unsigned long long FUN_14156c410(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_14156c480(unsigned long long, unsigned long long);
unsigned long long FUN_14156c4f0(unsigned long long, unsigned long long);
unsigned long long FUN_14156c9f0(unsigned long long);
unsigned long long FUN_14156ca30(unsigned long long, unsigned long long, unsigned long long);
unsigned long long FUN_14156c9d0(unsigned long long);
}

const char s_tagAuthListeners[] = ".P6AX_N@Z";
const char s_tagStateListeners[] = ".P6AXW4ClientConnectionState@@@Z";

namespace {

const char s_clientServicesFile[] = "D:\\BuildServer\\A\\work-git\\wow\\WoW\\Source\\WowSvcs\\WowSvcsClient\\ClientServices.cpp";
const char s_sockAddrFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Os\\SockAddr.h";

// DAT_142d09818: the build type (static value 2, live).
int32_t s_buildType = 2;

ClientServices* s_clientServices;   // DAT_1430d2b38: the ClientServices instance (0x860 bytes)
HSLOG s_connectionLog;      // DAT_1430d2b40: Logs/Connection.log
int64_t s_serverTimeOffset; // DAT_1430d25b0: the server's clock minus ours, in seconds
ClientServicesResponse s_glueResponse;  // DAT_142d098e8
uint32_t s_logoutPending;   // DAT_1430d2680
void* s_servicesHelper;     // DAT_1430d2a90: an 8-byte object freed at shutdown
CVar* s_portal;             // DAT_1430d2a98
CVar* s_serverAlert;        // DAT_1430d2aa0
CVar* s_auroraSuffix;       // DAT_1430d2ab8
uint8_t s_state2589;        // DAT_1430d2589
uint8_t s_sessionUp;        // DAT_1430d258a
uint8_t s_state258b;        // DAT_1430d258b

// "logout": ends the session when one is up.
// ref: FUN_14156b7c0
int32_t LogoutCommand(const char* command, const char* arguments) {
    (void)command;
    (void)arguments;
    // calls 0x141567e50 (is a session up?) and 0x141567d90(0, 1, 0) (log out); neither is
    // ported, and with no ClientServices instance there is no session.
    FROST_UNIMPLEMENTED();
    return 1;
}

// The listener the constructor adds for auth results: on success it calls an empty function
// (0x1402112e0), on failure nothing.
// ref: 0x14156b360 (reached only through the pointer)
void AuthResultListener(bool success) {
    (void)success;
}

// The Battlepay distribution state ClientServices owns (WoW/Source/WowServices,
// BattlepayDistribution.cpp). Not ported: it runs translated.
// translated: FUN_1425825f0
void* DistributionConstruct(void* memory) {
    __try {
        return reinterpret_cast<void*>(FUN_1425825f0(reinterpret_cast<uintptr_t>(memory)));
    } __except (GenFault(0x1425825f0ull, GetExceptionInformation())) {
        return memory;
    }
}

// translated: FUN_1425827c0
void DistributionDestroy(void* distribution) {
    __try {
        FUN_1425827c0(reinterpret_cast<uintptr_t>(distribution));
    } __except (GenFault(0x1425827c0ull, GetExceptionInformation())) {
    }
}

// Whether a portal name is one of the development portals.
// ref: FUN_141567440
bool IsDevPortal(const blz::string* name) {
    for (const DEVPORTAL& portal : s_devPortals) {
        if (_stricmp(portal.m_name, name->m_data) == 0) {
            return true;
        }
    }
    return false;
}

// The region a development portal stands for (1 when it is not one).
// ref: FUN_1415673d0
uint32_t DevPortalRegion(const blz::string* name) {
    for (const DEVPORTAL& portal : s_devPortals) {
        if (_stricmp(portal.m_name, name->m_data) == 0) {
            return portal.m_region;
        }
    }
    return 1;
}

// A base-10 (or prefixed, with base 0) unsigned 16-bit parse with C library semantics:
// leading whitespace and a sign, overflow saturating to 0xFFFF and reported in *overflow.
// ref: FUN_14026ab40
uint16_t ParseUInt16(const char* text, const char** end, uint32_t base, bool* overflow) {
    char sign = *text;
    const char* p = text + 1;
    while (sign == ' ' || static_cast<uint8_t>(sign - 9) < 5) {
        text = p;
        sign = *p;
        p = text + 1;
    }
    char c = sign;
    if (((sign - 0x2B) & 0xFD) == 0) {
        c = *p;
        p = text + 2;
    }
    if (base == 0) {
        if (c != '0') {
            base = 10;
        } else if (((*p + 0xA8) & 0xDF) != 0) {
            base = 8;
        } else {
            base = 16;
            c = p[1];
            p += 2;
        }
    } else if (base == 16 && c == '0' && ((*p + 0xA8) & 0xDF) == 0) {
        c = p[1];
        p += 2;
    }

    bool over = false;
    uint16_t value = 0;
    uint16_t limit = static_cast<uint16_t>(0xFFFF / (base & 0xFFFF));
    for (;;) {
        uint32_t digit;
        if (static_cast<uint8_t>(c - '0') < 10) {
            digit = static_cast<uint32_t>(c - '0');
        } else if (static_cast<uint8_t>(c - 'a') < 0x1A) {
            digit = static_cast<uint32_t>(c - 0x57);
        } else if (static_cast<uint8_t>(c - 'A') < 0x1A) {
            digit = static_cast<uint32_t>(c - 0x37);
        } else {
            digit = 0xFFFFFFFF;
        }
        if (base <= digit) {
            break;
        }
        if (value < limit || (value == limit && digit <= 0xFFFF % base)) {
            value = static_cast<uint16_t>(base * value + digit);
        } else {
            over = true;
        }
        c = *p++;
    }
    if (end) {
        *end = p - 1;
    }
    if (over) {
        value = 0xFFFF;
    }
    if (overflow) {
        *overflow = over;
    }
    if (sign == '-' && !over) {
        value = static_cast<uint16_t>(-value);
    }
    return value;
}

// `stream >> port` on the text after "host:": whitespace skipped, an optional sign and the
// digits collected, then converted; no digits or overflow is a failure (port 0 for the
// former). The reference runs this through blz's istream over a string buffer.
// ref: FUN_141566920
bool ReadPort(const char* text, uint16_t* port) {
    while (*text == ' ' || static_cast<uint8_t>(*text - 9) <= 4) {
        text++;
    }
    if (*text == '\0') {
        return false;
    }
    blz::string token;
    token.assign("");
    auto append = [&](char c) {
        token.reserve(token.m_size + 1);
        token.m_data[token.m_size] = c;
        token.m_data[++token.m_size] = '\0';
    };
    if (((*text - 0x2B) & 0xFD) == 0) {
        append(*text++);
    }
    while (static_cast<uint8_t>(*text - '0') <= 9) {
        append(*text++);
    }
    const char* end;
    bool overflow;
    *port = ParseUInt16(token.m_data, &end, 10, &overflow);
    bool ok = true;
    if (token.m_size == 0 || end != token.m_data + token.m_size) {
        *port = 0;
        ok = false;
    }
    if (overflow) {
        ok = false;
    }
    return ok;
}

}

// ref: FUN_141566760
int32_t GetBuildType() {
    return s_buildType;
}

// ref: FUN_1415667e0
void SetBuildType(int32_t type) {
    s_buildType = type;
}

// Creates the ClientServices instance and registers "logout".
// ref: FUN_14156ce70
void ClientServicesInitialize() {
    if (!s_clientServices) {
        void* memory = SMemAlloc(sizeof(ClientServices), 3, s_clientServicesFile, SStrHashFnv1a(s_clientServicesFile),
            0x432, 0);
        s_clientServices = new (memory) ClientServices(&s_glueResponse);
    }
    ConsoleCommandRegister("logout", &LogoutCommand, 5, nullptr, 0);
}

// ref: FUN_14156b750
void* ClientServicesGet() {
    return s_clientServices;
}

// A listener told each change of the connection's state.
// ref: FUN_14156d0e0
void ClientServicesAddStateListener(void* services, void (*listener)(ClientConnectionState)) {
    static_cast<ClientServices*>(services)->m_stateListeners.push_back(listener);
}

// ref: FUN_14156cf10
bool ClientServicesExists() {
    return s_clientServices != nullptr;
}

// Stores the callback ClientServices polls for whether the client is idle enough.
// ref: FUN_14156dc90
void ClientServicesSetStatusCallback(void* services, bool (*callback)()) {
    static_cast<ClientServices*>(services)->m_statusCallback = callback;
}

// ref: FUN_14156b7f0
void ClientServicesShutdown() {
    s_logoutPending = 0;
    ConsoleCommandUnregister("logout");
    if (ClientServices* services = s_clientServices) {
        if (services->m_sessionUp) {
            services->Disconnect(0xd);
        }
        delete services;
        s_clientServices = nullptr;
    }
    if (s_servicesHelper) {
        ::operator delete(s_servicesHelper, 8);
        s_servicesHelper = nullptr;
    }
}

// ref: FUN_141567f30
void ClientServicesSetServerTime(int64_t serverTime) {
    s_serverTimeOffset = serverTime - OsGetUnixTime();
}

// ref: FUN_14156cd10
bool ClientServicesSetupPortal(uint32_t region, uint32_t* systemRegion) {
    s_portal = CVarLookup(t2util::ConstFnvHashStringI("portal"));
    if (region != REGION_NONE) {
        CVarSet(s_portal, PortalForRegion(region), true, false, false, true);
    }
    bool empty = *s_portal->m_stringValue.m_str == '\0';
    if (empty) {
        *systemRegion = RegionFromSystem();
        CVarSet(s_portal, PortalForRegion(RegionFromSystem()), true, false, false, true);
    }
    s_serverAlert = CVarRegister("serverAlert", "Get the glue-string tag for the URL", 0, "SERVER_ALERT_URL",
        static_cast<CVARCALLBACK>(nullptr), 6, false, nullptr, false);
    s_auroraSuffix = CVarRegister("auroraClientRequestSuffix", "Set the suffix used for Aurora client requests", 0x44,
        "wotlk1", static_cast<CVARCALLBACK>(nullptr), 6, false, nullptr, false);
    return empty;
}

// ref: FUN_14156bcf0
const char* PortalForRegion(uint32_t region) {
    int32_t type = GetBuildType();
    if (type == 0) {
        return "test";
    }
    if (type == 1) {
        return region == 5 ? "cn-test" : "test";
    }
    if (type == 2) {
        if (static_cast<int32_t>(region) < 0x100) {
            return s_regionNames[region] ? s_regionNames[region] : "";
        }
    } else if (type == 3) {
        return "wow-dev";
    }
    return "";
}

// ref: FUN_141566cb0
PORTALHOST* PortalGetHost(PORTALHOST* result, const char* portal) {
    new (&result->m_host) blz::string();
    blz::string text;
    text.assign(portal);
    {
        blz::string copy;
        copy.construct(text.m_data, text.m_size);
        result->m_host = static_cast<blz::string&&>(copy);
    }
    result->m_port = 1119;

    if (text.m_size != 0) {
        // the last character that is not a digit; a ':' there splits off the port
        for (uint64_t i = text.m_size - 1;; i--) {
            if (static_cast<uint8_t>(text.m_data[i] - '0') > 9) {
                if (text.m_data[i] == ':') {
                    blz::string host;
                    host.construct(text.m_data, i < text.m_size ? i : text.m_size);
                    result->m_host = static_cast<blz::string&&>(host);
                    if (i + 1 < text.m_size) {
                        ReadPort(text.m_data + i + 1, &result->m_port);
                    }
                }
                break;
            }
            if (i == 0) {
                break;
            }
        }

        OSNETADDRESS address = {};
        if (!OsParseAddress(&address, result->m_host.m_data)) {
            // a name without a dot gets the Battle.net domain
            if (!std::strchr(result->m_host.m_data, '.')) {
                const char* suffix = IsDevPortal(&result->m_host) ? ".bgs.battle.net" : ".actual.battle.net";
                uint64_t length = std::strlen(suffix);
                uint64_t size = result->m_host.m_size + length;
                result->m_host.reserve(size);
                std::memcpy(result->m_host.m_data + result->m_host.m_size, suffix, length);
                result->m_host.m_data[size] = '\0';
                result->m_host.m_size = size;
            }
        }
    }
    return result;
}

// ref: FUN_14156bdb0
uint32_t RegionFromPortalHost(PORTALHOST* host) {
    const char* name = host->m_host.m_data;
    if (CVar* override = CVarLookup(t2util::ConstFnvHashStringI("regionOverride"))) {
        if (!override->m_defaultValue.m_str
            || std::strcmp(override->m_stringValue.m_str, override->m_defaultValue.m_str) != 0) {
            name = override->m_stringValue.m_str;
        }
    }

    if (const char* dot = std::strchr(name, '.')) {
        uint64_t length = static_cast<uint64_t>(dot - name);
        char* prefix = static_cast<char*>(_alloca(length + 1));
        std::memcpy(prefix, name, length);
        prefix[length] = '\0';
        name = prefix;
    }
    if (!name || *name == '\0') {
        return 1;
    }

    for (uint32_t i = 1; i < 0x100; i++) {
        if (!s_regionNames[i]) {
            break;
        }
        if (_stricmp(name, s_regionNames[i]) == 0) {
            return i;
        }
    }
    if (_stricmp(name, "test") == 0) {
        return 0x48;
    }
    if (std::strncmp(name, "cn", 2) == 0) {
        return 5;
    }
    blz::string portal;
    portal.assign(name);
    if (IsDevPortal(&portal)) {
        blz::string again;
        again.assign(name);
        return DevPortalRegion(&again);
    }
    return 1;
}

// ref: FUN_141567e20
bool ClientServicesIsInWorld() {
    return s_state2589 != 0 && s_state258b != 0;
}

// ref: FUN_141567e50
bool ClientServicesIsSessionUp() {
    return s_sessionUp != 0;
}

// ---- ClientServicesResponse: the glue's side (vtable 0x1427b50e8) ----

namespace {

// The login screen's handlers ClientServicesResponse calls (GlueLogin.cpp, CharacterCreate):
// not ported yet, they run translated.

// translated: FUN_14169fb00
void GlueLoginAuthResult(int32_t result) {
    __try {
        FUN_14169fb00(static_cast<uint32_t>(result));
    } __except (GenFault(0x14169fb00ull, GetExceptionInformation())) {
    }
}

// translated: FUN_1416a1750
void GlueLoginWaitQueueUpdate(uint32_t position, uint32_t minutes, bool hasFcm) {
    __try {
        FUN_1416a1750(position, minutes, hasFcm);
    } __except (GenFault(0x1416a1750ull, GetExceptionInformation())) {
    }
}

// translated: FUN_1416a16a0
void GlueLoginWaitQueueStatus(uint32_t position, uint32_t minutes, bool hasFcm) {
    __try {
        FUN_1416a16a0(position, minutes, hasFcm);
    } __except (GenFault(0x1416a16a0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14116d920
void GlueCharacterLoginFailed(const char* result) {
    __try {
        FUN_14116d920(reinterpret_cast<uintptr_t>(result));
    } __except (GenFault(0x14116d920ull, GetExceptionInformation())) {
    }
}

}

// Slots 0x00 and 0x10 are empty (_guard_check_icall).
void ClientServicesResponse::Slot00() {
}

// ref: 0x141569940 (reached only through the pointer)
void ClientServicesResponse::AuthResult(int32_t result) {
    GlueLoginAuthResult(result);
}

void ClientServicesResponse::Slot10() {
}

// ref: 0x141569950 (reached only through the pointer)
void ClientServicesResponse::WaitQueueUpdate(uint32_t position, uint32_t minutes, bool hasFcm) {
    GlueLoginWaitQueueUpdate(position, minutes, hasFcm);
}

// ref: 0x141569960 (reached only through the pointer)
void ClientServicesResponse::WaitQueueStatus(uint32_t position, uint32_t minutes, bool hasFcm) {
    GlueLoginWaitQueueStatus(position, minutes, hasFcm);
}

// translated: FUN_141569970
void ClientServicesResponse::CharacterCreateResult(const char* result, const void* guid) {
    __try {
        FUN_141569970(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(result),
            reinterpret_cast<uintptr_t>(guid));
    } __except (GenFault(0x141569970ull, GetExceptionInformation())) {
    }
}

// translated: FUN_1415699d0
void ClientServicesResponse::CharacterDeleteResult(const char* result) {
    __try {
        FUN_1415699d0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(result));
    } __except (GenFault(0x1415699d0ull, GetExceptionInformation())) {
    }
}

// ref: 0x141569a20 (reached only through the pointer)
void ClientServicesResponse::CharacterLoginFailed(const char* result) {
    if (result != nullptr && *result != '\0') {
        GlueCharacterLoginFailed(result);
    }
}

// ---- ClientServices ----

// ref: FUN_141569c00
ClientServices::ClientServices(ClientServicesResponse* response) {
    std::memset(&this->m_address.m_address, 0, 0x18);
    this->m_address.m_port = 0;
    std::memset(&this->m_fallbackAddress.m_address, 0, 0x18);
    this->m_fallbackAddress.m_port = 0;
    this->m_createTime = FrameTime_GetTime();
    std::memset(&this->m_join.m_address.m_address, 0, 0x18);
    this->m_join.m_address.m_port = 0;
    std::memset(&this->m_join.m_fallbackAddress.m_address, 0, 0x18);
    this->m_join.m_fallbackAddress.m_port = 0;
    this->m_reconnect = 0;
    this->m_sessionUp = 0;
    this->m_unk772 = 0;
    this->m_unk773 = 0;
    this->m_response = response;
    this->m_hasCharEnum = 0;
    OsCreateDirectory("Logs", false, false);
    SLogCreate("Logs/Connection.log", 0, &s_connectionLog);
    if (s_connectionLog) {
        SLogWrite(s_connectionLog, "Connection Initialize");
    }
    void* memory = SMemAlloc(0x40, 3, s_clientServicesFile, SStrHashFnv1a(s_clientServicesFile), 0x1fd, 0);
    this->m_distribution = DistributionConstruct(memory);
    this->m_authListeners.push_back(&AuthResultListener);
}

// The log is closed but its handle kept, as in the reference.
// ref: FUN_14156b1d0
ClientServices::~ClientServices() {
    if (void* distribution = this->m_distribution) {
        DistributionDestroy(distribution);
        ::operator delete(distribution, 0x40);
    }
    if (s_connectionLog) {
        SLogWrite(s_connectionLog, "Connection Destroy");
        SLogClose(s_connectionLog);
    }
    if (this->m_hasCharEnum) {
        // the held character list's own destructor (slot 0 of the object at +0x7b0)
        auto object = reinterpret_cast<void (***)(void*, uint32_t)>(this->m_charEnum);
        (**object)(this->m_charEnum, 0);
    }
    this->m_hasCharEnum = 0;
    // then the members' destructors: m_stateListeners, m_authListeners, m_join, the two addresses
}

// SockAddr.h's destructor: a family-3 address frees its host name, then all of it is cleared.
ClientServices::SERVERADDRESS::~SERVERADDRESS() {
    void* name = *reinterpret_cast<void**>(this->m_address.m_bytes);
    if (this->m_address.m_family == 3 && name != nullptr) {
        SMemFree(name, s_sockAddrFile, 0x27, 0);
    }
    std::memset(&this->m_address, 0, 0x18);
}

// The two addresses' destructors, the fallback first.
// ref: FUN_14156a110
ClientServices::REALMJOIN::~REALMJOIN() {
}

// The virtuals below are not ported yet: they run translated, each logging to Connection.log
// ("Handle Auth Response: %u", "Handle Char Enum", ...) and reporting to m_response.

// translated: FUN_14156b870
void ClientServices::Disconnect(uint32_t reason) {
    __try {
        FUN_14156b870(reinterpret_cast<uintptr_t>(this), reason);
    } __except (GenFault(0x14156b870ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c960
void ClientServices::Poll() {
    __try {
        FUN_14156c960(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x14156c960ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c7d0
void ClientServices::HandleConnect() {
    __try {
        FUN_14156c7d0(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x14156c7d0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c850
void ClientServices::HandleDisconnect() {
    __try {
        FUN_14156c850(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x14156c850ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c2b0
void ClientServices::HandleCantConnect() {
    __try {
        FUN_14156c2b0(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x14156c2b0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c0b0
void ClientServices::HandleAuthResponse(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156c0b0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156c0b0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156cc20
void ClientServices::HandleWaitQueueUpdate(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156cc20(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156cc20ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156cbc0
void ClientServices::HandleWaitQueueFinish(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156cbc0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156cbc0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c3a0
void ClientServices::HandleCharEnum(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156c3a0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156c3a0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156ca70
void ClientServices::HandleUpdateExpansionLevel(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156ca70(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156ca70ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156cb00
void ClientServices::HandleUpdateGameTimeState(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156cb00(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156cb00ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c650
void ClientServices::HandleLobbyMatchmakerPartyInfo(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156c650(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156c650ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c680
void ClientServices::HandleLobbyMatchmakerPartyInviteRejected(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156c680(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156c680ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c6b0
void ClientServices::HandleClientLobbyMatchmakerReceiveInvite(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156c6b0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156c6b0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c6e0
void ClientServices::HandleClientNewDataBuild(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156c6e0(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156c6e0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156cb80
void ClientServices::HandleOptionalTime(const NETMESSAGEARGS* args) {
    __try {
        FUN_14156cb80(reinterpret_cast<uintptr_t>(this), reinterpret_cast<uintptr_t>(args));
    } __except (GenFault(0x14156cb80ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c410
void ClientServices::HandleCharacterCreate(uint8_t result, const void* a3) {
    __try {
        FUN_14156c410(reinterpret_cast<uintptr_t>(this), result, reinterpret_cast<uintptr_t>(a3));
    } __except (GenFault(0x14156c410ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c480
void ClientServices::HandleCharacterDelete(uint8_t result) {
    __try {
        FUN_14156c480(reinterpret_cast<uintptr_t>(this), result);
    } __except (GenFault(0x14156c480ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c4f0
void ClientServices::HandleCharacterLoginFailed(uint32_t code) {
    __try {
        FUN_14156c4f0(reinterpret_cast<uintptr_t>(this), code);
    } __except (GenFault(0x14156c4f0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c9f0
void ClientServices::HandleLogoutComplete() {
    __try {
        FUN_14156c9f0(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x14156c9f0ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156ca30
void ClientServices::HandleLogoutResponse(int32_t result, bool instant) {
    __try {
        FUN_14156ca30(reinterpret_cast<uintptr_t>(this), static_cast<uint32_t>(result), instant);
    } __except (GenFault(0x14156ca30ull, GetExceptionInformation())) {
    }
}

// translated: FUN_14156c9d0
void ClientServices::HandleLogoutAbortAck() {
    __try {
        FUN_14156c9d0(reinterpret_cast<uintptr_t>(this));
    } __except (GenFault(0x14156c9d0ull, GetExceptionInformation())) {
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_14116d920=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141569970=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1415699d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156b870=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c0b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c2b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c3a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c410=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c480=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c4f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c650=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c680=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c6b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c6e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c7d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c850=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c960=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c9d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156c9f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156ca30=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156ca70=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156cb00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156cb80=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156cbc0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14156cc20=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14169fb00=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1416a16a0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1416a1750=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1425825f0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1425827c0=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_14026ab40 = (void*)&ParseUInt16;
extern "C" void* const gen_f_141566920 = (void*)&ReadPort;
extern "C" void* const gen_p_1430d2589 = (void*)&s_state2589;
extern "C" void* const gen_p_1430d258a = (void*)&s_sessionUp;
extern "C" void* const gen_p_1430d258b = (void*)&s_state258b;
extern "C" void* const gen_p_1430d25b0 = (void*)&s_serverTimeOffset;
extern "C" void* const gen_p_1430d2a98 = (void*)&s_portal;
extern "C" void* const gen_p_1430d2ab8 = (void*)&s_auroraSuffix;
extern "C" void* const gen_p_1430d2b38 = (void*)&s_clientServices;
extern "C" void* const gen_p_1430d2b40 = (void*)&s_connectionLog;
// ---- end of generated globals ----
