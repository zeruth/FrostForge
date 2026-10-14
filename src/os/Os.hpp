#ifndef OS_OS_HPP
#define OS_OS_HPP

#include "blz/Function.hpp"
#include <cstdint>

// Thread priority levels, an index into the platform's priority table. 0 keeps the thread's
// current priority; 1..7 run from idle to time-critical.
enum OS_THREAD_PRIORITY : int32_t {
    OS_THREAD_PRIORITY_CURRENT = 0,
    OS_THREAD_PRIORITY_IDLE = 1,
    OS_THREAD_PRIORITY_LOWEST = 2,
    OS_THREAD_PRIORITY_BELOW_NORMAL = 3,
    OS_THREAD_PRIORITY_NORMAL = 4,
    OS_THREAD_PRIORITY_ABOVE_NORMAL = 5,
    OS_THREAD_PRIORITY_HIGHEST = 6,
    OS_THREAD_PRIORITY_TIME_CRITICAL = 7,
};

bool OsHasOversizedAddressSpace();

void OsSetCurrentThreadPriority(int32_t level);

void OsCheckSingleInstance(const char* name);

bool OsIsFirstInstance();

void OsLogCpuInfo();

// The OS version, read once (0x98 bytes, FUN_140263b70 over FUN_140266d20).
struct OSVERSION {
    uint32_t m_platform;            // 0x00: 10 Windows
    uint32_t m_major;               // 0x04
    uint32_t m_minor;               // 0x08
    uint32_t m_servicePack;         // 0x0c
    uint32_t m_build;               // 0x10
    uint8_t m_server;               // 0x14: not a workstation edition
    uint8_t m_wine;                 // 0x15: running under Wine
    char m_extra[0x82];             // 0x16: the service pack text (UTF-8)
};
static_assert(sizeof(OSVERSION) == 0x98, "OSVERSION is 0x98 bytes");
const OSVERSION* OsGetVersion();

// The version as text into `dest` (bc's formatter, FUN_140263c50).
void OsFormatVersion(char* dest, uint64_t size, const OSVERSION* version);

// AC power and battery (0x20 bytes, FUN_140266fd0). Each flags byte: 1 queried, 2 known.
struct OSPOWERSTATUS {
    uint8_t m_onAc;                 // 0x00
    uint8_t m_acFlags;              // 0x01
    uint8_t m_batteryPresent;       // 0x02
    uint8_t m_batteryFlags;         // 0x03
    uint32_t m_batteryPercent;      // 0x04
    uint8_t m_percentFlags;         // 0x08
    uint8_t m_pad09[7];
    uint64_t m_batteryLifeNs;       // 0x10
    uint8_t m_lifeFlags;            // 0x18
    uint8_t m_pad19[7];
};
void OsGetPowerStatus(OSPOWERSTATUS* status);

// The machine's physical memory in bytes (FUN_141236ff0).
uint64_t OsGetPhysicalMemory();

// Whether the process runs in a Remote Desktop session (FUN_140267270).
bool OsIsRemoteSession();

// A message box with UTF-8 text: 0 for OK or Yes, 1 for No, 2 otherwise (style: buttons in the
// low nibble, the icon in 0xf0000).
uint32_t OsMessageBox(void* owner, uint32_t style, const char* text, const char* caption);

void OsInitializeExceptionHandling();

void OsRecordThreadEntryReturnAddresses();

void OsSetMainReturnAddress(void* returnAddress);

void OsGetExecutableDirectory(char* buffer, uint64_t size);

void OsPathStripFilename(char* path);

bool OsSetCurrentDirectory(const char* path);

void OsGetParentProcessName(char* buffer);

uint32_t OsGetCurrentThreadId();

bool OsCreateDirectory(const char* path, bool createParents, bool failIfExists);
// Deletes a file (UTF-8 path).
// ref: FUN_1402148e0
bool OsDeleteFile(const char* path);

uint32_t OsPathRootLength(const char* path);

bool OsGetWorkingDirectory(uint64_t size, char* buffer);

void OsPathCombine(char* dest, uint64_t destSize, const char* base, const char* relative);

bool OsPathIsAbsolute(const char* path);

uint32_t OsGetCurrentDirectory(char* buffer, uint64_t size);

void OsPathNormalize(char* dest, uint64_t destSize, const char* source);

void OsPathJoin(char* dest, uint64_t destSize, const char* base, const char* relative);
// Copies at most `count` characters (stopping at the terminator) and at most
// destSize - 1 of them; returns how many the source had.
uint64_t OsStrLCopyN(char* dest, uint64_t destSize, const char* source, uint64_t count);
// The directory part of a path, without its last separator; empty when there is none.
void OsPathGetDirectory(char* dest, uint64_t destSize, const char* path);
// The part of a path after its last separator (the whole path when there is none).
const char* OsPathGetFile(const char* path);

// Creates a directory, and with `recursive` every missing parent; an OS error code or 0.
uint32_t OsCreateDirectoryTree(const char* path, bool recursive);

// A directory iterator (0x360 bytes): the find data, its handle, the current UTF-8 name, and
// whether the directory exists but is empty.
struct OSDIRITER {
    uint8_t m_find[0x250];      // WIN32_FIND_DATAW
    void* m_handle;             // 0x250
    char m_name[0x104];         // 0x258
    uint8_t m_empty;            // 0x35c
};
struct OSDIRENTRY {
    const char* m_name;
    uint8_t m_directory;
};
void OsDirInit(OSDIRITER* iter);
uint32_t OsDirOpen(OSDIRITER* iter, const char* path);
bool OsDirNext(OSDIRITER* iter, OSDIRENTRY* entry);
void OsDirClose(OSDIRITER* iter);
bool OsDirExists(const char* path);
// True for an existing file (not a directory).
bool OsFileExists(const char* path);

const char* OsPathExtension(const char* path);
void OsPathStripExtension(char* path);

uint32_t OsErrorFromResult(int32_t result);
void OsSleepNanoseconds(int64_t nanoseconds);
uint32_t OsGetProcessId();
// Ends the process at once (exit code 1).
void OsTerminateProcess();
void OsSetProcessAffinity(uint32_t mask);
uint64_t OsGetAllocationGranularity();
int32_t OsSnprintf(char* dest, uint64_t size, const char* format, ...);

void OsPathToNative(const char* source, char* dest, int32_t destSize);

uint64_t OsStrLCopy(char* dest, uint64_t destSize, const char* source);

uint64_t OsStrLCat(char* dest, uint64_t destSize, const char* source);

uint32_t OsErrorFromCode(int32_t code);

bool OsFileReadAll(const char* path, uint8_t** data, uint32_t* size, int32_t a4, int32_t a5);

void OsFileFree(void* data);

// A parsed network address: 4 (family 1) or 16 (family 2) bytes. Family 3 holds an
// allocated host name in the first 8 bytes (SockAddr.h frees it).
struct OSNETADDRESS {
    uint8_t m_bytes[16];
    int32_t m_family;
};

bool OsParseIPv4(const char* text, uint32_t* address);
bool OsParseIPv6(const char* text, uint8_t address[16]);
bool OsParseAddress(OSNETADDRESS* address, const char* text);

// The user's locale as a four-character code read little-endian ("en-US" -> 'enUS'); enUS
// when Windows has none or it has no country part.
uint32_t OsGetUserLocaleCode();

// What OsFindFiles reports per file.
#define OS_FILE_READONLY  0x1
#define OS_FILE_HIDDEN    0x2
#define OS_FILE_DIRECTORY 0x10
struct OSFILEINFO {
    uint32_t m_size;         // low 32 bits
    uint32_t m_attributes;   // OS_FILE_*
    char m_name[0x108];
    const char* m_directory; // the dir argument
};

bool OsFindFiles(const char* dir, const char* pattern, blz::function_storage* callback, void* param, bool includeHidden);

// The invoker for an OsFindFiles callback held as a plain function pointer
// (bool (*)(OSFILEINFO*, void*) in the storage).
bool OsFindFilesInvokePointer(OSFILEINFO* info, void* param, void* storage);

// Cuts a path after its last separator (never into its root); no separator empties it.
void OsPathTruncateToDirectory(char* path);

// The last component's extension: after its last dot (OsPathGetExtension) or at it
// (OsPathGetDotExtension); the end of the path when there is none.
const char* OsPathGetExtension(const char* path);
const char* OsPathGetDotExtension(const char* path);
// Copies `path` with its extension replaced (an empty one removes it).
void OsPathChangeExtension(char* dest, uint64_t destSize, const char* path, const char* extension);
// The extension replaced by `extension` as given, dot included (FUN_140214020).
void OsPathReplaceExtension(char* dest, uint64_t destSize, const char* path, const char* extension);
// GetFullPathNameW over UTF-8; false (with the last error set) when it fails or does not fit.
bool OsGetFullPath(const char* path, char* dest, uint64_t destSize);
// FormatMessage text for an error (UTF-8, a trailing CR LF dropped).
char* OsGetErrorText(uint32_t error, char* dest, uint32_t destSize);
char* OsGetLastErrorText(char* dest, uint32_t destSize);

// The packages' thread layer (os/win/Thread.cpp).
struct OSTHREAD {
    void* m_handle;
    void (*m_function)(void* argument);  // cleared by the thread when it starts
    void* m_argument;
};
struct OSCPUINFO {
    uint32_t m_packages;
    uint32_t m_cores;
    uint32_t m_logical;   // +8: what the task scheduler sizes its pool by
    uint32_t m_unknown0C;
    uint64_t m_maxFrequency;  // +0x10: Hz
    uint32_t m_unknown18;
    uint32_t m_architecture;  // +0x1c: 1 x86_32, 2 x86_64, 3 arm32, 4 arm64, 5 WAsm_32
    uint8_t m_unknown20[0xA];
    uint8_t m_flags2A;
    uint8_t m_unknown2B[0x40];
    char m_brand[0x55];       // +0x6b: the processor's brand string
};
static_assert(sizeof(OSCPUINFO) == 0xC0, "OSCPUINFO is 0xc0 bytes");
int64_t OsGetNanoseconds();
void OsCpuDetectFeatures(OSCPUINFO* info);
bool OsCpuFrequencyFromPower(uint64_t* frequency);
void OsCpuDetectArchitecture(OSCPUINFO* info);
// The raw performance counter (FUN_140266c80) and a tick count in nanoseconds (FUN_140266bf0).
int64_t OsGetPerformanceCounter();
int64_t OsTicksToNanoseconds(uint64_t ticks);
// Sleeps to a millisecond short of `nanoseconds`, then spins out the rest (FUN_140263700).
void OsSleepPrecise(int64_t nanoseconds);
// Nanoseconds since the first call (QueryPerformanceCounter scaled through a double).
int64_t OsGetElapsedNanoseconds();
bool OsParkWait(const void* address, bool (*predicate)(void* context), void* context, int64_t timeoutNs);
bool OsUnpark(const void* address, bool all);
uint32_t OsThreadCreate(OSTHREAD* thread, void (*function)(void* argument), void* argument, uint32_t stackSize);
void OsThreadWaitStarted(OSTHREAD* thread);
void OsThreadJoin(OSTHREAD* thread);
uint32_t OsThreadSetAffinity(OSTHREAD* thread, uint64_t mask);
void OsThreadSetPriority(OSTHREAD* thread, int32_t level);
void OsThreadSetName(const char* name);
void OsGetProcessorTopology(OSCPUINFO* info);
const OSCPUINFO* OsGetCpuInfo();

// The packages' file layer (0x140265f30..): a handle in a struct, results as OS error codes
// (0 success).
struct OSFILE {
    void* m_handle;
};
// Open modes: 0 write (open or create), 1 read/write (open or create), 2 read (existing),
// 3 read/write (existing), 4 write (create always), 5 read/write (create always), 6 append
// write, 7 append read/write, 8 execute (open or create).
struct OSFILESTAT {
    uint32_t m_flags;   // 1 directory, 2 hidden, 4 read-only
    uint32_t m_unused;
    uint64_t m_size;
    int64_t m_created;  // nanoseconds since 1970
    int64_t m_accessed;
    int64_t m_written;
};
void OsFileInit(OSFILE* file);
uint32_t OsFileOpen(OSFILE* file, const char* path, int32_t mode, uint32_t flags = 0);
uint64_t OsFileGetSize(OSFILE* file);
uint32_t OsFileRead(OSFILE* file, void* buffer, uint32_t bytes, uint32_t* read);
void OsFileClose(OSFILE* file);
uint32_t OsFileDelete(const char* path);
uint32_t OsFileReadAt(OSFILE* file, void* buffer, uint32_t bytes, uint64_t offset, uint32_t* read);
uint32_t OsFileWrite(OSFILE* file, const void* buffer, uint32_t bytes, uint32_t* written);
uint32_t OsFileWriteAt(OSFILE* file, const void* buffer, uint32_t bytes, uint64_t offset, uint32_t* written);
uint64_t OsFileTell(OSFILE* file);
OSFILE* OsFileMove(OSFILE* dest, OSFILE* source);

// A read-only or writable view of part of a file (0x28 bytes).
struct OSMAPPING {
    void* m_view;           // 0x00
    uint64_t m_size;        // 0x08
    uint64_t m_adjust;      // 0x10: how far the view was moved back to the allocation granularity
    void* m_mapping;        // 0x18
    void* m_file;           // 0x20: a duplicate of the file handle
};
void OsMappingInit(OSMAPPING* mapping);
uint32_t OsMappingOpen(OSMAPPING* mapping, OSFILE* file, bool writable, uint64_t offset, uint64_t size);
uint32_t OsMappingClose(OSMAPPING* mapping);
uint32_t OsFileStat(const char* path, OSFILESTAT* stat);
int64_t OsFileTimeToNanoseconds(int64_t filetime);
uint64_t OsErrorString(char* buffer, uint64_t size, uint32_t error);

// Files are Win32 handles (INVALID_HANDLE_VALUE when not open).
void* OsCreateFile(const char* path, uint32_t access, uint32_t share, uint32_t disposition, uint32_t attributes);

void OsCloseFile(void* file);

uint64_t OsGetFileSize(void* file, uint64_t fallback);

bool OsReadFile(void* file, void* buffer, uint32_t bytes, uint32_t* read);

// A named page-file mapping (0x18 bytes).
struct OSSHAREDMEMORY {
    void* m_data;
    uint64_t m_size;
    void* m_mapping;
};
uint32_t OsSharedMemoryOpen(OSSHAREDMEMORY* memory, const char* name, int32_t mode, uint64_t size, bool* created);
uint32_t OsSharedMemoryClose(OSSHAREDMEMORY* memory);
bool OsSharedMemoryOpenNamed(OSSHAREDMEMORY* memory, const char* name, uint32_t id, uint32_t size, int32_t mode);
// ReadFile at an offset (an OVERLAPPED position on a synchronous handle).
bool OsReadFileAt(void* file, void* buffer, uint64_t offset, uint32_t bytes, uint32_t* read);
int64_t OsSetFilePointer(void* file, int64_t offset, uint32_t method);
bool OsWriteFile(void* file, const void* buffer, uint32_t bytes, uint32_t* written);
bool OsFlushFile(void* file);

#endif
