#include "model2/M2Particle.hpp"

#include "model2/M2Model.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2 particles (0x1412497e0..0x14125d800). The emitters themselves are not
// ported yet: nothing makes one until the model's emitters are created in InitializeLoaded.

// ref: FUN_141249af0
void ParticleEmitterDelete(CParticleEmitter2* emitter) {
    (void)emitter;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141249ed0
void ParticleEmitterSetParentMatrix(CParticleEmitter2* emitter, const C44Matrix* matrix) {
    (void)emitter;
    (void)matrix;
    FROST_UNIMPLEMENTED();
}

void ParticleGeneratorDestroy(CParticleGenerator* generator) {
    (void)generator;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14125c0d0
void ParticleGeneratorSetModel(CParticleGenerator* generator, CM2Model* model) {
    (void)generator;
    (void)model;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14125b960
void ParticleGeneratorInitializeBones(CParticleGenerator* generator) {
    (void)generator;
    FROST_UNIMPLEMENTED();
}

// The emitters' update and draw for the pass (FUN_1411b2c90), ported with the emitters.
// ref: FUN_1411b2c90
void CM2Model::UpdateParticles(float delta, uint32_t pass, uint32_t a4) {
    (void)delta;
    (void)pass;
    (void)a4;
    FROST_UNIMPLEMENTED();
}

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
