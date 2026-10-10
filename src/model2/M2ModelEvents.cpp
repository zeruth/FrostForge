#include <cmath>
#include <cstring>

#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

// Engine/Source/Model2/M2Model.cpp: a model's events. While a frame's callbacks are collected,
// every sequence playing on the model is walked over the time the frame covers; events whose
// keys fall in it become callbacks of kind 1 (the model's event function), and sequences that
// end become callbacks of kind 0 (start what follows). They run once the frame's lists are
// collected (CM2Scene::RunCallbacks).

namespace {

const char s_callbackName[] = ".?AUM2ModelCallback@@";  // DAT_142da1ce0

// What the end-of-sequence function is told (built on the stack by FUN_1411b5780).
struct SEQUENCEENDINFO {
    uint32_t m_keyBone;                 // 0x00
    uint32_t m_sequenceId;              // 0x04
    uint32_t m_variation;               // 0x08
    uint32_t m_24;                      // 0x0c
    uint32_t m_blendTime;               // 0x10
    float m_40[4];                      // 0x14
    uint32_t m_one;                     // 0x24
};

// A bone index of a key bone, 0 for -1 and 0xffff past the lookup (inlined throughout).
uint16_t KeyBoneIndex(const M2AnimState* anim, uint32_t keyBone) {
    if (keyBone == 0xffffffff) {
        return 0;
    }
    const M2Array<uint16_t>* lookup = anim->m_keyBoneLookup;
    if (keyBone < lookup->Count()) {
        return (*lookup)[keyBone];
    }
    return 0xffff;
}

}  // namespace

// Appends a callback, safe against `callback` pointing into the vector (FUN_1411cee10).
// ref: FUN_1411cee10
void M2ModelCallbacksPush(M2SceneVector<M2ModelCallback>* callbacks, const M2ModelCallback* callback) {
    M2ModelCallback* data = callbacks->m_data;
    uint64_t field = callbacks->m_capacity;
    uint64_t capacity = field & 0x7fffffffffffffffull;
    uint64_t at = static_cast<uint64_t>(callback - data);
    uint64_t size = callbacks->m_size;
    if (size == capacity && capacity < capacity + 1) {
        uint64_t grow = field >> 1 & 0x3fffffffffffffffull;
        uint64_t step = capacity + 1 <= capacity + grow ? grow : 1;
        uint64_t grown = step + capacity;
        callbacks->m_capacity = ((grown ^ field) & 0x8000000000000000ull) ^ grown;
        data = static_cast<M2ModelCallback*>(SMemAlloc(grown * sizeof(M2ModelCallback), 0xb, s_callbackName,
            SStrHashFnv1a(s_callbackName), -6, 0));
        size = callbacks->m_size;
        if (size != 0) {
            std::memcpy(data, callbacks->m_data, size * sizeof(M2ModelCallback));
        }
        if (static_cast<int64_t>(callbacks->m_capacity) >= 0) {
            SMemFree(callbacks->m_data, s_callbackName, -6, 0);
            size = callbacks->m_size;
        }
        callbacks->m_capacity &= 0x7fffffffffffffffull;
        callbacks->m_data = data;
    }
    if (at < size) {
        data[size] = data[at];
    } else {
        data[size] = *callback;
    }
    callbacks->m_size++;
}

// ref: FUN_1411b8670
bool CM2Model::QueueEvents(M2SceneDrawList* list, void* collected) {
    auto callbacks = static_cast<M2SceneVector<M2ModelCallback>*>(collected);
    if ((this->m_modelFlags2 & 0x800) != 0 || this->m_pauseRequest != 0 || (this->m_modelFlags & 0x800) == 0) {
        callbacks->m_size = 0;
        return true;
    }
    if ((list->m_flags & 0x10) != 0 && this->m_3c0 == 0) {
        return true;
    }
    uint32_t now = this->m_scene->m_time;
    uint32_t resume = this->m_3c0;
    uint32_t from = now - static_cast<uint32_t>(this->m_scene->m_timeStep);
    if (resume != 0) {
        this->m_3c0 = 0;
        from = resume;
    }
    int32_t ahead = static_cast<int32_t>(from - now);
    M2AnimState* anim = this->m_animState;
    int32_t budget = 100;
    M2Array<M2CompBone>* bones = anim->m_bones;
    M2BoneState* states = anim->m_boneStates;
    const uint16_t* head = anim->m_ownerHead;
    while (ahead < 0) {
        uint16_t bone = *head;
        uint32_t cursor = now;
        while (bone != 0xffff) {
            M2SequenceTime* slot = states[bone].m_sequence;
            if (slot->m_pending == 0) {
                M2Array<M2Sequence>* sequences = slot->m_source.m_sequences;
                float speed = std::fabs(slot->m_inverseSpeed);
                const M2Sequence& sequence = (*sequences)[slot->m_nextSequence];
                uint32_t duration = sequence.m_duration;
                if (0.000099999997f <= std::fabs(speed - 1.0f)) {  // DAT_14261a954
                    duration = static_cast<uint32_t>(std::roundf(static_cast<float>(duration) * speed));
                }
                if (duration != 0) {
                    uint32_t end;
                    uint32_t base;
                    bool ended;
                    if ((sequence.m_flags & 1) == 0) {
                        int32_t start = slot->m_startTime;
                        int32_t at = start;
                        if (static_cast<int32_t>((from - start) + 1) >= 0) {
                            at = static_cast<int32_t>(from + 1);
                        }
                        uint32_t loops = static_cast<uint32_t>(at - start) / duration;
                        base = loops * duration;
                        end = (loops + 1) * duration - 1 + start;
                        ended = !(static_cast<int32_t>(end - from) < 1 || 0 < static_cast<int32_t>(end - cursor));
                    } else {
                        end = static_cast<uint32_t>(slot->m_endTime);
                        if (slot->m_repeats < 2) {
                            base = 0;
                            ended = static_cast<int32_t>(end - cursor) < 1;
                        } else {
                            uint32_t start = static_cast<uint32_t>(slot->m_startTime);
                            uint32_t at = from;
                            if (static_cast<int32_t>(from - start) < 0) {
                                at = start;
                            }
                            uint32_t loops = (at - start) / duration;
                            base = loops * duration;
                            if (loops != 0) {
                                base++;
                            }
                            ended = static_cast<int32_t>(end - cursor) < 1;
                        }
                    }
                    cursor = this->DispatchEvents(&states[bone], sequences, bones, bone, slot->m_nextSequence, base,
                        duration, from, cursor, now, callbacks);
                    if (budget < 1 && cursor <= from && callbacks->m_size != 0) {
                        break;
                    }
                    if (ended) {
                        const M2CompBone& compBone = (*bones)[bone];
                        if (compBone.m_keyBoneId != -1 || bone == 0) {
                            if (end != cursor) {
                                callbacks->m_size = 0;
                                cursor = end;
                            }
                            uint32_t keyBone = compBone.m_parentBone == -1 ? 0xffffffff : static_cast<uint32_t>(compBone.m_keyBoneId);
                            if (callbacks->m_size < 0x400) {
                                M2ModelCallback callback = {};
                                callback.m_kind = 0;
                                callback.m_model = this;
                                callback.m_10 = keyBone;
                                callback.m_function = this->m_408;
                                callback.m_20 = states[bone].m_5c;
                                callback.m_24 = 0;
                                uint32_t index = slot->m_nextSequence;
                                std::memcpy(callback.m_28, &index, 4);
                                std::memcpy(callback.m_28 + 4, &slot->m_startTime, 4);
                                callback.m_38 = now - end;
                                std::memcpy(callback.m_40, &this->m_410, sizeof(callback.m_40));
                                M2ModelCallbacksPush(callbacks, &callback);
                            }
                        }
                    }
                }
            }
            bone = states[bone].m_ownerNext;
        }
        if (callbacks->m_size == 0) {
            return true;
        }
        bool queued = false;
        M2ModelCallback* callback = callbacks->m_data;
        M2ModelCallback* last = callback + callbacks->m_size;
        for (; callback != last; callback++) {
            if (callback->m_kind == 0) {
                uint16_t bone = KeyBoneIndex(anim, callback->m_10);
                M2SequenceTime* slot = this->m_animState->m_boneStates[bone].m_sequence;
                uint16_t index = slot->m_nextSequence;
                uint32_t queuedIndex;
                int32_t queuedStart;
                std::memcpy(&queuedIndex, callback->m_28, 4);
                std::memcpy(&queuedStart, callback->m_28 + 4, 4);
                if (queuedStart != slot->m_startTime || queuedIndex != index || index == 0xffff) {
                    continue;
                }
                if (((*slot->m_source.m_sequences)[index].m_flags & 1) != 0) {
                    slot->m_pending = 1;
                }
            }
            queued = true;
            M2ModelCallbacksPush(&list->m_callbacks, callback);
        }
        callbacks->m_size = 0;
        if (cursor <= from) {
            budget--;
        }
        if (queued) {
            if (0 <= static_cast<int32_t>(cursor - now)) {
                return true;
            }
            list->m_flags |= 8;
            this->m_3c0 = cursor;
            return true;
        }
        ahead = static_cast<int32_t>(cursor - now);
        from = cursor;
    }
    return true;
}

// The events of a sequence whose keys fall in (from, cursor], earliest first: their bones
// animated and their callbacks queued. Returns the time the collected callbacks are at.
// ref: FUN_1411b8f30
uint32_t CM2Model::DispatchEvents(M2BoneState* state, M2Array<M2Sequence>* sequences, M2Array<M2CompBone>* bones,
    uint32_t bone, uint32_t sequence, uint32_t base, uint32_t duration, uint32_t from, uint32_t cursor, uint32_t now,
    void* collected) {
    (void)duration;
    auto callbacks = static_cast<M2SceneVector<M2ModelCallback>*>(collected);
    if (this->m_events == nullptr || ((this->m_modelFlags2 >> 0x12 & 1) != 0 && base != 0)) {
        return cursor;
    }
    M2Data* data = this->m_shared->m_data;
    bool parentEvents = false;
    if (this->m_shared->m_parentEventData != nullptr) {
        parentEvents = ((*sequences)[sequence].m_flags & 0x1000) == 0;
    }
    bool animated = false;
    for (uint32_t i = 0; i < data->m_events.Count(); i++) {
        const M2Event* event = &data->m_events[i];
        const M2TrackBase* track = &event->m_enabled;
        if (parentEvents) {
            auto parent = static_cast<const M2Array<M2TrackBase>*>(this->m_shared->m_parentEventData);
            track = &(*parent)[i];
        }
        uint32_t slot = 0;
        if (track->m_timestamps.Count() == 0) {
            continue;
        }
        if (track->m_globalSequence == -1) {
            slot = sequence;
        }
        if (track->m_timestamps.Count() <= slot) {
            continue;
        }
        const M2Array<uint32_t>& times = track->m_timestamps[slot];
        uint32_t count = times.Count();
        if (count == 0) {
            continue;
        }
        uint16_t eventBone = event->m_bone;
        uint16_t owner = static_cast<uint16_t>((*bones)[eventBone].m_parentBone);
        if (owner != 0xffff) {
            if (bone != 0 && owner != bone) {
                uint32_t walk = owner;
                do {
                    if (walk == 0xffff) {
                        break;
                    }
                    walk = static_cast<uint16_t>((*bones)[walk].m_parentBone);
                } while (walk != bone);
                if (walk == 0xffff) {
                    continue;
                }
            }
            M2SequenceTime* eventSlot = this->m_animState->m_boneStates[eventBone].m_sequence;
            if (eventSlot != nullptr && eventSlot->m_nextSequence != sequence) {
                continue;
            }
        }
        for (uint32_t k = 0; k < count; k++) {
            M2SequenceTime* slotTime = state->m_sequence;
            int32_t at = static_cast<int32_t>(times[k]) - static_cast<int32_t>(slotTime->m_timeOffset);
            if (slotTime->m_speed != 1.0f) {
                at = static_cast<int32_t>(static_cast<float>(at) * slotTime->m_inverseSpeed);
            }
            at += slotTime->m_startTime;
            int32_t when = 0 <= at ? at : 0;
            when += static_cast<int32_t>(base);
            if (0 < when - static_cast<int32_t>(from) && when - static_cast<int32_t>(cursor) < 1) {
                if (static_cast<uint32_t>(when) != cursor) {
                    callbacks->m_size = 0;
                    cursor = static_cast<uint32_t>(when);
                }
                animated |= this->AnimateBoneFor(eventBone);
                if (callbacks->m_size < 0x400) {
                    M2ModelCallback callback = {};
                    callback.m_kind = 1;
                    callback.m_model = this;
                    callback.m_10 = this->EventKeyBone(bones, bone);
                    callback.m_14 = (*sequences)[sequence].m_id;
                    callback.m_function = this->m_events;
                    callback.m_20 = event->m_id;
                    callback.m_24 = event->m_data;
                    float position[3];
                    this->EventPosition(event, position);
                    std::memcpy(callback.m_28, position, 0xc);
                    callback.m_38 = now - static_cast<uint32_t>(when);
                    std::memcpy(callback.m_40, &this->m_5a0, sizeof(callback.m_40));
                    M2ModelCallbacksPush(callbacks, &callback);
                }
            }
        }
    }
    if (animated) {
        std::memset(this->m_boneDone, 0, this->m_boneDoneBytes);
    }
    return cursor;
}

// The key bone a sequence's events report: the bone's own, or its parent's when it has none.
uint32_t CM2Model::EventKeyBone(M2Array<M2CompBone>* bones, uint32_t bone) {
    const M2CompBone& compBone = (*bones)[bone];
    if (compBone.m_keyBoneId != -1) {
        return static_cast<uint32_t>(compBone.m_keyBoneId);
    }
    uint16_t parent = static_cast<uint16_t>(compBone.m_parentBone);
    if (parent == 0xffff) {
        return 0xffffffff;
    }
    return static_cast<uint32_t>((*bones)[parent].m_keyBoneId);
}

// An event's position through its bone and the model: the local matrix for a root model, and
// for an attached one the world matrix and then the scene's view.
void CM2Model::EventPosition(const M2Event* event, float* out) {
    const float* b = this->m_animState->m_boneMatrices[event->m_bone].m;
    bool attached = this->m_parent != nullptr && (this->m_flags >> 0xe & 1) == 0;
    const float* m = attached ? this->m_world.m : this->m_local.m;
    float rows[4][4];
    for (uint32_t c = 0; c < 4; c++) {
        rows[0][c] = (b[2] * m[8 + c] + b[3] * m[12 + c]) + (b[1] * m[4 + c] + b[0] * m[c]);
        rows[1][c] = (b[4] * m[c] + b[5] * m[4 + c]) + (b[6] * m[8 + c] + b[7] * m[12 + c]);
        rows[2][c] = (b[8] * m[c] + b[9] * m[4 + c]) + (b[10] * m[8 + c] + b[11] * m[12 + c]);
        rows[3][c] = (b[12] * m[c] + b[13] * m[4 + c]) + (b[14] * m[8 + c] + b[15] * m[12 + c]);
    }
    float x = event->m_position.x;
    float y = event->m_position.y;
    float z = event->m_position.z;
    float p[4];
    for (uint32_t c = 0; c < 4; c++) {
        p[c] = (rows[1][c] * y + rows[0][c] * x) + (rows[2][c] * z + rows[3][c]);
    }
    if (attached) {
        const float* v = this->m_scene->m_viewProjection.m;
        float q[4];
        for (uint32_t c = 0; c < 4; c++) {
            q[c] = (p[3] * v[12 + c] + p[2] * v[8 + c]) + (p[1] * v[4 + c] + p[0] * v[c]);
        }
        out[0] = q[0];
        out[1] = q[1];
        out[2] = q[2];
        return;
    }
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}

// A sequence's end: the end function told, then what follows started on the bone.
// ref: FUN_1411b5780
void CM2Model::RunSequenceCallback(const M2ModelCallback* callback) {
    M2Data* data = this->m_shared->m_data;
    uint16_t bone = KeyBoneIndex(this->m_animState, callback->m_10);
    M2AnimState* anim = this->m_animState;
    M2BoneState* state = &anim->m_boneStates[bone];
    M2SequenceTime* slot = state->m_sequence;
    uint16_t index = slot->m_nextSequence;
    if (index == 0xffff) {
        return;
    }
    M2Array<uint16_t>* lookup = anim->m_sequenceHash;
    M2Array<M2Sequence>* sequences = anim->m_sequences;
    const M2Sequence& sequence = (*slot->m_source.m_sequences)[index];
    if ((sequence.m_flags & 1) != 0) {
        slot->m_pending = 1;
    }
    if (callback->m_function != nullptr) {
        SEQUENCEENDINFO info;
        int32_t start = slot->m_startTime;
        info.m_keyBone = callback->m_10;
        info.m_sequenceId = callback->m_20;
        info.m_variation = state->m_variation;
        info.m_24 = callback->m_24;
        info.m_blendTime = callback->m_38;
        std::memcpy(info.m_40, callback->m_40, sizeof(info.m_40));
        info.m_one = 1;
        reinterpret_cast<void (*)(CM2Model* model, SEQUENCEENDINFO* info)>(callback->m_function)(callback->m_model, &info);
        if (start != slot->m_startTime || index != slot->m_nextSequence) {
            return;
        }
    }
    if ((sequence.m_flags & 1) != 0 || this->m_refCount <= 0 || slot->m_endless == 0
        || (sequence.m_variationIndex == 0 && sequence.m_variationNext == -1)) {
        return;
    }
    float speed = slot->m_speed;
    M2SequenceResolve resolve;
    M2ResolveSequence(callback->m_20, &resolve, anim, sequences, lookup,
        static_cast<M2Array<uint16_t>*>(this->m_shared->m_blocklistAnimData));
    if (resolve.m_parentHas != 0) {
        sequences = anim->m_parentSequences;
        lookup = anim->m_parentHash;
    }
    uint16_t picked = M2FindSequenceVariation(resolve.m_sequence, 0, sequences, lookup);
    int32_t variation;
    M2PickVariation(&variation, &picked, sequences);
    uint32_t blend;
    if (0.000099999997f <= std::fabs(speed - 1.0f)) {
        blend = static_cast<uint32_t>(std::roundf(static_cast<float>(callback->m_38) * slot->m_inverseSpeed));
    } else {
        blend = callback->m_38;
    }
    if (((*sequences)[picked].m_flags & 0x20) == 0) {
        if (this->m_skeleton == nullptr) {
            for (CM2SequenceLoad* load = this->m_shared->m_sequenceLoads.Head(); load != nullptr;
                 load = this->m_shared->m_sequenceLoads.Next(load)) {
                for (CM2SequencePlayBack* playback = load->m_playbacks.Head(); playback != nullptr;
                     playback = load->m_playbacks.Next(playback)) {
                    if (playback->m_model == this && playback->m_bone == bone && (playback->m_flags & 2) != 0) {
                        return;
                    }
                }
            }
        } else if (M2SkeletonHasPlayback(this->m_skeleton, this, bone, true)) {
            return;
        }
        this->QueuePlayback(picked, data, bone, blend, speed, &resolve, 1, 1, slot->m_repeats, 0, 0, 0x3f800000);
    } else {
        state->m_5c = callback->m_20;
        state->m_variation = static_cast<uint16_t>(variation);
        this->StartSequence(picked, bone, &resolve, blend, speed, static_cast<int32_t>(slot->m_repeats), 0, 0, 1.0f);
    }
}
