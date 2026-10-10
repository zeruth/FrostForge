#ifndef MODEL2_M2_PARTICLE_HPP
#define MODEL2_M2_PARTICLE_HPP

// CParticleEmitter2 (Engine/Source/Model2/Particles, 0x2f8): one M2Particle's emitter on a model,
// with its generator (the emission shape), its live particles and the emitters of the models it
// spawns. Only the fields the model code reads are named so far; the emitter's own functions
// are ported with M2Particle.cpp.

#include <cstddef>
#include <cstdint>

class CParticleGenerator;

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
    uint8_t m_d8[0x1e0 - 0xd8];
    // 1 emitting, 2 enabled, 0x40 burst, 0x8000 driven by the model
    uint32_t m_flags;                   // 0x1e0
    uint8_t m_1e4[0x2f8 - 0x1e4];
};
static_assert(offsetof(CParticleEmitter2, m_flags) == 0x1e0, "CParticleEmitter2 flags at 0x1e0");
static_assert(sizeof(CParticleEmitter2) == 0x2f8, "CParticleEmitter2 is 0x2f8 bytes");

// Whether the emitter or any child has particles alive (FUN_14124bd50).
bool ParticleEmitterHasParticles(const CParticleEmitter2* emitter);

#endif
