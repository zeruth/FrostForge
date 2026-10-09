#include "texture/Texture.hpp"

#include <windows.h>
#include <cstring>
#include <new>

#include "blz/Murmur.hpp"
#include "blz/Sync.hpp"
#include "cas/CAS.hpp"
#include "client/RenderSettings.hpp"
#include "fileservices/AsyncFile.hpp"
#include "gx/Gx.hpp"
#include "gx/GxFormat.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/GxShader.hpp"
#include "images/Images.hpp"
#include "os/Os.hpp"
#include "os/Sync.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/thread/SCritSect.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"

namespace {

const char s_textureFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Services\\Texture.cpp";
const char s_fileNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBVTextureFileHash@@PEAVCTexture@@@blz@@@blz@@";
const char s_fileBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBVTextureFileHash@@PEAVCTexture@@@blz@@@blz@@";
const char s_solidNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBUSolidTextureHashKey@@PEAVCTexture@@@blz@@@blz@@";
const char s_solidBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBUSolidTextureHashKey@@PEAVCTexture@@@blz@@@blz@@";
const char s_dequeMapTag[] = ".PEAPEAVCTexture@@";
const char s_dequeBlockTag[] = ".PEAVCTexture@@";
const char s_reloadTag[] = ".PEAVCTexture@@";
const char s_callbackTag[] = ".?AU?$pair@_KV?$function@$$A6AXH@Z@blz@@@blz@@";
const char s_allLoadedTag[] = ".?AU?$pair@_KV?$function@$$A6AXXZ@blz@@@blz@@";
const uint32_t s_errorColor[2] = { 0xff00ff00, 0 };  // DAT_142725ee0

// A blz::chained_hash: bucket count, buckets, count, maximum load.
struct CHAINEDHASH {
    uint64_t m_bucketCount;
    void** m_buckets;
    uint64_t m_count;
    float m_maxLoad;
};

// TextureFileHash's node (0x20 bytes).
struct FILENODE {
    FILENODE* m_next;
    TEXTUREKEY m_key;
    CTexture* m_texture;
};
static_assert(sizeof(FILENODE) == 0x20, "FILENODE is 0x20 bytes");

// SolidTextureHashKey's node (0x20 bytes).
struct SOLIDKEY {
    uint32_t m_color;
    uint32_t m_color2;
    uint32_t m_kind;
};
struct SOLIDNODE {
    SOLIDNODE* m_next;
    SOLIDKEY m_key;
    CTexture* m_texture;
};
static_assert(sizeof(SOLIDNODE) == 0x20, "SOLIDNODE is 0x20 bytes");

// The textures by file (DAT_142cfbdc0), the unique ones (DAT_142cfbde8) and the solid ones
// (DAT_142cfbe08).
CHAINEDHASH s_byFile = { 8, nullptr, 0, 1.0f };
CHAINEDHASH s_unique = { 8, nullptr, 0, 1.0f };
CHAINEDHASH s_solid = { 8, nullptr, 0, 1.0f };
int32_t s_uniqueNext = 1;           // DAT_142cfbde0

struct LOCKS {
    SCritSect m_byFile;             // DAT_1430a5490
    SCritSect m_atlas;              // DAT_1430a54f8
    LOCKS() {
        this->m_byFile.Initialize();
        this->m_atlas.Initialize();
    }
};
LOCKS s_locks;
OSMUTEX s_uniqueLock = { SRWLOCK_INIT };   // DAT_1430a54d0

// The unique textures that have a writer, as a blz::deque<CTexture*> (DAT_1430a54d8).
struct TEXTUREDEQUE {
    CTexture*** m_map;
    uint64_t m_mapSize;
    uint64_t m_offset;
    uint64_t m_size;
};
TEXTUREDEQUE s_fillQueue;

// Textures whose load was dropped while it ran (DAT_1430a54b8).
struct TEXTUREVECTOR {
    CTexture** m_data;
    uint64_t m_size;
    uint64_t m_capacity;
};
TEXTUREVECTOR s_dropped;

// The atlas pages (PTR_LOOP_142cfbe28): a list of pages by their link at +8.
struct ATLASLINK {
    ATLASLINK* m_next;
    ATLASLINK* m_prev;
};
ATLASLINK s_atlasPages = { &s_atlasPages, &s_atlasPages };

CTextureAtlas* AtlasOf(ATLASLINK* link) {
    return reinterpret_cast<CTextureAtlas*>(reinterpret_cast<uint8_t*>(link) - 8);
}

ATLASLINK* LinkOf(CTextureAtlas* atlas) {
    return reinterpret_cast<ATLASLINK*>(&atlas->m_next);
}

// The solid textures every draw falls back to.
CTexture* s_defaultTexture;         // DAT_1430a55a0
CTexture* s_blackTexture;           // DAT_1430a55a8
CTexture* s_greyTexture;            // DAT_1430a55b0
CTexture* s_transparentGreyTexture; // DAT_1430a55b8
CTexture* s_whiteTexture;           // DAT_1430a55c0
CTexture* s_transparentBlack3D;     // DAT_1430a55c8

uint32_t s_maxWidth = 0xffffffff;   // DAT_142cfbdb0
uint32_t s_maxHeight = 0xffffffff;  // DAT_142cfbdb4
uint32_t s_maxMips = 0xc;           // DAT_142cfbdb8
uint32_t s_pendingMaxMips = 0xc;    // DAT_142cfbdbc
uint32_t s_reduction;               // DAT_1430a5594
uint32_t s_pendingReduction;        // DAT_1430a5598
uint8_t s_settingsDirty;            // DAT_1430a5591
uint8_t s_atlasEnabled;             // DAT_1430a5590
uint8_t s_filter = 3;               // DAT_142cfbd1d
uint32_t s_anisotropy = 1;          // DAT_142cfbdac

// The storage format a mip format's data is uploaded as (DAT_142cfbd10).
const uint8_t s_dataFormats[0x10] = { 7, 8, 2, 4, 3, 5, 0, 9, 0, 0, 10, 11, 0x14, 3, 0, 0 };

// The callbacks: per finished texture (DAT_1430a5520, lock DAT_1430a5538) and once after any
// (DAT_1430a5558, lock DAT_1430a5570).
struct CALLBACKENTRY {
    uint64_t m_key;
    blz::function_storage m_function;
};
static_assert(sizeof(CALLBACKENTRY) == 0x28, "CALLBACKENTRY is 0x28 bytes");
struct CALLBACKLIST {
    CALLBACKENTRY* m_data;
    uint64_t m_size;
    uint64_t m_capacity;            // bit 63: not owned
};
CALLBACKLIST s_loadedCallbacks;
blz::shared_mutex s_loadedLock;
CALLBACKLIST s_allLoadedCallbacks;
blz::shared_mutex s_allLoadedLock;

const uint64_t NOT_OWNED = 0x8000000000000000ull;

// --- small helpers -------------------------------------------------------------------------

int32_t AddRefCount(volatile int32_t* count) {
    return InterlockedIncrement(reinterpret_cast<volatile long*>(count));
}

// ref: FUN_1402c8fc0
int32_t ReleaseCount(volatile int32_t* count) {
    return InterlockedDecrement(reinterpret_cast<volatile long*>(count));
}

void DestroyGxTex(void** tex) {
    void* old = *tex;
    *tex = nullptr;
    if (old != nullptr) {
        GxTexDestroy(&old);
    }
}

// --- the hash tables -----------------------------------------------------------------------

uint64_t ResizeTarget(const CHAINEDHASH* hash, uint64_t requested) {
    float load = static_cast<float>(hash->m_count) / hash->m_maxLoad;
    uint64_t base = static_cast<uint64_t>(load);
    uint64_t wanted = base + 1;
    if (static_cast<float>(hash->m_count) <= static_cast<float>(base) * hash->m_maxLoad) {
        wanted = base;
    }
    if (wanted < requested) {
        wanted = requested;
    }
    uint64_t size = 8;
    if (wanted > 8) {
        do {
            size *= 2;
        } while (size < wanted);
    }
    return size;
}

// ref: FUN_141282780
void FileRehash(CHAINEDHASH* hash, uint64_t requested) {
    uint64_t size = ResizeTarget(hash, requested);
    if (size == hash->m_bucketCount && hash->m_buckets != nullptr) {
        return;
    }
    auto buckets = static_cast<void**>(SMemAlloc(size * 8, 0xb, s_fileBucketTag, SStrHashFnv1a(s_fileBucketTag), -6, 0));
    std::memset(buckets, 0, size * 8);
    if (hash->m_buckets != nullptr) {
        for (uint64_t i = 0; i < hash->m_bucketCount; i++) {
            auto node = static_cast<FILENODE*>(hash->m_buckets[i]);
            while (node != nullptr) {
                FILENODE* next = node->m_next;
                uint64_t slot = static_cast<uint32_t>(node->m_key.m_fileDataId) & (size - 1);
                node->m_next = static_cast<FILENODE*>(buckets[slot]);
                buckets[slot] = node;
                node = next;
            }
        }
    }
    SMemFree(hash->m_buckets, s_fileBucketTag, -6, 0);
    hash->m_buckets = buckets;
    hash->m_bucketCount = size;
}

bool FileKeyMatches(const TEXTUREKEY& key, const FILENODE* node) {
    return key.m_fileDataId == node->m_key.m_fileDataId
        && (key.m_matchAny != 0 || node->m_key.m_matchAny != 0 || key.m_flags == node->m_key.m_flags);
}

// Inserts {key, texture} unless the key is there; the node either way.
// ref: FUN_141282170
FILENODE* FileInsert(CHAINEDHASH* hash, const TEXTUREKEY& key, CTexture* texture, bool* inserted) {
    if (hash->m_buckets == nullptr) {
        FileRehash(hash, hash->m_bucketCount);
    }
    uint64_t slot = static_cast<uint32_t>(key.m_fileDataId) & (hash->m_bucketCount - 1);
    auto link = reinterpret_cast<FILENODE**>(&hash->m_buckets[slot]);
    while (*link != nullptr) {
        if (FileKeyMatches(key, *link)) {
            *inserted = false;
            return *link;
        }
        link = &(*link)->m_next;
    }
    auto node = static_cast<FILENODE*>(SMemAlloc(sizeof(FILENODE), 0xb, s_fileNodeTag, SStrHashFnv1a(s_fileNodeTag), -6, 0));
    node->m_next = nullptr;
    node->m_key = key;
    node->m_texture = texture;
    *link = node;
    hash->m_count++;
    if (hash->m_maxLoad < static_cast<float>(hash->m_count) / static_cast<float>(hash->m_bucketCount)) {
        FileRehash(hash, hash->m_bucketCount * 2);
    }
    *inserted = true;
    return node;
}

// ref: FUN_141281ed0
bool FileRemove(CHAINEDHASH* hash, const TEXTUREKEY& key) {
    if (hash->m_buckets == nullptr) {
        return false;
    }
    uint64_t slot = static_cast<uint32_t>(key.m_fileDataId) & (hash->m_bucketCount - 1);
    auto link = reinterpret_cast<FILENODE**>(&hash->m_buckets[slot]);
    while (*link != nullptr) {
        if (FileKeyMatches(key, *link)) {
            FILENODE* node = *link;
            *link = node->m_next;
            SMemFree(node, s_fileNodeTag, -6, 0);
            hash->m_count--;
            return true;
        }
        link = &(*link)->m_next;
    }
    return false;
}

uint64_t SolidHash(const SOLIDKEY& key) {
    uint64_t hash[2];
    blz::murmur3_128(hash, &key, 0xc, 0);
    return hash[0];
}

// ref: FUN_141282580
void SolidRehash(CHAINEDHASH* hash, uint64_t requested) {
    uint64_t size = ResizeTarget(hash, requested);
    if (size == hash->m_bucketCount && hash->m_buckets != nullptr) {
        return;
    }
    auto buckets = static_cast<void**>(SMemAlloc(size * 8, 0xb, s_solidBucketTag, SStrHashFnv1a(s_solidBucketTag), -6, 0));
    std::memset(buckets, 0, size * 8);
    if (hash->m_buckets != nullptr) {
        for (uint64_t i = 0; i < hash->m_bucketCount; i++) {
            auto node = static_cast<SOLIDNODE*>(hash->m_buckets[i]);
            while (node != nullptr) {
                SOLIDNODE* next = node->m_next;
                uint64_t slot = SolidHash(node->m_key) & (size - 1);
                node->m_next = static_cast<SOLIDNODE*>(buckets[slot]);
                buckets[slot] = node;
                node = next;
            }
        }
    }
    SMemFree(hash->m_buckets, s_solidBucketTag, -6, 0);
    hash->m_buckets = buckets;
    hash->m_bucketCount = size;
}

// ref: FUN_141281f80
SOLIDNODE* SolidInsert(CHAINEDHASH* hash, const SOLIDKEY& key, bool* inserted) {
    uint64_t h = SolidHash(key);
    if (hash->m_buckets == nullptr) {
        SolidRehash(hash, hash->m_bucketCount);
    }
    auto link = reinterpret_cast<SOLIDNODE**>(&hash->m_buckets[h & (hash->m_bucketCount - 1)]);
    while (*link != nullptr) {
        SOLIDNODE* node = *link;
        if (key.m_color == node->m_key.m_color && key.m_color2 == node->m_key.m_color2 && key.m_kind == node->m_key.m_kind) {
            *inserted = false;
            return node;
        }
        link = &node->m_next;
    }
    auto node = static_cast<SOLIDNODE*>(SMemAlloc(sizeof(SOLIDNODE), 0xb, s_solidNodeTag, SStrHashFnv1a(s_solidNodeTag), -6, 0));
    node->m_next = nullptr;
    node->m_key = key;
    node->m_texture = nullptr;
    *link = node;
    hash->m_count++;
    if (hash->m_maxLoad < static_cast<float>(hash->m_count) / static_cast<float>(hash->m_bucketCount)) {
        SolidRehash(hash, hash->m_bucketCount * 2);
    }
    *inserted = true;
    return node;
}

template <class Node, class Visit>
void ForEachNode(CHAINEDHASH* hash, Visit visit) {
    if (hash->m_buckets == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < hash->m_bucketCount; i++) {
        for (auto node = static_cast<Node*>(hash->m_buckets[i]); node != nullptr;) {
            Node* next = node->m_next;
            visit(node);
            node = next;
        }
    }
}

// A texture by file, with a reference taken (the caller holds the lock).
// ref: FUN_14127d760
CTexture* FindByFile(int32_t fileDataId, uint64_t flags) {
    CTexture* found = nullptr;
    s_locks.m_byFile.Enter();
    if (s_byFile.m_buckets != nullptr) {
        uint64_t slot = static_cast<uint32_t>(fileDataId) & (s_byFile.m_bucketCount - 1);
        for (auto node = static_cast<FILENODE*>(s_byFile.m_buckets[slot]); node != nullptr; node = node->m_next) {
            if (node->m_key.m_fileDataId == fileDataId && (node->m_key.m_matchAny != 0 || node->m_key.m_flags == flags)) {
                found = node->m_texture;
                AddRefCount(&found->m_refCount);
                break;
            }
        }
    }
    s_locks.m_byFile.Leave();
    return found;
}

// The fill queue (blz::deque<CTexture*>::push_back).
// ref: FUN_141282380
CTexture** FillQueuePush(TEXTUREDEQUE* queue) {
    uint64_t offset = queue->m_offset;
    if (((queue->m_size + offset) & 7) == 0 && queue->m_mapSize <= (queue->m_size + 8) >> 3) {
        uint64_t size = queue->m_mapSize;
        if (size == 0) {
            size = 8;
        } else {
            uint64_t want = size + 1;
            while (size < want) {
                size *= 2;
            }
        }
        auto map = static_cast<CTexture***>(SMemAlloc(size * 8, 0xb, s_dequeMapTag, SStrHashFnv1a(s_dequeMapTag), -6, 0));
        uint64_t first = (queue->m_offset >> 3) & (queue->m_mapSize - 1);
        if (queue->m_mapSize != 0) {
            std::memcpy(map, queue->m_map + first, (queue->m_mapSize - first) * 8);
            std::memcpy(map + (queue->m_mapSize - first), queue->m_map, first * 8);
        }
        std::memset(map + queue->m_mapSize, 0, (size - queue->m_mapSize) * 8);
        SMemFree(queue->m_map, s_dequeMapTag, -6, 0);
        queue->m_offset &= 7;
        offset = queue->m_offset;
        queue->m_map = map;
        queue->m_mapSize = size;
    }
    uint64_t index = queue->m_size + offset;
    queue->m_size++;
    uint64_t block = (index >> 3) & (queue->m_mapSize - 1);
    if (queue->m_map[block] == nullptr) {
        queue->m_map[block] = static_cast<CTexture**>(SMemAlloc(0x40, 0xb, s_dequeBlockTag, SStrHashFnv1a(s_dequeBlockTag), -6, 0));
    }
    return queue->m_map[block] + (index & 7);
}

CTexture*& FillQueueAt(TEXTUREDEQUE* queue, uint64_t index) {
    uint64_t at = index + queue->m_offset;
    return queue->m_map[(at >> 3) & (queue->m_mapSize - 1)][at & 7];
}

void DroppedPush(CTexture* texture) {
    if (s_dropped.m_size == (s_dropped.m_capacity & ~NOT_OWNED)) {
        // FUN_1411ec8b0: grow by half again.
        uint64_t capacity = s_dropped.m_capacity & ~NOT_OWNED;
        uint64_t grown = (capacity >> 1) + capacity;
        if (grown < capacity + 1) {
            grown = capacity + 1;
        }
        auto data = static_cast<CTexture**>(SMemAlloc(grown * 8, 0xb, s_reloadTag, SStrHashFnv1a(s_reloadTag), -6, 0));
        if (s_dropped.m_size != 0) {
            std::memcpy(data, s_dropped.m_data, s_dropped.m_size * 8);
        }
        if ((s_dropped.m_capacity & NOT_OWNED) == 0) {
            SMemFree(s_dropped.m_data, s_reloadTag, -6, 0);
        }
        s_dropped.m_data = data;
        s_dropped.m_capacity = grown;
    }
    s_dropped.m_data[s_dropped.m_size++] = texture;
}

// --- callbacks lists ------------------------------------------------------------------------

void MoveFunction(blz::function_storage* dest, blz::function_storage* source) {
    source->m_manager(dest->m_storage, source->m_storage, 2);
    dest->m_invoke = source->m_invoke;
    dest->m_manager = source->m_manager;
    source->m_invoke = nullptr;
    source->m_manager = &blz::function_empty_manager;
}

void AddCallback(CALLBACKLIST* list, blz::shared_mutex* lock, const char* tag, blz::function_storage* function,
    uint64_t key) {
    lock->lock();
    CALLBACKENTRY entry;
    entry.m_key = key;
    entry.m_function.m_invoke = nullptr;
    entry.m_function.m_manager = &blz::function_empty_manager;
    MoveFunction(&entry.m_function, function);
    if (list->m_size == (list->m_capacity & ~NOT_OWNED)) {
        uint64_t capacity = list->m_capacity & ~NOT_OWNED;
        uint64_t grown = ((list->m_capacity >> 1) & 0x3fffffffffffffffull) + capacity;
        if (grown < capacity + 1) {
            grown = capacity + 1;
        }
        auto data = static_cast<CALLBACKENTRY*>(SMemAlloc(grown * sizeof(CALLBACKENTRY), 0xb, tag, SStrHashFnv1a(tag), -6, 0));
        for (uint64_t i = 0; i < list->m_size; i++) {
            data[i].m_key = list->m_data[i].m_key;
            data[i].m_function.m_manager = &blz::function_empty_manager;
            MoveFunction(&data[i].m_function, &list->m_data[i].m_function);
        }
        if ((list->m_capacity & NOT_OWNED) == 0) {
            SMemFree(list->m_data, tag, -6, 0);
        }
        list->m_data = data;
        list->m_capacity = grown;
    }
    CALLBACKENTRY* slot = &list->m_data[list->m_size];
    slot->m_key = entry.m_key;
    slot->m_function.m_manager = &blz::function_empty_manager;
    MoveFunction(&slot->m_function, &entry.m_function);
    list->m_size++;
    lock->unlock();
}

// --- the texture's reset and its Gx texture --------------------------------------------------

// The Gx texture, the placeholder, the mips and the atlas slot let go.
// ref: FUN_14127c090
void TextureReset(CTexture* texture) {
    void* gx = texture->m_gxTex;
    texture->m_flags = 0;
    texture->m_gxTex = nullptr;
    if (gx != nullptr) {
        GxTexDestroy(&gx);
    }
    DestroyGxTex(&texture->m_placeholder);
    MipBitsFree(&texture->m_mipBits);
    CTextureAtlas* atlas = texture->m_atlas;
    if (atlas != nullptr) {
        s_locks.m_atlas.Enter();
        atlas->m_slots[texture->m_atlasSlot] = nullptr;
        int32_t slot = texture->m_atlasSlot;
        atlas->m_used--;
        atlas->m_freeSlot = slot;
        if (ReleaseCount(&atlas->m_refCount) < 1) {
            GxTexDestroy(&atlas->m_gxTex);
            ATLASLINK* link = LinkOf(atlas);
            if (link->m_next != nullptr) {
                link->m_prev->m_next = link->m_next;
                link->m_next->m_prev = link->m_prev;
            }
            link->m_prev = nullptr;
            link->m_next = nullptr;
            operator delete(atlas, sizeof(CTextureAtlas));
        }
        texture->m_atlasSlot = -1;
        s_locks.m_atlas.Leave();
        texture->m_atlas = nullptr;
    }
}

// The Gx texture from the description; the old one, if any, released.
// ref: FUN_14127c670
bool CreateGxTex(GXTEXDESC* desc, void** tex) {
    void* created = nullptr;
    bool ok = GxTexCreate(desc, &created);
    void* old = *tex;
    *tex = created;
    if (old != nullptr) {
        GxTexDestroy(&old);
    }
    return ok;
}

// ref: FUN_14127c6d0
void ReleaseGxTex(void** tex) {
    DestroyGxTex(tex);
}

// A solid-colour texture's level: the same pixels for every level and face.
struct SOLIDFILL {
    uint32_t m_pitch;
    const void* m_data;
};

// ref: 0x1405e13c0
void SolidFill(void* param, uint32_t width, uint32_t height, uint32_t slice, uint32_t mip, uint32_t* pitch,
    const void** data) {
    (void)width;
    (void)height;
    (void)slice;
    (void)mip;
    auto fill = static_cast<SOLIDFILL*>(param);
    *pitch = fill->m_pitch;
    *data = fill->m_data;
}

// A small ARGB8888 texture of the given pixels.
// ref: FUN_14127bf60
void CreateSolidGx(CTexture* texture, uint8_t type, uint32_t width, uint16_t height, bool opaque, uint32_t pitch,
    const void* pixels) {
    TextureReset(texture);
    texture->m_desc.m_width = width & 0xffff;
    texture->m_desc.m_type = type;
    void* created = nullptr;
    texture->m_desc.m_height = height;
    texture->m_desc.m_depth = 1;
    texture->m_desc.m_format = 2;
    texture->m_desc.m_dataFormat = 2;
    bool ok = GxTexCreate(&texture->m_desc, &created);
    void* old = texture->m_gxTex;
    texture->m_gxTex = created;
    if (old != nullptr) {
        GxTexDestroy(&old);
    }
    if (!ok || texture->m_gxTex == nullptr) {
        DestroyGxTex(&texture->m_gxTex);
        return;
    }
    texture->m_flags = opaque ? texture->m_flags | 1 : texture->m_flags & ~1u;
    SOLIDFILL fill = { pitch, pixels };
    texture->m_compression = 3;
    auto gx = static_cast<uint16_t*>(texture->m_gxTex);
    int32_t rect[4] = { 0, 0, gx[1], gx[0] };
    GxTexUpdate(g_gxUploadInterface, texture->m_gxTex, rect, true, &SolidFill, &fill);
}

// ref: FUN_14127bc40
void InitSolid(CTexture* texture, const uint32_t* color, const uint32_t* color2, uint8_t kind) {
    uint8_t type;
    bool opaque;
    if (kind == 3) {
        opaque = (*color >> 24) > 0xfd;
        type = 1;
    } else if (kind == 2) {
        texture->m_desc.m_usage &= ~7u;
        uint32_t pixels[4] = { *color, *color2, *color2, *color };
        opaque = (*color >> 24) >= 0xfe && (*color2 >> 24) >= 0xfe;
        CreateSolidGx(texture, 0, 2, 2, opaque, 8, pixels);
        return;
    } else if (kind == 1) {
        opaque = (*color >> 24) > 0xfd;
        type = 3;
    } else {
        opaque = (*color >> 24) > 0xfd;
        type = 0;
    }
    CreateSolidGx(texture, type, 1, 1, opaque, 0, color);
}

// White levels for a texture whose data cannot be had again.
// ref: FUN_14127c4a0
void FillWhite(uint32_t width, uint32_t height, uint32_t format, bool cube, MIPBITS** bits) {
    MIPBITS* chain = *bits;
    if (chain == nullptr) {
        chain = MipBitsCreate(2, width, height, cube);
        *bits = chain;
    }
    MipBitsInit(2, width, height, cube, chain);
    (void)format;
    for (uint64_t level = 0; width >= 2 || height >= 2; level++) {
        std::memset((*bits)[level], 0xff, static_cast<uint64_t>(width) * height * 4);
        width = width >> 1 != 0 ? width >> 1 : 1;
        height = height >> 1 != 0 ? height >> 1 : 1;
    }
}

// The block size of a Gx format (4 for the block formats).
// ref: FUN_14127c550
uint32_t BlockDim(uint8_t format) {
    const uint32_t dims[0x23] = { 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 4, 1, 1, 1 };
    return dims[format];
}

// The Gx format and mip format a BLP format maps to.
// ref: FUN_1412809b0
void BlpFormats(uint32_t* mipFormat, uint8_t* gxFormat, uint32_t blpFormat) {
    switch (blpFormat) {
    case 0: *gxFormat = 7; *mipFormat = 0; return;
    case 1: *gxFormat = 8; *mipFormat = 1; return;
    case 2: *gxFormat = 2; *mipFormat = 2; return;
    case 3: *gxFormat = 4; *mipFormat = 2; return;
    case 4: *gxFormat = 3; *mipFormat = 2; return;
    case 5: *gxFormat = 5; *mipFormat = 2; return;
    case 7: *gxFormat = 9; *mipFormat = 7; return;
    case 8: *gxFormat = 0x14; *mipFormat = 0xc; return;
    case 10: *gxFormat = 10; *mipFormat = 10; return;
    case 11: *gxFormat = 0xb; *mipFormat = 0xb; return;
    default: return;
    }
}

// The BLP's header, checked against the sizes its levels must have; a level larger than its
// size leaves only the first.
// ref: FUN_14127d5a0
bool ParseBlp(BLPIMAGE* image, const uint8_t* data, uint32_t size) {
    if (!BlpImageParse(image, data, size, true)) {
        return false;
    }
    uint32_t mipFormat = 0;
    uint8_t gxFormat = 0;
    BlpFormats(&mipFormat, &gxFormat, image->m_format);
    uint32_t smaller = image->m_width < image->m_height ? image->m_width : image->m_height;
    if (smaller < BlockDim(gxFormat)) {
        return false;
    }
    for (uint32_t level = 0; level < image->m_mipCount; level++) {
        uint32_t want = ImageLevelSize(level, image->m_width, image->m_height, mipFormat, image->m_type == 2);
        if (want < image->m_mipSizes[level]) {
            image->m_mipCount = 1;
            return true;
        }
    }
    return true;
}

// The file kind by its contents: 0 PNG, 1 TGA, 2 BLP, 3 JPEG, 4 unknown.
// ref: FUN_14127beb0
uint32_t DetectKind(const uint8_t* data, uint64_t size) {
    if (size > 0x493 && *reinterpret_cast<const uint32_t*>(data) == 0x32504c42) {   // FUN_1424307b0
        return 2;
    }
    if (size >= 0x1a && _stricmp(reinterpret_cast<const char*>(data + size - 0x12), "TRUEVISION-XFILE.") == 0) {  // FUN_1406a6110
        return 1;
    }
    if (size > 9 && (*reinterpret_cast<const uint32_t*>(data + 6) == 0x4649464a
        || *reinterpret_cast<const uint32_t*>(data + 6) == 0x66697845)) {    // FUN_1406a36f0
        return 3;
    }
    if (size > 3 && *reinterpret_cast<const uint32_t*>(data) == 0x474e5089) {   // FUN_1406a5140
        return 0;
    }
    // FUN_1406a6080: a TGA without the footer.
    bool tga = false;
    if (size > 0x11) {
        uint8_t type = data[2];
        bool ok = false;
        switch (type) {
        case 1:
        case 9:
            ok = data[1] == 1;
            break;
        case 2:
        case 10:
            if (data[1] == 0 && ((data[0x10] - 0x10) & 0xe7) == 0 && data[0x10] != '(') {
                tga = data[0x11] < 0x40;
            }
            break;
        case 3:
        case 0xb:
            ok = data[1] == 0;
            break;
        default:
            break;
        }
        if (ok && data[0x10] == 8) {
            tga = data[0x11] < 0x40;
        }
    }
    return tga ? 1 : 4;
}

// The decoders of the other formats, through their image readers.
// ref: FUN_14127a2e0
bool DecodePng(const uint8_t* data, uint32_t size, MIPBITS** bits, BLPIMAGE* image) {
    (void)data;
    (void)size;
    (void)bits;
    (void)image;
    FROST_UNIMPLEMENTED();  // the PNG reader (0x1406a4b30.., libpng) is not ported
    return false;
}

// ref: FUN_14127c700
bool DecodeTga(const uint8_t* data, uint32_t size, MIPBITS** bits, BLPIMAGE* image) {
    (void)data;
    (void)size;
    (void)bits;
    (void)image;
    FROST_UNIMPLEMENTED();  // the TGA reader (0x1406a6c60..) is not ported
    return false;
}

// ref: FUN_14127a0a0
bool DecodeJpeg(const uint8_t* data, uint32_t size, MIPBITS** bits, BLPIMAGE* image) {
    (void)data;
    (void)size;
    (void)bits;
    (void)image;
    FROST_UNIMPLEMENTED();  // the JPEG reader (0x1406a3500.., libjpeg) is not ported
    return false;
}

// ref: FUN_14127e530
uint32_t PowerOfTwoLevels(uint32_t width, uint32_t height) {
    if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) {
        return 1;
    }
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        levels++;
        width = width >> 1 != 0 ? width >> 1 : 1;
        height = height >> 1 != 0 ? height >> 1 : 1;
    }
    return levels;
}

CTextureAtlas* AtlasAllocate(CTexture* texture);

// The decoded levels set up for the Gx texture: the size limits applied (whole levels dropped
// from the top), the formats chosen, the data decoded or pointed at, the description filled, and
// the Gx texture (or an atlas slot) made.
// ref: FUN_141281a50
bool CreateFromBlp(CTexture* texture, BLPIMAGE* image) {
    uint32_t flags = texture->m_flags;
    if ((flags & 4) != 0 && (image->m_flags & 1) == 0) {
        flags &= ~4u;
        texture->m_flags = flags;
    }
    texture->m_compression = static_cast<int32_t>(image->m_compression);
    texture->m_alphaDepth = static_cast<uint8_t>(image->m_alphaDepth);
    if (image->m_alphaDepth == 0) {
        texture->m_flags = flags | 1;
    }
    uint32_t firstMip = 0;
    uint32_t width = image->m_width;
    uint32_t height = image->m_height;
    uint32_t bias = texture->m_quality;
    texture->m_mipCount = static_cast<uint8_t>(image->m_mipCount);
    uint8_t mipBias;
    if (image->m_mipCount < 2) {
        mipBias = 1;
    } else {
        uint32_t maxWidth = s_maxWidth;
        uint32_t maxHeight = s_maxHeight;
        if (bias != 0) {
            if (s_reduction == 0 || ((~(image->m_flags >> 1)) & 1) == 0) {
                if (s_maxMips < bias) {
                    bias = s_maxMips;
                }
                uint32_t limit = 1u << (bias & 0x1f);
                if (limit < s_maxWidth) {
                    maxWidth = limit;
                }
                if (limit < s_maxHeight) {
                    maxHeight = limit;
                }
            } else if (width > 0x3f && height > 0x3f) {
                uint32_t w = width >> (s_reduction & 0x1f);
                uint32_t h = height >> (s_reduction & 0x1f);
                if (w < s_maxWidth) {
                    maxWidth = w;
                }
                if (h < s_maxHeight) {
                    maxHeight = h;
                }
            }
        }
        while (maxWidth < width || maxHeight < height) {
            firstMip++;
            width = width > 1 ? width >> 1 : 1;
            height = height > 1 ? height >> 1 : 1;
        }
        mipBias = static_cast<uint8_t>(bias);
    }
    texture->m_mipBias = mipBias;
    if (width == 0 || height == 0) {
        texture->m_flags |= 0x800;
        return false;
    }
    bool cube = image->m_type == 2;
    uint32_t faceWidth = cube ? width / 6 : width;
    uint8_t type;
    if ((faceWidth & (faceWidth - 1)) == 0 && (height & (height - 1)) == 0) {
        type = cube ? 1 : 0;
    } else {
        if ((texture->m_flags & 0x100) == 0) {
            texture->m_flags |= 0x800;
            return false;
        }
        type = 2;
    }
    uint32_t mipFormat = type;
    uint8_t gxFormat = type;
    BlpFormats(&mipFormat, &gxFormat, image->m_format);
    bool direct;
    if (image->m_compression == 2) {
        direct = mipFormat == 0 || mipFormat == 1 || mipFormat == 7 || mipFormat == 10 || mipFormat == 11;
    } else {
        direct = image->m_compression == 3 || image->m_compression == 4;
    }
    if (!direct) {
        MIPBITS* bits = MipBitsCreate(mipFormat, width, height, cube);
        if (!BlpImageDecode(image, mipFormat, bits, firstMip)) {
            MipBitsFree(&bits);
            texture->m_flags |= 0x800;
            return false;
        }
        MIPBITS* old = texture->m_mipBits;
        texture->m_mipBits = bits;
        MipBitsFree(&old);
    } else {
        if (texture->m_mipBits == nullptr) {
            texture->m_mipBits = MipBitsCreatePointers(width, height, cube);
        }
        for (uint32_t level = firstMip; level < image->m_mipCount; level++) {
            texture->m_mipBits[level - firstMip] = image->m_mips[level];
        }
    }
    uint32_t levels = image->m_mipCount;
    uint32_t usage = texture->m_desc.m_usage;
    texture->m_desc.m_depth = 1;
    texture->m_desc.m_format = gxFormat;
    uint32_t mipLimit = usage >> 0x1c;
    if (levels < mipLimit) {
        mipLimit = levels;
    }
    uint32_t updated = (mipLimit << 0x1c) | (usage & 0xfffffff);
    texture->m_desc.m_usage = updated;
    texture->m_desc.m_dataFormat = s_dataFormats[mipFormat];
    texture->m_desc.m_type = type;
    texture->m_desc.m_width = faceWidth & 0xffff;
    texture->m_desc.m_height = height & 0xffff;
    uint8_t filter = static_cast<uint8_t>(usage & 0xfffffff) & 7;
    if (filter > 1 && image->m_mipCount == 1) {
        if (filter == 2) {
            updated = (mipLimit << 0x1c) | (usage & 0xffffff8);
        } else {
            updated = (mipLimit << 0x1c) | (usage & 0xffffff9) | 1;
        }
        texture->m_desc.m_usage = updated;
    }
    if ((texture->m_flags & 4) != 0) {
        texture->m_atlas = AtlasAllocate(texture);
        if (texture->m_atlas == nullptr) {
            texture->m_flags &= ~4u;
        }
    }
    if (texture->m_atlas != nullptr) {
        return true;
    }
    if (CreateGxTex(&texture->m_desc, &texture->m_gxTex) && texture->m_gxTex != nullptr) {
        long previous = InterlockedExchange(&texture->m_state, 2);
        if (previous != 3) {
            return true;
        }
    }
    DestroyGxTex(&texture->m_gxTex);
    texture->m_flags |= 0x2000;
    InterlockedExchange(&texture->m_state, 3);
    return false;
}

// Decodes the file by its kind; failures are reported into the texture's status.
// ref: FUN_14127c9a0
bool DecodeFile(CTexture* texture, const uint8_t* data, uint32_t size) {
    BLPIMAGE image = {};
    uint32_t kind = (texture->m_flags >> 3) & 7;
    bool ok;
    switch (kind) {
    case 2: {
        if (!ParseBlp(&image, data, size)) {
            char name[0x110];
            if (texture->m_name.m_size == 0) {
                GxShaderName(texture->m_fileDataId, name, 0x104);
                uint64_t length = name[0] != '\0' ? std::strlen(name) : 0;
                blz::string_assign(reinterpret_cast<blz::string_header*>(&texture->m_name), name, length);
            } else {
                const char* source = texture->m_name.m_data;
                char* dest = name;
                for (int64_t left = 0x103;;) {
                    char c = *source++;
                    *dest++ = c;
                    if (c == '\0') {
                        break;
                    }
                    if (--left == 0) {
                        *dest = '\0';
                        break;
                    }
                }
            }
            auto header = reinterpret_cast<const uint32_t*>(data);
            texture->m_status.Add(3,
                "BLP Texture failure: \"%s\" invalid file (size = %d, magic = 0x%08X, version = %d, width = %d, height = %d)\n",
                name, size, header[0], header[1], header[3], header[4]);
            for (uint32_t level = 0; level < 0x10; level++) {
                texture->m_status.Add(3, "BLP Texture failure: (Mip Level %d - Offset %d, Size %d)\n", level,
                    header[5 + level], header[0x15 + level]);
            }
            texture->m_flags |= 0x800;
            return false;
        }
        return CreateFromBlp(texture, &image);
    }
    case 1:
        if (!DecodeTga(data, size, &texture->m_mipBits, &image)) {
            texture->m_flags |= 0x800;
            return false;
        }
        return CreateFromBlp(texture, &image);
    case 0:
        ok = DecodePng(data, size, &texture->m_mipBits, &image);
        break;
    case 3:
        ok = DecodeJpeg(data, size, &texture->m_mipBits, &image);
        break;
    default:
        texture->m_flags |= 0x800;
        return false;
    }
    if (!ok) {
        texture->m_flags |= 0x800;
        return false;
    }
    return CreateFromBlp(texture, &image);
}

// The reload of a BLP's levels into `bits` for a Gx texture that already exists, at its size.
// ref: FUN_14127cc70
bool ReloadBlp(CTexture* texture, const uint8_t* data, uint32_t size, MIPBITS** bits) {
    BLPIMAGE image = {};
    if (!ParseBlp(&image, data, size)) {
        return false;
    }
    uint8_t gxFormat = 0;
    uint32_t mipFormat = 0;
    BlpFormats(&mipFormat, &gxFormat, image.m_format);
    uint32_t firstMip = 0;
    uint8_t bias = texture->m_quality;
    texture->m_mipCount = static_cast<uint8_t>(image.m_mipCount);
    uint32_t width = image.m_width;
    uint32_t height = image.m_height;
    if (image.m_mipCount < 2) {
        bias = 1;
    } else {
        uint32_t maxWidth = s_maxWidth;
        uint32_t maxHeight = s_maxHeight;
        if (bias != 0) {
            if (s_reduction == 0 || ((~(image.m_flags >> 1)) & 1) == 0) {
                uint32_t clamped = bias;
                if (s_maxMips < clamped) {
                    clamped = s_maxMips;
                }
                bias = static_cast<uint8_t>(clamped);
                uint32_t limit = 1u << (clamped & 0x1f);
                if (limit < s_maxWidth) {
                    maxWidth = limit;
                }
                if (limit < s_maxHeight) {
                    maxHeight = limit;
                }
            } else if (width > 0x3f && height > 0x3f) {
                uint32_t w = width >> (s_reduction & 0x1f);
                uint32_t h = height >> (s_reduction & 0x1f);
                if (w < s_maxWidth) {
                    maxWidth = w;
                }
                if (h < s_maxHeight) {
                    maxHeight = h;
                }
            }
        }
        while (maxWidth < width || maxHeight < height) {
            firstMip++;
            width = width > 1 ? width >> 1 : 1;
            height = height > 1 ? height >> 1 : 1;
        }
    }
    texture->m_mipBias = bias;
    auto gx = static_cast<uint16_t*>(texture->m_gxTex);
    if (gx != nullptr) {
        uint32_t gxWidth = gx[0];
        if (reinterpret_cast<uint8_t*>(gx)[7] == 1) {
            gxWidth = gxWidth * 6;
        }
        if (width != gxWidth || height != gx[1] || image.m_mipCount < reinterpret_cast<uint8_t*>(gx)[6] + firstMip
            || gxFormat != reinterpret_cast<uint8_t*>(gx)[9]) {
            return false;
        }
    }
    bool cube = texture->m_desc.m_type == 1;
    if (*bits == nullptr) {
        *bits = MipBitsCreate(2, width, height, cube);
    }
    MipBitsInit(mipFormat, width, height, cube, *bits);
    return BlpImageDecode(&image, mipFormat, *bits, firstMip);
}

// A texture's file read again at once, decoded into `bits` at the existing Gx texture's size.
// ref: FUN_14127d250
bool ReloadFromFile(CTexture* texture, MIPBITS** bits) {
    bool allowLoose = ((texture->m_flags >> 1) & 1) != 0;
    uint8_t* data = nullptr;
    uint32_t size = 0;
    bool read;
    if (texture->m_fileDataId == 0) {
        read = cas::CASFileLoadByName(texture->m_name.m_data, &data, &size, allowLoose, 0x17);
    } else {
        read = cas::CASFileLoadById(texture->m_fileDataId, &data, &size, allowLoose, 0x17);
    }
    if (!read) {
        return false;
    }
    bool ok = false;
    uint32_t kind = (texture->m_flags >> 3) & 7;
    BLPIMAGE image = {};
    if (kind == 2) {
        ok = ReloadBlp(texture, data, size, bits);
    } else {
        bool decoded = false;
        if (kind == 1) {
            decoded = DecodeTga(data, size, bits, &image);
        } else if (kind == 0) {
            decoded = DecodePng(data, size, bits, &image);
        } else if (kind == 3) {
            decoded = DecodeJpeg(data, size, bits, &image);
        }
        if (decoded) {
            uint32_t mipFormat = 0;
            uint8_t gxFormat = 0;
            BlpFormats(&mipFormat, &gxFormat, image.m_format);
            ok = true;
            auto gx = static_cast<uint16_t*>(texture->m_gxTex);
            if (gx != nullptr) {
                uint32_t gxWidth = gx[0];
                if (reinterpret_cast<uint8_t*>(gx)[7] == 1) {
                    gxWidth = gxWidth * 6;
                }
                if (image.m_width != gxWidth || image.m_height != gx[1]
                    || image.m_mipCount < reinterpret_cast<uint8_t*>(gx)[6]
                    || gxFormat != reinterpret_cast<uint8_t*>(gx)[9]) {
                    ok = false;
                }
            }
        }
    }
    cas::CASFileFree(data);
    return ok;
}

// The upload's level reader: the texture's levels, a cube's faces in the Gx order.
// ref: FUN_14127a520
void UploadLevel(void* param, uint32_t width, uint32_t height, uint32_t face, uint32_t mip, uint32_t* pitch,
    const void** data) {
    (void)height;
    CTexture* texture = *static_cast<CTexture**>(param);
    uint32_t rowPitch = GxFormatRowPitch(texture->m_desc.m_dataFormat, width);
    *pitch = rowPitch;
    uint8_t* level = texture->m_mipBits[mip];
    *data = level;
    if (texture->m_desc.m_type == 1) {
        static const uint32_t s_faces[6] = { 0, 2, 4, 5, 3, 1 };
        uint32_t mapped = face < 6 ? s_faces[face] : face;
        *data = level + mapped * rowPitch;
        *pitch = rowPitch * 6;
    }
}

// The levels onto the Gx texture; read again from the file when they are gone, white when the
// file cannot be had.
// ref: FUN_1412818d0
void Upload(CTexture* texture) {
    MIPBITS* loaded = nullptr;
    if (texture->m_mipBits == nullptr) {
        if (!ReloadFromFile(texture, &texture->m_mipBits)) {
            FillWhite(texture->m_desc.m_width, texture->m_desc.m_height, 2, texture->m_desc.m_type == 1,
                &texture->m_mipBits);
        }
        loaded = texture->m_mipBits;
    }
    auto gx = static_cast<uint16_t*>(texture->m_gxTex);
    int32_t rect[4] = { 0, 0, gx[1], gx[0] };
    CTexture* param = texture;
    GxTexUpdate(g_gxUploadInterface, texture->m_gxTex, rect, ((texture->m_cache >> 5) & 1) != 0, &UploadLevel, &param);
    if ((texture->m_flags & 0x400) == 0) {
        DestroyGxTex(&texture->m_placeholder);
    }
    MipBitsFree(&loaded);
}

// --- the atlas -----------------------------------------------------------------------------

// A slot for the texture on a page of its size and format (up to 64x64 square textures).
// ref: FUN_14127c1b0
CTextureAtlas* AtlasAllocate(CTexture* texture) {
    if (texture->m_desc.m_width != texture->m_desc.m_height || texture->m_desc.m_depth != 1
        || texture->m_desc.m_width > 0x40 || texture->m_desc.m_height > 0x40) {
        return nullptr;
    }
    uint64_t usage = (static_cast<uint64_t>(texture->m_desc.m_usage2) << 32 | texture->m_desc.m_usage) | 0x4000000;
    s_locks.m_atlas.Enter();
    CTextureAtlas* page = nullptr;
    for (ATLASLINK* link = s_atlasPages.m_next; link != &s_atlasPages; link = link->m_next) {
        CTextureAtlas* candidate = AtlasOf(link);
        uint64_t candidateUsage;
        std::memcpy(&candidateUsage, reinterpret_cast<uint8_t*>(candidate) + 0x3c, 8);
        if (candidate->m_format == texture->m_desc.m_format && candidateUsage == usage
            && candidate->m_dataFormat == texture->m_desc.m_dataFormat
            && candidate->m_slotWidth == static_cast<int32_t>(texture->m_desc.m_width)
            && candidate->m_slotHeight == static_cast<int32_t>(texture->m_desc.m_height)
            && candidate->m_used < candidate->m_capacity
            && (page == nullptr || page->m_used < candidate->m_used)) {
            page = candidate;
        }
    }
    if (page == nullptr) {
        int32_t height = static_cast<int32_t>(texture->m_desc.m_height);
        int32_t width = static_cast<int32_t>(texture->m_desc.m_width);
        page = static_cast<CTextureAtlas*>(SMemAlloc(sizeof(CTextureAtlas), 0x17, s_textureFile,
            SStrHashFnv1a(s_textureFile), 0x151, 0));
        page->m_refCount = 0;
        page->m_prev = nullptr;
        page->m_next = nullptr;
        page->m_slotWidth = 0;
        page->m_slotHeight = 0;
        page->m_capacity = 0;
        page->m_used = 0;
        page->m_freeSlot = -1;
        std::memset(reinterpret_cast<uint8_t*>(page) + 0x3c, 0, 8);
        page->m_gxTex = nullptr;
        std::memset(page->m_slots, 0, sizeof(page->m_slots));
        page->m_slotHeight = height;
        page->m_slotWidth = width;
        page->m_used = 0;
        page->m_freeSlot = -1;
        page->m_capacity = 0x40;
        page->m_format = texture->m_desc.m_format;
        std::memcpy(reinterpret_cast<uint8_t*>(page) + 0x3c, &usage, 8);
        page->m_dataFormat = texture->m_desc.m_dataFormat;
        GXTEXDESC desc = {};
        desc.m_type = 0;
        desc.m_width = static_cast<uint32_t>(width * 8);
        desc.m_height = static_cast<uint32_t>(height * 8);
        desc.m_depth = 1;
        desc.m_format = page->m_format;
        desc.m_dataFormat = page->m_dataFormat;
        desc.m_usage = static_cast<uint32_t>(usage);
        desc.m_usage2 = static_cast<uint32_t>(usage >> 32);
        GxTexCreate(&desc, &page->m_gxTex);  // FUN_14228c330 "Atlas"
        ATLASLINK* link = LinkOf(page);
        link->m_prev = s_atlasPages.m_prev;
        link->m_next = &s_atlasPages;
        s_atlasPages.m_prev = link;
        link->m_prev->m_next = link;
    }
    texture->m_atlasSlot = -1;
    if (page->m_freeSlot == -1) {
        for (int32_t slot = 0; slot < 0x40; slot++) {
            if (page->m_slots[slot] == nullptr) {
                texture->m_atlasSlot = slot;
                break;
            }
        }
    } else {
        texture->m_atlasSlot = page->m_freeSlot;
        page->m_freeSlot = -1;
    }
    page->m_used++;
    AddRefCount(&page->m_refCount);
    page->m_slots[texture->m_atlasSlot] = texture;
    s_locks.m_atlas.Leave();
    return page;
}

struct ATLASUPLOAD {
    CTextureAtlas* m_page;
    int32_t m_slot;
};

// The atlas upload's level reader: the slot's texture's levels.
// ref: FUN_14127a5e0
void AtlasLevel(void* param, uint32_t width, uint32_t height, uint32_t face, uint32_t mip, uint32_t* pitch,
    const void** data) {
    (void)width;
    (void)height;
    (void)face;
    auto upload = static_cast<ATLASUPLOAD*>(param);
    CTexture* texture = upload->m_page->m_slots[upload->m_slot];
    if (texture->m_mipBits != nullptr) {
        *pitch = GxFormatRowPitch(texture->m_desc.m_dataFormat, static_cast<uint32_t>(upload->m_page->m_slotWidth));
        *data = texture->m_mipBits[mip];
    }
}

// ref: FUN_1412819c0
void AtlasUpload(CTextureAtlas* page, CTexture* texture) {
    int32_t slot = texture->m_atlasSlot;
    int32_t left = (slot & 7) * page->m_slotWidth;
    int32_t top = ((slot >> 3) & 7) * page->m_slotHeight;
    if (page->m_gxTex != nullptr) {
        int32_t rect[4] = { top, left, page->m_slotHeight + top, left + page->m_slotWidth };
        ATLASUPLOAD upload = { page, slot };
        GxTexUpdateInFrame(g_gxRenderContext, page->m_gxTex, rect, false, &AtlasLevel, &upload);
    }
}

// --- the async callbacks ---------------------------------------------------------------------

// ref: FUN_14127b3e0
void* AllocBuffer(uint32_t size) {
    return SMemAlloc(size, 0x17, s_textureFile, SStrHashFnv1a(s_textureFile), 0x6cf, 0);
}

// ref: FUN_14127b690
bool OpenById(CAsyncObject* object, cas::CASFILE** file) {
    return cas::CASFileOpenById(object->m_fileDataId, file, 1);
}

// ref: FUN_14127b6c0
bool OpenByName(CAsyncObject* object, cas::CASFILE** file) {
    return cas::CASFileOpenByName(object->m_name, file, 1);
}

// Decodes on the reader thread.
// ref: FUN_14127b6e0
void OnRead(void* userData, void* buffer, uint32_t size) {
    auto texture = static_cast<CTexture*>(userData);
    auto data = static_cast<const uint8_t*>(buffer);
    texture->m_flags |= DetectKind(data, size) << 3;
    if (DecodeFile(texture, data, size)) {
        return;
    }
    if (!texture->m_status.IsEmpty()) {
        // FUN_141283ba0: the SysMessage display, compiled down to nothing but the text.
        blz::string text;
        texture->m_status.Join(&text, 0, "\n");
        text.~string();
        blz::string message;
        texture->m_status.Join(&message, 0, "\n");
        GxLog("%s", message.m_data);
    }
}

// The texture lets go of the reference its load held; with only that left, the load is
// dropped instead.
// ref: FUN_14127bd70
void ReleaseTexture(CTexture* texture) {
    int32_t left = ReleaseCount(&texture->m_refCount);
    if (left == 0) {
        delete texture;
        return;
    }
    if (texture->m_async != nullptr && left == 1) {
        AsyncFileCancel(texture->m_async);
        texture->m_async = nullptr;
        if ((texture->m_cache & 1) != 0) {
            s_locks.m_byFile.Enter();
            FileRemove(&s_byFile, texture->m_key);
            texture->m_key.m_flags = 0x82001;
            texture->m_key.m_fileDataId = 0;
            texture->m_key.m_matchAny = 0;
            DroppedPush(texture);
            s_locks.m_byFile.Leave();
        }
    }
}

// Uploads on the main thread.
// ref: FUN_14127b7a0
void OnDone(void* userData, void* buffer, uint32_t size) {
    (void)size;
    auto texture = static_cast<CTexture*>(userData);
    CTextureAtlas* page = texture->m_atlas;
    if (page == nullptr) {
        if (!GxDevExists()) {
            DestroyGxTex(&texture->m_gxTex);
        } else if (((texture->m_flags >> 0xd) & 1) == 0) {
            if (((texture->m_flags >> 0xb) & 1) == 0 && texture->m_gxTex != nullptr) {
                Upload(texture);
            } else if (texture->m_placeholder == nullptr) {
                CreateSolidGx(texture, 0, 1, 1, true, 0, s_errorColor);
                if ((texture->m_flags & 0x400) == 0) {
                    ReleaseGxTex(&texture->m_placeholder);
                }
            }
        } else if (CreateGxTex(&texture->m_desc, &texture->m_gxTex)) {
            Upload(texture);
        } else {
            ReleaseGxTex(&texture->m_gxTex);
        }
    } else {
        if (page->m_gxTex == nullptr || (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(page->m_gxTex) + 0x10) & 0x40) != 0) {
            page->m_gxTex = nullptr;
            GXTEXDESC desc = {};
            desc.m_width = static_cast<uint32_t>(page->m_slotWidth << 3);
            desc.m_height = static_cast<uint32_t>(page->m_slotHeight << 3);
            desc.m_depth = 1;
            desc.m_format = page->m_format;
            desc.m_dataFormat = page->m_dataFormat;
            std::memcpy(&desc.m_usage, reinterpret_cast<uint8_t*>(page) + 0x3c, 8);
            GxTexCreate(&desc, &page->m_gxTex);  // FUN_14228c330 "Atlas"
        }
        AtlasUpload(page, texture);
    }
    MipBitsFree(&texture->m_mipBits);
    SMemFree(buffer, s_textureFile, 0x721, 0);
    texture->m_async = nullptr;
    ReleaseTexture(texture);
}

// The read failed: the error colour in its place.
// ref: FUN_14127b500
void OnError(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)size;
    (void)error;
    auto texture = static_cast<CTexture*>(userData);
    texture->m_async = nullptr;
    CreateSolidGx(texture, 0, 1, 1, true, 0, s_errorColor);
    MipBitsFree(&texture->m_mipBits);
    SMemFree(buffer, s_textureFile, 0x739, 0);
    ReleaseTexture(texture);
}

// ref: FUN_14127b440
void OnCancel(void* userData, void* buffer, uint32_t size) {
    (void)size;
    auto texture = static_cast<CTexture*>(userData);
    DestroyGxTex(&texture->m_gxTex);
    MipBitsFree(&texture->m_mipBits);
    SMemFree(buffer, s_textureFile, 0x744, 0);
    ReleaseTexture(texture);
    for (uint64_t i = 0; i < s_dropped.m_size; i++) {
        if (s_dropped.m_data[i] == texture) {
            if (i != s_dropped.m_size - 1) {
                s_dropped.m_data[i] = s_dropped.m_data[s_dropped.m_size - 1];
            }
            s_dropped.m_size--;
            break;
        }
    }
}

// ref: FUN_14127bad0
void LoadById(CTexture* texture, int32_t fileDataId, uint32_t priority) {
    uint32_t flags = texture->m_flags;
    AddRefCount(&texture->m_refCount);
    InterlockedExchange(&texture->m_state, 1);
    texture->m_flags &= ~0x2000u;
    auto open = ((flags >> 1) & 1) != 0 ? reinterpret_cast<void*>(&OpenById) : nullptr;
    texture->m_async = AsyncFileReadById(fileDataId, priority, texture, open, &AllocBuffer, &OnRead, &OnDone, &OnError,
        &OnCancel);
}

// ref: FUN_14127bb80
void LoadByName(CTexture* texture, const char* name, uint32_t priority) {
    uint32_t flags = texture->m_flags;
    AddRefCount(&texture->m_refCount);
    InterlockedExchange(&texture->m_state, 1);
    texture->m_flags &= ~0x2000u;
    auto open = ((flags >> 1) & 1) != 0 ? reinterpret_cast<void*>(&OpenByName) : nullptr;
    texture->m_async = AsyncFileReadByName(name, priority, texture, open, &AllocBuffer, &OnRead, &OnDone, &OnError,
        &OnCancel);
}

// The create flags onto the texture.
// ref: FUN_14127bd10
void ApplyCreateFlags(CTexture* texture, uint32_t createFlags, uint64_t gxFlags, uint8_t quality) {
    std::memcpy(&texture->m_desc.m_usage, &gxFlags, 8);
    uint32_t flags = texture->m_flags & 0xfffff7c7;
    texture->m_quality = quality;
    texture->m_flags = flags;
    if ((createFlags & 2) != 0) {
        flags |= 2;
        texture->m_flags = flags;
    }
    if ((createFlags & 4) != 0 && s_atlasEnabled != 0) {
        flags |= 4;
        texture->m_flags = flags;
    }
    if ((createFlags & 0x20) != 0) {
        flags |= 0x100;
        texture->m_flags = flags;
    }
    if ((createFlags & 0x40) != 0) {
        flags |= 0x200;
        texture->m_flags = flags;
    }
    if (static_cast<int8_t>(createFlags) < 0) {
        texture->m_flags = flags | 0x400;
    }
}

// The filter flags: the caller's, or the global filter and anisotropy.
uint64_t FilterFlags(uint64_t gxFlags, uint32_t createFlags) {
    uint32_t low = static_cast<uint32_t>(gxFlags);
    if ((createFlags & 1) != 0) {
        if ((low & 7) == 5 && (low & 0x3e000) > 0x20000) {
            gxFlags = (gxFlags & 0xfffffffffffe1fffull) | 0x20000;
        }
        return gxFlags;
    }
    low = ((low ^ s_filter) & 7) ^ low;
    uint32_t anisotropy = (s_filter & 7) != 5 ? 1 : s_anisotropy;
    return (gxFlags & 0xffffffff00000000ull) | ((((anisotropy << 0xd) ^ low) & 0x3e000) ^ low);
}

// A placeholder from the low-resolution preview table, while the texture loads.
// ref: FUN_141297be0
bool PreviewPlaceholder(int32_t fileDataId, CTexture* texture) {
    (void)fileDataId;
    (void)texture;
    // The preview table (DAT_1430a5768, filled by FUN_141297eb0) is not built yet, so no texture
    // has a preview; with it empty the reference returns here too.
    if (!GxDevExists()) {
        return false;
    }
    return false;
}

}

// ref: FUN_14127a680
CTexture::CTexture() {
    this->m_refCount = 0;
    std::memset(&this->m_desc, 0, sizeof(this->m_desc));
    this->m_desc.m_usage = 0x82001;
    this->m_async = nullptr;
    this->m_state = 0;
    this->m_gxTex = nullptr;
    this->m_placeholder = nullptr;
    this->m_flags = 0;
    this->m_mipBias = 0;
    this->m_quality = 0;
    this->m_mipCount = 0;
    this->m_alphaDepth = 0;
    this->m_compression = -1;
    this->m_90 = 0;
    this->m_fill.m_invoke = nullptr;
    this->m_fill.m_manager = &blz::function_empty_manager;
    this->m_mipBits = nullptr;
    this->m_atlas = nullptr;
    this->m_atlasSlot = 0;
    this->m_fileDataId = 0;
    this->m_key.m_flags = 0x82001;
    this->m_key.m_fileDataId = 0;
    this->m_key.m_matchAny = 0;
    this->m_cache = static_cast<uint8_t>((this->m_cache & 0xe0) | 0x20);
    AddRefCount(&this->m_refCount);
}

// Out of whichever cache holds it, then everything it owns.
// ref: FUN_14127a7b0
CTexture::~CTexture() {
    TextureReset(this);
    if ((this->m_cache & 1) == 0) {
        OsMutexLock(&s_uniqueLock);
        if ((this->m_cache & 2) != 0) {
            FileRemove(&s_unique, this->m_key);
        } else if ((this->m_cache & 4) != 0) {
            if (s_solid.m_buckets != nullptr) {
                SOLIDKEY key;
                key.m_color = static_cast<uint32_t>(this->m_key.m_flags);
                key.m_color2 = static_cast<uint32_t>(this->m_key.m_flags >> 32);
                key.m_kind = static_cast<uint32_t>(this->m_key.m_fileDataId);
                auto link = reinterpret_cast<SOLIDNODE**>(&s_solid.m_buckets[SolidHash(key) & (s_solid.m_bucketCount - 1)]);
                while (*link != nullptr) {
                    SOLIDNODE* node = *link;
                    if (node->m_key.m_color == key.m_color && node->m_key.m_color2 == key.m_color2
                        && node->m_key.m_kind == key.m_kind) {
                        *link = node->m_next;
                        SMemFree(node, s_solidNodeTag, -6, 0);
                        s_solid.m_count--;
                        break;
                    }
                    link = &node->m_next;
                }
            }
        } else if ((this->m_cache & 8) != 0) {
            uint64_t index = this->m_key.m_flags;
            FillQueueAt(&s_fillQueue, index) = FillQueueAt(&s_fillQueue, s_fillQueue.m_size - 1);
            FillQueueAt(&s_fillQueue, index)->m_key.m_flags = index;
            s_fillQueue.m_size--;
        }
        OsMutexUnlock(&s_uniqueLock);
    } else {
        s_locks.m_byFile.Enter();
        FileRemove(&s_byFile, this->m_key);
        s_locks.m_byFile.Leave();
    }
    this->m_name.~string();
    new (&this->m_name) blz::string();
    if (this->m_fill.m_manager != nullptr) {
        this->m_fill.m_manager(this->m_fill.m_storage, nullptr, 0);
    }
    void* placeholder = this->m_placeholder;
    if (placeholder != nullptr) {
        GxTexDestroy(&placeholder);
    }
    void* gx = this->m_gxTex;
    if (gx != nullptr) {
        GxTexDestroy(&gx);
    }
}

// --- creation --------------------------------------------------------------------------------

// ref: FUN_14127e660
HTEXTURE TextureCreate(int32_t fileDataId, uint64_t gxFlags, CStatus* status, uint32_t createFlags, uint32_t priority,
    uint8_t quality) {
    (void)status;
    gxFlags = FilterFlags(gxFlags, createFlags);
    CTexture* texture;
    if (ThreadPoolIsMainThread()) {
        texture = FindByFile(fileDataId, gxFlags);
        if (texture != nullptr) {
            return texture;
        }
        GxFrameWorkFlush(2);
    }
    s_locks.m_byFile.Enter();
    texture = FindByFile(fileDataId, gxFlags);
    if (texture == nullptr) {
        void* memory = SMemAlloc(sizeof(CTexture), 0x17, s_textureFile, SStrHashFnv1a(s_textureFile), 0xb9b, 0);
        texture = new (memory) CTexture();
        uint8_t ownFilter = static_cast<uint8_t>(~createFlags);
        texture->m_cache = static_cast<uint8_t>((((ownFilter << 4) ^ texture->m_cache) & 0x10) ^ texture->m_cache) | 1;
        texture->m_key.m_flags = gxFlags;
        texture->m_key.m_fileDataId = fileDataId;
        texture->m_key.m_matchAny = 0;
        bool inserted;
        FileInsert(&s_byFile, texture->m_key, texture, &inserted);
        texture->m_fileDataId = fileDataId;
        std::memcpy(&texture->m_desc.m_usage, &gxFlags, 8);
        texture->m_cache = static_cast<uint8_t>(((~(static_cast<uint8_t>(createFlags >> 8) << 5) ^ texture->m_cache) & 0x20)
            ^ texture->m_cache);
        ApplyCreateFlags(texture, createFlags, gxFlags, quality);
        if ((~createFlags & 4) != 0 && (static_cast<int8_t>(texture->m_flags) >= 0 || (texture->m_flags & 0x400) != 0)) {
            PreviewPlaceholder(fileDataId, texture);
        }
        if (texture->m_async != nullptr) {
            AsyncFileCancel(texture->m_async);
            texture->m_async = nullptr;
        }
        LoadById(texture, fileDataId, priority);
    }
    s_locks.m_byFile.Leave();
    return texture;
}

// ref: FUN_14127e630
HTEXTURE TextureCreateById(int32_t fileDataId, CStatus* status) {
    return TextureCreate(fileDataId, 0x82003, status, 0, 1, 0);
}

// By path: the path with its extension changed to .blp, then by the id the CAS knows it as;
// a path the CAS does not know becomes a unique texture loaded by name.
// ref: FUN_14127e940
HTEXTURE TextureCreateByName(const char* name, uint64_t gxFlags, CStatus* status, uint32_t createFlags,
    uint32_t priority, uint8_t quality) {
    (void)status;
    gxFlags = FilterFlags(gxFlags, createFlags);
    char path[0x110];
    OsPathChangeExtension(path, 0x104, name, ".blp");  // FUN_140214020
    int32_t fileDataId = cas::CASFileGetIdByName(path, (createFlags >> 1) & 0x21);
    uint8_t ownFilter = static_cast<uint8_t>(~createFlags);
    uint8_t notSampled = static_cast<uint8_t>(createFlags >> 8);
    CTexture* texture;
    if (fileDataId == 0) {
        void* memory = SMemAlloc(sizeof(CTexture), 0x17, s_textureFile, SStrHashFnv1a(s_textureFile), 0xb50, 0);
        texture = new (memory) CTexture();
        texture->m_cache = static_cast<uint8_t>((((ownFilter << 4) ^ texture->m_cache) & 0x10) ^ texture->m_cache);
        blz::string_assign(reinterpret_cast<blz::string_header*>(&texture->m_name), path, std::strlen(path));
        texture->m_fileDataId = 0;
        std::memcpy(&texture->m_desc.m_usage, &gxFlags, 8);
        texture->m_cache = static_cast<uint8_t>(((~(notSampled << 5) ^ texture->m_cache) & 0x20) ^ texture->m_cache);
        ApplyCreateFlags(texture, createFlags, gxFlags, quality);
        if (texture->m_async != nullptr) {
            AsyncFileCancel(texture->m_async);
            texture->m_async = nullptr;
        }
        LoadByName(texture, path, priority);
        OsMutexLock(&s_uniqueLock);
        int32_t index = s_uniqueNext;
        texture->m_flags |= 0x1000;
        texture->m_cache |= 2;
        s_uniqueNext++;
        texture->m_key.m_flags = gxFlags;
        texture->m_key.m_fileDataId = index;
        texture->m_key.m_matchAny = 0;
        bool inserted;
        FileInsert(&s_unique, texture->m_key, texture, &inserted);
        OsMutexUnlock(&s_uniqueLock);
        return texture;
    }
    if (ThreadPoolIsMainThread()) {
        texture = FindByFile(fileDataId, gxFlags);
        if (texture != nullptr) {
            return texture;
        }
        GxFrameWorkFlush(2);
    }
    s_locks.m_byFile.Enter();
    texture = FindByFile(fileDataId, gxFlags);
    if (texture == nullptr) {
        void* memory = SMemAlloc(sizeof(CTexture), 0x17, s_textureFile, SStrHashFnv1a(s_textureFile), 0xb35, 0);
        texture = new (memory) CTexture();
        texture->m_cache = static_cast<uint8_t>((((ownFilter << 4) ^ texture->m_cache) & 0x10) ^ texture->m_cache);
        blz::string_assign(reinterpret_cast<blz::string_header*>(&texture->m_name), path, std::strlen(path));
        texture->m_fileDataId = fileDataId;
        texture->m_cache = static_cast<uint8_t>(((~(notSampled << 5) ^ texture->m_cache) & 0x20) ^ texture->m_cache);
        ApplyCreateFlags(texture, createFlags, gxFlags, quality);
        if (texture->m_async != nullptr) {
            AsyncFileCancel(texture->m_async);
            texture->m_async = nullptr;
        }
        LoadByName(texture, path, priority);
        texture->m_cache |= 1;
        texture->m_key.m_flags = gxFlags;
        texture->m_key.m_fileDataId = fileDataId;
        texture->m_key.m_matchAny = 0;
        bool inserted;
        FileInsert(&s_byFile, texture->m_key, texture, &inserted);
    }
    s_locks.m_byFile.Leave();
    return texture;
}

// ref: FUN_14127e910
HTEXTURE TextureCreateByNameDefault(const char* name, CStatus* status) {
    return TextureCreateByName(name, 0x82003, status, 0, 1, 0);
}

// A unique texture the caller fills through `fill` (the Gx update callback's arguments).
// ref: FUN_14127edd0
HTEXTURE TextureCreateUnique(uint8_t type, uint32_t width, uint32_t height, uint32_t depth, uint8_t format,
    uint8_t dataFormat, uint64_t gxFlags, blz::function_storage* fill, const char* name, bool ownFilter) {
    void* memory = SMemAlloc(sizeof(CTexture), 0x17, s_textureFile, SStrHashFnv1a(s_textureFile), 0xaae, 0);
    CTexture* texture = new (memory) CTexture();
    const char* label = name != nullptr ? name : "UniqueTexture";
    blz::string_assign(reinterpret_cast<blz::string_header*>(&texture->m_name), label, std::strlen(label));
    texture->m_cache = static_cast<uint8_t>((texture->m_cache & 0xef) | (static_cast<uint8_t>(ownFilter) << 4));
    gxFlags = FilterFlags(gxFlags, ownFilter ? 0 : 1);
    texture->m_desc.m_dataFormat = dataFormat;
    texture->m_desc.m_format = format;
    texture->m_desc.m_width = width & 0xffff;
    texture->m_desc.m_height = height & 0xffff;
    texture->m_desc.m_depth = depth & 0xffff;
    texture->m_desc.m_type = type;
    std::memcpy(&texture->m_desc.m_usage, &gxFlags, 8);
    texture->m_fill.m_manager(texture->m_fill.m_storage, nullptr, 0);
    fill->m_manager(texture->m_fill.m_storage, fill->m_storage, 2);
    texture->m_fill.m_invoke = fill->m_invoke;
    texture->m_fill.m_manager = fill->m_manager;
    fill->m_invoke = nullptr;
    fill->m_manager = &blz::function_empty_manager;
    void* created = nullptr;
    GxTexCreate(&texture->m_desc, &created);
    void* old = texture->m_gxTex;
    texture->m_gxTex = created;
    if (old != nullptr) {
        GxTexDestroy(&old);
    }
    texture->m_async = nullptr;
    OsMutexLock(&s_uniqueLock);
    int32_t index = s_uniqueNext;
    if (texture->m_fill.m_invoke == nullptr) {
        texture->m_cache |= 8;
        texture->m_key.m_flags = s_fillQueue.m_size;
        *FillQueuePush(&s_fillQueue) = texture;
    } else {
        s_uniqueNext++;
        texture->m_cache |= 2;
        texture->m_key.m_flags = gxFlags;
        texture->m_key.m_fileDataId = index;
        texture->m_key.m_matchAny = 0;
        bool inserted;
        FileInsert(&s_unique, texture->m_key, texture, &inserted);
        auto gx = static_cast<uint16_t*>(texture->m_gxTex);
        int32_t rect[4] = { 0, 0, gx[1], gx[0] };
        GxTexUpdate(g_gxUploadInterface, gx, rect, false,
            [](void* param, uint32_t w, uint32_t h, uint32_t slice, uint32_t mip, uint32_t* pitch, const void** data) {
                // FUN_14127a630: the writer, called with its own storage.
                auto function = static_cast<blz::function_storage*>(param);
                typedef void (*FILL)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, const void**, void*);
                reinterpret_cast<FILL>(function->m_invoke)(w, h, slice, mip, pitch, data, function->m_storage);
            },
            &texture->m_fill);
    }
    OsMutexUnlock(&s_uniqueLock);
    return texture;
}

// ref: FUN_14127f9a0
HTEXTURE TextureCreateSolid(const uint32_t* color, const uint32_t* color2, const char* name, uint8_t kind) {
    (void)name;
    uint32_t first = *color;
    uint32_t second = color2 != nullptr ? *color2 : 0x7ba9317c;
    OsMutexLock(&s_uniqueLock);
    SOLIDKEY key = { first, second, kind };
    bool inserted;
    SOLIDNODE* node = SolidInsert(&s_solid, key, &inserted);
    if (!inserted) {
        AddRefCount(&node->m_texture->m_refCount);
    } else {
        void* memory = SMemAlloc(sizeof(CTexture), 0x17, s_textureFile, SStrHashFnv1a(s_textureFile), 0xbf1, 0);
        CTexture* texture = new (memory) CTexture();
        node->m_texture = texture;
        texture->m_cache |= 4;
        texture->m_key.m_flags = (static_cast<uint64_t>(second) << 32) | first;
        texture->m_key.m_fileDataId = kind;
        InitSolid(texture, color, color2, kind);
    }
    CTexture* result = node->m_texture;
    OsMutexUnlock(&s_uniqueLock);
    return result;
}

// ref: FUN_14127d730
void TextureAddRef(HTEXTURE texture) {
    AddRefCount(&texture->m_refCount);
}

void TextureRelease(HTEXTURE texture, int32_t flag) {
    (void)flag;
    ReleaseTexture(texture);
}

// ref: FUN_141281690
void TextureFree(HTEXTURE* texture) {
    if (*texture != nullptr) {
        ReleaseTexture(*texture);
        *texture = nullptr;
    }
}

// --- device events -----------------------------------------------------------------------------

// The device is back: every texture's Gx texture made again and its levels uploaded (read
// again from its file), solid and unique ones rebuilt, the atlas pages refilled, and the default
// textures made.
// ref: FUN_14127f120
void TextureOnDeviceCreated(void* param) {
    (void)param;
    s_maxMips = s_pendingMaxMips;
    s_reduction = s_pendingReduction;
    MIPBITS* scratch = nullptr;
    if (s_unique.m_count != 0 || s_fillQueue.m_size != 0 || s_atlasPages.m_next != &s_atlasPages) {
        scratch = MipBitsCreate(2, 0x1000, 0x1000, false);
    }
    s_locks.m_byFile.Enter();
    ForEachNode<FILENODE>(&s_byFile, [&](FILENODE* node) {
        CTexture* texture = node->m_texture;
        if ((texture->m_flags & 4) != 0 || texture->m_async != nullptr) {
            return;
        }
        if (texture->m_quality == 0) {
            texture->m_mipBits = scratch;
            if (!ReloadFromFile(texture, &texture->m_mipBits)) {
                FillWhite(texture->m_desc.m_width, texture->m_desc.m_height, 2, texture->m_desc.m_type == 1,
                    &texture->m_mipBits);
            }
            CreateGxTex(&texture->m_desc, &texture->m_gxTex);
            Upload(texture);
            texture->m_mipBits = nullptr;
            return;
        }
        texture->m_flags &= 0xfffff7c7;
        if (texture->m_fileDataId < 1) {
            LoadByName(texture, texture->m_name.m_data, 0);
        } else {
            if (static_cast<int8_t>(texture->m_flags) >= 0 || (texture->m_flags & 0x400) != 0) {
                PreviewPlaceholder(texture->m_fileDataId, texture);
            }
            LoadById(texture, texture->m_fileDataId, 0);
        }
        if ((texture->m_flags & 4) != 0) {
            if (texture->m_async != nullptr) {
                AsyncFileWait(texture->m_async);
            }
            if (texture->m_atlas != nullptr) {
                return;
            }
        }
        if (texture->m_async != nullptr) {
            AsyncFileWait(texture->m_async);
        }
    });
    s_locks.m_byFile.Leave();
    OsMutexLock(&s_uniqueLock);
    ForEachNode<FILENODE>(&s_unique, [&](FILENODE* node) {
        CTexture* texture = node->m_texture;
        if (((texture->m_flags >> 0xc) & 1) != 0 && ((texture->m_flags & 4) != 0 || texture->m_async != nullptr)) {
            return;
        }
        texture->m_mipBits = scratch;
        if (texture->m_fill.m_invoke == nullptr && !ReloadFromFile(texture, &texture->m_mipBits)) {
            FillWhite(texture->m_desc.m_width, texture->m_desc.m_height, 2, texture->m_desc.m_type == 1,
                &texture->m_mipBits);
        }
        CreateGxTex(&texture->m_desc, &texture->m_gxTex);
        if (texture->m_fill.m_invoke == nullptr) {
            Upload(texture);
        } else {
            auto gx = static_cast<uint16_t*>(texture->m_gxTex);
            int32_t rect[4] = { 0, 0, gx[1], gx[0] };
            GxTexUpdate(g_gxUploadInterface, gx, rect, false,
                [](void* p, uint32_t w, uint32_t h, uint32_t slice, uint32_t mip, uint32_t* pitch, const void** data) {
                    auto function = static_cast<blz::function_storage*>(p);
                    typedef void (*FILL)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, const void**, void*);
                    reinterpret_cast<FILL>(function->m_invoke)(w, h, slice, mip, pitch, data, function->m_storage);
                },
                &texture->m_fill);
        }
        texture->m_mipBits = nullptr;
    });
    ForEachNode<SOLIDNODE>(&s_solid, [](SOLIDNODE* node) {
        uint32_t color = node->m_key.m_color;
        uint32_t color2 = node->m_key.m_color2;
        InitSolid(node->m_texture, &color, &color2, static_cast<uint8_t>(node->m_key.m_kind));
    });
    OsMutexUnlock(&s_uniqueLock);
    s_locks.m_atlas.Enter();
    for (ATLASLINK* link = s_atlasPages.m_next; link != &s_atlasPages; link = link->m_next) {
        CTextureAtlas* page = AtlasOf(link);
        GXTEXDESC desc = {};
        desc.m_width = static_cast<uint32_t>(page->m_slotWidth << 3);
        desc.m_height = static_cast<uint32_t>(page->m_slotHeight << 3);
        desc.m_depth = 1;
        desc.m_format = page->m_format;
        desc.m_dataFormat = page->m_dataFormat;
        std::memcpy(&desc.m_usage, reinterpret_cast<uint8_t*>(page) + 0x3c, 8);
        GxTexCreate(&desc, &page->m_gxTex);  // FUN_14228c330 "Atlas"
        MIPBITS* bits = scratch;
        if (bits == nullptr) {
            bits = MipBitsCreate(2, static_cast<uint32_t>(page->m_slotWidth << 3), static_cast<uint32_t>(page->m_slotHeight << 3),
                false);
        }
        for (int32_t slot = 0; slot < 0x40; slot++) {
            CTexture* texture = page->m_slots[slot];
            if (texture != nullptr && texture->m_async == nullptr) {
                texture->m_mipBits = bits;
                if (!ReloadFromFile(texture, &bits)) {
                    // "CTextureAtlas::Reload(): Texture (FileData ID %d) not loaded.\n" (compiled out)
                    FillWhite(static_cast<uint32_t>(page->m_slotWidth << 3), static_cast<uint32_t>(page->m_slotHeight << 3), 2,
                        false, &bits);
                }
                AtlasUpload(page, texture);
                texture->m_mipBits = nullptr;
            }
        }
        if (scratch == nullptr) {
            MipBitsFree(&bits);
        }
    }
    s_locks.m_atlas.Leave();
    MipBitsFree(&scratch);
    uint32_t color = 0xff3f3f3f;
    s_defaultTexture = TextureCreateSolid(&color, nullptr, "", 0);
    color = 0xff707070;
    s_greyTexture = TextureCreateSolid(&color, nullptr, "defaultGrey", 0);
    color = 0xff000000;
    s_blackTexture = TextureCreateSolid(&color, nullptr, "defaultBlack", 0);
    color = 0xffffffff;
    s_whiteTexture = TextureCreateSolid(&color, nullptr, "defaultWhite", 0);
    color = 0x707070;
    s_transparentGreyTexture = TextureCreateSolid(&color, nullptr, "defaultTransparentGrey", 0);
    color = 0;
    s_transparentBlack3D = TextureCreateSolid(&color, nullptr, "defaultTransparentBlack3D", 1);
}

// The device is going: the defaults released and every Gx texture let go.
// ref: FUN_14127fb90
void TextureOnDeviceDestroyed(void* param) {
    (void)param;
    CTexture** defaults[6] = { &s_defaultTexture, &s_blackTexture, &s_greyTexture, &s_whiteTexture,
        &s_transparentGreyTexture, &s_transparentBlack3D };
    for (CTexture** slot : defaults) {
        if (*slot != nullptr) {
            ReleaseTexture(*slot);
            *slot = nullptr;
        }
    }
    s_locks.m_byFile.Enter();
    ForEachNode<FILENODE>(&s_byFile, [](FILENODE* node) {
        CTexture* texture = node->m_texture;
        if (texture->m_async == nullptr) {
            DestroyGxTex(&texture->m_gxTex);
        } else {
            long previous = InterlockedExchange(&texture->m_state, 3);
            if (previous == 2) {
                DestroyGxTex(&texture->m_gxTex);
                texture->m_flags |= 0x2000;
            }
        }
        DestroyGxTex(&texture->m_placeholder);
    });
    for (uint64_t i = 0; i < s_dropped.m_size; i++) {
        CTexture* texture = s_dropped.m_data[i];
        DestroyGxTex(&texture->m_gxTex);
        DestroyGxTex(&texture->m_placeholder);
    }
    s_dropped.m_size = 0;
    s_locks.m_byFile.Leave();
    OsMutexLock(&s_uniqueLock);
    ForEachNode<FILENODE>(&s_unique, [](FILENODE* node) {
        DestroyGxTex(&node->m_texture->m_gxTex);
    });
    ForEachNode<SOLIDNODE>(&s_solid, [](SOLIDNODE* node) {
        DestroyGxTex(&node->m_texture->m_gxTex);
    });
    for (uint64_t i = 0; i < s_fillQueue.m_size; i++) {
        DestroyGxTex(&FillQueueAt(&s_fillQueue, i)->m_gxTex);
    }
    OsMutexUnlock(&s_uniqueLock);
    s_locks.m_atlas.Enter();
    for (ATLASLINK* link = s_atlasPages.m_next; link != &s_atlasPages; link = link->m_next) {
        GxTexDestroy(&AtlasOf(link)->m_gxTex);
    }
    s_locks.m_atlas.Leave();
    s_settingsDirty = 0;
}

// Every texture read again (the settings changed).
// ref: FUN_14127cf00
void ReloadAll() {
    GxDeviceCallLock lock;
    s_locks.m_byFile.Enter();
    ForEachNode<FILENODE>(&s_byFile, [](FILENODE* node) {
        CTexture* texture = node->m_texture;
        if (texture->m_quality == 0 || texture->m_async != nullptr) {
            return;
        }
        texture->m_flags |= 0x80;
        CAsyncObject* async = texture->m_async;
        if (async != nullptr) {
            AsyncFileCancel(async);
            texture->m_async = nullptr;
            AsyncFileWait(async);
        }
        DestroyGxTex(&texture->m_gxTex);
        DestroyGxTex(&texture->m_placeholder);
    });
    GxDeviceBeginTextureReload();
    ForEachNode<FILENODE>(&s_byFile, [](FILENODE* node) {
        CTexture* texture = node->m_texture;
        if (texture->m_quality == 0 || texture->m_async != nullptr) {
            return;
        }
        texture->m_flags &= 0xfffff7c7;
        if (texture->m_fileDataId < 1) {
            LoadByName(texture, texture->m_name.m_data, 0);
        } else {
            if (static_cast<int8_t>(texture->m_flags) >= 0 || (texture->m_flags & 0x400) != 0) {
                PreviewPlaceholder(texture->m_fileDataId, texture);
            }
            LoadById(texture, texture->m_fileDataId, 0);
        }
        if ((texture->m_flags & 4) != 0) {
            if (texture->m_async != nullptr) {
                AsyncFileWait(texture->m_async);
            }
            if (texture->m_atlas != nullptr) {
                return;
            }
        }
        if (texture->m_async != nullptr) {
            AsyncFileWait(texture->m_async);
        }
    });
    GxDeviceEndTextureReload();
    s_locks.m_byFile.Leave();
}

// The per-frame check: settings changed since the last frame reload every texture.
// ref: FUN_1412812b0
void TextureApplySettings() {
    if (s_settingsDirty != 0) {
        s_maxMips = s_pendingMaxMips;
        s_reduction = s_pendingReduction;
        ReloadAll();
        s_settingsDirty = 0;
    }
}

namespace {

void ApplySettingsCallback(void* param) {
    (void)param;
    TextureApplySettings();
}

// The filter or anisotropy setting changed: textures that follow the global filter get it.
// ref: FUN_1412816c0
void OnRenderSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    (void)previous;
    (void)param;
    auto bytes = reinterpret_cast<const uint8_t*>(current);
    if (bytes[0x2d] == s_filter && bytes[0x2e] == s_anisotropy) {
        return;
    }
    s_anisotropy = bytes[0x2e];
    s_filter = bytes[0x2d];
    s_locks.m_byFile.Enter();
    ForEachNode<FILENODE>(&s_byFile, [](FILENODE* node) {
        CTexture* texture = node->m_texture;
        if ((texture->m_cache & 0x10) != 0 && texture->m_mipCount > 1) {
            texture->m_desc.m_usage ^= (s_filter ^ texture->m_desc.m_usage) & 7;
            texture->m_desc.m_usage = ((s_anisotropy & 0x1f) << 0xd) | (texture->m_desc.m_usage & 0xfffc1fff);
            if (texture->m_gxTex != nullptr) {
                GxTexSetFilter(texture->m_gxTex, s_filter, s_anisotropy);
            }
        }
    });
    s_locks.m_byFile.Leave();
}

}

// ref: FUN_141280a70
void TextureInitialize() {
    s_pendingMaxMips = 0xc;
    s_pendingReduction = 0;
    s_reduction = 0;
    s_maxMips = 0xc;
    GxAddDeviceCreatedCallback(&TextureOnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(&TextureOnDeviceDestroyed, nullptr);
    RenderSettingsAddHandler(&OnRenderSettingsChanged, nullptr);
    GxAddFrameStartCallback(&ApplySettingsCallback, nullptr);
    // The CAS update handlers (FUN_1402db190 list 1, FUN_1402db330 list 0) re-read the textures
    // a hot-fix changed; the in-place update is not ported, so they would never run.
}

// ref: FUN_14127faf0
void TextureShutdown() {
    GxRemoveDeviceCreatedCallback(&TextureOnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(&TextureOnDeviceDestroyed, nullptr);
    TextureOnDeviceDestroyed(nullptr);
}

// --- accessors ---------------------------------------------------------------------------------

namespace {

// The atlas page's Gx texture, the texture's own, or its placeholder (the shared test the
// reference inlines into every accessor).
void* OwnGxTex(CTexture* texture, bool* pending) {
    *pending = false;
    if ((texture->m_flags & 4) != 0) {
        if (texture->m_async != nullptr) {
            AsyncFileSetSortKey(texture->m_async, static_cast<int32_t>(0x80000000));
            *pending = true;
            return nullptr;
        }
        if (texture->m_atlas != nullptr) {
            return texture->m_atlas->m_gxTex;
        }
    }
    if (texture->m_async == nullptr && texture->m_gxTex != nullptr) {
        return texture->m_gxTex;
    }
    return texture->m_placeholder;
}

void* SolidGxTex(CTexture* texture) {
    if (texture == nullptr) {
        return nullptr;
    }
    bool pending;
    return OwnGxTex(texture, &pending);
}

void* WaitGxTex(CTexture* texture) {
    if ((texture->m_flags & 4) != 0) {
        if (texture->m_async != nullptr) {
            AsyncFileWait(texture->m_async);
        }
        if (texture->m_atlas != nullptr) {
            return texture->m_atlas->m_gxTex;
        }
    }
    if (texture->m_async != nullptr) {
        AsyncFileWait(texture->m_async);
    }
    return texture->m_gxTex;
}

}

// ref: FUN_1412808c0
void* TextureGetGxTex(HTEXTURE texture) {
    if (texture == nullptr) {
        return nullptr;
    }
    bool pending;
    return OwnGxTex(texture, &pending);
}

// ref: FUN_141280210
void* TextureGetGxTexPreferPlaceholder(HTEXTURE texture) {
    if (texture->m_placeholder != nullptr) {
        return texture->m_placeholder;
    }
    return SolidGxTex(s_defaultTexture);
}

// ref: FUN_141280410
void* TextureGetGxTexOrDefault(HTEXTURE texture) {
    if (texture != nullptr) {
        bool pending;
        void* gx = OwnGxTex(texture, &pending);
        if (!pending && gx != nullptr) {
            return gx;
        }
    }
    void* gx = WaitGxTex(texture);
    if (gx != nullptr) {
        return gx;
    }
    return SolidGxTex(s_defaultTexture);
}

// ref: FUN_141280550
void* TextureGetGxTexBlocking(HTEXTURE texture) {
    if (texture != nullptr) {
        bool pending;
        void* gx = OwnGxTex(texture, &pending);
        if (!pending && gx != nullptr) {
            return gx;
        }
    }
    return WaitGxTex(texture);
}

// A Gx texture left behind by a lost device is made again.
// ref: FUN_141280690
void* TextureGetGxTexOrDefaultRecreate(HTEXTURE texture) {
    if (texture != nullptr) {
        bool pending;
        void* gx = OwnGxTex(texture, &pending);
        if (!pending && gx != nullptr) {
            if ((*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(gx) + 0x10) & 0x40) == 0 || !GxDevExists()) {
                return gx;
            }
            DestroyGxTex(&texture->m_gxTex);
            if (CreateGxTex(&texture->m_desc, &texture->m_gxTex)) {
                return texture->m_gxTex;
            }
        }
    }
    return SolidGxTex(s_defaultTexture);
}

// ref: FUN_141280070
void* TextureGetGxTexWait(HTEXTURE texture) {
    return WaitGxTex(texture);
}

// ref: FUN_1412800d0
void* TextureGetGxTexIfResident(HTEXTURE texture) {
    if (texture->m_async != nullptr && AsyncFileIsResident(texture->m_async)) {
        return WaitGxTex(texture);
    }
    return nullptr;
}

// ref: FUN_1412803a0
void* TextureBlackGxTex() {
    return SolidGxTex(s_blackTexture);
}

// ref: FUN_141280620
void* TextureGreyGxTex() {
    return SolidGxTex(s_greyTexture);
}

// ref: FUN_1412807e0
void* TextureTransparentBlack3DGxTex() {
    return SolidGxTex(s_transparentBlack3D);
}

// ref: FUN_141280850
void* TextureTransparentGreyGxTex() {
    return SolidGxTex(s_transparentGreyTexture);
}

// ref: FUN_141280920
void* TextureWhiteGxTex() {
    return SolidGxTex(s_whiteTexture);
}

void* TextureDefaultGxTex() {
    return SolidGxTex(s_defaultTexture);
}

// ref: FUN_141280280
bool TextureGetSize(HTEXTURE texture, uint32_t* width, uint32_t* height, bool wait) {
    if (texture->m_async != nullptr) {
        if (!wait) {
            return false;
        }
        AsyncFileWait(texture->m_async);
    }
    if (width != nullptr) {
        *width = texture->m_desc.m_width;
    }
    if (height != nullptr) {
        *height = texture->m_desc.m_height;
    }
    return true;
}

// ref: FUN_1412802f0
int32_t TextureGetFileDataId(HTEXTURE texture) {
    return texture != nullptr ? texture->m_fileDataId : 0;
}

// ref: FUN_141280340
char* TextureGetName(HTEXTURE texture, char* buffer, uint64_t size) {
    *buffer = '\0';
    if (texture->m_name.m_size != 0 && size != 0) {
        const char* source = texture->m_name.m_data;
        char* dest = buffer;
        while (--size != 0) {
            char c = *source++;
            *dest++ = c;
            if (c == '\0') {
                return buffer;
            }
        }
        *dest = '\0';
    }
    return buffer;
}

// ref: FUN_141280c00
bool TextureIsLoaded(HTEXTURE texture) {
    if (texture == nullptr) {
        return false;
    }
    if ((texture->m_flags & 4) == 0) {
        if (texture->m_async != nullptr) {
            return texture->m_placeholder != nullptr;
        }
    } else {
        if (texture->m_async != nullptr) {
            return false;
        }
        if (texture->m_atlas != nullptr) {
            return true;
        }
    }
    return texture->m_gxTex != nullptr;
}

// ref: FUN_141280c60
bool TextureIsLoading(HTEXTURE texture) {
    return texture->m_async != nullptr;
}

// ref: FUN_141280c40
bool TextureIsReady(HTEXTURE texture) {
    return texture != nullptr && texture->m_async == nullptr;
}

// ref: FUN_141280bc0
bool TextureHasPlaceholder(HTEXTURE texture) {
    return texture->m_async != nullptr && texture->m_placeholder != nullptr;
}

// ref: FUN_141280c70
bool TextureIsOpaque(HTEXTURE texture) {
    if (texture != nullptr && texture->m_async == nullptr && texture->m_gxTex != nullptr) {
        return (*reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(texture->m_gxTex) + 0x10) & 1) != 0;
    }
    return false;
}

// ref: FUN_141280be0
bool TextureInFileCache(HTEXTURE texture) {
    return texture != nullptr && (texture->m_cache & 1) != 0;
}

// ref: FUN_1412812a0
bool TextureHasAtlas(HTEXTURE texture) {
    return texture->m_atlas != nullptr;
}

// ref: FUN_1412801c0
bool TextureGetAtlasCoords(HTEXTURE texture, float* origin, float* scale) {
    if (texture->m_atlas == nullptr) {
        return false;
    }
    *scale = 0.125f;
    uint32_t slot = static_cast<uint32_t>(texture->m_atlasSlot);
    origin[0] = static_cast<float>(slot & 7) * 0.125f;   // DAT_14261bf5c
    origin[1] = static_cast<float>((static_cast<int32_t>(slot) >> 3) & 7) * *scale;
    return true;
}

// ref: FUN_1412801b0
int32_t TextureLoadState(HTEXTURE texture) {
    return texture->m_async != nullptr ? 0 : 2;
}

// ref: FUN_1412818a0
bool TextureSetPriority(HTEXTURE texture, int32_t priority) {
    if (texture->m_async == nullptr) {
        return true;
    }
    return AsyncFileSetSortKey(texture->m_async, priority);
}

// ref: FUN_14127e590
bool TextureIsSampleable(HTEXTURE texture) {
    if (texture != nullptr && texture->m_gxTex != nullptr) {
        return (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(texture->m_gxTex) + 0x14) & 0x140000) == 0;  // FUN_142292860
    }
    return false;
}

// ref: FUN_1412814f0
void TextureAddAllLoadedCallback(blz::function_storage* callback, uint64_t key) {
    AddCallback(&s_allLoadedCallbacks, &s_allLoadedLock, s_allLoadedTag, callback, key);
}

void TextureAddLoadedCallback(blz::function_storage* callback, uint64_t key) {
    AddCallback(&s_loadedCallbacks, &s_loadedLock, s_callbackTag, callback, key);
}

// ref: FUN_1412818c0
void TextureRemoveAllLoadedCallback(uint64_t key) {
    s_allLoadedLock.lock();
    uint64_t kept = 0;
    for (uint64_t i = 0; i < s_allLoadedCallbacks.m_size; i++) {
        CALLBACKENTRY& entry = s_allLoadedCallbacks.m_data[i];
        if (entry.m_key != key) {
            if (kept != i) {
                s_allLoadedCallbacks.m_data[kept].m_key = entry.m_key;
                s_allLoadedCallbacks.m_data[kept].m_function.m_manager(s_allLoadedCallbacks.m_data[kept].m_function.m_storage,
                    nullptr, 0);
                MoveFunction(&s_allLoadedCallbacks.m_data[kept].m_function, &entry.m_function);
            }
            kept++;
        }
    }
    for (uint64_t i = kept; i < s_allLoadedCallbacks.m_size; i++) {
        s_allLoadedCallbacks.m_data[i].m_function.m_manager(s_allLoadedCallbacks.m_data[i].m_function.m_storage, nullptr, 0);
    }
    s_allLoadedCallbacks.m_size = kept;
    s_allLoadedLock.unlock();
}

// ref: FUN_141280060
void TextureSetAtlasEnabled(bool enabled) {
    s_atlasEnabled = enabled;
}

// ref: FUN_141281880
void TextureSetResolutionReduction(uint32_t reduction) {
    if (reduction > 2) {
        reduction = 2;
    }
    if (s_reduction != reduction) {
        s_settingsDirty = 1;
        s_pendingReduction = reduction;
    }
}

// ref: FUN_141280390
uint8_t TextureGetFilter() {
    return s_filter;
}

// ref: FUN_141280990
uint32_t TextureGetAnisotropy() {
    return s_anisotropy;
}

// ref: FUN_14127d830
int32_t TextureMemorySize(uint8_t format, uint32_t filter, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return 0;
    }
    auto level = [format](uint32_t w, uint32_t h) {
        uint32_t dim = BlockDim(format);
        return static_cast<int32_t>(((dim - 1 + h) / dim) * ((dim - 1 + w) / dim) * GxFormatBlockBytes(format));
    };
    int32_t size = level(width, height);
    if (filter > 1) {
        while (width != 1 || height != 1) {
            width = width < 2 ? width : width >> 1;
            height = height < 2 ? height : height >> 1;
            size += level(width, height);
        }
    }
    return size;
}

// The decode of a file outside the cache, into mips of the requested format (8: as the file is).
// ref: FUN_141281300
MIPBITS* TextureDecodeImage(const uint32_t* format, uint32_t kind, uint8_t* data, uint32_t size, uint32_t* width,
    uint32_t* height, bool* opaque, uint32_t* alphaBits) {
    MIPBITS* bits = nullptr;
    uint32_t wanted = format != nullptr ? *format : 8;
    BLPIMAGE image = {};
    if (kind == 0) {
        DecodePng(data, size, &bits, &image);
    } else if (kind == 1) {
        DecodeTga(data, size, &bits, &image);
    } else if (kind == 2) {
        if (ParseBlp(&image, data, size)) {
            if (wanted == 8) {
                wanted = image.m_format;
            }
            bits = MipBitsCreate(wanted, image.m_width, image.m_height, image.m_type == 2);
            BlpImageDecode(&image, wanted, bits, 0);
        }
    } else if (kind == 3) {
        DecodeJpeg(data, size, &bits, &image);
    }
    cas::CASFileFree(data);
    if (bits == nullptr) {
        return nullptr;
    }
    if (width != nullptr) {
        *width = image.m_width;
    }
    if (height != nullptr) {
        *height = image.m_height;
    }
    if (opaque != nullptr) {
        *opaque = image.m_alphaDepth == 0;
    }
    if (alphaBits != nullptr) {
        *alphaBits = image.m_alphaDepth & 0x3f;
    }
    return bits;
}

// ref: FUN_141281020
MIPBITS* TextureLoadImageById(int32_t fileDataId, uint32_t* width, uint32_t* height, uint32_t* format, bool* opaque,
    CStatus* status, uint32_t* alphaBits, uint8_t allowLoose) {
    uint8_t* data = nullptr;
    uint32_t size = 0;
    if (!cas::CASFileLoadById(fileDataId, &data, &size, allowLoose, 0x17)) {
        if (status != nullptr) {
            char error[0x200];
            status->Add(3, "Failed to load FileData ID %d: %s", fileDataId, cas::CASFormatLastError(error, sizeof(error)));
        }
        return nullptr;
    }
    uint32_t kind = DetectKind(data, size);
    if (data == nullptr) {
        return nullptr;
    }
    MIPBITS* bits = TextureDecodeImage(format, kind, data, size, width, height, opaque, alphaBits);
    if (bits == nullptr && status != nullptr) {
        status->Add(3, "Error loading texture file, FileData ID %d: unsupported image format\n", fileDataId);
    }
    return bits;
}

// ref: FUN_141281140
MIPBITS* TextureLoadImageByName(const char* name, uint32_t* width, uint32_t* height, uint32_t* format, bool* opaque,
    CStatus* status, uint32_t* alphaBits, uint8_t allowLoose) {
    char path[0x110];
    OsPathChangeExtension(path, 0x104, name, ".blp");
    uint8_t* data = nullptr;
    uint32_t size = 0;
    if (!cas::CASFileLoadByName(path, &data, &size, allowLoose, 0x17)) {
        if (status != nullptr) {
            char error[0x200];
            status->Add(3, "Failed to load %s: %s", path, cas::CASFormatLastError(error, sizeof(error)));
        }
        return nullptr;
    }
    uint32_t kind = DetectKind(data, size);
    if (data == nullptr) {
        return nullptr;
    }
    MIPBITS* bits = TextureDecodeImage(format, kind, data, size, width, height, opaque, alphaBits);
    if (bits == nullptr && status != nullptr) {
        status->Add(3, "Error loading texture file \"%s\": unsupported image format\n", name);
    }
    return bits;
}

namespace {

// A texture's source: the texture (0), a path (1) or a FileDataID (2).
struct TEXTURESOURCE {
    int32_t m_kind;
    union {
        CTexture* m_texture;
        const char* m_name;
        int32_t m_fileDataId;
    };
};

// The source's content key, when the CAS has one.
bool SourceContentKey(const TEXTURESOURCE& source, uint8_t* key) {
    if (source.m_kind == 0) {
        int32_t fileDataId = source.m_texture->m_fileDataId;
        if (fileDataId != 0) {
            return cas::CASFileGetContentKeyById(fileDataId, key);
        }
        return cas::CASFileGetContentKeyByName(source.m_texture->m_name.m_data, key, 0);
    }
    if (source.m_kind == 1) {
        return cas::CASFileGetContentKeyByName(source.m_name, key, 0);
    }
    if (source.m_kind == 2) {
        return cas::CASFileGetContentKeyById(source.m_fileDataId, key);
    }
    return false;
}

int32_t SourceFileDataId(const TEXTURESOURCE& source) {
    if (source.m_kind == 0) {
        CTexture* texture = source.m_texture;
        if (texture->m_fileDataId != 0) {
            return texture->m_fileDataId;
        }
        return cas::CASFileGetIdByName(texture->m_name.m_data, (texture->m_flags >> 1) & 1);
    }
    if (source.m_kind == 1) {
        return cas::CASFileGetIdByName(source.m_name, 1);
    }
    if (source.m_kind == 2) {
        return source.m_fileDataId;
    }
    return 0;
}

// ref: FUN_141280c90
bool TextureSourcesEqual(const TEXTURESOURCE& a, const TEXTURESOURCE& b) {
    uint8_t keyA[16];
    uint8_t keyB[16];
    if (SourceContentKey(a, keyA) && SourceContentKey(b, keyB)) {
        return std::memcmp(keyA, keyB, sizeof(keyA)) == 0;
    }
    return SourceFileDataId(a) == SourceFileDataId(b);
}

}

// ref: FUN_141280f10
bool TextureIsSameFile(HTEXTURE texture, int32_t fileDataId) {
    if ((texture->m_cache & 4) != 0) {
        return false;
    }
    TEXTURESOURCE a;
    a.m_kind = 0;
    a.m_texture = texture;
    TEXTURESOURCE b;
    b.m_kind = 2;
    b.m_fileDataId = fileDataId;
    return TextureSourcesEqual(a, b);
}

// ref: FUN_141280e60
bool TextureIsSameFileId(int32_t fileDataId, int32_t other) {
    TEXTURESOURCE a;
    a.m_kind = 2;
    a.m_fileDataId = fileDataId;
    TEXTURESOURCE b;
    b.m_kind = 2;
    b.m_fileDataId = other;
    return TextureSourcesEqual(a, b);
}
