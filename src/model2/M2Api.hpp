#ifndef MODEL2_M2_API_HPP
#define MODEL2_M2_API_HPP

// The Model2 and WorldClient2 entry points the UI's model frames call: a world scene of its
// own per frame (WorldScene.cpp, 0x1a10 bytes) holding a CM2Scene, the models it creates, and
// the cameras the models carry. Each function is the reference function its tag names; those
// whose subsystem (the world scene, CM2Model, CM2Camera) is not ported yet are marked so in
// M2Api.cpp, and until the scene is ported no frame gets a model to call the rest with.

#include <cstdint>

class CWorldScene;
class CM2Model;
class CM2Camera;

// ---- the world scene ------------------------------------------------------------------------

// FUN_14134dd50: a scene of `type` (the model frames make type 5, with `light` set).
CWorldScene* WorldSceneCreate(uint32_t flags, int32_t type, bool light);
// FUN_1413c5bb0: drops a reference; the last frees the scene. The remaining count.
int32_t WorldSceneRelease(CWorldScene* scene);
// FUN_14134e8a0: the scene's render parameters (+0x68); their first word is a flag set.
uint32_t* WorldSceneGetParams(CWorldScene* scene);
// FUN_1413571e0
void WorldSceneParamsSetFlags(uint32_t* params, uint32_t mask, bool set);
// FUN_1413c3460 / FUN_1413c3450: a model of the scene's CM2Scene, with a reference for the
// caller, or null.
CM2Model* WorldSceneCreateModelByName(CWorldScene* scene, const char* name, uint32_t flags, int32_t a4, int32_t a5);
CM2Model* WorldSceneCreateModelById(CWorldScene* scene, int32_t fileDataId, uint32_t flags, int32_t a4, int32_t a5);
// FUN_1413574e0: advances the scene's models by `elapsed` seconds.
void WorldSceneAnimate(CWorldScene* scene, uint32_t* params, float elapsed, int32_t a4, int32_t a5, uint32_t flags,
    int32_t a7);
// FUN_1413573b0
void WorldSceneUpdate(CWorldScene* scene, uint32_t* params, int32_t a3, int32_t a4);
// FUN_1424d1a00: the view a camera gives (0x64 bytes at `out`).
void* WorldSceneViewFromCamera(void* out, CM2Camera* camera);
// FUN_1424d1370: draws the scene into a frame's rectangle.
void WorldSceneRenderFrame(CWorldScene* scene, uint32_t* params, void* view, const void* frame, void* target,
    float elapsed, uint32_t flags);

// ---- CM2Model ------------------------------------------------------------------------------

void M2ModelAddRef(CM2Model* model);                    // FUN_1411e6070
uint32_t M2ModelRelease(CM2Model* model);               // FUN_1411e7880
bool M2ModelIsLoaded(CM2Model* model);                  // FUN_141356340
bool M2ModelHasExternalTransform(CM2Model* model);      // FUN_1413563f0
bool M2ModelIsReady(CM2Model* model);                   // FUN_1411e74e0
void M2ModelSetAlpha(CM2Model* model, float alpha);     // FUN_1413566c0
float M2ModelGetAlpha(CM2Model* model);                 // +0x328
int32_t M2ModelGetFileDataId(CM2Model* model);          // its slot 0x88
const float* M2ModelGetWorldScale(CM2Model* model);     // FUN_141356140
void M2ModelSub1411E7970(CM2Model* model, bool a2);     // FUN_1413566f0
void M2ModelSub141356E60(CM2Model* model);              // FUN_141356e60: its slot 0x100
void M2ModelSetSceneFlagDAE8(CM2Model* model, bool a2); // FUN_141356e70
bool M2ModelIsAnimationPaused(CM2Model* model);         // FUN_141355d70 (+0x3a8 bit 17)
bool M2ModelHasPauseRequest(CM2Model* model);           // FUN_141355fc0 (+0x3b4)
void M2ModelSetPausedA(CM2Model* model, bool paused);   // FUN_141356d00
void M2ModelSetPausedB(CM2Model* model, bool paused);   // FUN_141356ae0
void M2ModelSetParticlesA(CM2Model* model, bool on);    // FUN_141356920
void M2ModelSetParticlesB(CM2Model* model, bool on);    // FUN_141356dd0
// FUN_141355d90: the model's bounds as min, max (its slot 0x138).
float* M2ModelGetBounds(float* out, CM2Model* model);
// FUN_141356000: the centre of the model's bounds for sequence `sequence`.
float* M2ModelGetCenter(float* out, CM2Model* model, int32_t sequence, int32_t a4);
uint32_t M2ModelGetCameraCount(CM2Model* model);        // FUN_141355ef0
CM2Camera* M2ModelGetCameraByIndex(CM2Model* model, uint32_t index);  // FUN_1411baf60
CM2Camera* M2ModelGetCameraById(CM2Model* model, uint32_t id);        // FUN_1411baff0
bool M2ModelHasAttachments(CM2Model* model);            // FUN_1411be870
int32_t M2ModelGetEffectType(CM2Model* model);          // FUN_1411be3a0
float M2ModelGetEffectStrength(CM2Model* model);        // FUN_141355e60
void M2ModelClearEffect(CM2Model* model);               // FUN_1411b6240
void M2ModelSetEffect(CM2Model* model, int32_t type);   // FUN_1411cb800
void M2ModelSetEffectStrength(CM2Model* model, float strength);       // FUN_1411cbd00
void M2ModelInvalidateBones(CM2Model* model);           // FUN_1411b7870
void M2ModelSetWorldTransform(CM2Model* model, const float* matrix);  // FUN_1411e7fa0
void M2ModelReplaceTexture(CM2Model* model, int32_t slot, void* texture);  // FUN_1411c8e30
// FUN_1413567b0 (FUN_1411ca300): plays a sequence.
void M2ModelSetSequence(CM2Model* model, int32_t a2, uint32_t sequence, int32_t a4, int32_t timeOffset, float speed,
    int32_t a7, int32_t a8, int32_t a9, int32_t a10, int32_t a11, float a12);
// The model's notifications: FUN_1411cca30 (+0x420), FUN_1411cc990, FUN_141356c00 (+0x318),
// FUN_1411e7c70 (+0x388, the load callback).
typedef void (*M2MODELCALLBACK)(void* model, void* param);
void M2ModelSetCallback420(CM2Model* model, void* callback, void* const* closure);
void M2ModelSetCallbackCC990(CM2Model* model, void* callback, void* const* closure);
void M2ModelSetCallback318(CM2Model* model, void* callback, void* param);
void M2ModelSetLoadCallback(CM2Model* model, void (*callback)(CM2Model* model, void* param), void* param);
// FUN_1411caf50: the model's animation events (`event` a four-character code such as '$DSO').
void M2ModelSetEventCallback(CM2Model* model, void (*callback)(void* param, uint32_t event, void* data), void* const* closure);

// ---- CM2Camera -----------------------------------------------------------------------------

// Vector 7 is the position, 8 the target; float 3 the field of view, 5 the roll.
void M2CameraGetVector(CM2Camera* camera, int32_t which, float* out);   // FUN_141295d20
float M2CameraGetFloat(CM2Camera* camera, int32_t which);               // FUN_141295da0
void M2CameraSetVector(CM2Camera* camera, int32_t which, const float* value);  // FUN_141296020
void M2CameraSetFloat(CM2Camera* camera, int32_t which, float value);   // FUN_141296160
void M2CameraAddRef(CM2Camera* camera);                                 // FUN_140317cb0 on it
void M2CameraRelease(CM2Camera** camera);                               // FUN_141295ff0
CM2Camera* M2CameraCreateDefault();                                     // FUN_141295830
CM2Camera* M2CameraClone(CM2Camera* camera);                            // FUN_141295980

#endif
