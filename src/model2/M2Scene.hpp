#ifndef MODEL2_M2_SCENE_HPP
#define MODEL2_M2_SCENE_HPP

// CM2Scene (Engine/Source/Model2/M2Scene.cpp, 0xdb40): the models a world or a UI frame draws
// together, with the camera they are seen through, the lights they are lit by and the lists a
// frame's animation and drawing walk. Only the fields the model code reads are named so far;
// the scene's own functions are ported with M2Scene.cpp.

#include <cstddef>
#include <cstdint>

#include "model2/M2Model.hpp"

class CM2Scene {
    public:
    uint8_t m_000[0x78];
    CM2BaseModel* m_models;             // 0x078
    CM2BaseModel* m_released;           // 0x080 models whose last reference went this frame
    uint8_t m_088[0x10];
    uint32_t m_time;                    // 0x098 ms
    uint32_t m_pad09c;
    uint32_t m_frame;                   // 0x0a0
    uint8_t m_0a4[0xc];
    uint32_t m_flags;                   // 0x0b0 0x40 no model lighting, 0x200 ...
    uint8_t m_0b4[0x14];
    uint32_t m_maxLights;               // 0x0c8 8
    uint8_t m_0cc[0x5c];
    CM2BaseModel* m_buckets[0x40];      // 0x128
    uint8_t m_328[0xd478 - 0x328];
    C44Matrix m_camera;                 // 0xd478 the view
    C44Matrix m_viewProjection;         // 0xd4b8
    uint8_t m_d4f8[0xd538 - 0xd4f8];
    C3Vector m_windOrigin;              // 0xd538
    uint8_t m_d544[0xd560 - 0xd544];
    void* m_frustum;                    // 0xd560
    uint8_t m_d568[0xd674 - 0xd568];
    float m_animateRatio;               // 0xd674 screen size past which animation is light
    uint8_t m_d678[0xd6a8 - 0xd678];
    // The wind: direction, speed and gust (the last argument 0).
    void (*m_windCallback)(float* direction, float* speed, float* gust, void* param);  // 0xd6a8
    uint8_t m_d6b0[0xd790 - 0xd6b0];
    void* m_rayTracing;                 // 0xd790
    uint8_t m_d798[0xdae8 - 0xd798];
    bool m_collectDrawParams;           // 0xdae8
    bool m_bucketModels;                // 0xdae9
    uint8_t m_daea[0xdb1c - 0xdaea];
    uint32_t m_bucketCounter;           // 0xdb1c
    uint8_t m_db20[0xdb40 - 0xdb20];
};
static_assert(offsetof(CM2Scene, m_time) == 0x98, "CM2Scene time at 0x98");
static_assert(offsetof(CM2Scene, m_buckets) == 0x128, "CM2Scene buckets at 0x128");
static_assert(offsetof(CM2Scene, m_camera) == 0xd478, "CM2Scene camera at 0xd478");
static_assert(offsetof(CM2Scene, m_frustum) == 0xd560, "CM2Scene frustum at 0xd560");
static_assert(sizeof(CM2Scene) == 0xdb40, "CM2Scene is 0xdb40 bytes");

// The draw parameters a scene collects per pass (BaseModelDrawParams, 0x20): the model and its
// box (FUN_1411ace60 appends one to a pass's array at scene + 0x328 + pass * 0x18).
struct BaseModelDrawParams {
    CM2BaseModel* m_model;              // 0x00
    float m_box[6];                     // 0x08
};
static_assert(sizeof(BaseModelDrawParams) == 0x20, "BaseModelDrawParams is 0x20 bytes");

inline float M2SceneGetAnimateRatio(CM2Scene* scene) {
    return scene->m_animateRatio;
}

inline bool M2SceneHasWind(CM2Scene* scene) {
    return scene->m_windCallback != nullptr;
}

inline const float* M2SceneGetWindOrigin(CM2Scene* scene) {
    return &scene->m_windOrigin.x;
}

inline void M2SceneSampleWind(CM2Scene* scene, float* direction, float* speed, float* gust) {
    scene->m_windCallback(direction, speed, gust, nullptr);
}

// The scene's ray tracing (FUN_1411ddad0) and a model's entry in it (FUN_141260140).
void* M2SceneGetRayTracing(CM2Scene* scene);
void RayTracingRegisterModel(void* rayTracing, void* entry, uint32_t size);
// A registered model's instance marked for an update (FUN_14125ff50): `entry` is {index, state}.
void RayTracingMarkDirty(void* rayTracing, const int32_t* entry);

// ref: FUN_1411ace60
BaseModelDrawParams* M2SceneAddDrawParams(void* array);
// A box against a frustum's six planes: 0 when outside, else 3 (FUN_141346830).
int32_t M2FrustumTestBox(const float* planes, const float* box);
// The scene's lights into a model's (FUN_1411df990).
void M2SceneGatherLights(CM2Scene* scene, CM2ModelLighting* lighting);

#endif
