#ifndef MODEL2_M2_PARTICLE_HPP
#define MODEL2_M2_PARTICLE_HPP

// CParticleEmitter2 (Engine/Source/Model2/Particles, 0x2f8): one M2Particle's emitter on a model,
// with its generator (the emission shape), its live particles and the emitters of the models it
// spawns. Only the fields the model code reads are named so far; the emitter's own functions
// are ported with M2Particle.cpp.

#include <cstddef>
#include <cstdint>

class CParticleGenerator;
class CM2Model;
class CM2Scene;
struct C44Matrix;

class CParticleEmitter2 {
    public:
    uint8_t m_00[0x18];
    CParticleGenerator* m_generator;    // 0x18
    uint8_t m_20[0x20];
    int32_t m_liveCount;                // 0x40 particles alive (in the list at 0x20)
    uint8_t m_44[0xb0 - 0x44];
    uint32_t m_childCount;              // 0xb0
    uint32_t m_padb4;
    CParticleEmitter2* m_children[4];   // 0xb8 a recursive model's emitters
    uint8_t m_d8[0xe0 - 0xd8];
    CM2Scene* m_scene;                  // 0xe0
    uint8_t m_e8[0x1e0 - 0xe8];
    // 1 emitting, 2 enabled, 0x40 burst, 0x8000 driven by the model
    uint32_t m_flags;                   // 0x1e0
    uint8_t m_1e4[0x2f8 - 0x1e4];
};
static_assert(offsetof(CParticleEmitter2, m_flags) == 0x1e0, "CParticleEmitter2 flags at 0x1e0");
static_assert(sizeof(CParticleEmitter2) == 0x2f8, "CParticleEmitter2 is 0x2f8 bytes");

// A bone generator's model (FUN_14125c0d0) and its bones taken from it (FUN_14125b960).
void ParticleGeneratorSetModel(CParticleGenerator* generator, CM2Model* model);
void ParticleGeneratorInitializeBones(CParticleGenerator* generator);
// The matrix the emitter's particles are kept relative to, null for world space (FUN_141249ed0).
void ParticleEmitterSetParentMatrix(CParticleEmitter2* emitter, const C44Matrix* matrix);
// The emitter destroyed and freed (FUN_141249af0), and a generator's destructor (its vtable's
// first slot, called with 0).
void ParticleEmitterDelete(CParticleEmitter2* emitter);
void ParticleGeneratorDestroy(CParticleGenerator* generator);
// Whether the emitter or any child has particles alive (FUN_14124bd50).
bool ParticleEmitterHasParticles(const CParticleEmitter2* emitter);

#endif
