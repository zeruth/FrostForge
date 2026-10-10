#ifndef WORLD_WORLD_SCENE_CALLBACKS_HPP
#define WORLD_WORLD_SCENE_CALLBACKS_HPP

// The functions a world scene hands its CM2Scene and the device and render settings: what the
// models ask of the world (projected decals, a trace to the ground, wind, the ground height,
// the physics bones' collisions) and what the scene does when the device or the render size
// changes.

#include <cstdint>

class CWorldScene;
struct RENDERSETTINGS;

// A projected batch's decal, from the query the model builds ({scene, view, ...}).
// ref: FUN_141365a00
void* WorldSceneProjectModel(void* query);
// ref: FUN_141365a50
void WorldSceneProjectCheck(void* draw);
// The first thing under a model's bone (the query {.., scene, position, end}).
// ref: FUN_1413511d0
void WorldSceneTrace(void* query, float* hit, float* t);
// ref: FUN_14134f920
void WorldScenePhysics(void* states, void* param);
// The wind at a point, summed over the map's wind volumes (LAB_14134db40 jumps to it).
// ref: FUN_14139aff0
void WorldSceneSampleWind(const float* position, float* direction, float* speed, uint8_t gusts);
// The ground under a particle.
// ref: FUN_1413513b0
bool WorldSceneGroundHeight(const float* position, float* height, void* param);

// ref: FUN_1413c5040
void WorldSceneOnDeviceDestroyed(void* param);
// ref: FUN_1413c50e0
void WorldSceneOnRenderSettings(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param);

// The world's flag word (DAT_1430ab9d8 & DAT_1430ab9d4).
// ref: FUN_14134e3d0
uint32_t WorldGetFlags();

// The capture texture the render settings ask for: made at the render size over the capture
// divisor, and destroyed.
// ref: FUN_1412af5b0
void WorldCaptureCreate(void** tex, uint32_t width, uint32_t height);
// ref: FUN_1412af4e0
void WorldCaptureDestroy(void* tex);

#endif
