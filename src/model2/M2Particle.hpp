#ifndef MODEL2_M2_PARTICLE_HPP
#define MODEL2_M2_PARTICLE_HPP

// CParticleEmitter2 (Engine/Source/Model2 particles, 0x2f8): one M2Particle's emitter on a model,
// with its generator (the emission shape), its live particles and the emitters of the models it
// spawns. CParticleGenerator (0x48 and up) is the shape: plane, sphere, spline, bone or box. The
// fields named are those the model code and the emitters' setup read; the rest is padding until
// the emitters themselves are ported with M2Particle.cpp.

#include <cstddef>
#include <cstdint>

#include "model2/M2Data.hpp"

class CM2Model;
class CM2Scene;
class CTexture;
struct C44Matrix;

class CParticleGenerator {
    public:
    virtual ~CParticleGenerator() = default;
    uint8_t m_08[0x10];
    float m_speed;                      // 0x18
    float m_speedVariation;             // 0x1c
    float m_lifespan;                   // 0x20
    float m_emissionRate;               // 0x24
    C3Vector m_gravity;                 // 0x28
    float m_zSource;                    // 0x34
    float m_areaLength;                 // 0x38
    float m_areaWidth;                  // 0x3c
    float m_verticalRange;              // 0x40
    float m_horizontalRange;            // 0x44
};
static_assert(offsetof(CParticleGenerator, m_speed) == 0x18, "CParticleGenerator speed at 0x18");
static_assert(sizeof(CParticleGenerator) == 0x48, "CParticleGenerator base is 0x48 bytes");

class CParticleEmitter2 {
    public:
    uint64_t m_00;                      // 0x00 1
    int32_t m_cellShift;                // 0x08 log2 of the tile columns
    float m_cellWidth;                  // 0x0c
    float m_cellHeight;                 // 0x10
    int32_t m_tileRotation;             // 0x14
    CParticleGenerator* m_generator;    // 0x18
    uint8_t m_20[0x20];
    int32_t m_liveCount;                // 0x40 particles alive (in the list at 0x20)
    uint8_t m_44[0x80 - 0x44];
    uint32_t m_vertexFormat;            // 0x80 the draw's vertex format kind
    uint8_t m_84[0xa8 - 0x84];
    uint8_t m_random[8];                // 0xa8 the emitter's random seed
    uint32_t m_childCount;              // 0xb0
    uint32_t m_padb4;
    CParticleEmitter2* m_children[4];   // 0xb8 a recursive model's emitters
    CM2Model* m_model;                  // 0xd8 the geometry particles' model
    CM2Scene* m_scene;                  // 0xe0
    CM2Model* m_recursionModel;         // 0xe8
    uint8_t m_f0[0x100 - 0xf0];
    int32_t m_type;                     // 0x100 1 none, 2 quads, 3 the model's, 4 projected
    uint32_t m_pad104;
    float m_baseSpin;                   // 0x108
    float m_baseSpinVary;               // 0x10c
    float m_spin;                       // 0x110
    float m_spinVary;                   // 0x114
    uint8_t m_118[4];
    uint8_t m_blend;                    // 0x11c the M2 blend mode
    uint8_t m_renderFlags;              // 0x11d 1 unlit, 2 no fog, 4 no depth test, 8 ...
    uint8_t m_11e[2];
    const M2PartTrack<C3Vector>* m_colorTrack;      // 0x120
    const M2PartTrack<uint16_t>* m_alphaTrack;      // 0x128
    const M2PartTrack<C2Vector>* m_scaleTrack;      // 0x130
    C2Vector m_scaleVary;                           // 0x138
    const M2PartTrack<uint16_t>* m_headCellTrack;   // 0x140
    const M2PartTrack<uint16_t>* m_tailCellTrack;   // 0x148
    uint8_t m_150[0x1c8 - 0x150];
    void* m_1c8;                        // 0x1c8
    uint8_t m_1d0[8];
    uint32_t m_material;                // 0x1d8 the material key
    uint32_t m_pad1dc;
    // 1 emitting, 2 enabled, 0x40 burst, 0x8000 driven by the model
    uint32_t m_flags;                   // 0x1e0
    float m_twinkleSpeed;               // 0x1e4
    float m_twinklePercent;             // 0x1e8
    float m_twinkleScale[2];            // 0x1ec min, range
    float m_burstMultiplier;            // 0x1f4
    float m_tumble[6];                  // 0x1f8 min x, range x, min y, range y, min z, range z
    float m_drag;                       // 0x210
    C3Vector m_wind;                    // 0x214
    float m_windTime;                   // 0x220
    uint8_t m_224[0x2e0 - 0x224];
    const uint16_t* m_combiner;         // 0x2e0 the file's particle combiner
    const M2ExtendedParticle* m_extended;  // 0x2e8 EXP2
    uint8_t m_2f0[8];
};
static_assert(offsetof(CParticleEmitter2, m_random) == 0xa8, "CParticleEmitter2 random at 0xa8");
static_assert(offsetof(CParticleEmitter2, m_baseSpin) == 0x108, "CParticleEmitter2 spin at 0x108");
static_assert(offsetof(CParticleEmitter2, m_colorTrack) == 0x120, "CParticleEmitter2 tracks at 0x120");
static_assert(offsetof(CParticleEmitter2, m_flags) == 0x1e0, "CParticleEmitter2 flags at 0x1e0");
static_assert(offsetof(CParticleEmitter2, m_vertexFormat) == 0x80, "CParticleEmitter2 format at 0x80");
static_assert(offsetof(CParticleEmitter2, m_type) == 0x100, "CParticleEmitter2 type at 0x100");
static_assert(offsetof(CParticleEmitter2, m_blend) == 0x11c, "CParticleEmitter2 blend at 0x11c");
static_assert(offsetof(CParticleEmitter2, m_material) == 0x1d8, "CParticleEmitter2 material at 0x1d8");
static_assert(offsetof(CParticleEmitter2, m_wind) == 0x214, "CParticleEmitter2 wind at 0x214");
static_assert(offsetof(CParticleEmitter2, m_combiner) == 0x2e0, "CParticleEmitter2 combiner at 0x2e0");
static_assert(sizeof(CParticleEmitter2) == 0x2f8, "CParticleEmitter2 is 0x2f8 bytes");

// The particle effects by vertex format kind (DAT_1430a1ee8: unlit T1, lit T1, 2-colour-texture,
// two 3-colour-texture forms and refraction), looked up when the device comes up (FUN_14124aa50).
extern class CShaderEffect* g_particleEffects[6];
// Two emitters' draw order: blend, render flags, type, then their three textures (FUN_14124f4c0).
int32_t ParticleEmitterCompare(const CParticleEmitter2* left, const CParticleEmitter2* right);
// The vertex format a particle emitter draws with (FUN_14124bce0).
uint8_t ParticleEmitterVertexFormat(const CParticleEmitter2* emitter);

// An emitter made in place (FUN_1412497e0), and the generators made in place after it: plane
// (FUN_14125aa10), sphere (FUN_14125ace0), spline (FUN_14125af90), bone (FUN_14125b580, the
// mirrored one FUN_14125b550) and box (FUN_14125c130). `random` is the emitter's seed; `params`
// the emission rate and lifespan variations.
CParticleEmitter2* ParticleEmitterConstruct(void* memory, CM2Scene* scene);
CParticleGenerator* ParticleGeneratorCreatePlane(void* memory, void* random, const float* params);
CParticleGenerator* ParticleGeneratorCreateSphere(void* memory, void* random, const float* params, bool flag);
CParticleGenerator* ParticleGeneratorCreateSpline(void* memory, void* random, const float* params, uint16_t count,
    const C3Vector* points);
CParticleGenerator* ParticleGeneratorCreateBone(void* memory, void* random, const float* params);
CParticleGenerator* ParticleGeneratorCreateBoneMirrored(void* memory);
CParticleGenerator* ParticleGeneratorCreateBox(void* memory, void* random, const float* params);
// A bone generator's model (FUN_14125c0d0) and its bones taken from it (FUN_14125b960).
void ParticleGeneratorSetModel(CParticleGenerator* generator, CM2Model* model);
void ParticleGeneratorInitializeBones(CParticleGenerator* generator);
// The emitter's setup: its kind (FUN_14124e510), geometry model by name (FUN_14124e240) or id
// (FUN_14124e200), a texture layer's transform (FUN_14124e290), the tail and flags
// (FUN_14124e480), the tile grid (FUN_14124f220), random tiles (FUN_14124e070) and start cell
// (FUN_14124e540), the blend (FUN_14124e150), twinkle scale (FUN_14124f2a0), the follow ramp
// (FUN_14124e0a0), the recursion model (FUN_14124a010), the fast path check (FUN_14124ac30) and
// the colour overrides (FUN_14124b9e0, FUN_14124e2d0).
void ParticleEmitterSetKind(CParticleEmitter2* emitter, int32_t kind);
void ParticleEmitterSetModelByName(CParticleEmitter2* emitter, CM2Scene* scene, const char* name);
void ParticleEmitterSetModelById(CParticleEmitter2* emitter, CM2Scene* scene, int32_t fileDataId);
void ParticleEmitterSetTextureTransform(CParticleEmitter2* emitter, uint32_t layer, float rotation, const float* scroll,
    const float* scale);
void ParticleEmitterSetFlags(CParticleEmitter2* emitter, uint32_t a2, uint32_t a3, float tailLength, uint32_t a5);
void ParticleEmitterSetTileGrid(CParticleEmitter2* emitter, uint32_t rows, uint32_t columns);
void ParticleEmitterSetRandomTiles(CParticleEmitter2* emitter, int32_t on);
void ParticleEmitterSetRandomStartCell(CParticleEmitter2* emitter, int32_t on);
void ParticleEmitterSetBlend(CParticleEmitter2* emitter, const uint8_t* blend);
void ParticleEmitterSetTwinkleScale(CParticleEmitter2* emitter, const float* scale);
void ParticleEmitterSetFollow(CParticleEmitter2* emitter, float speed1, float scale1, float speed2, float scale2);
void ParticleEmitterSetRecursionModel(CParticleEmitter2* emitter, CM2Scene* scene, int32_t fileDataId, const char* name);
void ParticleEmitterPrepareFastPath(CParticleEmitter2* emitter);
void ParticleEmitterGetColors(CParticleEmitter2* emitter, uint8_t* start, uint8_t* middle, uint8_t* end);
void ParticleEmitterSetColors(CParticleEmitter2* emitter, const uint8_t* start, const uint8_t* middle, const uint8_t* end);
// The matrix the emitter's particles are kept relative to, null for world space (FUN_141249ed0).
void ParticleEmitterSetParentMatrix(CParticleEmitter2* emitter, const C44Matrix* matrix);
// One of the emitter's three textures (FUN_14124efd0).
void ParticleEmitterSetTexture(CParticleEmitter2* emitter, uint32_t slot, CTexture* texture);
// The emitter destroyed and freed (FUN_141249af0), and a generator's destructor (its vtable's
// first slot, called with 0).
void ParticleEmitterDelete(CParticleEmitter2* emitter);
void ParticleGeneratorDestroy(CParticleGenerator* generator);
// Whether the emitter or any child has particles alive (FUN_14124bd50).
bool ParticleEmitterHasParticles(const CParticleEmitter2* emitter);

#endif
