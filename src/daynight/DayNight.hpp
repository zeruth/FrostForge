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
struct GxPipeline;
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

// ---- the zone lights ----------------------------------------------------------------------

// A ZoneLight row as the scene keeps it (0x48): its Light record and distance once selected,
// the zone's id and light, its bounds, and the fade it is on.
struct CZONELIGHT {
    const void* m_light;            // 0x00 the Light record, once selected
    float m_distance;               // 0x08 how far inside, once selected
    int32_t m_id;                   // 0x0c the ZoneLight id
    uint32_t m_lightId;             // 0x10
    int32_t m_14;                   // 0x14 the selection's first sort key (not written by the reference)
    float m_min[3];                 // 0x18
    float m_max[3];                 // 0x24
    float m_fadeFrom;               // 0x30
    float m_fadeTo;                 // 0x34
    float m_fade;                   // 0x38
    float m_fadeDuration;           // 0x3c
    float m_fadeStart;              // 0x40
    uint32_t m_44;
};
static_assert(sizeof(CZONELIGHT) == 0x48, "CZONELIGHT is 0x48 bytes");

extern const char s_dnTagZoneLight[];       // ".?AVCZoneLight@DayNight@@"
typedef blz::vector<CZONELIGHT, s_dnTagZoneLight> CZONELIGHTVECTOR;

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

extern const char s_dnTagHeightFog[];   // ".?AUHeightFogBlendPercent@CurrentLight@DayNight@@"
extern const char s_dnTagCurrentFog[];  // ".?AUCurrentFog@DayNight@@"
extern const char s_dnTagLightQE[];     // ".?AVLightQE@World@@"

// DayNight::CurrentLight (0x4f0): the light computed from a LightParams row at a time of day.
// Its words 0..0x17 are colours (0xAARRGGBB), the rest floats and flags read by the compute
// (DNCompute.cpp) at their reference offsets; the HeightFogBlendPercent vector ends it.
struct DNCURRENTLIGHT {
    uint8_t m_data[0x4d8];
    blz::vector<uint64_t, s_dnTagHeightFog> m_heightFog;    // 0x4d8

    DNCURRENTLIGHT() {
        DNCurrentLightInit(reinterpret_cast<uint8_t*>(this));
    }

    uint32_t& Word(size_t index) {
        return reinterpret_cast<uint32_t*>(this->m_data)[index];
    }

    float& Float(size_t index) {
        return reinterpret_cast<float*>(this->m_data)[index];
    }
};
static_assert(sizeof(DNCURRENTLIGHT) == 0x4f0, "DNCURRENTLIGHT is 0x4f0 bytes");

// DayNight::CurrentFog (0x68): the fog computed with a light; word 0x17's low byte is a flag
// and word 0x19 the weight it carries into the fog list.
struct DNCURRENTFOG {
    float m_values[0x1a];

    DNCURRENTFOG();

    float& operator[](size_t index) {
        return this->m_values[index];
    }
};
static_assert(sizeof(DNCURRENTFOG) == 0x68, "DNCURRENTFOG is 0x68 bytes");

typedef blz::vector<DNCURRENTFOG, s_dnTagCurrentFog> DNFOGVECTOR;

// World::LightQE: a Light row near the camera and its squared distance.
struct DNLIGHTQE {
    float m_distance;
    uint32_t m_pad04;
    const void* m_light;
};
typedef blz::vector<DNLIGHTQE, s_dnTagLightQE> DNLIGHTQEVECTOR;

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
    // The textures the colour grid goes into, made the first time, and the grid's next columns
    // uploaded; a finished pass flips the texture drawn from.
    // ref: FUN_142452f30
    void Upload(DNScene* scene);
    // The next rows of the cloud grid: four octaves of value noise per cell, its density
    // through the falloff and its colour lit from the sun (or moon) side; a full pass when the
    // grid is marked.
    // ref: FUN_142452140
    void Generate(DNScene* scene);
    // ref: FUN_142451e20
    void MarkDirty();
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
    // The dome's colours for the frame: the zenith, four bands of rings shaded round the
    // azimuth `azimuth` (radians), the horizon ring and the bottom, each with the fog colour
    // beside it; `tint` (0..255) pulls them toward `tintColor`.
    // ref: FUN_142457d40
    void Compute(DNCURRENTLIGHT* light, DNCURRENTFOG* fog, float time, float azimuth, uint8_t tint,
        uint32_t tintColor, float underwater, float opacity, float farClip);
};
static_assert(sizeof(DNSKY) == 0x70, "DNSKY is 0x70 bytes");

// A day band's value at `t` (0..1): keys of (time, value), interpolated round the day.
// ref: FUN_14244c4d0
float DNFloatBand(const float* keys, uint32_t count, float t);
// Two colours' channels interpolated (truncated), opaque.
// ref: FUN_14244c440
uint32_t DNColorLerp(const uint32_t* from, const uint32_t* to, float t);

// ---- the colour grading (0x128, DNInt.cpp:0x40) --------------------------------------------

struct DNColorGrading {
    uint8_t m_data[0x128];

    // ref: FUN_14244f3b0
    DNColorGrading();
    // ref: FUN_14244f5f0
    ~DNColorGrading();

    // ref: FUN_14244fdb0
    void CreateTargets();
    // A light's grading blocks rendered into the grading target `index` (0 or 1).
    // ref: FUN_142450110
    void Blend(void* target, uint8_t* block, int32_t index);
};
static_assert(sizeof(DNColorGrading) == 0x128, "DNColorGrading is 0x128 bytes");

// The gradings' device callbacks, registered by FUN_14244fd20 (FUN_14244f8b0, FUN_14244f9f0).
void DNColorGradingOnDeviceCreated(void* param);
void DNColorGradingOnDeviceDestroyed(void* param);

// ---- the scene (0x2068) --------------------------------------------------------------------

struct DNAREALIGHTOVERRIDE {
    uint8_t m_data[0x28];
};

extern const char s_dnTagAreaOverride[];    // ".?AV?$TSFixedArray@UAreaLightOverride@DayNight@@$07@@"
extern const char s_dnTagPairIntFloat[];    // ".?AU?$pair@HM@blz@@"

struct DNScene {
    DNLIGHT m_light;                                        // 0x0000
    uint8_t m_loaded;                                       // 0x1600 a map is set
    uint8_t m_1601;
    uint8_t m_1602;                                         // 0x1602 1
    uint8_t m_1603;                                         // 0x1603 1
    uint32_t m_pad1604;
    CZONELIGHTVECTOR m_zoneLights;                          // 0x1608 the map's
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
    // The map's zone lights from ZoneLight and ZoneLightPoint (when asked), then their fades.
    // ref: FUN_1424628c0
    void UpdateZoneLights(float now);
    // The zone lights the position is inside, nearest first, at most five.
    // ref: FUN_1424623b0
    void SelectZoneLights(const float* position);
    // ref: FUN_142454cf0
    void AddZoneLight(const void* light, float distance, const CZONELIGHT* zone);
    // Each frame on the world's animate pass (FUN_142443dc0 through the handle).
    // ref: FUN_14245e3f0
    void Update(const float* position);
    // After the animate pass: the colour grading for a view that asks (flag 0x40), and the
    // clouds' upload while the light is live.
    // ref: FUN_14245e340
    void PrepareFrame(void* env, bool colorGrading);
    // The frame's two current lights and fogs: from the area's Light rows, the zone lights,
    // the nearby point lights, the area overrides and the transition, or the defaults when
    // the area has no lights; then the sky overrides and the water's colour.
    // ref: FUN_1424597a0
    void ComputeLights(bool outdoors, bool zoneLightsSingly);
    // The scene's light colours and fog colours from the two current lights (and their
    // defaults when the map has no light).
    // ref: FUN_14245a940
    void StoreLightColors();
    // The fogs the scene draws with: one per CurrentFog in the list and the eight fixed ones,
    // eased toward the far clip, blended toward the water's while surfacing, and scaled.
    // ref: FUN_14245b980
    void BuildFogs();
    // The sun and moon positions round the camera, their glare strengths, the moon's phase and
    // the twilight factor, from the time of day.
    // ref: FUN_14245aca0
    void ComputeBodies();
    // The sun's direction (and the light state's copy), and the sun in view space at +0xa0.
    // ref: FUN_14245a6a0
    void ComputeSunDirection(const float* viewMatrix);
    // The grading blocks faded by the water (+0xa68) and blended with the override's (+0x618),
    // then rendered when asked.
    // ref: FUN_14245b7f0
    void ApplyColorGrading(void* env, bool fade, bool render);
    // The frame's light: the clocks, the area overrides' fades, the clouds' dirt, the camera's
    // heading, the lights and fogs, the bodies, the sun, the clouds and the sky model, the
    // fogs and the colour grading.
    // The camera the frame is computed for: the view (for the sky models' scene), the camera's
    // position and heading, the point the lights are measured from, the liquid it is in and
    // the far clip.
    // ref: FUN_142459720
    void SetCamera(CWorldView* view, const float* position, const float* heading, const float* target,
        int32_t liquid, float liquidDepth, float farClip);
    // ref: FUN_14245a8f0
    void SetWater(bool underwater, const void* fog, bool waterFog, float depth);
    // Raises the sky's loads until every body, glare, sky model and override sky is in.
    // ref: FUN_14245fb20
    void BeginFrame();
    // ref: FUN_14245f870
    bool SkyLoaded();
    // ref: FUN_14245fb50
    void SetSkyPriority(int32_t priority);
    // ref: FUN_14245b520
    void Compute(int32_t timeMs, float elapsed, float timeSeconds, bool alive, void* env, const float* viewMatrix,
        bool colorGrading, bool zoneLightsSingly);
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
// ref: FUN_142443dc0
void DayNightUpdate(DAYNIGHT* dayNight, const float* position);
// The frame's light through the handle (DNScene::Compute).
// ref: FUN_142443370
void DayNightSetCamera(DAYNIGHT* dayNight, CWorldView* view, const float* position, const float* heading,
    const float* target, int32_t liquid, float liquidDepth, float farClip);
// The weather's strength (+0x54 of the scene).
// ref: FUN_1424438e0
void DayNightSetWeather(DAYNIGHT* dayNight, float strength);
// Whether the map is indoors (+4 of the scene).
// ref: FUN_1424438d0
void DayNightSetIndoors(DAYNIGHT* dayNight, bool indoors);
// ref: FUN_142443530
void DayNightSetWater(DAYNIGHT* dayNight, bool underwater, const void* fog, bool waterFog, float depth);
// ref: FUN_142443210
void DayNightBeginFrame(DAYNIGHT* dayNight);
// The light's direction block (+0x1580, or the second light's +0x15a4 for an env that asks).
// ref: FUN_142442810
uint8_t* DayNightLightBlock(DAYNIGHT* dayNight, const void* env);
// The light state's time of day (FUN_142442cd0).
uint32_t DayNightGetTime(DAYNIGHT* dayNight);
// A value, scale and offset for the scene's light transform (FUN_142442e70); writes nothing.
bool DayNightGetLightTransform(DAYNIGHT* dayNight, float* value, float* scale, float* offset);
// The water colour: the override's, or the current light's (second light for such an env).
// ref: FUN_142442c30
uint32_t* DayNightWaterColor(DAYNIGHT* dayNight, uint32_t* out, const void* env);
// The scene fogs the world copies (FogInfo `index`, or the first / last fixed one past them).
// ref: FUN_142442740
MODELFOG* DayNightFog(DAYNIGHT* dayNight, uint32_t index);
// ref: FUN_142442500
MODELFOG* DayNightFogF8(DAYNIGHT* dayNight);
// ref: FUN_142442780
MODELFOG* DayNightFog1B0(DAYNIGHT* dayNight);
// ref: FUN_142442790
MODELFOG* DayNightFog250(DAYNIGHT* dayNight);
// ref: FUN_1424427f0
MODELFOG* DayNightFog2F4(DAYNIGHT* dayNight);
// ref: FUN_142442800
MODELFOG* DayNightFog394(DAYNIGHT* dayNight);
// How many fogs the scene draws with: the FogInfo count for an env that blends them, else 1.
// ref: FUN_142442a50
uint32_t DayNightFogCount(DAYNIGHT* dayNight, const void* env, bool single);

// Whether the time of day is in the day band (0.2208..0.875).
// ref: FUN_142442fb0
bool DayNightIsDaytime(DAYNIGHT* dayNight);
// The map's flag that holds the light state (+0x1601 of the scene).
// ref: FUN_142443520
void DayNightSetOverride(DAYNIGHT* dayNight, bool set);

// ref: FUN_142443d80
void DayNightCompute(DAYNIGHT* dayNight, int32_t timeMs, float elapsed, float timeSeconds, bool alive, void* env,
    const float* viewMatrix, bool colorGrading, bool zoneLightsSingly);
// ref: FUN_1424434f0
void DayNightComputeSunDirection(DAYNIGHT* dayNight, const float* viewMatrix);
// ref: FUN_142443db0
void DayNightPrepareFrame(DAYNIGHT* dayNight, void* env, bool colorGrading);

// ref: FUN_142453550
void DayNightGatherLights(blz::vector<int32_t, s_dnTagInt>* lights, int32_t areaId);

// A celestial body row changed (the CelestialBody table's patch callback).
// ref: FUN_142443120
void DayNightOnCelestialBodyPatched(uint32_t a1, uint32_t id, uint32_t a3, void* a4, void* a5);

#endif
