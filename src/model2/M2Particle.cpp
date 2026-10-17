#include "model2/M2Particle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include "base/Matrix.hpp"
#include "base/ObjectAlloc.hpp"
#include "blz/Sync.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxVertexFormat.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "os/Sync.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/ParticleSystem/ParticleSystem2.cpp (0x141240fe0..0x14124ff80): the emitter,
// its particle list and the update. The draw is in M2ParticleRender.cpp and the generators in
// M2ParticleGenerator.cpp.

void M2IndicesReserve(M2SceneVector<uint32_t>* indices, uint64_t count);
void M2IndicesPush(M2SceneVector<uint32_t>* indices, uint32_t value);

namespace {

const char s_particleFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\ParticleSystem\\ParticleSystem2.cpp";
const char s_allocFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\ObjectAlloc\\ObjectAllocVariableSize.h";
const char s_indexName[] = ".?I";  // DAT_142d61348
const char s_heapPairName[] = ".?AU?$pair@IPEAVTObjectAllocMemBlock@@@blz@@";  // DAT_142da2ea0

// Storage at most this big comes from Storm rather than the heaps (FUN_1412c44d0).
constexpr uint32_t PARTICLE_HEAP_THRESHOLD = 0x100000;

// The traits by element kind (vtables 0x142724958, 0x142724978, 0x142724998, 0x1427249b8).
class ParticleTraitsPlain : public CParticleTraits {
    public:
    // ref: FUN_141241010
    void Construct(uint8_t* data, uint64_t count) const override {
        for (; count != 0; count--) {
            std::memset(data, 0, sizeof(CParticle2));
            data += sizeof(CParticle2);
        }
    }
    // ref: FUN_141241050
    void Destroy(uint8_t* data, uint64_t count) const override {
        (void)data;
        (void)count;
    }
    // ref: FUN_141241090
    void Move(uint8_t* to, uint8_t* from, uint64_t count) const override {
        for (; count != 0; count--) {
            std::memcpy(to, from, sizeof(CParticle2));  // FUN_14125cbc0
            to += sizeof(CParticle2);
            from += sizeof(CParticle2);
        }
    }
};

// ref: FUN_14125cc00
void ParticleModelConstruct(CParticle2_Model* particle) {
    std::memset(particle, 0, sizeof(CParticle2));
    particle->m_rotation[0] = 0.0f;
    particle->m_rotation[1] = 0.0f;
    particle->m_rotation[2] = 0.0f;
    particle->m_rotation[3] = 1.0f;
    particle->m_spin = {0.0f, 0.0f, 0.0f};
    particle->m_pad3c = 0;
    particle->m_model = nullptr;
}

// ref: FUN_14125cc30
void ParticleModelDestroy(CParticle2_Model* particle) {
    if (particle->m_model != nullptr) {
        particle->m_model->Release(1);
        particle->m_model = nullptr;
    }
}

class ParticleTraitsModel : public CParticleTraits {
    public:
    // ref: FUN_1412410e0
    void Construct(uint8_t* data, uint64_t count) const override {
        for (; count != 0; count--) {
            ParticleModelConstruct(reinterpret_cast<CParticle2_Model*>(data));
            data += sizeof(CParticle2_Model);
        }
    }
    // ref: FUN_141241120
    void Destroy(uint8_t* data, uint64_t count) const override {
        for (; count != 0; count--) {
            ParticleModelDestroy(reinterpret_cast<CParticle2_Model*>(data));
            data += sizeof(CParticle2_Model);
        }
    }
    // ref: FUN_141241160
    void Move(uint8_t* to, uint8_t* from, uint64_t count) const override {
        for (; count != 0; count--) {
            // FUN_14125cc60: the copy takes its own reference.
            auto source = reinterpret_cast<CParticle2_Model*>(from);
            std::memcpy(to, from, 0x3c);
            auto copy = reinterpret_cast<CParticle2_Model*>(to);
            copy->m_model = source->m_model;
            if (copy->m_model != nullptr) {
                copy->m_model->AddRef();
            }
            ParticleModelDestroy(source);
            to += sizeof(CParticle2_Model);
            from += sizeof(CParticle2_Model);
        }
    }
};

class ParticleTraitsQuad : public CParticleTraits {
    public:
    // ref: FUN_1412411b0
    void Construct(uint8_t* data, uint64_t count) const override {
        for (; count != 0; count--) {
            std::memset(data, 0, sizeof(CParticle2_Quad));  // FUN_14125d260
            data += sizeof(CParticle2_Quad);
        }
    }
    // ref: FUN_1412411f0
    void Destroy(uint8_t* data, uint64_t count) const override {
        (void)data;
        (void)count;
    }
    // ref: FUN_141241230
    void Move(uint8_t* to, uint8_t* from, uint64_t count) const override {
        for (; count != 0; count--) {
            std::memcpy(to, from, sizeof(CParticle2_Quad));  // FUN_14125d290
            to += sizeof(CParticle2_Quad);
            from += sizeof(CParticle2_Quad);
        }
    }
};

class ParticleTraitsProjected : public CParticleTraits {
    public:
    // ref: FUN_141241280
    void Construct(uint8_t* data, uint64_t count) const override {
        for (; count != 0; count--) {
            // FUN_14125d2e0
            auto particle = reinterpret_cast<CParticle2_Projected*>(data);
            std::memset(particle, 0, sizeof(CParticle2_Quad));
            particle->m_rotation[0] = 0.0f;
            particle->m_rotation[1] = 0.0f;
            particle->m_rotation[2] = 0.0f;
            particle->m_rotation[3] = 1.0f;
            data += sizeof(CParticle2_Projected);
        }
    }
    // ref: FUN_1412412c0
    void Destroy(uint8_t* data, uint64_t count) const override {
        (void)data;
        (void)count;
    }
    // ref: FUN_141241300
    void Move(uint8_t* to, uint8_t* from, uint64_t count) const override {
        for (; count != 0; count--) {
            std::memcpy(to, from, sizeof(CParticle2_Projected));
            to += sizeof(CParticle2_Projected);
            from += sizeof(CParticle2_Projected);
        }
    }
};

const ParticleTraitsPlain s_traitsPlain;
const ParticleTraitsModel s_traitsModel;
const ParticleTraitsQuad s_traitsQuad;
const ParticleTraitsProjected s_traitsProjected;

// The element kinds' traits and sizes (DAT_1430a1f20).
struct ParticleKind {
    const CParticleTraits* m_traits;
    uint32_t m_stride;
};
const ParticleKind s_kinds[5] = {
    {&s_traitsPlain, 0x20},
    {&s_traitsModel, 0x48},
    {&s_traitsQuad, 0x40},
    {&s_traitsQuad, 0x40},
    {&s_traitsProjected, 0x50},
};

// The pool of the fast path's keys (DAT_1430a1cc0).
TObjectAllocHeap* s_keyPool;
// What the models particles carry are updated under (DAT_1430a1f70).
OSMUTEX s_modelLock;  // DAT_1430a1f70
// How many have started the particle system (DAT_1430a1f18).
int32_t s_initCount;

// The static initializers: the heap list's room for the seven pools (FUN_1401e4f00) and the
// model lock (FUN_1401e4f30).
struct ParticleStatics {
    ParticleStatics() {
        g_particleHeaps.m_memFlags = 0x15;
        // FUN_14124ff80
        uint64_t field = g_particleHeaps.m_heaps.m_capacity;
        if ((field & 0x7fffffffffffffffull) < 7) {
            uint64_t capacity = (field >> 1 & 0x3fffffffffffffffull) + (field & 0x7fffffffffffffffull);
            if (capacity < 7) {
                capacity = 7;
            }
            g_particleHeaps.m_heaps.m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
            auto data = static_cast<M2ParticleHeap*>(SMemAlloc(capacity * sizeof(M2ParticleHeap), 0xb,
                s_heapPairName, SStrHashFnv1a(s_heapPairName), -6, 0));
            g_particleHeaps.m_heaps.m_capacity &= 0x7fffffffffffffffull;
            g_particleHeaps.m_heaps.m_data = data;
        }
        OsMutexInit(&s_modelLock);  // FUN_140262450
    }
};

// The storage a list gives back: to the heap it came from, or to Storm (FUN_14125cf30).
// ref: FUN_14125cf30
void ParticleStorageFree(M2ParticleHeaps* heaps, uint8_t* storage, uint32_t size) {
    if (PARTICLE_HEAP_THRESHOLD < size) {
        for (uint64_t i = 0; i < heaps->m_heaps.m_size; i++) {
            if (size <= heaps->m_heaps.m_data[i].m_size) {
                heaps->m_heaps.m_data[i].m_heap->Free(storage);
                return;
            }
        }
    }
    SMemFree(storage, s_allocFile, 0x46, 0);
}

// The vector's own storage freed when it owns it.
void IndicesFree(M2SceneVector<uint32_t>* indices) {
    if (static_cast<int64_t>(indices->m_capacity) >= 0) {
        SMemFree(indices->m_data, s_indexName, -6, 0);
        indices->m_data = nullptr;
    }
}

// The generator's life for a particle, never below a millisecond.
float ParticleLife(const CParticleGenerator* generator, const CParticle2* particle) {
    float life = (static_cast<float>(particle->m_lifeVariation) * 3.0518509e-05f * generator->m_lifespanVariation
                     + generator->m_lifespan)
        * generator->m_lifespanScale;  // DAT_1427228e4
    if (life <= 0.001f) {  // DAT_142618f58
        life = 0.001f;
    }
    return life;
}

// x less its floor, as the update truncates and steps down for negatives.
float Fraction(float x) {
    int32_t truncated = static_cast<int32_t>(x);
    float floor = x;
    if (truncated != INT32_MIN && static_cast<float>(truncated) != x) {
        floor = static_cast<float>(truncated - (x < 0.0f ? 1 : 0));
    }
    return x - floor;
}

float RowLength(const float* m) {
    return std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
}

}  // namespace

M2ParticleHeaps g_particleHeaps;
CShaderEffect* g_particleEffects[6];
GxBuf* g_particleIndices;
uint8_t g_particlePrebuild = 1;

namespace {
ParticleStatics s_particleStatics;
}  // namespace

// --- the list --------------------------------------------------------------------------------

// ref: FUN_14125ccb0
CParticleList::CParticleList() {
    this->m_traits = nullptr;
    this->m_live = {};
    this->m_free = {};
    this->m_storage = nullptr;
    this->m_stride = 0;
    this->m_count = 0;
    this->m_capacity = 0;
}

// The destructor (FUN_14125ccf0), and its deleting form (FUN_14125cdb0).
// ref: FUN_14125ccf0
// ref: FUN_14125cdb0
CParticleList::~CParticleList() {
    if (this->m_storage != nullptr && this->m_heaps != nullptr) {
        if (this->m_traits != nullptr) {
            this->m_traits->Destroy(this->m_storage, this->m_count);
        }
        ParticleStorageFree(this->m_heaps, this->m_storage, static_cast<uint32_t>(this->m_capacity) * this->m_stride);
        this->m_storage = nullptr;
    }
    this->m_live.m_size = 0;
    this->m_free.m_size = 0;
    this->m_traits = nullptr;
    this->m_stride = 0;
    this->m_count = 0;
    this->m_capacity = 0;
    IndicesFree(&this->m_free);
    IndicesFree(&this->m_live);
}

// ref: FUN_14125d020
void CParticleList::SetTraits(const CParticleTraits* traits, uint32_t stride, M2ParticleHeaps* heaps) {
    this->m_traits = traits;
    this->m_stride = stride;
    this->m_heaps = heaps;
}

// ref: FUN_14125d030
void CParticleList::Reserve(uint32_t count) {
    if (count <= this->m_count) {
        return;
    }
    if (this->m_capacity < count) {
        uint32_t bytes = (count * this->m_stride + 0x3ff) & 0xfffffc00;
        uint8_t* storage = nullptr;
        bool pooled = false;
        if (PARTICLE_HEAP_THRESHOLD < bytes) {
            for (uint64_t i = 0; i < this->m_heaps->m_heaps.m_size; i++) {
                if (bytes <= this->m_heaps->m_heaps.m_data[i].m_size) {
                    storage = static_cast<uint8_t*>(this->m_heaps->m_heaps.m_data[i].m_heap->Alloc());
                    pooled = true;
                    break;
                }
            }
        }
        if (!pooled) {
            storage = static_cast<uint8_t*>(
                SMemAlloc(bytes, this->m_heaps->m_memFlags, s_allocFile, SStrHashFnv1a(s_allocFile), 0x33, 0));
        }
        this->m_traits->Move(storage, this->m_storage, this->m_count);
        if (this->m_storage != nullptr) {
            ParticleStorageFree(this->m_heaps, this->m_storage, static_cast<uint32_t>(this->m_capacity) * this->m_stride);
        }
        this->m_storage = storage;
        this->m_capacity = bytes / this->m_stride;
    }
    this->m_traits->Construct(this->m_storage + this->m_count * this->m_stride, count - this->m_count);
    M2IndicesReserve(&this->m_free, count);
    M2IndicesReserve(&this->m_live, count);
    for (uint32_t i = this->m_count; i < count; i++) {
        M2IndicesPush(&this->m_free, i);
    }
    this->m_count = count;
}

// ref: FUN_14125ce90
uint8_t* CParticleList::Add() {
    uint32_t index = this->m_free.m_data[this->m_free.m_size - 1];
    this->m_free.m_size--;
    M2IndicesPush(&this->m_live, index);
    return this->m_storage + index * this->m_stride;
}

// ref: FUN_14125cfd0
void CParticleList::Remove(uint32_t index) {
    M2IndicesPush(&this->m_free, this->m_live.m_data[index]);  // FUN_1403095b0
    this->m_live.m_data[index] = this->m_live.m_data[this->m_live.m_size - 1];
    this->m_live.m_size--;
}

// --- the emitter -----------------------------------------------------------------------------

// ref: FUN_1412497e0
CParticleEmitter2* ParticleEmitterConstruct(void* memory, CM2Scene* scene) {
    auto emitter = static_cast<CParticleEmitter2*>(memory);
    emitter->m_00 = 1;
    emitter->m_emission = 0.0f;
    emitter->m_cellShift = 0;
    emitter->m_cellWidth = 1.0f;
    emitter->m_cellHeight = 1.0f;
    new (&emitter->m_list) CParticleList();
    emitter->m_vertexFormat = -1;
    emitter->m_pathSegments = nullptr;
    emitter->m_pathCount = 0;
    emitter->m_pathMode = 0;
    emitter->m_viewFade = 1.0f;
    emitter->m_drawnCount = 0;
    RndSeedInit(&emitter->m_random, 0);
    emitter->m_childCount = 4;
    emitter->m_blend = 0;
    emitter->m_renderFlags = 7;
    emitter->m_type = -1;
    emitter->m_tailLength = 1.0f;
    emitter->m_baseSpin = 0.0f;
    emitter->m_baseSpinVary = 0.0f;
    emitter->m_spin = 0.0f;
    emitter->m_spinVary = 0.0f;
    emitter->m_scale = 1.0f;
    emitter->m_model = nullptr;
    emitter->m_scene = scene;
    emitter->m_recursionModel = nullptr;
    emitter->m_verticesPerParticle = 0;
    emitter->m_indicesPerParticle = 0;
    emitter->m_colorTrack = nullptr;
    emitter->m_alphaTrack = nullptr;
    emitter->m_scaleTrack = nullptr;
    emitter->m_scaleVary = {0.0f, 0.0f};
    emitter->m_headCellTrack = nullptr;
    emitter->m_tailCellTrack = nullptr;
    std::memset(emitter->m_colors, 0, sizeof(emitter->m_colors));
    emitter->m_midpoint = 0.0f;
    emitter->m_fastKeys[0] = nullptr;
    emitter->m_fastKeys[1] = nullptr;
    std::memset(emitter->m_uvScale, 0, sizeof(emitter->m_uvScale));
    std::memset(emitter->m_uvSpeed, 0, sizeof(emitter->m_uvSpeed));
    std::memset(emitter->m_uvSpeedVary, 0, sizeof(emitter->m_uvSpeedVary));
    emitter->m_rows = 1;
    emitter->m_columns = 1;
    emitter->m_startCell = 0;
    emitter->m_cellMask = 0;
    emitter->m_textures[0] = nullptr;
    emitter->m_textures[1] = nullptr;
    emitter->m_textures[2] = nullptr;
    emitter->m_material = 0;
    emitter->m_alpha = 1.0f;
    emitter->m_flags = 6;
    emitter->m_twinkleSpeed = 10.0f;
    emitter->m_twinklePercent = 1.0f;
    emitter->m_twinkleScale[0] = 1.0f;
    emitter->m_twinkleScale[1] = 0.0f;
    emitter->m_burstMultiplier = 1.0f;
    std::memset(emitter->m_tumble, 0, sizeof(emitter->m_tumble));
    emitter->m_drag = 0.0f;
    emitter->m_wind = {0.0f, 0.0f, 0.0f};
    emitter->m_windTime = 0.0f;
    emitter->m_followBase = 0.0f;
    emitter->m_followSlope = 0.0f;
    emitter->m_pad22c = 0;
    MatrixIdentity(emitter->m_parentMatrix);
    emitter->m_origin = {0.0f, 0.0f, 0.0f};
    emitter->m_previousPosition = {0.0f, 0.0f, 0.0f};
    emitter->m_followTime = 0.0f;
    emitter->m_direction = {0.0f, 0.0f, 0.0f};
    emitter->m_motion = {0.0f, 0.0f, 0.0f};
    emitter->m_stepMotion = {0.0f, 0.0f, 0.0f};
    emitter->m_cameraAxis = {0.0f, 0.0f, 0.0f};
    emitter->m_bounds[0] = 3.4028235e+38f;  // DAT_14261f708
    emitter->m_bounds[1] = 3.4028235e+38f;
    emitter->m_bounds[2] = 3.4028235e+38f;
    emitter->m_bounds[3] = -3.4028235e+38f;  // DAT_14261f70c
    emitter->m_bounds[4] = -3.4028235e+38f;
    emitter->m_bounds[5] = -3.4028235e+38f;
    emitter->m_2dc = 0;
    emitter->m_combiner = nullptr;
    emitter->m_extended = nullptr;
    emitter->m_prebuiltOffset = -1;
    emitter->m_prebuiltSize = -1;
    int32_t high = rand();  // FUN_142415c90
    uint16_t low = static_cast<uint16_t>(rand());
    RndSeedInit(&emitter->m_random, static_cast<uint32_t>(low) | static_cast<uint32_t>(high) << 16);
    emitter->m_childCount = 0;
    return emitter;
}

// ref: FUN_141249af0
void ParticleEmitterDelete(CParticleEmitter2* emitter) {
    for (uint32_t i = 0; i < 3; i++) {
        if (emitter->m_textures[i] != nullptr) {
            TextureFree(&emitter->m_textures[i]);
            emitter->m_textures[i] = nullptr;
        }
    }
    if (emitter->m_model != nullptr) {
        emitter->m_model->Release(1);
        emitter->m_model = nullptr;
    }
    if (emitter->m_recursionModel != nullptr) {
        emitter->m_recursionModel->Release(1);
        emitter->m_recursionModel = nullptr;
    }
    for (uint32_t i = 0; i < 2; i++) {
        if (emitter->m_fastKeys[i] != nullptr) {
            s_keyPool->Free(emitter->m_fastKeys[i]);
        }
    }
    emitter->m_list.~CParticleList();
}

void ParticleGeneratorDestroy(CParticleGenerator* generator) {
    generator->~CParticleGenerator();
}

// ref: FUN_14124bd50
bool ParticleEmitterHasParticles(const CParticleEmitter2* emitter) {
    if (emitter->m_list.m_live.m_size != 0) {
        return true;
    }
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        if (ParticleEmitterHasParticles(emitter->m_children[i])) {
            return true;
        }
    }
    return false;
}

// ref: FUN_14124b8e0
uint32_t ParticleEmitterModelCount(const CParticleEmitter2* emitter) {
    uint32_t count = 0;
    if (emitter->m_type == 1) {
        count = static_cast<uint32_t>(emitter->m_list.m_live.m_size);
    }
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        count += ParticleEmitterModelCount(emitter->m_children[i]);
    }
    return count;
}

// ref: FUN_14124b940
CM2Model* ParticleEmitterGetModel(const CParticleEmitter2* emitter, uint32_t* index) {
    if (emitter->m_type == 1) {
        if (*index < emitter->m_list.m_live.m_size) {
            return reinterpret_cast<CParticle2_Model*>(emitter->m_list.Get(*index))->m_model;
        }
        *index -= static_cast<uint32_t>(emitter->m_list.m_live.m_size);
    }
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        CM2Model* model = ParticleEmitterGetModel(emitter->m_children[i], index);
        if (model != nullptr) {
            return model;
        }
    }
    return nullptr;
}

// ref: FUN_14124efd0
void ParticleEmitterSetTexture(CParticleEmitter2* emitter, uint32_t slot, CTexture* texture) {
    if (texture != nullptr) {
        TextureAddRef(texture);
    }
    if (emitter->m_textures[slot] != nullptr) {
        TextureFree(&emitter->m_textures[slot]);
    }
    emitter->m_textures[slot] = texture;
    auto mix = [](const CTexture* texture, uint32_t multiplier) {
        uint32_t h = (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(texture)) >> 4) * multiplier;
        h = ((h ^ 0x3d0000) >> 16 ^ h) * 9;
        h = (h >> 4 ^ h) * 0x27d4eb2b;
        return h >> 15 ^ h;
    };
    if (emitter->m_type == 2 || (emitter->m_type == 4 && emitter->m_textures[1] != nullptr)) {
        emitter->m_material = 0;
        uint32_t hash = mix(emitter->m_textures[0], 0x3f9786af);
        emitter->m_material = hash;
        hash += mix(emitter->m_textures[1], 0x7c7fdae1);
        emitter->m_material = hash;
        hash += mix(emitter->m_textures[2], 0xd80a5bd7);
        emitter->m_material = hash;
        return;
    }
    emitter->m_material = mix(emitter->m_textures[0], 0x3f9786af);
}

// ref: FUN_141249ed0
void ParticleEmitterSetParentMatrix(CParticleEmitter2* emitter, const C44Matrix* matrix) {
    auto m = reinterpret_cast<const float*>(matrix);
    if ((emitter->m_flags & 0x200) == 0) {
        for (uint64_t i = 0; i < emitter->m_list.m_live.m_size; i++) {
            auto particle = reinterpret_cast<CParticle2*>(emitter->m_list.Get(static_cast<uint32_t>(i)));
            C3Vector p = particle->m_position;
            particle->m_position.x = p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12];
            particle->m_position.y = p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13];
            particle->m_position.z = p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14];
            C3Vector v = particle->m_velocity;
            particle->m_velocity.x = v.x * m[0] + v.y * m[4] + v.z * m[8];
            particle->m_velocity.y = v.x * m[1] + v.y * m[5] + v.z * m[9];
            particle->m_velocity.z = v.x * m[2] + v.y * m[6] + v.z * m[10];
        }
    }
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        ParticleEmitterSetParentMatrix(emitter->m_children[i], matrix);
    }
}

// The recursion model loaded: its first four emitters become this one's children, each
// spawned from this one's particles (FUN_14124cfc0).
// ref: FUN_14124cfc0
static void ParticleEmitterOnRecursionLoaded(CM2BaseModel* base, void* param) {
    auto model = static_cast<CM2Model*>(base);
    auto emitter = static_cast<CParticleEmitter2*>(param);
    if (model == nullptr || emitter == nullptr) {
        return;
    }
    if ((model->m_flags & 1) == 0) {
        model->WaitForLoad(0);
    }
    uint32_t count;
    if (model->m_shared->m_data->m_particles.Count() < 4) {
        if ((model->m_flags & 1) == 0) {
            model->WaitForLoad(0);
        }
        count = model->m_shared->m_data->m_particles.Count();
        if (count == 0) {
            return;
        }
    } else {
        count = 4;
    }
    for (uint32_t i = 0; i < count; i++) {
        if ((model->m_flags & 1) == 0) {
            model->WaitForLoad(0);
        }
        CParticleEmitter2* child = model->m_particles[i];
        emitter->m_children[emitter->m_childCount] = child;
        emitter->m_childCount++;
        child->m_flags |= 1;
    }
}

// ref: FUN_14124a010
void ParticleEmitterSetRecursionModel(CParticleEmitter2* emitter, CM2Scene* scene, int32_t fileDataId, const char* name) {
    if (scene == nullptr) {
        return;
    }
    CM2Model* model;
    if (name == nullptr) {
        model = scene->CreateModel(fileDataId, 0, 1, 0);
    } else {
        model = scene->CreateModel(name, 0, 1, 0);
    }
    emitter->m_recursionModel = model;
    if (model != nullptr) {
        model->SetLoadCallback(ParticleEmitterOnRecursionLoaded, emitter);
    }
}

// ref: FUN_14124e070
void ParticleEmitterSetRandomTiles(CParticleEmitter2* emitter, int32_t on) {
    if (emitter->m_cellMask != 0 && on != 0) {
        emitter->m_flags |= 0x100000;
        return;
    }
    emitter->m_flags &= ~0x100000u;
}

// ref: FUN_14124e0a0
void ParticleEmitterSetFollow(CParticleEmitter2* emitter, float speed1, float scale1, float speed2, float scale2) {
    if (2.3841858e-07f <= std::fabs(speed2 - speed1)) {  // DAT_14261a954
        float slope = (scale2 - scale1) / (speed2 - speed1);
        emitter->m_followSlope = slope;
        emitter->m_followBase = scale1 - slope * speed1;
        return;
    }
    emitter->m_followSlope = 0.0f;
    emitter->m_followBase = 0.0f;
}

// ref: FUN_14124e150
void ParticleEmitterSetBlend(CParticleEmitter2* emitter, const uint8_t* blend) {
    emitter->m_blend = blend[0];
    emitter->m_renderFlags = blend[1];
    int32_t type = emitter->m_type;
    bool multi = type == 2 || (type == 4 && emitter->m_textures[1] != nullptr);
    if (multi) {
        const uint8_t* combiner = reinterpret_cast<const uint8_t*>(emitter->m_combiner);
        if (combiner != nullptr && (combiner[0] != 0 || combiner[1] != 0)) {
            emitter->m_vertexFormat = (emitter->m_renderFlags & 0x20) == 0 ? -1 : 4;
            return;
        }
        emitter->m_vertexFormat = static_cast<int32_t>((emitter->m_renderFlags & 0x20 | 0x40) >> 5);
        return;
    }
    if (type == 3) {
        emitter->m_vertexFormat = 5;
        return;
    }
    emitter->m_vertexFormat = emitter->m_renderFlags & 1;
}

// ref: FUN_14124e200
void ParticleEmitterSetModelById(CParticleEmitter2* emitter, CM2Scene* scene, int32_t fileDataId) {
    if (scene != nullptr) {
        emitter->m_scene = scene;
        emitter->m_model = scene->CreateModel(fileDataId, 0, 1, 0);
    }
}

// ref: FUN_14124e240
void ParticleEmitterSetModelByName(CParticleEmitter2* emitter, CM2Scene* scene, const char* name) {
    if (scene != nullptr && name != nullptr && *name != '\0') {
        emitter->m_scene = scene;
        emitter->m_model = scene->CreateModel(name, 0, 1, 0);
    }
}

// ref: FUN_14124e290
void ParticleEmitterSetTextureTransform(CParticleEmitter2* emitter, uint32_t layer, float rotation, const float* scroll,
    const float* scale) {
    emitter->m_uvScale[layer] = rotation;
    emitter->m_uvSpeed[layer * 2] = scroll[0];
    emitter->m_uvSpeed[layer * 2 + 1] = scroll[1];
    emitter->m_uvSpeedVary[layer * 2] = scale[0];
    emitter->m_uvSpeedVary[layer * 2 + 1] = scale[1];
}

// ref: FUN_14124e2c0
void ParticleSetPrebuild() {
    g_particlePrebuild = 1;
}

// ref: FUN_14124b9e0
void ParticleEmitterGetColors(CParticleEmitter2* emitter, uint8_t* start, uint8_t* middle, uint8_t* end) {
    start[1] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[0][1]));
    start[2] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[0][0]));
    start[0] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[0][2]));
    middle[2] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[1][0]));
    middle[0] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[1][2]));
    middle[1] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[1][1]));
    end[1] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[2][1]));
    end[0] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[2][2]));
    end[2] = static_cast<uint8_t>(std::lrintf(emitter->m_colors[2][0]));
}

// ref: FUN_14124e2d0
void ParticleEmitterSetColors(CParticleEmitter2* emitter, const uint8_t* start, const uint8_t* middle, const uint8_t* end) {
    emitter->m_colors[0][0] = static_cast<float>(start[2]);
    emitter->m_colors[0][1] = static_cast<float>(start[1]);
    emitter->m_colors[0][2] = static_cast<float>(start[0]);
    emitter->m_colors[1][0] = static_cast<float>(middle[2]);
    emitter->m_colors[1][1] = static_cast<float>(middle[1]);
    emitter->m_colors[1][2] = static_cast<float>(middle[0]);
    emitter->m_colors[2][0] = static_cast<float>(end[2]);
    emitter->m_colors[2][1] = static_cast<float>(end[1]);
    emitter->m_flags |= 0x10;
    emitter->m_colors[2][2] = static_cast<float>(end[0]);
    if ((emitter->m_flags & 0x1000000) != 0) {
        CParticleKeySimple* first = emitter->m_fastKeys[0];
        CParticleKeySimple* second = emitter->m_fastKeys[1];
        first->m_color[0] = static_cast<int32_t>(start[2]) << 16;
        first->m_color[1] = static_cast<int32_t>(start[1]) << 16;
        first->m_color[2] = static_cast<int32_t>(start[0]) << 16;
        first->m_colorDelta[0] = static_cast<int32_t>(middle[2]) - static_cast<int32_t>(start[2]);
        first->m_colorDelta[1] = static_cast<int32_t>(middle[1]) - static_cast<int32_t>(start[1]);
        first->m_colorDelta[2] = static_cast<int32_t>(middle[0]) - static_cast<int32_t>(start[0]);
        second->m_color[0] = static_cast<int32_t>(middle[2]) << 16;
        second->m_color[1] = static_cast<int32_t>(middle[1]) << 16;
        second->m_color[2] = static_cast<int32_t>(middle[0]) << 16;
        second->m_colorDelta[0] = static_cast<int32_t>(end[2]) - static_cast<int32_t>(middle[2]);
        second->m_colorDelta[1] = static_cast<int32_t>(end[1]) - static_cast<int32_t>(middle[1]);
        second->m_colorDelta[2] = static_cast<int32_t>(end[0]) - static_cast<int32_t>(middle[0]);
    }
}

// ref: FUN_14124e480
void ParticleEmitterSetFlags(CParticleEmitter2* emitter, uint32_t a2, uint32_t a3, float tailLength, uint32_t a5) {
    uint32_t flags = a2 == 0 ? emitter->m_flags & ~4u : emitter->m_flags | 4;
    flags = a3 == 0 ? flags & ~8u : flags | 8;
    flags = a5 == 0 ? flags & ~0x20000u : flags | 0x20000;
    emitter->m_flags = flags;
    emitter->m_tailLength = tailLength;
    emitter->m_verticesPerParticle = 0;
    emitter->m_indicesPerParticle = 0;
    uint32_t vertices = 0;
    uint32_t indices = 0;
    if ((flags & 4) != 0) {
        vertices = 4;
        indices = 6;
        emitter->m_verticesPerParticle = 4;
        emitter->m_indicesPerParticle = 6;
    }
    if ((flags & 8) != 0) {
        emitter->m_verticesPerParticle = vertices + 4;
        emitter->m_indicesPerParticle = indices + 6;
    }
}

// ref: FUN_14124e510
void ParticleEmitterSetKind(CParticleEmitter2* emitter, int32_t kind) {
    emitter->m_type = kind;
    emitter->m_list.SetTraits(s_kinds[kind].m_traits, s_kinds[kind].m_stride, &g_particleHeaps);
}

// ref: FUN_14124e540
void ParticleEmitterSetRandomStartCell(CParticleEmitter2* emitter, int32_t on) {
    if (on != 0) {
        uint16_t mask = emitter->m_cellMask;
        uint32_t next = RndSeedNext(&emitter->m_random);
        emitter->m_startCell = static_cast<uint16_t>((static_cast<uint64_t>(next) * (static_cast<uint64_t>(mask) + 1)) >> 32);
        return;
    }
    emitter->m_startCell = 0;
}

// ref: FUN_14124f220
void ParticleEmitterSetTileGrid(CParticleEmitter2* emitter, uint32_t rows, uint32_t columns) {
    emitter->m_rows = rows;
    emitter->m_columns = columns;
    emitter->m_cellShift = -1;
    emitter->m_cellMask = static_cast<uint16_t>(static_cast<int16_t>(columns) * static_cast<int16_t>(rows) - 1);
    if (columns != 0) {
        int32_t shift = -1;
        for (uint32_t c = columns; c != 0; c >>= 1) {
            shift++;
        }
        emitter->m_cellShift = shift;
    }
    emitter->m_cellWidth = 1.0f / static_cast<float>(emitter->m_columns);
    emitter->m_cellHeight = 1.0f / static_cast<float>(rows);
}

// ref: FUN_14124f2a0
void ParticleEmitterSetTwinkleScale(CParticleEmitter2* emitter, const float* scale) {
    emitter->m_twinkleScale[0] = scale[0];
    emitter->m_twinkleScale[1] = scale[1] - scale[0];
}

// --- the fast path ---------------------------------------------------------------------------

namespace {

// A key from the pool, cleared (FUN_141248510).
// ref: FUN_141248510
CParticleKeySimple* KeyAlloc(TObjectAllocHeap* pool) {
    auto key = static_cast<CParticleKeySimple*>(pool->Alloc());
    std::memset(key, 0, 0x40);
    return key;
}

int32_t Round(float value) {
    return static_cast<int32_t>(std::lrintf(value));
}

const float* TrackFloats(const M2PartTrack<C3Vector>* track) {
    return reinterpret_cast<const float*>(track->m_values.Data());
}

}  // namespace

// The colour keys from the overrides or the colour track (FUN_14124e740).
// ref: FUN_14124e740
static void ParticleEmitterFastColors(CParticleEmitter2* emitter) {
    CParticleKeySimple* first = emitter->m_fastKeys[0];
    CParticleKeySimple* second = emitter->m_fastKeys[1];
    if ((emitter->m_flags & 0x10) != 0) {
        for (uint32_t c = 0; c < 3; c++) {
            first->m_color[c] = (Round(emitter->m_colors[0][c]) & 0xff) << 16;
            second->m_color[c] = (Round(emitter->m_colors[1][c]) & 0xff) << 16;
            first->m_colorDelta[c] = (second->m_color[c] - first->m_color[c]) >> 16 & 0xff;
            second->m_colorDelta[c] = static_cast<int32_t>(Round(emitter->m_colors[2][c]) & 0xff)
                - static_cast<int32_t>(reinterpret_cast<const uint8_t*>(&second->m_color[c])[2]);
        }
        return;
    }
    const float* values = TrackFloats(emitter->m_colorTrack);
    first->m_color[0] = (Round(values[0]) & 0xff) << 16;
    first->m_color[1] = (Round(values[1]) & 0xff) << 16;
    first->m_color[2] = (Round(values[2]) & 0xff) << 16;
    uint16_t count = emitter->m_colorTrack->m_values.m_count;
    if (count == 1) {
        first->m_colorDelta[0] = 0;
        first->m_colorDelta[1] = 0;
        first->m_colorDelta[2] = 0;
    } else if (count == 2) {
        for (uint32_t c = 0; c < 3; c++) {
            first->m_colorDelta[c] = Round((values[3 + c] - values[c]) * emitter->m_midpoint);
        }
    } else if (count == 3) {
        for (uint32_t c = 0; c < 3; c++) {
            first->m_colorDelta[c] = Round(values[3 + c] - values[c]);
        }
    }
    for (uint32_t c = 0; c < 3; c++) {
        second->m_color[c] = static_cast<int16_t>(first->m_colorDelta[c]) * 0x10000 + first->m_color[c];
    }
    if (count == 1) {
        second->m_colorDelta[0] = 0;
        second->m_colorDelta[1] = 0;
        second->m_colorDelta[2] = 0;
    } else if (count == 2) {
        for (uint32_t c = 0; c < 3; c++) {
            second->m_colorDelta[c] = Round((values[3 + c] - values[c]) * (1.0f - emitter->m_midpoint));
        }
    } else if (count == 3) {
        for (uint32_t c = 0; c < 3; c++) {
            second->m_colorDelta[c] = Round(values[6 + c] - values[3 + c]);
        }
    }
}

// The alpha keys (FUN_14124e5a0).
// ref: FUN_14124e5a0
static void ParticleEmitterFastAlpha(CParticleEmitter2* emitter) {
    CParticleKeySimple* first = emitter->m_fastKeys[0];
    CParticleKeySimple* second = emitter->m_fastKeys[1];
    const float scale = 0.0077822199f;  // DAT_142724c2c, 255 over 32767
    const int16_t* values = reinterpret_cast<const int16_t*>(emitter->m_alphaTrack->m_values.Data());
    first->m_alpha = (Round(static_cast<float>(values[0]) * scale) & 0xff) << 16;
    uint16_t count = emitter->m_alphaTrack->m_values.m_count;
    if (count == 1) {
        first->m_alphaDelta = 0;
    } else if (count == 2) {
        first->m_alphaDelta = Round(static_cast<float>(static_cast<int16_t>(values[1] - values[0])) * scale * emitter->m_midpoint);
    } else if (count == 3) {
        first->m_alphaDelta = Round(static_cast<float>(static_cast<int16_t>(values[1] - values[0])) * scale);
    }
    second->m_alpha = first->m_alpha + static_cast<int16_t>(first->m_alphaDelta) * 0x10000;
    if (count == 1) {
        second->m_alphaDelta = 0;
    } else if (count == 2) {
        second->m_alphaDelta =
            Round((1.0f - emitter->m_midpoint) * static_cast<float>(static_cast<int16_t>(values[1] - values[0])) * scale);
    } else if (count == 3) {
        second->m_alphaDelta = Round(static_cast<float>(static_cast<int16_t>(values[2] - values[1])) * scale);
    }
}

// A cell track's keys into the slot at `cell` and `delta` (FUN_14124ec50, FUN_14124ee10).
static void ParticleEmitterFastCells(CParticleEmitter2* emitter, const M2PartTrack<uint16_t>* track, int32_t* first,
    int32_t* firstDelta, int32_t* second, int32_t* secondDelta) {
    if (track->m_values.m_count == 0) {
        *first = 0;
        *firstDelta = 0;
        *second = 0;
        *secondDelta = 0;
        return;
    }
    const uint16_t* values = track->m_values.Data();
    *first = static_cast<int32_t>(static_cast<int16_t>(values[0])) << 16;
    uint16_t count = track->m_values.m_count;
    if (count == 1) {
        *firstDelta = 0;
    } else if (count == 2) {
        *firstDelta = Round(static_cast<float>(static_cast<int32_t>(values[1]) - static_cast<int32_t>(values[0])) * emitter->m_midpoint);
    } else if (count == 3 || count == 4) {
        *firstDelta = static_cast<int32_t>(values[1]) - static_cast<int32_t>(values[0]);
    }
    *second = static_cast<int16_t>(*firstDelta) * 0x10000 + *first;
    if (count == 1) {
        *secondDelta = 0;
    } else if (count == 2) {
        *secondDelta = Round((1.0f - emitter->m_midpoint)
            * static_cast<float>(static_cast<int32_t>(values[1]) - static_cast<int32_t>(values[0])));
    } else if (count == 3) {
        *secondDelta = static_cast<int32_t>(values[2]) - static_cast<int32_t>(values[1]);
    } else if (count == 4) {
        *secondDelta = static_cast<int32_t>(values[3]) - static_cast<int32_t>(values[2]);
    }
}

// ref: FUN_14124ac30
void ParticleEmitterPrepareFastPath(CParticleEmitter2* emitter) {
    const M2PartTrack<C3Vector>* colors = emitter->m_colorTrack;
    if (2 < static_cast<uint16_t>(colors->m_times.m_count - 1)) {
        return;
    }
    const M2PartTrack<uint16_t>* alphas = emitter->m_alphaTrack;
    if (2 < static_cast<uint16_t>(alphas->m_times.m_count - 1)) {
        return;
    }
    const M2PartTrack<C2Vector>* scales = emitter->m_scaleTrack;
    if (2 < static_cast<uint16_t>(scales->m_times.m_count - 1)) {
        return;
    }
    const M2PartTrack<uint16_t>* heads = emitter->m_headCellTrack;
    uint16_t headKeys = heads->m_times.m_count;
    if (4 < headKeys) {
        return;
    }
    if (headKeys == 4 && heads->m_times.Data()[1] != heads->m_times.Data()[2]) {
        return;
    }
    const M2PartTrack<uint16_t>* tails = emitter->m_tailCellTrack;
    uint16_t tailKeys = tails->m_times.m_count;
    if (4 < tailKeys) {
        return;
    }
    if (tailKeys == 4 && tails->m_times.Data()[1] != tails->m_times.Data()[2]) {
        return;
    }
    if (emitter->m_generator->m_lifespanVariation != 0.0f) {
        return;
    }
    if (headKeys == 0 && (emitter->m_flags & 0x100000) != 0) {
        return;
    }
    if (emitter->m_scaleVary.x != 0.0f || emitter->m_scaleVary.y != 0.0f) {
        return;
    }
    const float* scaleValues = reinterpret_cast<const float*>(scales->m_values.Data());
    for (uint32_t i = 0; i < scales->m_values.m_count; i++) {
        if (2.3841858e-07f <= std::fabs(scaleValues[i * 2] - scaleValues[i * 2 + 1])) {
            return;
        }
    }
    // The middle key's time, which every three-key track must share.
    int16_t middle = 0;
    auto time1 = [](const uint16_t* times) {
        return static_cast<int16_t>(times[1]);
    };
    bool check = true;
    if (colors->m_times.m_count == 3) {
        middle = time1(colors->m_times.Data());
        if (alphas->m_times.m_count == 3 && time1(alphas->m_times.Data()) != middle) {
            return;
        }
    } else if (alphas->m_times.m_count == 3) {
        middle = time1(alphas->m_times.Data());
    } else if (scales->m_times.m_count == 3) {
        middle = time1(scales->m_times.Data());
        if (2 < headKeys && time1(heads->m_times.Data()) != middle) {
            return;
        }
        if (2 < tailKeys && time1(tails->m_times.Data()) != middle) {
            return;
        }
        check = false;
    } else if (headKeys < 3) {
        if (2 < tailKeys) {
            middle = time1(tails->m_times.Data());
        }
        check = false;
    } else {
        middle = time1(heads->m_times.Data());
        if (2 < tailKeys && time1(tails->m_times.Data()) != middle) {
            return;
        }
        check = false;
    }
    if (check) {
        if (scales->m_times.m_count == 3 && time1(scales->m_times.Data()) != middle) {
            return;
        }
        if (2 < headKeys && time1(heads->m_times.Data()) != middle) {
            return;
        }
        if (2 < tailKeys && time1(tails->m_times.Data()) != middle) {
            return;
        }
    }
    emitter->m_fastKeys[0] = KeyAlloc(s_keyPool);
    emitter->m_fastKeys[1] = KeyAlloc(s_keyPool);
    emitter->m_flags |= 0x1000000;
    emitter->m_midpoint = static_cast<float>(middle) * 3.0518509e-05f;
    ParticleEmitterFastColors(emitter);
    ParticleEmitterFastAlpha(emitter);
    CParticleKeySimple* first = emitter->m_fastKeys[0];
    CParticleKeySimple* second = emitter->m_fastKeys[1];
    first->m_scale = scaleValues[0];
    uint16_t scaleCount = scales->m_values.m_count;
    if (scaleCount == 1) {
        first->m_scaleDelta = 0.0f;
    } else if (scaleCount == 2) {
        first->m_scaleDelta = (scaleValues[2] - scaleValues[0]) * emitter->m_midpoint;
    } else if (scaleCount == 3) {
        first->m_scaleDelta = scaleValues[2] - scaleValues[0];
    }
    second->m_scale = first->m_scaleDelta + first->m_scale;
    if (scaleCount == 1) {
        second->m_scaleDelta = 0.0f;
    } else if (scaleCount == 2) {
        second->m_scaleDelta = (scaleValues[2] - scaleValues[0]) * (1.0f - emitter->m_midpoint);
    } else if (scaleCount == 3) {
        second->m_scaleDelta = scaleValues[4] - scaleValues[2];
    }
    // ref: FUN_14124ec50
    ParticleEmitterFastCells(emitter, heads, &first->m_headCell, &first->m_headCellDelta, &second->m_headCell,
        &second->m_headCellDelta);
    // ref: FUN_14124ee10
    ParticleEmitterFastCells(emitter, tails, &first->m_tailCell, &first->m_tailCellDelta, &second->m_tailCell,
        &second->m_tailCellDelta);
}

// --- the update ------------------------------------------------------------------------------

namespace {

// What one update applies to every particle (FUN_141249c00 builds it): the wind's push, the
// gravity's velocity and position steps, and the drag.
struct ParticleForces {
    C3Vector m_wind;            // 0x00
    C3Vector m_gravity;         // 0x0c
    C3Vector m_gravityStep;     // 0x18
    float m_drag;               // 0x24
};

using WindCallback = void (*)(const float* position, float* direction, float* speed, uint8_t gusts);
using GroundCallback = bool (*)(const float* position, float* height, void* param);

// ref: FUN_141249c00
void ParticleEmitterForces(CParticleEmitter2* emitter, float delta, ParticleForces* forces) {
    const float half = 0.5f;  // DAT_14261a988
    if ((emitter->m_flags & 0xc0000000) != 0) {
        if ((emitter->m_flags >> 30 & 1) == 0) {
            forces->m_wind.x = delta * emitter->m_wind.x * half;
            forces->m_wind.y = emitter->m_wind.y * delta * half;
            forces->m_wind.z = emitter->m_wind.z * delta * half;
        } else {
            float direction[3] = {0.0f, 0.0f, 0.0f};
            float speed = 0.0f;
            auto callback = reinterpret_cast<WindCallback>(emitter->m_scene->m_windCallback);
            if (callback != nullptr) {
                callback(&emitter->m_parentMatrix[12], direction, &speed, emitter->m_2dc);
            }
            if ((emitter->m_flags & 0x200) != 0) {
                float inverse[16] = {};
                MatrixInverseOrIdentity(emitter->m_parentMatrix, inverse);
                float x = direction[0];
                float y = direction[1];
                float z = direction[2];
                direction[0] = inverse[1] * y + inverse[0] * x + inverse[2] * z + inverse[3];
                direction[1] = inverse[5] * y + inverse[4] * x + inverse[6] * z + inverse[7];
                direction[2] = inverse[9] * y + inverse[8] * x + inverse[10] * z + inverse[11];
            }
            forces->m_wind.x = direction[0] * speed * delta * half;
            forces->m_wind.y = direction[1] * speed * delta * half;
            forces->m_wind.z = direction[2] * speed * delta * half;
        }
    }
    CParticleGenerator* generator = emitter->m_generator;
    float y = delta * generator->m_gravity.y;
    float z = delta * generator->m_gravity.z;
    forces->m_gravity.x = delta * generator->m_gravity.x;
    forces->m_gravity.y = y;
    forces->m_gravity.z = z;
    forces->m_gravityStep.x = forces->m_gravity.x * half * delta;
    forces->m_gravityStep.y = y * half * delta;
    forces->m_gravityStep.z = z * half * delta;
    if (emitter->m_drag == 0.0f) {
        forces->m_drag = 0.0f;
    } else {
        float drag = emitter->m_drag * delta;
        forces->m_drag = 1.0f <= drag ? 1.0f : drag;
    }
}

// A velocity component this small is zero.
void Settle(float* value) {
    if (*value != 0.0f && std::fabs(*value) < 9.9999999e-09f) {  // DAT_142724c28
        *value = 0.0f;
    }
}

// A particle moved one step; false when it left the emitter's half space (FUN_14124cd10).
// ref: FUN_14124cd10
bool ParticleIntegrate(CParticleEmitter2* emitter, CParticle2* particle, float delta, const ParticleForces* forces) {
    if ((emitter->m_flags & 0xc0000000) != 0) {
        particle->m_velocity.x += forces->m_wind.x;
        particle->m_velocity.y += forces->m_wind.y;
        particle->m_velocity.z += forces->m_wind.z;
        Settle(&particle->m_velocity.x);
        Settle(&particle->m_velocity.y);
        Settle(&particle->m_velocity.z);
    }
    if ((emitter->m_flags & 0x80000) != 0 && delta + delta < particle->m_age) {
        particle->m_position.x += emitter->m_stepMotion.x;
        particle->m_position.y += emitter->m_stepMotion.y;
        particle->m_position.z += emitter->m_stepMotion.z;
    }
    float dy = delta * particle->m_velocity.y;
    float dx = particle->m_velocity.x * delta;
    float dz = delta * particle->m_velocity.z;
    particle->m_velocity.x += forces->m_gravity.x;
    particle->m_velocity.y += forces->m_gravity.y;
    particle->m_velocity.z += forces->m_gravity.z;
    particle->m_position.x = dx + forces->m_gravityStep.x + particle->m_position.x;
    particle->m_position.y = dy + forces->m_gravityStep.y + particle->m_position.y;
    particle->m_position.z = dz + forces->m_gravityStep.z + particle->m_position.z;
    float drag = forces->m_drag;
    particle->m_velocity.x -= particle->m_velocity.x * drag;
    particle->m_velocity.y -= particle->m_velocity.y * drag;
    particle->m_velocity.z -= particle->m_velocity.z * drag;
    Settle(&particle->m_velocity.x);
    Settle(&particle->m_velocity.y);
    Settle(&particle->m_velocity.z);
    uint32_t flags = emitter->m_flags;
    if ((flags >> 12 & 1) != 0) {
        if ((flags >> 9 & 1) == 0) {
            return (particle->m_position.y - emitter->m_parentMatrix[13]) * dy
                + (particle->m_position.x - emitter->m_parentMatrix[12]) * dx
                + (particle->m_position.z - emitter->m_parentMatrix[14]) * dz
                <= 0.0f;
        }
        if (0.0f < dx * particle->m_position.x + dy * particle->m_position.y + dz * particle->m_position.z) {
            return false;
        }
    }
    return true;
}

}  // namespace

// A plain particle started: the generator's shape, then into the emitter's space or along its
// path, settled on the ground and pushed along the emitter's motion (FUN_14124a4c0).
// ref: FUN_14124a4c0
static void ParticleInit(CParticleEmitter2* emitter, CParticle2* particle, float delta, const float* m) {
    emitter->m_generator->CreateParticle(particle, delta);
    uint32_t count = emitter->m_pathCount;
    if (count == 0) {
        if ((emitter->m_flags & 0x200) == 0) {
            C3Vector v = particle->m_velocity;
            particle->m_velocity.x = v.y * m[4] + v.x * m[0] + v.z * m[8];
            particle->m_velocity.y = v.y * m[5] + v.x * m[1] + v.z * m[9];
            particle->m_velocity.z = v.y * m[6] + v.x * m[2] + v.z * m[10];
            C3Vector p = particle->m_position;
            particle->m_position.x = p.y * m[4] + p.x * m[0] + p.z * m[8] + m[12];
            particle->m_position.z = p.y * m[6] + p.x * m[2] + p.z * m[10] + m[14];
            particle->m_position.y = p.y * m[5] + p.x * m[1] + p.z * m[9] + m[13];
            if ((emitter->m_flags & 0x40000) != 0) {
                auto ground = reinterpret_cast<GroundCallback>(emitter->m_scene->m_d6b0);
                if (ground != nullptr) {
                    float height = 0.0f;
                    if (ground(&particle->m_position.x, &height, emitter->m_scene->m_owner)) {
                        float z;
                        if ((emitter->m_flags & 0x4000) == 0) {
                            float life = ParticleLife(emitter->m_generator, particle);
                            float scale[2];
                            ParticleTrackVector2(emitter->m_scaleTrack, particle->m_age / life, scale);
                            z = scale[1] <= scale[0] ? scale[0] : scale[1];
                            z += height;
                        } else {
                            z = height + 0.05f;  // DAT_14261a974
                        }
                        particle->m_position.z = z;
                    }
                }
            }
        }
    } else {
        uint16_t seed = particle->m_seed;
        uint32_t remainder = seed % count;
        float spread = (seed & 1) != 0 ? 0.5f : -0.5f;  // DAT_14261a988, DAT_1426c715c
        bool rotate = true;
        if (emitter->m_pathSegments == nullptr) {
            const float* points = emitter->m_pathPoints;
            if (points == nullptr) {
                rotate = false;
                goto follow;
            }
            uint32_t index = count - 2;
            if (remainder <= count - 2) {
                index = remainder;
            }
            float speed = particle->m_velocity.x * particle->m_velocity.x + particle->m_velocity.y * particle->m_velocity.y
                + particle->m_velocity.z * particle->m_velocity.z;
            speed = speed <= 2.3841858e-07f ? 0.0f : std::sqrt(speed);
            const float* a = points + index * 4;
            const float* b = points + (index + 1) * 4;
            float dx = b[0] - a[0];
            float dy = b[1] - a[1];
            float dz = b[2] - a[2];
            float length = dy * dy + dx * dx + dz * dz;
            if (2.3841858e-07f < length) {
                float scale = 1.0f / std::sqrt(length);
                dx *= scale;
                dy *= scale;
                dz *= scale;
            }
            particle->m_position.x = (a[0] + b[0]) * 0.5f;
            particle->m_position.y = (a[1] + b[1]) * 0.5f;
            particle->m_position.z = (a[2] + b[2]) * 0.5f;
            if (emitter->m_pathMode == 3) {
                particle->m_velocity = {dx * speed, dy * speed, dz * speed};
            } else if (emitter->m_pathMode == 4) {
                particle->m_velocity = {-(dx * speed), -(dy * speed), -(dz * speed)};
            }
            if ((emitter->m_flags & 0x200) == 0 || emitter->m_pathMode != 5) {
                rotate = false;
            }
        } else {
            uint32_t index = remainder;
            if (emitter->m_pathMode == 0) {
                float t = static_cast<float>(remainder) / static_cast<float>(count);
                index = static_cast<uint32_t>((t * t * spread + 0.5f) * static_cast<float>(count));
                if (count - 1 < index) {
                    index = count - 1;
                }
            }
            const C3Vector* a = &emitter->m_pathSegments[index * 2];
            const C3Vector* b = &emitter->m_pathSegments[index * 2 + 1];
            particle->m_position.x = (b->x + a->x) * 0.5f;
            particle->m_position.y = (b->y + a->y) * 0.5f;
            particle->m_position.z = (b->z + a->z) * 0.5f;
        }
        if (rotate) {
            C3Vector v = particle->m_velocity;
            particle->m_velocity.x = v.y * m[4] + v.x * m[0] + v.z * m[8];
            particle->m_velocity.y = v.y * m[5] + v.x * m[1] + v.z * m[9];
            particle->m_velocity.z = v.y * m[6] + v.x * m[2] + v.z * m[10];
        }
    }
follow:
    if ((emitter->m_flags & 0x800) != 0) {
        float speed = RndSeedSigned(&emitter->m_random) * emitter->m_generator->m_speedVariation + 1.0f;
        particle->m_velocity.y += emitter->m_direction.y * speed;
        particle->m_velocity.x += speed * emitter->m_direction.x;
        particle->m_velocity.z += emitter->m_direction.z * speed;
    }
}

// A model particle started: turned to the emitter's frame, given a tumble and a copy of the
// emitter's model (FUN_14124a080).
// ref: FUN_14124a080
static void ParticleInitModel(CParticleEmitter2* emitter, CParticle2_Model* particle, float delta, const float* m) {
    ParticleInit(emitter, particle, delta, m);
    if ((emitter->m_flags & 0x200) == 0) {
        float rotation[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
        float length = rotation[1] * rotation[1] + rotation[0] * rotation[0] + rotation[2] * rotation[2];
        if (2.3841858e-07f < length) {
            float scale = 1.0f / std::sqrt(length);
            rotation[0] *= scale;
            rotation[1] *= scale;
            rotation[2] *= scale;
        }
        QuaternionFromMatrix3(particle->m_rotation, rotation);
        QuaternionNormalize(particle->m_rotation);
    }
    CRndSeed* random = &emitter->m_random;
    float z = RndSeedMantissa(RndSeedNext(random));
    float y = RndSeedMantissa(RndSeedNext(random));
    float x = RndSeedMantissa(RndSeedNext(random));
    particle->m_spin.x = (x - 1.0f) * emitter->m_tumble[1] + emitter->m_tumble[0];
    // The reference drops the minimum from y and z.
    particle->m_spin.y = ((y - 1.0f) + 1.0f) * emitter->m_tumble[3];
    particle->m_spin.z = ((z - 1.0f) + 1.0f) * emitter->m_tumble[5];
    if ((emitter->m_flags & 0x10000) != 0) {
        float signZ = (RndSeedNext(random) & 1) != 0 ? 1.0f : -1.0f;
        float signY = (RndSeedNext(random) & 1) != 0 ? 1.0f : -1.0f;
        float signX = (RndSeedNext(random) & 1) == 0 ? -1.0f : 1.0f;
        particle->m_spin.x = signX * particle->m_spin.x;
        particle->m_spin.y = signY * particle->m_spin.y;
        particle->m_spin.z = signZ * particle->m_spin.z;
    }
    if (particle->m_model == nullptr && emitter->m_model != nullptr) {
        CM2BaseModel* copy = M2SceneCloneModel(emitter->m_scene, emitter->m_model, false);
        particle->m_model = copy->m_type == 0 ? static_cast<CM2Model*>(copy) : nullptr;
    }
}

// A multi-texture particle started: its layers' offsets and scroll speeds (FUN_14124a320).
// ref: FUN_14124a320
static void ParticleInitQuad(CParticleEmitter2* emitter, CParticle2_Quad* particle, float delta, const float* m) {
    ParticleInit(emitter, particle, delta, m);
    CRndSeed* random = &emitter->m_random;
    particle->m_uv[0] = RndSeedUnit(random);
    particle->m_uv[1] = RndSeedUnit(random);
    float vary = RndSeedSigned(random);
    particle->m_uvSpeed[0] = vary * emitter->m_uvSpeedVary[0] + emitter->m_uvSpeed[0];
    particle->m_uvSpeed[1] = vary * emitter->m_uvSpeedVary[1] + emitter->m_uvSpeed[1];
    particle->m_uv[2] = RndSeedUnit(random);
    particle->m_uv[3] = RndSeedUnit(random);
    vary = RndSeedSigned(random);
    particle->m_uvSpeed[2] = vary * emitter->m_uvSpeedVary[2] + emitter->m_uvSpeed[2];
    particle->m_uvSpeed[3] = vary * emitter->m_uvSpeedVary[3] + emitter->m_uvSpeed[3];
}

// A particle of the emitter's kind made from the list and started.
// ref: FUN_141407dc0
static void ParticleSpawnOne(CParticleEmitter2* emitter, float delta, const float* m) {
    int32_t type = emitter->m_type;
    if (type == 0) {
        ParticleInit(emitter, reinterpret_cast<CParticle2*>(emitter->m_list.Add()), delta, m);
    } else if (type == 1) {
        ParticleInitModel(emitter, reinterpret_cast<CParticle2_Model*>(emitter->m_list.Add()), delta, m);
    } else if (type == 2 || type == 3 || type == 4) {
        ParticleInitQuad(emitter, reinterpret_cast<CParticle2_Quad*>(emitter->m_list.Add()), delta, m);
    }
}

// The step's particles: a burst, then the rate's share, spread along the emitter's motion
// unless the emitter asks for none (FUN_14124b080).
// ref: FUN_14124b080
static void ParticleEmitterSpawn(CParticleEmitter2* emitter, float delta, const float* matrix, float distance) {
    float density = 1.0f;
    if ((emitter->m_flags & 0x400000) == 0) {
        float falloff = 1.0f - (distance - 50.0f) * 0.02f;  // DAT_14262f26c, DAT_14261a968
        density = 0.25f;  // DAT_1426193c4
        if (0.25f <= falloff) {
            density = falloff;
            if (1.0f <= falloff) {
                density = 1.0f;
            }
        }
    }
    float rate = emitter->m_generator->GetEmissionRate() * density;
    uint32_t flags = emitter->m_flags;
    if ((flags >> 28 & 1) == 0) {
        if (emitter->m_type == 2 || (emitter->m_type == 4 && emitter->m_textures[1] != nullptr)) {
            rate *= g_particleMTDensity;
        } else {
            rate *= g_particleDensity;
        }
    }
    if ((flags & 0x42) == 0x42) {
        uint64_t burst = static_cast<uint64_t>(rate);
        while (emitter->m_list.m_free.m_size != 0 && burst-- != 0) {
            ParticleSpawnOne(emitter, 0.0f, matrix);
        }
        flags = emitter->m_flags & ~0x40u;
        emitter->m_flags = flags;
    }
    if ((flags & 3) != 3 || std::fabs(rate) < 2.3841858e-07f) {
        return;
    }
    float owed = emitter->m_emission;
    float total = owed + rate * delta;
    emitter->m_emission = total;
    if (total < 1.0f || emitter->m_list.m_free.m_size == 0) {
        return;
    }
    if ((flags >> 13 & 1) == 0) {
        // Spread along the motion since the last update, the first where the last left off.
        float m[16];
        std::memcpy(m, matrix, sizeof(m));
        float lead = 1.0f - owed;
        float step = 1.0f / (rate * delta);
        float dy = step * (m[13] - emitter->m_previousPosition.y);
        float dz = step * (m[14] - emitter->m_previousPosition.z);
        float dx = step * (m[12] - emitter->m_previousPosition.x);
        m[13] = lead * dy + emitter->m_previousPosition.y;
        m[14] = lead * dz + emitter->m_previousPosition.z;
        m[12] = lead * dx + emitter->m_previousPosition.x;
        ParticleSpawnOne(emitter, delta, m);
        float left = emitter->m_emission - 1.0f;
        emitter->m_emission = left;
        while (1.0f < left) {
            if (emitter->m_list.m_free.m_size != 0) {
                m[12] += dx;
                m[13] += dy;
                m[14] += dz;
                ParticleSpawnOne(emitter, delta, m);
            }
            left = emitter->m_emission - 1.0f;
            emitter->m_emission = left;
        }
        return;
    }
    uint32_t made = 0;
    uint64_t count = static_cast<uint64_t>(total);
    while (count-- != 0) {
        RndSeedNext(&emitter->m_random);
        ParticleSpawnOne(emitter, delta, matrix);
        made++;
        if (emitter->m_list.m_free.m_size == 0) {
            break;
        }
    }
    emitter->m_emission -= static_cast<float>(made);
}

// Room for the particles the emitter may have alive at once, and its children's for each of
// them (FUN_14124f530).
// ref: FUN_14124f530
static void ParticleEmitterReserve(CParticleEmitter2* emitter) {
    const float margin = 1.15f;  // DAT_142724c34
    CParticleGenerator* generator = emitter->m_generator;
    float needed = generator->m_54 * generator->m_emissionRateScale
        * (generator->m_emissionRate + generator->m_emissionRateVariation)
        * (generator->m_lifespan + generator->m_lifespanVariation) * generator->m_lifespanScale * margin;
    float ceiling = needed;
    int32_t truncated = static_cast<int32_t>(needed);
    if (truncated != INT32_MIN && static_cast<float>(truncated) != needed) {
        ceiling = static_cast<float>(truncated + (needed < 0.0f ? 0 : 1));
    }
    int32_t count = 4;
    if (4 < static_cast<int32_t>(ceiling)) {
        count = static_cast<int32_t>(ceiling);
    }
    emitter->m_list.Reserve(static_cast<uint32_t>(count));
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        CParticleEmitter2* child = emitter->m_children[i];
        CParticleGenerator* theirs = child->m_generator;
        uint32_t each = static_cast<uint32_t>(static_cast<int64_t>(theirs->m_54 * theirs->m_emissionRateScale
            * (theirs->m_emissionRate + theirs->m_emissionRateVariation)
            * (theirs->m_lifespan + theirs->m_lifespanVariation) * theirs->m_lifespanScale * margin));
        uint32_t total = each * static_cast<uint32_t>(count);
        if (0x1000 < total) {
            total = 0x1000;
        }
        child->m_list.Reserve(total);
    }
}

static void ParticleEmitterStep(CParticleEmitter2* emitter, float delta, int32_t child, float distance);

// A live particle's children spawn from where it is, carried along its velocity or from where
// it was.
static void ParticleSpawnChildren(CParticleEmitter2* emitter, const CParticle2* particle, const C3Vector& before,
    float delta, float distance) {
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        CParticleEmitter2* child = emitter->m_children[i];
        float x = emitter->m_parentMatrix[12];
        float y = emitter->m_parentMatrix[13];
        float z = emitter->m_parentMatrix[14];
        emitter->m_parentMatrix[12] = particle->m_position.x;
        emitter->m_parentMatrix[13] = particle->m_position.y;
        emitter->m_parentMatrix[14] = particle->m_position.z;
        uint32_t flags = child->m_flags;
        if ((flags >> 11 & 1) != 0) {
            child->m_direction = particle->m_velocity;
        }
        if ((flags >> 13 & 1) != 0) {
            child->m_previousPosition = before;
        }
        ParticleEmitterSpawn(child, delta, emitter->m_parentMatrix, distance);
        emitter->m_parentMatrix[12] = x;
        emitter->m_parentMatrix[13] = y;
        emitter->m_parentMatrix[14] = z;
    }
}

// A model particle turned by its spin, then moved; false when it was removed (FUN_141248ed0).
// ref: FUN_141248ed0
static bool ParticleUpdateModel(CParticleEmitter2* emitter, float delta, CParticle2_Model* particle, uint32_t index,
    const ParticleForces* forces, float distance) {
    C3Vector before = particle->m_position;
    float rate = std::sqrt(particle->m_spin.x * particle->m_spin.x + particle->m_spin.y * particle->m_spin.y
        + particle->m_spin.z * particle->m_spin.z);
    if (9.9999997e-05f < rate) {  // DAT_1426c6f80
        float angle = rate * delta * 0.5f;
        float s = FastSin(angle) / rate;
        float c = FastCos(angle);
        float* q = particle->m_rotation;
        float x = q[0];
        float y = q[1];
        float z = q[2];
        float w = q[3];
        float sx = s * particle->m_spin.x;
        float sz = s * particle->m_spin.z;
        float sy = s * particle->m_spin.y;
        q[0] = (w * sx + x * c + y * sz) - z * sy;
        q[1] = (w * sy + y * c + z * sx) - x * sz;
        q[2] = (w * sz + z * c + x * sy) - y * sx;
        q[3] = ((w * c - x * sx) - y * sy) - z * sz;
        QuaternionNormalize(q);
    }
    if (!ParticleIntegrate(emitter, particle, delta, forces)) {
        emitter->m_list.Remove(index);
        return false;
    }
    ParticleSpawnChildren(emitter, particle, before, delta, distance);
    return true;
}

// The emitter's particles for `steps` steps of `delta`, by its kind (FUN_141248bc0 plain,
// FUN_141248580 models, FUN_141248770 multi-texture), each step spawning unless this is a
// child, which its parent spawns.
// ref: FUN_141248bc0
// ref: FUN_141248580
// ref: FUN_141248770
static void ParticleEmitterUpdateKind(CParticleEmitter2* emitter, float delta, int32_t child, uint32_t steps, float distance) {
    if (delta <= 0.0f) {
        return;
    }
    uint8_t flags = static_cast<uint8_t>(emitter->m_flags);
    if ((flags & 3) == 3 || (flags & 0x42) == 0x42) {
        ParticleEmitterReserve(emitter);
    }
    ParticleForces forces = {};
    ParticleEmitterForces(emitter, delta, &forces);
    int32_t type = emitter->m_type;
    for (; steps != 0; steps--) {
        if (child == 0) {
            ParticleEmitterSpawn(emitter, delta, emitter->m_parentMatrix, distance);
        }
        uint32_t i = 0;
        while (i < emitter->m_list.m_live.m_size) {
            auto particle = reinterpret_cast<CParticle2*>(emitter->m_list.Get(i));
            particle->m_age += delta;
            if (ParticleLife(emitter->m_generator, particle) <= particle->m_age) {
                emitter->m_list.Remove(i);
                continue;
            }
            if (type == 1) {
                if (ParticleUpdateModel(emitter, delta, static_cast<CParticle2_Model*>(particle), i, &forces, distance)) {
                    i++;
                }
                continue;
            }
            C3Vector before = particle->m_position;
            if (type != 0) {
                auto quad = static_cast<CParticle2_Quad*>(particle);
                quad->m_uv[0] = Fraction(delta * quad->m_uvSpeed[0] + quad->m_uv[0]);
                quad->m_uv[1] = Fraction(quad->m_uvSpeed[1] * delta + quad->m_uv[1]);
                quad->m_uv[2] = Fraction(delta * quad->m_uvSpeed[2] + quad->m_uv[2]);
                quad->m_uv[3] = Fraction(quad->m_uvSpeed[3] * delta + quad->m_uv[3]);
            }
            if (!ParticleIntegrate(emitter, particle, delta, &forces)) {
                emitter->m_list.Remove(i);
                continue;
            }
            ParticleSpawnChildren(emitter, particle, before, delta, distance);
            i++;
        }
        for (uint32_t c = 0; c < emitter->m_childCount; c++) {
            ParticleEmitterStep(emitter->m_children[c], delta, 1, distance);
        }
    }
}

// The update in steps of a tenth of a second, as many as the generator's lifespan allows, then
// the remainder (FUN_14124c530).
// ref: FUN_14124c530
static void ParticleEmitterStep(CParticleEmitter2* emitter, float delta, int32_t child, float distance) {
    float remaining = 0.0f <= delta ? delta : 0.0f;
    int32_t type = emitter->m_type;
    if (0.1f < remaining) {  // DAT_14261a97c
        float whole = std::floor(remaining * 10.0f);  // DAT_1426d2da4
        float limit = std::floor(emitter->m_generator->m_maxLifespan * 10.0f);
        remaining = remaining + whole * -0.1f;  // DAT_142724c40
        if (whole <= limit) {
            limit = whole;
        }
        int32_t count = static_cast<int32_t>(limit);
        float share = static_cast<float>(static_cast<int64_t>(static_cast<uint32_t>(count + 1)));
        emitter->m_stepMotion.x = emitter->m_motion.x / share;
        emitter->m_stepMotion.y = emitter->m_motion.y / share;
        emitter->m_stepMotion.z = emitter->m_motion.z / share;
        if (type >= 0 && type <= 4) {
            ParticleEmitterUpdateKind(emitter, 0.1f, child, static_cast<uint32_t>(count), distance);
        }
    } else {
        emitter->m_stepMotion = emitter->m_motion;
    }
    if (type >= 0 && type <= 4) {
        ParticleEmitterUpdateKind(emitter, remaining, child, 1, distance);
    }
}

// The emitter's matrices: the camera's position, the parent matrix (through the attachment's
// inverse when there is one) and the matrix's scale (FUN_14124fd70).
// ref: FUN_14124fd70
static void ParticleEmitterSetMatrices(CParticleEmitter2* emitter, const float* matrix, const float* origin,
    const float* attachment) {
    emitter->m_origin = {origin[0], origin[1], origin[2]};
    if (attachment == nullptr || (emitter->m_flags & 0x200) != 0) {
        std::memcpy(emitter->m_parentMatrix, matrix, sizeof(emitter->m_parentMatrix));
    } else {
        float inverse[16];
        MatrixRigidInverse(attachment, inverse);
        MatrixMultiply(emitter->m_parentMatrix, matrix, inverse);
    }
    emitter->m_parentScale = RowLength(matrix);
}

// The update's half of the model particles: each model placed, coloured, shown or twinkled
// out, and the emitter's bounds grown around it (FUN_14124d190).
// ref: FUN_14124d190
static bool ParticleUpdateModelInstance(CParticleEmitter2* emitter, CParticle2_Model* particle, const float* attachment) {
    uint32_t twinkle = 0;
    float percent = emitter->m_twinklePercent;
    if (percent < 1.0f || emitter->m_twinkleScale[1] != 0.0f) {
        twinkle = static_cast<uint32_t>(std::lrintf(emitter->m_twinkleSpeed * particle->m_age)
                      + static_cast<int32_t>(reinterpret_cast<uintptr_t>(particle) >> 5))
            & 0x7f;
        if (percent < 1.0f && percent < g_particleTwinkle[twinkle]) {
            particle->m_model->SetShown(false);
            return false;
        }
    }
    float t = particle->m_age / ParticleLife(emitter->m_generator, particle);
    uint8_t color[4];
    ParticleTrackColor(emitter, t, color);
    float alpha = ParticleTrackFloat(emitter->m_alphaTrack, t) * 255.0f;  // DAT_14261b484
    float scale[2];
    ParticleTrackVector2(emitter->m_scaleTrack, t, scale);
    float factor = g_particleTwinkle[twinkle] * emitter->m_twinkleScale[1] + emitter->m_twinkleScale[0];
    float sy = factor * scale[1];
    float sx = factor * scale[0];
    if ((emitter->m_flags >> 9 & 1) == 0 && (emitter->m_flags >> 10 & 1) != 0) {
        sy = emitter->m_parentScale * sy;
        sx = emitter->m_parentScale * sx;
    }
    float m[16];
    MatrixFromQuaternion(m, particle->m_rotation);
    float scales[3] = {sx, sy, (sy + sx) * 0.5f};
    MatrixScale(m, scales);
    m[12] = particle->m_position.x;
    m[13] = particle->m_position.y;
    m[14] = particle->m_position.z;
    const float* parent = nullptr;
    if ((emitter->m_flags & 0x200) == 0) {
        parent = attachment;
    } else {
        parent = emitter->m_parentMatrix;
    }
    if (parent != nullptr) {
        MatrixMultiply(m, m, parent);
    }
    CM2Model* model = particle->m_model;
    model->SetLocalMatrix(m);
    const float inverse = 0.0039215689f;  // DAT_142632528
    model->m_color.x = static_cast<float>(color[2]) * inverse;
    model->m_color.y = static_cast<float>(color[1]) * inverse;
    model->m_color.z = static_cast<float>(color[0]) * inverse;
    model->m_alpha = static_cast<float>(std::lrintf(alpha) & 0xff) * inverse;
    model->SetShown(true);
    if ((model->m_flags & 1) != 0) {
        float sphere[4];
        model->GetSphere(sphere);
        float radius = (sy <= sx ? sx : sy) * sphere[3];
        float cx = sphere[0] * m[0] + sphere[1] * m[4] + sphere[2] * m[8] + m[12];
        float cy = sphere[0] * m[1] + sphere[1] * m[5] + sphere[2] * m[9] + m[13];
        float cz = sphere[0] * m[2] + sphere[1] * m[6] + sphere[2] * m[10] + m[14];
        float* bounds = emitter->m_bounds;
        float low[3] = {cx - radius, cy - radius, cz - radius};
        float high[3] = {cx + radius, cy + radius, cz + radius};
        for (uint32_t i = 0; i < 3; i++) {
            if (low[i] < bounds[i]) {
                bounds[i] = low[i];
            }
            if (bounds[3 + i] < high[i]) {
                bounds[3 + i] = high[i];
            }
        }
    }
    return true;
}

// The emitter's frame (FUN_14124f660).
// ref: FUN_14124f660
void ParticleEmitterUpdate(CParticleEmitter2* emitter, float delta, const float* matrix, const float* camera,
    const float* attachment, M2ParticlePool* pool) {
    float m[16];
    std::memcpy(m, matrix, sizeof(m));
    float fade = -camera[10] - 0.70710677f;  // DAT_1426d2508
    if (fade <= 0.0f) {
        fade = 0.0f;
        emitter->m_viewFade = 0.0f;
    } else {
        fade *= 3.8637033f;  // DAT_142724c38
        emitter->m_viewFade = fade;
        if (1.0f <= fade) {
            fade = 1.0f;
        }
    }
    emitter->m_viewFade = fade;
    emitter->m_generator->Update(delta, reinterpret_cast<C44Matrix*>(m));
    float dy = m[13] - camera[13];
    float dx = m[12] - camera[12];
    float dz = m[14] - camera[14];
    float distance = dy * dy + dx * dx + dz * dz;
    distance = distance <= 0.0f ? 0.0f : std::sqrt(distance);
    emitter->m_previousPosition = {emitter->m_parentMatrix[12], emitter->m_parentMatrix[13], emitter->m_parentMatrix[14]};
    ParticleEmitterSetMatrices(emitter, m, &camera[12], attachment);
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        ParticleEmitterSetMatrices(emitter->m_children[i], m, &camera[12], attachment);
    }
    uint32_t flags = emitter->m_flags;
    if ((flags >> 27 & 1) == 0) {
        flags |= 0x8000000;
        emitter->m_previousPosition = {emitter->m_parentMatrix[12], emitter->m_parentMatrix[13], emitter->m_parentMatrix[14]};
        emitter->m_flags = flags;
    }
    if (2.3841858e-07f <= std::fabs(delta)) {
        if ((flags >> 19 & 1) != 0) {
            float my = emitter->m_parentMatrix[13] - emitter->m_previousPosition.y;
            float mz = emitter->m_parentMatrix[14] - emitter->m_previousPosition.z;
            emitter->m_motion.x = emitter->m_parentMatrix[12] - emitter->m_previousPosition.x;
            emitter->m_motion.y = my;
            emitter->m_motion.z = mz;
            float mx = emitter->m_motion.x;
            float speed = 0.0f;
            float length = mx * mx + my * my + mz * mz;
            if (0.0f < length) {
                speed = std::sqrt(length) / delta;
            }
            speed = speed * emitter->m_followSlope + emitter->m_followBase;
            float follow = 0.0f;
            if (0.0f <= speed) {
                follow = 1.0f <= speed ? 1.0f : speed;
            }
            emitter->m_motion.y = follow * emitter->m_motion.y;
            emitter->m_motion.x = mx * follow;
            emitter->m_motion.z = follow * emitter->m_motion.z;
        }
        if ((emitter->m_flags & 0x800) != 0) {
            float elapsed = delta + emitter->m_followTime;
            emitter->m_followTime = elapsed;
            const float period = 0.033333335f;  // DAT_142724c30
            if (period < elapsed) {
                emitter->m_followTime = 0.0f;
                if (emitter->m_list.m_live.m_size == 0) {
                    float scale = (period / elapsed) * emitter->m_burstMultiplier;
                    emitter->m_direction.x = (emitter->m_parentMatrix[12] - emitter->m_previousPosition.x) * scale;
                    emitter->m_direction.y = (emitter->m_parentMatrix[13] - emitter->m_previousPosition.y) * scale;
                    emitter->m_direction.z = (emitter->m_parentMatrix[14] - emitter->m_previousPosition.z) * scale;
                } else {
                    emitter->m_direction = {0.0f, 0.0f, 0.0f};
                }
            }
        }
        ParticleEmitterStep(emitter, delta, 0, distance);
        emitter->m_flags |= 0x80;
        if (emitter->m_type == 1 && emitter->m_model != nullptr) {
            OsMutexLock(&s_modelLock);
            for (uint64_t i = 0; i < emitter->m_list.m_live.m_size; i++) {
                ParticleUpdateModelInstance(emitter,
                    reinterpret_cast<CParticle2_Model*>(emitter->m_list.Get(static_cast<uint32_t>(i))), attachment);
            }
            OsMutexUnlock(&s_modelLock);
        }
    } else {
        emitter->m_flags = flags | 0x100;
    }
    if (emitter->m_type == 1 || pool == nullptr || g_particlePrebuild == 0) {
        return;
    }
    emitter->m_drawnCount = 0;
    uint32_t count = 0x4000 / emitter->m_verticesPerParticle;
    if (emitter->m_list.m_live.m_size <= count) {
        count = static_cast<uint32_t>(emitter->m_list.m_live.m_size);
    }
    if (count == 0) {
        return;
    }
    int32_t bytes = static_cast<int32_t>(GxVertexFormatStride(ParticleEmitterVertexFormat(emitter))
        * emitter->m_verticesPerParticle * count);
    float view[16];
    MatrixRigidInverse(camera, view);
    float space4[16];
    float normal[3] = {0.0f, 0.0f, 1.0f};
    float rotation[9];
    ParticleSpace space = {space4, normal, rotation};
    ParticleEmitterSetSpace(emitter, attachment, view, &space);
    int32_t offset = _InterlockedExchangeAdd(reinterpret_cast<volatile long*>(&pool->m_used), bytes);
    uint32_t end = static_cast<uint32_t>(bytes + offset);
    auto lock = reinterpret_cast<blz::shared_mutex*>(reinterpret_cast<uint8_t*>(pool) + sizeof(M2ParticlePool));
    if (static_cast<uint32_t>(pool->m_bytes) <= end) {
        lock->lock();
        uint32_t capacity = static_cast<uint32_t>(pool->m_bytes);
        if (capacity < end) {
            uint32_t grown = capacity * 2;
            if (grown <= end) {
                grown = end * 2;
            }
            if (capacity < grown) {
                auto data = static_cast<uint8_t*>(
                    SMemAlloc(grown, 0x15, s_particleFile, SStrHashFnv1a(s_particleFile), 0xe32, 0));
                if (data != nullptr) {
                    uint32_t used = static_cast<uint32_t>(pool->m_bytes);
                    if (static_cast<uint32_t>(pool->m_used) < used) {
                        used = static_cast<uint32_t>(pool->m_used);
                    }
                    std::memcpy(data, pool->m_data, used);
                    SMemFree(pool->m_data, s_particleFile, 0xe37, 0);
                    pool->m_data = data;
                    pool->m_bytes = static_cast<int32_t>(grown);
                }
            }
        }
        lock->unlock();
    }
    emitter->m_prebuiltOffset = offset;
    emitter->m_prebuiltSize = bytes;
    lock->lock_shared();
    ParticleEmitterWrite(emitter, pool->m_data + static_cast<uint32_t>(offset), count, camera, &space);
    lock->unlock_shared();
}

// The rotation particles' space keeps (DAT_142cfb100): a quarter turn about z, for models whose
// flag 0x400 asks for it.
static const float s_particleTurn[16] = {
    0.0f, 1.0f, 0.0f, 0.0f,
    -1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

// The emitters' update and draw preparation for the pass: their tracks onto the generators,
// their matrices from the bones, and the models their particles carry lit for the pass
// (FUN_1411b2c90).
// ref: FUN_1411b2c90
void CM2Model::UpdateParticles(float delta, uint32_t pass, uint32_t a4) {
    M2Data* data = this->m_shared->m_data;
    const M2CompBone* bones = this->m_animState->m_bones->Data();
    const float* boneMatrices = reinterpret_cast<const float*>(this->m_animState->m_boneMatrices);
    uint32_t lodMask = M2SharedLodBoneMask(this->m_shared, this->m_lodRequested);
    for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
        M2ParticleState* state = &this->m_particleStates[i];
        if (state->m_8d == 0) {
            continue;
        }
        uint8_t on = state->m_8c;
        CParticleEmitter2* emitter = this->m_particles[i];
        CParticleGenerator* generator = emitter->m_generator;
        const M2Particle* def = &data->m_particles[i];
        if ((bones[def->m_bone].m_flags & lodMask) != 0) {
            if (!ParticleEmitterHasParticles(emitter)) {
                continue;
            }
            on = 0;
        }
        if ((this->m_modelFlags & 0x20000) != 0) {
            delta = 0.0f;
        }
        if ((def->m_flags & 0x8000) == 0) {
            emitter->m_flags = on == 0 ? emitter->m_flags & ~1u : emitter->m_flags | 1;
        } else {
            uint8_t burst = 0;
            if (state->m_enabled.m_value != 0 && 0.0f < state->m_emissionRate.m_value) {
                burst = 1;
                if (state->m_8e == 0) {
                    emitter->m_flags |= 0x40;
                    state->m_8e = 1;
                    goto latched;
                }
            }
            state->m_8e = burst;
        }
    latched:
        float rate = 0.0f;
        if (state->m_8c != 0) {
            rate = state->m_emissionRate.m_value;
            if (rate <= 0.0f) {
                rate = 0.0f;
            }
        }
        generator->m_emissionRate = rate;
        if (state->m_enabled.m_value != 0) {
            generator->m_speed = state->m_emissionSpeed.m_value;
            generator->m_speedVariation = state->m_speedVariation.m_value;
            generator->m_lifespan = state->m_lifespan.m_value;
            generator->m_gravity = state->m_18.m_value;
            if (emitter->m_extended == nullptr) {
                generator->m_zSource = state->m_zSource.m_value;
            }
            generator->m_areaLength = state->m_emissionAreaLength.m_value;
            generator->m_areaWidth = state->m_emissionAreaWidth.m_value;
            generator->m_verticalRange = state->m_verticalRange.m_value;
            generator->m_horizontalRange = state->m_horizontalRange.m_value;
        }
        emitter->m_alpha = this->m_params.m_fade * this->m_params.m_alpha;
        float m[16];
        MatrixMultiply(m, boneMatrices + def->m_bone * 16, reinterpret_cast<const float*>(&this->m_world));
        MatrixTranslate(m, &def->m_position.x);
        float camera[16];
        MatrixMultiply(camera, m, reinterpret_cast<const float*>(&this->m_scene->m_inverseView));
        const float* matrix = camera;
        float turned[16];
        if ((this->m_modelFlags2 >> 10 & 1) != 0) {
            MatrixMultiply(turned, s_particleTurn, camera);
            matrix = turned;
        }
        ParticleEmitterUpdate(emitter, delta, matrix, reinterpret_cast<const float*>(&this->m_scene->m_inverseView),
            reinterpret_cast<const float*>(this->m_558), &this->m_scene->m_particles);
        uint32_t drawn = 0x4000 / emitter->m_verticesPerParticle;
        if (emitter->m_list.m_live.m_size <= drawn) {
            drawn = static_cast<uint32_t>(emitter->m_list.m_live.m_size);
        }
        this->m_scene->m_drawLists[pass].m_2f4 += static_cast<int32_t>(drawn);
        uint32_t models = ParticleEmitterModelCount(emitter);
        for (uint32_t j = 0; j < models; j++) {
            uint32_t index = j;
            CM2Model* model = ParticleEmitterGetModel(emitter, &index);
            CM2Scene* scene = this->m_scene;
            model->UpdateMatrices(&scene->m_camera, &this->m_params, scene->m_frame, scene->m_time,
                static_cast<uint32_t>(M2AnimFrameSkipLod()));
            model->PrepareLighting(static_cast<int32_t>(pass), a4);
            model->m_lightingUsed = this->m_lightingUsed;
        }
    }
}

// --- the system ------------------------------------------------------------------------------

float g_particleTwinkle[0x80];  // DAT_1430a1ce0
// The device has no window: the particle system makes nothing with it (DAT_1430a1cc8).
uint8_t g_particleHeadless;

// ref: FUN_14124bdc0
void ParticleSystemInitialize() {
    if (s_initCount != 0) {
        s_initCount++;
        return;
    }
    int32_t high = rand();
    uint16_t low = static_cast<uint16_t>(rand());
    CRndSeed random;
    RndSeedInit(&random, static_cast<uint32_t>(low) | static_cast<uint32_t>(high) << 16);
    for (uint32_t i = 0; i < 0x80; i++) {
        g_particleTwinkle[i] = RndSeedUnit(&random);
    }
    if (g_particleHeadless != 0) {
        s_initCount++;
        g_particleIndices = nullptr;
        return;
    }
    GxAddDeviceCreatedCallback(ParticleSystemOnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(ParticleSystemOnDeviceDestroyed, nullptr);
    s_initCount++;
}

// ref: FUN_14124ab20
void ParticleSystemShutdown() {
    s_initCount--;
    if (s_initCount != 0) {
        return;
    }
    GxRemoveDeviceCreatedCallback(ParticleSystemOnDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(ParticleSystemOnDeviceDestroyed, nullptr);
    GxBufDestroy(&g_particleIndices);
}

// ref: FUN_14124c270
void ParticleSystemCreatePools() {
    static const int32_t s_sizes[7] = {0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000, 0x20000};  // DAT_142724b60
    static const int32_t s_counts[7] = {32, 32, 32, 32, 16, 16, 8};  // DAT_142724b80
    static const char* const s_names[7] = {"Particle2K", "Particle4K", "Particle8K", "Particle16K", "Particle32K",
        "Particle64K", "Particle128K"};  // PTR_s_Particle2K_142cfbb78
    for (uint32_t i = 0; i < 7; i++) {
        void* memory = SMemAlloc(sizeof(TObjectAllocHeap), g_particleHeaps.m_memFlags, s_allocFile,
            SStrHashFnv1a(s_allocFile), 0x1d, 0);
        auto heap = new (memory) TObjectAllocHeap(s_names[i], (static_cast<uint32_t>(s_sizes[i]) + 7) & 0xfffffffc,
            static_cast<uint32_t>(s_counts[i]), g_particleHeaps.m_memFlags);
        M2SceneVector<M2ParticleHeap>* heaps = &g_particleHeaps.m_heaps;
        uint64_t capacity = heaps->m_capacity & 0x7fffffffffffffffull;
        if (heaps->m_size == capacity) {
            uint64_t field = heaps->m_capacity;
            uint64_t grown = (field >> 1 & 0x3fffffffffffffffull) + capacity;
            if (grown < capacity + 1) {
                grown = capacity + 1;
            }
            heaps->m_capacity = ((field ^ grown) & 0x8000000000000000ull) ^ grown;
            auto data = static_cast<M2ParticleHeap*>(SMemAlloc(grown * sizeof(M2ParticleHeap), 0xb, s_heapPairName,
                SStrHashFnv1a(s_heapPairName), -6, 0));
            for (uint64_t j = 0; j < heaps->m_size; j++) {
                data[j] = heaps->m_data[j];
            }
            if (static_cast<int64_t>(field) >= 0) {
                SMemFree(heaps->m_data, s_heapPairName, -6, 0);
            }
            heaps->m_capacity &= 0x7fffffffffffffffull;
            heaps->m_data = data;
        }
        heaps->m_data[heaps->m_size] = {static_cast<uint32_t>(s_sizes[i]), 0, heap};
        heaps->m_size++;
        // FUN_1412491b0, the standard library's sort by size.
        std::sort(heaps->m_data, heaps->m_data + heaps->m_size,
            [](const M2ParticleHeap& a, const M2ParticleHeap& b) { return a.m_size < b.m_size; });
    }
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 3, s_particleFile, SStrHashFnv1a(s_particleFile), 0xe7, 0);
    s_keyPool = new (memory) TObjectAllocHeap("CParticleKeySimple", 0x44, 0x1000, 0x15);
}

// ref: FUN_14124aba0
void ParticleSystemDestroyPools() {
    for (uint64_t i = 0; i < g_particleHeaps.m_heaps.m_size; i++) {
        TObjectAllocHeap* heap = g_particleHeaps.m_heaps.m_data[i].m_heap;
        if (heap != nullptr) {
            heap->~TObjectAllocHeap();
            SMemFree(heap, s_allocFile, 0, 0);
        }
    }
    g_particleHeaps.m_heaps.m_size = 0;
    if (s_keyPool != nullptr) {
        s_keyPool->~TObjectAllocHeap();
        SMemFree(s_keyPool, s_particleFile, 0, 0);
    }
}

// ref: FUN_14124ab60
int32_t M2ParticlePoolRelease(M2ParticlePool* pool) {
    SMemFree(pool->m_data, s_particleFile, 0xe44, 0);
    pool->m_bytes = 0;
    pool->m_data = nullptr;
    return static_cast<int32_t>(_InterlockedExchange(reinterpret_cast<volatile long*>(&pool->m_used), 0));
}

// ref: FUN_14124dfe0
void QuaternionNormalize(float* q) {
    float z = q[2];
    float y = q[1];
    float x = q[0];
    float w = q[3];
    float length = w * w + z * z + x * x + y * y;
    if (2.3841858e-07f < length) {
        float scale = 1.0f / std::sqrt(length);
        q[0] = x * scale;
        q[1] = y * scale;
        q[2] = z * scale;
        q[3] = w * scale;
    }
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a1f70 = (void*)&s_modelLock;
// ---- end of generated globals ----
