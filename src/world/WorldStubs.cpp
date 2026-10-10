#include "world/WorldStubs.hpp"

#include "storm/list/TSList.hpp"
#include "util/Unimplemented.hpp"
#include "world/WorldQueries.hpp"

// ---- the type 6 scene's objects ------------------------------------------------------------

// ref: FUN_1412a5ce0
void* WorldScene1950Create(void* memory) {
    FROST_UNIMPLEMENTED();
    return memory;
}

// ref: FUN_1412a9ff0
void WorldScene1950Release() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412a5dc0
void WorldScene1950Destroy(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141439100
void* WorldScene6d8Create(void* memory) {
    FROST_UNIMPLEMENTED();
    return memory;
}

// ref: FUN_141439440
void WorldScene6d8Destroy(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141439ee0
void WorldScene6d8Unload(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14143a9e0
void* WorldScene6e0Create(void* memory) {
    FROST_UNIMPLEMENTED();
    return memory;
}

// ref: FUN_14143ab50
void WorldScene6e0Destroy(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14143adf0
void WorldScene6e0Unload(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ---- the sky scene -------------------------------------------------------------------------

// ref: FUN_14143bcc0
void SkySceneDestroy(void* sky) {
    (void)sky;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14143cad0
void SkySceneUnload(void* sky) {
    (void)sky;
    FROST_UNIMPLEMENTED();
}

// ---- the scene's 0xfa50 object -------------------------------------------------------------

// ref: FUN_141423ae0
void* WorldScene030Create(void* memory, float a2, float a3, uint32_t a4) {
    (void)a2;
    (void)a3;
    (void)a4;
    FROST_UNIMPLEMENTED();
    return memory;
}

// ref: FUN_141423c00
void WorldScene030Destroy(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ---- MapWeather ----------------------------------------------------------------------------

// ref: FUN_14138a820
void* MapWeatherCreate(void* memory, CWorldScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();
    return memory;
}

// ref: FUN_14138b150
void MapWeatherDestroy(void* weather) {
    (void)weather;
    FROST_UNIMPLEMENTED();
}

// ---- fadeouts and capture ------------------------------------------------------------------

void WorldSceneFreeObjectFadeouts() {
    FROST_UNIMPLEMENTED();
}

void WorldSceneFreeFadeouts() {
    FROST_UNIMPLEMENTED();
}

void WorldSceneDeleteA80(void* object) {
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ---- map queries ---------------------------------------------------------------------------

// ref: FUN_1413bc550
bool WorldMapIntersectSegment(CWorldMap* map, WORLDSEGMENT* segment, WORLDSEGMENTHIT* hit) {
    (void)map;
    (void)segment;
    (void)hit;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1413bbf60
void WorldMapIntersectSegmentMulti() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413b5b40
bool WorldMapGroundHeight(CWorldMap* map, const float* position, float* height) {
    (void)map;
    (void)position;
    (void)height;
    FROST_UNIMPLEMENTED();
    return false;
}

// ---- wind ----------------------------------------------------------------------------------

namespace {

// DAT_142d00f60: the map's wind volumes (WoW/Source/WorldClient2/MapWind.cpp links them).
TSList<MAPWIND, TSGetLink<MAPWIND>, 8> s_mapWinds;

}  // namespace

MAPWIND* MapWindFirst() {
    return s_mapWinds.Head();
}

MAPWIND* MapWindNext(MAPWIND* wind) {
    return s_mapWinds.Next(wind);
}

// ---- the view's lists ----------------------------------------------------------------------

// ref: FUN_1413a2970
void* WorldProjectedDecalCreate(void* query) {
    (void)query;
    FROST_UNIMPLEMENTED();
    return nullptr;
}

// ref: FUN_141364cc0
void WorldViewListAdd(WORLDVIEWLIST* list, void* node) {
    (void)list;
    (void)node;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413e0270
void WorldViewListRelease(void* node) {
    (void)node;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413a7a20
void WorldViewReflectionDestroy(void* reflection) {
    (void)reflection;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1413a1e20
void WorldViewDrawProjected(CWorldView* view, GxRenderContext* context, ShaderEffectContext* shader,
    M2Element* element, uint8_t* box, uint32_t* color, uint8_t* object) {
    (void)view;
    (void)context;
    (void)shader;
    (void)element;
    (void)box;
    (void)color;
    (void)object;
    FROST_UNIMPLEMENTED();
}
