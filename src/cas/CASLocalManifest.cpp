#include "cas/CASLocalManifest.hpp"
#include "cas/CAS.hpp"
#include "blz/Murmur.hpp"
#include "blz/String.hpp"
#include "blz/Sync.hpp"
#include "casc/Hash.hpp"
#include "os/Os.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include <cstring>
#include <malloc.h>
#include <new>

namespace cas {

namespace {

const char s_nameNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CB_KULocalManifestEntry@@@blz@@@blz@@";
const char s_nameBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CB_KULocalManifestEntry@@@blz@@@blz@@";
const char s_idNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBHPEAULocalManifestEntry@@@blz@@@blz@@";
const char s_idBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBHPEAULocalManifestEntry@@@blz@@@blz@@";
const char s_handlerNodeTag[] =
    ".?AU?$chained_hash_node@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@blz@@V?$allocator@D@2@@blz@@"
    "P6A_NPEBD0PEAD_K@Z@blz@@@blz@@";
const char s_handlerBucketTag[] =
    ".PEAU?$chained_hash_node@U?$pair@$$CBV?$basic_string@DU?$char_traits@D@blz@@V?$allocator@D@2@@blz@@"
    "P6A_NPEBD0PEAD_K@Z@blz@@@blz@@";

// DAT_142e257a8 guards the by-name table, DAT_142e257c8 the by-id table.
blz::shared_mutex s_nameLock;
blz::shared_mutex s_idLock;

// The extension handlers (DAT_142ccbaa0), a chained map from blz::string.
struct HANDLERNODE {
    HANDLERNODE* m_next;            // 0x00
    blz::string m_key;              // 0x08
    LOCALFILEHANDLER m_handler;     // 0x30
};
static_assert(sizeof(HANDLERNODE) == 0x38, "HANDLERNODE is 0x38 bytes");

struct HANDLERTABLE {
    uint64_t m_capacity;
    HANDLERNODE** m_buckets;
    uint64_t m_count;
    float m_maxLoad;
};
HANDLERTABLE s_handlers = { 8, nullptr, 0, 1.0f };

// Files the walk never enters, by extension (the empty one too: a file without one).
const char* const s_excluded[] = {
    "", "bak", "bat", "bin", "cfg", "dat", "db", "dbs", "dll", "dmp", "dylib", "exp", "exe", "idx", "ilk", "index",
    "ini", "lib", "log", "md5", "metagen", "old", "pak", "pdb", "pl", "pm", "py", "sh", "signed", "tmp", "wdb",
    "zip",
};

const char* TagOf(LOCALNAMENODE*) {
    return s_nameNodeTag;
}
const char* TagOf(LOCALIDNODE*) {
    return s_idNodeTag;
}
const char* BucketTagOf(LOCALNAMENODE*) {
    return s_nameBucketTag;
}
const char* BucketTagOf(LOCALIDNODE*) {
    return s_idBucketTag;
}

uint64_t KeyHash(const LOCALNAMENODE* node) {
    return blz::fmix64(node->m_key);
}
uint64_t KeyHash(const LOCALIDNODE* node) {
    return blz::fmix64(static_cast<uint64_t>(static_cast<int64_t>(node->m_key)));
}

uint64_t ChainedCapacity(uint64_t count, float maxLoad, uint64_t minimum) {
    float load = static_cast<float>(count);
    uint64_t need = static_cast<uint64_t>(load / maxLoad);
    if (load > static_cast<float>(need) * maxLoad) {
        need++;
    }
    if (need < minimum) {
        need = minimum;
    }
    uint64_t capacity = 8;
    if (need > 8) {
        do {
            capacity *= 2;
        } while (capacity < need);
    }
    return capacity;
}

// The walk of one tree level and its visitor.
void ScanDirectory(const char* base, LOCALSCAN* scan);
bool VisitFile(OSFILEINFO* info, void* param);

// Copies `function` into fresh storage (the manager's copy), as blz::function's copy does.
void CopyFunction(blz::function_storage* dest, const blz::function_storage* source) {
    dest->m_invoke = nullptr;
    dest->m_manager = &blz::function_empty_manager;
    if (dest != source) {
        source->m_manager(dest->m_storage, const_cast<uint8_t*>(source->m_storage), 1);
        dest->m_invoke = source->m_invoke;
        dest->m_manager = source->m_manager;
    }
}

void DestroyFunction(blz::function_storage* function) {
    if (function->m_manager != &blz::function_empty_manager) {
        function->m_manager(function->m_storage, nullptr, 0);
    }
}

// Appends `text` to the string in `buffer` (0x104 bytes), as far as it fits.
void Append(char* buffer, const char* text) {
    char* end = buffer;
    uint64_t room = 0x104;
    while (room != 0 && *end != '\0') {
        end++;
        room--;
    }
    room = static_cast<uint64_t>(buffer + 0x104 - end);
    if (room != 0) {
        for (; *text != '\0'; text++) {
            if (room != 1) {
                *end++ = *text;
                room--;
            }
        }
        *end = '\0';
    }
}

void CopyBounded(char* dest, const char* source) {
    for (int64_t left = 0x103;;) {
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
        if (--left == 0) {
            *dest = '\0';
            return;
        }
    }
}

// ref: FUN_1402da3c0
void RehashHandlers(uint64_t minimum) {
    HANDLERTABLE* table = &s_handlers;
    uint64_t capacity = ChainedCapacity(table->m_count, table->m_maxLoad, minimum);
    if (capacity == table->m_capacity && table->m_buckets != nullptr) {
        return;
    }
    auto buckets = static_cast<HANDLERNODE**>(
        SMemAlloc(capacity * 8, 0xB, s_handlerBucketTag, SStrHashFnv1a(s_handlerBucketTag), -6, 0));
    if (capacity != 0) {
        std::memset(buckets, 0, capacity * 8);
    }
    if (table->m_buckets != nullptr && table->m_capacity != 0) {
        for (uint64_t i = 0; i < table->m_capacity; i++) {
            for (HANDLERNODE* node = table->m_buckets[i]; node != nullptr;) {
                HANDLERNODE* next = node->m_next;
                uint64_t hash[2];
                blz::murmur3_128(hash, node->m_key.m_data, node->m_key.m_size, 0);
                uint64_t slot = (capacity - 1) & hash[0];
                node->m_next = buckets[slot];
                buckets[slot] = node;
                node = next;
            }
        }
    }
    SMemFree(table->m_buckets, s_handlerBucketTag, -6, 0);
    table->m_buckets = buckets;
    table->m_capacity = capacity;
}

// The handler for a lower-cased dot extension, or null.
LOCALFILEHANDLER FindHandler(const blz::string& key) {
    if (s_handlers.m_buckets == nullptr) {
        return nullptr;
    }
    uint64_t hash[2];
    blz::murmur3_128(hash, key.m_data, key.m_size, 0);
    for (HANDLERNODE* node = s_handlers.m_buckets[(s_handlers.m_capacity - 1) & hash[0]]; node != nullptr;
         node = node->m_next) {
        uint64_t compare = key.m_size < node->m_key.m_size ? key.m_size : node->m_key.m_size;
        if ((compare == 0 || std::memcmp(key.m_data, node->m_key.m_data, compare) == 0)
            && static_cast<int32_t>(key.m_size) == static_cast<int32_t>(node->m_key.m_size)) {
            return node->m_handler;
        }
    }
    return nullptr;
}

// The default: the path as it is.
// ref: FUN_1402dbcb0
bool CopyName(const char* extension, const char* path, char* dest, uint64_t size) {
    (void)extension;
    if (size != 0) {
        for (;;) {
            if (--size == 0) {
                *dest = '\0';
                return true;
            }
            char c = *path++;
            *dest++ = c;
            if (c == '\0') {
                break;
            }
        }
    }
    return true;
}

// The name a loose file is looked up by.
// ref: FUN_1402d7750
bool LookupName(LOCALMANIFEST* manifest, const char* path, char* dest, uint64_t size) {
    (void)manifest;
    char native[0x110];
    OsPathToNative(path, native, 0x104);
    const char* extension = OsPathGetDotExtension(native);
    char lower[0x110];
    CopyBounded(lower, extension);
    _strlwr_s(lower, sizeof(lower));
    blz::string key;
    key.assign(lower);
    LOCALFILEHANDLER handler = FindHandler(key);
    if (handler != nullptr) {
        return handler(extension, native, dest, size);
    }
    return CopyName(extension, native, dest, size);
}

template <class NODE>
NODE* ChainedFind(LOCALTABLE<NODE>* table, NODE** slot, decltype(NODE::m_key) key, NODE*** link) {
    (void)table;
    NODE** at = slot;
    for (NODE* node = *slot; node != nullptr; node = node->m_next) {
        if (node->m_key == key) {
            break;
        }
        at = &node->m_next;
    }
    *link = at;
    return *at;
}

// Inserts unless the key is present; returns the node either way.
// ref: FUN_1402d9db0 (by name), FUN_1402d9bc0 (by id)
template <class NODE>
NODE* ChainedInsert(LOCALTABLE<NODE>* table, const NODE* proto) {
    uint64_t hash = KeyHash(proto);
    if (table->m_buckets == nullptr) {
        table->Rehash(table->m_capacity);
    }
    NODE** link;
    NODE* node = ChainedFind(table, &table->m_buckets[(table->m_capacity - 1) & hash], proto->m_key, &link);
    if (node != nullptr) {
        return node;
    }
    node = static_cast<NODE*>(SMemAlloc(sizeof(NODE), 0xB, TagOf(node), SStrHashFnv1a(TagOf(node)), -6, 0));
    if (node != nullptr) {
        std::memcpy(node, proto, sizeof(NODE));
        node->m_next = nullptr;
    }
    *link = node;
    table->m_count++;
    float load = static_cast<float>(table->m_count) / static_cast<float>(table->m_capacity);
    if (table->m_maxLoad < load) {
        table->Rehash(table->m_capacity * 2);
    }
    return node;
}

// ref: FUN_1402d9a00
bool RemoveById(LOCALIDTABLE* table, int32_t key) {
    if (table->m_buckets == nullptr) {
        return false;
    }
    uint64_t hash = blz::fmix64(static_cast<uint64_t>(static_cast<int64_t>(key)));
    LOCALIDNODE** link = &table->m_buckets[(table->m_capacity - 1) & hash];
    for (LOCALIDNODE* node = *link; node != nullptr && node->m_key != key; node = node->m_next) {
        link = &node->m_next;
    }
    LOCALIDNODE* node = *link;
    if (node == nullptr) {
        return false;
    }
    *link = node->m_next;
    SMemFree(node, s_idNodeTag, -6, 0);
    table->m_count--;
    return true;
}

// ref: FUN_1402d9bc0
void EnterById(LOCALMANIFEST* manifest, int32_t fileDataId, LocalManifestEntry* entry) {
    LOCALIDNODE proto;
    proto.m_next = nullptr;
    proto.m_key = fileDataId;
    proto.m_value = entry;
    ChainedInsert(&manifest->m_byId, &proto);
}

// ref: FUN_1402d7550
void ScanDirectory(const char* base, LOCALSCAN* scan) {
    char path[0x110];
    CopyBounded(path + 1, base);
    Append(path + 1, scan->m_directory);
    uint64_t length = std::strlen(path + 1);
    if (length >= 0x103) {
        return;
    }
    if (length != 0 && path[length] != '\\' && path[length] != '/') {
        Append(path + 1, "\\");
    }

    LOCALSCAN next;
    next.m_directory = path + 1;
    next.m_manifest = scan->m_manifest;
    CopyFunction(&next.m_onExisting, &scan->m_onExisting);

    blz::function_storage visit;
    visit.m_invoke = reinterpret_cast<void*>(&OsFindFilesInvokePointer);
    visit.m_manager = &blz::function_trivial_manager;
    auto visitor = &VisitFile;
    std::memcpy(visit.m_storage, &visitor, sizeof(visitor));
    OsFindFiles(path + 1, "*", &visit, &next, false);

    DestroyFunction(&next.m_onExisting);
}

// ref: FUN_1402d71b0
bool VisitFile(OSFILEINFO* info, void* param) {
    auto scan = static_cast<LOCALSCAN*>(param);
    const char* name = info->m_name;
    const char* directory = scan->m_directory;
    if ((info->m_attributes & OS_FILE_DIRECTORY) == 0) {
        if (_stricmp(name, "addons.txt") != 0 || SStrStrI(info->m_directory, "wtf") == nullptr) {
            const char* extension = OsPathGetExtension(name);
            for (const char* excluded : s_excluded) {
                if (_stricmp(extension, excluded) == 0) {
                    return false;
                }
            }
        }
        char full[0x110] = {};
        Append(full, directory);
        Append(full, name);
        LOCALMANIFEST* manifest = scan->m_manifest;
        uint64_t rootLength = std::strlen(manifest->m_root);
        const char* relative = full + rootLength;
        if (rootLength != 0 && manifest->m_root[rootLength - 1] != '\\' && manifest->m_root[rootLength - 1] != '/') {
            relative++;
        }
        manifest->Add(0, relative, manifest->m_root, 1, &scan->m_onExisting);
    } else if (*name != '.') {
        LOCALSCAN sub;
        sub.m_directory = name;
        sub.m_manifest = scan->m_manifest;
        CopyFunction(&sub.m_onExisting, &scan->m_onExisting);
        ScanDirectory(directory, &sub);
        DestroyFunction(&sub.m_onExisting);
    }
    return false;
}

}

template <class NODE>
// ref: FUN_1402da190
void LOCALTABLE<NODE>::Rehash(uint64_t minimum) {
    uint64_t capacity = ChainedCapacity(this->m_count, this->m_maxLoad, minimum);
    if (capacity == this->m_capacity && this->m_buckets != nullptr) {
        return;
    }
    NODE* tag = nullptr;
    auto buckets = static_cast<NODE**>(SMemAlloc(capacity * 8, 0xB, BucketTagOf(tag), SStrHashFnv1a(BucketTagOf(tag)), -6, 0));
    if (capacity != 0) {
        std::memset(buckets, 0, capacity * 8);
    }
    if (this->m_buckets != nullptr && this->m_capacity != 0) {
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            for (NODE* node = this->m_buckets[i]; node != nullptr;) {
                NODE* next = node->m_next;
                uint64_t slot = (capacity - 1) & KeyHash(node);
                node->m_next = buckets[slot];
                buckets[slot] = node;
                node = next;
            }
        }
    }
    SMemFree(this->m_buckets, BucketTagOf(tag), -6, 0);
    this->m_buckets = buckets;
    this->m_capacity = capacity;
}

// The node walk of SetRoot (inlined there): frees every node and empties the buckets.
template <class NODE>
void LOCALTABLE<NODE>::Clear() {
    if (this->m_buckets != nullptr && this->m_capacity != 0) {
        NODE* tag = nullptr;
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            for (NODE* node = this->m_buckets[i]; node != nullptr;) {
                NODE* next = node->m_next;
                SMemFree(node, TagOf(tag), -6, 0);
                node = next;
            }
            this->m_buckets[i] = nullptr;
        }
    }
    this->m_count = 0;
}

// ref: FUN_1402cc050 (by id), FUN_1402cc0e0 (by name)
template <class NODE>
void LOCALTABLE<NODE>::Destroy() {
    if (this->m_buckets != nullptr) {
        NODE* tag = nullptr;
        for (uint64_t i = 0; i < this->m_capacity; i++) {
            for (NODE* node = this->m_buckets[i]; node != nullptr;) {
                NODE* next = node->m_next;
                SMemFree(node, TagOf(tag), -6, 0);
                node = next;
            }
        }
        SMemFree(this->m_buckets, BucketTagOf(tag), -6, 0);
        this->m_buckets = nullptr;
    }
}

template struct LOCALTABLE<LOCALNAMENODE>;
template struct LOCALTABLE<LOCALIDNODE>;

// The construction CASInitialize inlines (0x1402cec40).
void LOCALMANIFEST::Construct() {
    this->m_nextLocalId = 0;
    this->m_byName = { 8, nullptr, 0, 1.0f };
    this->m_byId = { 8, nullptr, 0, 1.0f };
}

void LOCALMANIFEST::Destroy() {
    this->m_byId.Destroy();
    this->m_byName.Destroy();
}

// ref: FUN_1402d8640
void LOCALMANIFEST::SetRoot(const char* root) {
    CopyBounded(this->m_root, root);
    this->m_nextLocalId = 0;
    LOCALSCAN scan;
    scan.m_directory = "";
    scan.m_manifest = this;
    scan.m_onExisting.m_invoke = nullptr;
    scan.m_onExisting.m_manager = &blz::function_empty_manager;
    this->m_byId.Clear();
    this->m_byId.Rehash(static_cast<uint64_t>(1024.0f / this->m_byId.m_maxLoad + 0.5f));
    this->m_byName.Clear();
    ScanDirectory(root, &scan);
    DestroyFunction(&scan.m_onExisting);
}

// ref: FUN_1402d6c40
int32_t LOCALMANIFEST::Add(int32_t fileDataId, const char* path, const char* root, uint8_t byId,
    blz::function_storage* onExisting) {
    char name[0x110];
    if (!LookupName(this, path, name, 0x104)) {
        return 0;
    }
    uint64_t hash = LocalManifestNameHash(name);

    LocalManifestEntry* entry = nullptr;
    s_nameLock.lock_shared();
    if (this->m_byName.m_buckets != nullptr) {
        uint64_t slot = (this->m_byName.m_capacity - 1) & blz::fmix64(hash);
        LOCALNAMENODE** link;
        LOCALNAMENODE* node = ChainedFind(&this->m_byName, &this->m_byName.m_buckets[slot], hash, &link);
        if (node != nullptr) {
            entry = &node->m_value;
            if (onExisting->m_invoke != nullptr) {
                typedef void (*INVOKE)(LocalManifestEntry*, const char*, void*);
                reinterpret_cast<INVOKE>(onExisting->m_invoke)(entry, root ? root : path, onExisting->m_storage);
            }
        }
    }
    s_nameLock.unlock_shared();

    if (entry == nullptr) {
        LOCALNAMENODE proto;
        proto.m_next = nullptr;
        proto.m_key = hash;
        proto.m_value.m_path[0] = '\0';
        proto.m_value.m_fileDataId = fileDataId;
        // DIVERGED: with a caller-given id the reference leaves this byte as stack garbage;
        // every caller passes 0.
        proto.m_value.m_byId = 0;
        if (fileDataId == 0) {
            proto.m_value.m_fileDataId = CASGetFileDataIdByName(name);
            if (proto.m_value.m_fileDataId == 0) {
                this->m_nextLocalId--;
                proto.m_value.m_fileDataId = this->m_nextLocalId;
            }
            proto.m_value.m_byId = byId;
        }
        s_nameLock.lock();
        entry = &ChainedInsert(&this->m_byName, &proto)->m_value;
        s_nameLock.unlock();
        if (byId == 0) {
            return entry->m_fileDataId;
        }
        s_idLock.lock();
        EnterById(this, proto.m_value.m_fileDataId, entry);
        s_idLock.unlock();
    } else {
        if (fileDataId != 0) {
            s_idLock.lock();
            RemoveById(&this->m_byId, entry->m_fileDataId);
            entry->m_fileDataId = fileDataId;
            EnterById(this, fileDataId, entry);
            s_idLock.unlock();
        }
        if (entry->m_byId == 0 && fileDataId == 0) {
            entry->m_byId = 1;
            s_idLock.lock();
            EnterById(this, entry->m_fileDataId, entry);
            s_idLock.unlock();
        }
        if (byId == 0) {
            return entry->m_fileDataId;
        }
    }

    if (root == nullptr) {
        CopyBounded(entry->m_path, path);
    } else {
        OsPathCombine(entry->m_path, 0x104, root, path);
    }
    return entry->m_fileDataId;
}

// ref: FUN_1402d7dc0
LocalManifestEntry* LOCALMANIFEST::FindById(int32_t fileDataId) {
    s_idLock.lock_shared();
    LocalManifestEntry* entry = nullptr;
    if (this->m_byId.m_buckets != nullptr) {
        uint64_t slot = (this->m_byId.m_capacity - 1) & blz::fmix64(static_cast<uint64_t>(static_cast<int64_t>(fileDataId)));
        LOCALIDNODE** link;
        LOCALIDNODE* node = ChainedFind(&this->m_byId, &this->m_byId.m_buckets[slot], fileDataId, &link);
        if (node != nullptr && node->m_value->m_fileDataId == fileDataId) {
            entry = node->m_value;
        }
    }
    s_idLock.unlock_shared();
    return entry;
}

// ref: FUN_1402d7e90
const char* LOCALMANIFEST::FindPathByName(uint64_t nameHash, int32_t* fileDataId) {
    s_nameLock.lock_shared();
    const char* path = nullptr;
    if (this->m_byName.m_buckets != nullptr) {
        uint64_t slot = (this->m_byName.m_capacity - 1) & blz::fmix64(nameHash);
        LOCALNAMENODE** link;
        LOCALNAMENODE* node = ChainedFind(&this->m_byName, &this->m_byName.m_buckets[slot], nameHash, &link);
        if (node != nullptr && node->m_value.m_byId != 0) {
            if (fileDataId != nullptr) {
                *fileDataId = node->m_value.m_fileDataId;
            }
            path = node->m_value.m_path;
        }
    }
    if (path == nullptr && fileDataId != nullptr) {
        *fileDataId = 0;
    }
    s_nameLock.unlock_shared();
    return path;
}

// The id of the loose file a name maps to, or 0.
// ref: FUN_1402d7f90
int32_t LOCALMANIFEST::FindIdByName(const char* path) {
    char name[0x110];
    if (!LookupName(this, path, name, 0x104)) {
        CopyBounded(name, path);
    }
    uint64_t hash = LocalManifestNameHash(name);
    int32_t fileDataId = 0;
    s_nameLock.lock_shared();
    if (this->m_byName.m_buckets != nullptr) {
        uint64_t slot = (this->m_byName.m_capacity - 1) & blz::fmix64(hash);
        LOCALNAMENODE** link;
        LOCALNAMENODE* node = ChainedFind(&this->m_byName, &this->m_byName.m_buckets[slot], hash, &link);
        if (node != nullptr && node->m_value.m_byId != 0) {
            fileDataId = node->m_value.m_fileDataId;
        }
    }
    s_nameLock.unlock_shared();
    return fileDataId;
}

// ref: FUN_1402d9440
void LocalManifestRegisterHandler(LOCALFILEHANDLER handler, const char* extension) {
    char lower[0x110];
    CopyBounded(lower, extension);
    _strlwr_s(lower, sizeof(lower));
    blz::string key;
    key.assign(lower);
    if (s_handlers.m_buckets == nullptr) {
        RehashHandlers(s_handlers.m_capacity);
    }
    uint64_t hash[2];
    blz::murmur3_128(hash, key.m_data, key.m_size, 0);
    HANDLERNODE** bucket = &s_handlers.m_buckets[(s_handlers.m_capacity - 1) & hash[0]];
    HANDLERNODE* found = nullptr;
    for (HANDLERNODE* node = *bucket; node != nullptr; node = node->m_next) {
        uint64_t compare = key.m_size < node->m_key.m_size ? key.m_size : node->m_key.m_size;
        if ((compare == 0 || std::memcmp(key.m_data, node->m_key.m_data, compare) == 0)
            && static_cast<int32_t>(key.m_size) == static_cast<int32_t>(node->m_key.m_size)) {
            found = node;
            break;
        }
    }
    if (found == nullptr) {
        found = static_cast<HANDLERNODE*>(
            SMemAlloc(sizeof(HANDLERNODE), 0xB, s_handlerNodeTag, SStrHashFnv1a(s_handlerNodeTag), -6, 0));
        if (found != nullptr) {
            found->m_next = *bucket;
            new (&found->m_key) blz::string(static_cast<blz::string&&>(key));
            found->m_handler = nullptr;
        }
        *bucket = found;
        s_handlers.m_count++;
        if (s_handlers.m_maxLoad < static_cast<float>(s_handlers.m_count) / static_cast<float>(s_handlers.m_capacity)) {
            RehashHandlers(s_handlers.m_capacity * 2);
        }
    }
    found->m_handler = handler;
}

// ref: FUN_1402d6470
void LocalManifestDestroyHandlers() {
    if (s_handlers.m_buckets == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < s_handlers.m_capacity; i++) {
        for (HANDLERNODE* node = s_handlers.m_buckets[i]; node != nullptr;) {
            HANDLERNODE* next = node->m_next;
            node->m_key.~string();
            SMemFree(node, s_handlerNodeTag, -6, 0);
            node = next;
        }
    }
    SMemFree(s_handlers.m_buckets, s_handlerBucketTag, -6, 0);
    s_handlers.m_buckets = nullptr;
}

// ref: FUN_1402d4300
uint64_t LocalManifestNameHash(const char* path) {
    uint64_t length = std::strlen(path);
    auto upper = static_cast<char*>(_alloca(length + 1));
    for (uint64_t i = 0; i < length; i++) {
        char c = path[i];
        if (static_cast<uint8_t>(c - 'a') < 26) {
            c = static_cast<char>(c & 0xDF);
        } else if (c == '/') {
            c = '\\';
        }
        upper[i] = c;
    }
    upper[length] = '\0';
    uint32_t pc = 0;
    uint32_t pb = 0;
    casc::HashLittle2(upper, length, &pc, &pb);
    return (static_cast<uint64_t>(pc) << 32) | pb;
}

}
