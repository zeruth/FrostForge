#include "casc/ArchiveFileCache.hpp"
#include "blz/IdMap.hpp"
#include "blz/Log.hpp"
#include "casc/Common.hpp"
#include <cstring>
#include <malloc.h>
#include <new>

namespace casc {

namespace {

const char s_cacheFile[] = "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\archive_file_cache.cpp";
const char s_dynamicCacheFile[] =
    "D:\\BuildServer\\A\\work\\package_cache\\casc\\1.4.41\\src\\dynamic_archive_cache.cpp";

// DAT_142620be0: the packages' "no error" code.
const uint32_t s_noError = 0;

void ListInit(LISTLINK* link) {
    link->m_next = link;
    link->m_prev = link;
}

void ListUnlink(LISTLINK* link) {
    link->m_prev->m_next = link->m_next;
    link->m_next->m_prev = link->m_prev;
    link->m_prev = link;
    link->m_next = link;
}

// Links `link` in front of `before`.
void ListInsertBefore(LISTLINK* before, LISTLINK* link) {
    link->m_prev->m_next = link->m_next;
    link->m_next->m_prev = link->m_prev;
    link->m_prev = before->m_prev;
    link->m_next = before;
    before->m_prev = link;
    link->m_prev->m_next = link;
}

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

ARCHIVEFILE* FileOfLink(LISTLINK* link) {
    return reinterpret_cast<ARCHIVEFILE*>(reinterpret_cast<uint8_t*>(link) - offsetof(ARCHIVEFILE, m_link));
}

// ref: FUN_142166a40
void LogOpenError(const char* path, uint32_t error) {
    char text[0x100];
    OsErrorString(text, sizeof(text), error);
    blz::log(g_logCategory, 4, s_cacheFile, 0x76, "Error opening container archive file '%s'. (reason: %s)", path, text);
}

// A synchronous archive open. With exclusive reads a read-only open shares with readers only,
// so it fails (sharing violation) while another process has the archive open for writing.
// ref: FUN_142166aa0
uint32_t OpenArchiveFile(OSFILE* file, const char* path, int32_t mode, uint8_t kind, uint8_t exclusiveRead) {
    uint32_t flags = kind == 1 ? 2 : 0;
    if (mode == 2 && exclusiveRead) {
        uint64_t length = std::strlen(path);
        auto wide = static_cast<wchar_t*>(_alloca((length * 2 + 2 + 0xF) & ~uint64_t(0xF)));
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 0x3FFFFFFF);
        DWORD attributes = kind == 1 ? 0x20000080 : FILE_ATTRIBUTE_NORMAL;
        file->m_handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, attributes, nullptr);
        if (file->m_handle == INVALID_HANDLE_VALUE) {
            return GetLastError();
        }
        return s_noError;
    }
    return OsFileOpen(file, path, mode, flags);
}

}

// --- ARCHIVEFILE ---------------------------------------------------------------------------

// ref: FUN_1421660f0
ARCHIVEFILE::~ARCHIVEFILE() {
    ListUnlink(&this->m_link);
    OsFileClose(&this->m_asyncFile);
    OsFileClose(&this->m_file);
}

// ref: FUN_142166680
void ARCHIVEFILE::Destroy() {
    blz::allocator* allocator = this->m_allocator;
    this->~ARCHIVEFILE();
    allocator->deallocate(this);
}

void ARCHIVEFILE::AddRef() {
    _InterlockedIncrement(reinterpret_cast<volatile long*>(&this->m_refs));
}

void ARCHIVEFILE::Release() {
    if (_InterlockedDecrement(reinterpret_cast<volatile long*>(&this->m_refs)) == 0) {
        this->Destroy();
    }
}

// --- ARCHIVEMAP ----------------------------------------------------------------------------

// The id hash of the archive combined with the id hash of the kind (boost's hash_combine).
uint32_t ARCHIVEMAP::Hash(const ARCHIVEKEY* key) {
    uint64_t h = (key->m_archive >> 33 ^ key->m_archive) * 0xFF51AFD7ED558CCDull;
    h = (h >> 33 ^ h) * 0xC4CEB9FE1A85EC53ull;
    h = h >> 33 ^ h;
    uint32_t kind = blz::id_hash(key->m_kind);
    return (static_cast<uint32_t>(h >> 2) + kind + static_cast<uint32_t>(h << 6) + 0x9E3779B9u) ^ static_cast<uint32_t>(h);
}

// ref: FUN_142165f40
uint32_t ARCHIVEMAP::IndexOf(const ARCHIVEKEY* key, uint32_t slot) {
    if (!this->m_buckets) {
        return 0xFFFFFFFF;
    }
    uint32_t i = this->m_buckets[slot];
    while (i != 0xFFFFFFFF) {
        if (this->m_entries[i].m_key.m_archive == key->m_archive && this->m_entries[i].m_key.m_kind == key->m_kind) {
            return i;
        }
        i = this->m_next[i];
    }
    return i;
}

// ref: FUN_142165e80
ARCHIVEMAP::ENTRY* ARCHIVEMAP::Find(const ARCHIVEKEY* key) {
    uint32_t i = this->IndexOf(key, Hash(key) & (this->m_bucketCount - 1));
    if (i != 0xFFFFFFFF) {
        return &this->m_entries[i];
    }
    return nullptr;
}

// The index of a key and the link that points at it.
// ref: FUN_142165fc0
void ARCHIVEMAP::FindLink(const ARCHIVEKEY* key, uint32_t* index, uint32_t** link) {
    uint32_t slot = Hash(key) & (this->m_bucketCount - 1);
    uint32_t* at = &this->m_buckets[slot];
    uint32_t i = *at;
    while (i != 0xFFFFFFFF) {
        if (this->m_entries[i].m_key.m_archive == key->m_archive && this->m_entries[i].m_key.m_kind == key->m_kind) {
            break;
        }
        at = &this->m_next[i];
        i = this->m_next[i];
    }
    *index = i;
    *link = at;
}

// ref: FUN_142165bb0
ARCHIVEMAP::ENTRY* ARCHIVEMAP::Insert(const ARCHIVEKEY* key, ARCHIVEFILE** file, bool* inserted) {
    uint32_t slot = Hash(key) & (this->m_bucketCount - 1);
    uint32_t found = this->IndexOf(key, slot);
    if (found != 0xFFFFFFFF) {
        *inserted = false;
        return &this->m_entries[found];
    }
    uint32_t index = this->m_count;
    if (index == this->m_capacity) {
        this->Reserve(this->m_capacity * 2);
    }
    this->m_next[index] = this->m_buckets[slot];
    this->m_buckets[slot] = index;
    this->m_count++;
    ENTRY* entry = &this->m_entries[index];
    entry->m_key = *key;
    entry->m_file = *file;
    if (entry->m_file) {
        entry->m_file->AddRef();
    }
    if (this->m_bucketCount < this->m_count) {
        this->Rehash(this->m_bucketCount * 2);
    }
    *inserted = true;
    return entry;
}

// Removes a key, moving the last entry into its place.
// ref: FUN_142165d20
bool ARCHIVEMAP::Remove(const ARCHIVEKEY* key) {
    if (!this->m_buckets) {
        return false;
    }
    uint32_t index;
    uint32_t* link;
    this->FindLink(key, &index, &link);
    if (index == 0xFFFFFFFF) {
        return false;
    }
    *link = this->m_next[index];
    uint32_t last = --this->m_count;
    ENTRY* entry = &this->m_entries[index];
    if (index == last) {
        if (entry->m_file) {
            entry->m_file->Release();
        }
        return true;
    }
    ENTRY* moved = &this->m_entries[last];
    uint32_t movedIndex;
    uint32_t* movedLink;
    this->FindLink(&moved->m_key, &movedIndex, &movedLink);
    if (entry->m_file) {
        entry->m_file->Release();
    }
    entry->m_key = moved->m_key;
    entry->m_file = moved->m_file;
    moved->m_file = nullptr;
    *movedLink = index;
    this->m_next[index] = this->m_next[this->m_count];
    return true;
}

// ref: FUN_142166790
void ARCHIVEMAP::Rehash(uint32_t buckets) {
    if ((buckets & (buckets - 1)) != 0) {
        int32_t bit = 31;
        while (((buckets | 1) >> bit) == 0) {
            bit--;
        }
        buckets = 1u << ((bit + 1) & 31);
    }
    auto table = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(buckets) * 4, 0x10));
    if (buckets != 0) {
        std::memset(table, 0xFF, uint64_t(buckets) * 4);
    }
    if (this->m_entries) {
        for (uint32_t i = 0; i < this->m_count; i++) {
            uint32_t slot = Hash(&this->m_entries[i].m_key) & (buckets - 1);
            this->m_next[i] = table[slot];
            table[slot] = i;
        }
    }
    this->m_allocator->deallocate(this->m_buckets);
    this->m_bucketCount = buckets;
    this->m_buckets = table;
}

// ref: FUN_142166900
void ARCHIVEMAP::Reserve(uint32_t capacity) {
    if (!this->m_buckets) {
        this->Rehash(this->m_bucketCount);
    }
    if (capacity < 8) {
        capacity = 8;
    }
    auto entries = static_cast<ENTRY*>(this->m_allocator->allocate(uint64_t(capacity) * sizeof(ENTRY), 0x10));
    auto next = static_cast<uint32_t*>(this->m_allocator->allocate(uint64_t(capacity) * 4, 0x10));
    for (uint32_t i = 0; i < this->m_count; i++) {
        entries[i].m_key = this->m_entries[i].m_key;
        entries[i].m_file = this->m_entries[i].m_file;
        this->m_entries[i].m_file = nullptr;
        next[i] = this->m_next[i];
    }
    this->m_allocator->deallocate(this->m_entries);
    this->m_allocator->deallocate(this->m_next);
    this->m_entries = entries;
    this->m_next = next;
    this->m_capacity = capacity;
}

// ref: FUN_1420e8790
void ARCHIVEMAP::Clear() {
    if (this->m_buckets) {
        for (uint32_t i = 0; i < this->m_bucketCount; i++) {
            this->m_buckets[i] = 0xFFFFFFFF;
        }
    }
    for (uint32_t i = 0; i < this->m_count; i++) {
        if (this->m_entries[i].m_file) {
            this->m_entries[i].m_file->Release();
        }
    }
    this->m_count = 0;
}

// --- ARCHIVEFILECACHE ----------------------------------------------------------------------

// ref: FUN_1421660b0
ARCHIVEFILECACHE::ARCHIVEFILECACHE(blz::allocator* allocator, uint64_t capacity) {
    this->m_capacity = capacity;
    ListInit(&this->m_lru);
    this->m_map.m_buckets = nullptr;
    this->m_map.m_entries = nullptr;
    this->m_map.m_next = nullptr;
    this->m_map.m_count = 0;
    this->m_map.m_capacity = 0;
    this->m_map.m_allocator = allocator;
    this->m_map.m_bucketCount = 0x10;
    this->m_exclusiveRead = 0;
}

// ref: FUN_142156c60 (through FUN_142156d90)
ARCHIVEFILECACHE::~ARCHIVEFILECACHE() {
    if (this->m_map.m_buckets) {
        for (uint32_t i = 0; i < this->m_map.m_bucketCount; i++) {
            this->m_map.m_buckets[i] = 0xFFFFFFFF;
        }
    }
    for (uint32_t i = 0; i < this->m_map.m_count; i++) {
        if (this->m_map.m_entries[i].m_file) {
            this->m_map.m_entries[i].m_file->Release();
        }
    }
    this->m_map.m_count = 0;
    this->m_map.m_allocator->deallocate(this->m_map.m_buckets);
    this->m_map.m_allocator->deallocate(this->m_map.m_entries);
    this->m_map.m_allocator->deallocate(this->m_map.m_next);
    ListReset(&this->m_lru);
}

// Finds an open file and makes it the most recently used; the caller gets a reference.
// ref: FUN_1421666b0
ARCHIVEFILE* ARCHIVEFILECACHE::Find(uint64_t archive, uint8_t kind) {
    ARCHIVEKEY key = {};
    key.m_archive = archive;
    key.m_kind = kind;
    ARCHIVEMAP::ENTRY* entry = this->m_map.Find(&key);
    if (!entry) {
        return nullptr;
    }
    ARCHIVEFILE* file = entry->m_file;
    ARCHIVEFILE* first = this->m_lru.m_next == &this->m_lru ? nullptr : FileOfLink(this->m_lru.m_next);
    if (file != first) {
        ListUnlink(&file->m_link);
        ListInsertBefore(this->m_lru.m_next, &entry->m_file->m_link);
    }
    file = entry->m_file;
    if (file) {
        file->AddRef();
    }
    return file;
}

// Opens an archive file and caches it, evicting the least recently used one when full.
// ref: FUN_142166160
ARCHIVEFILE* ARCHIVEFILECACHE::Open(uint64_t archive, const char* path, int32_t mode, uint8_t kind, aio::CONTEXT* aio,
    void* a8, int32_t* error) {
    (void)a8;
    int32_t local;
    int32_t* result = error ? error : &local;
    if (kind == 2 && !aio) {
        blz::log(g_logCategory, 4, s_cacheFile, 0xcd, "Cannot open an async archive '%s' without an async instance", path);
        *result = 10;
        return nullptr;
    }
    if (static_cast<uint64_t>(this->m_map.m_count) == this->m_capacity) {
        // DIVERGED: with an empty list the reference removes the key at address 0x18 (a null
        // file's key); only a zero capacity reaches that, and it would fault. Skipped here.
        LISTLINK* last = this->m_lru.m_prev;
        if (last != &this->m_lru) {
            ListUnlink(last);
            this->m_map.Remove(&FileOfLink(last)->m_key);
        }
    }
    OSFILE file;
    OSFILE asyncFile;
    OsFileInit(&file);
    OsFileInit(&asyncFile);
    ARCHIVEFILE* opened = nullptr;
    if (kind == 0 || kind == 1) {
        uint32_t status = OpenArchiveFile(&file, path, mode, kind, this->m_exclusiveRead);
        if (status != 0) {
            LogOpenError(path, status);
            if (status == ERROR_FILE_NOT_FOUND) {
                *result = 2;
            } else {
                *result = status == ERROR_SHARING_VIOLATION ? 0x18 : 8;
            }
            OsFileClose(&asyncFile);
            OsFileClose(&file);
            return nullptr;
        }
    } else if (kind == 2) {
        uint8_t aioMode = static_cast<uint32_t>(mode) <= 5 ? static_cast<uint8_t>(mode) : 2;
        if (aio::FileOpen(aio, &asyncFile, path, aioMode) != 0) {
            blz::log(g_logCategory, 4, s_cacheFile, 0x102, "Error opening container archive file '%s'", path);
            *result = 8;
            OsFileClose(&asyncFile);
            OsFileClose(&file);
            return nullptr;
        }
    }
    *result = 0;
    blz::allocator* allocator = this->m_map.m_allocator;
    void* memory = allocator->allocate(sizeof(ARCHIVEFILE), 8);
    if (memory) {
        opened = new (memory) ARCHIVEFILE();
        opened->m_refs = 0;
        opened->m_allocator = allocator;
        opened->m_key = {};
        opened->m_key.m_archive = archive;
        opened->m_key.m_kind = kind;
        opened->m_28 = 1;
        OsFileInit(&opened->m_file);
        OsFileInit(&opened->m_asyncFile);
        ListInit(&opened->m_link);
        opened->AddRef();
    }
    OsFileMove(&opened->m_asyncFile, &asyncFile);
    OsFileMove(&opened->m_file, &file);
    bool inserted;
    this->m_map.Insert(&opened->m_key, &opened, &inserted);
    ListInsertBefore(this->m_lru.m_next, &opened->m_link);
    OsFileClose(&asyncFile);
    OsFileClose(&file);
    return opened;
}

// ref: FUN_1421664b0
void ARCHIVEFILECACHE::Clear() {
    ListReset(&this->m_lru);
    this->m_map.Clear();
}

// --- DYNAMICARCHIVECACHE -------------------------------------------------------------------

// ref: FUN_142157230
DYNAMICARCHIVECACHE::DYNAMICARCHIVECACHE(blz::allocator* allocator, uint64_t capacity)
    : ARCHIVEFILECACHE(allocator, capacity) {
    InitializeSRWLock(&this->m_lock);
}

// ref: FUN_142157260 (through FUN_142157290)
DYNAMICARCHIVECACHE::~DYNAMICARCHIVECACHE() {
}

// ref: FUN_1421572e0
void DYNAMICARCHIVECACHE::Clear() {
    AcquireSRWLockExclusive(&this->m_lock);
    ARCHIVEFILECACHE::Clear();
    ReleaseSRWLockExclusive(&this->m_lock);
}

// The archive's file, opening "<directory>/data.###" when it is not cached.
// ref: FUN_142157370
ARCHIVEFILE* DYNAMICARCHIVECACHE::Open(uint64_t archive, const char* directory, int32_t mode, int32_t* error,
    uint8_t kind, aio::CONTEXT* aio, void* a9) {
    *error = 0;
    AcquireSRWLockExclusive(&this->m_lock);
    ARCHIVEFILE* file = this->Find(archive, kind);
    if (!file) {
        char path[0x30C];
        std::memset(path, 0, sizeof(path));
        int32_t length;
        if (!directory) {
            length = OsSnprintf(path, sizeof(path), "data.%03d", static_cast<uint32_t>(archive));
        } else {
            length = OsSnprintf(path, sizeof(path), "%s/data.%03d", directory, static_cast<uint32_t>(archive));
        }
        if (static_cast<uint64_t>(static_cast<int64_t>(length)) > 0x30B) {
            blz::log(g_logCategory, 4, s_dynamicCacheFile, 0x2d, "Unsupported path length '%zu' requested. Max: '%d'",
                static_cast<int64_t>(length), 0x30C);
            *error = 10;
            ReleaseSRWLockExclusive(&this->m_lock);
            return nullptr;
        }
        file = ARCHIVEFILECACHE::Open(archive, path, mode, kind, aio, a9, error);
    }
    ReleaseSRWLockExclusive(&this->m_lock);
    return file;
}

}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_142166a40 = (void*)&casc::LogOpenError;
extern "C" void* const gen_f_142166aa0 = (void*)&casc::OpenArchiveFile;
// ---- end of generated globals ----
