#include "model2/M2Scene.hpp"

#include <cmath>
#include <cstring>

#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Scene.cpp.

namespace {

// The array's tag (DAT_142da1b90).
const char* const s_drawParamsName = "AUBaseModelDrawParams";

// A growable array of draw parameters: data, count, capacity (top bit set while the data is
// not owned).
struct DRAWPARAMSARRAY {
    BaseModelDrawParams* m_data;
    uint64_t m_count;
    uint64_t m_capacity;
};

}  // namespace

// ref: FUN_1411ace60
BaseModelDrawParams* M2SceneAddDrawParams(void* array) {
    auto params = static_cast<DRAWPARAMSARRAY*>(array);
    uint64_t capacityField = params->m_capacity;
    uint64_t count = params->m_count;
    uint64_t capacity = capacityField & 0x7fffffffffffffffull;
    if (count == capacity && capacity < capacity + 1) {
        uint64_t grow = capacityField >> 1 & 0x3fffffffffffffffull;
        uint64_t step = 1;
        if (capacity + 1 <= capacity + grow) {
            step = grow;
        }
        uint64_t newCapacity = step + capacity;
        params->m_capacity = ((newCapacity ^ capacityField) & 0x8000000000000000ull) ^ newCapacity;
        auto data = static_cast<BaseModelDrawParams*>(SMemAlloc(newCapacity * sizeof(BaseModelDrawParams), 0xb, s_drawParamsName, 0, -6, 0));
        BaseModelDrawParams* old = params->m_data;
        count = params->m_count;
        if (count != 0) {
            std::memcpy(data, old, count * sizeof(BaseModelDrawParams));
            old = params->m_data;
            count = params->m_count;
        }
        if (static_cast<int64_t>(params->m_capacity) >= 0) {
            SMemFree(old, s_drawParamsName, -6, 0);
            count = params->m_count;
        }
        params->m_capacity &= 0x7fffffffffffffffull;
        params->m_data = data;
    }
    BaseModelDrawParams* entry = &params->m_data[count];
    std::memset(entry, 0, sizeof(BaseModelDrawParams));
    params->m_count = params->m_count + 1;
    return entry;
}

// A box (min, max) against six planes: 0 when it is wholly outside one of them, else 3.
// ref: FUN_141346830
int32_t M2FrustumTestBox(const float* planes, const float* box) {
    const float* corners[2] = {box + 3, box};
    for (int32_t i = 0; i < 6; i++) {
        const float* plane = planes + i * 4;
        float distance = corners[std::signbit(plane[0]) ? 1 : 0][0] * plane[0]
            + corners[std::signbit(plane[1]) ? 1 : 0][1] * plane[1]
            + corners[std::signbit(plane[2]) ? 1 : 0][2] * plane[2] + plane[3];
        if (distance < -0.019444413f) {  // DAT_142734910
            return 0;
        }
    }
    return 3;
}

// ref: FUN_1411df990
void M2SceneGatherLights(CM2Scene* scene, CM2ModelLighting* lighting) {
    (void)scene;
    (void)lighting;
    FROST_UNIMPLEMENTED();
}
