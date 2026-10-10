#include "model2/M2Element.hpp"

#include <cstring>

#include "client/RenderSettings.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxShader.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

// The M2Element setters and Model3::PipelineCache (Engine/Source/Model2).

namespace {

// DAT_142da1dd0, DAT_142da2000.
const char s_blockArrayName[] = ".PEAU?$pair@UKey@PipelineCache@Model3@@V?$unique_ptr@UGxPipeline@@UGxResourceDelete@@@blz@@@blz@@";
const char s_blockName[] = ".?AU?$pair@UKey@PipelineCache@Model3@@V?$unique_ptr@UGxPipeline@@UGxResourceDelete@@@blz@@@blz@@";

// The block a pass falls back to (DAT_1430a12e0): the model shaders 0x49 and 0x7e.
struct M2SHADERBLOCK {
    uint64_t m_00;
    uint64_t m_08;
    int32_t m_vertexShader;
    int32_t m_pixelShader;
    uint64_t m_flags;
};
M2SHADERBLOCK s_defaultBlock;

// The flags a block must carry for passes 0 and 2 to keep it (DAT_142722958).
const uint64_t s_passBlockMasks[3] = { 0x700, 0, 0x300 };

// Sets bits [shift, shift + width) of `word` to `value` (the reference's xor-and-mask form).
uint64_t SetBits(uint64_t word, uint64_t value, uint64_t keepMask) {
    return ((value ^ word) & keepMask) ^ value;
}

}  // namespace

// ref: FUN_141205160
void M2Element::Initialize(uint32_t kind, uint64_t a3, CM2BaseModel* model, const M2SkinSection* section, float alpha,
    float screenSize, float distance, uint32_t a9, uint32_t flags) {
    this->m_batchKey = (reinterpret_cast<uint64_t>(model) & 0xfffffffffffffff0ull) << 0x24;
    this->m_sortKey = 0;
    uint64_t file = reinterpret_cast<uint64_t>(model->GetShared()) >> 1;
    uint64_t key = ((file ^ this->m_sortKey) & 0xffffffff80000007ull) ^ file;
    this->m_sortKey = key;
    uint64_t level = 0;
    if (section != nullptr) {
        level = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(section) + 0xe);
    }
    this->m_batchKey = SetBits(this->m_batchKey, level << 0x24, 0xffffff0fffffffffull);
    key = ((static_cast<uint64_t>(flags >> 2 & 1) << 2) | (key & 0x1ffffff87ffffffbull) | (a3 << 0x3d))
        ^ (static_cast<uint64_t>(kind & 0xf) << 0x1f);
    this->m_sortKey = key;
    uint64_t lod = model->m_lod;
    if (2 < model->m_lod) {
        lod = 3;
    }
    this->m_alpha = alpha;
    this->m_model = model;
    this->m_sortKey = ((key ^ lod) & 0xfffffffffffffffcull) ^ lod;
    this->m_3c = a9;
    this->m_screenSize = screenSize;
    this->m_distance = distance;
    this->m_flags = flags;
    this->m_batchIndex = 0;
    this->m_38 = 0;
    this->m_batch = nullptr;
    this->m_section = section;
    this->m_60 = -1;
    this->m_64 = 0;
}

// ref: FUN_1412052c0
void M2Element::SetEmitter(CParticleEmitter2* emitter) {
    uint64_t key = SetBits(this->m_batchKey, static_cast<uint64_t>(emitter->m_blend) << 0x20, 0xfffffff0ffffffffull);
    key = SetBits(key, static_cast<uint64_t>(emitter->m_renderFlags) << 0x1b, 0xffffffff07ffffffull);
    this->m_emitter = emitter;
    this->m_batchKey = SetBits(key, static_cast<uint64_t>(emitter->m_material) << 0x12, 0xfffffffff803ffffull);
}

// ref: FUN_141205340
void M2Element::SetPipeline(GxPipeline* pipeline) {
    this->m_pipeline = pipeline;
    uint64_t bits = reinterpret_cast<uint64_t>(pipeline) << 0x1f;
    this->m_sortKey = SetBits(this->m_sortKey, bits, 0xe0000007ffffffffull);
}

// ref: FUN_141205370
void M2Element::SetBatch(uint32_t index, const void* batch, const uint16_t* textures) {
    uint64_t key = SetBits(this->m_batchKey, static_cast<uint64_t>(index) * 8, 0xfffffffffffc0007ull);
    this->m_batchKey = key;
    uint64_t material = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(batch) + 0x10);
    key = SetBits(key, material << 0x12, 0xfffffffff803ffffull);
    this->m_batchKey = key;
    key = SetBits(key, static_cast<uint64_t>(textures[0]) << 0x1b, 0xffffffff07ffffffull);
    this->m_batchKey = key;
    uint16_t second = textures[1];
    this->m_batchIndex = index;
    this->m_batch = batch;
    this->m_batchKey = SetBits(key, static_cast<uint64_t>(second) << 0x20, 0xfffffff0ffffffffull);
}

// A ribbon: its textures hashed into the key in place of a material.
// ref: FUN_141205410
void M2Element::SetRibbon(uint32_t index) {
    CM2Model* model = nullptr;
    if (this->m_model->m_type == 0) {
        model = static_cast<CM2Model*>(this->m_model);
    }
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        model->WaitForLoad(0);
    }
    const M2Ribbon* ribbon = &model->m_shared->m_data->m_ribbons[index];
    uint64_t key = SetBits(this->m_batchKey, static_cast<uint64_t>(index) * 8, 0xfffffffffffc0007ull);
    this->m_batchKey = key;
    uint16_t count = static_cast<uint16_t>(ribbon->m_textureIndices.Count());
    uint16_t hash = 0;
    if (count != 0) {
        const uint16_t* entry = ribbon->m_textureIndices.Data();
        hash = count;
        for (uint32_t i = count; i != 0; i--) {
            hash = static_cast<uint16_t>((hash >> 6 | hash << 3) ^ *entry);
            entry++;
        }
    }
    this->m_batchIndex = index;
    this->m_batchKey = SetBits(key, static_cast<uint64_t>(hash) << 0x12, 0xfffffffff803ffffull);
}

// ref: FUN_141205510
void M2Element::SetShadowBatch(uint32_t index, const void* batch) {
    uint64_t key = SetBits(this->m_batchKey, static_cast<uint64_t>(index) * 8, 0xfffffffffffc0007ull);
    this->m_batchKey = key;
    uint16_t material = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(batch) + 6);
    this->m_flags |= 0x4000;
    this->m_batchIndex = index;
    this->m_batch = batch;
    this->m_batchKey = SetBits(key, static_cast<uint64_t>(material) << 0x12, 0xfffffffff803ffffull);
}

void M2Element::SetFaded(bool faded) {
    this->m_flags = faded ? (this->m_flags | 0x80) : (this->m_flags & 0xffffff7f);
}

// ref: FUN_141205580
GxPipeline** M2PipelineCache::Get(const M3PIPELINEENTRY* key) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(key);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < 0x90; i++) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    }
    uint64_t count = this->m_bucketCount;
    M3PIPELINEENTRY* buckets = this->m_buckets;
    uint64_t mask = count - 1;
    uint64_t index = mask & hash;
    M3PIPELINEENTRY* entry = &buckets[index];
    uint8_t state = entry->m_state;
    if (state != 1) {
        uint64_t step = 0;
        while (true) {
            if ((this->m_deleted == 0 || state != 2) && std::memcmp(entry, key, 0x90) == 0) {
                if (entry != &buckets[count]) {
                    return &entry->m_pipeline;
                }
                break;
            }
            step++;
            index = (index + step) & mask;
            entry = &buckets[index];
            state = entry->m_state;
            if (state == 1) {
                break;
            }
        }
    }
    OsMutexLock(&this->m_lock);
    uint64_t at = this->m_first;
    uint64_t end = this->m_size + at;
    for (; at != end; at++) {
        M3PIPELINEENTRY* queued = &this->m_blocks[(at >> 3) & (this->m_blockCount - 1)][at & 7];
        if (std::memcmp(queued, key, 0x90) == 0) {
            break;
        }
    }
    if (at == end) {
        // The desc is laid over a copy of the key, so the bytes no field claims keep the key's.
        uint8_t raw[sizeof(GXPIPELINEDESC)];
        std::memcpy(raw + 8, key, sizeof(raw) - 8);
        auto desc = reinterpret_cast<GXPIPELINEDESC*>(raw);
        RENDERTARGETFORMATS formats = RenderTargetFormats(key->m_targetKind);
        desc->m_vsPermute = key->m_vsPermute;
        desc->m_gsPermute = 0;
        desc->m_psPermute = key->m_psPermute;
        desc->m_topology = 3;
        desc->m_samplesLog2 = 0;
        if (key->m_targetKind == 6) {
            desc->m_samplesLog2 = g_renderSettings.m_msaaSamples;
        }
        std::memcpy(raw + 0x08, &key->m_raster, 4);
        std::memcpy(raw + 0x10, &key->m_depthStencil, 8);
        std::memcpy(raw + 0x18, &formats, 5);
        std::memset(raw + 0x20, 0, 4);
        desc->m_vertexShader = static_cast<int32_t>(key->m_vertexShader);
        desc->m_geometryShader = 0;
        desc->m_pixelShader = static_cast<int32_t>(key->m_pixelShader);
        desc->m_layouts = const_cast<GXVERTEXBUFFERFORMAT**>(key->m_layouts);
        uint64_t layoutCount = key->m_layoutCount;
        std::memcpy(raw + 0x40, &layoutCount, 4);
        std::memcpy(raw + 0x44, key->m_blend, 8);
        std::memcpy(raw + 0x4c, &key->m_24, 2);
        GxPipeline* pipeline = GxPipelineCreateDesc(desc, "PH Model3");
        M3PIPELINEENTRY* slot = this->PushBack();
        if (slot == nullptr) {
            if (pipeline != nullptr) {
                GxPipelineDestroy(&pipeline);
            }
        } else {
            std::memcpy(slot, key, 0x90);
            slot->m_pipeline = pipeline;
        }
        at = this->m_first - 1 + this->m_size;
    }
    entry = &this->m_blocks[(this->m_blockCount - 1) & (at >> 3)][at & 7];
    OsMutexUnlock(&this->m_lock);
    return &entry->m_pipeline;
}

// ref: FUN_1412059f0
GxPipeline* M2PipelineCache::Get(const M3PIPELINEREQUEST* request) {
    M3PIPELINEENTRY key;
    std::memset(reinterpret_cast<uint8_t*>(&key) + 0x27, 0, 0x90 - 0x27);
    key.m_depthStencil = request->m_depthStencil;
    key.m_raster = request->m_raster;
    std::memcpy(key.m_blend, request->m_blend, 8);
    key.m_24 = request->m_34;
    key.m_vertexShader = request->m_vertexShader;
    key.m_pixelShader = request->m_pixelShader;
    key.m_vsPermute = request->m_vsPermute;
    key.m_psPermute = request->m_psPermute;
    key.m_targetKind = request->m_targetKind;
    key.m_layoutCount = static_cast<uint8_t>(request->m_layoutCount);
    std::memcpy(key.m_layouts, request->m_layouts, static_cast<uint64_t>(request->m_layoutCount) << 3);
    return *this->Get(&key);
}

// ref: FUN_141205b10
M3PIPELINEENTRY* M2PipelineCache::PushBack() {
    uint64_t first = this->m_first;
    if (((this->m_size + first) & 7) == 0 && this->m_blockCount <= (this->m_size + 8) >> 3) {
        uint64_t count = this->m_blockCount;
        uint64_t grown;
        if (count == 0) {
            grown = 8;
        } else {
            grown = count;
            while (grown < count + 1) {
                grown *= 2;
            }
        }
        auto blocks = static_cast<M3PIPELINEENTRY**>(SMemAlloc(grown * 8, 0xb, s_blockArrayName, SStrHashFnv1a(s_blockArrayName), -6, 0));
        uint64_t start = (this->m_first >> 3) & (this->m_blockCount - 1);
        std::memcpy(blocks, this->m_blocks + start, (this->m_blockCount - start) * 8);
        std::memcpy(blocks + (this->m_blockCount - start), this->m_blocks, start * 8);
        std::memset(blocks + this->m_blockCount, 0, (grown - this->m_blockCount) * 8);
        SMemFree(this->m_blocks, s_blockArrayName, -6, 0);
        this->m_first &= 7;
        first = this->m_first;
        this->m_blocks = blocks;
        this->m_blockCount = grown;
    }
    uint64_t index = this->m_size + first;
    this->m_size++;
    uint64_t block = (this->m_blockCount - 1) & (index >> 3);
    if (this->m_blocks[block] == nullptr) {
        this->m_blocks[block] = static_cast<M3PIPELINEENTRY*>(SMemAlloc(0x4c0, 0xb, s_blockName, SStrHashFnv1a(s_blockName), -6, 0));
    }
    return &this->m_blocks[block][index & 7];
}

// ref: FUN_141205970
void M2ElementDeviceCreated() {
    int32_t vertexShader = GxShaderVertexId(0x49);
    int32_t pixelShader = GxShaderPixelId(0x7e);
    s_defaultBlock.m_00 = 0;
    s_defaultBlock.m_08 = 0;
    s_defaultBlock.m_vertexShader = vertexShader;
    s_defaultBlock.m_pixelShader = pixelShader;
    s_defaultBlock.m_flags = 0;
}

// ref: FUN_1412059c0
void M2ElementDeviceLost() {
    s_defaultBlock = {};
}

// ref: FUN_141205ab0
const void* M2ElementSelectBlock(int32_t pass, const void* block) {
    if ((5u >> (pass & 0x1f) & 1) == 0) {
        return block;
    }
    uint64_t flags = static_cast<const M2SHADERBLOCK*>(block)->m_flags;
    if ((flags & s_passBlockMasks[pass]) != 0) {
        return block;
    }
    return &s_defaultBlock;
}
