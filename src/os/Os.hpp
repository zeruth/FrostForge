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

void OsInitializeExceptionHandling();

void OsRecordThreadEntryReturnAddresses();

void OsSetMainReturnAddress(void* returnAddress);

void OsGetExecutableDirectory(char* buffer, uint64_t size);

void OsPathStripFilename(char* path);

bool OsSetCurrentDirectory(const char* path);

void OsGetParentProcessName(char* buffer);

uint32_t OsGetCurrentThreadId();

bool OsCreateDirectory(const char* path, bool createParents, bool failIfExists);

uint32_t OsPathRootLength(const char* path);

bool OsGetWorkingDirectory(uint64_t size, char* buffer);

void OsPathCombine(char* dest, uint64_t destSize, const char* base, const char* relative);

bool OsPathIsAbsolute(const char* path);

uint32_t OsGetCurrentDirectory(char* buffer, uint64_t size);

void OsPathNormalize(char* dest, uint64_t destSize, const char* source);

void OsPathJoin(char* dest, uint64_t destSize, const char* base, const char* relative);

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

const char* OsPathExtension(const char* path);
void OsPathStripExtension(char* path);

uint32_t OsErrorFromResult(int32_t result);
void OsSleepNanoseconds(int64_t nanoseconds);
uint32_t OsGetProcessId();
// Ends the process at once (exit code 1).
void OsTerminateProcess();
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
    uint8_t m_unknown0C[0x1E];
    uint8_t m_flags2A;
    uint8_t m_unknown2B[0x95];
};
int64_t OsGetNanoseconds();
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

#endif
