#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "base/ObjectAlloc.hpp"
#include "model2/M2Camera.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "ui/FrameTime.hpp"

// Engine/Source/Model2/M2Model.cpp: bone animation.

namespace {

// DAT_14261aa10
const C4Quaternion QUAT_IDENTITY = {0.0f, 0.0f, 0.0f, 1.0f};

uint32_t FloatBits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A slot's clock at `now`, before wrapping: its offset plus the time since it started, scaled
// by its speed.
uint32_t SlotClock(const M2SequenceTime* slot, int32_t now) {
    uint32_t offset = slot->m_timeOffset;
    if (FloatBits(slot->m_speed) == 0x3f800000) {
        return (offset - static_cast<uint32_t>(slot->m_startTime)) + static_cast<uint32_t>(now);
    }
    return static_cast<uint32_t>(
               static_cast<int32_t>(static_cast<float>(now - slot->m_startTime) * slot->m_speed))
        + offset;
}

uint32_t WrapToDuration(uint32_t value, uint32_t duration) {
    if (duration == 0) {
        return 0;
    }
    if (static_cast<int32_t>(value) < 0) {
        return duration + static_cast<int32_t>(value) % static_cast<int32_t>(duration);
    }
    return value % duration;
}

// The time a slot's next sequence is at, at `now` (the reference inlines this three times):
// a sequence without `holdFlag` loops; one with it holds at its end, or loops when the slot
// repeats.
uint32_t SlotTime(const M2SequenceTime* slot, int32_t now, uint32_t holdFlag) {
    const M2Sequence& sequence = (*slot->m_source.m_sequences)[slot->m_nextSequence];
    uint32_t offset = slot->m_timeOffset;
    if ((sequence.m_flags & holdFlag) == 0) {
        uint32_t clock = SlotClock(slot, now);
        if (offset != 0) {
            while (static_cast<int32_t>(clock) < 0) {
                clock += offset;
            }
        }
        uint32_t value = static_cast<int32_t>(clock) >= 0 ? clock : 0;
        return WrapToDuration(value, sequence.m_duration);
    }
    int32_t end = slot->m_endTime;
    if (end - now < 1) {
        uint32_t clock = SlotClock(slot, end);
        if (static_cast<int32_t>(clock) < 0) {
            if (offset != 0) {
                do {
                    clock += offset;
                } while (static_cast<int32_t>(clock) < 0);
                return sequence.m_duration < clock ? sequence.m_duration : clock;
            }
            clock = 0;
        }
        return sequence.m_duration < clock ? sequence.m_duration : clock;
    }
    int32_t at = now;
    if (0 < slot->m_startTime - now) {
        at = slot->m_startTime;
    }
    uint32_t clock = SlotClock(slot, at);
    if (offset != 0) {
        while (static_cast<int32_t>(clock) < 0) {
            clock += offset;
        }
    }
    uint32_t value = static_cast<int32_t>(clock) >= 0 ? clock : 0;
    if (1 < slot->m_repeats) {
        return WrapToDuration(value, sequence.m_duration);
    }
    return value;
}

void CopyMatrix(float* out, const float* in) {
    std::memcpy(out, in, sizeof(float) * 16);
}

void SetIdentity(float* m) {
    MatrixIdentity(m);
}

// out[r] = a[r] * b (row vectors, all four columns).
void Multiply(float* out, const float* a, const float* b) {
    MatrixMultiply(out, a, b);
}

}  // namespace

// The pause both slots of a bone take while the model is paused (m_3b8 ms this frame).
void CM2Model::ShiftSlotForPause(M2SequenceTime* slot, const M2AnimateContext* context) {
    if (this->m_pauseRequest != 0 && context->m_104 != 0) {
        slot->m_startTime += static_cast<int32_t>(this->m_3b8);
        slot->m_endTime += static_cast<int32_t>(this->m_3b8);
    }
}

// ref: FUN_1411ae6a0
void CM2Model::AnimateBone(const M2CompBone* bone, M2BoneState* state, uint32_t index, M2AnimateContext* context) {
    this->m_boneDone[index >> 3] |= static_cast<uint8_t>(1 << (index & 7));
    M2AnimState* anim = this->m_animState;
    M2Array<M2CompBone>* bones = anim->m_bones;
    C44Matrix* matrices = anim->m_boneMatrices;
    const uint32_t* globalTimes = anim->m_globalTimes;
    M2BoneState* boneStates = anim->m_boneStates;

    M2SequenceTime* slot = state->m_sequence;
    const M2CompBone* animBone = nullptr;
    if (slot != nullptr) {
        animBone = &(*slot->m_source.m_bones)[index];
    }
    const M2Track<M2CompQuat>* blendRotation = nullptr;
    const M2Track<C3Vector>* blendTranslation = nullptr;
    const M2Track<C3Vector>* blendScale = nullptr;
    if (state->m_blendSequence != nullptr) {
        const M2CompBone* blendBone = &(*state->m_blendSequence->m_source.m_bones)[index];
        blendRotation = &blendBone->m_rotation;
        blendTranslation = &blendBone->m_translation;
        blendScale = &blendBone->m_scale;
    }
    uint32_t flags = state->m_flags | bone->m_flags;
    float* local = context->m_local.m;

    if (this->m_skeleton != nullptr) {
        M2SkeletonAnimateBone(this->m_skeleton, bone, state, index, context, this->m_animState);
    } else {
        if ((state->m_flags >> 13 & 1) != 0 && slot->m_nextSequence != 0xffff) {
            this->ShiftSlotForPause(slot, context);
            M2SequenceTime* primary = state->m_sequence;
            primary->m_time = SlotTime(primary, context->m_time, 1);
            primary->m_sequence = primary->m_nextSequence;
            primary->m_bone = static_cast<uint16_t>(index);
        }
        M2SequenceTime* blend = state->m_blendSequence;
        if (blend != nullptr) {
            if ((state->m_flags & 0x4000) == 0 || blend->m_nextSequence == 0xffff) {
                if (index == 0) {
                    blend->m_time = state->m_sequence->m_time;
                    blend->m_sequence = state->m_sequence->m_sequence;
                }
            } else {
                this->ShiftSlotForPause(blend, context);
                blend->m_time = SlotTime(blend, context->m_time, 0x80);
                blend->m_sequence = blend->m_nextSequence;
                if (-1 < context->m_time - state->m_blendEnd) {
                    // The blend is over.
                    this->DropBlend(state);
                    M2BoneStatesPropagateSlots(boneStates, anim->m_bones);
                    animBone = state->m_sequence == nullptr
                        ? nullptr
                        : &(*state->m_sequence->m_source.m_bones)[index];
                    blendRotation = nullptr;
                    blendTranslation = nullptr;
                    blendScale = nullptr;
                    if (state->m_blendSequence != nullptr) {
                        const M2CompBone* blendBone = &(*state->m_blendSequence->m_source.m_bones)[index];
                        blendRotation = &blendBone->m_rotation;
                        blendTranslation = &blendBone->m_translation;
                        blendScale = &blendBone->m_scale;
                    }
                }
            }
        }

        // The blend weight: the parent's, or this bone's own blend easing in.
        M2SequenceTime* primary = state->m_sequence;
        float weight;
        bool ownBlend = false;
        if (primary->m_nextSequence == 0xffff || (state->m_flags & 0x2000) == 0) {
            if (state->m_blendSequence != nullptr && state->m_blendSequence->m_nextSequence != 0xffff
                && (state->m_flags & 0x4000) != 0) {
                ownBlend = true;
            } else if (bones->Count() <= static_cast<uint16_t>(bone->m_parentBone)) {
                weight = index != 0 ? boneStates[0].m_blendWeight : 0.0f;
            } else {
                weight = boneStates[static_cast<uint16_t>(bone->m_parentBone)].m_blendWeight;
            }
        } else {
            ownBlend = true;
        }
        if (ownBlend) {
            int32_t remaining = state->m_blendEnd - context->m_time;
            M2SequenceTime* from = state->m_blendSequence;
            if (remaining < 1
                || (from != nullptr && primary != nullptr && primary->m_time == from->m_time
                    && primary->m_sequence == from->m_sequence)) {
                weight = 0.0f;
            } else {
                weight = static_cast<float>(remaining) * state->m_blendRate;
                if (0.0f <= weight) {
                    if (weight <= 1.0f) {
                        weight = (3.0f - (weight + weight)) * weight * weight * state->m_blendMax;
                    } else {
                        weight = 1.0f * state->m_blendMax;
                    }
                } else {
                    weight = state->m_blendMax * 0.0f;
                }
            }
        }
        state->m_blendWeight = weight;

        // The parent's matrix, adjusted for the billboard kinds that keep part of it.
        uint16_t parent = static_cast<uint16_t>(bone->m_parentBone);
        const float* parentMatrix;
        if (parent == 0xffff) {
            parentMatrix = context->m_root.m;
            state->m_inheritedWeight = state->m_weight;
        } else {
            parentMatrix = matrices[parent].m;
            if ((flags & 0x802000) == 0) {
                state->m_inheritedWeight = boneStates[parent].m_inheritedWeight * state->m_weight;
            } else {
                state->m_inheritedWeight = state->m_weight;
            }
            if ((flags & 7) != 0) {
                float* adjusted = context->m_parent.m;
                CopyMatrix(adjusted, parentMatrix);
                parentMatrix = adjusted;
                float px = bone->m_pivot.x;
                float py = bone->m_pivot.y;
                float pz = bone->m_pivot.z;
                context->m_worldPivot.x = px * adjusted[0] + py * adjusted[4] + pz * adjusted[8] + adjusted[12];
                context->m_worldPivot.y = px * adjusted[1] + py * adjusted[5] + pz * adjusted[9] + adjusted[13];
                context->m_worldPivot.z = px * adjusted[2] + py * adjusted[6] + pz * adjusted[10] + adjusted[14];
                uint32_t keep = flags & 6;
                if (keep == 2) {
                    VectorNormalize(&adjusted[0]);
                    VectorNormalize(&adjusted[4]);
                    VectorNormalize(&adjusted[8]);
                } else if (keep == 4 || keep == 6) {
                    const float* root = context->m_root.m;
                    adjusted[0] = root[0];
                    adjusted[1] = root[1];
                    adjusted[2] = root[2];
                    adjusted[4] = root[4];
                    adjusted[5] = root[5];
                    adjusted[6] = root[6];
                    adjusted[8] = root[8];
                    adjusted[9] = root[9];
                    adjusted[10] = root[10];
                }
                if ((flags & 1) == 0) {
                    adjusted[12] = context->m_worldPivot.x - (px * adjusted[0] + py * adjusted[4] + pz * adjusted[8]);
                    adjusted[13] = context->m_worldPivot.y - (px * adjusted[1] + py * adjusted[5] + pz * adjusted[9]);
                    adjusted[14] = context->m_worldPivot.z - (px * adjusted[2] + py * adjusted[6] + pz * adjusted[10]);
                } else {
                    adjusted[12] = context->m_root.m[12];
                    adjusted[13] = context->m_root.m[13];
                    adjusted[14] = context->m_root.m[14];
                }
            }
        }

        float influence = state->m_inheritedWeight;
        float* out = matrices[index].m;
        if ((flags & 0x280) == 0 || ((flags >> 10 & 1) != 0 && (flags & 0x400400) != 0x400)
            || influence <= 0.0f || (this->m_shared->m_flags & CM2Shared::SHARED_TINTED) != 0) {
            // Not animated: the parent's matrix, or the one set from outside.
            if ((flags & 0x400400) == 0x400400 && state->m_override != nullptr) {
                CopyMatrix(local, state->m_override->m);
                float move[3] = {-bone->m_pivot.x, -bone->m_pivot.y, -bone->m_pivot.z};
                MatrixTranslate(local, move);
                CopyMatrix(out, local);
            } else {
                CopyMatrix(out, parentMatrix);
            }
        } else {
            // The rotation.
            uint16_t rotationKeys = animBone->m_rotation.m_values.m_count;
            if (rotationKeys == 0) {
                SetIdentity(local);
            } else {
                bool keep = (this->m_modelFlags2 & 0x100) != 0 && animBone->m_rotation.m_globalSequence != -1;
                C4Quaternion saved = keep ? state->m_rotation.m_value : QUAT_IDENTITY;
                if (1 < rotationKeys || (rotationKeys == 1 && 1 < animBone->m_rotation.m_values[0].m_count)) {
                    C4Quaternion def = QUAT_IDENTITY;
                    M2TrackSample(globalTimes, reinterpret_cast<const M2AnimTarget*>(state), &animBone->m_rotation,
                        blendRotation, &state->m_rotation, &def, 0);
                }
                if (keep) {
                    state->m_rotation.m_value = saved;
                }
                if (influence < 1.0f) {
                    C4Quaternion identity = QUAT_IDENTITY;
                    C4Quaternion rotation = state->m_rotation.m_value;
                    C4Quaternion result;
                    state->m_rotation.m_value = *QuaternionSlerp(&result, influence, &identity, &rotation);
                }
                MatrixFromQuaternionRotation(local, &state->m_rotation.m_value.x);
            }
            // The scale.
            uint16_t scaleKeys = animBone->m_scale.m_values.m_count;
            if (scaleKeys != 0) {
                bool keep = (this->m_modelFlags2 & 0x100) != 0 && bone->m_scale.m_globalSequence != -1;
                C3Vector saved = keep ? state->m_scale.m_value : C3Vector{0.0f, 0.0f, 0.0f};
                if (1 < scaleKeys || (scaleKeys == 1 && 1 < animBone->m_scale.m_values[0].m_count)) {
                    C3Vector def = {1.0f, 1.0f, 1.0f};
                    M2TrackSample(globalTimes, reinterpret_cast<const M2AnimTarget*>(state), &animBone->m_scale,
                        blendScale, &state->m_scale, &def, 0);
                }
                if (keep) {
                    state->m_scale.m_value = saved;
                }
                C3Vector& scale = state->m_scale.m_value;
                if (influence < 1.0f) {
                    scale.x = (scale.x - 1.0f) * influence + 1.0f;
                    scale.y = (scale.y - 1.0f) * influence + 1.0f;
                    scale.z = (scale.z - 1.0f) * influence + 1.0f;
                }
                if (scale.x * scale.x + scale.y * scale.y + scale.z * scale.z <= 0.0f) {
                    scale = C3Vector{0.0001f, 0.0001f, 0.0001f};
                }
                MatrixScale(local, &scale.x);
            }
            if ((flags & 0x1000080) == 0x80 && state->m_override != nullptr) {
                float r[16];
                Multiply(r, local, state->m_override->m);
                CopyMatrix(local, r);
            }
            // The translation.
            context->m_pivot = bone->m_pivot;
            uint16_t translationKeys = animBone->m_translation.m_values.m_count;
            if (translationKeys != 0) {
                bool keep = (this->m_modelFlags2 & 0x100) != 0 && animBone->m_translation.m_globalSequence != -1;
                C3Vector saved = keep ? state->m_translation.m_value : C3Vector{0.0f, 0.0f, 0.0f};
                if (1 < translationKeys || (translationKeys == 1 && 1 < animBone->m_translation.m_values[0].m_count)) {
                    C3Vector def = {0.0f, 0.0f, 0.0f};
                    M2TrackSample(globalTimes, reinterpret_cast<const M2AnimTarget*>(state), &animBone->m_translation,
                        blendTranslation, &state->m_translation, &def, 0);
                }
                if (keep) {
                    state->m_translation.m_value = saved;
                }
                C3Vector& translation = state->m_translation.m_value;
                if (influence < 1.0f) {
                    translation.x = influence * translation.x;
                    translation.y = translation.y * influence;
                    translation.z = translation.z * influence;
                }
                context->m_pivot.x = context->m_pivot.x + translation.x;
                context->m_pivot.y = translation.y + context->m_pivot.y;
                context->m_pivot.z = translation.z + context->m_pivot.z;
            }
            // A bone file's offset for this bone.
            if (static_cast<int32_t>(this->m_618) != -1) {
                CM2BoneOffset* offsets = M2SharedGetBoneOffset(this->m_shared, static_cast<uint32_t>(this->m_618));
                if (offsets != nullptr && M2BoneOffsetIsLoaded(offsets)) {
                    uint16_t remap = offsets->m_boneIndices[index];
                    if (remap != 0xffff) {
                        const float* offset = reinterpret_cast<const float*>(offsets->m_offsets) + remap * 16;
                        float r[16];
                        Multiply(r, local, offset);
                        CopyMatrix(local, r);
                    }
                }
            }
            local[12] = local[12] + context->m_pivot.x;
            local[13] = context->m_pivot.y + local[13];
            local[14] = context->m_pivot.z + local[14];
            float move[3] = {-bone->m_pivot.x, -bone->m_pivot.y, -bone->m_pivot.z};
            MatrixTranslate(local, move);
            Multiply(out, local, parentMatrix);
            if ((flags & 0x1000080) == 0x1000080 && state->m_override != nullptr) {
                float px = bone->m_pivot.x;
                float py = bone->m_pivot.y;
                float pz = bone->m_pivot.z;
                context->m_worldPivot.x = px * out[0] + py * out[4] + pz * out[8] + out[12];
                context->m_worldPivot.y = px * out[1] + py * out[5] + pz * out[9] + out[13];
                context->m_worldPivot.z = px * out[2] + py * out[6] + pz * out[10] + out[14];
                float r[16];
                Multiply(r, out, state->m_override->m);
                CopyMatrix(out, r);
                float rx = py * out[4] + px * out[0] + pz * out[8];
                float ry = py * out[5] + px * out[1] + pz * out[9];
                float rz = py * out[6] + px * out[2] + pz * out[10];
                context->m_pivot = C3Vector{rx, ry, rz};
                out[12] = context->m_worldPivot.x - rx;
                out[13] = context->m_worldPivot.y - ry;
                out[14] = context->m_worldPivot.z - rz;
            }
        }
    }

    // Swaying with the scene's wind (files flagged 0x8000).
    if ((this->m_shared->m_flags & CM2Shared::SHARED_TINTED) != 0) {
        SetIdentity(local);
        CM2Scene* scene = this->m_scene;
        if (M2SceneHasWind(scene) && index != 0) {
            this->AnimateBoneWind(index, context);
        }
    }

    float* out = matrices[index].m;
    if ((flags >> 21 & 1) != 0 && state->m_override != nullptr) {
        local[12] = state->m_override->m[12] + local[12];
        local[13] = state->m_override->m[13] + local[13];
        local[14] = state->m_override->m[14] + local[14];
        CopyMatrix(out, local);
    }

    // Billboards: the bone's rows replaced in world space, then taken back through the model's
    // inverse world matrix.
    uint32_t billboard = flags & 0x4000078;
    if (billboard != 0) {
        const float* world = this->m_world.m;
        float m[16];
        Multiply(m, out, world);
        float* r0 = &m[0];
        float* r1 = &m[4];
        float* r2 = &m[8];
        context->m_scale.z = std::sqrt(r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]);
        context->m_scale.y = std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
        context->m_scale.x = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
        float px = bone->m_pivot.x;
        float py = bone->m_pivot.y;
        float pz = bone->m_pivot.z;
        float wx = py * r1[0] + px * r0[0] + pz * r2[0] + m[12];
        float wy = py * r1[1] + px * r0[1] + pz * r2[1] + m[13];
        float wz = py * r1[2] + px * r0[2] + pz * r2[2] + m[14];
        context->m_worldPivot = C3Vector{wx, wy, wz};
        float b[16];
        float* b0 = &b[0];
        float* b1 = &b[4];
        float* b2 = &b[8];
        std::memcpy(b, m, sizeof(b));
        auto cross = [](const float* a, const float* c, float* o) {
            o[0] = a[2] * c[1] - a[1] * c[2];
            o[1] = a[0] * c[2] - a[2] * c[0];
            o[2] = a[1] * c[0] - a[0] * c[1];
        };
        if (billboard == 8) {
            if ((flags & 0x280) == 0) {
                b0[0] = 0.0f;
                b0[1] = 0.0f;
                b0[2] = -1.0f;
                b1[0] = (this->m_modelFlags & 0x4000000) == 0 ? 1.0f : -1.0f;
                b1[2] = 0.0f;
                b1[1] = 0.0f;
                b2[0] = 0.0f;
                b2[1] = 1.0f;
                b2[2] = 0.0f;
            } else {
                b0[0] = local[1];
                b0[1] = local[2];
                b0[2] = -local[0];
                VectorNormalize(b0);
                b1[0] = local[5];
                b1[1] = local[6];
                b1[2] = local[4];
                if ((this->m_modelFlags & 0x4000000) == 0) {
                    b1[2] = -b1[2];
                } else {
                    b1[0] = -b1[0];
                    b1[1] = -b1[1];
                }
                VectorNormalize(b1);
                b2[0] = local[9];
                b2[1] = local[10];
                b2[2] = -local[8];
                VectorNormalize(b2);
            }
        } else if (billboard == 0x10) {
            VectorNormalize(b0);
            b1[1] = -b0[0];
            b1[0] = b0[1];
            b1[2] = 0.0f;
            VectorNormalize(b1);
            cross(b0, b1, b2);
        } else if (billboard == 0x20) {
            VectorNormalize(b1);
            b0[0] = -b1[1];
            b0[2] = 0.0f;
            b0[1] = b1[0];
            VectorNormalize(b0);
            cross(b0, b1, b2);
        } else if (billboard == 0x40) {
            VectorNormalize(b2);
            b1[1] = -b2[0];
            b1[0] = b2[1];
            b1[2] = 0.0f;
            VectorNormalize(b1);
            b0[0] = b1[2] * b2[1] - b1[1] * b2[2];
            b0[1] = b1[0] * b2[2] - b1[2] * b2[0];
            b0[2] = b1[1] * b2[0] - b1[0] * b2[1];
        } else if (billboard == 0x4000000) {
            b0[0] = -wx;
            b0[1] = -wy;
            b0[2] = -wz;
            VectorNormalize(b0);
            b1[0] = -b0[2];
            b1[1] = 0.0f;
            b1[2] = 0.0f;
            VectorNormalize(b1);
            b2[0] = b0[2] * b1[1] - b0[1] * b1[2];
            b2[1] = b0[0] * b1[2] - b0[2] * b1[0];
            b2[2] = 0.0f;
            VectorNormalize(b2);
        }
        MatrixScale(b, &context->m_scale.x);
        b[3] = 0.0f;
        b[7] = 0.0f;
        b[11] = 0.0f;
        b[15] = 1.0f;
        b[12] = wx - (px * b0[0] + py * b1[0] + pz * b2[0]);
        b[13] = wy - (px * b0[1] + py * b1[1] + pz * b2[1]);
        b[14] = wz - (px * b0[2] + py * b1[2] + pz * b2[2]);
        if ((this->m_flags & BASE_INVERSE_CURRENT) == 0) {
            float determinant = MatrixDeterminant(this->m_world.m);
            if (std::fabs(determinant) < 1.0e-06f) {
                MatrixIdentity(this->m_inverse.m);
            } else {
                MatrixInverse(this->m_world.m, this->m_inverse.m, determinant);
            }
            this->m_flags |= BASE_INVERSE_CURRENT;
        }
        Multiply(out, b, this->m_inverse.m);
    }

    // A physics bone's rest offset.
    if ((flags >> 19 & 1) != 0 && this->m_4c0 != 0) {
        const float* rest = static_cast<const float*>(this->m_physicsBones) + index * 3;
        out[12] = out[12] - rest[0];
        out[13] = out[13] - rest[1];
        out[14] = out[14] - rest[2];
    }
}

// The scene's wind on a tinted model's bone (the tail of FUN_1411ae6a0).
void CM2Model::AnimateBoneWind(uint32_t index, M2AnimateContext* context) {
    float* local = context->m_local.m;
    CM2Scene* scene = this->m_scene;
    // The distance from the scene's wind origin (+0xd538) to the model's position.
    const float* camera = M2SceneGetWindOrigin(scene);
    float lx = camera[0] - this->m_local.m[12];
    float ly = camera[1] - this->m_local.m[13];
    float lz = camera[2] - this->m_local.m[14];
    float strength = 1.75f - std::sqrt(ly * ly + lx * lx + lz * lz) * 0.005882353f;  // DAT_142721200, DAT_1427211d8
    float phaseX = 0.0f;
    float phaseY = 0.0f;
    float phaseZ = 0.0f;
    if (0.0f <= strength) {
        if (1.0f <= strength) {
            strength = 1.0f;
        }
        if (0.0f < strength && this->m_windPhase != nullptr) {
            float direction[3] = {0.0f, 0.0f, 0.0f};
            float speed[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float gust;
            float time = static_cast<float>(FrameTime_GetElapsed());
            M2SceneSampleWind(scene, direction, speed, &gust);
            float* phase = static_cast<float*>(this->m_windPhase);
            float step = (gust * time) / this->m_shared->m_tint[3];
            phase[0] = std::fmod(step + phase[0], 31.415928f);   // DAT_142721220
            phase[2] = std::fmod(step + phase[2], 18.849556f);   // DAT_14272121c
            phase[1] = std::fmod(step + phase[1], 50.265484f);   // DAT_142721224
            phaseX = phase[0];
            phaseY = phase[1];
            phaseZ = phase[2];
        }
    } else {
        strength = 0.0f;
    }
    float scale = std::sqrt(this->m_world.m[0] * this->m_world.m[0] + this->m_world.m[1] * this->m_world.m[1]
        + this->m_world.m[2] * this->m_world.m[2]);
    if (1.25f <= scale) {  // DAT_1426c8874
        scale = 1.25f;
    }
    local[4] = scale;
    local[5] = strength * 0.325f;  // DAT_1427211e4
    local[6] = strength * 0.15f;   // DAT_1426c71a4
    local[8] = this->m_shared->m_tint[1];
    local[9] = this->m_shared->m_tint[0];
    local[10] = this->m_shared->m_tint[2];
    local[12] = phaseX;
    local[13] = phaseY;
    local[14] = phaseZ;
    CopyMatrix(this->m_animState->m_boneMatrices[index].m, local);
}

// The bone's parent first, then the bone (FUN_1411b0a30).
// ref: FUN_1411b0a30
void CM2Model::AnimateBoneTree(uint32_t index, M2AnimateContext* context) {
    M2Array<M2CompBone>* bones = this->m_animState->m_bones;
    M2BoneState* states = this->m_animState->m_boneStates;
    const M2CompBone* bone = &(*bones)[index];
    uint16_t parent = static_cast<uint16_t>(bone->m_parentBone);
    if (parent != 0xffff && (this->m_boneDone[parent >> 3] & static_cast<uint8_t>(1 << (parent & 7))) == 0) {
        this->AnimateBoneTree(parent, context);
    }
    this->AnimateBone(bone, &states[index], index, context);
}

// The model is not animated bone by bone: the root slot advances on the scene's clock and the
// root matrix is the identity (FUN_1411b0af0).
// ref: FUN_1411b0af0
void CM2Model::AnimateRootOnly(M2BoneState* state) {
    this->m_boneDone[0] |= 1;
    M2SequenceTime* slot = state->m_sequence;
    if (slot->m_nextSequence != 0xffff) {
        if (this->m_pauseRequest != 0) {
            slot->m_startTime += static_cast<int32_t>(this->m_3b8);
            slot->m_endTime += static_cast<int32_t>(this->m_3b8);
        }
        slot = state->m_sequence;
        int32_t now = static_cast<int32_t>(this->m_scene->m_time);
        // The sequences of the model's own source, not the slot's.
        M2SequenceTime probe = *slot;
        probe.m_source.m_sequences = this->m_animState->m_sequences;
        slot->m_time = SlotTime(&probe, now, 1);
        state->m_sequence->m_sequence = state->m_sequence->m_nextSequence;
        state->m_sequence->m_bone = 0;
    }
    MatrixIdentity(this->m_animState->m_boneMatrices[0].m);
}

// ref: FUN_1411b6cf0
void CM2Model::DropBlend(M2BoneState* state) {
    M2SequenceTime* blend = state->m_blendSequence;
    if (blend == nullptr) {
        return;
    }
    if ((state->m_flags & 0x4000) != 0) {
        void* skeleton = this->m_skeleton;
        if (skeleton == nullptr) {
            M2SharedReleaseSequence(this->m_shared, blend->m_nextSequence);
        } else {
            const M2Sequence& sequence = (*blend->m_source.m_sequences)[blend->m_nextSequence];
            void* source = M2SkeletonGetSource(skeleton, (sequence.m_flags >> 12 & 1) != 0);
            M2SkeletonReleaseSequence(source, blend->m_nextSequence);
        }
    }
    if (state->m_blendSequence != nullptr) {
        if ((state->m_flags & 0x4000) != 0) {
            std::memset(&state->m_blendSequence->m_source, 0, 0x34);
            M2GetBoneSeqPool()->Free(state->m_blendSequence);
            state->m_flags &= ~0x4000u;
        }
        state->m_blendSequence = nullptr;
    }
}

// A camera's position and target from its state, through the model's matrix (FUN_1411b0d10).
// ref: FUN_1411b0d10
void CM2Model::UpdateCamera(uint32_t index) {
    M2CameraState* state = &static_cast<M2CameraState*>(this->m_cameras)[index];
    M2Camera* camera = &this->m_shared->m_data->m_cameras[index];
    float x = camera->m_positionBase.x + state->m_position.m_value.x;
    float y = camera->m_positionBase.y + state->m_position.m_value.y;
    float z = camera->m_positionBase.z + state->m_position.m_value.z;
    float tx = (camera->m_targetPositionBase.x + state->m_target.m_value.x) - x;
    float ty = (camera->m_targetPositionBase.y + state->m_target.m_value.y) - y;
    float tz = (camera->m_targetPositionBase.z + state->m_target.m_value.z) - z;
    const float* m;
    if ((this->m_parent == nullptr || (this->m_flags >> 14 & 1) != 0) && (this->m_flags & BASE_VISIBLE) != 0) {
        m = this->m_local.m;
    } else {
        m = this->m_view.m;
    }
    float position[4] = {
        y * m[4] + x * m[0] + z * m[8] + m[12],
        y * m[5] + x * m[1] + z * m[9] + m[13],
        y * m[6] + x * m[2] + z * m[10] + m[14],
        y * m[7] + x * m[3] + z * m[11] + m[15],
    };
    float target[4] = {
        ty * m[4] + tx * m[0] + tz * m[8],
        ty * m[5] + tx * m[1] + tz * m[9],
        ty * m[6] + tx * m[2] + tz * m[10],
        ty * m[7] + tx * m[3] + tz * m[11],
    };
    M2CameraSetVector(state->m_camera, 7, position);
    M2CameraSetVector(state->m_camera, 9, target);
    M2CameraSetFloat(state->m_camera, 5, state->m_roll.m_value);
    M2CameraSetFloat(state->m_camera, 4, state->m_fieldOfView.m_value);
}

// The cameras' tracks at the root bone's sequences (FUN_1411b0f00).
// ref: FUN_1411b0f00
void CM2Model::AnimateCameras() {
    M2Data* data = this->m_shared->m_data;
    M2BoneState* root = this->m_animState->m_boneStates;
    const uint32_t* globalTimes = this->m_animState->m_globalTimes;
    uint32_t flags = 0;
    if (root->m_sequence != nullptr) {
        flags = (root->m_sequence->m_source.m_flags & 1) * 2;
    }
    if (root->m_blendSequence != nullptr) {
        flags |= (root->m_blendSequence->m_source.m_flags & 1) << 2;
    }
    auto target = reinterpret_cast<const M2AnimTarget*>(root);
    for (uint32_t i = 0; i < data->m_cameras.Count(); i++) {
        M2Camera* camera = &data->m_cameras[i];
        M2CameraState* state = &static_cast<M2CameraState*>(this->m_cameras)[i];
        if (1 < camera->m_positions.m_values.Count()
            || (camera->m_positions.m_values.Count() == 1 && 1 < camera->m_positions.m_values[0].Count())) {
            C3Vector def = {0.0f, 0.0f, 0.0f};
            M2TrackSample(globalTimes, target, &camera->m_positions, &camera->m_positions, &state->m_position, &def, flags);
        }
        if (1 < camera->m_targetPositions.m_values.Count()
            || (camera->m_targetPositions.m_values.Count() == 1 && 1 < camera->m_targetPositions.m_values[0].Count())) {
            C3Vector def = {0.0f, 0.0f, 0.0f};
            M2TrackSample(globalTimes, target, &camera->m_targetPositions, &camera->m_targetPositions, &state->m_target,
                &def, flags);
        }
        if (1 < camera->m_roll.m_values.Count()
            || (camera->m_roll.m_values.Count() == 1 && 1 < camera->m_roll.m_values[0].Count())) {
            float def = 0.0f;
            M2TrackSample(globalTimes, target, &camera->m_roll, &camera->m_roll, &state->m_roll, &def, flags);
        }
        if (1 < camera->m_fieldOfView.m_values.Count()
            || (camera->m_fieldOfView.m_values.Count() == 1 && 1 < camera->m_fieldOfView.m_values[0].Count())) {
            float def = 0.7853982f;
            M2TrackSample(globalTimes, target, &camera->m_fieldOfView, &camera->m_fieldOfView, &state->m_fieldOfView,
                &def, flags);
        }
    }
}

// Every camera someone holds besides the model (FUN_1411b1140).
// ref: FUN_1411b1140
void CM2Model::UpdateHeldCameras() {
    M2Data* data = this->m_shared->m_data;
    for (uint32_t i = 0; i < data->m_cameras.Count(); i++) {
        M2CameraState* state = &static_cast<M2CameraState*>(this->m_cameras)[i];
        if (state->m_camera != nullptr && 1 < M2CameraGetRefCount(state->m_camera)) {
            this->UpdateCamera(i);
        }
    }
}
