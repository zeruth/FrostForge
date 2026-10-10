#include "model2/M2Model.hpp"
#include "util/Unimplemented.hpp"

// Skeleton files (.skel, SKID; Engine/Source/Model2, 0x1411f6xxx..0x141204xxx): a model whose
// file names a skeleton animates its bones from the skeleton's sequences. The shared file does
// not read skeletons yet (M2Shared's SKID chunk is parsed but never loaded), so a model never
// has one and these are not reached.

// ref: FUN_141201f40
void M2SkeletonAnimateBone(void* skeleton, const M2CompBone* bone, M2BoneState* state, uint32_t index,
    M2AnimateContext* context, M2AnimState* anim) {
    (void)skeleton;
    (void)bone;
    (void)state;
    (void)index;
    (void)context;
    (void)anim;
    FROST_UNIMPLEMENTED();
}

void* M2SkeletonGetSource(void* skeleton, bool second) {
    auto fields = static_cast<void**>(skeleton);
    if (second && fields[0x40 / 8] != nullptr) {
        return fields[0x40 / 8];
    }
    return fields[0x38 / 8];
}

// ref: FUN_1411f8a30
void M2SkeletonReleaseSequence(void* source, uint32_t sequence) {
    (void)source;
    (void)sequence;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_141203330
void M2SkeletonSetFrame(void* skeleton, uint32_t frame, int32_t time) {
    (void)skeleton;
    (void)frame;
    (void)time;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1411f8a40
void M2SkeletonAddSequenceRef(void* source, uint32_t sequence) {
    (void)source;
    (void)sequence;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412041f0
void M2SkeletonRequestSequence(void* skeleton, uint32_t sequence, bool parent) {
    (void)skeleton;
    (void)sequence;
    (void)parent;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412034c0
void M2SkeletonCancelPlaybacks(void* skeleton, CM2Model* model, uint32_t bone, bool blend) {
    (void)skeleton;
    (void)model;
    (void)bone;
    (void)blend;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412045f0
void M2SkeletonQueuePlayback(void* skeleton, CM2Model* model, uint16_t sequence, M2AnimState* anim, uint16_t bone,
    uint32_t offset, float speed, const M2SequenceResolve* resolve, int32_t blend, int32_t endless,
    uint32_t repeats, uint8_t kind, uint16_t fixedBlend, uint32_t weight) {
    (void)skeleton;
    (void)model;
    (void)sequence;
    (void)anim;
    (void)bone;
    (void)offset;
    (void)speed;
    (void)resolve;
    (void)blend;
    (void)endless;
    (void)repeats;
    (void)kind;
    (void)fixedBlend;
    (void)weight;
    FROST_UNIMPLEMENTED();
}
