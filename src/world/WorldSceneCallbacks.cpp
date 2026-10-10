#include "world/WorldSceneCallbacks.hpp"

#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "model2/M2Scene.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldMap.hpp"
#include "world/WorldQueries.hpp"
#include "world/WorldScene.hpp"
#include "world/WorldStubs.hpp"
#include "world/WorldView.hpp"
#include <cmath>

namespace {

// DAT_1430ab9d4, DAT_1430ab9d8: the world's flag word and the mask it is read through.
uint32_t s_worldFlags;
uint32_t s_worldFlagMask;

constexpr float SEGMENT_EPSILON = 1.0000001e-06f;   // DAT_1426d270c
constexpr float GROUND_DROP = 20.0f;                // DAT_142632538
constexpr float WIND_EPSILON = 0.001f;              // DAT_142618f58

// What a model's bone trace asks (built in CM2Model's physics pass).
struct TRACEQUERY {
    uint64_t m_00;
    CWorldScene* m_scene;           // 0x08
    float m_position[3];            // 0x10
    float m_end[3];                 // 0x1c
    uint32_t m_28;
};

// A physics bone's state (0xe0 each, behind a count).
struct PHYSICSSTATES {
    uint32_t m_count;               // 0x00
    uint32_t m_pad04;
    uint8_t* m_states;              // 0x08
};

CWorldMap* CurrentMap(CWorldScene* scene) {
    return scene->m_maps[scene->m_mapIndex];
}

float SegmentLengthSquared(const WORLDSEGMENT& segment) {
    float dx = segment.m_end[0] - segment.m_start[0];
    float dy = segment.m_end[1] - segment.m_start[1];
    float dz = segment.m_end[2] - segment.m_start[2];
    return std::fabs(dy * dy + dx * dx + dz * dz);
}

}  // namespace

// ref: FUN_14134e3d0
uint32_t WorldGetFlags() {
    return s_worldFlagMask & s_worldFlags;
}

// ref: FUN_141365a00
void* WorldSceneProjectModel(void* query) {
    auto words = static_cast<uint64_t*>(query);
    auto scene = reinterpret_cast<CWorldScene*>(words[0]);
    auto view = reinterpret_cast<CWorldView*>(words[1]);
    if (scene != nullptr && view != nullptr && CurrentMap(scene) != nullptr) {
        if ((static_cast<uint8_t>(words[0xb]) & 1) == 0 && ((WorldGetFlags() >> 0xb) & 1) == 0) {
            return nullptr;
        }
        return WorldViewAddProjected(view, query);
    }
    return nullptr;
}

// ref: FUN_141365a50
void WorldSceneProjectCheck(void* drawHandle) {
    auto draw = static_cast<M2PROJECTEDDRAW*>(drawHandle);
    auto object = static_cast<uint8_t*>(draw->m_object);
    if (draw->m_owner != nullptr && draw->m_030 != nullptr && object != nullptr
        && *reinterpret_cast<int32_t*>(object + 0x280) != 0) {
        draw->m_shader->SetFrame864(reinterpret_cast<float*>(object + 0x198), reinterpret_cast<float*>(object + 0x1b8));
        draw->m_shader->FlushBatch(draw->m_context, 400);
        uint32_t& flags = *reinterpret_cast<uint32_t*>(object + 0x28);
        flags |= (draw->m_flags & 1) * 2;
        flags |= (draw->m_flags & 2) << 3;
        flags |= (draw->m_flags & 4) << 3;
        WorldViewDrawProjected(static_cast<CWorldView*>(draw->m_030), draw->m_context, draw->m_shader, draw->m_element,
            object + 0x30, &draw->m_color, object);
    }
}

// ref: FUN_1413511d0
void WorldSceneTrace(void* queryHandle, float* hit, float* t) {
    auto query = static_cast<TRACEQUERY*>(queryHandle);
    CWorldScene* scene = query->m_scene;
    if (scene == nullptr) {
        return;
    }
    CWorldMap* map = CurrentMap(scene);
    if (map == nullptr) {
        return;
    }
    WORLDSEGMENT segment;
    segment.m_start[0] = query->m_position[0];
    segment.m_start[1] = query->m_position[1];
    segment.m_start[2] = query->m_position[2];
    segment.m_end[0] = query->m_end[0];
    segment.m_end[1] = query->m_end[1];
    segment.m_end[2] = query->m_end[2];
    segment.m_flags = 0x120;
    segment.m_hits = 0;
    WORLDSEGMENTHIT result;
    result.m_point[0] = hit[0];
    result.m_point[1] = hit[1];
    result.m_point[2] = hit[2];
    float limit = *t;
    result.m_t = limit;
    result.m_10 = 0;
    result.m_18 = 0;
    float ground = 0.0f;
    if (SEGMENT_EPSILON <= SegmentLengthSquared(segment) && CurrentMap(scene) != nullptr) {
        WorldMapIntersectSegment(CurrentMap(scene), &segment, &result);
    }
    if (limit <= result.m_t && WorldMapGroundHeight(map, query->m_position, &ground)) {
        if (query->m_position[2] < ground) {
            segment.m_start[2] = ground;
        }
        if (SEGMENT_EPSILON <= SegmentLengthSquared(segment) && CurrentMap(scene) != nullptr) {
            if (segment.m_hits < 2) {
                WorldMapIntersectSegment(CurrentMap(scene), &segment, &result);
            } else {
                WorldMapIntersectSegmentMulti();
            }
        }
    }
    hit[0] = result.m_point[0];
    hit[1] = result.m_point[1];
    hit[2] = result.m_point[2];
    *t = result.m_t;
}

// ref: FUN_14134f920
void WorldScenePhysics(void* statesHandle, void* param) {
    auto states = static_cast<PHYSICSSTATES*>(statesHandle);
    CWorldView* view = static_cast<CWorldScene*>(param)->m_view;
    if (view == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < states->m_count; i++) {
        uint8_t* state = states->m_states + i * 0xe0;
        if (state[0xda] != 0 && state[0xd7] != 0) {
            WorldViewAddPhysics(view, state);
        }
    }
}

// ref: FUN_14139aff0
void WorldSceneSampleWind(const float* position, float* direction, float* speed, uint8_t gusts) {
    direction[1] = 0.0f;
    direction[2] = 0.0f;
    direction[0] = 0.0f;
    for (MAPWIND* wind = MapWindFirst(); wind != nullptr; wind = MapWindNext(wind)) {
        if ((wind->m_flags & 4) != 0) {
            float dx = position[0] - wind->m_center[0];
            float dz = position[2] - wind->m_center[2];
            float dy = position[1] - wind->m_center[1];
            if (wind->m_radius * wind->m_radius < dy * dy + dx * dx + dz * dz) {
                continue;
            }
        } else if ((gusts & 1) != 0 && (wind->m_flags & 0x20) == 0) {
            continue;
        }
        direction[0] = wind->m_direction[0] + direction[0];
        direction[1] = wind->m_direction[1] + direction[1];
        direction[2] = wind->m_direction[2] + direction[2];
    }
    float length = std::sqrt(direction[1] * direction[1] + direction[0] * direction[0] + direction[2] * direction[2]);
    *speed = length;
    if (WIND_EPSILON < length) {
        VectorNormalize(direction);
        return;
    }
    direction[2] = 1.0f;
}

// ref: FUN_1413513b0
bool WorldSceneGroundHeight(const float* position, float* height, void* param) {
    auto scene = static_cast<CWorldScene*>(param);
    WORLDSEGMENT segment;
    segment.m_start[0] = position[0];
    segment.m_start[1] = position[1];
    segment.m_start[2] = position[2];
    segment.m_end[0] = position[0];
    segment.m_end[1] = position[1];
    segment.m_end[2] = position[2] - GROUND_DROP;
    segment.m_flags = 0x100111;
    segment.m_hits = 0;
    WORLDSEGMENTHIT result;
    result.m_point[0] = 0.0f;
    result.m_point[1] = 0.0f;
    result.m_point[2] = 0.0f;
    result.m_t = 1.0f;
    result.m_10 = 0;
    result.m_18 = 0;
    if (scene != nullptr && SEGMENT_EPSILON <= SegmentLengthSquared(segment)) {
        CWorldMap* map = CurrentMap(scene);
        if (map != nullptr && WorldMapIntersectSegment(map, &segment, &result)) {
            *height = result.m_point[2];
            return true;
        }
    }
    return false;
}

// ref: FUN_1413c5040
void WorldSceneOnDeviceDestroyed(void* param) {
    auto scene = static_cast<CWorldScene*>(param);
    if (scene->m_19b8 != nullptr) {
        WorldCaptureDestroy(scene->m_19b8);
        void* buffer = scene->m_19c0;
        scene->m_19b8 = nullptr;
        scene->m_19c0 = nullptr;
        if (buffer != nullptr) {
            GxDeviceRelease0f8(&scene->m_19c0, buffer);
        }
        scene->m_19c8 = nullptr;
    }
}

// ref: FUN_1413c50e0
void WorldSceneOnRenderSettings(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    (void)previous;
    auto scene = static_cast<CWorldScene*>(param);
    if ((reinterpret_cast<const uint8_t*>(current)[0x34] & 0xc0) != 0) {
        int32_t size[2];
        RenderSettingsRenderSize(size);
        WorldCaptureCreate(&scene->m_19b8, static_cast<uint32_t>(size[0]), static_cast<uint32_t>(size[1]));
        return;
    }
    if (scene->m_19b8 != nullptr) {
        WorldCaptureDestroy(scene->m_19b8);
        void* buffer = scene->m_19c0;
        scene->m_19b8 = nullptr;
        scene->m_19c0 = nullptr;
        if (buffer != nullptr) {
            GxDeviceRelease0f8(&scene->m_19c0, buffer);
        }
        scene->m_19c8 = nullptr;
    }
}

// ref: FUN_1412af5b0
void WorldCaptureCreate(void** tex, uint32_t width, uint32_t height) {
    (void)tex;
    (void)width;
    (void)height;
    FROST_UNIMPLEMENTED();  // the capture divisor (DAT_142cfc390, set by FUN_1412aeef0)
}

// ref: FUN_1412af4e0
void WorldCaptureDestroy(void* tex) {
    void* local = tex;
    GxTexDestroy(&local);
}
