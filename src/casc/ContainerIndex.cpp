#include "casc/ContainerIndex.hpp"
#include "blz/Hex.hpp"
#include "blz/Log.hpp"
#include "casc/Dynamic.hpp"
#include "os/Os.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>
#include <malloc.h>
#include <windows.h>
#include <sddl.h>

namespace casc {

namespace {

const char s_indexFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic\\container_index.cpp";

struct LISTLINK {
    LISTLINK* m_next;
    LISTLINK* m_prev;
};

void ListReset(LISTLINK* head) {
    LISTLINK* node = head->m_next;
    while (node != head) {
        LISTLINK* next = node->m_next;
        node->m_next = node;
        node->m_prev = node;
        node = next;
    }
    head->m_next = head;
    head->m_prev = head;
}

void ListUnlink(LISTLINK* link) {
    link->m_prev->m_next = link->m_next;
    link->m_next->m_prev = link->m_prev;
    link->m_prev = link;
    link->m_next = link;
}

// "<message><error>" into a 0x100-byte buffer.
// ref: FUN_142149930
void FormatFatal(char text[0x100], const char* message, const uint32_t* error) {
    OsSnprintf(text, 0x100, "%s%u", message, *error);
}

// The global mutex helpers fail through the blz assertion reporter (a static record per site
// handed to 0x140262570 -> 0x140267440 -> 0x1402644b0), breaking into the debugger when it
// says so and otherwise terminating the process (0x1402651b0).
// ref: FUN_14214a010 (and its sibling sites FUN_14214a190, FUN_14214a230, FUN_14214a2d0)
// DIVERGED: the assertion reporter is not ported; the message goes to the casc log and the
// debugger break is taken when one is attached.
[[noreturn]] void Fatal(const char* message, uint32_t error) {
    char text[0x100];
    FormatFatal(text, message, &error);
    blz::log(g_logCategory, 4, s_indexFile, 0, "%s", text);
    if (IsDebuggerPresent()) {
        __debugbreak();
    }
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) {
    }
}

// The global mutex every process binding the container shares, created with a descriptor
// that lets everyone, including anonymous and low-integrity callers, use it.
// ref: FUN_14214ef30
void GlobalMutexCreate(HANDLE* mutex, const char* name) {
    SECURITY_ATTRIBUTES attributes;
    attributes.nLength = 0x18;
    attributes.bInheritHandle = FALSE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AN)S:(ML;;NW;;;ME)", 1,
            &attributes.lpSecurityDescriptor, nullptr)) {
        Fatal("Failed to create security descriptor from string. Error:", GetLastError());
    }
    uint64_t length = std::strlen(name);
    auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 2 + 0xF) & ~uint64_t(0xF)));
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 0x3FFFFFFF);
    for (uint32_t attempt = 0; attempt < 5; attempt++) {
        *mutex = CreateMutexW(&attributes, FALSE, wide);
        if (*mutex) {
            break;
        }
        uint32_t error = GetLastError();
        if (error != ERROR_ACCESS_DENIED) {
            Fatal("Failed to create global mutex. Error:", error);
        }
        *mutex = OpenMutexW(SYNCHRONIZE, FALSE, wide);
        if (*mutex) {
            break;
        }
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
            Fatal("Failed to open global mutex. Error:", error);
        }
    }
    if (*mutex) {
        LocalFree(attributes.lpSecurityDescriptor);
        return;
    }
    Fatal("Exhausted retries creating global mutex. LastError:", GetLastError());
}

bool Exclusive(int32_t mode) {
    return static_cast<uint32_t>(mode - 4) <= 1;
}

// Exclusive modes (4 and 5) have the container to themselves and take no lock.
// ref: FUN_14214ee50
bool MutexLock(HANDLE* mutex, int32_t mode) {
    if (!Exclusive(mode)) {
        DWORD wait = WaitForSingleObject(*mutex, INFINITE);
        if (wait != WAIT_OBJECT_0) {
            if (wait != WAIT_ABANDONED) {
                if (wait == WAIT_TIMEOUT) {
                    blz::log(g_logCategory, 4, s_indexFile, 0xcd, "Timed out when trying to acquire index writer lock.");
                } else if (wait == WAIT_FAILED) {
                    blz::log(g_logCategory, 4, s_indexFile, 0xd4, "Failed to acquire index writer lock. Error (%d)",
                        GetLastError());
                }
                return false;
            }
            blz::log(g_logCategory, 2, s_indexFile, 199, "Index writer lock acquired from an abandoned wait.");
        }
    }
    return true;
}

// ref: FUN_142368810
void MutexUnlock(HANDLE* mutex) {
    ReleaseMutex(*mutex);
}

// ref: FUN_142368800
void MutexClose(HANDLE* mutex) {
    if (*mutex) {
        CloseHandle(*mutex);
    }
}

// Modes other than 2..5 (and mode 3) may see another process's flush, so they bring the
// table up to the version shmem records before touching it.
bool NeedsRefresh(int32_t mode) {
    return (static_cast<uint32_t>(mode - 2) & 0xFFFFFFFC) != 0 || mode == 3;
}

void WriteBE40(uint8_t* p, uint64_t value) {
    p[0] = static_cast<uint8_t>(value >> 32);
    p[1] = static_cast<uint8_t>(value >> 24);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 8);
    p[4] = static_cast<uint8_t>(value);
}

// What the free space rebuild collects: the used spans as 9-byte records, and with frozen
// archives, the archives some key still references.
struct SPANCOLLECTOR {
    uint8_t* m_records;     // 0x00
    uint32_t m_capacity;    // 0x08
    uint32_t m_count;       // 0x0c
    blz::id_set* m_frozen;  // 0x10
    blz::id_set m_touched;  // 0x18
};

// A frozen archive counts as wholly used, and is recorded once.
// ref: FUN_14214db40
bool CollectSpan(void* context, const uint8_t* key, const SPAN* span, int32_t kind) {
    (void)key;
    auto collector = static_cast<SPANCOLLECTOR*>(context);
    if (!collector->m_frozen) {
        uint8_t* record = collector->m_records + uint64_t(collector->m_count) * 9;
        collector->m_count++;
        WriteBE40(record, span->m_offset);
        std::memcpy(record + 5, &span->m_size, 4);
        return true;
    }
    uint64_t archive = span->m_offset >> 30;
    if (kind != 3) {
        collector->m_touched.insert(archive);
    }
    if (collector->m_frozen->contains(archive)) {
        return true;
    }
    uint8_t* record = collector->m_records + uint64_t(collector->m_count) * 9;
    collector->m_count++;
    WriteBE40(record, (archive & 0x3FFFFFFFF) << 30);
    uint32_t size = 0x40000000;
    std::memcpy(record + 5, &size, 4);
    collector->m_frozen->insert(archive);
    return true;
}

struct FREEZER {
    blz::id_set* m_frozen;  // 0x00
    blz::id_set m_touched;  // 0x08
};

// ref: FUN_14214dcb0
bool FreezeSpan(void* context, const uint8_t* key, const SPAN* span, int32_t kind) {
    (void)key;
    auto freezer = static_cast<FREEZER*>(context);
    uint64_t archive = span->m_offset >> 30;
    if (kind != 3) {
        freezer->m_touched.insert(archive);
    }
    freezer->m_frozen->insert(archive);
    return true;
}

}

// --- FREESPACECACHE ------------------------------------------------------------------------

// ref: FUN_142157580
void FREESPACECACHE::Init(blz::allocator* allocator, STATSINK* stats) {
    this->m_stats = stats;
    this->m_allocator = allocator;
    this->m_pool.Init(allocator, 0x30, 8, 0x10, 1);
    auto freeHead = reinterpret_cast<LISTLINK*>(this->m_free);
    freeHead->m_next = freeHead;
    freeHead->m_prev = freeHead;
    auto usedHead = reinterpret_cast<LISTLINK*>(this->m_used);
    usedHead->m_next = usedHead;
    usedHead->m_prev = usedHead;
    this->m_reallocated = 0;
    this->m_unused = 0;
}

// A span node is 0x30 bytes: its size at +0x00, its free-list link at +0x10 and its used-list
// link at +0x20.
// ref: FUN_1421575f0
void FREESPACECACHE::Destroy() {
    auto usedHead = reinterpret_cast<LISTLINK*>(this->m_used);
    for (LISTLINK* link = usedHead->m_next; link != usedHead; link = link->m_next) {
        this->m_unused += *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(link) - 0x18);
    }
    StatsReport(this->m_stats, 3, "free_space_cache.bytes_reallocated", this->m_reallocated, 1.0f);
    StatsReport(this->m_stats, 3, "free_space_cache.bytes_unused", this->m_unused, 1.0f);
    ListReset(usedHead);
    auto freeHead = reinterpret_cast<LISTLINK*>(this->m_free);
    while (freeHead->m_next != freeHead) {
        LISTLINK* last = freeHead->m_prev;
        uint8_t* node = nullptr;
        if (last != freeHead) {
            ListUnlink(last);
            node = reinterpret_cast<uint8_t*>(last) - 0x10;
        }
        ListUnlink(reinterpret_cast<LISTLINK*>(node + 0x20));
        ListUnlink(reinterpret_cast<LISTLINK*>(node + 0x10));
        this->m_pool.Free(node);
    }
    ListReset(usedHead);
    ListReset(freeHead);
    this->m_pool.Destroy();
}

// --- CONTAINERINDEX ------------------------------------------------------------------------

// ref: FUN_142149cc0
void CONTAINERINDEX::Construct(blz::allocator* allocator, STATSINK* stats) {
    this->m_allocator = allocator;
    this->m_stats = stats;
    this->m_bound = 0;
    this->m_reclaim = 0;
    this->m_path.Init(allocator);
    this->m_mode = 0;
    this->m_maxSize = 0;
    this->m_writeHeaders = nullptr;
    this->m_writeHeadersContext = nullptr;
    this->m_noPidTable = 0;
    this->m_shmem.m_header = nullptr;
    this->m_shmem.m_freeSpace = nullptr;
    this->m_shmem.m_generation = 0;
    this->m_shmem.m_slot = 0xFFFFFFFF;
    this->m_shmem.m_created = 0;
    OsMappingInit(&this->m_shmem.m_mapping);
    this->m_mutex = nullptr;
    this->m_tables.Init(allocator);
    for (auto& kmt : this->m_kmts) {
        kmt.Init();
    }
    this->m_freeSpace.Init(allocator, stats);
    this->m_frozen = nullptr;
    for (uint32_t i = 0; i < 16; i++) {
        this->m_kmts[i].Setup(allocator, stats);
        InitializeSRWLock(&this->m_bucketLocks[i]);
    }
    InitializeSRWLock(&this->m_lock);
}

// Flushes the tables and leaves the process table on the way out.
// ref: FUN_142149df0
void CONTAINERINDEX::Destroy() {
    if (this->m_bound) {
        int32_t error = this->FlushAll();
        if (error != 0) {
            blz::log(g_logCategory, 4, s_indexFile, 0x281, "Failed to flush key mapping tables during shutdown. Error (%u): %s",
                error, ErrorString(error));
        }
        if (!this->m_noPidTable && this->m_mode != 2) {
            int32_t failure = 0;
            if (!MutexLock(&this->m_mutex, this->m_mode)) {
                failure = ERR_LOCKED;
            } else {
                if (this->m_shmem.m_slot != 0xFFFFFFFF) {
                    SHMEMHEADER* header = this->m_shmem.m_header;
                    auto table = reinterpret_cast<PIDTABLE*>(reinterpret_cast<uint8_t*>(header) + header->m_tableOffsets[0]);
                    if (!PidTableValid(table)) {
                        failure = ERR_UNSPECIFIED;
                    } else {
                        PidTableRemove(table, &this->m_shmem, this->m_mode);
                    }
                }
                if (!Exclusive(this->m_mode)) {
                    MutexUnlock(&this->m_mutex);
                }
            }
            if (failure != 0) {
                blz::log(g_logCategory, 4, s_indexFile, 0x288,
                    "Failed to remove ourself from the pid tracking table on shutdown. Error (%u): %s", failure,
                    ErrorString(failure));
            }
        }
        ShmemUnbind(&this->m_shmem);
    }
    if (!Exclusive(this->m_mode)) {
        MutexClose(&this->m_mutex);
    }
    this->m_freeSpace.Destroy();
    for (int32_t i = 15; i >= 0; i--) {
        this->m_kmts[i].Destroy();
    }
    this->m_tables.Destroy();
    OsMappingClose(&this->m_shmem.m_mapping);
    this->m_path.Destroy();
}

// ref: FUN_14214d300
bool CONTAINERINDEX::HasPidTable() {
    if (this->m_noPidTable) {
        return false;
    }
    return this->m_shmem.m_slot != 0xFFFFFFFF;
}

// The bucket of a key: its first nine bytes folded to a nibble, plus a salt.
uint32_t CONTAINERINDEX::BucketOf(const uint8_t key[9], uint32_t salt) {
    uint8_t x = key[0] ^ key[1] ^ key[2] ^ key[3] ^ key[4] ^ key[5] ^ key[6] ^ key[7] ^ key[8];
    return (static_cast<uint32_t>(static_cast<uint8_t>(x >> 4 ^ x)) + (salt & 0xFF)) & 0xF;
}

// Opens the index at `path`: read-only straight from the table files, otherwise through shmem.
// ref: FUN_14214d320
int32_t CONTAINERINDEX::Bind(const char* path, int32_t mode, uint64_t maxSize, uint8_t reclaim,
    HEADERWRITER writeHeaders, void* context, int32_t* processes, blz::id_set* frozen, uint8_t noPidTable) {
    char normalized[0x30C];
    if (!OsPathIsAbsolute(path)) {
        char cwd[0x30C];
        uint32_t error = OsGetCurrentDirectory(cwd, sizeof(cwd));
        if (error != 0) {
            char text[0x150];
            OsErrorString(text, 0x100, error);
            blz::log(g_logCategory, 4, s_indexFile, 0xf5, "Failed to get current directory. Error (%u): %s", error, text);
            return ERR_IO;
        }
        OsPathJoin(normalized, sizeof(normalized), cwd, path);
    } else {
        OsPathNormalize(normalized, sizeof(normalized), path);
    }
    this->m_path.Assign(normalized, static_cast<uint32_t>(std::strlen(normalized)));
    this->m_reclaim = reclaim;
    this->m_writeHeaders = writeHeaders;
    this->m_writeHeadersContext = context;
    this->m_noPidTable = noPidTable;
    this->m_mode = mode;
    this->m_maxSize = maxSize;
    int32_t result;
    if (mode == 2) {
        // A read-only bind sees a zeroed header: every table version 0, so the tables scan.
        uint8_t header[0x150];
        std::memset(header, 0, sizeof(header));
        SHMEMBINDING binding;
        binding.m_header = reinterpret_cast<SHMEMHEADER*>(header);
        binding.m_freeSpace = nullptr;
        binding.m_generation = 0;
        binding.m_slot = 0xFFFFFFFF;
        binding.m_created = 0;
        OsMappingInit(&binding.m_mapping);
        result = this->m_tables.Bind(this->m_path.c_str(), this->m_mode, &binding);
        if (result == 0) {
            for (uint32_t bucket = 0; bucket < 16; bucket++) {
                int32_t bound = this->m_kmts[bucket].Bind(this->m_tables.File(bucket), this->m_mode, bucket, 0);
                if (bound != 0) {
                    blz::log(g_logCategory, 4, s_indexFile, 0x7b2, "Failed to bind read-only mapping table `%u`", bucket);
                    result = bound;
                    break;
                }
            }
            if (result == 0) {
                this->m_maxSize = this->m_kmts[0].m_maxSize;
            }
        }
        OsMappingClose(&binding.m_mapping);
    } else {
        result = this->BindWritable(processes, frozen);
    }
    if (result == 0) {
        this->m_bound = 1;
    }
    return result;
}

// ref: FUN_14214e580
int32_t CONTAINERINDEX::BindWritable(int32_t* processes, blz::id_set* frozen) {
    uint32_t error = OsCreateDirectoryTree(this->m_path.c_str(), true);
    if (error != 0) {
        char text[0x100];
        OsErrorString(text, sizeof(text), error);
        blz::log(g_logCategory, 4, s_indexFile, 0x7c3, "Failed to create index directory '%s'. Error (%u): %s",
            this->m_path.c_str(), error, text);
        return ErrorFromOs(error);
    }
    for (uint32_t attempt = 0; attempt < 3; attempt++) {
        bool retry = false;
        int32_t result = ShmemBind(this->m_allocator, this->m_path.c_str(), this->m_mode, this->m_stats, &this->m_shmem, &retry);
        if (result != 0) {
            if (!retry) {
                return result;
            }
            OsSleepNanoseconds(20000000);
            blz::log(g_logCategory, 3, s_indexFile, 0x7d8, "Retrying shared memory bind.");
            continue;
        }

        bool unbind = true;
        if (!Exclusive(this->m_mode)) {
            GlobalMutexCreate(&this->m_mutex, this->m_shmem.m_header->m_mutexName);
        }
        if (!MutexLock(&this->m_mutex, this->m_mode)) {
            ShmemUnbind(&this->m_shmem);
            return ERR_LOCKED;
        }
        result = this->m_tables.Bind(this->m_path.c_str(), this->m_mode, &this->m_shmem);
        if (result == 0) {
            for (uint32_t bucket = 0; bucket < 16; bucket++) {
                result = this->m_kmts[bucket].Bind(this->m_tables.File(bucket), this->m_mode, bucket, this->m_maxSize);
                if (result != 0) {
                    blz::log(g_logCategory, 4, s_indexFile, 0x7fb, "Failed to bind mapping table `%u`", bucket);
                    break;
                }
            }
        }
        if (result == 0) {
            this->m_maxSize = this->m_kmts[0].m_maxSize;
            if (this->m_mode == 4) {
                unbind = false;
            } else {
                result = this->Reconcile(frozen);
                if (result == 0 && !this->m_noPidTable) {
                    SHMEMHEADER* header = this->m_shmem.m_header;
                    this->m_shmem.m_slot = 0xFFFFFFFF;
                    if (header->m_version != 4 && (header->m_flags & 2) != 0 && (header->m_tableFlags & 1) != 0) {
                        auto table =
                            reinterpret_cast<PIDTABLE*>(reinterpret_cast<uint8_t*>(header) + header->m_tableOffsets[0]);
                        if (!PidTableValid(table)) {
                            PidTableInit(table, header->m_tableSizes[0]);
                        }
                        *processes = 0;
                        result = PidTableAdd(table, &this->m_shmem, this->m_mode);
                        if (result == 0) {
                            *processes = static_cast<int32_t>(table->m_count);
                        }
                    }
                }
                if (result == 0) {
                    unbind = false;
                }
            }
        }
        if (!Exclusive(this->m_mode)) {
            MutexUnlock(&this->m_mutex);
        }
        if (unbind) {
            ShmemUnbind(&this->m_shmem);
        }
        return result;
    }
    blz::log(g_logCategory, 4, s_indexFile, 0x7dd, "Another process is interfering with our ability to bind shared memory.");
    return ERR_LOCKED;
}

// The first process to bind (the free space table not yet valid) rebuilds it from every
// key's span, frozen archives counting as wholly used; later ones only work out which of the
// frozen archives no key references any more.
// ref: FUN_14214d5c0
int32_t CONTAINERINDEX::Reconcile(blz::id_set* frozen) {
    FREESPACETABLE* table = this->m_shmem.m_freeSpace;
    if (!PidTableValid(reinterpret_cast<PIDTABLE*>(table))) {
        this->m_frozen = frozen;
        uint32_t capacity = 1;
        for (auto& kmt : this->m_kmts) {
            capacity += kmt.CountKeys();
        }
        auto records = static_cast<uint8_t*>(this->m_allocator->allocate(uint64_t(capacity) * 9, 1));
        if (records) {
            std::memset(records, 0, uint64_t(capacity) * 9);
        }
        SPANCOLLECTOR collector;
        collector.m_records = records;
        collector.m_capacity = capacity;
        collector.m_count = 0;
        collector.m_frozen = frozen;
        collector.m_touched.init(this->m_allocator);
        for (auto& kmt : this->m_kmts) {
            kmt.Enumerate(&CollectSpan, &collector, 0);
        }
        // The end of the container closes the last free span.
        uint8_t* last = records + uint64_t(collector.m_count) * 9;
        WriteBE40(last, this->m_maxSize);
        uint32_t end = 0xFFFFFFFF;
        std::memcpy(last + 5, &end, 4);
        uint32_t count = collector.m_count + 1;
        auto pointers = static_cast<uint8_t**>(this->m_allocator->allocate(uint64_t(count) * 8, 8));
        if (pointers) {
            std::memset(pointers, 0, uint64_t(count) * 8);
        }
        for (uint32_t i = 0; i < count; i++) {
            pointers[i] = records + uint64_t(i) * 9;
        }
        for (uint32_t i = 0; i < collector.m_touched.m_count; i++) {
            frozen->remove(collector.m_touched.m_entries[i]);
        }
        if (frozen) {
            for (uint32_t i = 0; i < frozen->m_count; i++) {
                blz::log(g_logCategory, 2, s_indexFile, 0x854, "Freezing archive %llu", frozen->m_entries[i]);
            }
        }
        FreeSpaceRebuild(table, pointers, count);
        this->m_allocator->deallocate(pointers);
        collector.m_touched.destroy();
        this->m_allocator->deallocate(records);
    } else if (frozen) {
        FREEZER freezer;
        freezer.m_frozen = frozen;
        freezer.m_touched.init(this->m_allocator);
        for (auto& kmt : this->m_kmts) {
            kmt.Enumerate(&FreezeSpan, &freezer, 0);
        }
        for (uint32_t i = 0; i < freezer.m_touched.m_count; i++) {
            frozen->remove(freezer.m_touched.m_entries[i]);
        }
        for (uint32_t i = 0; i < frozen->m_count; i++) {
            blz::log(g_logCategory, 2, s_indexFile, 0xa43, "Freezing archive %llu", frozen->m_entries[i]);
        }
        this->m_frozen = frozen;
        freezer.m_touched.destroy();
    }
    return 0;
}

// ref: FUN_14214ce20
int32_t CONTAINERINDEX::LookupLocked(uint32_t bucket, const uint8_t key[9], SPAN* span) {
    if (NeedsRefresh(this->m_mode)) {
        int32_t result =
            this->m_tables.Refresh(bucket, this->m_shmem.m_header->m_tableVersions[bucket], &this->m_kmts[bucket]);
        if (result != 0) {
            return result;
        }
    }
    return this->m_kmts[bucket].Lookup(key, span);
}

// ref: FUN_14214c970 (also reached through FUN_14214ca30, which copies the key first)
int32_t CONTAINERINDEX::Lookup(const uint8_t key[9], uint32_t salt, SPAN* span) {
    uint32_t bucket = BucketOf(key, salt);
    AcquireSRWLockExclusive(&this->m_bucketLocks[bucket]);
    int32_t result = this->LookupLocked(bucket, key, span);
    ReleaseSRWLockExclusive(&this->m_bucketLocks[bucket]);
    return result;
}

// Whether part of a key's span is present and resident.
// ref: FUN_14214b0c0
int32_t CONTAINERINDEX::QuerySpan(const uint8_t key[9], uint32_t salt, const SPAN* request) {
    uint32_t bucket = BucketOf(key, salt);
    AcquireSRWLockExclusive(&this->m_bucketLocks[bucket]);
    int32_t result = 0;
    if (NeedsRefresh(this->m_mode)) {
        result = this->m_tables.Refresh(bucket, this->m_shmem.m_header->m_tableVersions[bucket], &this->m_kmts[bucket]);
    }
    if (result == 0) {
        uint8_t key9[9];
        std::memcpy(key9, key, 9);
        result = this->m_kmts[bucket].GetSpan(key9, request);
    }
    ReleaseSRWLockExclusive(&this->m_bucketLocks[bucket]);
    return result;
}

// Resolves part of a key's span to container offsets.
// ref: FUN_14214cf30
int32_t CONTAINERINDEX::Resolve(const uint8_t key[9], uint32_t salt, const SPAN* request, SPAN* out, SPAN* stored) {
    uint32_t bucket = BucketOf(key, salt);
    AcquireSRWLockExclusive(&this->m_bucketLocks[bucket]);
    int32_t result = 0;
    if (NeedsRefresh(this->m_mode)) {
        result = this->m_tables.Refresh(bucket, this->m_shmem.m_header->m_tableVersions[bucket], &this->m_kmts[bucket]);
    }
    if (result == 0) {
        uint8_t key9[9];
        std::memcpy(key9, key, 9);
        result = this->m_kmts[bucket].ResolveSpan(key9, request, out, stored);
    }
    ReleaseSRWLockExclusive(&this->m_bucketLocks[bucket]);
    return result;
}

// Who else writes to the container, from the process table: 0 untracked, 1/2 no writers,
// 3/4 only us, 5 others (2 and 4 when the table changed since last asked).
// ref: FUN_14214b1f0
uint8_t CONTAINERINDEX::CheckWriters() {
    if (this->m_noPidTable || this->m_shmem.m_slot == 0xFFFFFFFF) {
        return 0;
    }
    SHMEMHEADER* header = this->m_shmem.m_header;
    auto table = reinterpret_cast<PIDTABLE*>(reinterpret_cast<uint8_t*>(header) + header->m_tableOffsets[0]);
    uint32_t writers = table->m_writers;
    uint64_t generation = table->m_generation;
    uint64_t seen = this->m_shmem.m_generation;
    if (generation != seen) {
        this->m_shmem.m_generation = generation;
    }
    if (writers == 0) {
        return (seen != generation) + 1;
    }
    if (writers != 1) {
        return 5;
    }
    return (seen != generation) + 3;
}

// ref: FUN_14214bae0
int32_t CONTAINERINDEX::RemoveKeyLocked(const uint8_t key[9], uint32_t salt) {
    uint32_t bucket = BucketOf(key, salt);
    if (NeedsRefresh(this->m_mode)) {
        int32_t result =
            this->m_tables.Refresh(bucket, this->m_shmem.m_header->m_tableVersions[bucket], &this->m_kmts[bucket]);
        if (result != 0) {
            return result;
        }
    }
    SPAN previous = {};
    int32_t result = this->Update(bucket, key, 3, nullptr, &previous);
    if (result == 0) {
        if (this->m_reclaim) {
            // The span would go back to the free space cache (FUN_142157a00), which belongs to
            // the write path and is not ported.
            FROST_UNIMPLEMENTED();
        }
        result = 0;
    }
    return result;
}

// ref: FUN_14214b870
int32_t CONTAINERINDEX::RemoveKey(const uint8_t key[9], uint32_t salt) {
    uint32_t bucket = BucketOf(key, salt);
    AcquireSRWLockExclusive(&this->m_bucketLocks[bucket]);
    int32_t result = this->RemoveKeyLocked(key, salt);
    ReleaseSRWLockExclusive(&this->m_bucketLocks[bucket]);
    return result;
}

// Removes 16-byte keys; a large batch of data keys (10000 or more, salt 0) goes through a sorted
// rewrite of the tables instead.
// ref: FUN_14214bbe0
int32_t CONTAINERINDEX::RemoveKeys(const uint8_t* keys, uint32_t count, uint8_t salt) {
    if (this->m_mode == 2) {
        return ERR_UNSUPPORTED;
    }
    if (count == 0) {
        return 0;
    }
    if (count > 9999 && salt == 0) {
        // The batch removal (FUN_14214b260) is not ported.
        FROST_UNIMPLEMENTED();
        return ERR_UNSPECIFIED;
    }
    if (!MutexLock(&this->m_mutex, this->m_mode)) {
        return ERR_LOCKED;
    }
    int32_t result = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t key9[9];
        std::memcpy(key9, keys + uint64_t(i) * 16, 9);
        result = this->RemoveKey(key9, salt);
        if (result != 0) {
            break;
        }
    }
    if (!Exclusive(this->m_mode)) {
        MutexUnlock(&this->m_mutex);
    }
    return result;
}

// A full update page is flushed to a new table file and the update retried.
// ref: FUN_14214e910
int32_t CONTAINERINDEX::Update(uint32_t bucket, const uint8_t key[9], uint8_t type, const SPAN* span, SPAN* previous) {
    KEYMAPPINGTABLE& kmt = this->m_kmts[bucket];
    int32_t result = kmt.Update(key, type, span, previous);
    if (result == ERR_CANCELED) {
        result = this->FlushTable(bucket, nullptr, 0);
        if (result != 0) {
            blz::log(g_logCategory, 4, s_indexFile, 0x9e3, "Failed to flush table '%u' during update.", bucket);
            return result;
        }
        result = kmt.Update(key, type, span, nullptr);
    }
    if (type == 3 && result == ERR_NOT_RESERVED) {
        result = 0;
    } else if (result != 0) {
        char text[0x13];
        blz::hex_encode(text, sizeof(text), key, 9);  // FUN_142157c10
        blz::log(g_logCategory, 4, s_indexFile, 0x9f3,
            "Failed to update key mapping for key '%s' to update type %u. Error (%u): %s", text, type, result,
            ErrorString(result));
    }
    return result;
}

// Marks part of a key's span resident (update type 6) or not (7).
// ref: FUN_14214ea70
int32_t CONTAINERINDEX::UpdateResidency(const uint8_t key[16], uint32_t salt, const SPAN* span, uint8_t resident) {
    if (this->m_mode == 2) {
        return ERR_UNSUPPORTED;
    }
    if (!MutexLock(&this->m_mutex, this->m_mode)) {
        return ERR_LOCKED;
    }
    uint32_t bucket = BucketOf(key, salt);
    AcquireSRWLockExclusive(&this->m_bucketLocks[bucket]);
    int32_t result = 0;
    if (NeedsRefresh(this->m_mode)) {
        result = this->m_tables.Refresh(bucket, this->m_shmem.m_header->m_tableVersions[bucket], &this->m_kmts[bucket]);
    }
    if (result == 0) {
        uint8_t key9[9];
        std::memcpy(key9, key, 9);
        result = this->Update(bucket, key9, static_cast<uint8_t>((resident ^ 1) + 6), span, nullptr);
    }
    ReleaseSRWLockExclusive(&this->m_bucketLocks[bucket]);
    if (!Exclusive(this->m_mode)) {
        MutexUnlock(&this->m_mutex);
    }
    return result;
}

// ref: FUN_14214be70
int32_t CONTAINERINDEX::FlushTable(uint32_t bucket, KMTRECORD* records, uint32_t count) {
    uint32_t version = 0;
    int32_t result = this->m_tables.Flush(bucket, &this->m_kmts[bucket], &version, records, count);
    if (result == 0) {
        this->m_shmem.m_header->m_tableVersions[bucket] = version;
        return 0;
    }
    blz::log(g_logCategory, 4, s_indexFile, 0xa0a, "Failed to flush table '%u'. Error (%u): %s", bucket, result,
        ErrorString(result));
    return result;
}

// ref: FUN_14214bf30
int32_t CONTAINERINDEX::FlushAll() {
    if (this->m_mode == 2) {
        return 0;
    }
    if (!MutexLock(&this->m_mutex, this->m_mode)) {
        return ERR_LOCKED;
    }
    int32_t result = 0;
    for (uint32_t bucket = 0; bucket < 16; bucket++) {
        AcquireSRWLockExclusive(&this->m_bucketLocks[bucket]);
        int32_t error = 0;
        if (NeedsRefresh(this->m_mode)) {
            error = this->m_tables.Refresh(bucket, this->m_shmem.m_header->m_tableVersions[bucket], &this->m_kmts[bucket]);
        }
        if (error == 0 && this->m_kmts[bucket].HasUpdates()) {
            error = this->FlushTable(bucket, nullptr, 0);
        }
        ReleaseSRWLockExclusive(&this->m_bucketLocks[bucket]);
        if (error != 0) {
            result = error;
            break;
        }
    }
    if (!Exclusive(this->m_mode)) {
        MutexUnlock(&this->m_mutex);
    }
    return result;
}

// ref: FUN_14214d990
int32_t CONTAINERINDEX::EnumerateAll(KMTSPANVISITOR visitor, void* context) {
    for (uint32_t bucket = 0; bucket < 16; bucket++) {
        AcquireSRWLockExclusive(&this->m_bucketLocks[bucket]);
        int32_t result = 0;
        if (NeedsRefresh(this->m_mode)) {
            result = this->m_tables.Refresh(bucket, this->m_shmem.m_header->m_tableVersions[bucket], &this->m_kmts[bucket]);
        }
        if (result == 0) {
            result = this->m_kmts[bucket].Enumerate(visitor, context, 0);
        }
        ReleaseSRWLockExclusive(&this->m_bucketLocks[bucket]);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142157c10=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_14214ee50 = (void*)&casc::MutexLock;
extern "C" void* const gen_f_142368810 = (void*)&casc::MutexUnlock;
// ---- end of generated globals ----
