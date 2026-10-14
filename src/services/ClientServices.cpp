#include "services/ClientServices.hpp"
#include "client/Locale.hpp"
#include "client/LocaleTables.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Fnv.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <malloc.h>
#include <new>
#include <windows.h>

extern "C" {
// src/gen: the reference's constructor, translated (tools/translate.py --addr FUN_141569c00)
unsigned long long FUN_141569c00(unsigned long long memory, unsigned long long vtable);
int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);
}

namespace {

// DAT_142d09818: the build type (static value 2, live).
int32_t s_buildType = 2;

void* s_clientServices;     // DAT_1430d2b38: the ClientServices instance (0x860 bytes)
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

// The ClientServices constructor: the NetClient / RealmConnection base (0x1414f7080, which
// also installs the realm packet handlers), Logs/Connection.log, and a handler list.
// translated: FUN_141569c00
void* ClientServicesConstruct(void* memory, const void* vtable) {
    // The reference's constructor, translated (src/gen): RealmConnection and NetClient under it,
    // the connection object, the realm packet handlers, Logs/Connection.log. Nothing connects
    // here; a fault in it leaves the instance null as before.
    __try {
        return reinterpret_cast<void*>(FUN_141569c00(reinterpret_cast<unsigned long long>(memory),
            reinterpret_cast<unsigned long long>(vtable)));
    } __except (GenFault(0x141569c00ull, GetExceptionInformation())) {
        return nullptr;
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
        void* memory = SMemAlloc(0x860, 3, __FILE__, SStrHashFnv1a(__FILE__), 0x432, 0);
        s_clientServices = ClientServicesConstruct(memory, nullptr);
    }
    ConsoleCommandRegister("logout", &LogoutCommand, 5, nullptr, 0);
}

// ref: FUN_14156b750
void* ClientServicesGet() {
    return s_clientServices;
}

// Stores the callback ClientServices polls for whether the client is idle enough (+0x858).
// ref: FUN_14156dc90
void ClientServicesSetStatusCallback(void* services, bool (*callback)()) {
    *reinterpret_cast<bool (**)()>(static_cast<uint8_t*>(services) + 0x858) = callback;
}

// ref: FUN_14156b7f0
void ClientServicesShutdown() {
    s_logoutPending = 0;
    ConsoleCommandUnregister("logout");
    if (s_clientServices) {
        // a session that is up (+0x771) is ended with reason 0xd, then the instance deletes
        // itself; both are the reference's virtuals (translated, vtable 0x1427b5518)
        auto services = static_cast<uint8_t*>(s_clientServices);
        void** vtable = *reinterpret_cast<void***>(services);
        __try {
            if (services[0x771] != 0) {
                reinterpret_cast<void (*)(void*, uint32_t)>(vtable[0x58 / 8])(services, 0xd);
            }
            reinterpret_cast<void (*)(void*, uint32_t)>(vtable[0])(services, 1);
        } __except (GenFault(reinterpret_cast<unsigned long long>(vtable[0]), GetExceptionInformation())) {
        }
        s_clientServices = nullptr;
    }
    if (s_servicesHelper) {
        ::operator delete(s_servicesHelper, 8);
        s_servicesHelper = nullptr;
    }
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

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430d258a = (void*)&s_sessionUp;
extern "C" void* const gen_p_1430d258b = (void*)&s_state258b;
extern "C" void* const gen_p_1430d2ab8 = (void*)&s_auroraSuffix;
extern "C" void* const gen_p_1430d2b38 = (void*)&s_clientServices;
// ---- end of generated globals ----
