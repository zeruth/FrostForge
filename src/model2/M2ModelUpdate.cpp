#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "model2/M2Model.hpp"
#include "util/Unimplemented.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Ribbon.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "os/Sync.hpp"

// Engine/Source/Model2/M2Model.cpp: the per-frame update of a model's animated elements.

namespace {

// The flags the samplers take from the root bone's slots: a parent skeleton's primary or
// blended-from sequence is skipped.
uint32_t RootSlotFlags(const M2BoneState* state) {
    uint32_t flags = 0;
    if (state->m_sequence != nullptr) {
        flags = (state->m_sequence->m_source.m_flags & 1) * 2;
    }
    if (state->m_blendSequence != nullptr) {
        flags |= (state->m_blendSequence->m_source.m_flags & 1) << 2;
    }
    return flags;
}

template <class T>
bool IsAnimated(const M2Track<T>& track) {
    return 1 < track.m_values.Count() || (track.m_values.Count() == 1 && 1 < track.m_values[0].Count());
}

const M2AnimTarget* Target(const M2BoneState* state) {
    return reinterpret_cast<const M2AnimTarget*>(state);
}

float FloatFromBits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

// The lights' tracks and their CM2Lighting colours (FUN_1411b13a0).
// ref: FUN_1411b13a0
void CM2Model::AnimateLights(M2AnimateContext* context) {
    M2Data* data = this->m_shared->m_data;
    M2BoneState* states = this->m_animState->m_boneStates;
    const uint32_t* globalTimes = this->m_animState->m_globalTimes;
    uint32_t tinted = data->m_flags & 0x8000;
    if (data->m_lights.Count() != 0) {
        this->GetRenderScale();
    }
    for (uint32_t i = 0; i < data->m_lights.Count(); i++) {
        M2LightState* light = &this->m_lights[i];
        M2Light* def = &data->m_lights[i];
        M2BoneState* bone = &states[static_cast<uint16_t>(def->m_bone)];
        uint32_t flags = RootSlotFlags(bone);
        bool visible;
        if (context->m_f8 == 0) {
            visible = light->m_64 != 0 && light->m_visibility.m_value != 0;
        } else {
            if (light->m_64 != 0 && def->m_visibility.m_values.Count() != 0) {
                uint8_t on = 1;
                M2TrackSample(globalTimes, Target(bone), &def->m_visibility, &def->m_visibility, &light->m_visibility, &on,
                    flags);
            }
            visible = light->m_64 != 0 && light->m_visibility.m_value != 0;
        }
        if (context->m_f8 != 0 && visible) {
            uint16_t boneIndex = static_cast<uint16_t>(def->m_bone);
            if ((this->m_boneDone[boneIndex >> 3] & static_cast<uint8_t>(1 << (boneIndex & 7))) == 0) {
                this->AnimateBoneTree(boneIndex, context);
            }
            if (IsAnimated(def->m_ambientIntensity)) {
                float zero = 0.0f;
                M2TrackSample(globalTimes, Target(bone), &def->m_ambientIntensity, &def->m_ambientIntensity,
                    &light->m_ambientIntensity, &zero, flags);
            }
            if (IsAnimated(def->m_ambientColor)) {
                C3Vector zero = {0.0f, 0.0f, 0.0f};
                M2TrackSample(globalTimes, Target(bone), &def->m_ambientColor, &def->m_ambientColor, &light->m_ambientColor,
                    &zero, flags);
            }
            if (IsAnimated(def->m_diffuseIntensity)) {
                float zero = 0.0f;
                M2TrackSample(globalTimes, Target(bone), &def->m_diffuseIntensity, &def->m_diffuseIntensity,
                    &light->m_diffuseIntensity, &zero, flags);
            }
            if (IsAnimated(def->m_diffuseColor)) {
                C3Vector zero = {0.0f, 0.0f, 0.0f};
                M2TrackSample(globalTimes, Target(bone), &def->m_diffuseColor, &def->m_diffuseColor, &light->m_diffuseColor,
                    &zero, flags);
            }
            if (tinted != 0) {
                if (IsAnimated(def->m_attenuationStart)) {
                    float zero = 0.0f;
                    M2TrackSample(globalTimes, Target(bone), &def->m_attenuationStart, &def->m_attenuationStart,
                        &light->m_attenuationStart, &zero, flags);
                }
                if (IsAnimated(def->m_attenuationEnd)) {
                    float zero = 0.0f;
                    M2TrackSample(globalTimes, Target(bone), &def->m_attenuationEnd, &def->m_attenuationEnd,
                        &light->m_attenuationEnd, &zero, flags);
                }
            }
        }

        CM2Lighting* lighting = &light->m_lighting;
        float scale = this->m_params.m_alpha * this->m_params.m_fade;
        float shadows = 1.0f;
        if ((~(RenderSettingsRayTracedShadows() >> 1) & 1) == 0 && (lighting->m_flags & 1) != 0) {
            shadows = lighting->m_7c;
        }
        float diffuse = scale * light->m_diffuseIntensity.m_value;
        lighting->m_diffuse[0] = diffuse * light->m_diffuseColor.m_value.x * shadows;
        lighting->m_diffuse[1] = diffuse * light->m_diffuseColor.m_value.y * shadows;
        lighting->m_diffuse[2] = light->m_diffuseColor.m_value.z * diffuse * shadows;
        float ambient = scale * light->m_ambientIntensity.m_value;
        float red = ambient * light->m_ambientColor.m_value.x;
        for (int32_t j = 0; j < 3; j++) {
            lighting->m_ambient[j][0] = red;
            lighting->m_ambient[j][1] = ambient * light->m_ambientColor.m_value.y;
            lighting->m_ambient[j][2] = ambient * light->m_ambientColor.m_value.z;
        }
        // Two files' lights are fixed colours.
        if (this->m_shared != nullptr) {
            int32_t fileDataId = this->m_shared->m_fileDataId;
            if (fileDataId == 0x20382) {
                if (i == 0) {
                    lighting->m_diffuse[0] = FloatFromBits(0x3f9a8af1);
                    lighting->m_diffuse[1] = FloatFromBits(0x3f497a78);
                    lighting->m_diffuse[2] = FloatFromBits(0x3ec4c4c6);
                }
            } else if (fileDataId == 0x203c3 && i == 1) {
                lighting->m_diffuse[0] = FloatFromBits(0x3e97885a);
                lighting->m_diffuse[1] = FloatFromBits(0x3e7efa5f);
                lighting->m_diffuse[2] = FloatFromBits(0x3d09dd6f);
            }
        }
        float clamped[3];
        lighting->GetDiffuse(clamped);
        // DAT_1427211ec, DAT_142721228, DAT_142721214, DAT_142721210
        float magnitude = std::sqrt(clamped[0] * clamped[0] + clamped[1] * clamped[1] + clamped[2] * clamped[2]);
        lighting->m_range = std::sqrt(0.49f - magnitude * -12.0f) * 16.666668f - 11.666667f;
    }
}

// Each light's CM2Lighting placed from its bone and enabled when visible; then the same for the
// models attached to visible points (FUN_1411b1a40).
// ref: FUN_1411b5720
void CM2Model::RunCallback(const M2ModelCallback* callback) {
    if (callback->m_kind == 0) {
        this->RunSequenceCallback(callback);
        return;
    }
    if (callback->m_kind == 1) {
        auto function = reinterpret_cast<void (*)(CM2Model* model, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
            const void* a6, uint32_t a7, const float* a8)>(callback->m_function);
        function(callback->m_model, callback->m_10, callback->m_14, callback->m_20, callback->m_24, callback->m_28,
            callback->m_38, callback->m_40);
    }
}

// ref: FUN_1411b6750
void CM2Model::UpdateProjectedBatches() {
    CM2Scene* scene = this->m_scene;
    if (scene->m_projectCheckCallback == nullptr || scene->m_projectCallback == nullptr) {
        return;
    }
    const CM2SkinProfileData* skin = &this->m_shared->m_skins[this->m_lod];
    uint16_t count;
    if (this->m_600 == nullptr) {
        count = skin->m_profile->m_batches.m_count;
    } else {
        count = *static_cast<const uint16_t*>(this->m_600);
    }
    for (uint32_t i = 0; i < count; i++) {
        this->m_5f8[i] = nullptr;
        const uint8_t* batch;
        const void* section;
        if (this->m_600 == nullptr) {
            batch = reinterpret_cast<const uint8_t*>(&skin->m_profile->m_batches[i]);
            uint16_t index = *reinterpret_cast<const uint16_t*>(batch + 4);
            section = &skin->m_sections[index];
            if (((1 << (index & 7)) & this->m_lodBoneMasks[this->m_lod][index >> 3]) == 0) {
                continue;
            }
        } else {
            auto lod = static_cast<const uint8_t*>(this->m_600);
            batch = *reinterpret_cast<const uint8_t* const*>(lod + 0x10) + i * 0x18;
            uint16_t index = *reinterpret_cast<const uint16_t*>(batch + 4);
            section = *reinterpret_cast<const uint8_t* const*>(lod + 0x20) + index * 0x30;
        }
        if ((*batch & 4) != 0) {
            uint64_t query[5] = {};
            query[0] = reinterpret_cast<uint64_t>(scene->m_owner);
            query[1] = reinterpret_cast<uint64_t>(scene->m_030);
            this->BuildBatchProjection(query, section);
            this->m_5f8[i] = scene->m_projectCallback(query);
        }
    }
}

// The projection's box and matrix through the scene's camera (1954 bytes over FUN_1411bf190);
// reached only through the world scene's projection callbacks.
// ref: FUN_1411be9e0
bool CM2Model::BuildBatchProjection(void* query, const void* section) {
    (void)query;
    (void)section;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1411b1a40
void CM2Model::UpdateLights() {
    M2Data* data = this->m_shared->m_data;
    if ((this->m_flags & BASE_LOADED) != 0 && data != nullptr) {
        for (uint32_t i = 0; i < data->m_lights.Count(); i++) {
            M2LightState* light = &this->m_lights[i];
            M2Light* def = &data->m_lights[i];
            // A light ray traced shadows replace stays off while they are on.
            bool shadowOnly = (~(RenderSettingsRayTracedShadows() >> 1) & 1) == 0
                || (light->m_lighting.m_flags & 2) == 0;
            int32_t enabled = 0;
            if (light->m_64 != 0 && light->m_visibility.m_value != 0 && shadowOnly) {
                const float* bone = this->m_animState->m_boneMatrices[static_cast<uint16_t>(def->m_bone)].m;
                if (def->m_type == 1) {
                    // A point light: the bone's position through the world (and the camera).
                    float position[4];
                    const float* m;
                    bool local = this->m_parent == nullptr || (this->m_flags >> 14 & 1) != 0;
                    m = local ? this->m_local.m : this->m_world.m;
                    float w[16];
                    MatrixMultiply(w, bone, m);
                    float x = def->m_position.x;
                    float y = def->m_position.y;
                    float z = def->m_position.z;
                    position[0] = w[4] * y + w[0] * x + w[8] * z + w[12];
                    position[1] = w[5] * y + w[1] * x + w[9] * z + w[13];
                    position[2] = w[6] * y + w[2] * x + w[10] * z + w[14];
                    position[3] = w[7] * y + w[3] * x + w[11] * z + w[15];
                    if (!local) {
                        const float* v = this->m_scene->m_inverseView.m;
                        float p[4] = {position[0], position[1], position[2], position[3]};
                        position[0] = p[3] * v[12] + p[2] * v[8] + p[1] * v[4] + p[0] * v[0];
                        position[1] = p[3] * v[13] + p[2] * v[9] + p[1] * v[5] + p[0] * v[1];
                        position[2] = p[3] * v[14] + p[2] * v[10] + p[1] * v[6] + p[0] * v[2];
                        position[3] = p[3] * v[15] + p[2] * v[11] + p[1] * v[7] + p[0] * v[3];
                    }
                    light->m_lighting.SetPosition(position);
                } else {
                    // A directional light: the bone's -Z through the world and the camera's rotation.
                    float dx = -(bone[8] * this->m_world.m[0] + bone[9] * this->m_world.m[4] + bone[10] * this->m_world.m[8]
                        + bone[11] * this->m_world.m[12]);
                    float dy = -(bone[8] * this->m_world.m[1] + bone[9] * this->m_world.m[5] + bone[10] * this->m_world.m[9]
                        + bone[11] * this->m_world.m[13]);
                    float dz = -(bone[8] * this->m_world.m[2] + bone[9] * this->m_world.m[6] + bone[10] * this->m_world.m[10]
                        + bone[11] * this->m_world.m[14]);
                    const float* v = this->m_scene->m_inverseView.m;
                    float direction[4] = {
                        dy * v[4] + dx * v[0] + dz * v[8],
                        dy * v[5] + dx * v[1] + dz * v[9],
                        dy * v[6] + dx * v[2] + dz * v[10],
                        dy * v[7] + dx * v[3] + dz * v[11],
                    };
                    light->m_lighting.SetDirection(direction);
                }
                enabled = 1;
            }
            light->m_lighting.SetEnabled(enabled);
            light->m_lighting.m_08 = this->m_scene->m_frame;
        }
    }
    for (CM2Model* child = this->m_attachedModels; child != nullptr; child = child->m_attachedNext) {
        bool hidden;
        if (child->m_attachIndex == 0xffff) {
            hidden = (child->m_flags & BASE_ATTACHED_HIDDEN) == 0;
        } else {
            hidden = this->m_animState->m_attachmentStates[child->m_attachIndex].m_value == 0;
        }
        if (!hidden) {
            child->UpdateLights();
        }
    }
}

// The model's bone-done bits cleared and its transform brought up (FUN_1411b61c0).
// ref: FUN_1411b61c0
void CM2Model::ResetBoneState(const C44Matrix* parent, const CM2ModelParams* params, uint32_t frame, int32_t time) {
    std::memset(this->m_boneDone, 0, this->m_boneDoneBytes);
    this->UpdateTransform(parent, params, time);
    if (this->m_skeleton != nullptr) {
        M2SkeletonSetFrame(this->m_skeleton, frame, time);
    }
    this->m_matrixFrame = frame;
}

// ref: FUN_1411b1fe0
void CM2Model::Animate(const C44Matrix* parentMatrix, const CM2ModelParams* params, uint32_t frame, int32_t time,
    int32_t throttle) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        return;
    }
    if (this->m_matrixFrame == frame) {
        this->UpdateMatricesFrom(params);
    } else {
        this->ResetBoneState(parentMatrix, params, frame, time);
    }
    bool compute = true;
    float ratio = 0.0f;
    M2Data* data = this->m_shared->m_data;
    float height;
    if (data->m_bounds.m_max.x < data->m_bounds.m_min.x || data->m_bounds.m_max.y < data->m_bounds.m_min.y
        || data->m_bounds.m_max.z < data->m_bounds.m_min.z) {
        height = 0.0f;
    } else {
        height = data->m_bounds.m_max.y - data->m_bounds.m_min.y;
    }
    if (1.1754944e-35f < std::fabs(height)) {  // DAT_1426c8880
        ratio = this->m_screenSize / height;
    }
    if (throttle == 0) {
        this->m_lastAnimateTime = static_cast<uint32_t>(time);
    } else if (ratio <= 800.0f) {  // DAT_142cfb140
        compute = true;
        this->m_lastAnimateTime = static_cast<uint32_t>(time);
    } else {
        uint32_t since = static_cast<uint32_t>(time) - this->m_lastAnimateTime;
        compute = 0x21 < since;
        if (compute) {
            this->m_lastAnimateTime = static_cast<uint32_t>(time);
        }
    }
    if ((this->m_shared->m_flags & CM2Shared::SHARED_HAS_PHYSICS_BONES) != 0 && this->m_4c0 == 0) {
        this->InitializePhysicsBones();
    }
    M2AnimateContext context;
    M2AnimateContextInit(&context, static_cast<uint32_t>(time), frame, compute ? 1 : 0, static_cast<uint32_t>(throttle),
        M2SceneGetAnimateRatio(this->m_scene) < ratio ? 1 : 0, 1);
    if (compute && (this->m_modelFlags2 & 0x800) == 0) {
        M2BoneState* states = this->m_animState->m_boneStates;
        M2Array<M2CompBone>* bones = this->m_animState->m_bones;
        uint32_t lodMask = M2SharedLodBoneMask(this->m_shared, this->m_lod);
        if ((this->m_boneDone[0] & 1) == 0) {
            this->AnimateBone(&(*bones)[0], &states[0], 0, &context);
        }
        for (uint32_t i = 0; i < bones->Count(); i++) {
            uint32_t index = i;
            if (this->m_shared->m_boneOrder != nullptr) {
                index = this->m_shared->m_boneOrder[i];
            }
            const M2CompBone* bone = &(*bones)[index];
            if ((bone->m_flags & lodMask) != 0) {
                break;
            }
            M2BoneState* state = &states[index];
            if (((bone->m_flags | state->m_flags) & 0x2180000) != 0
                && (this->m_boneDone[index >> 3] & static_cast<uint8_t>(1 << (index & 7))) == 0) {
                this->AnimateBone(bone, state, index, &context);
            }
        }
    }
    if (data->m_colors.Count() != 0) {
        M2BoneState* root = this->m_animState->m_boneStates;
        const uint32_t* globalTimes = this->m_animState->m_globalTimes;
        uint32_t flags = RootSlotFlags(root);
        for (uint32_t i = 0; i < data->m_colors.Count(); i++) {
            M2Color* color = &data->m_colors[i];
            M2ColorState* state = &this->m_colors[i];
            if (IsAnimated(color->m_color)) {
                C3Vector zero = {0.0f, 0.0f, 0.0f};
                M2TrackSample(globalTimes, Target(root), &color->m_color, &color->m_color, &state->m_color, &zero, flags);
            }
            if (IsAnimated(color->m_alpha)) {
                float one = 1.0f;
                M2TrackSampleFixed16(globalTimes, Target(root), &color->m_alpha, &color->m_alpha, &state->m_alpha, &one,
                    flags);
            }
        }
    }
    if (data->m_textureWeights.Count() != 0) {
        this->AnimateTextureWeights();
    }
    if (data->m_textureTransforms.Count() != 0) {
        this->AnimateTextureTransforms(&context);
    }
    if (data->m_lights.Count() != 0) {
        this->AnimateLights(&context);
    }
    if (compute) {
        if (data->m_cameras.Count() != 0) {
            this->AnimateCameras();
        }
        if (data->m_ribbons.Count() != 0) {
            this->AnimateRibbonTracks(&context);
        }
    }
    this->m_modelFlags &= ~8u;
    if (data->m_particles.Count() != 0) {
        this->AnimateParticleTracks(&context);
    }
    if (this->m_animState->m_attachmentStates != nullptr || this->m_children != nullptr) {
        this->UpdateChildren(&context);
    }
    if ((this->m_modelFlags & 0x800000) != 0) {
        this->UpdateAccumulatedBounds();
    }
}

// The particle emitters' tracks and whether each is on (FUN_1411b2640).
// ref: FUN_1411b2640
void CM2Model::AnimateParticleTracks(M2AnimateContext* context) {
    if ((this->m_modelFlags2 >> 1 & 1) != 0 && context->m_100 != 0) {
        return;
    }
    M2BoneState* states = this->m_animState->m_boneStates;
    const uint32_t* globalTimes = this->m_animState->m_globalTimes;
    for (uint32_t i = 0; i < this->m_shared->m_data->m_particles.Count(); i++) {
        M2ParticleState* state = &this->m_particleStates[i];
        M2Particle* def = &this->m_shared->m_data->m_particles[i];
        CParticleEmitter2* emitter = this->m_particles[i];
        M2BoneState* bone = &states[def->m_bone];
        uint32_t flags = RootSlotFlags(bone);
        if (context->m_f8 != 0 && IsAnimated(def->m_enabledIn)) {
            uint8_t on = 1;
            M2TrackSample(globalTimes, Target(bone), &def->m_enabledIn, &def->m_enabledIn, &state->m_enabled, &on, flags);
        }
        uint8_t active;
        if (state->m_enabled.m_value == 0 || (emitter->m_flags & 2) == 0) {
            state->m_8c = 0;
            active = ParticleEmitterHasParticles(emitter) ? 1 : 0;
        } else {
            state->m_8c = 1;
            active = 1;
        }
        state->m_8d = active;
        this->m_modelFlags |= static_cast<uint32_t>(active) << 3;
        if (context->m_f8 != 0 && state->m_enabled.m_value != 0) {
            uint16_t boneIndex = def->m_bone;
            if ((this->m_boneDone[boneIndex >> 3] & static_cast<uint8_t>(1 << (boneIndex & 7))) == 0
                && (this->m_modelFlags2 & 0x800) == 0) {
                this->AnimateBoneTree(boneIndex, context);
            }
            float zero = 0.0f;
            if (IsAnimated(def->m_emissionSpeed)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_emissionSpeed, &def->m_emissionSpeed,
                    &state->m_emissionSpeed, &zero, flags);
            }
            if (IsAnimated(def->m_speedVariation)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_speedVariation, &def->m_speedVariation,
                    &state->m_speedVariation, &zero, flags);
            }
            if (IsAnimated(def->m_verticalRange)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_verticalRange, &def->m_verticalRange,
                    &state->m_verticalRange, &zero, flags);
            }
            if (IsAnimated(def->m_horizontalRange)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_horizontalRange, &def->m_horizontalRange,
                    &state->m_horizontalRange, &zero, flags);
            }
            auto extra = static_cast<M2Track<C3Vector>*>(this->m_shared->m_particleData);
            if (extra != nullptr) {
                M2Track<C3Vector>* track = &extra[i];
                if (IsAnimated(*track)) {
                    C3Vector none = {0.0f, 0.0f, 0.0f};
                    M2TrackSample(globalTimes, Target(bone), track, track, &state->m_18, &none, flags);
                }
            }
            if (IsAnimated(def->m_lifespan)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_lifespan, &def->m_lifespan, &state->m_lifespan, &zero,
                    flags);
            }
            if (IsAnimated(def->m_emissionRate)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_emissionRate, &def->m_emissionRate,
                    &state->m_emissionRate, &zero, flags);
            }
            if (IsAnimated(def->m_emissionAreaLength)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_emissionAreaLength, &def->m_emissionAreaLength,
                    &state->m_emissionAreaLength, &zero, flags);
            }
            if (IsAnimated(def->m_emissionAreaWidth)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_emissionAreaWidth, &def->m_emissionAreaWidth,
                    &state->m_emissionAreaWidth, &zero, flags);
            }
            if (IsAnimated(def->m_zSource)) {
                M2TrackSample(globalTimes, Target(bone), &def->m_zSource, &def->m_zSource, &state->m_zSource, &zero,
                    flags);
            }
        }
    }
}

// The ribbon emitters' bones and tracks (FUN_1411b3490).
// ref: FUN_1411b3490
void CM2Model::AnimateRibbonTracks(M2AnimateContext* context) {
    M2Data* data = this->m_shared->m_data;
    const uint32_t* globalTimes = this->m_animState->m_globalTimes;
    M2BoneState* states = this->m_animState->m_boneStates;
    for (uint32_t i = 0; i < data->m_ribbons.Count(); i++) {
        M2Ribbon* def = &data->m_ribbons[i];
        M2RibbonState* state = &this->m_ribbonStates[i];
        uint32_t boneIndex = def->m_boneIndex & 0xffff;
        M2BoneState* bone = &states[boneIndex];
        uint32_t flags = RootSlotFlags(bone);
        if ((this->m_boneDone[boneIndex >> 3] & static_cast<uint8_t>(1 << (boneIndex & 7))) == 0) {
            const M2CompBone* compBone = &(*this->m_animState->m_bones)[boneIndex];
            uint16_t parent = static_cast<uint16_t>(compBone->m_parentBone);
            if (parent != 0xffff && (this->m_boneDone[parent >> 3] & static_cast<uint8_t>(1 << (parent & 7))) == 0) {
                this->AnimateBoneTree(parent, context);
            }
            this->AnimateBone(compBone, &this->m_animState->m_boneStates[boneIndex], boneIndex, context);
        }
        if (IsAnimated(def->m_visibility)) {
            uint8_t on = 1;
            M2TrackSample(globalTimes, Target(bone), &def->m_visibility, &def->m_visibility, &state->m_visibility, &on,
                flags);
        }
        if (IsAnimated(def->m_color)) {
            C3Vector zero = {0.0f, 0.0f, 0.0f};
            M2TrackSample(globalTimes, Target(bone), &def->m_color, &def->m_color, &state->m_color, &zero, flags);
        }
        if (IsAnimated(def->m_alpha)) {
            float one = 1.0f;
            M2TrackSampleFixed16(globalTimes, Target(bone), &def->m_alpha, &def->m_alpha, &state->m_alpha, &one, flags);
        }
        if (IsAnimated(def->m_heightAbove)) {
            float zero = 0.0f;
            M2TrackSample(globalTimes, Target(bone), &def->m_heightAbove, &def->m_heightAbove, &state->m_heightAbove,
                &zero, flags);
        }
        if (IsAnimated(def->m_heightBelow)) {
            float zero = 0.0f;
            M2TrackSample(globalTimes, Target(bone), &def->m_heightBelow, &def->m_heightBelow, &state->m_heightBelow,
                &zero, flags);
        }
    }
}

// The ribbons' colours, heights, texture transforms and matrices, then their update
// (FUN_1411b3870).
// ref: FUN_1411b3870
void CM2Model::UpdateRibbons(float delta) {
    M2Data* data = this->m_shared->m_data;
    C44Matrix* matrices = this->m_animState->m_boneMatrices;
    for (uint32_t i = 0; i < data->m_ribbons.Count(); i++) {
        M2Ribbon* def = &data->m_ribbons[i];
        M2RibbonState* state = &this->m_ribbonStates[i];
        CRibbonEmitter* ribbon = this->m_ribbons[i];
        RibbonEmitterSetColor(ribbon, state->m_color.m_value.x, state->m_color.m_value.y, state->m_color.m_value.z);
        RibbonEmitterSetAlpha(ribbon, state->m_alpha.m_value * this->m_params.m_alpha * this->m_params.m_fade);
        ribbon->m_heightAbove = state->m_heightAbove.m_value;
        ribbon->m_heightBelow = state->m_heightBelow.m_value;
        RibbonEmitterSetVisible(ribbon, state->m_visibility.m_value != 0);
        if ((data->m_flags & 0x20000) != 0) {
            uint32_t layers = ribbon->m_texture2 == nullptr ? 1 : 3;
            C4Quaternion rotation = {0.0f, 0.0f, 0.0f, 1.0f};
            C3Vector scale = {0.0f, 0.0f, 0.0f};
            C3Vector translation = {0.0f, 0.0f, 0.0f};
            for (uint32_t layer = 0; layer < layers; layer++) {
                uint16_t transform = data->m_textureTransformCombos[def->m_textureTransformLookupIndex + layer];
                if (transform < data->m_textureTransforms.Count()) {
                    if ((this->m_flags & BASE_LOADED) != 0) {
                        M2TextureTransformState* t = &this->m_textureTransforms[transform];
                        rotation = t->m_rotation.m_value;
                        scale = t->m_scale.m_value;
                        translation = t->m_translation.m_value;
                    }
                    RibbonEmitterSetTextureTransform(ribbon, layer, &rotation, &scale, &translation);
                }
            }
        }
        if ((this->m_flags & BASE_VISIBLE) != 0) {
            const float* bone = matrices[def->m_boneIndex & 0xffff].m;
            float m[16];
            if (this->m_parent == nullptr || (this->m_flags >> 14 & 1) != 0) {
                // Through the local matrix: the bone's rotation rows and its position moved to the
                // emitter's point.
                MatrixMultiply(m, bone, this->m_local.m);
                float px = def->m_position.x;
                float py = def->m_position.y;
                float pz = def->m_position.z;
                float origin[4] = {
                    bone[12] + py * bone[4] + px * bone[0] + pz * bone[8],
                    bone[13] + py * bone[5] + px * bone[1] + pz * bone[9],
                    bone[14] + py * bone[6] + px * bone[2] + pz * bone[10],
                    bone[15],
                };
                const float* l = this->m_local.m;
                for (int32_t c = 0; c < 4; c++) {
                    m[12 + c] = origin[0] * l[c] + origin[1] * l[4 + c] + origin[2] * l[8 + c] + origin[3] * l[12 + c];
                }
            } else {
                // Through the world and the camera.
                float w[16];
                MatrixMultiply(w, bone, this->m_world.m);
                float px = def->m_position.x;
                float py = def->m_position.y;
                float pz = def->m_position.z;
                const float* wv = this->m_world.m;
                float origin[4];
                for (int32_t c = 0; c < 4; c++) {
                    origin[c] = bone[15] * wv[12 + c] + bone[14] * wv[8 + c] + bone[12] * wv[c] + bone[13] * wv[4 + c];
                    if (c < 3) {
                        origin[c] = origin[c] + py * w[4 + c] + px * w[c] + pz * w[8 + c];
                    }
                }
                std::memcpy(w + 12, origin, sizeof(origin));
                MatrixMultiply(m, w, this->m_scene->m_inverseView.m);
            }
            RibbonEmitterSetMatrix(ribbon, m, this->m_558);
            RibbonEmitterUpdate(ribbon, delta, state->m_visibility.m_value == 0);
        }
    }
}

// The cameras, ribbons and particles brought up to date, the model listed for drawing and its
// attachments told (FUN_1411b3fc0).
// ref: FUN_1411b3fc0
void CM2Model::UpdateDrawables(int32_t pass, float delta, uint32_t a4) {
    M2Data* data = this->m_shared->m_data;
    if (data->m_cameras.Count() != 0) {
        this->UpdateHeldCameras();
    }
    if (data->m_ribbons.Count() != 0) {
        this->UpdateRibbons(delta);
    }
    float ratio = 0.0f;
    float height;
    if (data->m_bounds.m_max.x < data->m_bounds.m_min.x || data->m_bounds.m_max.y < data->m_bounds.m_min.y
        || data->m_bounds.m_max.z < data->m_bounds.m_min.z) {
        height = 0.0f;
    } else {
        height = data->m_bounds.m_max.y - data->m_bounds.m_min.y;
    }
    if (1.1754944e-35f < std::fabs(height)) {
        ratio = this->m_screenSize / height;
    }
    if (data->m_particles.Count() != 0
        && ((this->m_modelFlags2 >> 1 & 1) == 0 || ratio <= M2SceneGetAnimateRatio(this->m_scene))) {
        this->UpdateParticles(delta, static_cast<uint32_t>(pass), a4);
    }
    bool listed = false;
    if ((this->m_flags & BASE_SHOWN) != 0 && this->m_scene->m_collectDrawParams) {
        BaseModelDrawParams* params = M2SceneAddDrawParams(
            reinterpret_cast<uint8_t*>(this->m_scene) + static_cast<int64_t>(pass) * 0x18 + 0x328);
        params->m_model = this;
        float box[6];
        this->GetBounds(box);
        std::memcpy(params->m_box, box, sizeof(box));
        listed = true;
    }
    if ((this->m_modelFlags & 8) != 0 && listed) {
        BaseModelDrawParams* params = M2SceneAddDrawParams(
            reinterpret_cast<uint8_t*>(this->m_scene) + static_cast<int64_t>(pass) * 0x18 + 0x928);
        std::memset(params, 0, sizeof(BaseModelDrawParams));
        params->m_model = this;
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        bool shown;
        if (child->m_attachIndex == 0xffff) {
            shown = (child->m_flags >> 24 & 1) != 0;
        } else {
            shown = this->IsAttachmentVisible(child->m_attachIndex);
        }
        if (shown) {
            child->PrepareLighting(pass, a4);
        }
    }
}

// The model's matrices from its parent's, the global sequences' clocks, and how big it looks
// (FUN_1411b41e0).
// ref: FUN_1411b41e0
void CM2Model::UpdateTransform(const C44Matrix* parent, const CM2ModelParams* params, int32_t time) {
    M2Data* data = this->m_shared->m_data;
    this->UpdateMatricesFrom(params);
    int32_t paused = static_cast<int32_t>(this->m_pauseRequest);
    this->m_3b8 = 0;
    if (paused != 0 && time != 0) {
        this->m_pauseRequest = static_cast<uint32_t>(time);
        int32_t delta = time - paused;
        this->m_3b8 = static_cast<uint32_t>(delta);
        if ((this->m_modelFlags & 0x20000) != 0) {
            this->m_initTime = static_cast<uint32_t>(static_cast<int32_t>(this->m_initTime) + delta);
        }
    }
    if (this->m_skeleton == nullptr) {
        for (uint32_t i = 0; i < data->m_loops.Count(); i++) {
            uint32_t length = data->m_loops[i].m_timestamp;
            uint32_t value = 0;
            if (length != 0) {
                value = static_cast<uint32_t>(static_cast<int32_t>(
                            static_cast<float>(time - static_cast<int32_t>(this->m_initTime)) * this->m_404))
                    % length;
            }
            this->m_globalTimes[i] = value;
        }
    }
    this->SetWorldMatrix(parent);
    CM2Model* above = this->m_parentModel;
    if ((this->m_modelFlags2 & 4) == 0) {
        if (above == nullptr || (above->m_createFlags & 1) != 0) {
            this->m_screenSize = this->m_world.m[12] * this->m_world.m[12] + this->m_world.m[13] * this->m_world.m[13]
                + this->m_world.m[14] * this->m_world.m[14];
        } else {
            this->m_screenSize = above->m_screenSize;
        }
    } else {
        float reach = std::sqrt(this->m_world.m[12] * this->m_world.m[12] + this->m_world.m[13] * this->m_world.m[13]
                          + this->m_world.m[14] * this->m_world.m[14])
            + std::sqrt(this->m_local.m[0] * this->m_local.m[0] + this->m_local.m[1] * this->m_local.m[1]
                  + this->m_local.m[2] * this->m_local.m[2])
                * data->m_boundsRadius;
        reach = reach * reach;
        this->m_screenSize = reach;
        if (above != nullptr) {
            float other = above->m_screenSize;
            this->m_screenSize = other <= reach ? reach : other;
        }
    }
    if (this->m_shared != nullptr && this->m_shared->m_nerf != nullptr) {
        // The camera's distance to the model's box, negative inside it.
        float box[6];
        this->GetBoundingBox(box);
        bool empty = !(box[0] <= box[3] || box[1] <= box[4] || box[2] <= box[5]);
        if (empty) {
            this->m_43c = -10000.0f;
            return;
        }
        float inverse[16];
        MatrixInverse(this->m_view.m, inverse, MatrixDeterminant(this->m_view.m));
        const float* camera = this->m_scene->m_inverseView.m + 12;
        float cx = camera[1] * inverse[4] + camera[0] * inverse[0] + camera[2] * inverse[8] + inverse[12];
        float cy = camera[1] * inverse[5] + camera[0] * inverse[1] + camera[2] * inverse[9] + inverse[13];
        float cz = camera[1] * inverse[6] + camera[0] * inverse[2] + camera[2] * inverse[10] + inverse[14];
        float dx = std::fabs(cx - (box[0] + box[3]) * 0.5f) - (box[3] - box[0]) * 0.5f;
        float dy = std::fabs(cy - (box[1] + box[4]) * 0.5f) - (box[4] - box[1]) * 0.5f;
        float dz = std::fabs(cz - (box[2] + box[5]) * 0.5f) - (box[5] - box[2]) * 0.5f;
        float distance;
        if (0.0f <= dx || 0.0f <= dy || 0.0f <= dz) {
            if (dy <= 0.0f) {
                dy = 0.0f;
            }
            if (dx <= 0.0f) {
                dx = 0.0f;
            }
            if (dz <= 0.0f) {
                dz = 0.0f;
            }
            distance = dy * dy + dx * dx + dz * dz;
        } else {
            float inside = dx <= dy ? dx : dy;
            if (dz <= inside) {
                inside = dz;
            }
            distance = -(inside * inside);
        }
        this->m_43c = distance;
    }
}

// The model's draw parameters from its parent's (FUN_1411ce630): colour multiplied, colour
// added, alpha multiplied, the effect inherited unless the model has its own, and the fade by
// the file's NERF distances.
// ref: FUN_1411ce630
void CM2Model::UpdateMatricesFrom(const CM2ModelParams* parent) {
    CM2Shared* shared = this->m_shared;
    uint8_t fileFlags = static_cast<uint8_t>(shared->m_data->m_flags);
    this->m_params.m_color = this->m_color;
    if ((fileFlags & 4) == 0) {
        if ((this->m_modelFlags & 0x200000) == 0) {
            this->m_params.m_color.x = parent->m_color.x * this->m_params.m_color.x;
            this->m_params.m_color.y = parent->m_color.y * this->m_params.m_color.y;
            this->m_params.m_color.z = parent->m_color.z * this->m_params.m_color.z;
        }
        this->m_params.m_colorAdd = this->m_colorAdd;
        if ((this->m_modelFlags & 0x100) == 0) {
            this->m_params.m_colorAdd.x = parent->m_colorAdd.x + this->m_params.m_colorAdd.x;
            this->m_params.m_colorAdd.y = parent->m_colorAdd.y + this->m_params.m_colorAdd.y;
            this->m_params.m_colorAdd.z = parent->m_colorAdd.z + this->m_params.m_colorAdd.z;
            shared = this->m_shared;
        }
        float alpha = this->m_alpha;
        float colorAlpha = alpha * this->m_32c;
        this->m_params.m_alpha = alpha;
        this->m_params.m_colorAlpha = colorAlpha;
        if ((this->m_modelFlags >> 9 & 1) == 0) {
            this->m_params.m_alpha = alpha * parent->m_alpha;
            this->m_params.m_colorAlpha = colorAlpha * parent->m_colorAlpha;
        }
        this->m_params.m_effect = nullptr;
        if (this->m_effect2 == nullptr) {
            if ((this->m_modelFlags >> 22 & 1) == 0 && parent->m_effect != nullptr) {
                this->m_params.m_effect = parent->m_effect;
            }
        } else {
            this->m_params.m_effect = this->m_effect2;
        }
    } else {
        this->m_params.m_colorAdd = this->m_colorAdd;
        this->m_params.m_alpha = this->m_alpha;
        this->m_params.m_effect = this->m_effect2;
        this->m_params.m_colorAlpha = this->m_alpha * this->m_32c;
    }
    float fade = 1.0f;
    if (shared != nullptr) {
        const float* nerf = static_cast<const float*>(shared->m_nerf);
        if (nerf != nullptr) {
            float distance = this->m_43c;
            if (nerf[1] < distance) {
                float limit = nerf[0];
                if (limit < distance) {
                    goto done;
                }
                float span = limit - nerf[1];
                if (span <= 1.0f) {
                    span = 1.0f;
                }
                fade = 1.0f - (limit - distance) / span;
                if (0.0f <= fade) {
                    if (1.0f <= fade) {
                        fade = 1.0f;
                    }
                    goto done;
                }
            }
            fade = 0.0f;
        }
    }
done:
    this->m_params.m_matrix = this->m_558;
    this->m_params.m_fade = this->m_334 * parent->m_fade * fade;
}

// One texture transform's matrix: rotation and scale about the texture's centre, then the
// translation (FUN_1411b45e0).
// ref: FUN_1411b45e0
void CM2Model::AnimateTextureTransform(uint32_t index, M2BoneState* state) {
    const uint32_t* globalTimes = this->m_animState->m_globalTimes;
    M2TextureTransform* def = &this->m_shared->m_data->m_textureTransforms[index];
    float* m = this->m_textureMatrices[index].m;
    M2TextureTransformState* transform = &this->m_textureTransforms[index];
    MatrixIdentity(m);
    // DAT_142cfb148
    static const float center[3] = {0.5f, 0.5f, 0.0f};
    const float back[3] = {-center[0], -center[1], -center[2]};
    if (def->m_rotation.m_values.Count() != 0) {
        C4Quaternion identity = {0.0f, 0.0f, 0.0f, 1.0f};
        M2TrackSample(globalTimes, Target(state), &def->m_rotation, &def->m_rotation, &transform->m_rotation, &identity,
            0);
        MatrixTranslate(m, center);
        MatrixRotateQuaternion(m, &transform->m_rotation.m_value.x);
        MatrixTranslate(m, back);
    }
    if (def->m_scale.m_values.Count() != 0) {
        C3Vector one = {1.0f, 1.0f, 1.0f};
        M2TrackSample(globalTimes, Target(state), &def->m_scale, &def->m_scale, &transform->m_scale, &one, 0);
        MatrixTranslate(m, center);
        MatrixScale(m, &transform->m_scale.m_value.x);
        MatrixTranslate(m, back);
    }
    if (def->m_translation.m_values.Count() != 0) {
        C3Vector zero = {0.0f, 0.0f, 0.0f};
        M2TrackSample(globalTimes, Target(state), &def->m_translation, &def->m_translation, &transform->m_translation,
            &zero, 0);
        MatrixTranslate(m, &transform->m_translation.m_value.x);
    }
}

// ref: FUN_1411b4830
void CM2Model::AnimateTextureTransforms(M2AnimateContext* context) {
    M2BoneState* states = this->m_animState->m_boneStates;
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    M2Data* data = this->m_shared->m_data;
    uint16_t count = data->m_textureTransforms.m_count;
    if ((data->m_flags & 0x800) == 0) {
        for (uint32_t i = 0; i < count; i++) {
            this->AnimateTextureTransform(i, states);
            count = this->m_shared->m_data->m_textureTransforms.m_count;
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            if ((this->m_flags & BASE_LOADED) == 0) {
                this->WaitForLoad(0);
            }
            uint16_t bone = this->m_shared->m_data->m_textureTransformBoneMap[i];
            if ((this->m_boneDone[bone >> 3] & static_cast<uint8_t>(1 << (bone & 7))) == 0) {
                this->AnimateBoneTree(bone, context);
            }
            this->AnimateTextureTransform(i, &states[bone]);
            count = this->m_shared->m_data->m_textureTransforms.m_count;
        }
    }
}

// The texture weights, from the parent's blocklist animation data when the file has some
// (FUN_1411b4980).
// ref: FUN_1411b4980
void CM2Model::AnimateTextureWeights() {
    M2Data* data = this->m_shared->m_data;
    M2BoneState* root = this->m_animState->m_boneStates;
    const uint32_t* globalTimes = this->m_animState->m_globalTimes;
    uint32_t flags = RootSlotFlags(root);
    const M2Array<M2TextureWeight>* primary = &data->m_textureWeights;
    const M2Array<M2TextureWeight>* blend = &data->m_textureWeights;
    auto parentWeights = static_cast<const M2Array<M2TextureWeight>*>(this->m_shared->m_parentAnimData);
    if (parentWeights != nullptr) {
        if ((flags & 2) != 0) {
            primary = parentWeights;
        }
        if ((flags & 4) != 0) {
            blend = parentWeights;
        }
        flags = 0;
    }
    for (uint32_t i = 0; i < data->m_textureWeights.Count(); i++) {
        const M2Track<uint16_t>& own = data->m_textureWeights[i].m_weight;
        if (IsAnimated(own) || this->m_shared->m_parentAnimData != nullptr) {
            float one = 1.0f;
            M2TrackSampleFixed16(globalTimes, Target(root), &(*primary)[i].m_weight, &(*blend)[i].m_weight,
                &this->m_textureWeights[i], &one, flags);
        }
    }
}

// The attachment points' visibility tracks, then each shown child's matrices: from its
// attachment point (or this model's world matrix), turned to the camera for billboarded
// children, with a child's extra matrix applied in this model's space (FUN_1411ada40).
// ref: FUN_1411ada40
void CM2Model::UpdateChildren(M2AnimateContext* context) {
    M2AnimState* anim = this->m_animState;
    M2Array<M2Attachment>* attachments = anim->m_attachments;
    M2TrackState<uint8_t>* states = anim->m_attachmentStates;
    M2BoneState* bones = anim->m_boneStates;
    const uint32_t* globalTimes = anim->m_globalTimes;
    for (uint32_t i = 0; i < attachments->Count(); i++) {
        M2Attachment& attachment = (*attachments)[i];
        if (context->m_f8 != 0 && IsAnimated(attachment.m_animate)) {
            uint8_t shown = 1;
            M2TrackSample(globalTimes, Target(&bones[attachment.m_bone]), &attachment.m_animate, &attachment.m_animate,
                &states[i], &shown, 0);
        }
    }
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        uint8_t childShown = child->m_type == 0 ? static_cast<uint8_t>(child->m_flags >> 24 & 1) : 1;
        uint32_t index = child->m_attachIndex;
        bool atRoot = index == 0xffff && childShown != 0;
        if (!((index < attachments->Count() && states[index].m_value != 0) || atRoot)) {
            continue;
        }
        const float* camera = this->m_scene->m_camera.m;
        if ((child->m_flags >> 14 & 1) == 0) {
            if (atRoot) {
                std::memcpy(m, this->m_world.m, sizeof(m));
            } else {
                uint16_t bone = (*attachments)[index].m_bone;
                if (context->m_f8 != 0 && (this->m_boneDone[bone >> 3] & static_cast<uint8_t>(1 << (bone & 7))) == 0) {
                    this->AnimateBoneTree(bone, context);
                }
                this->GetAttachmentMatrix(child->m_attachIndex, reinterpret_cast<C44Matrix*>(m));
            }
            if ((child->m_flags >> 16 & 1) == 0) {
                if ((child->m_flags >> 15 & 1) != 0) {
                    float scale = std::sqrt(m[1] * m[1] + m[0] * m[0] + m[2] * m[2]);
                    m[2] = scale * camera[2];
                    m[1] = scale * camera[1];
                    m[0] = scale * camera[0];
                    m[4] = scale * camera[4];
                    m[5] = scale * camera[5];
                    m[6] = scale * camera[6];
                    m[8] = scale * camera[8];
                    m[9] = scale * camera[9];
                    m[10] = scale * camera[10];
                }
            } else {
                m[0] = camera[0];
                m[1] = camera[1];
                m[2] = camera[2];
                m[4] = camera[4];
                m[5] = camera[5];
                m[6] = camera[6];
                m[8] = camera[8];
                m[9] = camera[9];
                m[10] = camera[10];
            }
        } else {
            std::memcpy(m, camera, sizeof(m));
        }
        if (child->m_extraMatrix != nullptr) {
            if ((this->m_flags & BASE_INVERSE_CURRENT) == 0) {
                float determinant = MatrixDeterminant(this->m_world.m);
                if (std::fabs(determinant) < 1.0e-06f) {  // DAT_14261a95c
                    std::memset(this->m_inverse.m, 0, sizeof(C44Matrix));
                    this->m_inverse.m[0] = 1.0f;
                    this->m_inverse.m[5] = 1.0f;
                    this->m_inverse.m[10] = 1.0f;
                    this->m_inverse.m[15] = 1.0f;
                } else {
                    float inverse[16];
                    MatrixInverse(this->m_world.m, inverse, determinant);
                    std::memcpy(this->m_inverse.m, inverse, sizeof(inverse));
                }
                this->m_flags |= BASE_INVERSE_CURRENT;
            }
            const float* inv = this->m_inverse.m;
            // P = m * inverse (its last row only partly: the translation comes in below).
            float p[16];
            p[0] = m[2] * inv[8] + m[3] * inv[12] + m[0] * inv[0] + m[1] * inv[4];
            p[1] = m[2] * inv[9] + m[3] * inv[13] + m[0] * inv[1] + m[1] * inv[5];
            p[2] = m[2] * inv[10] + m[3] * inv[14] + m[0] * inv[2] + m[1] * inv[6];
            p[3] = m[2] * inv[11] + m[3] * inv[15] + m[0] * inv[3] + m[1] * inv[7];
            for (int32_t col = 0; col < 4; col++) {
                p[4 + col] = m[4] * inv[col] + m[5] * inv[4 + col] + m[6] * inv[8 + col] + m[7] * inv[12 + col];
                p[8 + col] = m[10] * inv[8 + col] + m[11] * inv[12 + col] + m[8] * inv[col] + m[9] * inv[4 + col];
            }
            float p15 = m[12] * inv[3] + m[13] * inv[7] + m[14] * inv[11] + m[15] * inv[15];
            // Q = extra * P with P's last row (0, 0, 0, p15).
            const float* e = child->m_extraMatrix->m;
            float q[16];
            for (int32_t row = 0; row < 4; row++) {
                const float* er = e + row * 4;
                for (int32_t col = 0; col < 3; col++) {
                    q[row * 4 + col] = er[2] * p[8 + col] + er[3] * 0.0f + er[1] * p[4 + col] + er[0] * p[col];
                }
                q[row * 4 + 3] = er[2] * p[11] + er[3] * p15 + er[1] * p[7] + er[0] * p[3];
            }
            float t0 = m[12] * inv[0] + m[13] * inv[4] + m[14] * inv[8] + m[15] * inv[12] + q[12];
            float t1 = m[12] * inv[1] + m[13] * inv[5] + m[14] * inv[9] + m[15] * inv[13] + q[13];
            float t2 = m[12] * inv[2] + m[13] * inv[6] + m[14] * inv[10] + m[15] * inv[14] + q[14];
            // m = Q * world.
            const float* w = this->m_world.m;
            for (int32_t row = 0; row < 3; row++) {
                const float* qr = q + row * 4;
                for (int32_t col = 0; col < 4; col++) {
                    m[row * 4 + col] = qr[0] * w[col] + qr[1] * w[4 + col] + qr[2] * w[8 + col] + qr[3] * w[12 + col];
                }
            }
            for (int32_t col = 0; col < 4; col++) {
                m[12 + col] = q[15] * w[12 + col] + t2 * w[8 + col] + t0 * w[col] + t1 * w[4 + col];
            }
        }
        child->UpdateMatrices(reinterpret_cast<const C44Matrix*>(m), &this->m_params, context->m_frame,
            static_cast<uint32_t>(context->m_time), context->m_fc);
    }
}

// The animated bounds merged into the accumulated ones, then taken through the inverse render
// matrix with their centre and radius (FUN_1411b1fe0's tail).
void CM2Model::UpdateAccumulatedBounds() {
    float render[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    this->GetRenderMatrix(reinterpret_cast<C44Matrix*>(render));
    float inverse[16];
    MatrixInverse(render, inverse, MatrixDeterminant(render));
    M2AccumBounds* accum = this->m_accumBounds;
    float animated[6];
    float* box = this->GetAnimatedBox(animated, true);
    float merged[6];
    // FUN_1411cda20
    float* a = &accum->m_box.m_min.x;
    float maxX = box[3] < a[3] ? a[3] : box[3];
    float maxY = box[4] < a[4] ? a[4] : box[4];
    float maxZ = box[5] < a[5] ? a[5] : box[5];
    float minX = box[0] < a[0] ? box[0] : a[0];
    float minY = box[1] < a[1] ? box[1] : a[1];
    float minZ = box[2] < a[2] ? box[2] : a[2];
    a[0] = merged[0] = minX;
    a[1] = merged[1] = minY;
    a[2] = merged[2] = minZ;
    a[3] = merged[3] = maxX;
    a[4] = merged[4] = maxY;
    a[5] = merged[5] = maxZ;
    float local[6];
    BoxTransform(local, a, inverse);
    std::memcpy(&accum->m_localBox, local, sizeof(local));
    const CAaBox& lb = accum->m_localBox;
    float dx = lb.m_max.x - lb.m_min.x;
    float dy = lb.m_max.y - lb.m_min.y;
    float dz = lb.m_max.z - lb.m_min.z;
    accum->m_center.x = (lb.m_min.x + lb.m_max.x) * 0.5f;
    accum->m_center.y = (lb.m_min.y + lb.m_max.y) * 0.5f;
    accum->m_center.z = (lb.m_min.z + lb.m_max.z) * 0.5f;
    accum->m_radius = std::sqrt(dy * dy + dx * dx + dz * dz) * 0.5f;
}

// Each bone's offset from the ground below its pivot, found through the scene's trace
// callback, for the physics bones (FUN_1411b73f0).
// ref: FUN_1411b73f0
void CM2Model::InitializePhysicsBones() {
    static OSMUTEX s_lock;  // DAT_1430a1028
    static bool s_lockReady = OsMutexInit(&s_lock);
    (void)s_lockReady;
    OsMutexLock(&s_lock);
    const float* v = this->m_view.m;
    auto offsets = static_cast<float*>(this->m_physicsBones);
    M2Array<M2CompBone>* bones = this->m_animState->m_bones;
    C44Matrix* matrices = this->m_animState->m_boneMatrices;
    // The segment traced: from above the pivot down past it.
    struct {
        uint64_t m_00;
        uint64_t m_08;  // the scene's +0x28
        float m_position[3];
        float m_end[3];
        uint32_t m_28;
    } query = {};
    query.m_08 = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(this->m_scene) + 0x28);
    float view[16];
    std::memcpy(view, v, sizeof(view));
    float inverse[16];
    MatrixInverse(view, inverse, MatrixDeterminant(view));
    for (uint32_t i = 0; i < bones->Count(); i++) {
        const M2CompBone& bone = (*bones)[i];
        if ((bone.m_flags & 0x80000) != 0) {
            this->AnimateBoneFor(i);
        }
        const float* b = matrices[i].m;
        float px = bone.m_pivot.x;
        float py = bone.m_pivot.y;
        float pz = bone.m_pivot.z;
        float wx = py * b[4] + px * b[0] + pz * b[8] + b[12];
        float wy = py * b[5] + px * b[1] + pz * b[9] + b[13];
        float wz = py * b[6] + px * b[2] + pz * b[10] + b[14];
        query.m_position[0] = wx * v[0] + wy * v[4] + wz * v[8] + v[12];
        query.m_position[1] = wx * v[1] + wy * v[5] + wz * v[9] + v[13];
        query.m_position[2] = wx * v[2] + wy * v[6] + wz * v[10] + v[14] + 3.5f;  // DAT_142721208
        query.m_end[0] = query.m_position[0];
        query.m_end[1] = query.m_position[1];
        query.m_end[2] = query.m_position[2] - 15.0f;  // DAT_1426d2b04
        float hit[3] = {0.0f, 0.0f, 0.0f};
        float t = 1.0f;
        float hx = 0.0f;
        float hy = 0.0f;
        float hz = 0.0f;
        if (this->m_scene->m_traceCallback != nullptr) {
            this->m_scene->m_traceCallback(&query, hit, &t);
            hy = hit[1];
            hx = hit[0];
            hz = hit[2];
        }
        float lx = hy * inverse[4] + hx * inverse[0] + hz * inverse[8] + inverse[12];
        float ly = hy * inverse[5] + hx * inverse[1] + hz * inverse[9] + inverse[13];
        float lz = hy * inverse[6] + hx * inverse[2] + hz * inverse[10] + inverse[14];
        float ox;
        float oy;
        float oz;
        if (1.0f <= t) {
            oz = 0.0f;
            ox = 0.0f;
            oy = 0.0f;
        } else {
            ox = wx - lx;
            oy = wy - ly;
            oz = wz - lz;
        }
        float reach = std::fabs(oz + oz);
        if (reach <= this->m_580) {
            reach = this->m_580;
        }
        this->m_580 = reach;
        offsets[i * 3] = ox;
        offsets[i * 3 + 1] = oy;
        offsets[i * 3 + 2] = oz;
    }
    this->m_4c0 = 1;
    OsMutexUnlock(&s_lock);
}
