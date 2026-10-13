// WoW/Source/WorldClient2/MapWind.cpp: the wind sources and the global wind they add up to.

#include "base/Matrix.hpp"
#include "base/RndSeed.hpp"
#include "storm/list/TSList.hpp"
#include "ui/FrameTime.hpp"
#include "world/WorldQueries.hpp"
#include <cmath>
#include <new>

extern "C" unsigned long long FUN_14134ec00();

namespace {

constexpr float WIND_VELOCITY_MAX = 200.0f;         // DAT_142737528
constexpr float WIND_VELOCITY_MIN = -200.0f;        // DAT_142738274
constexpr float WIND_DAMPING = 0.55f;               // DAT_1427211f0
constexpr float WIND_EPSILON = 0.001f;              // DAT_142618f58
constexpr float WIND_NORMALIZE_MIN = 2.3841858e-07f;  // DAT_14261a954
constexpr uint32_t WIND_RESAMPLE_MS = 150;          // the sources are stepped this often

float s_windDirection[3] = {1.0f, 0.0f, 0.0f};      // DAT_142d00f40: the sources' sum, normalized
float s_windVelocity[2] = {1.0f, 0.0f};             // DAT_142d00f50: chases the direction, damped
// DAT_142d00f58: the wind sources (the map's volumes), linked at their first field.
TSList<MAPWIND, TSGetLink<MAPWIND>, 8> s_mapWinds;  // DAT_142d00f58
// DAT_142d00f70: the second list of sources, stepped and expired with the first.
TSList<MAPWIND, TSGetLink<MAPWIND>, 8> s_mapWinds2;  // DAT_142d00f70
float s_windStrength;                               // DAT_1430ac108: the length of the sources' sum
float s_windSpeed;                                  // DAT_1430ac10c: the velocity's length
uint8_t s_windRestart;                              // DAT_1430ac110: start the interval over
uint32_t s_windSampled;                             // DAT_1430ac124: when the sources last stepped

// x held to [lo, hi], as the reference compares: below lo (or not a number) is lo.
float Clamp(float x, float lo, float hi) {
    if (!(lo <= x)) {
        return lo;
    }
    return hi < x ? hi : x;
}

// The velocity a step closer to the direction scaled by `strength`, held to +-200, and its
// length (under 0.001 is none).
void ChaseDirection(float strength, float x, float y, float elapsed) {
    float vx = (strength * x - s_windVelocity[0] * WIND_DAMPING) * elapsed + s_windVelocity[0];
    float vy = (strength * y - s_windVelocity[1] * WIND_DAMPING) * elapsed + s_windVelocity[1];
    s_windVelocity[0] = Clamp(vx, WIND_VELOCITY_MIN, WIND_VELOCITY_MAX);
    s_windVelocity[1] = Clamp(vy, WIND_VELOCITY_MIN, WIND_VELOCITY_MAX);
    s_windSpeed = std::sqrt(s_windVelocity[0] * s_windVelocity[0] + s_windVelocity[1] * s_windVelocity[1]);
    if (s_windSpeed < WIND_EPSILON) {
        s_windSpeed = 0.0f;
    }
}

// One source's step over `elapsed` seconds: the speed and each direction component wander by
// at most their rate a second, each held within its range; false once its lifetime ran out.
// ref: FUN_14139baf0
bool MapWindStep(MAPWIND* wind, float elapsed) {
    float speed = RndSeedSigned(&wind->m_seed) * elapsed;
    speed = Clamp(speed, -wind->m_speedRate, wind->m_speedRate) + wind->m_speed;
    wind->m_speed = Clamp(speed, wind->m_baseSpeed - wind->m_speedBelow, wind->m_baseSpeed + wind->m_speedAbove);
    float x = RndSeedSigned(&wind->m_seed) * wind->m_directionRate[0] + wind->m_currentDirection[0];
    float y = RndSeedSigned(&wind->m_seed) * wind->m_directionRate[1] + wind->m_currentDirection[1];
    float z = RndSeedSigned(&wind->m_seed) * wind->m_directionRate[2] + wind->m_currentDirection[2];
    wind->m_currentDirection[0] = Clamp(x, wind->m_baseDirection[0] - wind->m_directionRange[0],
        wind->m_baseDirection[0] + wind->m_directionRange[0]);
    wind->m_currentDirection[1] = Clamp(y, wind->m_baseDirection[1] - wind->m_directionRange[1],
        wind->m_baseDirection[1] + wind->m_directionRange[1]);
    wind->m_currentDirection[2] = Clamp(z, wind->m_baseDirection[2] - wind->m_directionRange[2],
        wind->m_baseDirection[2] + wind->m_directionRange[2]);
    VectorNormalize(wind->m_currentDirection);
    wind->m_direction[0] = wind->m_speed * wind->m_currentDirection[0];
    wind->m_direction[1] = wind->m_currentDirection[1] * wind->m_speed;
    wind->m_direction[2] = wind->m_currentDirection[2] * wind->m_speed;
    if ((wind->m_flags & 2) == 0) {
        return true;
    }
    wind->m_lifetime = wind->m_lifetime - elapsed;
    return 0.0f < wind->m_lifetime;
}

// Every source on `list` steps; those that expired are unlinked and deleted.
void StepList(TSList<MAPWIND, TSGetLink<MAPWIND>, 8>& list, float elapsed) {
    MAPWIND* wind = list.Head();
    while (wind != nullptr) {
        MAPWIND* next = list.Next(wind);
        if (!MapWindStep(wind, elapsed)) {
            list.Link(wind)->Unlink();
            delete wind;
        }
        wind = next;
    }
}

}  // namespace

MAPWIND* MapWindFirst() {
    return s_mapWinds.Head();
}

MAPWIND* MapWindNext(MAPWIND* wind) {
    return s_mapWinds.Next(wind);
}

// translated: FUN_14134ec00
// ref: FUN_14139b480
void MapWindUpdate() {
    // the first call starts the interval (a function-local static, guard DAT_1430ac128)
    static bool s_started = (s_windSampled = FrameTime_GetTime(), true);
    (void)s_started;
    uint32_t now = FrameTime_GetTime();
    float elapsed = FrameTime_GetElapsedSeconds();
    if (!(0.0f < elapsed)) {
        return;
    }
    if (s_windRestart != 0) {
        s_windSampled = now;
        s_windRestart = 0;
    }
    if (now - s_windSampled < WIND_RESAMPLE_MS) {
        // between samples the velocity keeps chasing the last direction
        if (0.0f < s_windStrength || 0.0f < s_windSpeed) {
            ChaseDirection(s_windStrength, s_windDirection[0], s_windDirection[1], elapsed);
        }
        return;
    }
    s_windSampled = now;
    StepList(s_mapWinds, elapsed);
    StepList(s_mapWinds2, elapsed);
    // the camera position: a sphere source only blows inside its radius
    auto position = reinterpret_cast<const float*>(FUN_14134ec00());
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    s_windDirection[2] = 0.0f;
    s_windDirection[1] = 0.0f;
    s_windDirection[0] = 0.0f;
    for (MAPWIND* wind = s_mapWinds.Head(); wind != nullptr; wind = s_mapWinds.Next(wind)) {
        if ((wind->m_flags & 4) != 0) {
            float dx = position[0] - wind->m_center[0];
            float dy = position[1] - wind->m_center[1];
            float dz = position[2] - wind->m_center[2];
            if (wind->m_radius * wind->m_radius < dy * dy + dx * dx + dz * dz) {
                continue;
            }
        }
        x += wind->m_direction[0];
        y += wind->m_direction[1];
        z += wind->m_direction[2];
        s_windDirection[0] = x;
        s_windDirection[1] = y;
        s_windDirection[2] = z;
    }
    float lengthSquared = y * y + x * x + z * z;
    s_windStrength = std::sqrt(lengthSquared);
    if (WIND_EPSILON < s_windStrength) {
        if (WIND_NORMALIZE_MIN < lengthSquared) {
            float scale = 1.0f / s_windStrength;
            x *= scale;
            y *= scale;
            z *= scale;
            s_windDirection[0] = x;
            s_windDirection[1] = y;
            s_windDirection[2] = z;
        }
    } else {
        s_windDirection[2] = 1.0f;
        if (!(0.0f < s_windStrength) && !(0.0f < s_windSpeed)) {
            return;
        }
    }
    ChaseDirection(s_windStrength, x, y, elapsed);
}
