#ifndef SERVICES_CLIENT_SERVICES_HPP
#define SERVICES_CLIENT_SERVICES_HPP

#include "blz/String.hpp"
#include <cstdint>

struct CVar;

// WoW/Source/WowSvcs/WowSvcsClient/ClientServices.cpp: the client's connection to the login
// and realm services. Its NetClient / RealmConnection base is not ported: the instance stays
// null, and the accessors below that the reference calls unconditionally are null-checked
// (see docs/stubs.md).

// Which build this is: 0 PTR, 1 beta, 2 live, 3 internal.
int32_t GetBuildType();
void SetBuildType(int32_t type);

void ClientServicesInitialize();
void* ClientServicesGet();
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
