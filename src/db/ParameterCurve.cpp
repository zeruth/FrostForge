// ParameterCurve (WoW/Source/DB/DBEngine/ParameterCurve_C.cpp).

#include "db/ParameterCurve.hpp"

#include <cmath>
#include <cstring>
#include <vector>

#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"

namespace {

// The curve table (DAT_1434913b0, 0x38): the points and one word per curve id.
struct CURVETABLE {
    std::vector<C2VECTORF> m_points;    // +0x08
    std::vector<uint32_t> m_curves;     // +0x20
};
CURVETABLE* s_table;                    // DAT_1434913b0
uint8_t s_dirty;                        // DAT_1434913c0
uint8_t s_building;                     // DAT_1434913c1
std::vector<void (*)()> s_rebuildHandlers;  // DAT_1434913f8

// A point as the build sorts it: the CurvePoint's order and position.
struct ORDEREDPOINT {
    uint64_t m_order;
    C2VECTORF m_position;
};

// LAB_14230b000: a Curve or CurvePoint record was patched.
void MarkDirty(uint32_t, uint32_t, uint32_t, void*, void*) {
    s_dirty = 1;
}

// Every curve id's points, in OrderIndex order.
// ref: FUN_14230ac70
std::vector<std::vector<ORDEREDPOINT>> GatherPoints() {
    std::vector<std::vector<ORDEREDPOINT>> curves(static_cast<size_t>(g_curveDB.m_maxId + 1));
    WowDB2Lookup* lookup = g_curvePointDB.m_lookup;
    if (lookup == nullptr) {
        return curves;
    }
    DB2LOOKUPITER it;
    WowDB2LookupBegin(lookup, &it);
    while (it.m_index != -2) {
        auto record = static_cast<const uint8_t*>(it.m_record);
        int32_t curveId = DB2FieldI32(record + 0x10, 3, 0x10, &g_curvePointDB);
        if (g_curveDB.HasRecord(curveId)) {
            std::vector<ORDEREDPOINT>& points = curves[static_cast<size_t>(curveId)];
            if (points.empty()) {
                points.reserve(8);
            }
            uint8_t order = DB2FieldU8(record + 0x10, 4, 0x10, &g_curvePointDB);
            // lower_bound on the order
            auto first = points.begin();
            auto last = points.end();
            while (first != last) {
                auto middle = first + (last - first) / 2;
                if (static_cast<int32_t>(middle->m_order) < static_cast<int32_t>(order)) {
                    first = middle + 1;
                } else {
                    last = middle;
                }
            }
            ORDEREDPOINT point;
            std::memcpy(&point.m_position, record, sizeof(C2VECTORF));
            point.m_order = order;
            points.insert(first, point);
        }
        lookup->Next(&it);
    }
    return curves;
}

// A constant curve.
// ref: FUN_14230d370
void EncodeConstant(uint32_t* curve, float value) {
    std::memcpy(curve, &value, sizeof(value));
    *curve |= 1;
}

// A curve over `count` points from `first`, by its Curve type.
// ref: FUN_14230d380
void Encode(uint32_t* curve, const C2VECTORF* points, int32_t type, uint64_t first, uint64_t count) {
    if (count < 2) {
        if (count != 0) {
            EncodeConstant(curve, points[first].y);
        }
        return;
    }
    int32_t interpolation = 0;
    *curve ^= ((static_cast<uint32_t>(first) << 4) ^ *curve) & 0xfffff0;
    reinterpret_cast<uint8_t*>(curve)[3] = static_cast<uint8_t>(count);
    if (type == 1) {
        interpolation = count < 4 ? 1 : 2;
    } else if (type == 2) {
        if (count == 2) {
            interpolation = 1;
        } else if (count == 3) {
            interpolation = 3;
        } else if (count == 4) {
            interpolation = 4;
        } else {
            interpolation = 5;
        }
    } else if (type == 3) {
        interpolation = 1;
    }
    *curve = (*curve & 0xfffffff0) | static_cast<uint32_t>(interpolation * 2);
}

// The Bezier curve of any order, by de Casteljau over the points' y.
// ref: FUN_14230d420
float EvaluateBezier(const C2VECTORF* points, uint64_t count, float t) {
    float span = points[count - 1].x - points[0].x;
    if (span <= 0.0f) {
        return points[count - 1].y;
    }
    float u = (t - points[0].x) / span;
    float values[24];
    for (uint64_t i = 0; i < count; i++) {
        values[i] = points[i].y;
    }
    float v = 1.0f - u;
    for (uint64_t level = 1; level < count; level++) {
        for (uint64_t i = 0; i + level < count; i++) {
            values[i] = u * values[i + 1] + v * values[i];
        }
    }
    return values[0];
}

// The index of the first point whose x is past t, from `first` (the reference unrolls by four).
uint64_t FindSegment(const C2VECTORF* points, uint64_t first, uint64_t end, float t) {
    uint64_t i = first;
    while (i < end && !(t < points[i].x)) {
        i++;
    }
    return i;
}

// ref: FUN_14230c910
float Evaluate(const uint32_t* curve, const C2VECTORF* points, float t) {
    uint32_t word = *curve;
    if ((word & 1) != 0) {
        uint32_t bits = word & 0xfffffffe;
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    uint64_t count = word >> 24;
    if (count < 2) {
        return 0.0f;
    }
    uint64_t first = (word >> 4) & 0xfffff;
    uint64_t end = first + count;
    const C2VECTORF* p = points;
    switch ((word >> 1) & 7) {
        case 0: {
            uint64_t i = FindSegment(p, first, end, t);
            if (i == first) {
                return p[i].y;
            }
            if (end <= i) {
                return p[end - 1].y;
            }
            float span = p[i].x - p[i - 1].x;
            if (0.0f < span) {
                return (t - p[i - 1].x) / span * (p[i].y - p[i - 1].y) + p[i - 1].y;
            }
            return p[i].y;
        }
        case 1: {
            uint64_t i = FindSegment(p, first, end, t);
            if (i == first) {
                return p[i].y;
            }
            if (end <= i) {
                return p[end - 1].y;
            }
            float span = p[i].x - p[i - 1].x;
            if (0.0f < span) {
                float c = std::cos((t - p[i - 1].x) / span * 3.14159265f);
                return (1.0f - c) * 0.5f * (p[i].y - p[i - 1].y) + p[i - 1].y;
            }
            return p[i].y;
        }
        case 2: {
            // Catmull-Rom over the inner points; the build duplicated the ends.
            uint64_t start = first + 1;
            if (start < end) {
                uint64_t i = FindSegment(p, start, end, t);
                if (start < i) {
                    if (end - 1 <= i) {
                        return p[end - 2].y;
                    }
                    float x0 = p[i - 1].x;
                    float span = p[i].x - x0;
                    float y1 = p[i - 1].y;
                    if (span <= 0.0f) {
                        return y1;
                    }
                    float y2 = p[i].y;
                    float y0 = p[i - 2].y;
                    float u = (t - x0) / span;
                    float a = y0 * 0.5f;
                    float d = p[i + 1].y * 0.5f;
                    return ((((y1 * 1.5f - a) - y2 * 1.5f) + d) * u + (((y0 - y1 * 2.5f) + y2 + y2) - d)) * u * u
                        + (y2 * 0.5f - a) * u + y1;
                }
            }
            return p[first + 1].y;
        }
        case 3: {
            float x0 = p[first].x;
            float value = p[first + 1].y;
            float span = p[first + 2].x - x0;
            if (0.0f < span) {
                float u = (t - x0) / span;
                float v = 1.0f - u;
                value = v * v * p[first].y + (v + v) * u * value + u * u * p[first + 2].y;
            }
            return value;
        }
        case 4: {
            float x0 = p[first].x;
            float value = p[first + 1].y;
            float span = p[first + 3].x - x0;
            if (0.0f < span) {
                float u = (t - x0) / span;
                float v = 1.0f - u;
                value = v * p[first + 2].y * u * u * 3.0f + u * u * u * p[first + 3].y + value * u * v * v * 3.0f
                    + v * v * v * p[first].y;
            }
            return value;
        }
        case 5:
            return EvaluateBezier(p + first, count, t);
        default:
            return 0.0f;
    }
}

// The rebuild listeners (FUN_14230d330).
void NotifyRebuilt() {
    for (void (*handler)() : s_rebuildHandlers) {
        handler();
    }
}

}

// ref: FUN_14230b080
void ParameterCurveBuild() {
    std::vector<std::vector<ORDEREDPOINT>> gathered = GatherPoints();
    if (s_table == nullptr) {
        s_table = new CURVETABLE();
    }
    s_table->m_points.clear();
    s_table->m_curves.assign(gathered.size(), 0);
    for (size_t id = 0; id < gathered.size(); id++) {
        const std::vector<ORDEREDPOINT>& points = gathered[id];
        uint64_t count = points.size();
        if (count == 0) {
            continue;
        }
        uint32_t* curve = &s_table->m_curves[id];
        if (count == 1) {
            EncodeConstant(curve, points[0].m_position.y);
            continue;
        }
        bool local;
        auto record = static_cast<const uint8_t*>(g_curveDB.GetRecord(static_cast<uint32_t>(id), false, &local));
        int32_t type = 0;
        uint64_t first = s_table->m_points.size();
        if (record != nullptr) {
            type = DB2FieldU8(record, 1, 0, &g_curveDB);
            if (type == 1) {
                s_table->m_points.push_back(points[0].m_position);
            }
        }
        for (const ORDEREDPOINT& point : points) {
            s_table->m_points.push_back(point.m_position);
        }
        if (type == 1) {
            s_table->m_points.push_back(points[count - 1].m_position);
            count += 2;
        }
        Encode(curve, s_table->m_points.data(), type, first, count);
    }
    s_table->m_points.shrink_to_fit();
    s_table->m_curves.shrink_to_fit();
}

// ref: FUN_14230b600
void ParameterCurveInitialize() {
    g_curveDB.AddPatchCallback(&MarkDirty, nullptr);
    g_curvePointDB.AddPatchCallback(&MarkDirty, nullptr);
    ParameterCurveBuild();
}

// ref: FUN_14230b040
void ParameterCurveUpdate() {
    if (s_dirty != 0) {
        s_building = 1;
        ParameterCurveBuild();
        NotifyRebuilt();
        s_dirty = 0;
        s_building = 0;
    }
}

// ref: FUN_14230ce30
float ParameterCurveEvaluate(uint32_t curveId, float t) {
    if (s_table == nullptr || s_table->m_curves.size() <= curveId) {
        return 0.0f;
    }
    return Evaluate(&s_table->m_curves[curveId], s_table->m_points.data(), t);
}

// ref: FUN_14230d1e0
uint32_t ParameterCurveGetPointCount(uint32_t curveId) {
    if (s_table == nullptr || s_table->m_curves.size() <= curveId) {
        return 0;
    }
    uint32_t word = s_table->m_curves[curveId];
    return (word & 1) == 0 ? word >> 24 : 0;
}

// ref: FUN_14230d140
bool ParameterCurveGetPoint(uint32_t curveId, uint32_t index, C2VECTORF* point) {
    if (s_table == nullptr || s_table->m_curves.size() <= curveId) {
        return false;
    }
    uint32_t word = s_table->m_curves[curveId];
    if (index < (word >> 24)) {
        *point = s_table->m_points[((word >> 4) & 0xfffff) + index];
        return true;
    }
    return false;
}

// ref: FUN_14230d240
bool ParameterCurveGetFirstPoint(uint32_t curveId, C2VECTORF* point) {
    if (s_table == nullptr || s_table->m_curves.size() <= curveId) {
        return false;
    }
    uint32_t word = s_table->m_curves[curveId];
    if ((word >> 24) != 0) {
        *point = s_table->m_points[(word >> 4) & 0xfffff];
        return true;
    }
    return false;
}

// ref: FUN_14230d070
bool ParameterCurveGetLastPoint(uint32_t curveId, C2VECTORF* point) {
    if (s_table == nullptr || s_table->m_curves.size() <= curveId) {
        return false;
    }
    uint32_t word = s_table->m_curves[curveId];
    uint32_t count = (word & 1) == 0 ? word >> 24 : 0;
    if ((word >> 24) <= count - 1) {
        return false;
    }
    *point = s_table->m_points[((word >> 4) & 0xfffff) + (count - 1)];
    return true;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1434913b0 = (void*)&s_table;
extern "C" void* const gen_p_1434913c0 = (void*)&s_dirty;
// ---- end of generated globals ----
