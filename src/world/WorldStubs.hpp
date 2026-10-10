#ifndef WORLD_WORLD_STUBS_HPP
#define WORLD_WORLD_STUBS_HPP

// World-side entry points the scene reaches that are not ported yet. Each is the reference
// function its tag names, with a body that reports itself (WorldStubs.cpp); a port moves it to
// its own file. Grouped by the object they serve.

#include <cstdint>

class CWorldScene;
class CWorldView;

// The type 6 scene's own objects (the world's, never a model frame's).
void* WorldScene1950Create(void* memory);       // FUN_1412a5ce0 (0x68)
void WorldScene1950Release();                   // FUN_1412a9ff0
void WorldScene1950Destroy(void* object);       // FUN_1412a5dc0
void* WorldScene6d8Create(void* memory);        // FUN_141439100 (0x7878)
void WorldScene6d8Destroy(void* object);        // FUN_141439440
void WorldScene6d8Unload(void* object);         // FUN_141439ee0
void* WorldScene6e0Create(void* memory);        // FUN_14143a9e0 (0x38)
void WorldScene6e0Destroy(void* object);        // FUN_14143ab50
void WorldScene6e0Unload(void* object);         // FUN_14143adf0

// The sky scene at the scene's +0x40 (0x20 bytes; CSkySceneModel nodes).
void SkySceneDestroy(void* sky);                // FUN_14143bcc0
void SkySceneUnload(void* sky);                 // FUN_14143cad0

// The scene's 0xfa50 object (made for a map).
void* WorldScene030Create(void* memory, float a2, float a3, uint32_t a4);  // FUN_141423ae0
void WorldScene030Destroy(void* object);        // FUN_141423c00

// Engine/Source/DayNight (DayNight.cpp): the handle at the scene's +0x28.
void* DayNightCreate(CWorldScene* scene, CWorldView* view);     // FUN_142442100
void DayNightDestroy(void* dayNight);           // FUN_142442370
void DayNightSetMap(void* dayNight, int32_t mapId);             // FUN_142442fe0
void DayNightSetArea(void* dayNight, int32_t lightId, int32_t index);  // FUN_142443100
void DayNightSetView(void* dayNight, CWorldView* view, const float* position, const float* clip,
    const float* viewMatrix);                   // FUN_142443200

// WoW/Source/WorldClient2/MapWeather.cpp (0x1378 bytes) at the scene's +0x38.
void* MapWeatherCreate(void* memory, CWorldScene* scene);       // FUN_14138a820
void MapWeatherDestroy(void* weather);          // FUN_14138b150

// The object fadeouts (TSList<SObjectFadeout>, DAT_142d011b8 / DAT_142d011e0).
void WorldSceneFreeObjectFadeouts();            // the loop in FUN_1413c3a40
void WorldSceneFreeFadeouts();                  // the loop in FUN_1413c24e0

// The object at the scene's +0xa80, deleted through its vtable's +8.
void WorldSceneDeleteA80(void* object);

#endif
