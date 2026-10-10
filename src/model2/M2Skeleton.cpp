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
