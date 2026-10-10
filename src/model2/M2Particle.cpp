#include "model2/M2Particle.hpp"

// Engine/Source/Model2 particles (0x1412497e0..0x14125d800).

// ref: FUN_14124bd50
bool ParticleEmitterHasParticles(const CParticleEmitter2* emitter) {
    if (emitter->m_liveCount != 0) {
        return true;
    }
    for (uint32_t i = 0; i < emitter->m_childCount; i++) {
        if (ParticleEmitterHasParticles(emitter->m_children[i])) {
            return true;
        }
    }
    return false;
}
