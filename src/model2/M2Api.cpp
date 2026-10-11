#include "model2/M2Api.hpp"

#include "model2/M2Data.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Shared.hpp"
#include "util/Unimplemented.hpp"

// The CM2Model calls the model frames make through the world scene's models. The world scene
// entry points live in world/WorldFrame.cpp.

// ---- CM2Model ------------------------------------------------------------------------------

void M2ModelAddRef(CM2Model* model) {
    model->AddRef();
}

// ref: FUN_142458520
uint32_t M2ModelRelease(CM2Model* model) {
    return model->Release(1);
}

// ref: FUN_141356340
bool M2ModelIsLoaded(CM2Model* model) {
    return (model->m_flags & CM2BaseModel::BASE_LOADED) != 0;
}

// ref: FUN_1413563f0
bool M2ModelHasExternalTransform(CM2Model* model) {
    return (model->m_flags >> 2 & 1) != 0;
}

bool M2ModelIsReady(CM2Model* model) {
    return model->IsReady();
}

// ref: FUN_1413566c0
void M2ModelSetAlpha(CM2Model* model, float alpha) {
    model->m_alpha = alpha;
}

float M2ModelGetAlpha(CM2Model* model) {
    return model->m_alpha;
}

int32_t M2ModelGetFileDataId(CM2Model* model) {
    return model->GetFileDataId();
}

// ref: FUN_141356140
const float* M2ModelGetLocalMatrix(CM2Model* model) {
    return reinterpret_cast<const float*>(&model->m_local);
}

// ref: FUN_1413566f0
void M2ModelSetBucketed(CM2Model* model, bool bucketed) {
    model->SetBucketed(bucketed ? 1 : 0);
}

// ref: FUN_141356e60
void M2ModelSetShown(CM2Model* model, bool shown) {
    model->SetShown(shown);
}

// ref: FUN_141356e70
void M2ModelSetSceneFlagDAE8(CM2Model* model, bool a2) {
    reinterpret_cast<uint8_t*>(model->m_scene)[0xdae8] = a2 ? 1 : 0;
}

// ref: FUN_141355d70
bool M2ModelIsAnimationPaused(CM2Model* model) {
    return (model->m_modelFlags >> 0x11 & 1) != 0;
}

// ref: FUN_141355fc0
bool M2ModelHasPauseRequest(CM2Model* model) {
    return model->m_pauseRequest != 0;
}

// ref: FUN_141356d00
void M2ModelSetPausedA(CM2Model* model, bool paused) {
    model->SetPauseRequest(paused ? 1 : 0);
}

// ref: FUN_141356ae0
void M2ModelSetPausedB(CM2Model* model, bool paused) {
    model->SetAnimationPaused(paused ? 1 : 0);
}

// ref: FUN_141356920
void M2ModelSetParticlesA(CM2Model* model, bool on) {
    model->SetParticlesEnabled(on ? 1 : 0);
}

// ref: FUN_141356dd0
void M2ModelSetParticlesB(CM2Model* model, bool on) {
    model->SetRibbonsEnabled(on ? 1 : 0);
}

// ref: FUN_141355d90
float* M2ModelGetBounds(float* out, CM2Model* model) {
    model->GetBox(out);
    return out;
}

// ref: FUN_141356000
float* M2ModelGetCenter(float* out, CM2Model* model, int32_t sequence, int32_t a4) {
    model->GetSequenceCenter(out, static_cast<uint32_t>(sequence), static_cast<uint32_t>(a4));
    return out;
}

// ref: FUN_141355ef0
uint32_t M2ModelGetCameraCount(CM2Model* model) {
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        model->WaitForLoad(0);
    }
    return static_cast<uint16_t>(model->m_shared->m_data->m_cameras.Count());
}

// ref: FUN_1411baf60
CM2Camera* M2ModelGetCameraByIndex(CM2Model* model, uint32_t index) {
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        model->WaitForLoad(0);
    }
    M2Data* data = model->m_shared->m_data;
    if (index < static_cast<uint16_t>(data->m_cameraIndicesById.Count())) {
        uint16_t camera = data->m_cameraIndicesById[index];
        if (camera != 0xffff) {
            model->UpdateCamera(camera);
            return model->m_cameras[camera].m_camera;
        }
    }
    return nullptr;
}

// ref: FUN_1411baff0
CM2Camera* M2ModelGetCameraById(CM2Model* model, uint32_t id) {
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        model->WaitForLoad(0);
    }
    if (static_cast<uint16_t>(model->m_shared->m_data->m_cameras.Count()) <= id) {
        return nullptr;
    }
    model->UpdateCamera(id);
    return model->m_cameras[id].m_camera;
}

// ref: FUN_1411be870
bool M2ModelHasAttachments(CM2Model* model) {
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        model->WaitForLoad(0);
    }
    return static_cast<uint16_t>(model->m_animState->m_attachments->Count()) != 0;
}

// ref: FUN_1411be3a0
int32_t M2ModelGetEffectType(CM2Model* model) {
    if (model->m_effect2 != nullptr) {
        return *static_cast<int32_t*>(model->m_effect2);
    }
    return 0;
}

// ref: FUN_141355e60
float M2ModelGetEffectStrength(CM2Model* model) {
    if (model->m_effect2 != nullptr) {
        return static_cast<float*>(model->m_effect2)[1];
    }
    return 0.0f;
}

// ref: FUN_141355730
void M2ModelClearEffect(CM2Model* model) {
    model->ClearEffect();
}

// ref: FUN_141356cd0
void M2ModelSetEffect(CM2Model* model, int32_t type) {
    model->SetEffect(type, nullptr);
}

// ref: FUN_141356ce0
void M2ModelSetEffectStrength(CM2Model* model, float strength) {
    model->SetEffectStrength(strength);
}

// ref: FUN_1411b7870
void M2ModelResetEmitters(CM2Model* model) {
    uint16_t count = static_cast<uint16_t>(model->m_shared->m_data->m_particles.Count());
    for (uint32_t i = 0; i < count; i++) {
        model->m_particles[i]->m_flags |= 0x8000;
    }
}

void M2ModelSetWorldTransform(CM2Model* model, const float* matrix) {
    model->SetLocalMatrix(matrix);
}

// ref: FUN_1411c8e30
void M2ModelReplaceTexture(CM2Model* model, int32_t slot, void* texture) {
    model->SetReplaceableTexture(slot, static_cast<CTexture*>(texture), nullptr);
}

// ref: FUN_1413567b0
void M2ModelSetSequence(CM2Model* model, int32_t a2, uint32_t sequence, int32_t a4, int32_t timeOffset, float speed,
    int32_t a7, int32_t a8, int32_t a9, int32_t a10, int32_t a11, float a12) {
    model->SetSequence(static_cast<uint32_t>(a2), sequence, static_cast<uint32_t>(a4), timeOffset, speed, a7, a8,
        static_cast<uint8_t>(a9), static_cast<uint16_t>(a10), a11, a12);
}

// ref: FUN_1411cca30
void M2ModelSetCallback420(CM2Model* model, void* callback, void* const* closure) {
    model->m_callback420 = callback;
    model->m_callback420Closure[0] = closure[0];
    model->m_callback420Closure[1] = closure[1];
}

// ref: FUN_1411cc990
void M2ModelSetCallbackCC990(CM2Model* model, void* callback, void* const* closure) {
    if (model->m_408 == callback && model->m_410 == closure[0] && model->m_418 == closure[1]) {
        return;
    }
    model->m_408 = callback;
    model->m_410 = closure[0];
    model->m_418 = closure[1];
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        return;
    }
    if (callback != nullptr) {
        model->m_modelFlags |= 0x800;
        return;
    }
    M2AnimState* anim = model->m_animState;
    if (static_cast<uint16_t>(anim->m_sequences->Count()) < 2 && (anim->m_flags & 1) != 0
        && (model->m_events == nullptr || static_cast<uint16_t>(model->m_shared->m_data->m_events.Count()) == 0)) {
        model->m_modelFlags &= ~0x800u;
    }
}

// ref: FUN_141356c00
void M2ModelSetCallback318(CM2Model* model, void* callback, void* param) {
    model->m_lightingCallback = reinterpret_cast<void (*)(CM2BaseModel*, CM2ModelLighting*, void*)>(callback);
    model->m_lightingParam = param;
}

void M2ModelSetLoadCallback(CM2Model* model, void (*callback)(CM2Model* model, void* param), void* param) {
    model->SetLoadCallback(reinterpret_cast<void (*)(CM2BaseModel*, void*)>(callback), param);
}

// ref: FUN_1411caf50
void M2ModelSetEventCallback(CM2Model* model, void (*callback)(void* param, uint32_t event, void* data), void* const* closure) {
    auto function = reinterpret_cast<void*>(callback);
    if (model->m_events == function && model->m_5a0 == closure[0] && model->m_5a8 == closure[1]) {
        return;
    }
    model->m_events = function;
    model->m_5a0 = closure[0];
    model->m_5a8 = closure[1];
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        return;
    }
    if (function != nullptr && static_cast<uint16_t>(model->m_shared->m_data->m_events.Count()) != 0) {
        model->m_modelFlags |= 0x800;
        return;
    }
    M2AnimState* anim = model->m_animState;
    if (model->m_408 == nullptr && static_cast<uint16_t>(anim->m_sequences->Count()) < 2 && (anim->m_flags & 1) == 0) {
        model->m_modelFlags &= ~0x800u;
    }
}

