#include "model2/M2Api.hpp"

#include "util/Unimplemented.hpp"

// The CM2Model calls the model frames make through the world scene's models. The world scene
// entry points live in world/WorldFrame.cpp.

// ---- CM2Model ------------------------------------------------------------------------------

#define M2_MODEL_UNPORTED() FROST_UNIMPLEMENTED()  // CM2Model (Engine/Source/Model2/M2Model.cpp)

// ref: FUN_1411e6070
void M2ModelAddRef(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411e7880
uint32_t M2ModelRelease(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return 0;
}

// ref: FUN_141356340
bool M2ModelIsLoaded(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return false;
}

// ref: FUN_1413563f0
bool M2ModelHasExternalTransform(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return false;
}

// ref: FUN_1411e74e0
bool M2ModelIsReady(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return false;
}

// ref: FUN_1413566c0
void M2ModelSetAlpha(CM2Model* model, float alpha) {
    (void)model;
    (void)alpha;
    M2_MODEL_UNPORTED();
}

float M2ModelGetAlpha(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return 1.0f;
}

int32_t M2ModelGetFileDataId(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return 0;
}

// ref: FUN_141356140
const float* M2ModelGetWorldScale(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    static const float s_one = 1.0f;
    return &s_one;
}

// ref: FUN_1413566f0
void M2ModelSub1411E7970(CM2Model* model, bool a2) {
    (void)model;
    (void)a2;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141356e60
void M2ModelSub141356E60(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141356e70
void M2ModelSetSceneFlagDAE8(CM2Model* model, bool a2) {
    (void)model;
    (void)a2;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141355d70
bool M2ModelIsAnimationPaused(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return false;
}

// ref: FUN_141355fc0
bool M2ModelHasPauseRequest(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return false;
}

// ref: FUN_141356d00
void M2ModelSetPausedA(CM2Model* model, bool paused) {
    (void)model;
    (void)paused;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141356ae0
void M2ModelSetPausedB(CM2Model* model, bool paused) {
    (void)model;
    (void)paused;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141356920
void M2ModelSetParticlesA(CM2Model* model, bool on) {
    (void)model;
    (void)on;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141356dd0
void M2ModelSetParticlesB(CM2Model* model, bool on) {
    (void)model;
    (void)on;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141355d90
float* M2ModelGetBounds(float* out, CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    for (int32_t i = 0; i < 6; i++) {
        out[i] = 0.0f;
    }
    return out;
}

// ref: FUN_141356000
float* M2ModelGetCenter(float* out, CM2Model* model, int32_t sequence, int32_t a4) {
    (void)model;
    (void)sequence;
    (void)a4;
    M2_MODEL_UNPORTED();
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    return out;
}

// ref: FUN_141355ef0
uint32_t M2ModelGetCameraCount(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return 0;
}

// ref: FUN_1411baf60
CM2Camera* M2ModelGetCameraByIndex(CM2Model* model, uint32_t index) {
    (void)model;
    (void)index;
    M2_MODEL_UNPORTED();
    return nullptr;
}

// ref: FUN_1411baff0
CM2Camera* M2ModelGetCameraById(CM2Model* model, uint32_t id) {
    (void)model;
    (void)id;
    M2_MODEL_UNPORTED();
    return nullptr;
}

// ref: FUN_1411be870
bool M2ModelHasAttachments(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return false;
}

// ref: FUN_1411be3a0
int32_t M2ModelGetEffectType(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return 0;
}

// ref: FUN_141355e60
float M2ModelGetEffectStrength(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
    return 0.0f;
}

// ref: FUN_1411b6240
void M2ModelClearEffect(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411cb800
void M2ModelSetEffect(CM2Model* model, int32_t type) {
    (void)model;
    (void)type;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411cbd00
void M2ModelSetEffectStrength(CM2Model* model, float strength) {
    (void)model;
    (void)strength;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411b7870
void M2ModelInvalidateBones(CM2Model* model) {
    (void)model;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411e7fa0
void M2ModelSetWorldTransform(CM2Model* model, const float* matrix) {
    (void)model;
    (void)matrix;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411c8e30
void M2ModelReplaceTexture(CM2Model* model, int32_t slot, void* texture) {
    (void)model;
    (void)slot;
    (void)texture;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1413567b0
void M2ModelSetSequence(CM2Model* model, int32_t a2, uint32_t sequence, int32_t a4, int32_t timeOffset, float speed,
    int32_t a7, int32_t a8, int32_t a9, int32_t a10, int32_t a11, float a12) {
    (void)model;
    (void)a2;
    (void)sequence;
    (void)a4;
    (void)timeOffset;
    (void)speed;
    (void)a7;
    (void)a8;
    (void)a9;
    (void)a10;
    (void)a11;
    (void)a12;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411cca30
void M2ModelSetCallback420(CM2Model* model, void* callback, void* const* closure) {
    (void)model;
    (void)callback;
    (void)closure;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411cc990
void M2ModelSetCallbackCC990(CM2Model* model, void* callback, void* const* closure) {
    (void)model;
    (void)callback;
    (void)closure;
    M2_MODEL_UNPORTED();
}

// ref: FUN_141356c00
void M2ModelSetCallback318(CM2Model* model, void* callback, void* param) {
    (void)model;
    (void)callback;
    (void)param;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411e7c70
void M2ModelSetLoadCallback(CM2Model* model, void (*callback)(CM2Model* model, void* param), void* param) {
    (void)model;
    (void)callback;
    (void)param;
    M2_MODEL_UNPORTED();
}

// ref: FUN_1411caf50
void M2ModelSetEventCallback(CM2Model* model, void (*callback)(void* param, uint32_t event, void* data), void* const* closure) {
    (void)model;
    (void)callback;
    (void)closure;
    M2_MODEL_UNPORTED();
}

