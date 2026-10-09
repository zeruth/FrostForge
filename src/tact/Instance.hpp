#ifndef TACT_INSTANCE_HPP
#define TACT_INSTANCE_HPP

// pkg/tact/3.6.39/src/instance.cpp: the tact instance (0x11c0 bytes) - the op registry, the
// build configuration, the storage container, the scheduler and the manifests read at
// initialization. Members that belong to subsystems not ported yet are laid out by size, with
// their constructors ported so every offset holds what the reference puts there.

#include "tact/BlockCache.hpp"
#include "tact/EncodingTable.hpp"
#include "tact/FileManifest.hpp"
#include "tact/PatchIndex.hpp"
#include "tact/ReadQueue.hpp"
#include "blz/AllocString.hpp"
#include "blz/ObjectPool.hpp"
#include "tact/AsyncState.hpp"
#include "tact/Common.hpp"
#include "tact/TaskScheduler.hpp"
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace tact {

class Instance;
class Container;

// A content/encoding record from the build configuration (0x48 bytes): content key and size,
// encoding key and size, and the encoding spec (default "b:256K*=z").
struct CONFIGRECORD {
    uint8_t m_ckey[16];             // 0x00
    uint32_t m_csize;               // 0x10
    uint8_t m_ekey[16];             // 0x14
    uint32_t m_esize;               // 0x24
    blz::alloc_string m_eSpec;      // 0x28

    void Init();
    void CopyKeyTo(struct FILEKEY* out) const;
};
static_assert(sizeof(CONFIGRECORD) == 0x48, "CONFIGRECORD is 0x48 bytes");

// What a file read is asked for (0x40 bytes), built from a record: the keys and sizes, the
// spec text and its flags.
struct FILEKEY {
    uint8_t m_ckey[16];             // 0x00
    uint32_t m_csize;               // 0x10
    uint8_t m_ekey[16];             // 0x14
    uint32_t m_esize;               // 0x24
    const char* m_eSpec;            // 0x28
    uint8_t m_eSpecFlags;           // 0x30: 0x10
    uint8_t m_31;                   // 0x31
    uint64_t m_38;                  // 0x38
};
static_assert(sizeof(FILEKEY) == 0x40, "FILEKEY is 0x40 bytes");

// The six records DynamicInit reads by index.
enum {
    CONFIG_ENCODING = 3,
    CONFIG_ROOT = 4,
    CONFIG_PATCH_INDEX = 5,
};

// A static key layout ("key-layout-<n> = chunk archive offset alignment").
struct KEYLAYOUT {
    uint8_t m_chunkBits;
    uint8_t m_archiveBits;
    uint8_t m_offsetBits;
    uint8_t m_03;
    uint32_t m_alignment;
};

// A "build-partial-priority" entry: key:size.
struct PARTIALPRIORITY {
    uint8_t m_key[16];
    uint32_t m_size;
};

// The parsed build configuration (0x258 bytes, at instance+0x50). The records are, in order,
// download, install, size, encoding, vfs-root (the root manifest) and patch-index.
struct BUILDCONFIG {
    uint8_t m_patchKey[16];         // 0x00: "patch"
    uint32_t m_patchSize;           // 0x10: "patch-size"
    uint8_t m_patchConfig[16];      // 0x14: "patch-config"
    uint8_t m_rootKey[16];          // 0x24: "root"
    uint8_t m_34[4];                // 0x34
    CONFIGRECORD m_records[6];      // 0x38
    uint8_t m_keyLayoutIndexBits;   // 0x1e8
    Array<KEYLAYOUT> m_keyLayouts;  // 0x1f0: "key-layout-<n>"
    Array<CONFIGRECORD> m_vfs;      // 0x208: "vfs-<n>"
    Array<PARTIALPRIORITY> m_partialPriority;  // 0x220
    Array<char> m_buildLines;       // 0x238: every other "build-*" line, "key = value\0"
    uint8_t m_250;                  // 0x250

    void Init();
    void Destroy();
};
static_assert(sizeof(BUILDCONFIG) == 0x258, "BUILDCONFIG is 0x258 bytes");

// The op registry (0x48 bytes, at instance+8): every live operation by id, in a chained hash
// map whose entries are {id, state} pairs.
struct OPENTRY {
    uint64_t m_id;
    AsyncState* m_state;
};

struct OPMAP {
    uint32_t* m_buckets;            // 0x00
    OPENTRY* m_entries;             // 0x08
    uint32_t* m_next;               // 0x10
    blz::allocator* m_allocator;    // 0x18
    uint32_t m_bucketCount;         // 0x20
    uint32_t m_count;               // 0x24
    uint32_t m_capacity;            // 0x28

    static uint32_t Hash(uint64_t id);
    void Rehash(uint32_t buckets);
    void Grow(uint32_t capacity);
    OPENTRY* Insert(uint64_t* id, AsyncState** state, bool* inserted);
    bool Remove(const uint64_t* id);
    void Destroy();
};

struct OPREGISTRY {
    Instance* m_instance;           // 0x00
    SRWLOCK m_lock;                 // 0x08
    uint64_t m_nextId;              // 0x10
    OPMAP m_map;                    // 0x18

    void Init(Instance* instance);
    void Destroy();
    OPHANDLE* Find(OPHANDLE* out, uint64_t id);
    OPHANDLE* FindShared(OPHANDLE* out, uint64_t id);
    void Release(OPHANDLE* handle);
    uint8_t WaitResult(uint64_t id, int32_t* detail);
    void WaitAll();
    uint64_t Register(AsyncState* state);
};
static_assert(sizeof(OPREGISTRY) == 0x48, "OPREGISTRY is 0x48 bytes");

// The scratch buffer (0x18 bytes, at instance+0xa10): 128 KB pages handed out in 16-byte
// aligned slices with a 16-byte header each, freed when their last slice is returned.
struct SCRATCHBUFFER {
    int32_t* m_current;             // 0x00
    int32_t* m_first;               // 0x08
    SRWLOCK m_lock;                 // 0x10

    void Init();
    void Destroy();
    static int32_t* NewPage(SCRATCHBUFFER* buffer, uint32_t size);
    static void* Slice(SCRATCHBUFFER* buffer, int32_t* page, uint32_t size);
    void* Allocate(uint32_t size);
    void Free(void* data);
};

// The parameters the client passes to Create (0x1e0 bytes; DynamicInit keeps a copy).
struct INSTANCEPARAMS {
    uint8_t m_mode;                 // 0x00: 0 dynamic (local storage), 1 and 2 other modes
    uint8_t m_01;                   // 0x01
    uint64_t m_08;                  // 0x08: -1
    const char* m_storagePath;      // 0x10
    uint8_t m_buildKey[16];         // 0x18
    const char* m_buildConfigPath;  // 0x28
    uint8_t m_cdnKey[16];           // 0x30
    const char* m_cdnPath;          // 0x40
    const char** m_cdnHosts;        // 0x48
    uint32_t m_cdnHostCount;        // 0x50
    const char* m_armadilloKey;     // 0x58
    uint32_t m_60;                  // 0x60: 200
    uint8_t m_64;                   // 0x64
    uint8_t m_65;                   // 0x65: copied to instance+0xa80
    uint32_t m_68;                  // 0x68: 0x10
    uint32_t m_6c;                  // 0x6c
    uint32_t m_70;                  // 0x70: 0x10
    uint32_t m_74;                  // 0x74: 0x20, streaming connections
    uint32_t m_78;                  // 0x78: 0x20000
    int32_t m_streaming;            // 0x7c: 1 = no streaming
    void* m_ioContext;              // 0x80: an existing aio context, or null for a new one
    uint64_t m_88;                  // 0x88
    uint64_t m_affinity;            // 0x90
    int32_t m_priority;             // 0x98
    uint32_t m_threads;             // 0x9c
    uint64_t m_a0;                  // 0xa0
    uint32_t m_a8;                  // 0xa8
    double m_b0;                    // 0xb0: 0.75
    uint64_t m_b8;                  // 0xb8
    uint64_t m_c0;                  // 0xc0
    uint8_t m_c8;                   // 0xc8
    uint64_t m_d0[4];               // 0xd0
    void* m_f0;                     // 0xf0
    void* m_f8;                     // 0xf8
    uint64_t m_100[6];              // 0x100: to instance+0xf68..0xf90
    void* m_onFileFetched;          // 0x130
    void* m_138;                    // 0x138
    void* m_140;                    // 0x140: to instance+0xf58
    void* m_148;                    // 0x148: to instance+0xf60
    void* m_150;                    // 0x150
    void* m_158;                    // 0x158
    POSTFUNCTION m_post;            // 0x160: the client's scheduler, or null for tact's own pool
    void* m_postContext;            // 0x168
    void (*m_postRelease)(void*);   // 0x170
    void* m_postReleaseContext;     // 0x178
    uint64_t m_180[2];              // 0x180
    uint8_t m_loadEncodingTable;    // 0x190
    void* m_198;                    // 0x198
    void* m_1a0;                    // 0x1a0
    void* m_progress;               // 0x1a8: progress callback
    void* m_progressContext;        // 0x1b0
    uint8_t m_1b8;                  // 0x1b8
    const char* m_1c0;              // 0x1c0
    uint64_t m_1c8;                 // 0x1c8
    uint64_t m_1d0;                 // 0x1d0
    uint8_t m_1d8;                  // 0x1d8
    uint8_t m_1d9;                  // 0x1d9
};
static_assert(offsetof(INSTANCEPARAMS, m_streaming) == 0x7c, "INSTANCEPARAMS layout");
static_assert(offsetof(INSTANCEPARAMS, m_post) == 0x160, "INSTANCEPARAMS layout");
static_assert(offsetof(INSTANCEPARAMS, m_progress) == 0x1a8, "INSTANCEPARAMS layout");
static_assert(sizeof(INSTANCEPARAMS) == 0x1e0, "INSTANCEPARAMS is 0x1e0 bytes");

class Instance {
    public:
    void* m_ioContext;              // 0x000
    OPREGISTRY m_ops;               // 0x008
    BUILDCONFIG m_buildConfig;      // 0x050
    uint8_t m_2a8[0x30];            // 0x2a8
    struct {
        Instance* m_instance;
        void* m_08;
        void* m_10;
        void* m_18;
        void* m_20;
    } m_cdnCallbacks;               // 0x2d8
    FILEMANIFEST m_fileManifest;    // 0x300
    READQUEUE m_readQueue;          // 0x378
    uint8_t m_downloadQueue[0x590]; // 0x480
    SCRATCHBUFFER m_scratch;        // 0xa10
    uint8_t m_a28[0x50];            // 0xa28
    BLOCKCACHE m_blockCache;        // 0xa78
    ENCODINGTABLE m_encodingTable;  // 0xd90
    Container* m_container;         // 0xe00
    TaskSchedulerBase* m_scheduler; // 0xe08
    void* m_memoryLimit;            // 0xe10
    blz::object_pool m_fileReadPool;// 0xe18: 0x268-byte file read states
    SRWLOCK m_fileReadPoolLock;     // 0xeb0
    blz::object_pool m_eb8;         // 0xeb8: 0x110-byte objects
    SRWLOCK m_eb8Lock;              // 0xf50
    void* m_f58;                    // 0xf58
    void* m_f60;                    // 0xf60
    uint64_t m_f68[6];              // 0xf68
    uint8_t m_patchManifest[0x90];  // 0xf98
    PATCHINDEX m_patchIndex;        // 0x1028
    blz::alloc_string m_storagePath;// 0x1198
    uint8_t m_initialized;          // 0x11b8
    uint8_t m_ownsIoContext;        // 0x11b9

    static Instance* Construct(blz::allocator* allocator, const char* tag, int32_t line);
    static Instance* Create(INSTANCEPARAMS* params, uint8_t* error);
    static uint64_t InitializeAsync(Instance** out, INSTANCEPARAMS* params, OPCALLBACK callback, void* context);
    static void Destroy(Instance* instance);

    uint8_t CreateIoContext(uint8_t owns);
    uint8_t InitializeContainer(const char* path, int32_t streaming, uint32_t a4, uint32_t a5, uint32_t a6,
        uint8_t a7, uint64_t a8, uint8_t a9);
};
static_assert(offsetof(Instance, m_buildConfig) == 0x50, "Instance layout");
static_assert(offsetof(Instance, m_downloadQueue) == 0x480, "Instance layout");
static_assert(offsetof(Instance, m_scratch) == 0xa10, "Instance layout");
static_assert(offsetof(Instance, m_container) == 0xe00, "Instance layout");
static_assert(offsetof(Instance, m_fileReadPool) == 0xe18, "Instance layout");
static_assert(offsetof(Instance, m_patchManifest) == 0xf98, "Instance layout");
static_assert(offsetof(Instance, m_storagePath) == 0x1198, "Instance layout");
static_assert(sizeof(Instance) == 0x11c0, "Instance is 0x11c0 bytes");

uint8_t InstanceGetOpResult(Instance* instance, uint64_t id, int32_t* detail);

// What a path stat returns (0x48 bytes): whether the manifest has the file, and its key.
struct FILESTAT {
    uint32_t m_state;               // 0x00: 1 found
    uint32_t m_04;                  // 0x04
    FILEKEY m_key;                  // 0x08
};
static_assert(sizeof(FILESTAT) == 0x48, "FILESTAT is 0x48 bytes");

uint8_t InstanceStatByName(Instance* instance, const char* name, FILESTAT* stat, uint64_t flags);
uint8_t InstanceIsResident(Instance* instance, const char* name);
uint64_t InstanceStartReadByName(Instance* instance, const char* name, void* dest, uint32_t size,
    OPCALLBACK callback, void* context, uint8_t priority, uint32_t flags);
void InstanceCancelOp(Instance* instance, uint64_t id);
uint8_t AioToTactError(uint8_t result);
bool NormalizeStoragePath(char* out, uint64_t size, const char* path);
int64_t WidePathLength(const char* path);

}

#endif
