#ifndef WORLD_WORLD_QUERIES_HPP
#define WORLD_WORLD_QUERIES_HPP

// What the world answers about its geometry: segment intersections, the ground under a
// point, the wind volumes, and the projected decals and physics bones a view collects.

#include "base/RndSeed.hpp"
#include <cstdint>

class CWorldMap;
class CWorldView;
struct WORLDVIEWLIST;
class GxRenderContext;
class ShaderEffectContext;
struct M2Element;

// A segment to intersect (0x24): from, to, what to test against, and how many hits it took.
struct WORLDSEGMENT {
    float m_start[3];               // 0x00
    float m_end[3];                 // 0x0c
    uint64_t m_flags;               // 0x18 0x120 bones, 0x100111 the ground
    uint32_t m_hits;                // 0x20
};

// Where it stopped (0x20).
struct WORLDSEGMENTHIT {
    float m_point[3];               // 0x00
    float m_t;                      // 0x0c the fraction along the segment, 1 for no hit
    uint64_t m_10;                  // 0x10
    uint64_t m_18;                  // 0x18
};

// A wind source (0x88, WoW/Source/WorldClient2/MapWind.cpp), on one of two global lists by its
// link at +0. Each frame its direction and speed wander at random within their ranges.
struct MAPWIND {
    void* m_prevlink;               // 0x00
    MAPWIND* m_next;                // 0x08 tagged at the end of the list
    float m_baseDirection[3];       // 0x10 the direction it wanders about
    float m_baseSpeed;              // 0x1c
    float m_directionRange[3];      // 0x20 how far each component may stray from it
    float m_speedAbove;             // 0x2c how far the speed may rise above its base
    float m_speedBelow;             // 0x30 and fall below it
    float m_currentDirection[3];    // 0x34 normalized
    float m_speed;                  // 0x40
    float m_direction[3];           // 0x44 the current direction scaled by its speed
    float m_directionRate[3];       // 0x50 the most each component moves in a second
    float m_speedRate;              // 0x5c the most the speed moves in a second
    float m_lifetime;               // 0x60 seconds left, when flag 2 is set
    uint8_t m_flags;                // 0x64 2 expires, 4 a sphere, 0x20 gusts
    uint8_t m_pad65[3];
    float m_center[3];              // 0x68
    float m_radius;                 // 0x74
    uint32_t m_pad78;
    CRndSeed m_seed;                // 0x7c
    uint32_t m_pad84;
};
static_assert(sizeof(MAPWIND) == 0x88, "MAPWIND is 0x88 bytes");

// The first wind volume, and the next.
MAPWIND* MapWindFirst();
MAPWIND* MapWindNext(MAPWIND* wind);
// Once a frame, from the scene's animate: every source steps, the expired ones are deleted,
// and the global wind follows their sum.
// ref: FUN_14139b480
void MapWindUpdate();
// Once a frame: the WMO group BSP caches forget everything when the frame time went back.
// ref: FUN_14139abd0
void BspCacheUpdate();

// ref: FUN_1413bc550
bool WorldMapIntersectSegment(CWorldMap* map, WORLDSEGMENT* segment, WORLDSEGMENTHIT* hit);
// ref: FUN_1413bbf60
void WorldMapIntersectSegmentMulti();
// ref: FUN_1413b5b40
bool WorldMapGroundHeight(CWorldMap* map, const float* position, float* height);

// A projected decal for the view to draw, from a model's query (null when the list is held).
// ref: FUN_1413dd0a0
void* WorldViewAddProjected(CWorldView* view, void* query);
// ref: FUN_1413a1e20
void WorldViewDrawProjected(CWorldView* view, GxRenderContext* context, ShaderEffectContext* shader,
    M2Element* element, uint8_t* box, uint32_t* color, uint8_t* object);
// A projected decal (WoW/Source/WorldClient2, FUN_1413a2970) and the view list it joins.
// ref: FUN_1413a2970
void* WorldProjectedDecalCreate(void* query);
// ref: FUN_141364cc0
void WorldViewListAdd(WORLDVIEWLIST* list, void* node);
// ref: FUN_1413e0270
void WorldViewListRelease(void* node);
// The view's reflection state at +0x238 (two 0x13e0 renders and their jobs).
// ref: FUN_1413a7a20
void WorldViewReflectionDestroy(void* reflection);
// A physics bone's state for the view's culling.
// ref: FUN_1413da150
void WorldViewAddPhysics(CWorldView* view, uint8_t* state);

#endif
