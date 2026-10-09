#ifndef SHADEREFFECT_PIPELINE_MAP_HPP
#define SHADEREFFECT_PIPELINE_MAP_HPP

// The pipeline caches the UI and the shader effects keep (blz::unordered_map<PipelineHash,
// GxPipeline*>): keyed by what the pipeline was built from, hashed by the key's first word.
// A cache is read without a lock; new pipelines go into a second, locked map that is merged
// into the first at the start of the next frame (the TransientPipelineMap,
// ShaderEffectManager.cpp).

#include <cstdint>

#include "blz/ChainedHash.hpp"

struct GxPipeline;

// PipelineHash (0x18 bytes).
struct PIPELINEHASH {
    // Bits 0-15 the pixel shader permutation, 16-31 the vertex one, 32-34 the topology,
    // 35-44 the vertex format, 45-54 the target format kind, 55-62 the extra byte.
    uint64_t m_bits;                // 0x00
    uint64_t m_depthStencil;        // 0x08
    uint16_t m_blend;               // 0x10
    uint16_t m_pad12;
    uint32_t m_raster;              // 0x14
};
static_assert(sizeof(PIPELINEHASH) == 0x18, "PipelineHash is 0x18 bytes");

// chained_hash_node<pair<PipelineHash const, GxPipeline*>> (0x28 bytes).
struct PIPELINENODE {
    PIPELINENODE* m_next;           // 0x00
    PIPELINEHASH m_key;             // 0x08
    GxPipeline* m_pipeline;         // 0x20
};
static_assert(sizeof(PIPELINENODE) == 0x28, "a pipeline node is 0x28 bytes");

extern const char s_pipelineNodeTag[];
extern const char s_pipelineBucketTag[];

// Builds a key (FUN_1412ab590).
PIPELINEHASH* PipelineHashMake(PIPELINEHASH* key, uint16_t vsPermute, uint16_t psPermute, uint64_t depthStencil,
    uint16_t blend, uint32_t raster, uint8_t vertexFormat, uint8_t topology, uint32_t targetKind, uint8_t extra);

bool PipelineHashEqual(const PIPELINEHASH& a, const PIPELINEHASH& b);

// The node holding `key`, or null.
PIPELINENODE* PipelineMapFind(blz::chained_hash_header* map, const PIPELINEHASH& key);
// The node holding `key`, made (with no pipeline) when missing.
PIPELINENODE* PipelineMapInsert(blz::chained_hash_header* map, const PIPELINEHASH& key);
// Resizes the buckets (FUN_1406218f0).
void PipelineMapRehash(blz::chained_hash_header* map, uint64_t count);
// Frees the nodes, keeping the buckets (FUN_140621610).
void PipelineMapClear(blz::chained_hash_header* map);
// Frees the nodes and the buckets (FUN_1412ab610).
void PipelineMapDestroy(blz::chained_hash_header* map);
// Destroys every pipeline the map holds, leaving the nodes.
void PipelineMapDestroyPipelines(blz::chained_hash_header* map);

// TransientPipelineMap: `owner`'s locked map whose pipelines join `cache` at the next frame
// start (FUN_1412a2f50), and the owner going away (FUN_1412a4160).
struct TRANSIENTPIPELINEMAP {
    uint64_t m_owner;
    blz::chained_hash_header* m_transient;
    blz::chained_hash_header* m_cache;
};
void TransientPipelineMapAdd(const TRANSIENTPIPELINEMAP* map);
void TransientPipelineMapRemove(const TRANSIENTPIPELINEMAP* map);
// Merges every transient map into its cache (FUN_1412a39d0, a frame-start callback).
void TransientPipelineMapMerge(void* param);

#endif
