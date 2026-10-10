#include "model2/M2Model.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "base/ObjectAlloc.hpp"
#include "base/Matrix.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Model.cpp.

namespace {

const char* const s_modelFile = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Model.cpp";

// The pools (FUN_1411c5d90).
TObjectAllocHeap* s_accumBoundsPool;    // DAT_1430a0f40 (0x44)
TObjectAllocHeap* s_effectPool;         // DAT_1430a0f48 (0xb0)
TObjectAllocHeap* s_matrixPool;         // DAT_1430a0f50 (0x44)
TObjectAllocHeap* s_windPhasePool;      // DAT_1430a0f58 (0x10)
TObjectAllocHeap* s_boneSeqPool;        // DAT_1430a0f60 (0x68)

// DAT_142cfb0f0
uint32_t s_flag400;

TObjectAllocHeap* MakePool(const char* name, uint32_t size, uint32_t perHeap, int32_t line) {
    void* memory = SMemAlloc(sizeof(TObjectAllocHeap), 0x12, s_modelFile, 0, line, 0);
    return new (memory) TObjectAllocHeap(name, size, perHeap, 0x12);
}

}  // namespace

// ref: FUN_1411c62e0
void M2ModelLinkWaiting(CM2Model* model) {
    CM2Shared* shared = model->m_shared;
    model->m_waitPrev = shared->m_waitingTail;
    model->m_waitNext = nullptr;
    *shared->m_waitingTail = model;
    shared->m_waitingTail = &model->m_waitNext;
}

// ref: FUN_1411cdb30
void M2ModelUnlinkWaiting(CM2Model* model) {
    if (model->m_waitPrev == nullptr) {
        return;
    }
    *model->m_waitPrev = model->m_waitNext;
    if (model->m_waitNext != nullptr) {
        model->m_waitNext->m_waitPrev = model->m_waitPrev;
    } else {
        model->m_shared->m_waitingTail = model->m_waitPrev;
    }
    model->m_waitPrev = nullptr;
    model->m_waitNext = nullptr;
}

// ref: FUN_1411c24f0
bool M2ModelInitializeLoaded(CM2Model* model) {
    (void)model;
    FROST_UNIMPLEMENTED();
    return false;
}

// ref: FUN_1411c5d90
void M2ModelInitializePools() {
    s_accumBoundsPool = MakePool("M2 Accum Bounds Pool", 0x44, 0x10, 0x1b6);
    s_effectPool = MakePool("M2 Model Effect Pool", 0xb0, 0x40, 0x1b7);
    s_matrixPool = MakePool("M2 Model Matrix Pool", 0x44, 8, 0x1b8);
    s_windPhasePool = MakePool("M2 Model Wind Phase Pool", 0x10, 0x40, 0x1b9);
    s_boneSeqPool = MakePool("M2 Bone Seq Pool", 0x68, 0x400, 0x3d66);
}

TObjectAllocHeap* M2GetBoneSeqPool() {
    return s_boneSeqPool;
}

// ref: FUN_1411c69e0
M2ModelEffect* M2ModelEffectAlloc() {
    auto effect = static_cast<M2ModelEffect*>(s_effectPool->Alloc());
    std::memset(effect, 0, 0xa8);
    effect->m_textures[1] = nullptr;
    effect->m_textures[2] = nullptr;
    *reinterpret_cast<void**>(effect->m_98) = nullptr;
    effect->m_a0 = 1.0f;
    effect->m_a4 = 1.0f;
    return effect;
}

void M2ModelEffectFree(M2ModelEffect* effect) {
    s_effectPool->Free(effect);
}

// ref: FUN_1411b7d60
void M2ModelSetFlag400(bool set) {
    s_flag400 = set;
}

void M2CreateModuleScene() {
    FROST_UNIMPLEMENTED();  // FUN_141196e40: CM2Scene
}

// ref: FUN_1411ff490
void M2BoneStatesPropagateSlots(M2BoneState* states, const M2Array<M2CompBone>* bones) {
    uint32_t count = bones->Count();
    if (count == 0) {
        return;
    }
    uint32_t i = 0;
    do {
        const M2CompBone& bone = (*bones)[i];
        M2BoneState* state = &states[i];
        if (state->m_sequence == nullptr || (state->m_flags & 0x2000) == 0) {
            uint16_t parent = static_cast<uint16_t>(bone.m_parentBone);
            if (parent < count) {
                state->m_sequence = states[parent].m_sequence;
                state->m_5c = states[parent].m_5c;
            } else if (i != 0) {
                state->m_sequence = states[0].m_sequence;
                state->m_5c = states[0].m_5c;
            }
        }
        if (state->m_blendSequence == nullptr || (state->m_flags & 0x4000) == 0) {
            uint16_t parent = static_cast<uint16_t>(bone.m_parentBone);
            if (parent < bones->Count()) {
                state->m_blendSequence = states[parent].m_blendSequence;
            } else if (i != 0) {
                state->m_blendSequence = states[0].m_blendSequence;
            }
        }
        count = bones->Count();
        i++;
    } while (i < count);
}
