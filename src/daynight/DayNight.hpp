#ifndef DAYNIGHT_DAY_NIGHT_HPP
#define DAYNIGHT_DAY_NIGHT_HPP

// Engine/Source/DayNight: the world's sky and light. A world scene that has a map makes one
// (DayNight.cpp:0x66, a 0x20-byte handle around a 0x2068-byte DNScene); it holds the light state
// the scene's fog and lights are computed into, the clouds, the sun and moon glare queries, the
// four planets (sun and moons), the sky dome, the sky model and the colour grading, and a
// CM2Scene of its own for the sky models.
//
// Most layouts are known only from their constructors and are reached through Store/Field at
// the reference offsets; members are named where a reader says what they hold.

#include "blz/Vector.hpp"
#include "storm/array/TSFixedArray.hpp"
#include "storm/list/TSLink.hpp"
#include "world/ModelFog.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

class CM2Model;
class CM2Scene;
class CWorldScene;
class CWorldView;
class GxPipeline;
struct DNScene;

extern const char s_dnTagU8[];          // ".?AV?$TSFixedArray@E$07@@"
extern const char s_dnTagFloat[];       // ".?AV?$TSFixedArray@M$07@@"
extern const char s_dnTagC3Vector[];    // ".?AV?$TSFixedArray@VC3Vector@@$07@@"
extern const char s_dnTagC2Vector[];    // ".?AV?$TSFixedArray@VC2Vector@@$07@@"
extern const char s_dnTagArgb[];        // ".?AV?$TSFixedArray@VCArgb@@$07@@"
extern const char s_dnTagU16[];         // ".?AV?$TSFixedArray@G$07@@"
extern const char s_dnTagArgbVector[];  // ".?AVCArgb@@"
extern const char s_dnTagTexPtr[];      // ".?AV?$unique_ptr@UGxTex@@UGxResourceDelete@@@blz@@"
extern const char s_dnTagInt[];         // ".H"

struct DNVEC2 {
    float x;
    float y;
};

struct DNVEC3 {
    float x;
    float y;
    float z;
};

template <class T>
inline void DNStore(void* base, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(T));
}

template <class T>
inline T& DNField(void* base, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// ---- the light state (0x1600 at the scene's +0) --------------------------------------------

struct DNLIGHT {
    uint8_t m_data[0x1600];

    // ref: FUN_1424549d0
    DNLIGHT();
    // ref: FUN_142458d10
    ~DNLIGHT();

    template <class T>
    T& Field(size_t offset) {
        return DNField<T>(this, offset);
    }

    // ref: FUN_142454eb0
    void Reset();
    // A map's light state: reset, the map id, and marked to be computed.
    // ref: FUN_142454fc0
    void SetMap(int32_t mapId);
};
static_assert(sizeof(DNLIGHT) == 0x1600, "DNLIGHT is 0x1600 bytes");

// The current light's block (0x4f0, FUN_142441dc0), two of them in the light state.
// ref: FUN_142441dc0
void DNCurrentLightInit(uint8_t* light);
// A light's 0x104 zeroed block (FUN_14244f2e0).
// ref: FUN_14244f2e0
void DNLightParamsInit(uint8_t* params);

// ---- the clouds (0x108 at the scene's +0x1620, DNClouds) -----------------------------------

struct DNCLOUDS {
    float m_00;                     // 0x00 1, then 0.96
    uint8_t m_falloff;              // 0x04 the density as a byte
    uint8_t m_05;                   // 0x05 100 when the density reaches full
    uint8_t m_quality;              // 0x06 the grid size index
    uint8_t m_dirty;                // 0x07
    uint8_t m_08;
    uint8_t m_pad09;
    uint16_t m_0a;                  // 0x0a
    uint8_t m_0c[4];
    float m_10;                     // 0x10 0.002
    int32_t m_cells;                // 0x14 8
    int32_t m_18;                   // 0x18 -1
    uint32_t m_1c;                  // 0x1c
    uint32_t m_20;                  // 0x20
    uint8_t m_24;                   // 0x24 2
    uint8_t m_pad25[3];
    int32_t m_gridSize;             // 0x28 128 << quality
    int32_t m_gridLog2;             // 0x2c
    int32_t m_gridMask;             // 0x30
    int32_t m_34;                   // 0x34 4
    int32_t m_enabled;              // 0x38
    uint32_t m_pad3c;
    blz::vector<void*, s_dnTagTexPtr> m_textures;           // 0x40 GxTex
    blz::vector<uint32_t, s_dnTagArgbVector> m_colors;      // 0x58 CArgb
    TSFixedArray<uint8_t, s_dnTagU8> m_density;             // 0x70
    TSFixedArray<float, s_dnTagFloat> m_88;                 // 0x88
    TSFixedArray<DNVEC3, s_dnTagC3Vector> m_positions;      // 0xa0
    TSFixedArray<DNVEC2, s_dnTagC2Vector> m_texCoords;      // 0xb8
    TSFixedArray<uint32_t, s_dnTagArgb> m_vertexColors;     // 0xd0 CArgb
    TSFixedArray<uint16_t, s_dnTagU16> m_indices;           // 0xe8
    uint16_t m_indexCount;          // 0x100
    uint16_t m_vertexCount;         // 0x102
    uint32_t m_104;                 // 0x104

    // ref: FUN_142450700
    DNCLOUDS();
    // ref: FUN_1424507f0
    ~DNCLOUDS();
    // The noise tables (once), the device objects (first user), the grid and the dome.
    // ref: FUN_142451030
    void Initialize();
    // ref: FUN_142452b80
    void BuildGrid();
    // The cloud dome's rings at `radius`.
    // ref: FUN_142450aa0
    void BuildDome(float radius);
    // ref: FUN_142451380
    void DestroyTextures();
    // The last user's release of the shared pipelines.
    // ref: FUN_142450a00
    void Release();
};
static_assert(offsetof(DNCLOUDS, m_textures) == 0x40, "DNCLOUDS layout");
static_assert(offsetof(DNCLOUDS, m_density) == 0x70, "DNCLOUDS layout");
static_assert(offsetof(DNCLOUDS, m_indices) == 0xe8, "DNCLOUDS layout");
static_assert(sizeof(DNCLOUDS) == 0x108, "DNCLOUDS is 0x108 bytes");

// ---- the glare queries (0xd8 at +0x1728 and +0x1800) ---------------------------------------

// vtable 0x142979a30 and its two derivations (0x142979a60 sun, 0x142979a98 moon): an occlusion
// query over the body's disc, and how much of it the clouds cover.
class DNGlare {
    public:
    // ref: FUN_142453920
    virtual ~DNGlare();
    // ref: FUN_142454630
    virtual void Update(DNScene* scene);
    // ref: FUN_142454590
    virtual void Slot10();
    // ref: FUN_142453d00
    virtual uint32_t Visibility();
    // ref: FUN_142453ce0
    virtual uint8_t TextureLoaded();
    // ref: FUN_142454620
    virtual uint64_t SetTexturePriority(int32_t priority);
    virtual float CloudCover(DNScene* scene) = 0;

    void* m_query;                  // 0x08
    uint8_t m_10[0x18];             // 0x10 the query's results
    void* m_texture;                // 0x28 HTEXTURE
    uint8_t m_30[0xa8];

    template <class T>
    T& Field(size_t offset) {
        return DNField<T>(this, offset);
    }

    // The query, once the device can make one.
    // ref: FUN_1424539c0
    void CreateQuery();
    // ref: FUN_142453b20
    void DestroyQuery();
    // The texture, the shared pipeline's first user.
    // ref: FUN_142453c00
    void Initialize(int32_t fileDataId, void* glareColor);
    // ref: FUN_142453ac0
    void Release();
};
static_assert(sizeof(DNGlare) == 0xd8, "DNGlare is 0xd8 bytes");

class DNSunGlare : public DNGlare {
    public:
    // ref: FUN_142453800
    DNSunGlare();
    // ref: FUN_142453bc0
    float CloudCover(DNScene* scene) override;
};

class DNMoonGlare : public DNGlare {
    public:
    // ref: FUN_142453720
    DNMoonGlare();
    // ref: FUN_142453b60
    float CloudCover(DNScene* scene) override;
};

// ---- the planets (0xa8, four at +0x18d8) ---------------------------------------------------

struct DNPLANET {
    uint8_t m_data[0xa8];

    // ref: FUN_142455150
    DNPLANET();

    template <class T>
    T& Field(size_t offset) {
        return DNField<T>(this, offset);
    }

    // Its texture and the two scales.
    // ref: FUN_142455a70
    void Initialize(int32_t fileDataId, float scale, float scale2);
    // ref: FUN_142455680
    void Release();
};
static_assert(sizeof(DNPLANET) == 0xa8, "DNPLANET is 0xa8 bytes");

// ---- the sky dome (0x70 at +0x1b78) --------------------------------------------------------

struct DNSKY {
    TSFixedArray<DNVEC3, s_dnTagC3Vector> m_positions;      // 0x00
    TSFixedArray<uint32_t, s_dnTagArgb> m_colors;           // 0x18
    TSFixedArray<uint32_t, s_dnTagArgb> m_colors2;          // 0x30
    TSFixedArray<uint16_t, s_dnTagU16> m_indices;           // 0x48
    int32_t m_rings;                // 0x60 24
    uint16_t m_vertexCount;         // 0x64
    uint16_t m_indexCount;          // 0x66
    uint8_t m_pad68[8];

    // ref: FUN_142456b70
    DNSKY();
    // ref: FUN_142456ba0
    ~DNSKY();
    // ref: FUN_1424571d0
    void Initialize();
    // ref: FUN_142456e50
    void Build(float radius);
    // ref: FUN_142456d80
    void Release();
};
static_assert(sizeof(DNSKY) == 0x70, "DNSKY is 0x70 bytes");

// ---- the colour grading (0x128, DNInt.cpp:0x40) --------------------------------------------

struct DNColorGrading {
    uint8_t m_data[0x128];

    // ref: FUN_14244f3b0
    DNColorGrading();
    // ref: FUN_14244f5f0
    ~DNColorGrading();

    // ref: FUN_14244fdb0
    void CreateTargets();
};
static_assert(sizeof(DNColorGrading) == 0x128, "DNColorGrading is 0x128 bytes");

// ---- the scene (0x2068) --------------------------------------------------------------------

struct DNAREALIGHTOVERRIDE {
    uint8_t m_data[0x28];
};

extern const char s_dnTagAreaOverride[];    // ".?AV?$TSFixedArray@UAreaLightOverride@DayNight@@$07@@"
extern const char s_dnTagZoneLight[];       // ".?AVCZoneLight@DayNight@@"
extern const char s_dnTagPairIntFloat[];    // ".?AU?$pair@HM@blz@@"

struct DNScene {
    DNLIGHT m_light;                                        // 0x0000
    uint8_t m_loaded;                                       // 0x1600 a map is set
    uint8_t m_1601;
    uint8_t m_1602;                                         // 0x1602 1
    uint8_t m_1603;                                         // 0x1603 1
    uint32_t m_pad1604;
    uint8_t* m_zoneLights;                                  // 0x1608 blz::vector<CZoneLight>
    uint64_t m_zoneLightCount;                              // 0x1610
    uint64_t m_zoneLightCapacity;                           // 0x1618
    DNCLOUDS m_clouds;                                      // 0x1620
    DNSunGlare m_sunGlare;                                  // 0x1728
    DNMoonGlare m_moonGlare;                                // 0x1800
    DNPLANET m_planets[4];                                  // 0x18d8
    DNSKY m_sky;                                            // 0x1b78
    CM2Model* m_skyModel;                                   // 0x1be8
    uint32_t m_1bf0;                                        // 0x1bf0
    uint32_t m_pad1bf4;
    uint8_t m_1bf8;                                         // 0x1bf8
    uint8_t m_1bf9[0x43];                                   // 0x1bf9
    float m_1c3c;                                           // 0x1c3c 8000
    float m_1c40;                                           // 0x1c40 8000
    uint32_t m_pad1c44;
    uint64_t m_overrideBuckets;                             // 0x1c48 8: blz::unordered_map<string, DNOverrideSky*>
    void** m_overrideTable;                                 // 0x1c50
    uint64_t m_overrideCount;                               // 0x1c58
    float m_overrideLoad;                                   // 0x1c60 1
    uint32_t m_pad1c64;
    TSFixedArray<DNAREALIGHTOVERRIDE, s_dnTagAreaOverride> m_areaOverrides;    // 0x1c68
    uint32_t m_1c80;                                        // 0x1c80
    uint32_t m_pad1c84;
    blz::vector<int32_t, s_dnTagInt> m_areaLights[0x18];    // 0x1c88 light ids per area slot
    uint8_t m_1ec8;                                         // 0x1ec8
    uint8_t m_1ec9[3];
    uint8_t m_1ecc[0x24];                                   // 0x1ecc
    CWorldScene* m_worldScene;                              // 0x1ef0
    CM2Scene* m_m2Scene;                                    // 0x1ef8 the sky models'
    DNColorGrading* m_colorGrading;                         // 0x1f00
    uint32_t m_time;                                        // 0x1f08
    uint32_t m_pad1f0c;
    uint64_t* m_blends;                                     // 0x1f10 embedded_vector<pair<int, float>, 16>
    uint64_t m_blendCount;                                  // 0x1f18
    uint64_t m_blendCapacity;                               // 0x1f20
    uint64_t m_blendInline[0x10];                           // 0x1f28
    uint64_t* m_blends2;                                    // 0x1fa8
    uint64_t m_blendCount2;                                 // 0x1fb0
    uint64_t m_blendCapacity2;                              // 0x1fb8
    uint64_t m_blendInline2[0x10];                          // 0x1fc0
    uint64_t m_2040[4];                                     // 0x2040
    uint32_t m_2060;                                        // 0x2060
    uint32_t m_pad2064;

    // ref: FUN_1424587b0
    DNScene(CWorldScene* worldScene, CWorldView* view);
    // ref: FUN_142458b20
    ~DNScene();

    // The glare queries and the planet registry, once the device is up.
    // ref: FUN_142458e70
    void CreateDeviceObjects();
    // ref: FUN_142458ed0
    void DestroyDeviceObjects();
    // The map's sky: drops the last one, then the light state, the area light lists, the
    // clouds, the glare and planet textures and the sky model.
    // ref: FUN_142442fe0 (through the handle)
    void SetMap(int32_t mapId);
    // ref: FUN_142443a40
    void UnloadMap();
};
static_assert(offsetof(DNScene, m_loaded) == 0x1600, "DNScene layout");
static_assert(offsetof(DNScene, m_clouds) == 0x1620, "DNScene layout");
static_assert(offsetof(DNScene, m_sunGlare) == 0x1728, "DNScene layout");
static_assert(offsetof(DNScene, m_moonGlare) == 0x1800, "DNScene layout");
static_assert(offsetof(DNScene, m_planets) == 0x18d8, "DNScene layout");
static_assert(offsetof(DNScene, m_sky) == 0x1b78, "DNScene layout");
static_assert(offsetof(DNScene, m_skyModel) == 0x1be8, "DNScene layout");
static_assert(offsetof(DNScene, m_1c3c) == 0x1c3c, "DNScene layout");
static_assert(offsetof(DNScene, m_overrideBuckets) == 0x1c48, "DNScene layout");
static_assert(offsetof(DNScene, m_areaOverrides) == 0x1c68, "DNScene layout");
static_assert(offsetof(DNScene, m_areaLights) == 0x1c88, "DNScene layout");
static_assert(offsetof(DNScene, m_worldScene) == 0x1ef0, "DNScene layout");
static_assert(offsetof(DNScene, m_blends) == 0x1f10, "DNScene layout");
static_assert(offsetof(DNScene, m_blends2) == 0x1fa8, "DNScene layout");
static_assert(offsetof(DNScene, m_2040) == 0x2040, "DNScene layout");
static_assert(sizeof(DNScene) == 0x2068, "DNScene is 0x2068 bytes");

// ---- the handle (0x20) ---------------------------------------------------------------------

struct DAYNIGHT {
    DNScene* m_scene;               // 0x00
    uint32_t m_08;                  // 0x08
    uint8_t m_0c;                   // 0x0c set while the update runs on a job
    uint8_t m_pad0d[3];
    TSLink<DAYNIGHT> m_link;        // 0x10 every handle (DAT_142d4ca20)
};
static_assert(sizeof(DAYNIGHT) == 0x20, "DAYNIGHT is 0x20 bytes");

// ref: FUN_142442100
DAYNIGHT* DayNightCreate(CWorldScene* scene, CWorldView* view);
// ref: FUN_142442370
void DayNightDestroy(DAYNIGHT* dayNight);
// ref: FUN_142442fe0
void DayNightSetMap(DAYNIGHT* dayNight, int32_t mapId);
// The light ids of area slot `index`.
// ref: FUN_142443100
void DayNightSetArea(DAYNIGHT* dayNight, int32_t areaId, int32_t index);
// ref: FUN_142443200
void DayNightSetView(DAYNIGHT* dayNight, CWorldView* view, const float* position, const float* clip,
    const float* viewMatrix);

// The light ids of `areaId` (FUN_142453550): the Light rows on that continent with a position,
// and first the one without (or 1 when there is none).
// ref: FUN_142453550
void DayNightGatherLights(blz::vector<int32_t, s_dnTagInt>* lights, int32_t areaId);

// A celestial body row changed (the CelestialBody table's patch callback).
// ref: FUN_142443120
void DayNightOnCelestialBodyPatched(uint32_t a1, uint32_t id, uint32_t a3, void* a4, void* a5);

#endif
