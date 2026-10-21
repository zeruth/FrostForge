#include "shadereffect/PipelineMap.hpp"

#include <cstring>

#include "gx/GxPipeline.hpp"
#include "os/Sync.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

const char s_pipelineNodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBVPipelineHash@@PEAUGxPipeline@@@blz@@@blz@@";
const char s_pipelineBucketTag[] = ".PEAU?$chained_hash_node@U?$pair@$$CBVPipelineHash@@PEAUGxPipeline@@@blz@@@blz@@";

namespace {

const char s_transientNodeTag[] = ".?AU?$chained_hash_node@UTransientPipelineMap@@@blz@@";
const char s_transientBucketTag[] = ".PEAU?$chained_hash_node@UTransientPipelineMap@@@blz@@";

// chained_hash_node<TransientPipelineMap> (0x20 bytes).
struct TRANSIENTNODE {
    TRANSIENTNODE* m_next;
    TRANSIENTPIPELINEMAP m_map;
};
static_assert(sizeof(TRANSIENTNODE) == 0x20, "a transient map node is 0x20 bytes");

// The transient maps (DAT_142cfc310) and their lock (DAT_1430a5b80).
blz::chained_hash_header s_transientMaps;
OSMUTEX s_transientLock = { SRWLOCK_INIT };

// The bucket count a table of `size` entries at `maxLoad` wants, `count` at least, as a power of
// two from 8.
uint64_t BucketsFor(const blz::chained_hash_header* map, uint64_t count) {
    float needed = static_cast<float>(map->m_size) / map->m_maxLoad;
    uint64_t wanted = static_cast<uint64_t>(needed);
    uint64_t grown = wanted + 1;
    if (static_cast<float>(map->m_size) <= static_cast<float>(wanted) * map->m_maxLoad) {
        grown = wanted;
    }
    if (grown < count) {
        grown = count;
    }
    uint64_t buckets = 8;
    while (buckets < grown) {
        buckets *= 2;
    }
    return buckets;
}

// ref: FUN_1412a45b0
void TransientRehash(blz::chained_hash_header* map, uint64_t count) {
    uint64_t buckets = BucketsFor(map, count);
    if (buckets == map->m_bucketCount && map->m_buckets != nullptr) {
        return;
    }
    auto fresh = static_cast<void**>(
        SMemAlloc(buckets * 8, 0xb, s_transientBucketTag, SStrHashFnv1a(s_transientBucketTag), -6, 0));
    std::memset(fresh, 0, buckets * 8);
    if (map->m_buckets != nullptr) {
        for (uint64_t i = 0; i < map->m_bucketCount; i++) {
            for (auto node = static_cast<TRANSIENTNODE*>(map->m_buckets[i]); node != nullptr;) {
                TRANSIENTNODE* next = node->m_next;
                uint64_t index = node->m_map.m_owner & (buckets - 1);
                node->m_next = static_cast<TRANSIENTNODE*>(fresh[index]);
                fresh[index] = node;
                node = next;
            }
        }
    }
    SMemFree(map->m_buckets, s_transientBucketTag, -6, 0);
    map->m_buckets = fresh;
    map->m_bucketCount = buckets;
}

// ref: FUN_1412a4200
void TransientClear(blz::chained_hash_header* map) {
    if (map->m_buckets == nullptr) {
        map->m_size = 0;
        return;
    }
    for (uint64_t i = 0; i < map->m_bucketCount; i++) {
        for (auto node = static_cast<TRANSIENTNODE*>(map->m_buckets[i]); node != nullptr;) {
            TRANSIENTNODE* next = node->m_next;
            SMemFree(node, s_transientNodeTag, -6, 0);
            node = next;
        }
        map->m_buckets[i] = nullptr;
    }
    map->m_size = 0;
}

}

// ref: FUN_1412ab590
PIPELINEHASH* PipelineHashMake(PIPELINEHASH* key, uint16_t vsPermute, uint16_t psPermute, uint64_t depthStencil,
    uint16_t blend, uint32_t raster, uint8_t vertexFormat, uint8_t topology, uint32_t targetKind, uint8_t extra) {
    uint64_t low = static_cast<uint64_t>(psPermute) | (static_cast<uint64_t>(vsPermute) << 0x10);
    key->m_depthStencil = depthStencil;
    key->m_raster = raster;
    key->m_blend = blend;
    key->m_pad12 = 0;
    key->m_bits = ((low | (static_cast<uint64_t>(topology) << 0x20)) & 0x7ffffffffull | (static_cast<uint64_t>(vertexFormat) << 0x23))
        ^ (static_cast<uint64_t>(targetKind & 0x3ff) << 0x2d) | (static_cast<uint64_t>(extra) << 0x37);
    return key;
}

bool PipelineHashEqual(const PIPELINEHASH& a, const PIPELINEHASH& b) {
    return a.m_bits == b.m_bits && a.m_raster == b.m_raster && a.m_depthStencil == b.m_depthStencil && a.m_blend == b.m_blend;
}

PIPELINENODE* PipelineMapFind(blz::chained_hash_header* map, const PIPELINEHASH& key) {
    if (map->m_buckets == nullptr) {
        return nullptr;
    }
    for (auto node = static_cast<PIPELINENODE*>(map->m_buckets[key.m_bits & (map->m_bucketCount - 1)]); node != nullptr;
         node = node->m_next) {
        if (PipelineHashEqual(node->m_key, key)) {
            return node;
        }
    }
    return nullptr;
}

// ref: FUN_1412aee70
PIPELINENODE* PipelineMapInsert(blz::chained_hash_header* map, const PIPELINEHASH& key) {
    if (map->m_buckets == nullptr) {
        PipelineMapRehash(map, map->m_bucketCount);
    }
    auto link = reinterpret_cast<PIPELINENODE**>(&map->m_buckets[key.m_bits & (map->m_bucketCount - 1)]);
    while (*link != nullptr) {
        if (PipelineHashEqual((*link)->m_key, key)) {
            return *link;
        }
        link = &(*link)->m_next;
    }
    auto node = static_cast<PIPELINENODE*>(
        SMemAlloc(sizeof(PIPELINENODE), 0xb, s_pipelineNodeTag, SStrHashFnv1a(s_pipelineNodeTag), -6, 0));
    node->m_next = nullptr;
    node->m_key = key;
    node->m_pipeline = nullptr;
    *link = node;
    map->m_size++;
    if (map->m_maxLoad < static_cast<float>(map->m_size) / static_cast<float>(map->m_bucketCount)) {
        PipelineMapRehash(map, map->m_bucketCount * 2);
    }
    return node;
}

// ref: FUN_1406218f0
void PipelineMapRehash(blz::chained_hash_header* map, uint64_t count) {
    uint64_t buckets = BucketsFor(map, count);
    if (buckets == map->m_bucketCount && map->m_buckets != nullptr) {
        return;
    }
    auto fresh = static_cast<void**>(
        SMemAlloc(buckets * 8, 0xb, s_pipelineBucketTag, SStrHashFnv1a(s_pipelineBucketTag), -6, 0));
    std::memset(fresh, 0, buckets * 8);
    if (map->m_buckets != nullptr) {
        for (uint64_t i = 0; i < map->m_bucketCount; i++) {
            for (auto node = static_cast<PIPELINENODE*>(map->m_buckets[i]); node != nullptr;) {
                PIPELINENODE* next = node->m_next;
                uint64_t index = node->m_key.m_bits & (buckets - 1);
                node->m_next = static_cast<PIPELINENODE*>(fresh[index]);
                fresh[index] = node;
                node = next;
            }
        }
    }
    SMemFree(map->m_buckets, s_pipelineBucketTag, -6, 0);
    map->m_buckets = fresh;
    map->m_bucketCount = buckets;
}

// ref: FUN_140621610
void PipelineMapClear(blz::chained_hash_header* map) {
    if (map->m_buckets != nullptr) {
        for (uint64_t i = 0; i < map->m_bucketCount; i++) {
            for (auto node = static_cast<PIPELINENODE*>(map->m_buckets[i]); node != nullptr;) {
                PIPELINENODE* next = node->m_next;
                SMemFree(node, s_pipelineNodeTag, -6, 0);
                node = next;
            }
            map->m_buckets[i] = nullptr;
        }
    }
    map->m_size = 0;
}

// ref: FUN_1412ab610
void PipelineMapDestroy(blz::chained_hash_header* map) {
    if (map->m_buckets == nullptr) {
        return;
    }
    for (uint64_t i = 0; i < map->m_bucketCount; i++) {
        for (auto node = static_cast<PIPELINENODE*>(map->m_buckets[i]); node != nullptr;) {
            PIPELINENODE* next = node->m_next;
            SMemFree(node, s_pipelineNodeTag, -6, 0);
            node = next;
        }
    }
    SMemFree(map->m_buckets, s_pipelineBucketTag, -6, 0);
    map->m_buckets = nullptr;
}

void PipelineMapDestroyPipelines(blz::chained_hash_header* map) {
    blz::chained_for_each<PIPELINENODE>(map, [](PIPELINENODE* node) {
        GxPipelineDestroy(&node->m_pipeline);
    });
}

// ref: FUN_1412a2f50
void TransientPipelineMapAdd(const TRANSIENTPIPELINEMAP* map) {
    OsMutexLock(&s_transientLock);
    if (s_transientMaps.m_buckets == nullptr) {
        TransientRehash(&s_transientMaps, s_transientMaps.m_bucketCount);
    }
    auto link = reinterpret_cast<TRANSIENTNODE**>(&s_transientMaps.m_buckets[map->m_owner & (s_transientMaps.m_bucketCount - 1)]);
    while (*link != nullptr && (*link)->m_map.m_owner != map->m_owner) {
        link = &(*link)->m_next;
    }
    if (*link == nullptr) {
        auto node = static_cast<TRANSIENTNODE*>(
            SMemAlloc(sizeof(TRANSIENTNODE), 0xb, s_transientNodeTag, SStrHashFnv1a(s_transientNodeTag), -6, 0));
        node->m_next = nullptr;
        node->m_map = *map;
        *link = node;
        s_transientMaps.m_size++;
        if (s_transientMaps.m_maxLoad < static_cast<float>(s_transientMaps.m_size) / static_cast<float>(s_transientMaps.m_bucketCount)) {
            TransientRehash(&s_transientMaps, s_transientMaps.m_bucketCount * 2);
        }
    }
    OsMutexUnlock(&s_transientLock);
}

// ref: FUN_1412a4160
void TransientPipelineMapRemove(const TRANSIENTPIPELINEMAP* map) {
    OsMutexLock(&s_transientLock);
    if (s_transientMaps.m_buckets != nullptr) {
        auto link = reinterpret_cast<TRANSIENTNODE**>(
            &s_transientMaps.m_buckets[map->m_owner & (s_transientMaps.m_bucketCount - 1)]);
        while (*link != nullptr && (*link)->m_map.m_owner != map->m_owner) {
            link = &(*link)->m_next;
        }
        TRANSIENTNODE* node = *link;
        if (node != nullptr) {
            *link = node->m_next;
            SMemFree(node, s_transientNodeTag, -6, 0);
            s_transientMaps.m_size--;
        }
    }
    OsMutexUnlock(&s_transientLock);
}

// Every transient map's pipelines join its cache and the transient map empties; then the list
// of transient maps empties.
// ref: FUN_1412a39d0
void TransientPipelineMapMerge(void* param) {
    (void)param;
    if (s_transientMaps.m_size == 0) {
        return;
    }
    blz::chained_for_each<TRANSIENTNODE>(&s_transientMaps, [](TRANSIENTNODE* node) {
        blz::chained_hash_header* cache = node->m_map.m_cache;
        blz::chained_hash_header* transient = node->m_map.m_transient;
        blz::chained_for_each<PIPELINENODE>(transient, [&](PIPELINENODE* entry) {
            PIPELINENODE* merged = PipelineMapInsert(cache, entry->m_key);
            if (merged->m_pipeline == nullptr) {
                merged->m_pipeline = entry->m_pipeline;
            }
        });
        PipelineMapClear(transient);
    });
    TransientClear(&s_transientMaps);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1412a4200 = (void*)&TransientClear;
// ---- end of generated globals ----
