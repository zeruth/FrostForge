#include "daynight/DayNight.hpp"

#include "base/Matrix.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "model2/M2Scene.hpp"
#include "storm/Memory.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldClip.hpp"
#include "world/WorldScene.hpp"
#include <algorithm>
#include <cmath>

// DayNight's light computation (DayNight.cpp, DNLight.cpp): a Light row names eight
// LightParams rows, each LightParams row owns the LightData rows of its day (keyed by the time
// in half minutes), and a CurrentLight / CurrentFog pair is those rows interpolated at the
// time of day. The frame blends the area's light with the zone lights it is inside, the point
// lights near it, the area overrides and the transition the world asked for.

const char s_dnTagHeightFog[] = ".?AUHeightFogBlendPercent@CurrentLight@DayNight@@";
const char s_dnTagCurrentFog[] = ".?AUCurrentFog@DayNight@@";
const char s_dnTagLightQE[] = ".?AVLightQE@World@@";

namespace {

constexpr float DAY_LENGTH = 2880.0f;               // DAT_1429798e0, half minutes
constexpr int32_t DAY_SPAN = 0xb40;
constexpr float EPSILON = 1.1920929e-07f;          // DAT_1426c6f7c
constexpr float FLAG_EPSILON = 0.001f;             // DAT_142618f58
constexpr float BLEND_MINIMUM = 0.05f;             // DAT_14261a974
constexpr float ZONE_FADE_RANGE = 100.0f;          // DAT_142618d68
constexpr float ZONE_FADE_SCALE = 0.01f;           // DAT_142636334
constexpr float FOG_DENSITY_BASE = 1.5f;           // DAT_1426c70a8
constexpr float FOG_DENSITY_RANGE = 5.5f;          // DAT_1427e4104
constexpr float FOG_FAR_MAX = 700.0f;              // DAT_1427fb064
constexpr float FOG_FAR_OFFSET = 200.0f;           // DAT_142737528
constexpr float FOG_DENSITY_FLOOR = 0.9f;          // DAT_14261bf70
constexpr float FOG_END_SCALE = 1.0f / 36.0f;      // DAT_142722f6c
constexpr float FOG_END_MINIMUM = 27.777779f;      // DAT_1429798dc
constexpr int32_t DEFAULT_COLOR_GRADING = 0x1167fd;

// The LightParams slots the two current lights read, by the scene's state at +0xec and +0xf0
// (DAT_142979be0, DAT_142979bf8).
const int32_t s_paramSlotsA[] = {2, 0, 5, 6, 7, 0};
const int32_t s_paramSlotsB[] = {3, 1, 5, 6, 7};

// A fresh CurrentLight's fog defaults (DAT_1429798f0, DAT_1427219f0, DAT_142979900,
// DAT_14261a9d0).
const float s_fogDefaults[0x10] = {
    1e10f, 0.0f, 0.0f, 0.0f,
    -10000.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.001f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f, 0.0f,
};

uint8_t* Bytes(void* p) {
    return static_cast<uint8_t*>(p);
}

float Lerp(float from, float to, float t) {
    return (to - from) * t + from;
}

// The light block's (id, weight) pairs: 32 of them and their count at +0x100.
struct LIGHTPARAMSBLOCK {
    struct {
        int32_t m_id;
        float m_weight;
    } m_entries[0x20];
    uint32_t m_count;
};
static_assert(sizeof(LIGHTPARAMSBLOCK) == 0x104, "a light's params block is 0x104 bytes");

LIGHTPARAMSBLOCK* ParamsBlock(DNCURRENTLIGHT* light, size_t offset) {
    return reinterpret_cast<LIGHTPARAMSBLOCK*>(light->m_data + offset);
}

// Whether the light blocks blend at all (DAT_142d4d458, set at load and never cleared).
bool s_blendParams = true;

// ---- the LightData rows ----------------------------------------------------------------------

bool Same(const DB2PARENTITER& a, const DB2PARENTITER& b) {
    return a.m_entry == b.m_entry && a.m_overflow == b.m_overflow && a.m_parent == b.m_parent
        && a.m_source == b.m_source;
}

int32_t DataTime(const uint8_t* data) {
    return DB2FieldU16(data, 1, 0, &g_lightDataDB);
}

// The colour grading the LightData row names (FUN_14244c650, the gatherer's field).
// ref: FUN_14244c650
int32_t DataColorGrading(const uint8_t* data) {
    return static_cast<int32_t>(DB2FieldU32(data, 0x1f, 0, &g_lightDataDB));
}

// The LightData rows of `paramsId` either side of `time` and how far between them it is; a
// LightParams row without data falls back to the first one that has some.
// ref: FUN_142448710
void LightDataPair(int32_t time, int32_t paramsId, float* t, const uint8_t** prev, const uint8_t** next) {
    if (DB2ParentCount(&g_lightDataDB, paramsId) < 1) {
        paramsId = 0;
        do {
            if (0 < DB2ParentCount(&g_lightDataDB, paramsId)) {
                break;
            }
            paramsId++;
        } while (paramsId < 10000);
    }
    WowDB2ParentLookup* lookup = g_lightDataDB.m_parentLookup;
    if (lookup == nullptr) {
        return;
    }
    DB2PARENTITER first;
    DB2PARENTITER end;
    lookup->Begin(&first, paramsId);
    lookup->End(&end);
    DB2PARENTITER it = first;
    *prev = static_cast<const uint8_t*>(lookup->Record(&it));
    lookup->Next(&it);
    if (Same(it, end)) {
        *next = static_cast<const uint8_t*>(lookup->Record(&first));
        return;
    }
    *next = static_cast<const uint8_t*>(lookup->Record(&it));
    *t = 0.0f;
    while (!Same(it, first)) {
        int32_t from = DataTime(*prev);
        if (from <= time) {
            int32_t to = DataTime(*next);
            if (time < to) {
                *t = static_cast<float>(time - from) / static_cast<float>(to - from);
                return;
            }
        }
        *prev = static_cast<const uint8_t*>(lookup->Record(&it));
        lookup->Next(&it);
        if (Same(it, end)) {
            *next = static_cast<const uint8_t*>(lookup->Record(&first));
            break;
        }
        *next = static_cast<const uint8_t*>(lookup->Record(&it));
    }
    int32_t from = DataTime(*prev);
    int32_t span = DataTime(*next) - from + DAY_SPAN;
    int32_t at = from <= time ? time : time + DAY_SPAN;
    *t = static_cast<float>(at - from) / static_cast<float>(span);
}

// The values `field` gives the rows of `paramsId` either side of `current` (rows giving 0 are
// passed over, wrapping round the day) and how far between them `time` is.
// ref: FUN_14244baf0
void LightDataBlendIds(int32_t time, int32_t paramsId, const uint8_t* current, int32_t* idA, int32_t* idB,
    float* t, int32_t (*field)(const uint8_t*)) {
    const uint8_t* records[0x3fb];
    uint32_t count = 0;
    int32_t valued = 0;
    int32_t last = 0;
    WowDB2ParentLookup* lookup = g_lightDataDB.m_parentLookup;
    if (lookup != nullptr) {
        DB2PARENTITER it;
        DB2PARENTITER end;
        lookup->Begin(&it, paramsId);
        lookup->End(&end);
        while (!Same(it, end)) {
            auto record = static_cast<const uint8_t*>(lookup->Record(&it));
            if (field(record) != 0) {
                valued++;
                last = field(record);
            }
            records[count++] = record;
            lookup->Next(&it);
        }
    }
    if (count == 0 || valued == 0) {
        *idB = 0;
        *idA = 0;
        *t = 0.0f;
        return;
    }
    if (count == 1 || valued == 1) {
        *idB = last;
        *idA = last;
        *t = 0.0f;
        return;
    }
    const uint8_t** begin = records;
    const uint8_t** lastRecord = records + count - 1;
    const uint8_t** at = begin;
    while (at != records + count && *at != current) {
        at++;
    }
    const uint8_t** a = at;
    int32_t value = field(*at);
    while (value == 0 && a != begin) {
        value = field(a[-1]);
        a--;
    }
    if (field(*a) == 0) {
        value = field(*lastRecord);
        for (a = lastRecord; value == 0 && a != at && a != begin; a--) {
            value = field(a[-1]);
        }
    }
    const uint8_t** b = begin;
    if (at != lastRecord) {
        value = field(at[1]);
        b = at;
        for (;;) {
            b++;
            if (value != 0 || b == lastRecord) {
                break;
            }
            value = field(b[1]);
        }
    }
    if (field(*b) == 0) {
        value = field(*begin);
        for (b = begin; value == 0 && b != at && b != lastRecord; b++) {
            value = field(b[1]);
        }
    }
    const uint8_t* recordA = *a;
    const uint8_t* recordB = *b;
    *idA = field(recordA);
    *idB = field(recordB);
    int32_t from = DataTime(recordA);
    int32_t to = DataTime(recordB);
    if (to < from) {
        int32_t at2 = from <= time ? time : time + DAY_SPAN;
        *t = static_cast<float>(at2 - from) / static_cast<float>(to - from + DAY_SPAN);
    } else if (to == from) {
        *t = 0.0f;
    } else {
        *t = static_cast<float>(time - from) / static_cast<float>(to - from);
    }
}

// Two weighted ids into a light block: `idA` at 1 - t and `idB` at t (0 meaning the default).
// ref: FUN_14244f840
void LightParamsPush(LIGHTPARAMSBLOCK* block, int32_t idA, int32_t idB, float t) {
    if (!s_blendParams) {
        return;
    }
    if (0.0f < 1.0f - t) {
        block->m_entries[block->m_count].m_weight = 1.0f - t;
        block->m_entries[block->m_count].m_id = idA != 0 ? idA : DEFAULT_COLOR_GRADING;
        block->m_count++;
    }
    if (0.0f < t) {
        block->m_entries[block->m_count].m_weight = t;
        block->m_entries[block->m_count].m_id = idB != 0 ? idB : DEFAULT_COLOR_GRADING;
        block->m_count++;
    }
}

// The fog's density from where it starts: the nearer the start inside the scene's view range,
// the denser.
// ref: FUN_14244ade0
float FogDensity(DNScene* scene, float start, float end) {
    float range = DNField<float>(scene, 0x44);
    if (FOG_FAR_MAX <= range) {
        range = FOG_FAR_MAX;
    }
    range -= FOG_FAR_OFFSET;
    if (end - start <= range && 0.0f < range) {
        return (1.0f - (end - start) / range) * FOG_DENSITY_RANGE + FOG_DENSITY_BASE;
    }
    return FOG_DENSITY_BASE;
}

// A LightData colour column interpolated between two rows, opaque.
uint32_t LerpColor(uint32_t next, uint32_t prev, float t) {
    uint32_t result = 0xff000000;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        auto from = static_cast<int32_t>((prev >> shift) & 0xff);
        auto to = static_cast<int32_t>((next >> shift) & 0xff);
        auto channel = static_cast<int32_t>(std::nearbyint(static_cast<float>(to - from) * t + static_cast<float>(from)));
        result |= (static_cast<uint32_t>(channel) & 0xff) << shift;
    }
    return result;
}

uint32_t DataColor(const uint8_t* data, uint32_t column) {
    return static_cast<uint32_t>(DB2FieldI32(data, column, 0, &g_lightDataDB));
}

uint32_t DataColorU(const uint8_t* data, uint32_t column) {
    return DB2FieldU32(data, column, 0, &g_lightDataDB);
}

float DataFloat(const uint8_t* data, uint32_t column) {
    return DB2FieldF32(data, column, 0, &g_lightDataDB);
}

// The colour in column `column`, or column 9's when the row leaves it 0.
uint32_t DataColorOr9(const uint8_t* data, uint32_t column) {
    if (DataColorU(data, column) == 0) {
        return DataColor(data, 9);
    }
    return DataColorU(data, column);
}

float Clamp(float value, float low, float high) {
    float v = value;
    if (high <= v) {
        v = high;
    }
    if (v <= low) {
        return low;
    }
    return v;
}

CWorldScene* SceneWorld(DNScene* scene) {
    return static_cast<CWorldScene*>(scene->m_m2Scene->m_owner);
}

// The light and fog of a LightParams row at `time` (half minutes): its LightData rows either
// side interpolated, then the row's own settings.
// ref: FUN_142448aa0
int32_t ComputeLight(DNScene* scene, int32_t time, DNCURRENTLIGHT* light, DNCURRENTFOG* fog, int32_t paramsId,
    const uint8_t* params) {
    if (params == nullptr) {
        return 0;
    }
    float t = 0.0f;
    const uint8_t* prev = nullptr;
    const uint8_t* next = nullptr;
    LightDataPair(time, paramsId, &t, &prev, &next);
    int32_t gradingA = 0;
    int32_t gradingB = 0;
    float gradingT = 0.0f;
    LightDataBlendIds(time, paramsId, prev, &gradingA, &gradingB, &gradingT, DataColorGrading);
    LightParamsPush(ParamsBlock(light, 0x20c), gradingA, gradingB, gradingT);
    LightParamsPush(ParamsBlock(light, 0x310), gradingA, gradingB, gradingT);

    light->Word(3) = LerpColor(DataColor(next, 2), DataColor(prev, 2), t);
    light->Word(0) = LerpColor(DataColor(next, 3), DataColor(prev, 3), t);
    light->Word(1) = LerpColor(DataColor(next, 0x21), DataColor(prev, 0x21), t);
    if ((light->Word(1) & 0xffffff) == 0) {
        light->Word(1) = light->Word(0);
    }
    light->Word(2) = LerpColor(DataColor(next, 0x22), DataColor(prev, 0x22), t);
    if ((light->Word(2) & 0xffffff) == 0) {
        light->Word(2) = light->Word(0);
    }
    light->Word(5) = LerpColor(DataColor(next, 4), DataColor(prev, 4), t);
    light->Word(6) = LerpColor(DataColor(next, 5), DataColor(prev, 5), t);
    light->Word(7) = LerpColor(DataColor(next, 6), DataColor(prev, 6), t);
    light->Word(8) = LerpColor(DataColor(next, 7), DataColor(prev, 7), t);
    light->Word(9) = LerpColor(DataColor(next, 8), DataColor(prev, 8), t);
    light->Word(10) = LerpColor(DataColor(next, 9), DataColor(prev, 9), t);
    light->Word(0xc) = LerpColor(DataColorU(next, 0x26), DataColorU(prev, 0x26), t);
    light->Word(4) = LerpColor(DataColor(next, 0x13), DataColor(prev, 0x13), t);
    light->Word(0xf) = LerpColor(DataColor(next, 10), DataColor(prev, 10), t);
    light->Word(0x10) = LerpColor(DataColor(next, 0xb), DataColor(prev, 0xb), t);
    light->Word(0x11) = LerpColor(DataColor(next, 0xd), DataColor(prev, 0xd), t);
    light->Word(0x12) = LerpColor(DataColor(next, 0xc), DataColor(prev, 0xc), t);
    light->Word(0x14) = LerpColor(DataColor(next, 0xf), DataColor(prev, 0xf), t);
    light->Word(0x15) = LerpColor(DataColor(next, 0x10), DataColor(prev, 0x10), t);
    light->Word(0x16) = LerpColor(DataColor(next, 0x11), DataColor(prev, 0x11), t);
    light->Word(0x17) = LerpColor(DataColor(next, 0x12), DataColor(prev, 0x12), t);
    light->Word(0xb) = LerpColor(DataColorOr9(next, 0x23), DataColorOr9(prev, 0x23), t);
    light->Word(0xd) = LerpColor(DataColorOr9(next, 0x28), DataColorOr9(prev, 0x28), t);
    uint32_t nextE = DataColorU(next, 0x29) != 0 ? DataColorU(next, 0x29) : DataColorOr9(next, 0x23);
    uint32_t prevE = DataColorU(prev, 0x29) != 0 ? DataColorU(prev, 0x29) : DataColorOr9(prev, 0x23);
    light->Word(0xe) = LerpColor(nextE, prevE, t);

    // The fog.
    float scalarA = Clamp(DataFloat(prev, 0x15), -3.0f, 1.0f);        // DAT_142738538
    float scalarB = Clamp(DataFloat(next, 0x15), -3.0f, 1.0f);
    float endA = DataFloat(prev, 0x14);
    if (endA <= 0.0f) {
        endA = 0.0f;
    }
    float endB = DataFloat(next, 0x14);
    if (endB <= 0.0f) {
        endB = 0.0f;
    }
    float heightA = DataFloat(prev, 0x17);
    if (heightA <= -10000.0f) {                                       // DAT_1426cc58c
        heightA = -10000.0f;
    }
    float heightScaleA = Clamp(DataFloat(prev, 0x18), -1.0f, 1.0f);   // DAT_14261a9c8
    float heightDensityA = DataFloat(prev, 0x19);
    float heightB = DataFloat(next, 0x17);
    if (heightB <= -10000.0f) {
        heightB = -10000.0f;
    }
    float heightScaleB = Clamp(DataFloat(next, 0x18), -1.0f, 1.0f);
    float heightDensityB = DataFloat(next, 0x19);
    float zA = DataFloat(prev, 0x1d);
    float zB = DataFloat(next, 0x1d);
    float zEndA = DataFloat(prev, 0x27);
    float zEndB = DataFloat(next, 0x27);
    if (DataColorU(prev, 0x26) == 0) {
        zA = 1.0f;
    }
    bool split = false;
    if (DataColorU(next, 0x26) == 0) {
        zB = 1.0f;
    } else if (zB < 1.0f) {
        split = true;
    }
    if (zA < 1.0f) {
        if (zB < 1.0f) {
            light->Float(0x2c) = 1.0f;
            zEndB = Lerp(zEndA, zEndB, t);
            zB = Lerp(zA, zB, t);
        } else {
            light->Float(0x2c) = 1.0f - t;
            zB = zA;
            zEndB = zEndA;
        }
    } else if (split) {
        light->Float(0x2c) = t;
    } else {
        light->Float(0x2c) = 0.0f;
        zB = 1.0f;
        zEndB = 0.0f;
    }
    light->Float(0x2d) = zB;
    light->Float(0x2e) = zEndB;

    Bytes(fog)[0x5c] = DataFloat(prev, 0x24) == 0.0f && DataFloat(next, 0x24) == 0.0f ? 1 : 0;
    float farA = DataFloat(prev, 0x24) <= 0.0f ? WorldSceneFarClip(SceneWorld(scene)) : DataFloat(prev, 0x24);
    float farB = DataFloat(next, 0x24) <= 0.0f ? WorldSceneFarClip(SceneWorld(scene)) : DataFloat(next, 0x24);
    (*fog)[0x16] = Lerp(farA, farB, t);
    if (10000.0f < heightA) {                                         // DAT_1426c6510
        heightA = 0.0f;
    }
    if (10000.0f < heightB) {
        heightB = 0.0f;
    }
    float minimumScalar = 0.0f;
    float densityA;
    if (DataFloat(prev, 0x16) <= 0.0f) {
        densityA = FogDensity(scene, endA * scalarA, endA);
    } else {
        minimumScalar = -0.2f;                                        // DAT_142738534
        densityA = DataFloat(prev, 0x16);
    }
    float densityB;
    if (DataFloat(next, 0x16) <= 0.0f) {
        densityB = FogDensity(scene, endB * scalarB, endB);
    } else {
        minimumScalar = -0.2f;
        densityB = DataFloat(next, 0x16);
    }
    if (heightScaleB == 0.0f) {
        heightDensityB = densityB;
    }
    if (heightScaleA == 0.0f) {
        heightDensityA = densityA;
    }
    (*fog)[1] = Lerp(scalarA, scalarB, t);
    (*fog)[0] = Lerp(endA, endB, t) * FOG_END_SCALE;
    float density = Lerp(densityA, densityB, t);
    (*fog)[4] = Lerp(heightA, heightB, t);
    (*fog)[5] = Lerp(heightScaleA, heightScaleB, t);
    (*fog)[3] = Lerp(DataFloat(prev, 0x25), DataFloat(next, 0x25), t);
    (*fog)[6] = Lerp(heightDensityA, heightDensityB, t);
    float* f = fog->m_values;
    if (f[0xd] * f[0xd] + f[0xc] * f[0xc] + f[10] * f[10] + f[0xb] * f[0xb] <= EPSILON) {
        f[10] = 0.0f;
        f[0xb] = 0.0f;
        f[0xc] = 1.0f;
        f[0xd] = 0.0f;
    }
    if (EPSILON < f[0x11] * f[0x11] + f[0x10] * f[0x10] + f[0xe] * f[0xe] + f[0xf] * f[0xf]
        || EPSILON < f[0x15] * f[0x15] + f[0x14] * f[0x14] + f[0x12] * f[0x12] + f[0x13] * f[0x13]) {
        f[0x18] = 0.0f;
    }
    if (density <= FOG_DENSITY_FLOOR) {
        density = FOG_DENSITY_FLOOR;
    }
    light->Float(0x1b) = Lerp(DataFloat(prev, 0x1e), DataFloat(next, 0x1e), t);

    // The LightParams row's own settings.
    const uint8_t* row = params + 0xc;
    light->Float(0x18) = static_cast<float>(DB2FieldU8(row, 2, 0xc, &g_lightParamsDB));
    light->Float(0x19) = DB2FieldF32(row, 5, 0xc, &g_lightParamsDB);
    light->Float(0x1e) = DB2FieldF32(row, 6, 0xc, &g_lightParamsDB);
    light->Float(0x1f) = DB2FieldF32(row, 7, 0xc, &g_lightParamsDB);
    light->Float(0x20) = DB2FieldF32(row, 8, 0xc, &g_lightParamsDB);
    light->Float(0x21) = DB2FieldF32(row, 9, 0xc, &g_lightParamsDB);
    uint8_t flags = DB2FieldU8(row, 10, 0xc, &g_lightParamsDB);
    light->Float(0x22) = (flags & 1) != 0 ? 1.0f : 0.0f;
    light->m_data[0x1e8] = (flags >> 1) & 1;
    uint8_t bits = light->m_data[0xc8];
    bits = static_cast<uint8_t>((bits & ~1) | ((flags & 4) == 0 ? 1 : 0));
    if ((flags & 4) != 0) {
        light->Float(0x2d) = 1.1f;
    }
    bits = static_cast<uint8_t>((bits & ~2) | ((flags & 8) == 0 ? 2 : 0));
    bits = static_cast<uint8_t>((bits & ~4) | ((flags & 0x10) == 0 ? 4 : 0));
    bits = static_cast<uint8_t>((bits & ~8) | ((flags & 0x20) != 0 ? 8 : 0));
    light->m_data[0xc8] = bits;
    light->Float(0x33) = (bits & 4) != 0 ? 0.0f : 1.0f;
    light->Float(0x34) = (bits & 8) != 0 ? 0.0f : 1.0f;
    std::memcpy(light->m_data + 0xbc, params, 0xc);
    light->Float(0x35) = static_cast<int8_t>(flags) < 0 ? 0.0f : 1.0f;
    if ((light->m_data[0xc8] & 8) != 0
        && light->Float(0x2f) * light->Float(0x2f) + light->Float(0x30) * light->Float(0x30)
                + light->Float(0x31) * light->Float(0x31)
            < FLAG_EPSILON) {
        light->m_data[0xc8] &= 0xf7;
    }
    light->Word(0x7b) = DB2FieldU16(row, 3, 0xc, &g_lightParamsDB);
    light->Float(0x7c) = 1.0f;
    uint8_t shadow = DB2FieldU8(row, 4, 0xc, &g_lightParamsDB);
    light->m_data[0xd8] = 0;
    light->Float(0x37) = 0.0f;
    light->Float(0x38) = 45.0f;
    light->Word(0x81) = shadow;
    light->Float(0x82) = 1.0f;

    (*fog)[2] = density;
    if (DNField<uint8_t>(scene, 4) == 0) {
        float scalar = (*fog)[1];
        if ((*fog)[1] < minimumScalar) {
            (*fog)[1] = minimumScalar;
            scalar = minimumScalar;
        }
        if (FOG_END_MINIMUM <= (*fog)[0]) {
            (*fog)[2] = FogDensity(scene, (*fog)[0] * scalar, (*fog)[0]);
            (*fog)[0] = WorldSceneFarClip3(SceneWorld(scene));
        }
    } else {
        (*fog)[2] = 1.0f;
    }

    light->m_data[0x8c] = 0;
    int32_t ssao = DB2FieldI32(row, 0xb, 0xc, &g_lightParamsDB);
    if (ssao != 0) {
        bool local = false;
        auto settings = static_cast<const uint8_t*>(g_sSAOSettingsDB.GetRecord(static_cast<uint32_t>(ssao), false, &local));
        if (settings != nullptr) {
            light->m_data[0x8c] = 1;
            light->Float(0x24) = DB2FieldF32(settings, 1, 0, &g_sSAOSettingsDB);
            light->Float(0x25) = DB2FieldF32(settings, 2, 0, &g_sSAOSettingsDB);
            light->Float(0x26) = DB2FieldF32(settings, 3, 0, &g_sSAOSettingsDB);
            light->Float(0x27) = DB2FieldF32(settings, 4, 0, &g_sSAOSettingsDB);
        }
    }
    return 1;
}

// The LightParams row of a Light row's slot.
// ref: FUN_142453520
const uint8_t* LightParamsOf(const uint8_t* lightRecord, int32_t slot, uint32_t* id) {
    uint16_t paramsId;
    std::memcpy(&paramsId, lightRecord + 0x16 + slot * 2, sizeof(paramsId));
    *id = paramsId;
    bool local = false;
    return static_cast<const uint8_t*>(g_lightParamsDB.GetRecord(paramsId, false, &local));
}

// ---- blending --------------------------------------------------------------------------------

// A light block faded by 1 - t with `from`'s weighted by t appended (32 at most).
// ref: FUN_14244f770
void BlendLightParams(LIGHTPARAMSBLOCK* to, const LIGHTPARAMSBLOCK* from, float t) {
    if (!s_blendParams) {
        return;
    }
    float keep = 1.0f - t;
    if (keep <= 0.0f) {
        to->m_count = 0;
    } else {
        for (uint32_t i = 0; i < to->m_count; i++) {
            if (0x1f < i) {
                to->m_count = 0x20;
                return;
            }
            to->m_entries[i].m_weight = keep * to->m_entries[i].m_weight;
        }
    }
    if (0.0f < t) {
        uint32_t added = 0;
        for (uint32_t i = 0; i < from->m_count; i++) {
            uint32_t slot = to->m_count + i;
            if (0x1f < slot) {
                to->m_count = 0x20;
                return;
            }
            to->m_entries[slot].m_id = from->m_entries[i].m_id;
            to->m_entries[to->m_count + i].m_weight = t * from->m_entries[i].m_weight;
            added = from->m_count;
        }
        to->m_count += added;
    }
}

// The flags, direction and shadow settings of a light taken over once the blend passes 5%.
// ref: FUN_142447330
void BlendLightFlags(DNCURRENTLIGHT* to, DNCURRENTLIGHT* from, float t) {
    if (BLEND_MINIMUM < t) {
        float keep = 1.0f - t;
        uint8_t bits = to->m_data[0xc8];
        bits = static_cast<uint8_t>((bits & ~3) | (from->m_data[0xc8] & 3));
        to->Float(0x33) = t * from->Float(0x33) + keep * to->Float(0x33);
        to->Float(0x34) = t * from->Float(0x34) + keep * to->Float(0x34);
        to->Float(0x35) = t * from->Float(0x35) + keep * to->Float(0x35);
        std::memcpy(to->m_data + 0xbc, from->m_data + 0xbc, 0xc);
        bits = static_cast<uint8_t>((bits & ~0xc) | (from->m_data[0xc8] & 0xc));
        to->m_data[0xc8] = bits;
        to->m_data[0xd8] |= from->m_data[0xd8];
        to->Float(0x37) = Lerp(to->Float(0x37), from->Float(0x37), t);
        to->Float(0x38) = Lerp(to->Float(0x38), from->Float(0x38), t);
    }
}

// The light's colour-grading pair (+0x1ec, three of them) moved toward `from`'s.
void BlendGradingPairs(DNCURRENTLIGHT* to, DNCURRENTLIGHT* from, float t) {
    float keep = 1.0f;
    uint32_t i = 0;
    for (; i < 3; i++) {
        int32_t id = static_cast<int32_t>(to->Word(0x7b + i * 2));
        keep = 1.0f - t;
        if (id == 0 || id == static_cast<int32_t>(from->Word(0x7b))) {
            break;
        }
        to->Float(0x7c + i * 2) = (1.0f - t) * to->Float(0x7c + i * 2);
    }
    if (i == 3) {
        return;
    }
    if (to->Word(0x7b + i * 2) == from->Word(0x7b)) {
        to->Float(0x7c + i * 2) = t * from->Float(0x7c) + keep * to->Float(0x7c + i * 2);
    } else {
        to->Word(0x7b + i * 2) = from->Word(0x7b);
        to->Word(0x7c + i * 2) = from->Word(0x7c);
        to->Float(0x7c + i * 2) = t;
    }
}

// One light toward another: the colours by a byte alpha, the rest linearly.
// ref: FUN_142445ee0
void BlendCurrentLight(DNCURRENTLIGHT* to, DNCURRENTLIGHT* from, float t, bool grading) {
    float f = 0.0f;
    if (0.0f <= t) {
        f = t;
        if (1.0f <= t) {
            f = 1.0f;
        }
    }
    BlendLightParams(ParamsBlock(to, 0x20c), ParamsBlock(from, 0x20c), f);
    BlendLightParams(ParamsBlock(to, 0x310), ParamsBlock(from, 0x310), f);
    auto alpha = static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(f * 255.0f))));
    static const size_t s_colors[] = {3, 0, 1, 2, 5, 6, 7, 8, 9, 10, 0xb, 0xc, 0xd, 0xe, 0xf, 0x10, 0x11, 0x12,
        0x14, 0x15, 0x16, 0x17};
    for (size_t index : s_colors) {
        ColorLerpByte(&to->Word(index), alpha, from->Word(index));
    }
    to->Float(0x2d) = Lerp(to->Float(0x2d), from->Float(0x2d), f);
    to->Float(0x2e) = Lerp(to->Float(0x2e), from->Float(0x2e), f);
    ColorLerpByte(&to->Word(4), alpha, from->Word(4));
    to->Float(0x1b) = Lerp(to->Float(0x1b), from->Float(0x1b), f);
    to->Float(0x19) = Lerp(to->Float(0x19), from->Float(0x19), f);
    if (from->Word(0x7b) != 0 || (grading && from->m_data[0x1e8] != 0)) {
        BlendGradingPairs(to, from, f);
    }
    for (size_t index = 0x1e; index <= 0x22; index++) {
        to->Float(index) = Lerp(to->Float(index), from->Float(index), f);
    }
    BlendLightFlags(to, from, f);
}

// One fog toward another.
// ref: FUN_142446e30
void BlendCurrentFog(DNCURRENTFOG* to, DNCURRENTFOG* from, float t) {
    for (size_t index = 0; index <= 0x16; index++) {
        (*to)[index] = Lerp((*to)[index], (*from)[index], t);
    }
    (*to)[0x18] = Lerp((*to)[0x18], (*from)[0x18], t);
}

// One fog toward another, the far distance taken over from a fog that has one.
// ref: FUN_1424470a0
void BlendCurrentFogWeighted(DNCURRENTFOG* to, DNCURRENTFOG* from, float t) {
    for (size_t index = 0; index <= 0x15; index++) {
        (*to)[index] = Lerp((*to)[index], (*from)[index], t);
    }
    (*to)[0x18] = Lerp((*to)[0x18], (*from)[0x18], t);
    if (Bytes(from)[0x5c] == 0) {
        if (Bytes(to)[0x5c] != 0) {
            Bytes(to)[0x5c] = 0;
            (*to)[0x16] = (*from)[0x16];
            return;
        }
        (*to)[0x16] = Lerp((*to)[0x16], (*from)[0x16], t);
    }
}

// A colour's channels moved toward another's by t, truncated, opaque.
uint32_t LerpColorTruncated(uint32_t to, uint32_t from, float t) {
    uint32_t result = 0xff000000;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        auto a = static_cast<float>((to >> shift) & 0xff);
        auto b = static_cast<float>((from >> shift) & 0xff);
        result |= (static_cast<uint32_t>(static_cast<int32_t>((b - a) * t + a)) & 0xff) << shift;
    }
    return result;
}

// One light toward another for a zone or point light: the colours channel by channel, the
// override sky slots and the grading pairs merged by weight.
// ref: FUN_1424474a0
void BlendCurrentLightWeighted(DNCURRENTLIGHT* to, DNCURRENTLIGHT* from, float t, bool reorder) {
    uint32_t water = to->Word(0xc);
    BlendLightParams(ParamsBlock(to, 0x20c), ParamsBlock(from, 0x20c), t);
    BlendLightParams(ParamsBlock(to, 0x310), ParamsBlock(from, 0x310), t);
    for (size_t index = 0; index <= 0x17; index++) {
        to->Word(index) = LerpColorTruncated(to->Word(index), from->Word(index), t);
    }
    if ((from->Word(0xc) & 0xffffff) == 0) {
        to->Word(0xc) = water;
    }
    if ((water & 0xffffff) == 0) {
        to->Word(0xc) = from->Word(0xc);
    }
    float z = to->Float(0x2c);
    if (z == 0.0f) {
        to->Float(0x2d) = from->Float(0x2d);
    } else if (0.0f < from->Float(0x2c)) {
        to->Float(0x2d) = Lerp(to->Float(0x2d), from->Float(0x2d), t);
    }
    to->Float(0x2c) = Lerp(z, from->Float(0x2c), t);
    to->Float(0x2e) = Lerp(to->Float(0x2e), from->Float(0x2e), t);
    to->Float(0x18) = Lerp(to->Float(0x18), from->Float(0x18), t);
    to->Float(0x1b) = Lerp(to->Float(0x1b), from->Float(0x1b), t);
    to->Float(0x19) = Lerp(to->Float(0x19), from->Float(0x19), t);
    for (size_t index = 0x1e; index <= 0x22; index++) {
        to->Float(index) = Lerp(to->Float(index), from->Float(index), t);
    }
    BlendLightFlags(to, from, t);
    to->m_data[0x8c] |= from->m_data[0x8c];
    for (size_t index = 0x24; index <= 0x2a; index++) {
        to->Float(index) = Lerp(to->Float(index), from->Float(index), t);
    }
    if (from->m_data[0x1e8] == 0 || t <= 0.0f) {
        if (from->Word(0x7b) != 0 && 0.0f < t) {
            uint32_t free = 3;
            for (uint32_t i = 0; i < 3; i++) {
                if (to->Word(0x7b + i * 2) == 0) {
                    free = i;
                    break;
                }
            }
            for (uint32_t s = 0; s < 3; s++) {
                int32_t id = static_cast<int32_t>(from->Word(0x7b + s * 2));
                if (id == 0) {
                    break;
                }
                uint32_t d = 0;
                while (d < 3 && static_cast<int32_t>(to->Word(0x7b + d * 2)) != id) {
                    d++;
                }
                if (d != 3) {
                    float weight = t * from->Float(0x7c + s * 2) + to->Float(0x7c + d * 2);
                    to->Float(0x7c + d * 2) = weight;
                    if (1.0f < weight) {
                        to->Float(0x7c + d * 2) = 1.0f;
                    }
                    if (reorder) {
                        uint64_t pair;
                        std::memcpy(&pair, to->m_data + 0x1ec + d * 8, 8);
                        if (d < 2) {
                            std::memmove(to->m_data + 0x1ec + d * 8, to->m_data + 0x1f4 + d * 8, (2 - d) * 8);
                        }
                        for (int32_t c = 2; c >= 0; c--) {
                            if (c == 0 || to->Word(0x7b + c * 2) != 0) {
                                std::memcpy(to->m_data + 0x1ec + c * 8, &pair, 8);
                                break;
                            }
                        }
                    }
                } else if (free != 3) {
                    to->Word(0x7b + free * 2) = static_cast<uint32_t>(id);
                    to->Float(0x7c + free * 2) = t * from->Float(0x7c + s * 2);
                    free++;
                }
            }
        }
    } else {
        uint32_t count = to->Word(0x79);
        if (count + 1 < 9) {
            to->Word(0x79) = count + 1;
            uint8_t* entry = to->m_data + 0xe4 + count * 0x20;
            entry[0x1c] = 1;
            std::memcpy(entry, from->m_data + 0x1ec, 0x18);
            std::memcpy(entry + 0x18, &t, sizeof(t));
        }
    }
    to->Word(0x81) = from->Word(0x81);
    to->Float(0x82) = t;
}

// The light's copy assignment.
// ref: FUN_1424451f0
void CopyCurrentLight(DNCURRENTLIGHT* to, DNCURRENTLIGHT* from) {
    std::memcpy(to->m_data, from->m_data, sizeof(to->m_data));
    to->m_heightFog.assign_from(from->m_heightFog);
}

// The light's copy construction: the vector's storage exactly its size.
// ref: FUN_142444ac0
void CopyConstructCurrentLight(DNCURRENTLIGHT* to, DNCURRENTLIGHT* from) {
    std::memcpy(to->m_data, from->m_data, sizeof(to->m_data));
    uint64_t count = from->m_heightFog.m_size;
    // FUN_14244c5e0: the storage of the fresh light, allocated even when empty.
    to->m_heightFog.m_data = static_cast<uint64_t*>(
        SMemAlloc(count * 8, 0xb, s_dnTagHeightFog, SStrHashFnv1a(s_dnTagHeightFog), -6, 0));
    to->m_heightFog.m_capacity = count;
    std::memcpy(to->m_heightFog.m_data, from->m_heightFog.m_data, count * 8);
    to->m_heightFog.m_size = count;
}

// ---- the blend list ------------------------------------------------------------------------

// The scene's (LightParams id, weight) list for one of the two lights (+0x1f10, +0x1fa8): an
// embedded vector of sixteen.
struct BLENDLIST {
    uint64_t* m_data;
    uint64_t m_size;
    uint64_t m_capacity;
};

BLENDLIST* Blends(DNScene* scene, int32_t which) {
    return reinterpret_cast<BLENDLIST*>(which == 1 ? &scene->m_blends2 : &scene->m_blends);
}

// The list's room for `count` (half again, at least `count`).
// ref: FUN_14244c8d0
void BlendsReserve(BLENDLIST* list, uint64_t count) {
    uint64_t capacity = list->m_capacity & ~blz::VECTOR_NOT_OWNED;
    if (count <= capacity) {
        return;
    }
    uint64_t grown = ((list->m_capacity >> 1) & 0x3fffffffffffffffull) + capacity;
    if (grown < count) {
        grown = count;
    }
    list->m_capacity = ((list->m_capacity ^ grown) & blz::VECTOR_NOT_OWNED) ^ grown;
    auto data = static_cast<uint64_t*>(SMemAlloc(grown * 8, 0xb, s_dnTagPairIntFloat,
        SStrHashFnv1a(s_dnTagPairIntFloat), -6, 0));
    for (uint64_t i = 0; i < list->m_size; i++) {
        data[i] = list->m_data[i];
    }
    if (static_cast<int64_t>(list->m_capacity) >= 0) {
        SMemFree(list->m_data, s_dnTagPairIntFloat, -6, 0);
    }
    list->m_capacity &= ~blz::VECTOR_NOT_OWNED;
    list->m_data = data;
}

void BlendsPush(BLENDLIST* list, int32_t id, float weight) {
    if (list->m_size == (list->m_capacity & ~blz::VECTOR_NOT_OWNED)) {
        BlendsReserve(list, (list->m_capacity & ~blz::VECTOR_NOT_OWNED) + 1);
    }
    auto pair = reinterpret_cast<uint32_t*>(&list->m_data[list->m_size]);
    pair[0] = static_cast<uint32_t>(id);
    std::memcpy(&pair[1], &weight, sizeof(weight));
    list->m_size++;
}

int32_t SlotParamsId(const uint8_t* lightRecord, int32_t slot) {
    uint16_t id;
    std::memcpy(&id, lightRecord + 0x16 + slot * 2, sizeof(id));
    return id;
}

// The blend list's entries for a light at `weight` (and its two transition slots).
void PushLightBlends(DNScene* scene, const uint8_t* lightRecord, int32_t slot, int32_t slotA, int32_t slotB,
    float t, float weight) {
    BLENDLIST* list = Blends(scene, slot);
    BlendsPush(list, SlotParamsId(lightRecord, slot), weight);
    float blend = DNField<float>(scene, 0xe8);
    if (0.0f < blend) {
        if (slot != slotA) {
            BlendsPush(list, SlotParamsId(lightRecord, slotA), (1.0f - t) * blend);
        }
        if (slotA != slotB) {
            BlendsPush(list, SlotParamsId(lightRecord, slotB), t * DNField<float>(scene, 0xe8));
        }
    }
}

// The fog list's push_back.
// ref: FUN_14244c680
void PushFog(DNFOGVECTOR* fogs, DNCURRENTFOG* fog) {
    fogs->push_back(*fog);
}

// The fog list after a light's weight: replaced when it covers everything, added to when it
// counts at all.
void AddFog(DNFOGVECTOR* fogs, DNCURRENTFOG* fog, float weight) {
    if (1.0f <= weight) {
        fogs->m_size = 0;
        PushFog(fogs, fog);
    } else if (0.0f < weight) {
        PushFog(fogs, fog);
    }
}

int32_t DayTime(DNScene* scene) {
    return static_cast<int32_t>(DNField<float>(scene, 0xc) * DAY_LENGTH);
}

// A Light row's light at the time of day in `slot`, then (while the weather blends, +0xe8)
// toward its two weather slots.
// ref: FUN_142445ac0
int32_t ComputeAreaLight(DNScene* scene, DNCURRENTLIGHT* light, DNCURRENTFOG* fog, const uint8_t* lightRecord,
    int32_t slot, int32_t slotA, int32_t slotB, float t) {
    uint32_t id = 0;
    const uint8_t* params = LightParamsOf(lightRecord, slot, &id);
    if (ComputeLight(scene, DayTime(scene), light, fog, static_cast<int32_t>(id), params) == 0) {
        return 0;
    }
    if (DNField<float>(scene, 0xe8) <= 0.0f) {
        return 1;
    }
    DNCURRENTLIGHT lightA;
    DNCURRENTLIGHT lightB;
    DNCURRENTFOG fogA;
    DNCURRENTFOG fogB;
    int32_t fallback = slot == 1 ? 3 : 2;
    params = LightParamsOf(lightRecord, slotA, &id);
    if (params == nullptr) {
        params = LightParamsOf(lightRecord, fallback, &id);
    }
    int32_t okA = ComputeLight(scene, DayTime(scene), &lightA, &fogA, static_cast<int32_t>(id), params);
    if (slotA == slotB) {
        if (okA != 0) {
            BlendCurrentLight(light, &lightA, DNField<float>(scene, 0xe8), true);
            BlendCurrentFog(fog, &fogA, DNField<float>(scene, 0xe8));
            return 1;
        }
        return 0;
    }
    params = LightParamsOf(lightRecord, slotB, &id);
    if (params == nullptr) {
        params = LightParamsOf(lightRecord, fallback, &id);
    }
    int32_t okB = ComputeLight(scene, DayTime(scene), &lightB, &fogB, static_cast<int32_t>(id), params);
    if (okA != 0 && okB != 0) {
        BlendCurrentLight(light, &lightA, DNField<float>(scene, 0xe8), true);
        BlendCurrentFog(fog, &fogA, DNField<float>(scene, 0xe8));
        BlendCurrentLight(light, &lightB, t, true);
        BlendCurrentFog(fog, &fogB, t);
        return 1;
    }
    return 0;
}

// A zone light's share: its light at `distance` inside the zone (full past 100 yards) times
// its fade over the group's total, blended into `light` and `fog`.
// ref: FUN_142448400
float AddZoneLight(DNScene* scene, DNCURRENTLIGHT* light, DNCURRENTFOG* fog, DNCURRENTFOG* out,
    const uint8_t* lightRecord, float distance, int32_t slot, int32_t slotA, int32_t slotB, float t, float total,
    float fade) {
    DNCURRENTLIGHT computed;
    if (ComputeAreaLight(scene, &computed, out, lightRecord, slot, slotA, slotB, t) == 0) {
        return 0.0f;
    }
    float inside = (ZONE_FADE_RANGE - distance) * ZONE_FADE_SCALE;
    float weight = 1.0f;
    if (0.0f < inside) {
        weight = 1.0f - inside;
        if (weight < 0.0f) {
            weight = 0.0f;
        }
    }
    if (1.0f <= weight) {
        weight = 1.0f;
    }
    weight = (weight * fade) / total;
    (*out)[0x19] = weight;
    PushLightBlends(scene, lightRecord, slot, slotA, slotB, t, weight);
    BlendCurrentLightWeighted(light, &computed, weight, false);
    BlendCurrentFogWeighted(fog, out, weight);
    return weight;
}

// A point light's share: full inside its inner radius (or when forced), fading out to its
// outer one, at most `limit`.
// ref: FUN_142445770
float AddPointLight(DNScene* scene, DNCURRENTLIGHT* light, DNCURRENTFOG* fog, DNCURRENTFOG* out, float limit,
    const uint8_t* lightRecord, int32_t slot, int32_t slotA, int32_t slotB, float t, bool forced) {
    DNCURRENTLIGHT computed;
    if (ComputeAreaLight(scene, &computed, out, lightRecord, slot, slotA, slotB, t) == 0) {
        return 0.0f;
    }
    auto position = reinterpret_cast<const float*>(lightRecord);
    float dx = DNField<float>(scene, 0x20) - position[0];
    float dz = DNField<float>(scene, 0x28) - position[2];
    float dy = DNField<float>(scene, 0x24) - position[1];
    float inner = position[3];
    float distance = std::sqrt(dy * dy + dx * dx + dz * dz);
    float weight = 1.0f;
    if (inner < distance && !forced) {
        weight = 1.0f - (distance - inner) / (position[4] - inner);
        if (weight < 0.0f) {
            weight = 0.0f;
        }
    }
    if (limit <= weight) {
        weight = limit;
    }
    (*out)[0x19] = weight;
    PushLightBlends(scene, lightRecord, slot, slotA, slotB, t, weight);
    BlendCurrentLightWeighted(light, &computed, weight, true);
    BlendCurrentFogWeighted(fog, out, weight);
    return weight;
}

// The pool the area overrides' Light copies come from (DAT_1447aa310).
struct LIGHTPOOL {
    uint64_t m_00;
    int32_t m_count;                // 0x08
    uint32_t m_pad0c;
    uint64_t m_10;
    void* m_free;                   // 0x18
};
LIGHTPOOL s_overrideLights;

// ref: FUN_142430fb0
void LightPoolFree(LIGHTPOOL* pool, void* light) {
    *static_cast<void**>(light) = pool->m_free;
    pool->m_count--;
    pool->m_free = light;
}

// An area override (0x28): a Light row forced over the area, fading in (1) or out (2).
struct AREAOVERRIDE {
    uint64_t m_00;
    const uint8_t* m_light;         // 0x08
    float m_weight;                 // 0x10
    int32_t m_state;                // 0x14
    int32_t m_time;                 // 0x18
    uint32_t m_duration;            // 0x1c
    uint8_t m_forced;               // 0x20
    uint8_t m_pad21[7];
};
static_assert(sizeof(AREAOVERRIDE) == 0x28, "an area override is 0x28 bytes");

AREAOVERRIDE* Overrides(DNScene* scene) {
    return reinterpret_cast<AREAOVERRIDE*>(scene->m_areaOverrides.m_data);
}

// One of the two lights with everything layered over the area's: the zone lights (each alone,
// or grouped by priority), the point lights near the camera and the area overrides; the fogs
// each contributed, nearest-weighted first, four at most.
// ref: FUN_14244af40
void GatherLights(DNScene* scene, DNLIGHTQEVECTOR* nearby, DNCURRENTLIGHT* light, DNCURRENTFOG* fog,
    DNFOGVECTOR* fogs, int32_t slot, int32_t slotA, int32_t slotB, float t, bool singly) {
    DNCURRENTFOG out;
    PushFog(fogs, fog);
    uint64_t zoneCount = DNField<uint64_t>(scene, 0x738);
    if (zoneCount != 0) {
        auto zones = DNField<CZONELIGHT*>(scene, 0x730);
        auto count = static_cast<int64_t>(static_cast<int32_t>(zoneCount));
        if (!singly) {
            DNCURRENTLIGHT saved;
            CopyConstructCurrentLight(&saved, light);
            DNCURRENTFOG savedFog = *fog;
            int64_t i = 0;
            uint32_t groupStart = 0;
            uint32_t groupEnd;
            do {
                if (count <= i) {
                    break;
                }
                int64_t j = i + 1;
                groupEnd = groupStart + 1;
                while (j < count && zones[j].m_14 == zones[i].m_14) {
                    groupEnd++;
                    j++;
                }
                int64_t members = static_cast<int32_t>(groupEnd - groupStart);
                float total = 0.0f;
                for (int64_t k = 0; k < members; k++) {
                    float inside = (ZONE_FADE_RANGE - zones[i + k].m_distance) * ZONE_FADE_SCALE;
                    float weight = 1.0f;
                    if (0.0f < inside) {
                        weight = 1.0f - inside;
                        if (weight < 0.0f) {
                            weight = 0.0f;
                        }
                    }
                    if (1.0f <= weight) {
                        weight = 1.0f;
                    }
                    total += weight * zones[i + k].m_fade;
                }
                DNCURRENTLIGHT groupLight;
                DNCURRENTFOG groupFog;
                for (int64_t k = 0; k < members; k++) {
                    auto zone = &DNField<CZONELIGHT*>(scene, 0x730)[i + k];
                    AddZoneLight(scene, &groupLight, &groupFog, &out, static_cast<const uint8_t*>(zone->m_light),
                        zone->m_distance, slot, slotA, slotB, t, total, zone->m_fade);
                }
                if (1.0f <= total) {
                    total = 1.0f;
                }
                groupFog[0x19] = total;
                BlendCurrentLightWeighted(&saved, &groupLight, total, false);
                BlendCurrentFogWeighted(&savedFog, &groupFog, total);
                AddFog(fogs, &out, total);
                groupStart = groupEnd;
                i = j;
            } while (static_cast<int32_t>(groupEnd) < 5);
            CopyCurrentLight(light, &saved);
            *fog = savedFog;
        } else {
            for (int64_t i = 0; i < count; i++) {
                auto zone = &DNField<CZONELIGHT*>(scene, 0x730)[i];
                float weight = AddZoneLight(scene, light, fog, &out, static_cast<const uint8_t*>(zone->m_light),
                    zone->m_distance, slot, slotA, slotB, t, 1.0f, zone->m_fade);
                AddFog(fogs, &out, weight);
            }
        }
    }
    for (uint64_t i = 0; i < nearby->m_size; i++) {
        float weight = AddPointLight(scene, light, fog, &out, 1.0f,
            static_cast<const uint8_t*>(nearby->m_data[i].m_light), slot, slotA, slotB, t, false);
        AddFog(fogs, &out, weight);
    }
    uint32_t overrides = scene->m_areaOverrides.m_count;
    for (uint32_t i = 0; i < overrides; i++) {
        AREAOVERRIDE* entry = &Overrides(scene)[i];
        auto position = reinterpret_cast<const float*>(entry->m_light);
        float weight = entry->m_weight;
        float dx = DNField<float>(scene, 0x20) - position[0];
        float dz = DNField<float>(scene, 0x28) - position[2];
        float dy = DNField<float>(scene, 0x24) - position[1];
        if (0.0f < weight && (dy * dy + dx * dx + dz * dz < position[4] * position[4] || entry->m_forced != 0)) {
            float applied = AddPointLight(scene, light, fog, &out, weight, entry->m_light, slot, slotA, slotB, t,
                entry->m_forced != 0);
            AddFog(fogs, &out, applied);
        }
        entry = &Overrides(scene)[i];
        if (entry->m_state == 2 && entry->m_weight == 0.0f) {
            overrides--;
            LightPoolFree(&s_overrideLights, const_cast<uint8_t*>(entry->m_light));
            Overrides(scene)[i] = Overrides(scene)[overrides];
            // TSGrowableArray::SetCount(count - 1): its growth arm cannot run when shrinking.
            scene->m_areaOverrides.m_count = overrides;
            i--;
        }
    }
    BLENDLIST* list = Blends(scene, slot);
    auto pairs = reinterpret_cast<float*>(list->m_data);
    for (uint32_t k = 1; k < static_cast<uint32_t>(list->m_size); k++) {
        float keep = 1.0f - pairs[k * 2 + 1];
        for (int64_t m = static_cast<int64_t>(k) - 1; 0 <= m; m--) {
            pairs[m * 2 + 1] = keep * pairs[m * 2 + 1];
        }
    }
    (*fog)[0x19] = 1.0f;
    uint32_t fogCount = static_cast<uint32_t>(fogs->m_size);
    for (uint32_t i = 0; i < fogCount; i++) {
        float weight = fogs->m_data[i][0x19];
        for (uint32_t k = i + 1; k < fogCount; k++) {
            weight *= 1.0f - fogs->m_data[k][0x19];
        }
        fogs->m_data[i][0x19] = weight;
    }
    // FUN_142443fa0: the fogs sorted by weight, heaviest first (std::sort's introsort).
    std::sort(fogs->begin(), fogs->end(), [](const DNCURRENTFOG& a, const DNCURRENTFOG& b) {
        return b.m_values[0x19] < a.m_values[0x19];
    });
    if (4 < fogCount) {
        fogs->m_size = 4;
    }
}

// The skybox override's flags (LightSkybox field 1) and its celestial body (field 3).
// ref: FUN_142459340
uint8_t SkyboxFlags(const uint8_t* row) {
    return DB2FieldU8(row, 1, 4, &g_lightSkyboxDB);
}

// ref: FUN_142459370
int32_t SkyboxCelestialBody(const uint8_t* row) {
    return DB2FieldI32(row, 3, 4, &g_lightSkyboxDB);
}

// The override sky of a LightSkybox row: its model by file id, or by name.
// ref: FUN_142460e20
uint8_t* OverrideSkyFor(DNScene* scene, const uint8_t* skybox, int32_t id) {
    (void)scene;
    (void)skybox;
    (void)id;
    FROST_UNIMPLEMENTED();  // FUN_142460740 / FUN_142460a90: DNOverrideSky by file id or by path
    return nullptr;
}

// The override sky of a celestial body ("Cel%d").
// ref: FUN_142460440
void* OverrideCelestial(DNScene* scene, int32_t body, uint8_t flags) {
    (void)scene;
    (void)body;
    (void)flags;
    FROST_UNIMPLEMENTED();  // DNOverrideSky "Cel%d": FUN_14245fcb0, FUN_1411d8c80
    return nullptr;
}

// A LightSkybox row's sky over the light at `weight` (three skies at most): already listed or
// the transition's, its weight grows; otherwise it takes the next slot.
// ref: FUN_1424594b0
void AddSkybox(DNScene* scene, uint32_t id, uint32_t* slots, float* clear, float weight, bool fromLight) {
    if (!g_lightSkyboxDB.m_loaded) {
        return;
    }
    bool local = false;
    auto row = static_cast<const uint8_t*>(g_lightSkyboxDB.GetRecord(id, false, &local));
    if (row == nullptr || weight <= EPSILON) {
        return;
    }
    uint8_t* sky = OverrideSkyFor(scene, row, static_cast<int32_t>(id));
    const uint8_t* fields = row + 4;
    uint8_t full = fromLight ? 1 : static_cast<uint8_t>(SkyboxFlags(fields) & 2);
    uint8_t underwater = (DB2FieldU8(fields, 1, 4, &g_lightSkyboxDB) >> 2) & 1;
    uint8_t flags = DB2FieldU8(fields, 1, 4, &g_lightSkyboxDB);
    if (1.0f <= weight && full == 0) {
        *slots = 0;
    }
    uint32_t count = *slots;
    int32_t model = DB2FieldI32(fields, 2, 4, &g_lightSkyboxDB);
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (DNField<int32_t>(DNField<uint8_t*>(scene, 0xb8 + i * 8), 0x54) == model) {
            found = true;
            break;
        }
    }
    if (!found && (DNField<uint8_t*>(scene, 0x80) == nullptr || DNField<int32_t>(DNField<uint8_t*>(scene, 0x80), 0x54) != model)) {
        DNField<float>(sky, 0x4c) = weight;
        DNField<uint8_t>(sky, 0x50) = full != 0;
        if (*slots < 3) {
            DNField<uint8_t*>(scene, 0xb8 + *slots * 8) = sky;
            uint8_t skyFlags = SkyboxFlags(fields);
            int32_t body = SkyboxCelestialBody(fields);
            DNField<void*>(scene, 0xd0 + *slots * 8) = OverrideCelestial(scene, body, skyFlags);
            (*slots)++;
        }
    } else {
        float total = weight + DNField<float>(sky, 0x4c);
        if (1.0f <= total) {
            total = 1.0f;
        }
        DNField<uint8_t>(sky, 0x50) |= full != 0;
        DNField<float>(sky, 0x4c) = total;
    }
    DNField<uint8_t>(scene, 0x92) |= underwater;
    if (full != 0) {
        float total = weight + DNField<float>(scene, 0x9c);
        if (1.0f <= total) {
            total = 1.0f;
        }
        DNField<float>(scene, 0x9c) = total;
    }
    if (underwater != 0) {
        float total = weight + DNField<float>(scene, 0x98);
        if (1.0f <= total) {
            total = 1.0f;
        }
        DNField<float>(scene, 0x98) = total;
    }
    if (((flags >> 4) & 1) != 0) {
        *clear = (1.0f - weight) * *clear;
    }
}

// The transition the world asked for (+0x70, faded in over +0x60 ms and out over +0x64): its
// LightParams light over both lights and every fog, its skybox over the sky.
// ref: FUN_14245dde0
void ApplyTransition(DNScene* scene, DNCURRENTLIGHT* light, DNCURRENTFOG* fog, DNFOGVECTOR* fogs,
    DNCURRENTLIGHT* lightB, DNCURRENTFOG* fogB) {
    int32_t target = DNField<int32_t>(scene, 0x70);
    int32_t current = DNField<int32_t>(scene, 0x74);
    DNField<uint64_t>(scene, 0x80) = 0;
    DNField<uint64_t>(scene, 0x88) = 0;
    DNField<uint8_t>(scene, 0x79) = 0;
    int32_t id = target;
    float blend = 1.0f;
    if (current != target) {
        int32_t elapsed = DNField<int32_t>(scene, 0x6c);
        bool settled = true;
        if (target == 0) {
            int32_t fadeOut = DNField<int32_t>(scene, 0x64);
            if (elapsed < fadeOut) {
                id = current;
                blend = 1.0f - static_cast<float>(elapsed) / static_cast<float>(fadeOut);
                settled = blend <= 0.0f;
            }
            if (settled) {
                blend = 0.0f;
                DNField<int32_t>(scene, 0x74) = 0;
                id = 0;
            }
        } else {
            int32_t fadeIn = DNField<int32_t>(scene, 0x60);
            if (elapsed < fadeIn) {
                blend = static_cast<float>(elapsed) / static_cast<float>(fadeIn);
                settled = 1.0f <= blend;
            } else {
                DNField<uint8_t>(scene, 0x90) = DNField<uint8_t>(scene, 0x91);
            }
            if (settled) {
                blend = 1.0f;
                DNField<int32_t>(scene, 0x74) = target;
            }
        }
        if (settled) {
            DNField<int32_t>(scene, 0x6c) = 0;
        }
        DNField<float>(scene, 0x68) = blend;
    }
    if (id == 0) {
        return;
    }
    bool local = false;
    auto params = static_cast<const uint8_t*>(g_lightParamsDB.GetRecord(static_cast<uint32_t>(id), false, &local));
    if (params == nullptr) {
        return;
    }
    DNCURRENTLIGHT computed;
    DNCURRENTFOG computedFog;
    ComputeLight(scene, DayTime(scene), &computed, &computedFog, id, params);
    DNField<uint8_t>(scene, 0x79) = computed.m_data[0x1e8];
    computed.Word(0x7b) = 0;
    BlendCurrentLight(light, &computed, blend, false);
    BlendCurrentFog(fog, &computedFog, blend);
    for (uint64_t i = 0; i < fogs->m_size; i++) {
        BlendCurrentFog(&fogs->m_data[i], &computedFog, blend);
    }
    BlendLightFlags(light, &computed, blend);
    BlendCurrentLight(lightB, &computed, blend, false);
    BlendCurrentFog(fogB, &computedFog, blend);
    BlendsPush(Blends(scene, 0), id, blend);
    uint16_t skyboxId = DB2FieldU16(params + 0xc, 3, 0xc, &g_lightParamsDB);
    auto skybox = static_cast<const uint8_t*>(g_lightSkyboxDB.GetRecord(skyboxId, false, &local));
    if (skybox != nullptr) {
        const uint8_t* fields = skybox + 4;
        uint8_t flagsA = DB2FieldU8(fields, 1, 4, &g_lightSkyboxDB);
        uint8_t flagsB = DB2FieldU8(fields, 1, 4, &g_lightSkyboxDB);
        uint8_t* sky = OverrideSkyFor(scene, skybox, DB2FieldU16(params + 0xc, 3, 0xc, &g_lightParamsDB));
        uint8_t flags = DB2FieldU8(fields, 1, 4, &g_lightSkyboxDB);
        int32_t body = DB2FieldI32(fields, 3, 4, &g_lightSkyboxDB);
        void* celestial = OverrideCelestial(scene, body, flags);
        DNField<float>(sky, 0x4c) = blend;
        DNField<uint8_t>(sky, 0x50) = ((flagsA >> 1) & 1) != 0;
        DNField<uint8_t>(scene, 0x78) = ((flagsB >> 2) & 1) != 0;
        if (((flagsB >> 2) & 1) != 0) {
            DNField<float>(scene, 0x98) = (1.0f - DNField<float>(scene, 0x68)) * DNField<float>(scene, 0x98)
                + DNField<float>(scene, 0x68);
        }
        DNField<uint8_t*>(scene, 0x80) = sky;
        DNField<void*>(scene, 0x88) = celestial;
    }
}

// A colour with its brightness (HSV value) scaled by `scale` (clamped to 0..1).
// ref: FUN_14244ae70
uint32_t Desaturate(uint32_t color, float scale) {
    float s = 0.0f;
    if (0.0f <= scale) {
        s = std::min(scale, 1.0f);
    }
    float rgb[3] = {
        static_cast<float>((color >> 16) & 0xff) * (1.0f / 255.0f),
        static_cast<float>((color >> 8) & 0xff) * (1.0f / 255.0f),
        static_cast<float>(color & 0xff) * (1.0f / 255.0f),
    };
    float hsv[3] = {0.0f, 0.0f, 0.0f};
    ColorRgbToHsv(rgb, hsv);
    hsv[2] *= s;
    ColorHsvToRgb(hsv, rgb);
    return ColorFromVector(rgb);
}

// The world's light query over the areas' Light rows (World::LightQE, nearest first).
// ref: FUN_14134e050
void WorldQueryLights(CWorldScene* scene, const float* position, DNLIGHTQEVECTOR* lights) {
    (void)scene;
    (void)position;
    (void)lights;
    FROST_UNIMPLEMENTED();  // FUN_1413c3ed0: the map's area chunks (FUN_14134e350), Map flags (FUN_14134cac0)
}

// The band of the day-time keys (time, value) at `t`: the key at or after it and the one before
// it (wrapping round the day) interpolated.
const float s_band780[] = {0.25f, 1.0f, 0.29166666f, 0.0f, 0.85416669f, 0.0f, 0.89583331f, 1.0f};

}  // namespace

// ref: FUN_14244c4d0
float DNFloatBand(const float* keys, uint32_t count, float t) {
    if (0.0f <= t) {
        if (1.0f <= t) {
            t = 1.0f;
        }
    } else {
        t = 0.0f;
    }
    uint32_t i = 0;
    while (i < count && keys[i * 2] < t) {
        i++;
    }
    uint32_t next = i != count ? i : 0;
    uint32_t prev = next != 0 ? next - 1 : count - 1;
    float from = keys[prev * 2];
    float span = keys[next * 2] - from;
    if (std::fabs(span) < FLAG_EPSILON) {
        return keys[prev * 2 + 1];
    }
    if (span < 0.0f) {
        span += 1.0f;
    }
    float at = t - from;
    if (at < 0.0f) {
        at += 1.0f;
    }
    return (keys[next * 2 + 1] - keys[prev * 2 + 1]) * (at / span) + keys[prev * 2 + 1];
}

// ref: FUN_1424597a0
void DNScene::ComputeLights(bool alive, bool zoneLightsSingly) {
    const uint8_t* liquid = nullptr;
    float liquidDepth = DNField<float>(this, 0x3c);
    if (DNField<int32_t>(this, 0x38) != 0) {
        bool local = false;
        liquid = static_cast<const uint8_t*>(
            g_liquidTypeDB.GetRecord(static_cast<uint32_t>(DNField<int32_t>(this, 0x38)), false, &local));
    }
    for (size_t offset = 0xb8; offset <= 0xe0; offset += 8) {
        DNField<uint64_t>(this, offset) = 0;
    }
    int32_t slot = alive ? 0 : 4;
    DNField<uint8_t>(this, 0x92) = 0;
    auto lightA = reinterpret_cast<DNCURRENTLIGHT*>(this->m_light.m_data + 0xa70);
    auto lightB = reinterpret_cast<DNCURRENTLIGHT*>(this->m_light.m_data + 0xf60);
    auto fogA = reinterpret_cast<DNCURRENTFOG*>(this->m_light.m_data + 0x1468);
    auto fogB = reinterpret_cast<DNCURRENTFOG*>(this->m_light.m_data + 0x14d0);
    if (this->m_areaLights[0].m_size == 0) {
        // No area lights: white everywhere, a grey ambient and a far fog.
        std::memset(this->m_light.m_data + 0xa7c, 0xff, 0x54);
        std::memset(this->m_light.m_data + 0xafc, 0xff, 0x160);
        DNField<uint32_t>(this, 0xc60) = 0xffffffff;
        DNField<uint32_t>(this, 0xc68) = 0xffffffff;
        DNField<uint32_t>(this, 0xc70) = 0xffffffff;
        std::memset(this->m_light.m_data + 0xc78, 0xff, 0x2e8);
        std::memset(this->m_light.m_data + 0xf6c, 0xff, 0x54);
        std::memset(this->m_light.m_data + 0xfec, 0xff, 0x160);
        DNField<uint32_t>(this, 0x1150) = 0xffffffff;
        DNField<uint32_t>(this, 0x1158) = 0xffffffff;
        DNField<uint32_t>(this, 0x1160) = 0xffffffff;
        std::memset(this->m_light.m_data + 0x1168, 0xff, 0x2e8);
        DNField<float>(this, 0x14d4) = 0.5f;
        DNField<float>(this, 0x1468) = 1e10f;
        DNField<float>(this, 0x146c) = 0.5f;
        DNField<float>(this, 0x14d8) = 4.0f;
        DNField<float>(this, 0x1470) = 4.0f;
        DNField<float>(this, 0x14e0) = -10000.0f;
        DNField<float>(this, 0x1478) = -10000.0f;
        DNField<float>(this, 0x14e4) = 0.0f;
        DNField<float>(this, 0x147c) = 0.0f;
        DNField<float>(this, 0x14e8) = 1.0f;
        DNField<float>(this, 0x1480) = 1.0f;
        DNField<uint32_t>(this, 0xfc0) = 0;
        DNField<uint32_t>(this, 0xad0) = 0;
        DNField<uint64_t>(this, 0xfc8) = 0;
        DNField<uint64_t>(this, 0xad8) = 0;
        DNField<uint64_t>(this, 0xfd0) = 0;
        DNField<uint64_t>(this, 0xae0) = 0;
        DNField<uint32_t>(this, 0xf60) = 0xff404040;
        DNField<uint32_t>(this, 0xa70) = 0xff404040;
        DNField<uint32_t>(this, 0xf68) = 0xff404040;
        DNField<uint32_t>(this, 0xa78) = 0xff404040;
        DNField<uint32_t>(this, 0xf64) = 0xff404040;
        DNField<uint32_t>(this, 0xa74) = 0xff404040;
        DNField<float>(this, 0xfc4) = 0.5f;
        DNField<float>(this, 0xad4) = 0.5f;
        DNField<float>(this, 0xfe0) = 0.75f;
        DNField<float>(this, 0xaf0) = 0.75f;
        DNField<uint64_t>(this, 0xfe4) = 0x3f800000;
        DNField<uint64_t>(this, 0xaf4) = 0x3f800000;
        DNField<float>(this, 0xfd8) = 0.5f;
        DNField<float>(this, 0xae8) = 0.5f;
        DNField<float>(this, 0xfdc) = 1.0f;
        DNField<float>(this, 0xaec) = 1.0f;
        DNField<uint32_t>(this, 0x1164) = 0;
        DNField<uint32_t>(this, 0xc74) = 0;
        DNField<uint32_t>(this, 0x114c) = 0;
        DNField<uint32_t>(this, 0xc5c) = 0;
        DNField<uint32_t>(this, 0x1154) = 0;
        DNField<uint32_t>(this, 0xc64) = 0;
        DNField<uint32_t>(this, 0x115c) = 0;
        DNField<uint32_t>(this, 0xc6c) = 0;
    } else {
        DNCURRENTLIGHT computedA;
        DNCURRENTLIGHT computedB;
        DNCURRENTFOG computedFogA;
        DNCURRENTFOG computedFogB;
        DNFOGVECTOR fogsA;
        DNFOGVECTOR fogsB;
        bool local = false;
        auto lightRecord = static_cast<const uint8_t*>(
            g_lightDB.GetRecord(static_cast<uint32_t>(this->m_areaLights[0].m_data[0]), false, &local));
        computedA.Float(0x33) = 0.0f;
        int32_t weather = DNField<int32_t>(this, 0xec);
        int32_t weatherNext = DNField<int32_t>(this, 0xf0);
        float weatherT = DNField<float>(this, 0xf4);
        ComputeAreaLight(this, &computedA, &computedFogA, lightRecord, slot, s_paramSlotsA[weather],
            s_paramSlotsA[weatherNext], weatherT);
        ComputeAreaLight(this, &computedB, &computedFogB, lightRecord, 1, s_paramSlotsA[weather],
            s_paramSlotsA[weatherNext], weatherT);
        DNLIGHTQEVECTOR nearby;
        this->m_blendCount = 0;
        this->m_blendCount2 = 0;
        WorldQueryLights(this->m_worldScene, &DNField<float>(this, 0x20), &nearby);
        GatherLights(this, &nearby, &computedA, &computedFogA, &fogsA, slot, s_paramSlotsA[weather],
            s_paramSlotsA[weatherNext], weatherT, zoneLightsSingly);
        GatherLights(this, &nearby, &computedB, &computedFogB, &fogsB, 1, s_paramSlotsB[weather],
            s_paramSlotsA[weatherNext], weatherT, zoneLightsSingly);
        // FUN_142454ea0: this frame's zone selection is spent.
        DNField<uint64_t>(this, 0x738) = 0;
        ApplyTransition(this, &computedA, &computedFogA, &fogsA, &computedB, &computedFogB);
        CopyCurrentLight(lightA, &computedA);
        CopyCurrentLight(lightB, &computedB);
        // FUN_142459180: the light state's fog list.
        reinterpret_cast<DNFOGVECTOR*>(this->m_light.m_data + 0x1450)->assign_from(fogsA);
        *fogA = computedFogA;
        *fogB = computedFogB;
        if (liquid != nullptr) {
            const uint8_t* liquidFields = liquid + 0x1c;
            if (DB2FieldU16(liquidFields, 10, 0x1c, &g_liquidTypeDB) != 0) {
                uint16_t paramsId = DB2FieldU16(liquidFields, 10, 0x1c, &g_liquidTypeDB);
                auto params = static_cast<const uint8_t*>(g_lightParamsDB.GetRecord(paramsId, false, &local));
                ComputeLight(this, DayTime(this), lightB, fogB, DB2FieldU16(liquidFields, 10, 0x1c, &g_liquidTypeDB),
                    params);
            }
        }
    }
    this->StoreLightColors();

    // The water's depth tint, eased toward the liquid's.
    float target[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float dt = static_cast<float>(static_cast<uint32_t>(DNField<int32_t>(this, 0x50) - this->m_2060)) * 0.001f;
    if (liquid != nullptr) {
        const uint8_t* liquidFields = liquid + 0x1c;
        float depth = DB2FieldF32(liquidFields, 6, 0x1c, &g_liquidTypeDB);
        if (1e-05f < depth) {
            float floor = -DB2FieldF32(liquidFields, 6, 0x1c, &g_liquidTypeDB);
            float under = (DNField<float>(this, 0xfe8) - 1.0f) * liquidDepth;
            float low = under;
            if (0.0f < under) {
                low = 0.0f;
            }
            float value = floor;
            if (floor < low) {
                value = under;
                if (0.0f < under) {
                    value = 0.0f;
                }
            }
            target[0] = 1.0f - value / floor;
            target[1] = DB2FieldF32(liquidFields, 7, 0x1c, &g_liquidTypeDB);
            target[2] = DB2FieldF32(liquidFields, 8, 0x1c, &g_liquidTypeDB);
            target[3] = DB2FieldF32(liquidFields, 9, 0x1c, &g_liquidTypeDB);
        }
    }
    // FUN_141343780: the spring at +0x2040, its velocity at +0x2050.
    SmoothDamp4(reinterpret_cast<float*>(&this->m_2040[0]), reinterpret_cast<float*>(&this->m_2040[2]), target,
        1.0f, dt, 0.0f);
    this->m_2060 = static_cast<uint32_t>(DNField<int32_t>(this, 0x50));
    auto spring = reinterpret_cast<float*>(&this->m_2040[0]);
    DNField<uint32_t>(this, 0x250) = Desaturate(DNField<uint32_t>(this, 0x250), (spring[0] - 1.0f) * spring[1] + 1.0f);
    DNField<uint32_t>(this, 0x15b4) = Desaturate(DNField<uint32_t>(this, 0x15b4), (spring[0] - 1.0f) * spring[2] + 1.0f);
    DNField<uint32_t>(this, 0x15b0) = Desaturate(DNField<uint32_t>(this, 0x15b0), (spring[0] - 1.0f) * spring[3] + 1.0f);
    if (this->m_light.Field<uint32_t>(0x15f0) != 0) {
        float farClip = WorldSceneFarClip(static_cast<CWorldScene*>(this->m_m2Scene->m_owner));
        this->m_sky.Compute(lightA, fogA, DNField<float>(this, 0xc), DNField<float>(this, 0x40),
            DNField<uint8_t>(this, 0x58), DNField<uint32_t>(this, 0x5c), DNField<float>(this, 0x98),
            DNField<float>(this, 0x9c), farClip);
    }
    DNField<uint8_t>(this, 0x92) = 0;
    DNField<uint64_t>(this, 0x98) = 0;
    uint32_t water = DNField<uint32_t>(this, 0xaac);
    DNField<uint32_t>(this, 0x18e4) = water;
    DNField<uint32_t>(this, 0x174c) = water;
    uint32_t band = DNField<uint32_t>(this, 0x1b60);
    if (9 <= band) {
        band = 9;
    }
    DNField<uint32_t>(this, 0x1adc) = DNField<uint32_t>(this, 0xa84 + band * 4);
    DNField<uint8_t>(this, 0x1adf) = 0xff;
    DNField<uint32_t>(this, 0x198c) = water;
    DNField<uint32_t>(this, 0x1a34) = water;
    DNField<uint32_t>(this, 0x1824) = water;
    uint8_t tint = DNField<uint8_t>(this, 0x58);
    if (static_cast<float>(tint) != 0.0f) {
        uint32_t color = DNField<uint32_t>(this, 0x5c);
        uint64_t fogInfos = DNField<uint64_t>(this, 0x1a0);
        for (uint64_t i = 0; i < fogInfos; i = static_cast<uint32_t>(i + 1)) {
            auto info = reinterpret_cast<uint32_t*>(DNField<uint8_t*>(this, 0x198) + i * 0xa0);
            ColorLerpByte(info, DNField<uint8_t>(this, 0x58), color);
        }
        static const size_t s_tinted[] = {0x1590, 0x158c, 0x250, 0x15b4, 0x15b0, 0x18e4, 0x198c, 0x1a34};
        for (size_t offset : s_tinted) {
            ColorLerpByte(&DNField<uint32_t>(this, offset), DNField<uint8_t>(this, 0x58), DNField<uint32_t>(this, 0x5c));
        }
    }
    float weatherBlend = DNField<float>(this, 0xe8);
    if (0.0f < weatherBlend) {
        auto alpha = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint((1.0f - weatherBlend) * 255.0f)));
        DNField<uint8_t>(this, 0x18e7) = alpha;
        DNField<uint8_t>(this, 0x174f) = alpha;
        DNField<uint8_t>(this, 0x198f) = alpha;
        DNField<uint8_t>(this, 0x1827) = alpha;
        DNField<uint8_t>(this, 0x1a37) = alpha;
    }

    // The override skies the current light lists, newest first, then its grading skies.
    uint32_t slots = 0;
    auto clear = &DNField<float>(this, 0xb1c);
    *clear = 1.0f;
    float remaining = 1.0f;
    for (int64_t i = static_cast<int32_t>(lightA->Word(0x79) - 1); 0 <= i; i--) {
        uint8_t* entry = lightA->m_data + 0xe4 + i * 0x20;
        if (entry[0x1c] == 0) {
            continue;
        }
        float entryWeight = DNField<float>(entry, 0x18);
        for (uint32_t k = 0; k < 3 && slots < 3; k++) {
            uint32_t id = DNField<uint32_t>(entry, k * 8);
            if (id != 0) {
                AddSkybox(this, id, &slots, clear, DNField<float>(entry, k * 8 + 4) * entryWeight * remaining, true);
            }
        }
        remaining *= 1.0f - entryWeight;
        if (remaining <= EPSILON) {
            break;
        }
    }
    for (uint32_t k = 0; k < 3; k++) {
        uint32_t id = lightA->Word(0x7b + k * 2);
        if (id != 0) {
            AddSkybox(this, id, &slots, clear, remaining * lightA->Float(0x7c + k * 2), false);
        }
    }
    for (uint32_t i = slots; i < 3; i++) {
        auto sky = DNField<uint8_t*>(this, 0xb8 + i * 8);
        if (sky != nullptr) {
            DNField<uint32_t>(sky, 0x4c) = 0;
        }
    }
    DNField<float>(this, 0x728) = DNFloatBand(s_band780, 4, DNField<float>(this, 0xc));
}

// ref: FUN_14245a940
void DNScene::StoreLightColors() {
    uint32_t ambientA = DNField<uint32_t>(this, 0xa7c);
    uint32_t directA = DNField<uint32_t>(this, 0xa70);
    DNField<uint32_t>(this, 0x1598) = DNField<uint32_t>(this, 0xa74);
    DNField<uint32_t>(this, 0x1594) = DNField<uint32_t>(this, 0xa78);
    DNField<uint32_t>(this, 0x15bc) = DNField<uint32_t>(this, 0xf64);
    DNField<uint32_t>(this, 0x15b8) = DNField<uint32_t>(this, 0xf68);
    uint32_t directB = DNField<uint32_t>(this, 0xf60);
    uint32_t ambientB = DNField<uint32_t>(this, 0xf6c);
    DNField<uint32_t>(this, 0x158c) = ambientA;
    DNField<uint32_t>(this, 0x1590) = directA;
    DNField<uint32_t>(this, 0x15b4) = directB;
    DNField<uint32_t>(this, 0x15b0) = ambientB;
    // Halfway between the two colours, brightened by 16 per channel (saturating).
    auto halfway = [](uint32_t from, uint32_t to) {
        uint32_t result = 0;
        for (uint32_t shift = 0; shift < 24; shift += 8) {
            uint32_t a = (from >> shift) & 0xff;
            uint32_t b = (to >> shift) & 0xff;
            auto channel = static_cast<uint8_t>(static_cast<uint8_t>(((b - a) << 7) >> 8) + a);
            result |= static_cast<uint32_t>(channel) << shift;
        }
        return result;
    };
    auto brighten = [](uint32_t color) {
        uint32_t sum = color - 0xefeff0u;
        uint32_t carry = ((sum ^ color) ^ 0xfffefeffu) & 0x1010100u;
        sum -= carry;
        return (carry - (carry >> 8)) | sum;
    };
    DNField<uint32_t>(this, 0x15a0) = brighten(halfway(directA, ambientA));
    DNField<uint32_t>(this, 0x159c) = halfway(ambientA, directA);
    DNField<uint32_t>(this, 0x15c4) = brighten(halfway(directB, ambientB));
    DNField<uint32_t>(this, 0x15c0) = halfway(ambientB, directB);
    std::memmove(this->m_light.m_data + 0x1538, this->m_light.m_data + 0x1580, 0x48);
    uint32_t r = (directA >> 16) & 0xff;
    uint32_t g = (directA >> 8) & 0xff;
    uint32_t b = directA & 0xff;
    DNField<uint32_t>(this, 0x15ec) = (((r + 3) * 0x5500 | (directA & 0xff00ffff)) & 0xffff0000)
        | (((g + 3) * 0x55) & 0xffffff00) | (((b + 3) * 0x55) >> 8);
    DNField<uint8_t>(this, 0x15ef) = DNField<uint8_t>(this, 0xa82);
    if (DNField<uint32_t>(this, 0x15f4) == 0) {
        DNField<uint32_t>(this, 0x1590) = 0xffffffff;
        DNField<uint32_t>(this, 0x158c) = 0;
        DNField<uint32_t>(this, 0x159c) = 0;
        DNField<uint32_t>(this, 0x15a0) = 0xffffffff;
        std::memmove(this->m_light.m_data + 0x15a4, this->m_light.m_data + 0x1580, 0x24);
    }
}

namespace {

const char s_fogInfoTag[] = ".?AUFogInfo@@";
typedef blz::vector<MODELFOG, s_fogInfoTag> FOGINFOVECTOR;

constexpr float FOG_FAR_FLOOR = 277.5f;     // DAT_142979c24
constexpr float FOG_NEAR_MAX = 3000.0f;     // DAT_1427f9054
constexpr float FOG_DENSITY_SCALE = 0.0005f;    // DAT_142722fec, DAT_142979c30
constexpr float WATER_FOG_MINIMUM = 30.0f;  // DAT_142724e98

// A scene fog's word as a float or a colour.
float& FogF(DNScene* scene, size_t fog, int32_t word) {
    return DNField<float>(scene, fog + word * 4);
}

uint32_t& FogW(DNScene* scene, size_t fog, int32_t word) {
    return DNField<uint32_t>(scene, fog + word * 4);
}

// A scene fog from a CurrentFog and the light's fog colours (its words 0xa..0xe and 0x2d).
void FogFromCurrent(DNScene* scene, uint32_t* fog, const DNCURRENTFOG* current, const DNCURRENTLIGHT* light,
    float fogFar, float nearLimit) {
    auto f = reinterpret_cast<float*>(fog);
    auto c = current->m_values;
    auto words = reinterpret_cast<const uint32_t*>(light->m_data);
    f[2] = std::min(c[0], fogFar);
    f[1] = std::min(c[0], nearLimit) * c[1];
    f[4] = c[2];
    f[0x26] = c[3];
    fog[0] = words[0xa];
    fog[0xc] = words[0xb];
    fog[0xe] = words[0xd];
    fog[0xd] = words[0xc];
    fog[0x25] = words[0xe];
    std::memcpy(&f[5], &c[4], 6 * 4);
    std::memcpy(&f[0x17], &c[10], 12 * 4);
    f[0x12] = c[0x18];
    fog[0xb] = words[0x2d];
    f[0xf] = c[0x16];
    std::memcpy(&f[0x13], &DNField<float>(scene, 0xa0), 12);
    f[0x27] = c[0x19];
}

// A scene fog from the water's fog parameters (end, near scale, colour).
// ref: FUN_1424593a0
void FogFromWater(DNScene* scene, uint32_t* fog, const float* water) {
    auto f = reinterpret_cast<float*>(fog);
    float end = std::min(DNField<float>(scene, 0x44), water[0]);
    float start = end * water[1];
    f[2] = end;
    f[1] = start;
    uint32_t color;
    std::memcpy(&color, &water[2], 4);
    fog[0] = color;
    f[4] = 1.0f;
    fog[0xe] = color;
    fog[0xc] = color;
    fog[0xd] = color;
    fog[0x25] = color;
    if (end < WATER_FOG_MINIMUM) {
        f[2] = WATER_FOG_MINIMUM;
        end = WATER_FOG_MINIMUM;
    }
    float density = 1.0f;
    if (DNField<uint8_t>(scene, 4) == 0) {
        density = FogDensity(scene, start, end);
        f[4] = density;
        f[2] = DNField<float>(scene, 0x44);
        if (f[1] < 0.0f) {
            f[1] = 0.0f;
        }
    }
    f[7] = density;
    f[0x26] = 0.0f;
    f[6] = 1.0f;
    f[8] = 0.0f;
    f[5] = -10000.0f;
    f[0x12] = 1.0f;
    f[0xf] = 10000.0f;
}

}  // namespace

// ref: FUN_14245b980
void DNScene::BuildFogs() {
    float farClip = DNField<float>(this, 0x44);
    float smooth = std::min(farClip, this->m_1c3c);
    float target = std::min(farClip, this->m_1c40);
    this->m_1c3c = smooth;
    this->m_1c40 = target;
    if (smooth != target) {
        float dt = FrameTime_GetElapsedSeconds();
        target = this->m_1c40;
        smooth = this->m_1c3c;
        if (target < smooth) {
            smooth -= dt * 2000.0f;                 // DAT_142738438
            this->m_1c3c = smooth;
            if (smooth < target) {
                this->m_1c3c = target;
                smooth = target;
            }
        } else {
            smooth = dt * 1000.0f + smooth;         // DAT_14261b418
            this->m_1c3c = smooth;
            if (target < smooth) {
                this->m_1c3c = target;
                smooth = target;
            }
        }
    }
    farClip = DNField<float>(this, 0x44);
    float nearLimit = std::min(farClip, FOG_NEAR_MAX);
    this->m_1c40 = farClip;
    float fogFar = std::min(std::max(smooth, FOG_FAR_FLOOR), farClip);
    auto infos = reinterpret_cast<FOGINFOVECTOR*>(this->m_light.m_data + 0x198);
    auto currents = reinterpret_cast<DNFOGVECTOR*>(this->m_light.m_data + 0x1450);
    auto count = static_cast<uint32_t>(currents->m_size);
    infos->resize(count);
    float glare = DNField<float>(this, 0x17e8) * DNField<float>(this, 0xb20) * DNField<float>(this, 0xb28);
    DNField<float>(this, 0x138) = glare;
    for (uint32_t i = 0; i < count; i++) {
        infos->m_data[i].Set(0x10, DNField<float>(this, 0x17e8) * DNField<float>(this, 0xb20) * DNField<float>(this, 0xb28));
    }
    auto lightA = reinterpret_cast<DNCURRENTLIGHT*>(this->m_light.m_data + 0xa70);
    auto lightB = reinterpret_cast<DNCURRENTLIGHT*>(this->m_light.m_data + 0xf60);
    auto fogA = reinterpret_cast<DNCURRENTFOG*>(this->m_light.m_data + 0x1468);
    auto fogB = reinterpret_cast<DNCURRENTFOG*>(this->m_light.m_data + 0x14d0);
    if (this->m_1bf8 == 0) {
        for (uint32_t i = 0; i < count; i++) {
            FogFromCurrent(this, infos->m_data[i].m_words, &currents->m_data[i], lightA, fogFar, nearLimit);
            infos->m_data[i].m_words[0x16] = i + 1;
        }
        FogFromCurrent(this, &FogW(this, 0xf8, 0), fogA, lightA, fogFar, nearLimit);
        FogFromCurrent(this, &FogW(this, 0x250, 0), fogB, lightB, fogFar, nearLimit);
    } else {
        // The world's fog override (+0x1bfc..+0x1c38).
        FogF(this, 0xf8, 1) = DNField<float>(this, 0x1bfc);
        float end = DNField<float>(this, 0x1c00);
        float density = DNField<float>(this, 0x1c08);
        FogF(this, 0xf8, 2) = std::min(end, fogFar);
        FogW(this, 0xf8, 5) = DNField<uint32_t>(this, 0x1c14);
        FogW(this, 0xf8, 6) = DNField<uint32_t>(this, 0x1c18);
        FogW(this, 0xf8, 8) = DNField<uint32_t>(this, 0x1484);
        FogW(this, 0xf8, 9) = DNField<uint32_t>(this, 0x1488);
        FogW(this, 0xf8, 10) = DNField<uint32_t>(this, 0x148c);
        uint32_t color = DNField<uint32_t>(this, 0x1c0c);
        uint32_t secondary = DNField<uint32_t>(this, 0x1c10);
        FogW(this, 0xf8, 0xc) = (secondary & 0xffffff) != 0 ? secondary : color;
        FogF(this, 0xf8, 4) = density;
        FogW(this, 0xf8, 0x26) = 0;
        FogW(this, 0xf8, 0) = color;
        std::memcpy(&FogW(this, 0xf8, 0x17), &DNField<uint32_t>(this, 0x1c1c), 0x10);
        uint32_t third = DNField<uint32_t>(this, 0x1c38);
        if ((third & 0xffffff) == 0) {
            third = DNField<uint32_t>(this, 0xaa0);
        }
        FogW(this, 0xf8, 0xd) = third;
        std::memcpy(&FogW(this, 0xf8, 0x13), &DNField<uint32_t>(this, 0xa0), 12);
        FogW(this, 0xf8, 0xb) = DNField<uint32_t>(this, 0xb24);
        if (DNField<uint8_t>(this, 0x1c2c) != 0) {
            density = DNField<float>(this, 0x1c30);
            color = DNField<uint32_t>(this, 0x1c34);
        }
        FogW(this, 0xf8, 0xe) = color;
        FogF(this, 0xf8, 7) = density;
        if (DNField<float>(this, 0x1c04) != 0.0f) {
            end = DNField<float>(this, 0x1c04);
        }
        FogF(this, 0xf8, 0xf) = end;
        uint32_t index = FogW(this, 0x250, 0x16);
        std::memcpy(&FogW(this, 0x250, 0), &FogW(this, 0xf8, 0), 0xa0);
        FogW(this, 0x250, 0x16) = index;
    }

    // Under water: the water's fog (+0xa4c, +0xa58), unless the liquid opts out.
    const uint8_t* liquid = nullptr;
    bool water = false;
    if (DNField<uint8_t>(this, 0xa30) != 0) {
        bool useWater = false;
        int32_t liquidId = DNField<int32_t>(this, 0x38);
        if (liquidId == 0) {
            useWater = true;
        } else {
            bool local = false;
            liquid = static_cast<const uint8_t*>(g_liquidTypeDB.GetRecord(static_cast<uint32_t>(liquidId), false, &local));
            if (liquid != nullptr) {
                water = true;
                uint16_t flags = DB2FieldU16(liquid + 0x1c, 2, 0x1c, &g_liquidTypeDB);
                if ((flags & 0x20) != 0 && (DNField<uint32_t>(this, 0xa34) & 0x100) == 0) {
                    water = false;
                }
                flags = DB2FieldU16(liquid + 0x1c, 2, 0x1c, &g_liquidTypeDB);
                if (((flags & 0x100) == 0 || (DNField<uint8_t>(this, 0xa34) & 0x10) != 0) && water) {
                    useWater = true;
                }
            }
        }
        if (useWater) {
            FogFromWater(this, &FogW(this, 0x434, 0), &DNField<float>(this, 0xa4c));
            FogFromWater(this, &FogW(this, 0x4d4, 0), &DNField<float>(this, water ? 0xa58 : 0xa4c));
            if (DNField<int32_t>(this, 0) == 0x673) {
                float end = std::min(DNField<float>(this, 0xa4c), 5554.0f);    // DAT_142979c28
                if (FOG_DENSITY_BASE == FogF(this, 0x434, 4) && 500.0f < end) {   // DAT_14261a9bc
                    FogF(this, 0x434, 4) = FOG_DENSITY_BASE - (end - 500.0f) * 0.00029673590f;   // DAT_142979c0c
                }
            }
            if (water && liquid != nullptr
                && (DB2FieldU16(liquid + 0x1c, 2, 0x1c, &g_liquidTypeDB) & 0x40) != 0) {
                FogW(this, 0x250, 2) = FogW(this, 0x4d4, 2);
                FogW(this, 0x250, 1) = FogW(this, 0x4d4, 1);
                FogW(this, 0x250, 4) = FogW(this, 0x4d4, 4);
                FogW(this, 0x250, 0) = FogW(this, 0x4d4, 0);
                FogW(this, 0x250, 0x26) = 0;
            }
        } else {
            std::memcpy(&FogW(this, 0x434, 0), &FogW(this, 0xf8, 0), 0xa0);
            FogW(this, 0x434, 0x16) = 0;
            std::memcpy(&FogW(this, 0x4d4, 0), &FogW(this, 0x250, 0), 0xa0);
            FogW(this, 0x4d4, 0x16) = 6;
        }
    }

    // The blend toward the water fog while surfacing (+0xa68).
    float w = DNField<float>(this, 0xa68) * 0.04f;          // DAT_1426d3078
    float blend = 0.0f;
    if (0.0f <= w) {
        blend = std::min(w, 1.0f);
    }
    if (DNField<uint8_t>(this, 0x90) != 0 || DNField<uint8_t>(this, 0x91) != 0) {
        blend *= 1.0f - DNField<float>(this, 0x68);
    }
    DNField<float>(this, 0x2f0) = blend;
    float waterScale = 0.001f;                              // DAT_142618f58
    if (DNField<uint8_t>(this, 0xa30) != 0 && DNField<uint8_t>(this, 0xa64) != 0) {
        waterScale = (blend + 1.0f) * FOG_DENSITY_SCALE;
        auto alpha = static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(blend * 255.0f))));
        auto lerpF = [&](size_t dst, size_t a, size_t b, int32_t word) {
            FogF(this, dst, word) = Lerp(FogF(this, a, word), FogF(this, b, word), blend);
        };
        auto lerpC = [&](size_t dst, size_t a, size_t b, int32_t word) {
            uint32_t color = FogW(this, a, word);
            ColorLerpByte(&color, alpha, FogW(this, b, word));
            FogW(this, dst, word) = color;
        };
        // 0x2f4 between the surface fog (0xf8) and the water's (0x434).
        FogW(this, 0x2f4, 0x26) = FogW(this, 0xf8, 0x26);
        FogW(this, 0x2f4, 9) = FogW(this, 0xf8, 9);
        FogW(this, 0x2f4, 10) = FogW(this, 0xf8, 10);
        lerpF(0x2f4, 0xf8, 0x434, 2);
        lerpF(0x2f4, 0xf8, 0x434, 1);
        lerpF(0x2f4, 0xf8, 0x434, 4);
        std::memcpy(&FogW(this, 0x2f4, 0x1b), &FogW(this, 0xf8, 0x1b), 0x20);
        lerpF(0x2f4, 0xf8, 0x434, 3);
        lerpC(0x2f4, 0xf8, 0x434, 0);
        if (-9999.0f <= FogF(this, 0x434, 5)) {                 // DAT_142979c2c
            FogF(this, 0x2f4, 5) = blend * blend * blend * (FogF(this, 0x434, 5) - FogF(this, 0xf8, 5))
                + FogF(this, 0xf8, 5);
            lerpF(0x2f4, 0xf8, 0x434, 7);
            lerpF(0x2f4, 0xf8, 0x434, 6);
            lerpF(0x2f4, 0xf8, 0x434, 8);
            for (int32_t word = 0x17; word <= 0x1a; word++) {
                lerpF(0x2f4, 0xf8, 0x434, word);
            }
        } else {
            FogF(this, 0x2f4, 5) = FogF(this, 0xf8, 5);
            FogF(this, 0x2f4, 6) = FogF(this, 0xf8, 6);
            FogF(this, 0x2f4, 8) = (1.0f - blend) * FogF(this, 0xf8, 8);
            FogF(this, 0x2f4, 7) = Lerp(FogF(this, 0xf8, 7), FogF(this, 0x434, 4), blend);
            std::memcpy(&FogW(this, 0x2f4, 0x17), &FogW(this, 0xf8, 0x17), 0x10);
        }
        lerpF(0x2f4, 0xf8, 0x434, 0xb);
        lerpC(0x2f4, 0xf8, 0x434, 0xc);
        lerpC(0x2f4, 0xf8, 0x434, 0xd);
        lerpC(0x2f4, 0xf8, 0x434, 0xe);
        lerpF(0x2f4, 0xf8, 0x434, 0xf);
        lerpF(0x2f4, 0xf8, 0x434, 0x10);
        std::memcpy(&FogW(this, 0x2f4, 0x13), &DNField<uint32_t>(this, 0xa0), 12);
        FogF(this, 0x2f4, 0x12) = Lerp(FogF(this, 0xf8, 0x12), 1.0f, blend);
        // 0x394 between the second fog (0x250) and the water's (0x4d4).
        lerpF(0x394, 0x250, 0x4d4, 2);
        lerpF(0x394, 0x250, 0x4d4, 1);
        lerpF(0x394, 0x250, 0x4d4, 4);
        lerpF(0x394, 0x250, 0x4d4, 3);
        FogW(this, 0x394, 0x26) = FogW(this, 0x250, 0x26);
        FogW(this, 0x394, 9) = FogW(this, 0x250, 9);
        FogW(this, 0x394, 10) = FogW(this, 0x250, 10);
        std::memcpy(&FogW(this, 0x394, 0x1b), &FogW(this, 0x250, 0x1b), 0x20);
        lerpC(0x394, 0x250, 0x4d4, 0);
        for (int32_t word = 5; word <= 8; word++) {
            lerpF(0x394, 0x250, 0x4d4, word);
        }
        for (int32_t word = 0x17; word <= 0x1a; word++) {
            lerpF(0x394, 0x250, 0x4d4, word);
        }
        lerpF(0x394, 0x250, 0x4d4, 0xb);
        lerpC(0x394, 0x250, 0x4d4, 0xc);
        lerpC(0x394, 0x250, 0x4d4, 0xd);
        lerpC(0x394, 0x250, 0x4d4, 0xe);
        lerpF(0x394, 0x250, 0x4d4, 0xf);
        lerpF(0x394, 0x250, 0x4d4, 0x10);
        std::memcpy(&FogW(this, 0x394, 0x13), &DNField<uint32_t>(this, 0xa0), 12);
        FogF(this, 0x394, 0x12) = Lerp(FogF(this, 0x250, 0x12), 1.0f, blend);
        // The blends back into the fogs the scene draws with.
        static const int32_t s_backA[] = {2, 1, 4, 0x26, 9, 10, 5, 6, 0x17, 0x18, 0x19, 0x1a, 7, 8, 0x1b, 0x1c, 0x1d,
            0x1e, 0x1f, 0x20, 0x21, 0x22, 0xb, 0x10, 0x12};
        for (int32_t word : s_backA) {
            FogW(this, 0xf8, word) = FogW(this, 0x2f4, word);
        }
        std::memcpy(&FogW(this, 0xf8, 0x13), &DNField<uint32_t>(this, 0xa0), 12);
        FogW(this, 0x250, 2) = FogW(this, 0x394, 2);
        FogW(this, 0x250, 1) = FogW(this, 0x394, 1);
        FogW(this, 0x250, 4) = FogW(this, 0x394, 4);
        FogW(this, 0x250, 0x26) = FogW(this, 0x394, 0x26);
        std::memcpy(&FogW(this, 0x250, 0x13), &DNField<uint32_t>(this, 0xa0), 12);
    } else {
        if (DNField<uint8_t>(this, 0xa64) != 0) {
            waterScale = (blend + 1.0f) * FOG_DENSITY_SCALE;
        }
        std::memcpy(&FogW(this, 0x2f4, 0), &FogW(this, 0xf8, 0), 0xa0);
        FogW(this, 0x2f4, 0x16) = 0;
        std::memcpy(&FogW(this, 0x394, 0), &FogW(this, 0x250, 0), 0xa0);
        FogW(this, 0x394, 0x16) = 6;
    }

    // Densities at least a half, the second fogs' doubled when the light is live, their
    // starts set to it, and all of them scaled to the draw's units.
    for (uint32_t i = 0; i < count; i++) {
        infos->m_data[i].Set(4, std::max(infos->m_data[i].Get(4), 0.5f));   // DAT_14261a988
    }
    static const size_t s_floored[] = {0x250, 0xf8, 0x2f4, 0x394, 0x434, 0x4d4};
    for (size_t fog : s_floored) {
        FogF(this, fog, 4) = std::max(FogF(this, fog, 4), 0.5f);
    }
    if (DNField<uint8_t>(this, 4) == 0) {
        FogF(this, 0x250, 4) += FogF(this, 0x250, 4);
        FogF(this, 0x394, 4) += FogF(this, 0x394, 4);
    }
    FogF(this, 0xf8, 3) = FogF(this, 0xf8, 4);
    FogF(this, 0x250, 3) = FogF(this, 0x250, 4);
    FogF(this, 0x2f4, 3) = FogF(this, 0x2f4, 4);
    FogF(this, 0x394, 3) = FogF(this, 0x394, 4);
    for (uint32_t i = 0; i < count; i++) {
        infos->m_data[i].Set(4, infos->m_data[i].Get(4) * FOG_DENSITY_SCALE);
        infos->m_data[i].Set(7, infos->m_data[i].Get(7) * FOG_DENSITY_SCALE);
    }
    FogF(this, 0xf8, 4) *= FOG_DENSITY_SCALE;
    FogF(this, 0x250, 4) *= FOG_DENSITY_SCALE;
    FogF(this, 0xf8, 7) *= FOG_DENSITY_SCALE;
    FogF(this, 0x250, 7) *= FOG_DENSITY_SCALE;
    static const size_t s_waterScaled[] = {0x2f4, 0x394, 0x434, 0x4d4};
    for (size_t fog : s_waterScaled) {
        FogF(this, fog, 4) *= waterScale;
    }
    for (size_t fog : s_waterScaled) {
        FogF(this, fog, 7) *= waterScale;
    }
    std::memcpy(&FogW(this, 0x1b0, 0), &FogW(this, 0xf8, 0), 0xa0);
    FogF(this, 0x1b0, 0x10) = 1.0f;
    FogW(this, 0x1b0, 0x16) = 5;
    static const size_t s_weighted[] = {0xf8, 0x1b0, 0x250, 0x2f4, 0x394, 0x434, 0x4d4};
    for (size_t fog : s_weighted) {
        FogF(this, fog, 0x27) = 1.0f;
    }
}

// The reference builds it inline wherever a CurrentFog is made.
DNCURRENTFOG::DNCURRENTFOG() {
    std::memcpy(this->m_values, s_fogDefaults, sizeof(s_fogDefaults));
    std::memset(&this->m_values[0x10], 0, 0x10);
    this->m_values[0x14] = 1.0f;
    this->m_values[0x15] = 0.0f;
    this->m_values[0x16] = 0.0f;
    this->m_values[0x17] = 0.0f;
    Bytes(this)[0x5c] = 1;
    this->m_values[0x18] = 1.0f;
    this->m_values[0x19] = 1.0f;
}

namespace {

// The day bands the bodies follow (time, value), DAT_142d4d760..DAT_142d4d910.
const float s_skyModelBand[] = {0.125f, 1.0f, 0.1875f, 0.0f, 0.9375f, 0.0f, 1.0f, 1.0f};
const float s_sunPhiBand[] = {0.0f, 2.2165682f, 0.25f, 1.9198622f, 0.5f, 2.2165682f, 0.75f, 1.9198622f};
const float s_sunThetaBand[] = {0.0f, 3.9269907f, 0.25f, 3.9269907f, 0.5f, 3.9269907f, 0.75f, 3.9269907f};
const float s_bodySunPhiBand[] = {0.22916667f, 1.7453293f, 0.49652779f, 0.087266460f, 0.5f, 0.087266460f,
    0.50347221f, 0.087266460f, 0.89583331f, 1.7453293f};
const float s_sunGlareBand[] = {0.21875f, 0.0f, 0.22916667f, 1.0f, 0.89583331f, 1.0f, 0.90625f, 0.0f};
const float s_sunStrengthBand[] = {0.25f, 0.0f, 0.27083334f, 1.0f, 0.85416669f, 1.0f, 0.875f, 0.0f};
const float s_bodySunThetaBand[] = {0.22916667f, 0.78539819f, 0.5f, 0.78539819f, 0.89583331f, 0.78539819f};
const float s_moonPhiBand[] = {0.0f, 0.61086524f, 0.0034722222f, 0.61086524f, 0.16666667f, 1.7453293f,
    0.91666669f, 1.7453293f, 0.99652779f, 0.61086524f};
const float s_moonThetaBand[] = {0.0f, 0.78539819f, 0.16666667f, 0.78539819f, 0.91666669f, 0.78539819f};
const float s_phasePhiBand[] = {0.0f, 0.61086524f, 0.0034722222f, 0.61086524f, 0.16666667f, 1.7453293f,
    0.91666669f, 1.7453293f, 0.99652779f, 0.61086524f};
const float s_phaseThetaBand[] = {0.0f, 2.3561945f, 0.16666667f, 2.6179938f, 0.91666669f, 2.8797932f};
const float s_sunSizeBand[] = {0.25f, 2.0f, 0.28125f, 1.0f, 0.84375f, 1.0f, 0.875f, 2.0f};
const float s_moonSizeBand[] = {0.041666668f, 1.0f, 0.16666667f, 1.5f, 0.91666669f, 1.5f, 0.99930555f, 1.0f};

constexpr float BODY_DISTANCE = 12.0f;      // DAT_142737e48
constexpr float DEGREES = 0.017453292f;     // DAT_14263a04c
constexpr float DAY_START = 0.22916667f;    // DAT_142979c10
constexpr float DAY_END = 0.91666669f;      // DAT_142979c18

// A direction from its polar angle and its azimuth.
void Polar(float phi, float theta, float* out) {
    float s = FastSin(phi);
    out[2] = FastCos(phi);
    out[1] = FastSin(theta) * s;
    out[0] = FastCos(theta) * s;
}

// The sky model's alpha over the day.
// ref: FUN_142458770
void SkyModelAlpha(DNScene* scene) {
    float value = DNFloatBand(s_skyModelBand, 4, DNField<float>(scene, 0xc));
    DNField<uint8_t>(scene, 0x1bf3) = static_cast<uint8_t>(static_cast<int32_t>(value * 254.0f + 1.0f));
}

// Every override sky's light ids are spent for the frame.
// ref: FUN_14245ff30
void ResetOverrideSkies(DNScene* scene) {
    void** table = scene->m_overrideTable;
    if (table == nullptr) {
        return;
    }
    for (uint64_t bucket = 0; bucket < scene->m_overrideBuckets; bucket++) {
        for (auto node = static_cast<uint8_t*>(table[bucket]); node != nullptr; node = DNField<uint8_t*>(node, 0)) {
            DNField<uint64_t>(DNField<uint8_t*>(node, 0x30), 0x60) = 0;
        }
    }
}

// A grading block faded toward the default grading as the camera goes under water.
// ref: FUN_14244fab0
void GradingFade(LIGHTPARAMSBLOCK* block, float depth) {
    if (depth <= 0.0f) {
        return;
    }
    LIGHTPARAMSBLOCK fallback = {};
    fallback.m_entries[0].m_id = DEFAULT_COLOR_GRADING;
    fallback.m_entries[0].m_weight = 1.0f;
    fallback.m_count = 1;
    float t = depth * 0.04f;                    // DAT_1426d3078
    if (1.0f <= t) {
        t = 1.0f;
    }
    BlendLightParams(block, &fallback, t);
}

// A grading block rendered when the blocks blend at all and the caller asks.
// ref: FUN_1424506d0
void GradingRender(DNColorGrading* grading, uint8_t* block, bool render, void* target, int32_t index) {
    if (s_blendParams && render) {
        grading->Blend(target, block, index);
    }
}

// The renderer's grading target (DAT_14348b658).
void* s_gradingTarget;

}  // namespace

// ref: FUN_142450110
void DNColorGrading::Blend(void* target, uint8_t* block, int32_t index) {
    (void)target;
    (void)block;
    (void)index;
    FROST_UNIMPLEMENTED();  // the grading LUTs blended into a render target
}

// ref: FUN_14245b7f0
void DNScene::ApplyColorGrading(void* env, bool fade, bool render) {
    auto lightA = reinterpret_cast<DNCURRENTLIGHT*>(this->m_light.m_data + 0xa70);
    auto lightB = reinterpret_cast<DNCURRENTLIGHT*>(this->m_light.m_data + 0xf60);
    if (fade) {
        bool alternate = DNField<uint8_t>(env, 0x5bc) != 0;
        DNCURRENTLIGHT* light = alternate ? lightB : lightA;
        GradingFade(ParamsBlock(light, 0x20c), DNField<float>(this, 0xa68));
        GradingFade(ParamsBlock(light, 0x310), DNField<float>(this, 0xa68));
        if (DNField<uint8_t>(this, 0x71c) != 0) {
            auto overrideBlock = reinterpret_cast<LIGHTPARAMSBLOCK*>(this->m_light.m_data + 0x618);
            float t = DNField<float>(this, 0x720);
            BlendLightParams(ParamsBlock(lightA, 0x20c), overrideBlock, t);
            BlendLightParams(ParamsBlock(lightB, 0x20c), overrideBlock, t);
            BlendLightParams(ParamsBlock(lightA, 0x310), overrideBlock, t);
            BlendLightParams(ParamsBlock(lightB, 0x310), overrideBlock, t);
        }
    }
    void* target = render ? s_gradingTarget : nullptr;
    bool alternate = DNField<uint8_t>(env, 0x5bc) != 0;
    DNCURRENTLIGHT* light = alternate ? lightB : lightA;
    GradingRender(this->m_colorGrading, light->m_data + 0x20c, render, target, 0);
    GradingRender(this->m_colorGrading, light->m_data + 0x310, render, target, 1);
}

// ref: FUN_14245a6a0
void DNScene::ComputeSunDirection(const float* viewMatrix) {
    float time = DNField<float>(this, 0xc);
    float phi = DNFloatBand(s_sunPhiBand, 4, time);
    float theta = DNFloatBand(s_sunThetaBand, 4, time);
    if (DNField<uint8_t>(this, 0xb48) != 0) {
        float weight = DNField<float>(this, 0xb4c);
        theta = (DNField<float>(this, 0xb50) * DEGREES + 3.1415927f) * weight + (1.0f - weight) * theta;
    }
    float s = FastSin(phi);
    float x = FastCos(theta) * s;
    float y = FastSin(theta) * s;
    float z = FastCos(phi);
    DNField<float>(this, 0x1580) = x;
    DNField<float>(this, 0x1584) = y;
    DNField<float>(this, 0x1588) = z;
    DNField<float>(this, 0x15a4) = x;
    DNField<float>(this, 0x15a8) = y;
    DNField<float>(this, 0x15ac) = z;
    float m[9];
    Matrix3FromMatrix(m, viewMatrix);
    float dx = DNField<float>(this, 0x18d8) - DNField<float>(this, 0x14);
    float dy = DNField<float>(this, 0x18dc) - DNField<float>(this, 0x18);
    float dz = DNField<float>(this, 0x18e0) - DNField<float>(this, 0x1c);
    float scale = 1.0f / std::sqrt(dx * dx + dy * dy + dz * dz);
    dy *= scale;
    dx *= scale;
    dz *= scale;
    DNField<float>(this, 0xa0) = dy * m[3] + dx * m[0] + dz * m[6];
    DNField<float>(this, 0xa4) = dx * m[1] + dy * m[4] + dz * m[7];
    DNField<float>(this, 0xa8) = dy * m[5] + dx * m[2] + dz * m[8];
}

// ref: FUN_14245aca0
void DNScene::ComputeBodies() {
    float time = DNField<float>(this, 0xc);
    DNField<float>(this, 0x94) = DNFloatBand(s_sunGlareBand, 4, time);
    DNField<float>(this, 0xb20) = DNFloatBand(s_sunStrengthBand, 4, DNField<float>(this, 0xc)) * DNField<float>(this, 0xb20);
    float phi = DNFloatBand(s_bodySunPhiBand, 5, DNField<float>(this, 0xc));
    float theta = DNFloatBand(s_bodySunThetaBand, 3, DNField<float>(this, 0xc));
    if (DNField<uint8_t>(this, 0xb48) != 0) {
        float weight = DNField<float>(this, 0xb4c);
        theta = DNField<float>(this, 0xb50) * DEGREES * weight + (1.0f - weight) * theta;
    }
    auto sun = &DNField<float>(this, 0x18d8);
    float local[3];
    Polar(phi, theta, local);
    float t = DNField<float>(this, 0xc);
    if ((DNField<uint8_t>(this, 0xb38) & 8) != 0 && DAY_START <= t && t <= DAY_END) {
        auto from = &DNField<float>(this, 0xb2c);
        float blend = DNField<float>(this, 0xb40);
        for (int32_t i = 0; i < 3; i++) {
            sun[i] = (local[i] - from[i]) * blend + from[i];
        }
    } else {
        sun[0] = local[0];
        sun[1] = local[1];
        sun[2] = local[2];
    }
    float x = sun[0];
    float y = sun[1];
    float z = sun[2];
    float length = x * x + y * y + z * z;
    if (1e-05f < length) {                          // DAT_1426c75c4
        float scale = BODY_DISTANCE / std::sqrt(length);
        x *= scale;
        y *= scale;
        z *= scale;
        sun[0] = x;
        sun[1] = y;
        sun[2] = z;
    }
    sun[0] = x + DNField<float>(this, 0x14);
    sun[1] = y + DNField<float>(this, 0x18);
    sun[2] = z + DNField<float>(this, 0x1c);
    DNField<float>(this, 0x1930) = DNFloatBand(s_sunSizeBand, 4, DNField<float>(this, 0xc)) * DNField<float>(this, 0x1934);
    std::memcpy(&DNField<float>(this, 0x1740), sun, 12);

    // The moon.
    float moonPhi = DNFloatBand(s_moonPhiBand, 5, DNField<float>(this, 0xc));
    float moonTheta = DNFloatBand(s_moonThetaBand, 3, DNField<float>(this, 0xc));
    auto moon = &DNField<float>(this, 0x1980);
    Polar(moonPhi, moonTheta, local);
    moon[1] = local[1];
    moon[2] = local[2];
    moon[0] = local[0];
    Polar(moonPhi, moonTheta, local);
    t = DNField<float>(this, 0xc);
    if ((DNField<uint8_t>(this, 0xb38) & 8) != 0 && (t < DAY_START || DAY_END < t)) {
        auto from = &DNField<float>(this, 0xb2c);
        float blend = DNField<float>(this, 0xb40);
        for (int32_t i = 0; i < 3; i++) {
            moon[i] = (local[i] - from[i]) * blend + from[i];
        }
    } else {
        moon[1] = local[1];
        moon[2] = local[2];
        moon[0] = local[0];
    }
    float scale = BODY_DISTANCE / std::sqrt(moon[0] * moon[0] + moon[1] * moon[1] + moon[2] * moon[2]);
    moon[1] *= scale;
    moon[2] *= scale;
    moon[0] = moon[0] * scale + DNField<float>(this, 0x14);
    moon[1] += DNField<float>(this, 0x18);
    moon[2] += DNField<float>(this, 0x1c);
    float moonSize = DNFloatBand(s_moonSizeBand, 4, DNField<float>(this, 0xc)) * DNField<float>(this, 0x19dc);
    DNField<float>(this, 0x19d8) = moonSize;
    std::memcpy(&DNField<float>(this, 0x1818), moon, 12);
    DNField<float>(this, 0x18a8) = moonSize;
    DNField<float>(this, 0x18ac) = moonSize;

    // The far body and the moon's phase over its cycle (+0x1a88 days).
    std::memcpy(&DNField<float>(this, 0x1ad0), &DNField<float>(this, 0x1b64), 12);
    DNField<float>(this, 0x1ad0) = DNField<float>(this, 0x14) + DNField<float>(this, 0x1ad0);
    DNField<float>(this, 0x1ad4) += DNField<float>(this, 0x18);
    DNField<uint32_t>(this, 0x1b28) = DNField<uint32_t>(this, 0x1b2c);
    DNField<float>(this, 0x1ad8) += DNField<float>(this, 0x1c);
    float day = DNField<float>(this, 0x10);
    float cycle = DNField<float>(this, 0x1a88);
    t = DNField<float>(this, 0xc);
    auto now = static_cast<uint32_t>(static_cast<int32_t>(day * 65536.0f) - static_cast<int32_t>(t * -65536.0f));
    float inverse = 1.0f / cycle;
    float cycles = (t + day) * inverse;
    auto whole = static_cast<int32_t>(cycles);
    if (whole != static_cast<int32_t>(0x80000000) && static_cast<float>(whole) != cycles) {
        cycles = static_cast<float>(whole - (cycles < 0.0f ? 1 : 0));
    }
    auto start = static_cast<uint32_t>(static_cast<int32_t>(cycles * cycle * 65536.0f));
    uint32_t elapsed = now - (start < now ? start : now);
    float phase = static_cast<float>(static_cast<uint64_t>(elapsed)) * 1.5258789e-05f * inverse;
    float bodyPhi = DNFloatBand(s_phasePhiBand, 5, phase);
    float bodyTheta = DNFloatBand(s_phaseThetaBand, 3, phase);
    float s = FastSin(bodyPhi);
    float bx;
    float by;
    float bz;
    if ((DNField<uint8_t>(this, 0xb38) & 8) == 0) {
        bz = FastCos(bodyPhi);
        by = FastSin(bodyTheta) * s;
        bx = FastCos(bodyTheta) * s;
    } else {
        bz = -1.0f;
        by = 0.0f;
        bx = 0.0f;
    }
    DNField<float>(this, 0x1a28) = bx;
    float bodyScale = BODY_DISTANCE / std::sqrt(bx * bx + by * by + bz * bz);
    DNField<float>(this, 0x1a30) = bz * bodyScale;
    DNField<float>(this, 0x1a2c) = bodyScale * by;
    DNField<float>(this, 0x1a28) = bx * bodyScale + DNField<float>(this, 0x14);
    DNField<float>(this, 0x1a2c) += DNField<float>(this, 0x18);
    DNField<float>(this, 0x1a30) += DNField<float>(this, 0x1c);
    DNField<float>(this, 0x1a80) = DNFloatBand(s_moonSizeBand, 4, phase) * DNField<float>(this, 0x1a84);

    // Twilight: rising through the morning, falling through the evening, and the night's.
    t = DNField<float>(this, 0xc);
    float twilight = 0.0f;
    if (DAY_START <= t && t < 0.5f) {
        twilight = (t - DAY_START) * 3.6923075f;                    // DAT_142979c20
    } else if (0.5f <= t && t < 0.89583331f) {                       // DAT_142979c14
        twilight = 1.0f - (t - 0.5f) * 2.5263157f;                  // DAT_142979c1c
    } else if (DAY_END <= t) {
        twilight = t < 1.0f ? (t - DAY_END) * BODY_DISTANCE : 0.0f;
    } else if (0.0f <= t && t < 0.16666667f) {                      // DAT_142723008
        twilight = 1.0f - t * 6.0f;                                 // DAT_1426c8878
    }
    DNField<float>(this, 0x724) = twilight;
}

// ref: FUN_14245b520
void DNScene::Compute(int32_t timeMs, float elapsed, float timeSeconds, bool alive, void* env,
    const float* viewMatrix, bool colorGrading, bool zoneLightsSingly) {
    DNField<uint32_t>(this, 0xac) = DNField<uint32_t>(this, 0xb20);
    DNField<float>(this, 0x4c) = elapsed;
    DNField<uint32_t>(this, 0xb0) = DNField<uint32_t>(this, 0x138);
    DNField<float>(this, 0x48) = timeSeconds;
    DNField<int32_t>(this, 0x50) = timeMs;
    DNField<int32_t>(this, 0x6c) += static_cast<int32_t>(static_cast<int64_t>(elapsed * 1000.0f));
    ResetOverrideSkies(this);
    for (uint32_t i = 0; i < this->m_areaOverrides.m_count; i++) {
        AREAOVERRIDE* entry = &Overrides(this)[i];
        uint32_t step = static_cast<uint32_t>(DNField<int32_t>(this, 0x50) - entry->m_time);
        entry->m_time = DNField<int32_t>(this, 0x50);
        if (entry->m_state == 1) {
            float weight = static_cast<float>(static_cast<uint64_t>(step)) / static_cast<float>(static_cast<uint64_t>(entry->m_duration))
                + entry->m_weight;
            if (1.0f < weight) {
                weight = 1.0f;
            }
            entry->m_weight = weight;
        } else if (entry->m_state == 2) {
            float weight = entry->m_weight
                - static_cast<float>(static_cast<uint64_t>(step)) / static_cast<float>(static_cast<uint64_t>(entry->m_duration));
            if (weight < 0.0f) {
                weight = 0.0f;
            }
            entry->m_weight = weight;
        }
    }
    if (this->m_1602 != 0) {
        this->m_clouds.MarkDirty();
    }
    float heading = DNField<float>(this, 0x30);
    float x = DNField<float>(this, 0x2c);
    if (static_cast<double>(heading * heading + x * x) <= 0.0001) {
        heading = DNField<float>(this, 0x34);
    }
    float angle = std::atan2(heading, x);
    DNField<float>(this, 0x40) = angle;
    if (angle < 0.0f) {
        DNField<float>(this, 0x40) = angle + 6.2831855f;
    }
    float weather = 0.0f;
    if (alive) {
        weather = DNField<float>(this, 0x54) * 4.0f;
        if (1.0f < weather) {
            weather = 1.0f;
        }
    }
    DNField<float>(this, 0xe8) = weather;
    this->ComputeLights(alive, zoneLightsSingly);
    this->ComputeBodies();
    this->ComputeSunDirection(viewMatrix);
    if (this->m_1ec8 != 0) {
        std::memcpy(&DNField<float>(this, 0x1580), &DNField<float>(this, 0x1ecc), 12);
        DNField<uint32_t>(this, 0x158c) = DNField<uint32_t>(this, 0x1ee4);
        DNField<uint32_t>(this, 0x1590) = DNField<uint32_t>(this, 0x1ed8);
        DNField<uint32_t>(this, 0x1594) = DNField<uint32_t>(this, 0x1ee0);
        DNField<uint32_t>(this, 0x1598) = DNField<uint32_t>(this, 0x1edc);
        std::memmove(&DNField<uint8_t>(this, 0x15a4), &DNField<uint8_t>(this, 0x1580), 0x24);
    }
    if (this->m_light.Field<uint32_t>(0x15f0) != 0) {
        this->m_clouds.Generate(this);
        SkyModelAlpha(this);
    }
    this->BuildFogs();
    if (colorGrading) {
        this->ApplyColorGrading(env, true, false);
    }
    this->m_1602 = 0;
}
