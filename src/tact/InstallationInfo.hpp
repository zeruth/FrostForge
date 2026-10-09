#ifndef TACT_INSTALLATION_INFO_HPP
#define TACT_INSTALLATION_INFO_HPP

// pkg/tact/3.6.39/src/installation_info.cpp, psv/psv_build_info.cpp: an installation's
// .build.info (one row per installed product build) and .flavor.info.

#include "tact/Psv.hpp"

namespace tact {

// One .build.info row (0xb0 bytes).
struct BUILDINFOENTRY {
    const char* m_product;       // 0x00 "Product"
    const char* m_branch;        // 0x08 "Branch"
    uint8_t m_active;            // 0x10 "Active"
    uint8_t m_buildKey[16];      // 0x11 "Build Key"
    uint8_t m_bgdlKey[16];       // 0x21 "BGDL Key"
    uint8_t m_cdnKey[16];        // 0x31 "CDN Key"
    uint8_t m_installKey[16];    // 0x41 "Install Key"
    uint8_t m_keyring[16];       // 0x51 "Keyring"
    uint8_t m_unknown61[7];      // 0x61
    const char* m_cdnPath;       // 0x68 "CDN Path"
    const char** m_cdnHosts;     // 0x70 "CDN Hosts"
    uint32_t m_cdnHostCount;     // 0x78
    const char** m_cdnServers;   // 0x80 "CDN Servers"
    uint32_t m_cdnServerCount;   // 0x88
    const char* m_tags;          // 0x90 "Tags"
    const char* m_armadillo;     // 0x98 "Armadillo"
    const char* m_lastActivated; // 0xa0 "Last Activated"
    const char* m_version;       // 0xa8 "Version"

    static void Init(BUILDINFOENTRY* entry);
};
static_assert(sizeof(BUILDINFOENTRY) == 0xB0, "BUILDINFOENTRY is 0xb0 bytes");

// One .flavor.info row: the product flavor ("wow_classic").
struct FLAVORINFOENTRY {
    const char* m_flavor;

    static void Init(FLAVORINFOENTRY* entry);
};

PSVMANIFEST* ParseBuildInfo(const char* text, uint32_t length);
PSVMANIFEST* ParseFlavorInfo(const char* text, uint32_t length);

// The active row for a product (null: the only product there is).
BUILDINFOENTRY* BuildInfoGetActive(PSVMANIFEST* buildInfo, const char* product);

struct INSTALLATIONINFO {
    PSVMANIFEST* m_buildInfo;
    PSVMANIFEST* m_flavorInfo;
    BUILDINFOENTRY* m_active;
};

// Reads an installation: a directory (its .flavor.info, then .build.info - one level up
// when there is a flavor) or a .build.info file. Never null; parts that could not be read
// stay null.
INSTALLATIONINFO* InstallationInfoRead(const char* path);
void InstallationInfoFree(INSTALLATIONINFO* info);

// Reads a whole file into a byte array; 0 or an OS error.
uint32_t ReadFileToArray(const char* path, Array<char>* array);

}

#endif
