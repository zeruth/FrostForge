#include "daynight/DayNight.hpp"

#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "ui/FrameTime.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {

constexpr float EPSILON = 2.3841858e-07f;   // DAT_14261a954
constexpr float ZONE_MARGIN = 50.0f;        // DAT_14262f26c
constexpr uint64_t MAX_SELECTED = 5;

// A ZoneLightPoint as the zone outline reads it: the position, then its order (field 1) at +8.
struct ZONELIGHTPOINT {
    float m_position[2];            // 0x00
};

// The light state's two embedded vectors of selections (+0x730 this frame's, +0x8b0 all).
CZONELIGHTVECTOR& Selected(DNScene* scene) {
    return *reinterpret_cast<CZONELIGHTVECTOR*>(scene->m_light.m_data + 0x730);
}

CZONELIGHTVECTOR& SelectedAll(DNScene* scene) {
    return *reinterpret_cast<CZONELIGHTVECTOR*>(scene->m_light.m_data + 0x8b0);
}

// The squared distance from a point to the segment a-b, in the plane (FUN_14134c7a0).
// ref: FUN_14134c7a0
float SegmentDistanceSquared(const float* a, const float* b, const float* point) {
    float ay = a[1];
    float px = point[0];
    float py = point[1];
    float dy = py - ay;
    float ax = a[0];
    float dx = px - ax;
    float ex = b[0] - ax;
    float ey = b[1] - ay;
    float length = ey * ey + ex * ex;
    if (EPSILON <= std::fabs(length)) {
        float t = (ey * dy + ex * dx) / length;
        if (0.0f <= t) {
            float cx;
            float cy;
            if (t <= 1.0f) {
                cy = t * ey + ay;
                cx = t * ex + ax;
            } else {
                cx = b[0];
                cy = b[1];
            }
            return (py - cy) * (py - cy) + (px - cx) * (px - cx);
        }
        return dy * dy + dx * dx;
    }
    return (ay - py) * (ay - py) + (ax - px) * (ax - px);
}

bool Ended(const DB2PARENTITER& it, const DB2PARENTITER& end) {
    return it.m_entry == end.m_entry && it.m_overflow == end.m_overflow && it.m_parent == end.m_parent
        && it.m_source == end.m_source;
}

// The points of zone `id` (the ZoneLightPoint table's parent lookup), begin and end.
WowDB2ParentLookup* ZonePoints(int32_t id, DB2PARENTITER* it, DB2PARENTITER* end) {
    WowDB2ParentLookup* points = g_zoneLightPointDB.m_parentLookup;
    if (points == nullptr) {
        *it = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
        *end = *it;
        return nullptr;
    }
    points->Begin(it, id);
    points->End(end);
    return points;
}

// How many points zone `id` has (FUN_14057b590 on ZoneLightPoint).
// ref: FUN_14057b590
int32_t ZonePointCount(int32_t id) {
    DB2PARENTITER it;
    DB2PARENTITER end;
    WowDB2ParentLookup* points = ZonePoints(id, &it, &end);
    if (points == nullptr) {
        return 0;
    }
    int32_t count = 0;
    while (!Ended(it, end)) {
        count++;
        points->Next(&it);
    }
    return count;
}

// The zone's point of order `order`, or null: the first point not before it in the zone's
// ordered points (FUN_1405ce600's binary search over the range).
// ref: FUN_1405ce600
const ZONELIGHTPOINT* ZonePointByOrder(int32_t id, uint32_t order) {
    DB2PARENTITER it;
    DB2PARENTITER end;
    WowDB2ParentLookup* points = ZonePoints(id, &it, &end);
    if (points == nullptr) {
        return nullptr;
    }
    while (!Ended(it, end)) {
        auto point = static_cast<const uint8_t*>(points->Record(&it));
        uint8_t pointOrder = DB2FieldU8(point + 8, 1, 8, &g_zoneLightPointDB);
        if (pointOrder >= order) {
            return pointOrder == order ? reinterpret_cast<const ZONELIGHTPOINT*>(point) : nullptr;
        }
        points->Next(&it);
    }
    return nullptr;
}

// How far `position` is from the zone's outline and from its height band, and whether it is
// inside each (FUN_142462640).
// ref: FUN_142462640
void ZoneLightDistance(const CZONELIGHT* zone, const float* position, bool* inside, bool* insideHeight,
    float* distance, float* heightDistance) {
    int32_t count = ZonePointCount(zone->m_id);
    const ZONELIGHTPOINT* previous = ZonePointByOrder(zone->m_id, static_cast<uint32_t>(count));
    float best = FLT_MAX;
    *inside = false;
    if (previous != nullptr) {
        DB2PARENTITER it;
        DB2PARENTITER end;
        WowDB2ParentLookup* points = ZonePoints(zone->m_id, &it, &end);
        while (!Ended(it, end)) {
            auto point = static_cast<const ZONELIGHTPOINT*>(points->Record(&it));
            float planar[2] = {position[0], position[1]};
            float d = SegmentDistanceSquared(point->m_position, previous->m_position, planar);
            if (best <= d) {
                d = best;
            }
            best = d;
            float y = position[1];
            float py = point->m_position[1];
            float qy = previous->m_position[1];
            bool crosses = y <= py ? qy < y : y <= qy;
            if (crosses) {
                float x = ((y - py) / (qy - py)) * (previous->m_position[0] - point->m_position[0])
                    + point->m_position[0];
                if (x < position[0]) {
                    *inside = !*inside;
                }
            }
            points->Next(&it);
            previous = point;
        }
    }
    *distance = std::sqrt(best);
    float above = zone->m_max[2] - position[2];
    float below = position[2] - zone->m_min[2];
    *insideHeight = !(above <= 0.0f || below <= 0.0f);
    below = std::fabs(below);
    above = std::fabs(above);
    if (above <= below) {
        below = above;
    }
    *heightDistance = below;
}

}  // namespace

// ref: FUN_1424628c0
void DNScene::UpdateZoneLights(float now) {
    if (this->m_1603 != 0) {
        this->m_1603 = 0;
        this->m_zoneLights.erase(0, this->m_zoneLights.size());
        WowDB2Lookup* zones = g_zoneLightDB.m_lookup;
        if (g_zoneLightDB.m_liveCount != 0 && zones != nullptr) {
            uint32_t mapId = this->m_light.Field<uint32_t>(0);
            DB2LOOKUPITER it;
            WowDB2LookupBegin(zones, &it);
            while (it.m_index != -2) {
                auto row = static_cast<const uint8_t*>(it.m_record);
                if (DB2FieldU16(row + 4, 1, 4, &g_zoneLightDB) == mapId) {
                    CZONELIGHT zone = {};
                    zone.m_min[0] = FLT_MAX;
                    zone.m_min[1] = FLT_MAX;
                    zone.m_min[2] = FLT_MAX;
                    zone.m_max[0] = -FLT_MAX;
                    zone.m_max[1] = -FLT_MAX;
                    zone.m_max[2] = -FLT_MAX;
                    zone.m_fadeFrom = 1.0f;
                    zone.m_fadeTo = 1.0f;
                    zone.m_fade = 1.0f;
                    zone.m_fadeDuration = 0.0f;
                    zone.m_fadeStart = 0.0f;
                    zone.m_id = static_cast<int32_t>(zones->Id(&it));
                    zone.m_lightId = DB2FieldU16(row + 4, 2, 4, &g_zoneLightDB);
                    DB2PARENTITER point;
                    DB2PARENTITER end;
                    WowDB2ParentLookup* points = ZonePoints(static_cast<int32_t>(zones->Id(&it)), &point, &end);
                    while (!Ended(point, end)) {
                        auto position = static_cast<const float*>(points->Record(&point));
                        zone.m_max[0] = zone.m_max[0] <= position[0] ? position[0] : zone.m_max[0];
                        zone.m_max[1] = zone.m_max[1] <= position[1] ? position[1] : zone.m_max[1];
                        zone.m_min[0] = position[0] <= zone.m_min[0] ? position[0] : zone.m_min[0];
                        zone.m_min[1] = zone.m_min[1] <= position[1] ? zone.m_min[1] : position[1];
                        points->Next(&point);
                    }
                    zone.m_max[2] = DB2FieldF32(row + 4, 5, 4, &g_zoneLightDB);
                    zone.m_min[2] = DB2FieldF32(row + 4, 4, 4, &g_zoneLightDB);
                    this->m_zoneLights.push_back(zone);
                }
                zones->Next(&it);
            }
        }
    }
    for (CZONELIGHT& zone : this->m_zoneLights) {
        float target = zone.m_fadeTo;
        if (EPSILON <= std::fabs(zone.m_fade - target)) {
            float duration = zone.m_fadeDuration;
            if (EPSILON <= std::fabs(duration)) {
                float t = now - zone.m_fadeStart;
                if (0.0f <= t) {
                    if (duration < t) {
                        t = duration;
                    }
                } else {
                    t = 0.0f;
                }
                zone.m_fade = (t / duration) * (target - zone.m_fadeFrom) + zone.m_fadeFrom;
            } else {
                zone.m_fade = target;
            }
        }
    }
}

// ref: FUN_142454cf0
void DNScene::AddZoneLight(const void* light, float distance, const CZONELIGHT* zone) {
    CZONELIGHTVECTOR& selected = Selected(this);
    if (selected.size() < MAX_SELECTED) {
        selected.push_back(*zone);
        selected.back().m_light = light;
        selected.back().m_distance = distance;
        SelectedAll(this).push_back(selected.back());
    }
}

// ref: FUN_1424623b0
void DNScene::SelectZoneLights(const float* position) {
    Selected(this).m_size = 0;
    SelectedAll(this).m_size = 0;
    for (const CZONELIGHT& zone : this->m_zoneLights) {
        bool flat = zone.m_min[2] == 0.0f && zone.m_max[2] == 0.0f;
        bool aboveMin = (zone.m_min[0] - ZONE_MARGIN <= position[0] && zone.m_min[1] - ZONE_MARGIN <= position[1]
                            && zone.m_min[2] - ZONE_MARGIN <= position[2])
            || flat;
        bool belowMax = (position[0] <= zone.m_max[0] + ZONE_MARGIN && position[1] <= zone.m_max[1] + ZONE_MARGIN
                            && position[2] <= zone.m_max[2] + ZONE_MARGIN)
            || flat;
        if (!aboveMin || !belowMax || std::fabs(zone.m_fade) < EPSILON) {
            continue;
        }
        float distance = FLT_MAX;
        float heightDistance = FLT_MAX;
        bool inside = false;
        bool insideHeight = false;
        ZoneLightDistance(&zone, position, &inside, &insideHeight, &distance, &heightDistance);
        float planar = inside ? -distance : distance;
        float height = insideHeight ? -heightDistance : heightDistance;
        if (planar - ZONE_MARGIN < 0.0f && height - ZONE_MARGIN < 0.0f) {
            bool local = false;
            const void* light = g_lightDB.GetRecord(zone.m_lightId, false, &local);
            if (light == nullptr) {
                return;
            }
            float a = std::fabs(height - ZONE_MARGIN);
            float b = std::fabs(planar - ZONE_MARGIN);
            if (b <= a) {
                a = b;
            }
            this->AddZoneLight(light, a, &zone);
        }
    }
    CZONELIGHTVECTOR& selected = Selected(this);
    std::sort(selected.begin(), selected.end(), [](const CZONELIGHT& a, const CZONELIGHT& b) {
        return a.m_14 != b.m_14 ? a.m_14 < b.m_14 : a.m_id < b.m_id;
    });
}

// ref: FUN_14245e3f0
void DNScene::Update(const float* position) {
    float now = FrameTime_GetTimeSeconds();
    this->UpdateZoneLights(now);
    // FUN_142454ea0: this frame's selection starts empty.
    this->m_light.Field<uint64_t>(0x738) = 0;
    this->SelectZoneLights(position);
}
