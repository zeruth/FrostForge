#ifndef MODEL2_C_M2_LIGHTING_HPP
#define MODEL2_C_M2_LIGHTING_HPP

// Engine/Source/Model2 CM2Lighting (0x98): a light a scene applies to its models, point
// (omnidirectional) or directional, with three ambient colours and a diffuse one. An enabled
// light with a scene is linked into one of the scene's light lists; a model frame keeps one
// without a scene as a description of the light it wants.

#include <cstdint>

class CM2Scene;

class CM2Lighting {
    public:
    enum : int32_t {
        LIGHT_DIRECTIONAL = 0,
        LIGHT_POINT = 1,
    };

    CM2Scene* m_scene;              // 0x00
    uint32_t m_08;                  // 0x08
    int32_t m_type;                 // 0x0c
    float m_position[3];            // 0x10 scene space
    float m_worldPosition[3];       // 0x1c through the scene's view
    float m_direction[3];           // 0x28 normalised
    float m_ambient[3][3];          // 0x34 the same colour three times
    float m_diffuse[3];             // 0x58
    uint8_t m_pad64[0xc];
    float m_range;                  // 0x70 from the diffuse colour's strength
    int32_t m_enabled;              // 0x74
    float m_78;                     // 0x78 0.5
    float m_7c;                     // 0x7c 1.0
    uint16_t m_flags;               // 0x80
    uint8_t m_pad82[6];
    CM2Lighting** m_link;           // 0x88 the slot that points here
    CM2Lighting* m_next;            // 0x90

    // ref: FUN_1411ff570
    CM2Lighting();
    // ref: FUN_141200500 (the unlink the destructor is)
    ~CM2Lighting();

    // ref: FUN_141200300
    void SetEnabled(int32_t enabled);
    // ref: FUN_141200210
    void SetLightType(int32_t type);
    // ref: FUN_141200250
    void SetPosition(const float* position);
    // ref: FUN_1412001f0
    void SetDirection(const float* direction);
    // The diffuse colour, each channel at most 10 (DAT_142cfb700).
    // ref: FUN_1411ffcd0
    float* GetDiffuse(float* out) const;
    // ref: FUN_141200500
    void Unlink();
    // Into the scene's point or directional list (FUN_141200090).
    // ref: FUN_141200090
    void Link();
};

static_assert(sizeof(CM2Lighting) == 0x98, "CM2Lighting is 0x98 bytes");

// The lights one model is lit by this frame (0x120, CM2BaseModel+0x1f0): the scene's
// directional term and its nearest point lights, gathered by the scene (FUN_1411df990).
struct CM2ModelLighting {
    // m_flags: 1 finished, 2 the directional terms worked out, 0x20 always, 0x40 the plane is
    // in camera space, 0x100 the scene's flag 0x200.
    CM2Scene* m_scene;                  // 0x000
    float m_position[4];                // 0x008
    uint16_t m_flags;                   // 0x018
    uint16_t m_pointCount;              // 0x01a
    float m_axes[3][3];                 // 0x01c
    float m_direction[3];               // 0x040
    float m_directionColor[3];          // 0x04c
    float m_ambient[3];                 // 0x058
    float m_064[3];                     // 0x064
    float m_070[3];                     // 0x070
    float m_directional[3];             // 0x07c
    float m_088[3];                     // 0x088
    float m_lightDirection[3];          // 0x094 normalised
    CM2Lighting* m_points[8];           // 0x0a0 nearest first
    float m_pointDistances[8];          // 0x0e0 squared
    uint32_t m_maxLights;               // 0x100
    uint32_t m_pad104;
    float m_plane[4];                   // 0x108
    uint8_t m_118[8];
};
static_assert(sizeof(CM2ModelLighting) == 0x120, "CM2ModelLighting is 0x120 bytes");

// Starts a model's lighting at `position` (FUN_141200000).
void M2ModelLightingBegin(CM2ModelLighting* lighting, CM2Scene* scene, const float* position);
// Works out the directional terms (FUN_141200330).
void M2ModelLightingFinish(CM2ModelLighting* lighting, void* param);
// Done: the plane into camera space when asked (FUN_1411ffb70).
void M2ModelLightingEnd(CM2ModelLighting* lighting);
// A plane through a matrix (FUN_14134ba00).
void PlaneTransform(float* plane, const float* m);

#endif
