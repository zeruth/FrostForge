#ifndef MODEL2_M2_PARTICLE_HPP
#define MODEL2_M2_PARTICLE_HPP

// Engine/Source/ParticleSystem/ParticleSystem2.cpp: CParticleEmitter2 (0x2f8), one M2Particle's
// emitter on a model, with its generator (the emission shape), its live particles and the
// emitters of the models it spawns. CParticleGenerator (0x60 and up) is the shape: plane,
// sphere, spline, bone or box. The particles live in a CParticleList whose element type follows
// the emitter's kind: 0 plain billboards, 1 particles carrying a model, 2 and 3 multi-texture
// quads, 4 projected quads.

#include <cstddef>
#include <cstdint>

#include "base/RndSeed.hpp"
#include "model2/M2Data.hpp"
#include "model2/M2Scene.hpp"

class CM2Model;
class CM2Scene;
class CTexture;
class CShaderEffect;
class GxRenderContext;
class ShaderEffectContext;
class TObjectAllocHeap;
struct C44Matrix;
struct GxBuf;

// A particle (0x20), and the extended forms the other kinds use.
struct CParticle2 {
    C3Vector m_position;                // 0x00
    float m_age;                        // 0x0c
    C3Vector m_velocity;                // 0x10
    int16_t m_lifeVariation;            // 0x1c over 32767, scales the generator's life variation
    uint16_t m_seed;                    // 0x1e the particle's own random stream
};
static_assert(sizeof(CParticle2) == 0x20, "CParticle2 is 0x20 bytes");

// Kind 1 (0x48): turned by its quaternion, spun by its angular velocity, drawn as a model.
struct CParticle2_Model : CParticle2 {
    float m_rotation[4];                // 0x20 x, y, z, w
    C3Vector m_spin;                    // 0x30
    uint32_t m_pad3c;
    CM2Model* m_model;                  // 0x40
};
static_assert(sizeof(CParticle2_Model) == 0x48, "CParticle2_Model is 0x48 bytes");

// Kinds 2 and 3 (0x40): two texture layers' offsets, scrolled every update.
struct CParticle2_Quad : CParticle2 {
    float m_uv[4];                      // 0x20 layer 0 u, v, layer 1 u, v
    float m_uvSpeed[4];                 // 0x30
};
static_assert(sizeof(CParticle2_Quad) == 0x40, "CParticle2_Quad is 0x40 bytes");

// Kind 4 (0x50): the quad's own orientation.
struct CParticle2_Projected : CParticle2_Quad {
    float m_rotation[4];                // 0x40
};
static_assert(sizeof(CParticle2_Projected) == 0x50, "CParticle2_Projected is 0x50 bytes");

// What a particle list does with its elements (vtables 0x142724958.. by kind): make `count` in
// place, destroy them, and move them to new storage.
class CParticleTraits {
    public:
    virtual ~CParticleTraits() = default;
    virtual void Construct(uint8_t* data, uint64_t count) const = 0;
    virtual void Destroy(uint8_t* data, uint64_t count) const = 0;
    virtual void Move(uint8_t* to, uint8_t* from, uint64_t count) const = 0;
};

// The heaps particle storage may come from, by the size they hold (DAT_142cfbbb0,
// blz::vector<blz::pair<uint32_t, TObjectAllocMemBlock*>>), kept sorted, and the memory flags
// storage outside them is allocated with (DAT_142cfbbc8).
struct M2ParticleHeap {
    uint32_t m_size;
    uint32_t m_pad04;
    TObjectAllocHeap* m_heap;
};
struct M2ParticleHeaps {
    M2SceneVector<M2ParticleHeap> m_heaps;      // 0x00
    int32_t m_memFlags;                         // 0x18 0x15
    uint32_t m_pad1c;
};
extern M2ParticleHeaps g_particleHeaps;

// The particles of an emitter (0x60, vtable 0x1427250b8): storage for `m_count` elements of
// `m_stride` bytes, the indices in use and the indices free.
class CParticleList {
    public:
    CParticleList();
    virtual ~CParticleList();
    // The element kind's traits and size, and the heaps to draw storage from (FUN_14125d020).
    void SetTraits(const CParticleTraits* traits, uint32_t stride, M2ParticleHeaps* heaps);
    // Room for `count` elements, the new ones constructed and free (FUN_14125d030).
    void Reserve(uint32_t count);
    // A free element taken into use (FUN_14125ce90).
    uint8_t* Add();
    // The `index`th element in use returned to the free list (FUN_14125cfd0).
    void Remove(uint32_t index);
    uint8_t* Get(uint32_t index) const {
        return this->m_storage + this->m_live.m_data[index] * this->m_stride;
    }

    const CParticleTraits* m_traits;            // 0x08
    M2ParticleHeaps* m_heaps;                   // 0x10
    M2SceneVector<uint32_t> m_live;             // 0x18
    M2SceneVector<uint32_t> m_free;             // 0x30
    uint8_t* m_storage;                         // 0x48
    uint32_t m_stride;                          // 0x50
    uint32_t m_count;                           // 0x54
    uint64_t m_capacity;                        // 0x58
};
static_assert(sizeof(CParticleList) == 0x60, "CParticleList is 0x60 bytes");

// The generator: the emitter's shape, its random stream and the animated emission values. The
// virtuals are the emission rate (FUN_14125d600), the per-frame update (FUN_14125d770) and a
// particle made (the shape's own).
class CParticleGenerator {
    public:
    CParticleGenerator(CRndSeed* random, const float* variation);
    virtual ~CParticleGenerator();
    virtual float GetEmissionRate();
    virtual void Update(float delta, C44Matrix* matrix);
    virtual void CreateParticle(CParticle2* particle, float delta) = 0;

    // A speed (FUN_14125d5a0), and the life, age and seed every shape starts a particle with
    // (FUN_14125d660).
    float RandomSpeed();
    void InitParticle(CParticle2* particle, float delta);

    CRndSeed* m_random;                 // 0x08 the emitter's
    float m_emissionRateVariation;      // 0x10
    float m_lifespanVariation;          // 0x14
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
    float m_emissionRateScale;          // 0x48 1
    float m_lifespanScale;              // 0x4c 1
    float m_maxLifespan;                // 0x50 the lifespan times its scale, this frame
    float m_54;                         // 0x54 1, scales the emission rate
    int32_t m_kind;                     // 0x58 0 sphere, 1 plane, 2 box, 3 bone, 4 base
    uint32_t m_pad5c;
};
static_assert(offsetof(CParticleGenerator, m_speed) == 0x18, "CParticleGenerator speed at 0x18");
static_assert(sizeof(CParticleGenerator) == 0x60, "CParticleGenerator base is 0x60 bytes");

// A rectangle in the emitter's plane, its length along x and width along y (vtable 0x142724f68).
class CPlaneParticleGenerator : public CParticleGenerator {
    public:
    CPlaneParticleGenerator(CRndSeed* random, const float* variation);
    ~CPlaneParticleGenerator() override;
    void CreateParticle(CParticle2* particle, float delta) override;
};

// A shell between two radii (vtable 0x142724f88); `m_upward` sends every particle up the z axis.
class CSphereParticleGenerator : public CParticleGenerator {
    public:
    CSphereParticleGenerator(CRndSeed* random, const float* variation, bool upward);
    ~CSphereParticleGenerator() override;
    void CreateParticle(CParticle2* particle, float delta) override;

    bool m_upward;                      // 0x60
    uint8_t m_pad61[7];
};
static_assert(sizeof(CSphereParticleGenerator) == 0x68, "CSphereParticleGenerator is 0x68 bytes");

// Along a spline through the file's points (vtable 0x142725018), the spline (NTempest's
// CSpline, vtable 0x142724fa8) inside it.
class CSplineParticleGenerator : public CParticleGenerator {
    public:
    CSplineParticleGenerator(CRndSeed* random, const float* variation, uint16_t count, const C3Vector* points);
    ~CSplineParticleGenerator() override;
    float GetEmissionRate() override;
    void CreateParticle(CParticle2* particle, float delta) override;

    float m_position;                   // 0x60 where along the spline the last particle was made
    uint32_t m_pad64;
    void* m_splineVtable;               // 0x68
    uint8_t m_spline[0x228 - 0x70];     // 0x70
};
static_assert(sizeof(CSplineParticleGenerator) == 0x228, "CSplineParticleGenerator is 0x228 bytes");

// From a model's bones (vtables 0x142725058, and 0x142725078 for the mirrored one, which
// spreads particles between a bone and its parent).
class CBoneParticleGenerator : public CParticleGenerator {
    public:
    CBoneParticleGenerator(CRndSeed* random, const float* variation);
    ~CBoneParticleGenerator() override;
    void Update(float delta, C44Matrix* matrix) override;
    void CreateParticle(CParticle2* particle, float delta) override;

    CM2Model* m_model;                  // 0x60
    float m_68[16];                     // 0x68
};
static_assert(sizeof(CBoneParticleGenerator) == 0xa8, "CBoneParticleGenerator is 0xa8 bytes");

class CMirroredBoneParticleGenerator : public CBoneParticleGenerator {
    public:
    CMirroredBoneParticleGenerator(CRndSeed* random, const float* variation);
    void CreateParticle(CParticle2* particle, float delta) override;
};

// From a model's mesh (vtable 0x142725098), the emitter kind 4 a model created with flag 0x800
// gets.
class CMeshParticleGenerator : public CParticleGenerator {
    public:
    CMeshParticleGenerator(CRndSeed* random, const float* variation);
    ~CMeshParticleGenerator() override;
    void CreateParticle(CParticle2* particle, float delta) override;

    uint32_t m_60;                      // 0x60
    uint32_t m_64;                      // 0x64
    uint32_t m_68;                      // 0x68
    uint32_t m_6c;                      // 0x6c 2
    uint64_t m_70[4];                   // 0x70
    uint32_t m_90;                      // 0x90 low four bits cleared
    uint32_t m_pad94;
};
static_assert(sizeof(CMeshParticleGenerator) == 0x98, "CMeshParticleGenerator is 0x98 bytes");

// The fast path's per-emitter keys (CParticleKeySimple, 0x40 plus its heap index), two for each
// emitter whose tracks are simple enough: before and after the midpoint, in 16.16 fixed point.
struct CParticleKeySimple {
    int32_t m_color[3];                 // 0x00 b, g, r << 16
    int32_t m_alpha;                    // 0x0c << 16
    float m_scale;                      // 0x10
    int32_t m_headCell;                 // 0x14 << 16
    int32_t m_tailCell;                 // 0x18 << 16
    uint32_t m_1c;
    int32_t m_colorDelta[3];            // 0x20
    int32_t m_alphaDelta;               // 0x2c
    float m_scaleDelta;                 // 0x30
    int32_t m_headCellDelta;            // 0x34
    int32_t m_tailCellDelta;            // 0x38
    uint32_t m_3c;
    uint32_t m_heapIndex;               // 0x40
};
static_assert(sizeof(CParticleKeySimple) == 0x44, "CParticleKeySimple is 0x44 bytes");

class CParticleEmitter2 {
    public:
    uint32_t m_00;                      // 0x00 1
    float m_emission;                   // 0x04 particles owed, carried between updates
    int32_t m_cellShift;                // 0x08 log2 of the tile columns
    float m_cellWidth;                  // 0x0c
    float m_cellHeight;                 // 0x10
    int32_t m_tileRotation;             // 0x14
    CParticleGenerator* m_generator;    // 0x18
    CParticleList m_list;               // 0x20
    int32_t m_vertexFormat;             // 0x80 the draw's vertex format kind
    uint32_t m_pad84;
    const C3Vector* m_pathSegments;     // 0x88 pairs of points particles start between
    const float* m_pathPoints;          // 0x90 points 0x10 apart
    uint32_t m_pathCount;               // 0x98
    int32_t m_pathMode;                 // 0x9c 3 along the path, 4 against it, 5 kept
    float m_viewFade;                   // 0xa0 how far the camera looks down, 0..1
    uint32_t m_drawnCount;              // 0xa4 the particles the last render wrote
    CRndSeed m_random;                  // 0xa8 the emitter's random stream
    uint32_t m_childCount;              // 0xb0
    uint32_t m_padb4;
    CParticleEmitter2* m_children[4];   // 0xb8 a recursive model's emitters
    CM2Model* m_model;                  // 0xd8 the geometry particles' model
    CM2Scene* m_scene;                  // 0xe0
    CM2Model* m_recursionModel;         // 0xe8
    uint32_t m_verticesPerParticle;     // 0xf0
    uint32_t m_indicesPerParticle;      // 0xf4
    uint8_t m_f8[8];
    int32_t m_type;                     // 0x100 0 plain, 1 models, 2/3 multi-texture, 4 projected
    float m_tailLength;                 // 0x104
    float m_baseSpin;                   // 0x108
    float m_baseSpinVary;               // 0x10c
    float m_spin;                       // 0x110
    float m_spinVary;                   // 0x114
    float m_scale;                      // 0x118
    uint8_t m_blend;                    // 0x11c the M2 blend mode
    uint8_t m_renderFlags;              // 0x11d 1 lit, 2 no fog, 4 no depth test, 8 sized by area, 0x20 ...
    uint8_t m_11e[2];
    const M2PartTrack<C3Vector>* m_colorTrack;      // 0x120
    const M2PartTrack<uint16_t>* m_alphaTrack;      // 0x128
    const M2PartTrack<C2Vector>* m_scaleTrack;      // 0x130
    C2Vector m_scaleVary;                           // 0x138
    const M2PartTrack<uint16_t>* m_headCellTrack;   // 0x140
    const M2PartTrack<uint16_t>* m_tailCellTrack;   // 0x148
    // The colour overrides (flag 0x10), start, middle and end, r g b 0..255.
    float m_colors[3][3];               // 0x150
    float m_midpoint;                   // 0x174 where the fast path's two keys meet
    CParticleKeySimple* m_fastKeys[2];  // 0x178
    float m_uvScale[2];                 // 0x188 each texture layer's repeat
    float m_uvSpeed[4];                 // 0x190 the layers' scroll
    float m_uvSpeedVary[4];             // 0x1a0
    uint32_t m_rows;                    // 0x1b0
    uint32_t m_columns;                 // 0x1b4
    uint16_t m_startCell;               // 0x1b8
    uint16_t m_cellMask;                // 0x1ba
    uint32_t m_pad1bc;
    CTexture* m_textures[3];            // 0x1c0
    uint32_t m_material;                // 0x1d8 the textures' hash
    float m_alpha;                      // 0x1dc
    // 1 emitting, 2 enabled, 4 head quads, 8 tail quads, 0x10 colour overrides, 0x20 ...,
    // 0x40 burst, 0x80 updated, 0x100 skipped, 0x200 particles in the emitter's space, 0x400
    // scaled with it, 0x800 follow, 0x1000 clip to the emitter's plane, 0x2000 no interpolation,
    // 0x4000 world oriented, 0x8000 driven by the model, 0x10000 random flips, 0x20000 tail by
    // age, 0x40000 snap to the ground, 0x80000 move with the emitter, 0x100000 random tiles,
    // 0x200000 along the velocity, 0x400000 no density, 0x800000 separate scale variation,
    // 0x1000000 fast path, 0x8000000 has moved, 0x10000000 full rate, 0x20000000 offset by the
    // spin, 0x40000000 world wind, 0x80000000 the file's wind
    uint32_t m_flags;                   // 0x1e0
    float m_twinkleSpeed;               // 0x1e4
    float m_twinklePercent;             // 0x1e8
    float m_twinkleScale[2];            // 0x1ec min, range
    float m_burstMultiplier;            // 0x1f4
    float m_tumble[6];                  // 0x1f8 min x, range x, min y, range y, min z, range z
    float m_drag;                       // 0x210
    C3Vector m_wind;                    // 0x214
    float m_windTime;                   // 0x220
    float m_followBase;                 // 0x224
    float m_followSlope;                // 0x228
    uint32_t m_pad22c;
    float m_parentMatrix[16];           // 0x230 its translation (0x260) is the emitter's position
    C3Vector m_origin;                  // 0x270 the camera's
    C3Vector m_previousPosition;        // 0x27c
    float m_followTime;                 // 0x288
    C3Vector m_direction;               // 0x28c
    float m_parentScale;                // 0x298
    uint32_t m_pad29c;
    C3Vector m_motion;                  // 0x2a0 since the last update
    C3Vector m_stepMotion;              // 0x2ac each step's share
    C3Vector m_cameraAxis;              // 0x2b8
    float m_bounds[6];                  // 0x2c4 min, max
    uint8_t m_2dc;                      // 0x2dc passed to the world's wind
    uint8_t m_pad2dd[3];
    const uint16_t* m_combiner;         // 0x2e0 the file's particle combiner
    const M2ExtendedParticle* m_extended;  // 0x2e8 EXP2
    int32_t m_prebuiltOffset;           // 0x2f0 the vertices made at update in the scene's pool
    int32_t m_prebuiltSize;             // 0x2f4
};
static_assert(offsetof(CParticleEmitter2, m_list) == 0x20, "CParticleEmitter2 list at 0x20");
static_assert(offsetof(CParticleEmitter2, m_random) == 0xa8, "CParticleEmitter2 random at 0xa8");
static_assert(offsetof(CParticleEmitter2, m_baseSpin) == 0x108, "CParticleEmitter2 spin at 0x108");
static_assert(offsetof(CParticleEmitter2, m_colorTrack) == 0x120, "CParticleEmitter2 tracks at 0x120");
static_assert(offsetof(CParticleEmitter2, m_fastKeys) == 0x178, "CParticleEmitter2 fast keys at 0x178");
static_assert(offsetof(CParticleEmitter2, m_textures) == 0x1c0, "CParticleEmitter2 textures at 0x1c0");
static_assert(offsetof(CParticleEmitter2, m_flags) == 0x1e0, "CParticleEmitter2 flags at 0x1e0");
static_assert(offsetof(CParticleEmitter2, m_vertexFormat) == 0x80, "CParticleEmitter2 format at 0x80");
static_assert(offsetof(CParticleEmitter2, m_type) == 0x100, "CParticleEmitter2 type at 0x100");
static_assert(offsetof(CParticleEmitter2, m_blend) == 0x11c, "CParticleEmitter2 blend at 0x11c");
static_assert(offsetof(CParticleEmitter2, m_material) == 0x1d8, "CParticleEmitter2 material at 0x1d8");
static_assert(offsetof(CParticleEmitter2, m_wind) == 0x214, "CParticleEmitter2 wind at 0x214");
static_assert(offsetof(CParticleEmitter2, m_parentMatrix) == 0x230, "CParticleEmitter2 matrix at 0x230");
static_assert(offsetof(CParticleEmitter2, m_bounds) == 0x2c4, "CParticleEmitter2 bounds at 0x2c4");
static_assert(offsetof(CParticleEmitter2, m_combiner) == 0x2e0, "CParticleEmitter2 combiner at 0x2e0");
static_assert(sizeof(CParticleEmitter2) == 0x2f8, "CParticleEmitter2 is 0x2f8 bytes");

// The matrices a render or a prebuild works in (passed to the vertex writers): the particles'
// space to the view, the view's z axis, and its 3x3 for world-oriented quads.
struct ParticleSpace {
    float* m_matrix;                    // 4x4
    float* m_normal;                    // 3
    float* m_rotation;                  // 3x3
};

// A particle readied for drawing (the writers' input): where it is in view space, its colour,
// size, cells and the extended alpha cutoff.
struct ParticleDrawState {
    C3Vector m_position;                // 0x00
    uint8_t m_color[4];                 // 0x0c b, g, r, a
    float m_scale[2];                   // 0x10
    int32_t m_headCell;                 // 0x18
    int32_t m_tailCell;                 // 0x1c
    float m_alphaCutoff;                // 0x20
    float m_24[4];
    float m_inverseSize;                // 0x34 1, or over its area with render flag 8
};
static_assert(sizeof(ParticleDrawState) == 0x38, "ParticleDrawState is 0x38 bytes");

// The particle effects by vertex format kind (DAT_1430a1ee8: unlit T1, lit T1, 2-colour-texture,
// two 3-colour-texture forms and refraction), looked up when the device comes up (FUN_14124aa50).
extern CShaderEffect* g_particleEffects[6];
// The quads' shared index buffer (DAT_1430a1cd0).
extern GxBuf* g_particleIndices;
// Whether updates write the particles' vertices into the scene's pool for the render to copy
// (DAT_142cfbb30, set by FUN_14124e2c0).
extern uint8_t g_particlePrebuild;
// Particles drawn at all (DAT_142cfbb34) and the two densities (DAT_142cfbb38, DAT_142cfbb3c),
// set from the graphics settings.
extern uint32_t g_particleDensityEnabled;
extern float g_particleDensity;
extern float g_particleMTDensity;

// Two emitters' draw order: blend, render flags, type, then their three textures (FUN_14124f4c0).
int32_t ParticleEmitterCompare(const CParticleEmitter2* left, const CParticleEmitter2* right);
// The emitter's draw: whether its textures are loaded (FUN_14124cc90); its textures and
// constants (FUN_14124f2c0); its particles into `vertices`, or drawn on their own into a
// transient buffer when null (FUN_14124d0a0); whether two emitters can share a draw
// (FUN_14124ca70); its flipbook's texture matrix (FUN_14124f140); the outline map for model
// particles (FUN_14124f440); and a merged run's draw (FUN_14124d790).
bool ParticleEmitterTexturesReady(const CParticleEmitter2* emitter);
void ParticleEmitterSetConstants(CParticleEmitter2* emitter, GxRenderContext* context, ShaderEffectContext* shader,
    const float* environment);
void ParticleEmitterRender(CParticleEmitter2* emitter, GxRenderContext* context, const C44Matrix* view,
    const C44Matrix* matrix, uint8_t* vertices, M2ParticlePool* pool);
bool ParticleEmitterBatchable(const CParticleEmitter2* emitter, const CParticleEmitter2* other);
void ParticleEmitterSetTextureMatrix(const CParticleEmitter2* emitter, ShaderEffectContext* shader);
void ParticleEmitterBindOutline(const CParticleEmitter2* emitter, GxRenderContext* context, const uint8_t* outline);
void ParticleEmitterDrawBatch(const CParticleEmitter2* emitter, GxRenderContext* context, GxBuf* vertices,
    uint32_t vertexCount, uint32_t indexCount, uint8_t shadingRate);
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
CParticleGenerator* ParticleGeneratorCreateBoneMirrored(void* memory, void* random, const float* params);
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
// The emitter's frame: its matrices, the generator, the particles' update and, when the
// vertices are prebuilt, their vertices into the scene's pool (FUN_14124f660).
void ParticleEmitterUpdate(CParticleEmitter2* emitter, float delta, const float* matrix, const float* camera,
    const float* attachment, M2ParticlePool* pool);
// The models carried by the emitter's particles and its children's: how many (FUN_14124b8e0) and
// the one at `index`, which counts down through the children (FUN_14124b940).
uint32_t ParticleEmitterModelCount(const CParticleEmitter2* emitter);
CM2Model* ParticleEmitterGetModel(const CParticleEmitter2* emitter, uint32_t* index);

// The particle helpers the update and the draw share: the twinkle table (DAT_1430a1ce0, 128
// randoms in 0..1), a key's interpolation (FUN_14124b750), the colour track or overrides as b g r a
// bytes (FUN_14124c730), a 2-vector track (FUN_14124c8a0), a fixed 16 track as a float
// (FUN_14124c950) and as an integer (FUN_14124c9f0), the matrices a write works in
// (FUN_14124dc00), the vertices of `count` particles by the emitter's format (the loops at
// FUN_141245bf0, FUN_1412463a0, FUN_141246be0, FUN_141247420), and a quaternion normalised
// unless it is about zero (FUN_14124dfe0).
extern float g_particleTwinkle[0x80];
float ParticleTrackKey(const M2Array<uint16_t>* times, float t, uint32_t* first, uint32_t* second);
void ParticleTrackColor(const CParticleEmitter2* emitter, float t, uint8_t* out);
void ParticleTrackVector2(const M2PartTrack<C2Vector>* track, float t, float* out);
float ParticleTrackFloat(const M2PartTrack<uint16_t>* track, float t);
uint32_t ParticleTrackCell(const M2PartTrack<uint16_t>* track, float t);
void ParticleEmitterSetSpace(CParticleEmitter2* emitter, const float* attachment, const float* view, ParticleSpace* space);
void ParticleEmitterWrite(CParticleEmitter2* emitter, uint8_t* vertices, uint32_t count, const float* bounds,
    ParticleSpace* space);
void QuaternionNormalize(float* q);
// The device's index buffer gone (LAB_14124ac20).
void ParticleSystemOnDeviceDestroyed(void* param);

// The particle system's start: the twinkle table and the device callbacks (FUN_14124bdc0), and
// its end (FUN_14124ab20); the pools (FUN_14124c270) and their release (FUN_14124aba0); the
// index buffer and effects made with the device (FUN_14124aa50).
void ParticleSystemInitialize();
void ParticleSystemShutdown();
void ParticleSystemCreatePools();
void ParticleSystemDestroyPools();
void ParticleSystemOnDeviceCreated(void* param);
// The vertex writers' pool released (FUN_14124ab60).
int32_t M2ParticlePoolRelease(M2ParticlePool* pool);
// Updates prebuild the vertices (FUN_14124e2c0).
void ParticleSetPrebuild();

#endif
