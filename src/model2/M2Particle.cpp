#include "model2/M2Particle.hpp"

#include "model2/M2Model.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2 particles (0x1412497e0..0x14125d800). The emitters themselves are not
// ported yet: nothing makes one until the model's emitters are created in InitializeLoaded.

// ref: FUN_14124efd0
void ParticleEmitterSetTexture(CParticleEmitter2* emitter, uint32_t slot, CTexture* texture) {
    (void)emitter;
    (void)slot;
    (void)texture;
    FROST_UNIMPLEMENTED();
}

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

// ref: FUN_1412497e0
CParticleEmitter2* ParticleEmitterConstruct(void* memory, CM2Scene* scene) {
    (void)memory;
    (void)scene;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14125aa10
CParticleGenerator* ParticleGeneratorCreatePlane(void* memory, void* random, const float* params) {
    (void)memory;
    (void)random;
    (void)params;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14125ace0
CParticleGenerator* ParticleGeneratorCreateSphere(void* memory, void* random, const float* params, bool flag) {
    (void)memory;
    (void)random;
    (void)params;
    (void)flag;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14125af90
CParticleGenerator* ParticleGeneratorCreateSpline(void* memory, void* random, const float* params, uint16_t count, const C3Vector* points) {
    (void)memory;
    (void)random;
    (void)params;
    (void)count;
    (void)points;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14125b580
CParticleGenerator* ParticleGeneratorCreateBone(void* memory, void* random, const float* params) {
    (void)memory;
    (void)random;
    (void)params;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14125b550
CParticleGenerator* ParticleGeneratorCreateBoneMirrored(void* memory) {
    (void)memory;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14125c130
CParticleGenerator* ParticleGeneratorCreateBox(void* memory, void* random, const float* params) {
    (void)memory;
    (void)random;
    (void)params;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_14124e510
void ParticleEmitterSetKind(CParticleEmitter2* emitter, int32_t kind) {
    (void)emitter;
    (void)kind;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e240
void ParticleEmitterSetModelByName(CParticleEmitter2* emitter, CM2Scene* scene, const char* name) {
    (void)emitter;
    (void)scene;
    (void)name;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e200
void ParticleEmitterSetModelById(CParticleEmitter2* emitter, CM2Scene* scene, int32_t fileDataId) {
    (void)emitter;
    (void)scene;
    (void)fileDataId;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e290
void ParticleEmitterSetTextureTransform(CParticleEmitter2* emitter, uint32_t layer, float rotation, const float* scroll, const float* scale) {
    (void)emitter;
    (void)layer;
    (void)rotation;
    (void)scroll;
    (void)scale;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e480
void ParticleEmitterSetFlags(CParticleEmitter2* emitter, uint32_t a2, uint32_t a3, float tailLength, uint32_t a5) {
    (void)emitter;
    (void)a2;
    (void)a3;
    (void)tailLength;
    (void)a5;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124f220
void ParticleEmitterSetTileGrid(CParticleEmitter2* emitter, uint32_t rows, uint32_t columns) {
    (void)emitter;
    (void)rows;
    (void)columns;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e070
void ParticleEmitterSetRandomTiles(CParticleEmitter2* emitter, int32_t on) {
    (void)emitter;
    (void)on;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e540
void ParticleEmitterSetRandomStartCell(CParticleEmitter2* emitter, int32_t on) {
    (void)emitter;
    (void)on;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e150
void ParticleEmitterSetBlend(CParticleEmitter2* emitter, const uint8_t* blend) {
    (void)emitter;
    (void)blend;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124f2a0
void ParticleEmitterSetTwinkleScale(CParticleEmitter2* emitter, const float* scale) {
    (void)emitter;
    (void)scale;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e0a0
void ParticleEmitterSetFollow(CParticleEmitter2* emitter, float speed1, float scale1, float speed2, float scale2) {
    (void)emitter;
    (void)speed1;
    (void)scale1;
    (void)speed2;
    (void)scale2;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124a010
void ParticleEmitterSetRecursionModel(CParticleEmitter2* emitter, CM2Scene* scene, int32_t fileDataId, const char* name) {
    (void)emitter;
    (void)scene;
    (void)fileDataId;
    (void)name;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124ac30
void ParticleEmitterPrepareFastPath(CParticleEmitter2* emitter) {
    (void)emitter;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124b9e0
void ParticleEmitterGetColors(CParticleEmitter2* emitter, uint8_t* start, uint8_t* middle, uint8_t* end) {
    (void)emitter;
    (void)start;
    (void)middle;
    (void)end;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14124e2d0
void ParticleEmitterSetColors(CParticleEmitter2* emitter, const uint8_t* start, const uint8_t* middle, const uint8_t* end) {
    (void)emitter;
    (void)start;
    (void)middle;
    (void)end;
    FROST_UNIMPLEMENTED();
}
