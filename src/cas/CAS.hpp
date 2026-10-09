#ifndef CAS_CAS_HPP
#define CAS_CAS_HPP

// Engine/Source/CAS/CAS.cpp: the client's content-addressable storage front. It brings up a
// tact instance on the install's active build (.build.info), reads the build manifest
// (".root") through it, and keeps the local manifest of loose files beside it.
//
// The secondary instances of the patch path (DAT_142e20868 and the vector at
// DAT_142ccba08, live while DAT_142e20881 is set) are not ported; nothing here creates them.

#include "blz/Function.hpp"
#include "cas/CASManifest.hpp"
#include <cstdint>

class CStatus;

namespace blz {
class string;
}

namespace tact {
class Instance;
struct BUILDINFOENTRY;
}

namespace cas {

struct LOCALMANIFEST;

// The download statistics tact reports through its callback (0x20 bytes).
struct CASSTATS {
    volatile long m_active;         // 0x00: reads in flight
    int64_t m_start;                // 0x08: when the current sample began
    uint32_t m_bytes;               // 0x10: bytes in the current sample
    float m_smoothing;              // 0x14: 0.7
    float m_rate;                   // 0x18: bytes a second, smoothed
};
static_assert(sizeof(CASSTATS) == 0x20, "CASSTATS is 0x20 bytes");

// 0x58 bytes (DAT_142e25570).
struct CASINSTANCE {
    tact::Instance* m_tact;         // 0x00
    CASSTATS* m_stats;              // 0x08
    uint64_t m_ops[3];              // 0x10: background reads, cancelled at the teardown
    int32_t m_28;                   // 0x28
    uint8_t m_noStreaming;          // 0x2c
    uint8_t m_noCdn;                // 0x2d: the build lists no CDN servers
    const char* m_storagePath;      // 0x30
    uint8_t m_buildKey[16];         // 0x38
    uint8_t m_cdnKey[16];           // 0x48
};
static_assert(sizeof(CASINSTANCE) == 0x58, "CASINSTANCE is 0x58 bytes");

// The key lookup tact calls for encrypted content: (key name, out key, key size).
typedef int64_t (*CASKEYCALLBACK)(uint64_t* keyName, void* key, uint32_t keySize);

// The last error of a CAS call, per thread: what kind it is, and the code of that kind.
struct CASERROR {
    int32_t m_kind;                 // TLS +0x2148: 0 tact, 1 and 3 m_code, 2 the OS's last error
    uint32_t m_code;                // TLS +0x214c
    uint8_t m_tactError;            // TLS +0x2150
};
CASERROR* CASGetLastError();
// The last error's code and kind.
uint32_t CASGetErrorCode(int32_t* kind);

// Handlers for a hot-fix swap (given its status) and for a manifest change (given a name),
// keyed by who registered them (blz::vector<blz::pair<uint64_t, blz::function<...>>>).
void CASAddHotfixHandler(blz::function_storage* handler, uint64_t key);
void CASAddManifestHandler(blz::function_storage* handler, uint64_t key);

extern CASINSTANCE* g_casInstance;       // DAT_142e25570
extern CASMANIFEST* g_casManifest;       // DAT_142e25568
extern LOCALMANIFEST* g_localManifest;   // DAT_142e20860

// Enables the Tact.log the next CASInitialize opens.
void CASSetLogEnabled(bool enabled);
void CASSetKeyCallback(CASKEYCALLBACK callback);

// The data folder (null or empty: "../Data", written back), the local manifest, the log.
bool CASInitialize(const char** dataPath, CStatus* status);

// Brings up the instance on `entry` and loads its build manifest; `manifestOnly` skips the
// instance (and so the manifest's data).
bool CASOpen(uint32_t patch, const MANIFESTLOCALE* locale, tact::BUILDINFOENTRY* entry, uint32_t a4,
    bool manifestOnly, CStatus* status);

// The whole client path: the active .build.info row must match `version` (as is, or as one of
// the WOW-<build>patch<version>_<branch> product names); `product` receives its product.
bool CASInitFromBuildInfo(uint32_t patch, const char* version, const MANIFESTLOCALE* locale, const char* dataPath,
    const char* localRoot, CStatus* status, blz::string* product, bool offline);

bool CASCreateInstance(CASINSTANCE* instance, const char* storagePath, tact::BUILDINFOENTRY* entry, CStatus* status,
    int32_t streaming, void* keyFunction, void* keyContext, CASSTATS* stats);

// Reads a whole file by name into a new block (`flags` 2: one more byte, zeroed; 8: only when
// resident). `allocFlags` is the block's SMemAlloc category.
bool CASReadFile(CASINSTANCE* instance, const char* name, uint8_t** data, uint32_t* size, uint32_t flags,
    int32_t allocFlags);

bool CASLoadManifest(const uint8_t* const* data, uint64_t size, const MANIFESTLOCALE* locale, CStatus* status);

// The build manifest's file data id for a path, or 0.
int32_t CASGetFileDataIdByName(const char* name);

void CASShutdown();

}

#endif
