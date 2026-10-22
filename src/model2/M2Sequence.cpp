#include <cmath>
#include <cstring>

#include "base/ObjectAlloc.hpp"
#include "db/DB2Tables.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"

// Engine/Source/Model2: choosing and starting sequences (M2Model.cpp) and the sequence helpers
// of M2Animate.cpp.

namespace {

const char* const s_playbackTag = ".?AV?$TSList@UCM2SequencePlayBack@@V?$TSGetLink@UCM2SequencePlayBack@@@@$07@@";
const char* const s_modelFile = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Model.cpp";

M2Sequence& SequenceAt(M2Array<M2Sequence>* sequences, uint32_t index) {
    return (*sequences)[index];
}

void* AlignedModelAlloc(uint32_t size, int32_t line) {
    // FUN_1412be640: 16-byte aligned, the offset kept in the byte before.
    auto memory = static_cast<uint8_t*>(SMemAlloc(size + 0x10, 0x12, s_modelFile, 0, line, 0));
    if (memory == nullptr) {
        return nullptr;
    }
    intptr_t pad = -static_cast<intptr_t>(reinterpret_cast<uintptr_t>(memory) & 0xf) + 0x10;
    memory[pad - 1] = static_cast<uint8_t>(pad);
    return memory + pad;
}

}  // namespace

// ---- M2Animate.cpp: sequence lookup -----------------------------------------------------------

// AnimationData's flags (field 3) for a record (the reference reads the packed file directly).
// ref: FUN_1411febe0
uint32_t M2AnimationDataFlags(const void* record, uint32_t index) {
    return g_animationDataDB.FieldValue(static_cast<const uint8_t*>(record), 3, static_cast<int32_t>(index));
}

// The blend time from sequence `from` into `to`: none for kind 5, the "to" sequence's blend-in
// time, or the shorter/longer of that and the "from" one's blend-out by kind (FUN_1411feca0).
// ref: FUN_1411feca0
uint32_t M2SequenceBlendTime(uint16_t to, uint16_t from, const M2Array<M2Sequence>* fromSequences, uint8_t kind,
    uint16_t fixed, const M2Array<M2Sequence>* toSequences) {
    if (kind == 5) {
        return 0;
    }
    if (kind == 1) {
        return fixed;
    }
    const M2Sequence& sequence = (*toSequences)[to];
    uint32_t blendIn;
    if ((sequence.m_flags & 0x200) == 0) {
        blendIn = *reinterpret_cast<const uint32_t*>(&sequence.m_blendTimeIn);
    } else {
        blendIn = sequence.m_blendTimeIn;
    }
    uint32_t result = blendIn;
    if (from != 0xffff && kind != 4) {
        const M2Sequence& previous = (*fromSequences)[from];
        if ((previous.m_flags & 0x200) != 0) {
            uint16_t blendOut = previous.m_blendTimeOut;
            if (blendOut != 0) {
                result = blendOut;
                if (blendIn != 0 && kind != 0) {
                    if (kind == 2) {
                        result = blendIn < blendOut ? blendIn : blendOut;
                    } else {
                        result = blendIn;
                        if (kind == 3) {
                            result = blendOut < blendIn ? blendIn : blendOut;
                        }
                    }
                }
            }
        }
    }
    if (kind == 2) {
        if (fixed < result) {
            result = fixed;
        }
    } else if (kind == 3) {
        return fixed <= result ? result : fixed;
    }
    return result;
}

// The `variation`th variation of sequence `id`, 0xffff when there is none (FUN_1411fee80).
// ref: FUN_1411fee80
uint16_t M2FindSequenceVariation(uint32_t id, int32_t variation, const M2Array<M2Sequence>* sequences,
    const M2Array<uint16_t>* hash) {
    uint16_t index = M2FindSequenceById(id, hash, sequences);
    for (;;) {
        if (sequences->Count() <= index) {
            return 0xffff;
        }
        if (variation == 0) {
            return index;
        }
        variation--;
        index = static_cast<uint16_t>((*sequences)[index].m_variationNext);
    }
}

// Whether the model has sequence `id` and it is not blocked (FUN_1411feef0).
// ref: FUN_1411feef0
bool M2HasSequence(int16_t id, const M2Array<M2Sequence>* sequences, const M2Array<uint16_t>* hash,
    const M2Array<uint16_t>* blocked) {
    if (blocked != nullptr) {
        for (uint32_t i = 0; i < blocked->Count(); i++) {
            if (static_cast<int16_t>((*blocked)[i]) == id) {
                return false;
            }
        }
    }
    return M2FindSequenceById(static_cast<uint16_t>(id), hash, sequences) < sequences->Count();
}

// Whether the model or its parent skeleton has sequence `id` (FUN_1411fef70).
// ref: FUN_1411fef70
bool M2AnimStateHasSequence(uint32_t id, const M2AnimState* anim) {
    const M2Array<M2Sequence>* sequences = anim->m_sequences;
    uint16_t index = M2FindSequenceById(id, anim->m_sequenceHash, sequences);
    if ((anim->m_flags & 1) == 0) {
        return index < sequences->Count();
    }
    if (sequences->Count() <= index) {
        const M2Array<M2Sequence>* parent = anim->m_parentSequences;
        if (parent->Count() <= M2FindSequenceById(id, anim->m_parentHash, parent)) {
            return false;
        }
    }
    return true;
}

// How many times a sequence repeats: between its minimum and maximum, at random, at least once
// (FUN_1411ff000).
// ref: FUN_1411ff000
int32_t M2SequenceRepeats(const M2Sequence* sequence) {
    int32_t minimum = static_cast<int32_t>(sequence->m_replayMin);
    int32_t spread = (std::rand() * (static_cast<int32_t>(sequence->m_replayMax) - minimum));
    int32_t repeats = ((spread + (spread >> 31 & 0x7fff)) >> 15) + minimum;
    if (repeats == 0) {
        repeats = 1;
    }
    return repeats;
}

// A variation chosen by the sequences' frequencies (FUN_1411ff040).
// ref: FUN_1411ff040
void M2PickVariation(int32_t* variation, uint16_t* index, const M2Array<M2Sequence>* sequences) {
    int32_t count = 0;
    *variation = 0;
    uint32_t roll = static_cast<uint32_t>(std::rand());
    uint16_t current = *index;
    while (current != 0xffff) {
        uint32_t frequency = static_cast<uint32_t>(static_cast<uint16_t>((*sequences)[current].m_frequency));
        if (roll < frequency) {
            *index = current;
            *variation = count;
            return;
        }
        current = static_cast<uint16_t>((*sequences)[current].m_variationNext);
        roll = roll - frequency;
        count++;
    }
}

// ref: FUN_1411ff0e0
void M2ResolveSequence(uint32_t id, M2SequenceResolve* out, M2AnimState* anim, M2Array<M2Sequence>* sequences,
    M2Array<uint16_t>* hash, M2Array<uint16_t>* blocked) {
    out->m_fallback = 0;
    out->m_parentHas = 0;
    int32_t visited[0x6ec];
    std::memset(visited, 0, sizeof(visited));
    int32_t balance = 0;
    uint8_t blend = 0;
    int32_t direction = 1;
    uint32_t current = id;
    uint8_t kind;
    for (;;) {
        uint16_t index = M2FindSequenceById(current, hash, sequences);
        bool found = index < sequences->Count();
        if (!found && (anim->m_flags & 1) != 0) {
            bool isBlocked = false;
            if (blocked != nullptr) {
                for (uint32_t i = 0; i < blocked->Count(); i++) {
                    if (static_cast<int16_t>((*blocked)[i]) == static_cast<int16_t>(current)) {
                        isBlocked = true;
                        break;
                    }
                }
            }
            if (!isBlocked) {
                M2Array<M2Sequence>* parent = anim->m_parentSequences;
                if (M2FindSequenceById(current, anim->m_parentHash, parent) < parent->Count()) {
                    out->m_parentHas = 1;
                    found = true;
                }
            }
        }
        if (found) {
            if (direction < 1) {
                kind = direction < 0 ? 1 : static_cast<uint8_t>((0 < balance) + 2);
            } else {
                kind = 0;
            }
            break;
        }
        bool local;
        const void* record = g_animationDataDB.GetRecord(current, false, &local);
        bool exhausted = 0x6eb < current || visited[current] != 0 || record == nullptr;
        if (!exhausted) {
            uint32_t fallback = DB2FieldU16(static_cast<const uint8_t*>(record), 0, 0, &g_animationDataDB);
            exhausted = current == fallback;
        }
        if (exhausted) {
            // No more fallbacks: Stand, else 147, else the file's first sequence.
            uint16_t count = sequences->m_count;
            if (M2FindSequenceById(0, hash, sequences) < count) {
                current = 0;
            } else {
                current = 0x93;
                if (count <= M2FindSequenceById(0x93, hash, sequences)) {
                    current = (*sequences)[0].m_id;
                }
            }
            kind = 0;
            out->m_fallback = 1;
            break;
        }
        visited[current] = 1;
        current = DB2FieldU16(static_cast<const uint8_t*>(record), 0, 0, &g_animationDataDB);
        uint32_t flags = M2AnimationDataFlags(record, 0);
        if ((flags & 0x10) != 0) {
            balance += direction;
        }
        int32_t next = (flags & 0x10) != 0 ? -direction : direction;
        flags = M2AnimationDataFlags(record, 0);
        bool keep = (flags >> 5 & 1) == 0;
        if (!keep) {
            balance = next + balance;
        }
        direction = keep ? next : 0;
        if ((M2AnimationDataFlags(record, 0) & 0x80) != 0) {
            blend = 1;
        }
    }
    out->m_sequence = static_cast<uint16_t>(current);
    out->m_kind = current != 0 ? kind : 0;
    out->m_blend = blend;
}

// A slot's clock for a sequence played from `offset` at `speed` (FUN_1411ff380): a reversed
// kind plays backwards from the end, the hold kinds freeze it.
// ref: FUN_1411ff380
void M2SequenceTimeStart(const M2SequenceResolve* resolve, uint32_t offset, float speed, M2SequenceTime* slot,
    const M2Sequence* sequence, CM2Scene* scene) {
    uint32_t total = slot->m_repeats * sequence->m_duration;
    uint32_t from = offset;
    // DAT_1426c6510, DAT_1426cc58c
    if (10000.0f < speed) {
        speed = 1.0f;
    } else if (speed < -10000.0f) {
        speed = -1.0f;
    }
    if (speed != 0.0f) {
        if (((resolve->m_kind - 1u) & 0xfd) == 0) {
            speed = -speed;
        }
        uint32_t base = 0;
        if (total != 0 && speed < 0.0f) {
            from = (total - offset) % total;
            base = total;
        }
        offset = base;
        if (static_cast<uint8_t>(resolve->m_kind - 2) < 2) {
            speed = 0.0f;
        }
    }
    float inverse;
    if (std::fabs(speed) <= 1.0e-05f) {  // DAT_1426c75c4
        inverse = 0.0f;
    } else {
        inverse = 1.0f / speed;
    }
    float rate = std::fabs(inverse);
    int32_t start = static_cast<int32_t>(scene->m_time) - static_cast<int32_t>(static_cast<float>(static_cast<int32_t>(from)) * rate);
    if ((~(scene->m_flags >> 2) & 1) != 0 && 0.0f <= speed) {
        start = start + 1;
    }
    slot->m_speed = speed;
    slot->m_inverseSpeed = inverse;
    slot->m_startTime = start;
    slot->m_timeOffset = offset;
    slot->m_endTime = static_cast<int32_t>(static_cast<int64_t>(static_cast<float>(total) * rate)) + start;
}

// A source record from the model's or its parent skeleton's arrays (FUN_1412006d0).
// ref: FUN_1412006d0
void M2AnimStateCopySource(const M2AnimState* anim, M2SequenceSource* out, bool parent) {
    if (parent) {
        out->m_sequences = anim->m_parentSequences;
        out->m_sequenceHash = anim->m_parentHash;
        out->m_bones = anim->m_parentBones;
        out->m_keyBoneLookup = anim->m_parentKeyBoneLookup;
        out->m_attachments = anim->m_parentAttachments;
        out->m_attachmentLookup = anim->m_parentAttachmentLookup;
        out->m_flags |= 1;
        return;
    }
    out->m_sequences = anim->m_sequences;
    out->m_sequenceHash = anim->m_sequenceHash;
    out->m_bones = anim->m_bones;
    out->m_keyBoneLookup = anim->m_keyBoneLookup;
    out->m_attachments = anim->m_attachments;
    out->m_attachmentLookup = anim->m_attachmentLookup;
}

// ref: FUN_141200740
void M2SequenceSourceClear(M2SequenceSource* source) {
    source->m_sequences = nullptr;
    source->m_sequenceHash = nullptr;
    source->m_bones = nullptr;
    source->m_keyBoneLookup = nullptr;
    source->m_attachments = nullptr;
    source->m_attachmentLookup = nullptr;
    source->m_flags = 0;
}

// ---- M2Model.cpp: slots ----------------------------------------------------------------------

// A new slot (FUN_1411c6950).
// ref: FUN_1411c6950
M2SequenceTime* M2SequenceTimeAlloc() {
    auto slot = static_cast<M2SequenceTime*>(M2GetBoneSeqPool()->Alloc());
    slot->m_time = 0;
    slot->m_sequence = 0xffff;
    slot->m_bone = 0xffff;
    slot->m_nextSequence = 0xffff;
    slot->m_pending = 1;
    slot->m_endless = 1;
    slot->m_startTime = 0;
    slot->m_endTime = 0;
    slot->m_speed = 1.0f;
    slot->m_inverseSpeed = 1.0f;
    slot->m_timeOffset = 0;
    slot->m_repeats = 1;
    // FUN_141200670
    std::memset(&slot->m_source, 0, 0x34);
    return slot;
}

// The primary slot released (FUN_1411b6bc0).
// ref: FUN_1411b6bc0
void CM2Model::DropSequence(M2BoneState* state) {
    M2SequenceTime* slot = state->m_sequence;
    if (slot == nullptr) {
        return;
    }
    if ((state->m_flags & 0x2000) != 0) {
        if (this->m_skeleton == nullptr) {
            M2SharedReleaseSequence(this->m_shared, slot->m_nextSequence);
        } else {
            const M2Sequence& sequence = SequenceAt(slot->m_source.m_sequences, slot->m_nextSequence);
            M2SkeletonReleaseSequence(M2SkeletonGetSource(this->m_skeleton, (sequence.m_flags >> 12 & 1) != 0),
                slot->m_nextSequence);
        }
    }
    if (state->m_sequence != nullptr) {
        if ((state->m_flags & 0x2000) != 0) {
            M2SequenceSourceClear(&state->m_sequence->m_source);
            M2GetBoneSeqPool()->Free(state->m_sequence);
            state->m_flags &= ~0x2000u;
        }
        state->m_sequence = nullptr;
    }
}

// The primary slot released without its sequence reference (FUN_1411b6c90).
// ref: FUN_1411b6c90
void M2BoneStateFreeSequence(M2BoneState* state) {
    if (state->m_sequence == nullptr) {
        return;
    }
    if ((state->m_flags & 0x2000) != 0) {
        M2SequenceSourceClear(&state->m_sequence->m_source);
        M2GetBoneSeqPool()->Free(state->m_sequence);
        state->m_flags &= ~0x2000u;
    }
    state->m_sequence = nullptr;
}

// The primary slot becomes the blended-from one (FUN_1411c65e0).
// ref: FUN_1411c65e0
void CM2Model::BlendFromCurrent(M2BoneState* state) {
    this->DropBlend(state);
    M2SequenceTime* copy = M2SequenceTimeAlloc();
    M2SequenceTime* current = state->m_sequence;
    state->m_flags |= 0x4000;
    state->m_blendSequence = copy;
    copy->m_time = current->m_time;
    copy->m_sequence = current->m_sequence;
    copy->m_bone = current->m_bone;
    copy->m_nextSequence = current->m_nextSequence;
    copy->m_startTime = current->m_startTime;
    copy->m_endTime = current->m_endTime;
    copy->m_speed = current->m_speed;
    copy->m_inverseSpeed = current->m_inverseSpeed;
    copy->m_timeOffset = current->m_timeOffset;
    copy->m_repeats = current->m_repeats;
    copy->m_pending = current->m_pending;
    copy->m_endless = current->m_endless;
    // FUN_141200690
    std::memcpy(&copy->m_source, &current->m_source, 0x34);
    M2SequenceTime* blend = state->m_blendSequence;
    if (this->m_skeleton == nullptr) {
        M2SharedAddSequenceRef(this->m_shared, blend->m_nextSequence);
    } else {
        const M2Sequence& sequence = SequenceAt(blend->m_source.m_sequences, blend->m_nextSequence);
        M2SkeletonAddSequenceRef(M2SkeletonGetSource(this->m_skeleton, (sequence.m_flags >> 12 & 1) != 0),
            blend->m_nextSequence);
    }
    this->DropSequence(state);
}

// Asks the callback at 0x408 whether a bone's sequence may change; false when the model went
// away inside it (FUN_1411c6c00).
// ref: FUN_1411c6c00
bool CM2Model::AllowSequenceChange(uint32_t id, uint16_t bone) {
    if (this->m_408 == nullptr) {
        return true;
    }
    M2BoneState* state = &this->m_animState->m_boneStates[bone];
    M2SequenceTime* slot = state->m_sequence;
    if (slot == nullptr || slot->m_nextSequence == 0xffff || slot->m_pending != 0) {
        return true;
    }
    const M2CompBone& def = (*this->m_animState->m_bones)[bone];
    if (def.m_keyBoneId == -1 && bone != 0) {
        return true;
    }
    this->AddRef();
    // (the reference's layout to the byte: `mov qword ptr [rsp + 0x2c], 1` puts its 1 at +0xc,
    // unaligned, and +0x10 is 0; an aligned uint64_t moved it to +0x10 and left +0xc as padding,
    // and the owner's callback, reading +0xc, set a sequence again and recursed until the stack
    // ran out on Enter World, 2026-10-22)
    struct {
        uint32_t m_id;          // 0x0
        uint32_t m_previous;    // 0x4
        uint32_t m_variation;   // 0x8
        uint32_t m_one;         // 0xc
        uint32_t m_zero;        // 0x10
        uint32_t m_14;          // 0x14 (not written by the reference)
        uint32_t m_closure[4];  // 0x18
        uint32_t m_one2;        // 0x28
    } args;
    static_assert(sizeof(args) == 0x2c, "the callback's argument block is 0x2c bytes");
    args.m_id = def.m_parentBone != -1 ? id : 0xffffffff;
    args.m_previous = state->m_5c;
    args.m_variation = state->m_variation;
    args.m_one = 1;
    args.m_zero = 0;
    args.m_14 = 0;
    std::memcpy(args.m_closure, &this->m_410, sizeof(args.m_closure));
    args.m_one2 = 1;
    reinterpret_cast<void (*)(CM2Model*, void*)>(this->m_408)(this, &args);
    return this->Release(1) != 0;
}

// A bone's sequence set without starting a new one: the bone keeps its parent's slots, the blend
// easing in or not (FUN_1411cde00).
// ref: FUN_1411cde00
void CM2Model::ReleaseBoneSequence(uint32_t bone, uint8_t kind, int32_t blend, uint16_t fixedBlend,
    M2Array<M2CompBone>* bones, M2BoneState* states) {
    M2BoneState* state = &states[bone];
    if (blend == 0 || state->m_sequence == nullptr) {
        this->DropBlend(state);
        M2BoneStatesPropagateSlots(states, bones);
        return;
    }
    if (state->m_ownerLink != nullptr) {
        *state->m_ownerLink = state->m_ownerNext;
    }
    if (state->m_ownerNext != 0xffff) {
        states[state->m_ownerNext].m_ownerLink = state->m_ownerLink;
    }
    state->m_ownerNext = 0xffff;
    state->m_ownerLink = nullptr;
    // The nearest parent with its own primary slot.
    M2BoneState* owner = nullptr;
    M2Array<M2Sequence>* ownerSequences = nullptr;
    for (uint16_t parent = static_cast<uint16_t>((*bones)[bone].m_parentBone); parent < bones->Count();
         parent = static_cast<uint16_t>((*bones)[parent].m_parentBone)) {
        M2BoneState* candidate = &states[parent];
        owner = candidate;
        if (candidate->m_sequence != nullptr) {
            ownerSequences = candidate->m_sequence->m_source.m_sequences;
            if ((candidate->m_flags & 0x2000) != 0) {
                break;
            }
        }
    }
    M2SequenceTime* blendSlot = state->m_blendSequence;
    M2Array<M2Sequence>* sequences = this->m_animState->m_sequences;
    float weight;
    if (blendSlot == nullptr || (state->m_flags & 0x4000) == 0) {
        weight = 0.0f;
    } else {
        weight = static_cast<float>(state->m_blendEnd - static_cast<int32_t>(this->m_scene->m_time)) * state->m_blendRate;
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
    int16_t from;
    if (weight <= 0.5f) {
        from = static_cast<int16_t>(state->m_sequence->m_nextSequence);
    } else {
        from = static_cast<int16_t>(blendSlot->m_nextSequence);
    }
    if (from != -1) {
        sequences = weight <= 0.5f ? state->m_sequence->m_source.m_sequences : blendSlot->m_source.m_sequences;
    }
    uint32_t time;
    if (owner == nullptr || owner->m_blendSequence == nullptr || (owner->m_flags & 0x4000) == 0
        || owner->m_blendMax <= 0.0f || (time = static_cast<uint32_t>(owner->m_blendEnd)) == 0) {
        time = M2SequenceBlendTime(owner != nullptr ? owner->m_sequence->m_nextSequence : 0, static_cast<uint16_t>(from),
            sequences, kind, fixedBlend, ownerSequences);
    } else {
        uint32_t now = this->m_scene->m_time;
        if (time <= now) {
            goto noBlend;
        }
        time = time - now;
    }
    if (0xf < time) {
        if (weight <= 0.5f) {
            this->BlendFromCurrent(state);
        } else {
            this->DropSequence(state);
        }
        state->m_blendMax = 1.0f;
        state->m_blendEnd = static_cast<int32_t>(this->m_scene->m_time + time);
        state->m_blendRate = 1.0f / static_cast<float>(time);
        goto done;
    }
noBlend:
    this->DropBlend(state);
    this->DropSequence(state);
    kind = 5;
    fixedBlend = 0;
done:
    state->m_blendKind = kind;
    state->m_fixedBlend = fixedBlend;
    state->m_5c = 0xffffffff;
    state->m_variation = 0;
    M2BoneStatesPropagateSlots(states, bones);
}

// A bone's own sequence ended: back to the parent's (FUN_1411cdc50).
// ref: FUN_1411cdc50
void CM2Model::SetBoneSequenceNone(uint32_t a2, uint8_t kind, int32_t blend, int32_t check, uint32_t id,
    uint16_t fixedBlend) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call != nullptr) {
            call->m_type = 7;
            call->m_next = nullptr;
            call->m_frame = this->m_scene->m_time;
            call->m_args[3] = static_cast<uint32_t>(check);
            call->m_args[0] = a2;
            *reinterpret_cast<uint8_t*>(&call->m_args[1]) = kind;
            call->m_args[2] = static_cast<uint32_t>(blend);
            call->m_args[4] = id;
            *reinterpret_cast<uint16_t*>(&call->m_args[5]) = fixedBlend;
            *this->m_callsTail = call;
            this->m_callsTail = &call->m_next;
        }
        return;
    }
    M2AnimState* anim = this->m_animState;
    M2Array<M2CompBone>* bones = anim->m_bones;
    M2BoneState* states = anim->m_boneStates;
    uint8_t blendKind = (this->m_modelFlags & 0x10) == 0 ? kind : 5;
    if (check != 0 && !M2AnimStateHasSequence(id, anim)) {
        return;
    }
    uint32_t bone = 0;
    if (a2 != 0xffffffff) {
        bone = a2 < anim->m_keyBoneLookup->Count() ? (*anim->m_keyBoneLookup)[a2] : 0xffff;
    }
    if (static_cast<uint16_t>(bone) < bones->Count() && static_cast<uint16_t>(bone) != 0
        && (*bones)[bone].m_parentBone != -1 && this->AllowSequenceChange(a2, static_cast<uint16_t>(bone))) {
        this->CancelPlaybacks(bone, blend != 0);
        this->ReleaseBoneSequence(bone, blendKind, blend, fixedBlend, bones, states);
    }
}

// The pending .anim playbacks of a bone dropped, or marked dead when the file is still being read
// (FUN_1411b5e20).
// ref: FUN_1411b5e20
void CM2Model::CancelPlaybacks(uint32_t bone, bool blend) {
    if (this->m_skeleton != nullptr) {
        M2SkeletonCancelPlaybacks(this->m_skeleton, this, bone, blend);
    }
    CM2Shared* shared = this->m_shared;
    for (CM2SequenceLoad* load = shared->m_sequenceLoads.Head(); load != nullptr;
         load = shared->m_sequenceLoads.Next(load)) {
        for (CM2SequencePlayBack* playback = load->m_playbacks.Head(); playback != nullptr;) {
            CM2SequencePlayBack* next = load->m_playbacks.Next(playback);
            if (playback->m_model == this && playback->m_bone == bone && (playback->m_flags >> 1 & 1) == blend) {
                if ((shared->m_flags & CM2Shared::SHARED_SEQUENCES_PENDING) == 0) {
                    playback->m_link.Unlink();
                    SMemFree(playback, s_playbackTag, -2, 0);
                } else {
                    playback->m_flags |= 8;
                }
            }
            playback = next;
        }
    }
}

// A playback waiting for sequence `sequence`'s .anim (FUN_1411ca7d0): joined to the load already
// reading it, or a new load.
// ref: FUN_1411ca7d0
void CM2Model::QueuePlayback(uint16_t sequence, M2Data* data, uint16_t bone, uint32_t offset, float speed,
    const M2SequenceResolve* resolve, int32_t blend, int32_t endless, uint32_t repeats, uint8_t kind,
    uint16_t fixedBlend, uint32_t weight) {
    if (this->m_skeleton != nullptr) {
        M2SkeletonQueuePlayback(this->m_skeleton, this, sequence, this->m_animState, bone, offset, speed, resolve,
            blend, endless, repeats, kind, fixedBlend, weight);
        return;
    }
    M2Array<M2Sequence>& sequences = data->m_sequences;
    uint32_t flags = sequences[sequence].m_flags;
    CM2SequencePlayBack* playback = nullptr;
    if ((flags & 0x10) != 0) {
        // Already being read: find the load of this sequence or an alias of it.
        uint16_t alias = sequence;
        CM2SequenceLoad* found = nullptr;
        for (;;) {
            for (CM2SequenceLoad* load = this->m_shared->m_sequenceLoads.Head(); load != nullptr;
                 load = this->m_shared->m_sequenceLoads.Next(load)) {
                if (load->m_sequence == alias) {
                    found = load;
                    break;
                }
            }
            if (found != nullptr) {
                break;
            }
            alias = sequences[alias].m_aliasNext;
            if (alias == sequence) {
                return;
            }
        }
        for (CM2SequencePlayBack* existing = found->m_playbacks.Head(); existing != nullptr;
             existing = found->m_playbacks.Next(existing)) {
            if (existing->m_model == this && existing->m_bone == bone) {
                playback = existing;
                break;
            }
        }
        if (playback == nullptr) {
            playback = static_cast<CM2SequencePlayBack*>(SMemAlloc(sizeof(CM2SequencePlayBack), 8, s_playbackTag, 0, -2, 8));
            std::memset(playback, 0, 0x18);
            found->m_playbacks.LinkToTail(playback);
            playback->m_model = this;
        }
    } else {
        if ((flags >> 8 & 1) != 0) {
            return;
        }
        CM2SequenceLoad* load = M2SharedRequestSequence(this->m_shared, sequence);
        if (load == nullptr) {
            return;
        }
        playback = static_cast<CM2SequencePlayBack*>(SMemAlloc(sizeof(CM2SequencePlayBack), 8, s_playbackTag, 0, -2, 8));
        std::memset(playback, 0, 0x18);
        load->m_playbacks.LinkToTail(playback);
        playback->m_model = this;
        this->PreloadSequence(sequences[sequence].m_id);
    }
    playback->m_offset = offset;
    playback->m_speed = speed;
    playback->m_bone = bone;
    playback->m_resolve = *resolve;
    uint8_t playFlags = kind != 5 ? 1 : 0;
    playback->m_flags = playFlags;
    if (blend != 0) {
        playFlags |= 2;
        playback->m_flags = playFlags;
    }
    if (endless != 0) {
        playback->m_flags = playFlags | 4;
    }
    playback->m_repeats = static_cast<uint8_t>(repeats);
    playback->m_fixedBlend = fixedBlend;
    playback->m_weight = weight;
    playback->m_kind = kind;
}

// A slot started on sequence `index` (FUN_1411cccd0).
// ref: FUN_1411cccd0
void CM2Model::StartSlot(uint16_t index, const M2SequenceResolve* resolve, uint32_t offset, float speed,
    M2SequenceTime* slot, int32_t repeats, M2Array<M2Sequence>* sequences) {
    if (repeats == 0) {
        repeats = M2SequenceRepeats(&(*sequences)[index]);
    }
    if (this->m_skeleton == nullptr) {
        M2SharedAddSequenceRef(this->m_shared, index);
    } else {
        M2SkeletonAddSequenceRef(M2SkeletonGetSource(this->m_skeleton, resolve->m_parentHas != 0), index);
    }
    slot->m_nextSequence = index;
    slot->m_sequence = index;
    slot->m_repeats = static_cast<uint32_t>(repeats);
    slot->m_pending = 0;
    M2AnimStateCopySource(this->m_animState, &slot->m_source, resolve->m_parentHas != 0);
    M2SequenceTimeStart(resolve, offset, speed, slot, &(*sequences)[index], this->m_scene);
}

// A bone's primary sequence started, blending from what it played (FUN_1411cbfa0).
// ref: FUN_1411cbfa0
void CM2Model::StartSequence(uint16_t index, uint16_t bone, const M2SequenceResolve* resolve, uint32_t offset,
    float speed, int32_t repeats, uint8_t kind, uint16_t fixedBlend, float weight) {
    M2Data* data = this->m_shared->m_data;
    M2AnimState* anim = this->m_animState;
    M2Array<M2CompBone>* bones = anim->m_bones;
    M2Array<M2Sequence>* sequences = resolve->m_parentHas == 0 ? anim->m_sequences : anim->m_parentSequences;
    M2BoneState* states = anim->m_boneStates;
    M2Sequence* sequence = &(*sequences)[index];
    uint16_t* ownerHead = anim->m_ownerHead;
    M2BoneState* state = &states[bone];
    M2SequenceTime* blendSlot = state->m_blendSequence;
    M2Array<M2Sequence>* fromSequences = sequences;
    int16_t from = -1;     // (set on every path below)
    bool useBlend = false;
    bool decided = false;
    if (blendSlot != nullptr && (state->m_flags & 0x4000) != 0) {
        float w = static_cast<float>(state->m_blendEnd - static_cast<int32_t>(this->m_scene->m_time)) * state->m_blendRate;
        float eased = 0.0f;
        if (0.0f <= w) {
            eased = 1.0f;
            if (w <= 1.0f) {
                eased = (3.0f - (w + w)) * w * w;
            }
        }
        if (0.5f < eased * state->m_blendMax) {
            from = static_cast<int16_t>(blendSlot->m_nextSequence);
            useBlend = true;
            decided = true;
        }
    }
    if (!decided) {
        M2SequenceTime* primary = state->m_sequence;
        if (primary == nullptr && blendSlot != nullptr) {
            from = static_cast<int16_t>(blendSlot->m_nextSequence);
            useBlend = true;
        } else {
            useBlend = false;
            from = primary == nullptr ? -1 : static_cast<int16_t>(primary->m_nextSequence);
        }
    }
    if (from != -1) {
        if (useBlend) {
            fromSequences = blendSlot->m_source.m_sequences;
        } else {
            fromSequences = state->m_sequence == nullptr ? nullptr : state->m_sequence->m_source.m_sequences;
        }
    }
    uint32_t blendTime = M2SequenceBlendTime(index, static_cast<uint16_t>(from), fromSequences, kind, fixedBlend,
        sequences);
    M2SequenceTime* current = state->m_sequence;
    if (current != nullptr && current->m_nextSequence == index && (state->m_flags & 0x2000) != 0
        && resolve->m_parentHas == (current->m_source.m_flags & 1)) {
        uint32_t ahead = offset - current->m_time;
        uint32_t behind = current->m_time - offset;
        uint32_t distance = ahead <= behind ? ahead : behind;
        if (distance <= blendTime) {
            if (distance < blendTime || 2.3841858e-07f <= std::fabs(current->m_speed - speed)) {
                M2SequenceTimeStart(resolve, offset, speed, current,
                    &(*current->m_source.m_sequences)[current->m_nextSequence], this->m_scene);
            }
            if (state->m_sequence->m_endTime - static_cast<int32_t>(this->m_scene->m_time) < 1) {
                return;
            }
            state->m_sequence->m_pending = 0;
            return;
        }
    }
    if (blendTime < 0x10) {
        this->DropBlend(state);
        this->DropSequence(state);
    } else {
        if (from != -1) {
            if (useBlend) {
                this->DropSequence(state);
            } else {
                this->BlendFromCurrent(state);
            }
        }
        int32_t end = static_cast<int32_t>(this->m_scene->m_time + blendTime);
        state->m_blendEnd = end;
        if ((~(this->m_scene->m_flags >> 2) & 1) != 0) {
            state->m_blendEnd = end + 1;
        }
        state->m_blendMax = 1.0f;
        state->m_blendRate = 1.0f / static_cast<float>(blendTime);
    }
    M2SequenceTime* slot = M2SequenceTimeAlloc();
    state->m_flags |= 0x2000;
    state->m_sequence = slot;
    this->StartSlot(index, resolve, offset, speed, slot, repeats, sequences);
    slot = state->m_sequence;
    state->m_blendKind = kind;
    state->m_fixedBlend = fixedBlend;
    state->m_weight = weight;
    if (slot->m_startTime == slot->m_endTime
        || ((sequence->m_flags & 1) != 0 && slot->m_endTime - static_cast<int32_t>(this->m_scene->m_time) < 1)) {
        slot->m_pending = 1;
    }
    if (state->m_ownerLink == nullptr) {
        state->m_ownerLink = ownerHead;
        uint16_t head = *ownerHead;
        state->m_ownerNext = head;
        if (head != 0xffff) {
            states[head].m_ownerLink = &state->m_ownerNext;
        }
        *ownerHead = bone;
    }
    M2BoneStatesPropagateSlots(states, bones);
    if (this->m_callback420 != nullptr) {
        uint32_t args[9];
        const M2CompBone& def = (*bones)[bone];
        args[0] = def.m_parentBone == -1 ? 0xffffffff : static_cast<uint32_t>(def.m_keyBoneId);
        args[1] = state->m_5c;
        args[2] = state->m_variation;
        args[3] = offset;
        std::memcpy(&args[4], this->m_callback420Closure, sizeof(uint32_t) * 4);
        args[8] = 1;
        reinterpret_cast<void (*)(CM2Model*, void*)>(this->m_callback420)(this, args);
    }
    if (bone == 0 && data->m_ribbons.Count() == 0 && data->m_particles.Count() == 0) {
        uint32_t empty = 0x2000;
        const CAaBox& box = sequence->m_bounds.m_extent;
        if (box.m_min.x <= box.m_max.x || box.m_min.y <= box.m_max.y || box.m_min.z <= box.m_max.z) {
            empty = 0;
        }
        this->m_flags = (this->m_flags & ~0x2000u) | empty;
    }
}

// ref: FUN_1411cc860
void CM2Model::StartBlendSequence(uint16_t index, uint16_t bone, const M2SequenceResolve* resolve, uint32_t offset,
    float speed, int32_t repeats) {
    M2AnimState* anim = this->m_animState;
    M2BoneState* states = anim->m_boneStates;
    M2Array<M2Sequence>* sequences = resolve->m_parentHas == 1 ? anim->m_parentSequences : anim->m_sequences;
    M2Array<M2CompBone>* bones = anim->m_bones;
    M2BoneState* state = &states[bone];
    uint32_t duration = (*sequences)[index].m_duration;
    state->m_blendEnd = static_cast<int32_t>(this->m_scene->m_time + duration);
    float rate = 1.0f;
    if (duration != 0) {
        rate = 1.0f / static_cast<float>(duration);
    }
    state->m_blendRate = rate;
    state->m_blendMax = 0.75f;
    this->DropBlend(state);
    M2SequenceTime* slot = M2SequenceTimeAlloc();
    state->m_flags |= 0x4000;
    state->m_blendSequence = slot;
    this->StartSlot(index, resolve, offset, speed, slot, repeats, sequences);
    M2BoneStatesPropagateSlots(states, bones);
}

// ref: FUN_1411ca300
void CM2Model::SetSequence(uint32_t keyBone, uint32_t id, uint32_t variation, int32_t offset, float speed,
    int32_t primary, int32_t repeats, int32_t kind, int32_t fixedBlend, int32_t check, float weight) {
    if (static_cast<int32_t>(id) == -1) {
        this->SetBoneSequenceNone(keyBone, static_cast<uint8_t>(kind), primary, 0, 0, 0x96);
        return;
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call == nullptr) {
            return;
        }
        call->m_type = 6;
        call->m_next = nullptr;
        call->m_frame = this->m_scene->m_time;
        call->m_args[0] = keyBone;
        call->m_args[1] = id;
        call->m_args[2] = variation;
        call->m_args[3] = static_cast<uint32_t>(offset);
        call->m_args[5] = static_cast<uint32_t>(primary);
        call->m_args[6] = static_cast<uint32_t>(repeats);
        *reinterpret_cast<uint8_t*>(&call->m_args[7]) = static_cast<uint8_t>(kind);
        std::memcpy(&call->m_args[4], &speed, sizeof(float));
        *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(&call->m_args[7]) + 2) = static_cast<uint16_t>(fixedBlend);
        call->m_args[8] = static_cast<uint32_t>(check);
        std::memcpy(&call->m_args[9], &weight, sizeof(float));
        *this->m_callsTail = call;
        this->m_callsTail = &call->m_next;
        return;
    }
    uint8_t blendKind = (this->m_modelFlags & 0x10) == 0 ? static_cast<uint8_t>(kind) : 5;
    M2Data* data = this->m_shared->m_data;
    if (check != 0 && !M2AnimStateHasSequence(id, this->m_animState)) {
        return;
    }
    M2AnimState* anim = this->m_animState;
    bool endless = false;
    uint16_t bone = 0;
    M2BoneState* states = anim->m_boneStates;
    M2Array<M2Sequence>* sequences = anim->m_sequences;
    M2Array<uint16_t>* hash = anim->m_sequenceHash;
    if (keyBone != 0xffffffff) {
        bone = keyBone < anim->m_keyBoneLookup->Count() ? (*anim->m_keyBoneLookup)[keyBone] : 0xffff;
    }
    if (anim->m_bones->Count() <= bone) {
        return;
    }
    M2SequenceResolve resolve;
    M2ResolveSequence(id, &resolve, anim, sequences, hash, static_cast<M2Array<uint16_t>*>(this->m_shared->m_blocklistAnimData));
    if (resolve.m_parentHas == 1) {
        sequences = this->m_animState->m_parentSequences;
        hash = this->m_animState->m_parentHash;
    }
    uint16_t index;
    bool search = false;
    if (resolve.m_blend == 0) {
        endless = variation == 0xffffffff;
        search = variation < 0xfffffffe;
    } else {
        variation = 0;
        search = true;
    }
    index = 0xffff;
    if (search) {
        index = M2FindSequenceVariation(resolve.m_sequence, static_cast<int32_t>(variation), sequences, hash);
    }
    if (index == 0xffff) {
        index = M2FindSequenceById(resolve.m_sequence, hash, sequences);
        if (sequences->Count() <= index) {
            return;
        }
        int32_t picked;
        M2PickVariation(&picked, &index, sequences);
        variation = static_cast<uint32_t>(picked);
    }
    if (((*sequences)[index].m_flags & 0x20) == 0) {
        this->QueuePlayback(index, data, bone, static_cast<uint32_t>(offset), speed, &resolve, primary, endless ? 1 : 0,
            static_cast<uint32_t>(repeats), blendKind, static_cast<uint16_t>(fixedBlend), *reinterpret_cast<uint32_t*>(&weight));
        return;
    }
    if (!this->AllowSequenceChange(keyBone, bone)) {
        return;
    }
    this->CancelPlaybacks(bone, primary != 0);
    if ((this->m_modelFlags2 & 0x800) != 0 || states == nullptr) {
        return;
    }
    std::memset(this->m_boneDone, 0, this->m_boneDoneBytes);
    M2SequenceTime* target;
    if (primary == 0) {
        this->StartBlendSequence(index, bone, &resolve, static_cast<uint32_t>(offset), speed, repeats);
        target = states[bone].m_blendSequence;
    } else {
        states[bone].m_5c = id;
        states[bone].m_variation = static_cast<uint16_t>(variation);
        this->StartSequence(index, bone, &resolve, static_cast<uint32_t>(offset), speed, repeats, blendKind,
            static_cast<uint16_t>(fixedBlend), weight);
        target = states[bone].m_sequence;
    }
    target->m_endless = endless ? 1 : 0;
}

// A bone's matrix set from outside by key bone (FUN_1411ca130).
// ref: FUN_1411ca130
void CM2Model::SetKeyBoneMatrix(uint32_t keyBone, const C44Matrix* matrix) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call != nullptr) {
            call->m_next = nullptr;
            call->m_type = 10;
            call->m_frame = this->m_scene->m_time;
            call->m_args[0] = keyBone;
            std::memcpy(&call->m_args[1], matrix, sizeof(C44Matrix));
            *this->m_callsTail = call;
            this->m_callsTail = &call->m_next;
        }
        return;
    }
    M2AnimState* anim = this->m_animState;
    uint32_t bone = 0;
    if (keyBone != 0xffffffff) {
        bone = keyBone < anim->m_keyBoneLookup->Count() ? (*anim->m_keyBoneLookup)[keyBone] : 0xffff;
    }
    if (bone < anim->m_bones->Count()) {
        M2BoneState* state = &anim->m_boneStates[bone];
        if (state->m_override == nullptr) {
            state->m_override = static_cast<C44Matrix*>(AlignedModelAlloc(sizeof(C44Matrix), 0x1e90));
        }
        *state->m_override = *matrix;
    }
}

// ref: FUN_1411ca270
void CM2Model::SetBoneMatrix(uint32_t bone, const C44Matrix* matrix) {
    if ((this->m_flags & BASE_LOADED) == 0 || this->m_animState->m_bones->Count() <= bone) {
        return;
    }
    M2BoneState* state = &this->m_animState->m_boneStates[bone];
    if (state->m_override == nullptr) {
        state->m_override = static_cast<C44Matrix*>(AlignedModelAlloc(sizeof(C44Matrix), 0x1eaa));
    }
    *state->m_override = *matrix;
}

// The .anim files of a sequence and its variations asked for ahead of playing them
// (FUN_1411c6320).
// ref: FUN_1411c6320
void CM2Model::PreloadSequence(uint32_t id) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call != nullptr) {
            call->m_next = nullptr;
            call->m_type = 0xf;
            call->m_frame = this->m_scene->m_time;
            call->m_args[0] = id;
            *this->m_callsTail = call;
            this->m_callsTail = &call->m_next;
        }
        return;
    }
    M2AnimState* anim = this->m_animState;
    M2Array<M2Sequence>* sequences = anim->m_sequences;
    M2Array<uint16_t>* hash = anim->m_sequenceHash;
    M2SequenceResolve resolve;
    M2ResolveSequence(id, &resolve, anim, sequences, hash, static_cast<M2Array<uint16_t>*>(this->m_shared->m_blocklistAnimData));
    if (resolve.m_parentHas == 1) {
        sequences = this->m_animState->m_parentSequences;
        hash = this->m_animState->m_parentHash;
    }
    for (uint16_t index = M2FindSequenceById(resolve.m_sequence, hash, sequences); index < sequences->Count();
         index = static_cast<uint16_t>((*sequences)[index].m_variationNext)) {
        if (((*sequences)[index].m_flags & 0x130) == 0) {
            if (this->m_skeleton == nullptr) {
                M2SharedRequestSequence(this->m_shared, index);
            } else {
                M2SkeletonRequestSequence(this->m_skeleton, index, resolve.m_parentHas != 0);
            }
        }
    }
}
