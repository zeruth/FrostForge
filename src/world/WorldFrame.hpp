#ifndef WORLD_WORLD_FRAME_HPP
#define WORLD_WORLD_FRAME_HPP

// WoW/Source/WorldClient2/World.cpp's scene entry points (the ones the model frames and the
// client's startup call) and the world state the scene's frame reads (WorldFrame.cpp).

#include <cstdint>

class CM2Camera;
class CM2Model;
class CWorldScene;
class CWorldView;

// A scene of `type` (the model frames make type 5 with `light`, the client's startup the
// world's type 6 with flag 8), with a reference for the caller; with `light` it loads no map
// (map -1) so it has a sky and lights.
CWorldScene* WorldSceneCreate(uint32_t flags, int32_t type, bool light);
// Drops a reference (CWorldScene::Release); the remaining count.
int32_t WorldSceneRelease(CWorldScene* scene);
// The scene's own view, whose first word is the flag set the frames switch features in.
uint32_t* WorldSceneGetParams(CWorldScene* scene);
void WorldSceneParamsSetFlags(uint32_t* params, uint32_t mask, bool set);
// A model of the scene's CM2Scene, with a reference for the caller, or null.
CM2Model* WorldSceneCreateModelByName(CWorldScene* scene, const char* name, uint32_t flags, int32_t priority,
    int32_t lod);
CM2Model* WorldSceneCreateModelById(CWorldScene* scene, int32_t fileDataId, uint32_t flags, int32_t priority,
    int32_t lod);
// Advances the scene by `elapsed` seconds with its camera at `position` looking at `target`
// (the origin for null); `latch` latches the view for the draw.
void WorldSceneAnimate(CWorldScene* scene, uint32_t* params, float elapsed, const float* position,
    const float* target, uint32_t flags, bool latch);
// ref: FUN_141352ea0
void WorldSceneAnimateFrame(CWorldScene* scene, CWorldView* view, const float* position, const float* target,
    const int32_t* lod, float elapsed, uint32_t flags, bool latch);
// The scene's models placed for the view and animated (`skipDrawParams` holds back the
// draw parameters this once).
void WorldSceneUpdate(CWorldScene* scene, uint32_t* params, bool skipDrawParams, const float* position);
// The view a camera gives (0x50 bytes at `out`: its matrix, field of view, clips and aspect).
void* WorldSceneViewFromCamera(void* out, CM2Camera* camera);
// Draws the scene into a frame's rectangle (`view` from WorldSceneViewFromCamera, `frame` a
// MODELFRAMEVIEW, `target` a FRAMEBUFFER or null for the window's).
void WorldSceneRenderFrame(CWorldScene* scene, uint32_t* params, void* view, const void* frame, void* target,
    float elapsed, uint32_t flags);

// Whether a model file is ready in `scene` (in any scene for null); what is missing is asked
// for.
bool WorldSceneModelReady(CWorldScene* scene, const char* name, uint8_t priority);
// ref: FUN_1413cb320
bool WorldScenesModelReady(const char* name, uint8_t priority);

// The scene's screen rectangle (scene + 0xc8).
void WorldSceneSetRect(const float* rect, CWorldScene* scene);

// DAT_1430ab790: DayNight's zone lights and light run on a pool job.
extern uint8_t g_dayNightOnJob;

// ref: FUN_141354f80
void WorldReadGraphicsQuality();
// ref: FUN_14134e7e0
uint32_t WorldGraphicsQuality();
// ref: FUN_14135cf20
uint32_t WorldFrameBudget();
// ref: FUN_14134e950
float* WorldPlayerPosition();
// ref: FUN_1413c0930
float WorldLodScaleMinimum();
// ref: FUN_1413c1020
void WorldSetLodScale(float scale);
// ref: FUN_140309ec0
bool RenderFeatureEnabled(int32_t feature);
// ref: FUN_142526ee0
uint8_t WorldWaterDetail();
// ref: FUN_142526ec0
uint8_t WorldReflectionMode();
// ref: FUN_14252ce30
void WorldWaitGlobalJobs();
// The environment the frame's shaders read.
// ref: FUN_141353210
void WorldSceneSetEnvironment(uint8_t (*environment)[0xe0], CWorldScene* scene, const float* position,
    const float* view, const float* targetScale, const float* direction, const float* color, float shadowFactor);

#endif
