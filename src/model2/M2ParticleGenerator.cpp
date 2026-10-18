#include "model2/M2Particle.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "model2/M2Model.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/ParticleSystem (0x14125aa10..0x14125d7a0): the generators, the shapes particles
// are made in. Each starts a particle's life and seed the same way and then places it and gives
// it a velocity by its shape.

namespace {

constexpr float EPSILON = 2.3841858e-07f;  // DAT_14261a954
constexpr float Z_SOURCE_MIN = 0.001f;     // DAT_1427250e0

// The velocity of a particle at `position` pushed away from the point zSource below the
// emitter, or `fallback` when there is no such point.
void AwayFromSource(const CParticleGenerator* generator, const C3Vector& position, float* direction) {
    float x = position.x;
    float y = position.y;
    float z = position.z - generator->m_zSource;
    float length = y * y + x * x + z * z;
    if (EPSILON < length) {
        float scale = 1.0f / std::sqrt(length);
        x *= scale;
        y *= scale;
        z *= scale;
    }
    direction[0] = x;
    direction[1] = y;
    direction[2] = z;
}

}  // namespace

// --- the base --------------------------------------------------------------------------------

// ref: FUN_14125d4d0
CParticleGenerator::CParticleGenerator(CRndSeed* random, const float* variation) {
    this->m_random = random;
    this->m_emissionRateVariation = variation[0];
    this->m_lifespanVariation = variation[1];
    // FUN_14125d780
    this->m_speed = 0.0f;
    this->m_speedVariation = 0.0f;
    this->m_lifespan = 0.0f;
    this->m_emissionRate = 0.0f;
    this->m_gravity = {0.0f, 0.0f, 0.0f};
    this->m_zSource = 0.0f;
    this->m_areaLength = 0.0f;
    this->m_areaWidth = 0.0f;
    this->m_verticalRange = 0.0f;
    this->m_horizontalRange = 0.0f;
    this->m_emissionRateScale = 1.0f;
    this->m_lifespanScale = 1.0f;
    this->m_maxLifespan = 1.0f;
    this->m_54 = 1.0f;
    this->m_kind = 4;
}

// ref: FUN_14125d530
CParticleGenerator::~CParticleGenerator() {
}

// ref: FUN_14125d600
float CParticleGenerator::GetEmissionRate() {
    return (RndSeedSigned(this->m_random) * this->m_emissionRateVariation + this->m_emissionRate)
        * this->m_emissionRateScale * this->m_54;
}

// ref: FUN_14125d770
void CParticleGenerator::Update(float delta, C44Matrix* matrix) {
    (void)delta;
    (void)matrix;
    this->m_maxLifespan = this->m_lifespanScale * this->m_lifespan;
}

// ref: FUN_14125d5a0
float CParticleGenerator::RandomSpeed() {
    return (RndSeedSigned(this->m_random) * this->m_speedVariation + 1.0f) * this->m_speed;
}

// ref: FUN_14125d660
void CParticleGenerator::InitParticle(CParticle2* particle, float delta) {
    float start = delta * RndSeedUnit(this->m_random);
    float vary = RndSeedSigned(this->m_random);
    int16_t variation;
    if (1.0f <= vary) {
        variation = 0x7fff;
    } else if (-1.0f < vary) {
        variation = static_cast<int16_t>(static_cast<int32_t>(vary * 32767.0f + 0.5f));  // DAT_142724c3c
    } else {
        variation = -0x7fff;
    }
    particle->m_lifeVariation = variation;
    float life = (static_cast<float>(variation) * 3.0518509e-05f * this->m_lifespanVariation + this->m_lifespan)
        * this->m_lifespanScale;
    if (life <= 0.001f) {
        life = 0.001f;
    }
    particle->m_age = std::fmod(start, life);  // FUN_140279b50
    particle->m_seed = static_cast<uint16_t>(RndSeedNext(this->m_random));
}

// --- plane -----------------------------------------------------------------------------------

// ref: FUN_14125aa10
CPlaneParticleGenerator::CPlaneParticleGenerator(CRndSeed* random, const float* variation)
    : CParticleGenerator(random, variation) {
    this->m_kind = 1;
}

// ref: FUN_14125aa40
CPlaneParticleGenerator::~CPlaneParticleGenerator() {
}

// ref: FUN_14125aa80
void CPlaneParticleGenerator::CreateParticle(CParticle2* particle, float delta) {
    this->InitParticle(particle, delta);
    float width = this->m_areaWidth;
    float y = RndSeedSigned(this->m_random) * width * 0.5f;  // FUN_14125abd0
    particle->m_position.x = RndSeedSigned(this->m_random) * this->m_areaLength * 0.5f;
    particle->m_position.y = y;
    particle->m_position.z = 0.0f;
    float speed = this->RandomSpeed();
    if (Z_SOURCE_MIN < this->m_zSource) {
        float x = particle->m_position.x;
        float py = particle->m_position.y;
        float z = particle->m_position.z - this->m_zSource;
        float scale = speed / std::sqrt(x * x + py * py + z * z);
        particle->m_velocity = {x * scale, py * scale, z * scale};
        return;
    }
    float vertical = RndSeedSigned(this->m_random) * this->m_verticalRange;
    float horizontal = RndSeedSigned(this->m_random) * this->m_horizontalRange;
    // FUN_14240ad63: both angles' sines and cosines at once.
    float sv = std::sin(vertical);
    float cv = std::cos(vertical);
    float sh = std::sin(horizontal);
    float ch = std::cos(horizontal);
    particle->m_velocity.x = ch * (speed * sv);
    particle->m_velocity.y = sh * (speed * sv);
    particle->m_velocity.z = cv * speed;
}

// --- sphere ----------------------------------------------------------------------------------

// ref: FUN_14125ace0
CSphereParticleGenerator::CSphereParticleGenerator(CRndSeed* random, const float* variation, bool upward)
    : CParticleGenerator(random, variation) {
    this->m_upward = upward;
    std::memset(this->m_pad61, 0, sizeof(this->m_pad61));
    this->m_kind = 0;
}

// ref: FUN_14125ad20
CSphereParticleGenerator::~CSphereParticleGenerator() {
}

// ref: FUN_14125ad60
void CSphereParticleGenerator::CreateParticle(CParticle2* particle, float delta) {
    this->InitParticle(particle, delta);
    float inner = this->m_areaLength;
    float span = this->m_areaWidth - inner;
    float radius = RndSeedUnit(this->m_random) * span + inner;
    float vertical = RndSeedSigned(this->m_random) * this->m_verticalRange;
    float horizontal = RndSeedSigned(this->m_random) * this->m_horizontalRange;
    float sv = std::sin(vertical);
    float cv = std::cos(vertical);
    float sh = std::sin(horizontal);
    float ch = std::cos(horizontal);
    float dx = ch * cv;
    float dy = sh * cv;
    float dz = sv;
    particle->m_position.x = dx * radius;
    particle->m_position.y = dy * radius;
    particle->m_position.z = dz * radius;
    if (Z_SOURCE_MIN < this->m_zSource) {
        float direction[3];
        AwayFromSource(this, particle->m_position, direction);
        dx = direction[0];
        dy = direction[1];
        dz = direction[2];
    } else if (this->m_upward) {
        dx = 0.0f;
        dy = 0.0f;
        dz = 1.0f;
    }
    float speed = this->RandomSpeed();
    particle->m_velocity = {speed * dx, speed * dy, speed * dz};
}

// --- spline ----------------------------------------------------------------------------------

// ref: FUN_14125af90
CSplineParticleGenerator::CSplineParticleGenerator(CRndSeed* random, const float* variation, uint16_t count,
    const C3Vector* points)
    : CParticleGenerator(random, variation) {
    this->m_position = 0.0f;
    this->m_pad64 = 0;
    this->m_splineVtable = nullptr;
    std::memset(this->m_spline, 0, sizeof(this->m_spline));
    // The spline through the points (NTempest's CSpline, FUN_14133fc00) is not ported.
    (void)count;
    (void)points;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14125b020
// The spline's destructor (FUN_14022b620) has nothing to release while it is not ported.
CSplineParticleGenerator::~CSplineParticleGenerator() {
}

// ref: FUN_14125b470
float CSplineParticleGenerator::GetEmissionRate() {
    float vary = RndSeedSigned(this->m_random);
    return (this->m_areaWidth * this->m_emissionRate + vary * this->m_emissionRateVariation) * this->m_emissionRateScale;
}

// ref: FUN_14125b070
void CSplineParticleGenerator::CreateParticle(CParticle2* particle, float delta) {
    // Placing the particle walks the spline (FUN_14133f8b0, FUN_1413406c0), not ported: the
    // particle starts dead.
    this->InitParticle(particle, delta);
    particle->m_age = 3.4028235e+38f;
    particle->m_position = {0.0f, 0.0f, 0.0f};
    particle->m_velocity = {0.0f, 0.0f, 0.0f};
    FROST_UNIMPLEMENTED();
}

// --- bone ------------------------------------------------------------------------------------

// ref: FUN_14125b4d0
CBoneParticleGenerator::CBoneParticleGenerator(CRndSeed* random, const float* variation)
    : CParticleGenerator(random, variation) {
    this->m_kind = 3;
    this->m_model = nullptr;
    static const float s_frames[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    std::memcpy(this->m_68, s_frames, sizeof(this->m_68));
}

// ref: FUN_14125b5b0
CBoneParticleGenerator::~CBoneParticleGenerator() {
}

// ref: FUN_14125c0f0
void CBoneParticleGenerator::Update(float delta, C44Matrix* matrix) {
    CParticleGenerator::Update(delta, matrix);
    if (this->m_model != nullptr) {
        this->m_model->GetRootMatrix(reinterpret_cast<float*>(matrix));
    }
}

// ref: FUN_14125c020
void CBoneParticleGenerator::CreateParticle(CParticle2* particle, float delta) {
    // The bone emission table (FUN_14125b730, FUN_14125b5f0 and the model's bone positions,
    // FUN_1411ba510) is not ported: the particle starts dead.
    this->InitParticle(particle, delta);
    particle->m_age = 3.4028235e+38f;
    particle->m_position = {0.0f, 0.0f, 0.0f};
    particle->m_velocity = {0.0f, 0.0f, 0.0f};
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14125b550
CMirroredBoneParticleGenerator::CMirroredBoneParticleGenerator(CRndSeed* random, const float* variation)
    : CBoneParticleGenerator(random, variation) {
}

// ref: FUN_14125bec0
void CMirroredBoneParticleGenerator::CreateParticle(CParticle2* particle, float delta) {
    this->InitParticle(particle, delta);
    particle->m_age = 3.4028235e+38f;
    particle->m_position = {0.0f, 0.0f, 0.0f};
    particle->m_velocity = {0.0f, 0.0f, 0.0f};
    FROST_UNIMPLEMENTED();
}

// --- mesh ------------------------------------------------------------------------------------

// ref: FUN_14125c130
CMeshParticleGenerator::CMeshParticleGenerator(CRndSeed* random, const float* variation)
    : CParticleGenerator(random, variation) {
    this->m_60 = 0;
    this->m_6c = 2;
    this->m_90 = 0;
    this->m_64 = 0;
    this->m_68 = 0;
    std::memset(this->m_70, 0, sizeof(this->m_70));
    this->m_pad94 = 0;
    this->m_kind = 2;
}

// ref: FUN_14125c190
CMeshParticleGenerator::~CMeshParticleGenerator() {
}

// ref: FUN_14125c1d0
void CMeshParticleGenerator::CreateParticle(CParticle2* particle, float delta) {
    // The mesh's triangles (FUN_14125c220, FUN_14125c4f0, FUN_14125c8b0) are not ported: the
    // particle starts dead.
    this->InitParticle(particle, delta);
    particle->m_age = 3.4028235e+38f;
    particle->m_position = {0.0f, 0.0f, 0.0f};
    particle->m_velocity = {0.0f, 0.0f, 0.0f};
    FROST_UNIMPLEMENTED();
}

// --- made in place ---------------------------------------------------------------------------

CParticleGenerator* ParticleGeneratorCreatePlane(void* memory, void* random, const float* params) {
    return new (memory) CPlaneParticleGenerator(static_cast<CRndSeed*>(random), params);
}

CParticleGenerator* ParticleGeneratorCreateSphere(void* memory, void* random, const float* params, bool flag) {
    return new (memory) CSphereParticleGenerator(static_cast<CRndSeed*>(random), params, flag);
}

CParticleGenerator* ParticleGeneratorCreateSpline(void* memory, void* random, const float* params, uint16_t count,
    const C3Vector* points) {
    return new (memory) CSplineParticleGenerator(static_cast<CRndSeed*>(random), params, count, points);
}

// ref: FUN_14125b580
CParticleGenerator* ParticleGeneratorCreateBone(void* memory, void* random, const float* params) {
    return new (memory) CBoneParticleGenerator(static_cast<CRndSeed*>(random), params);
}

// The caller's seed and variations reach the base constructor in the registers they arrived in.
CParticleGenerator* ParticleGeneratorCreateBoneMirrored(void* memory, void* random, const float* params) {
    return new (memory) CMirroredBoneParticleGenerator(static_cast<CRndSeed*>(random), params);
}

CParticleGenerator* ParticleGeneratorCreateBox(void* memory, void* random, const float* params) {
    return new (memory) CMeshParticleGenerator(static_cast<CRndSeed*>(random), params);
}

// ref: FUN_14125c0d0
void ParticleGeneratorSetModel(CParticleGenerator* generator, CM2Model* model) {
    auto bone = static_cast<CBoneParticleGenerator*>(generator);
    if (bone->m_model == model) {
        return;
    }
    bone->m_model = model;
    if (model == nullptr) {
        return;
    }
    ParticleGeneratorInitializeBones(generator);
}

// ref: FUN_14125b960
void ParticleGeneratorInitializeBones(CParticleGenerator* generator) {
    // The model's shared bone emission table (FUN_1411c0460, FUN_1411be8e0, FUN_1411ba660,
    // FUN_1411a5700) is not ported.
    (void)generator;
    FROST_UNIMPLEMENTED();
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140279b50=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14125abd0=GenForwardMissing")
// ---- end of link fallbacks ----
