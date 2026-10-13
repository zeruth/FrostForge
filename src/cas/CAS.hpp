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
struct FILEHANDLE;
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
// The download rate, bytes a second (smoothed).
float CASGetBytesPerSecond();
// The last error as a class (7: not resident yet).
uint32_t CASGetErrorClass();
// The last error's code and kind.
uint32_t CASGetErrorCode(int32_t* kind);
// The last error as text, truncated to `size` (FUN_1402cdca0).
char* CASFormatLastError(char* dest, uint64_t size);

// Handlers for a hot-fix swap (given its status) and for a manifest change (given a name),
// keyed by who registered them (blz::vector<blz::pair<uint64_t, blz::function<...>>>).
void CASAddHotfixHandler(blz::function_storage* handler, uint64_t key);
void CASAddManifestHandler(blz::function_storage* handler, uint64_t key);
// Handlers run before a manifest swap (blz::function<void()>).
void CASAddPreSwapHandler(blz::function_storage* handler, uint64_t key);
void CASRemoveManifestHandlers(uint64_t key);
void CASRemovePreSwapHandlers(uint64_t key);

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

// An open game file (0x68 bytes): a loose file on disk, or a file in the build read through tact.
struct CASFILE {
    int32_t m_type;                 // 0x00: 1 a loose file, 2 a build file
    tact::FILEHANDLE* m_handle;     // 0x08
    union {
        void* m_file;               // 0x10: type 1, the OS handle
        CASINSTANCE* m_instance;    // 0x10: type 2
    };
    uint8_t m_ckey[16];             // 0x18
    char m_name[0x36];              // 0x28: the content-key name tact knows it by
    uint64_t m_position;            // 0x60
};
static_assert(sizeof(CASFILE) == 0x68, "CASFILE is 0x68 bytes");

// Called after every build-file read with the bytes read (DAT_142ccba50, its context
// DAT_142ccba60).
extern void (*g_casReadHook)(const uint8_t* ckey, uint32_t bytes, void* context);

// The name tact resolves a content key by: "%08X%04X:%08X" (content flags, locale bits,
// file data id) and the key in hex.
void CASFormatKeyName(char* dest, const uint8_t* ckey, uint32_t fileDataId, uint32_t localeFlags, uint32_t contentFlags);

// Opens a file by id: with `flags` 1 a loose file entered under that id (or its name) wins.
bool CASFileOpenById(int32_t fileDataId, CASFILE** file, uint64_t flags);
// Whether the keys a file is encrypted with are known (FUN_1402d2460, by FileDataID
// FUN_1402cd520, by path FUN_1402cd660): 1 yes, 0 a key is missing, 0x2100 no such file.
uint64_t CASInstanceCheckKeys(CASINSTANCE* instance, const char* name, blz::function_storage* check, uint32_t offset,
    uint32_t size);
uint64_t CASFileCheckKeysById(int32_t fileDataId, blz::function_storage* check, uint32_t offset, uint32_t size);
uint64_t CASFileCheckKeysByName(const char* path, blz::function_storage* check, uint32_t offset, uint32_t size,
    uint32_t flags);
bool CASFileOpenByKey(CASINSTANCE* instance, const char* name, const uint8_t* ckey, CASFILE** file);
uint64_t CASFileGetSize(CASFILE* file);
int64_t CASFileSetPosition(CASFILE* file, int64_t offset, uint32_t method);
bool CASFileRead(CASFILE* file, void* dest, uint32_t size);
bool CASFileReadEx(CASFILE* file, void* dest, uint32_t size, uint32_t* read, uint32_t flags, int32_t slot);
bool CASFileReadAt(CASINSTANCE* instance, CASFILE* file, void* dest, uint64_t offset, uint32_t size, uint32_t* read,
    uint8_t flags, int32_t slot);
void CASFileClose(CASFILE** file);
// Whether an id names a file (a loose one, or one the build knows).
bool CASFileExists(int32_t fileDataId);
// A file's FileDataID by name: a loose override first (flag 1), else the build manifest's.
int32_t CASFileGetIdByName(const char* name, uint32_t flags);
// A file's content key by FileDataID (FUN_1402cdfc0) or path (FUN_1402ce000).
bool CASFileGetContentKeyById(int32_t fileDataId, uint8_t* key);
bool CASFileGetContentKeyByName(const char* name, uint8_t* key, uint32_t flags);
// Reads a whole file by FileDataID into a new Storm block (FUN_1402cf090).
bool CASFileLoadById(int32_t fileDataId, uint8_t** data, uint32_t* size, uint64_t flags, int32_t allocFlags);
bool CASFileLoadByName(const char* name, uint8_t** data, uint32_t* size, uint64_t flags, int32_t allocFlags);
// Whether a named file can be loaded (flag 1: local overrides count).
bool CASFileExistsByName(const char* name, uint64_t flags);
void CASFileFree(void* data);
// 2 resident, 1 not resident (tact's 7), 0 unknown or failed.
uint8_t CASFileGetResidency(int32_t fileDataId, uint64_t size, uint32_t a3);

// 2 resident, 1 not, 0 unknown, for a span of a build file (by its content-key name).
uint8_t CASInstanceGetSpanResidency(CASINSTANCE* instance, const char* name, uint64_t offset, uint64_t size);
uint8_t CASFileGetSpanResidency(CASFILE* file, uint64_t offset, uint32_t size);
void CASFileGetResidentBytes(CASFILE* file, uint64_t* resident, uint64_t* total);
uint8_t CASGetResidencyByName(const char* name, uint64_t offset, uint32_t size, uint64_t flags);
uint8_t CASGetResidencyByKey(const uint8_t* ckey, uint32_t fileDataId, uint32_t localeFlags, uint32_t contentFlags);
bool CASFileOpenByCKey(const uint8_t* ckey, uint32_t fileDataId, uint32_t localeFlags, uint32_t contentFlags,
    CASFILE** file);
// Opens a file by path: with `flags` 1 a loose file entered under that name wins.
bool CASFileOpenByName(const char* name, CASFILE** file, uint64_t flags);
// Reads at an offset; a null destination makes the span resident. `flags` 8: only when
// resident; 0x10: the read is kept in `slot` so it can be cancelled.
bool CASFileReadAtOffset(CASFILE* file, void* dest, uint32_t offset, uint32_t size, uint32_t* read, uint32_t flags,
    int32_t slot);
void CASCancelSlot(int32_t slot);
void CASClearSlot(int32_t slot);
bool CASLastErrorIsNotResident();
bool CASLastErrorWasCanceled();
char* CASFormatError(int32_t kind, uint32_t code, char* dest, uint64_t size);
bool CASFileGetMetaData(CASFILE** files, uint32_t* residency, uint64_t* containers, uint64_t* offsets, uint64_t count);

// The build manifest's file data id for a path, or 0.
int32_t CASGetFileDataIdByName(const char* name);

// Steps an in-place build update (DAT_142e20868) from the main thread.
void CASPollUpdate();

void CASShutdown();

}

#endif
