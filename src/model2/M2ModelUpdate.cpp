#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "client/RenderSettings.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"

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
        float scale = this->m_modelBounds.m_10 * this->m_modelBounds.m_18;
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
                        const float* v = this->m_scene->m_viewProjection.m;
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
                    const float* v = this->m_scene->m_viewProjection.m;
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
    for (CM2Model* child = static_cast<CM2Model*>(this->m_658); child != nullptr;
         child = static_cast<CM2Model*>(child->m_668)) {
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

// ref: FUN_1411b1fe0
void CM2Model::Animate(void* a2, const C44Matrix* parent, uint32_t frame, int32_t time, int32_t throttle) {
    (void)a2;
    if ((this->m_flags & BASE_LOADED) == 0) {
        return;
    }
    if (this->m_matrixFrame == frame) {
        this->UpdateMatricesFrom(parent);
    } else {
        this->ResetBoneState();
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
