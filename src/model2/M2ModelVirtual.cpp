#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "cas/CAS.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Ribbon.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Model.cpp: CM2Model's virtual functions (vtable 0x142720d70).

namespace {

// DAT_14261f708, DAT_14261f70c: an empty box.
constexpr float BOX_EMPTY_MIN = 3.4028235e38f;
constexpr float BOX_EMPTY_MAX = -3.4028235e38f;

void SetEmptyBox(float* box) {
    box[0] = BOX_EMPTY_MIN;
    box[1] = BOX_EMPTY_MIN;
    box[3] = BOX_EMPTY_MAX;
    box[4] = BOX_EMPTY_MAX;
    box[5] = BOX_EMPTY_MAX;
    box[2] = BOX_EMPTY_MIN;
}

// A box with any extent: min <= max on some axis.
bool BoxHasExtent(const float* box) {
    return box[0] <= box[3] || box[1] <= box[4] || box[2] <= box[5];
}

HTEXTURE ModelTexture(const CM2Model* model, uint32_t index) {
    return static_cast<HTEXTURE>(model->m_textures[index]);
}

// The bounds of the sequence the root bone plays: the file's, or the parent skeleton's (PSBC)
// when the sequence is the parent's.
const float* RootSequenceBounds(const CM2Model* model) {
    M2SequenceTime* slot = model->m_animState->m_boneStates[0].m_sequence;
    const float* box = &(*slot->m_source.m_sequences)[slot->m_nextSequence].m_bounds.m_extent.m_min.x;
    auto parentBounds = static_cast<M2Array<uint8_t>*>(model->m_shared->m_parentSequenceBounds);
    if (parentBounds != nullptr && (slot->m_source.m_flags & 1) != 0) {
        box = reinterpret_cast<const float*>(&(*parentBounds)[0] + slot->m_nextSequence * 0x1c);
    }
    return box;
}

// The matrix the model's box is placed with: the view-space one when attached, else the local.
const float* PlacementMatrix(const CM2Model* model) {
    return model->m_parent == nullptr ? model->m_local.m : model->m_view.m;
}

// The union of two boxes into both `a` and `out` (FUN_1411cda20).
// ref: FUN_1411cda20
float* BoxUnion(float* a, float* out, const float* b) {
    float maxX = b[3] < a[3] ? a[3] : b[3];
    float maxY = b[4] < a[4] ? a[4] : b[4];
    float maxZ = b[5] < a[5] ? a[5] : b[5];
    float minX = b[0] < a[0] ? b[0] : a[0];
    float minY = b[1] < a[1] ? b[1] : a[1];
    float minZ = b[2] < a[2] ? b[2] : a[2];
    a[0] = minX;
    a[1] = minY;
    a[2] = minZ;
    a[3] = maxX;
    a[4] = maxY;
    a[5] = maxZ;
    out[0] = minX;
    out[1] = minY;
    out[2] = minZ;
    out[3] = maxX;
    out[4] = maxY;
    out[5] = maxZ;
    return out;
}

// `bone * world` in the reference's order of terms for the first row and the rest.
void BoneTimesWorld(const float* b, const float* w, float* out, bool rootOrder) {
    if (rootOrder) {
        // FUN_1411b98e0
        out[0] = b[3] * w[12] + b[2] * w[8] + b[1] * w[4] + b[0] * w[0];
        out[1] = b[3] * w[13] + b[2] * w[9] + b[1] * w[5] + b[0] * w[1];
        out[2] = b[3] * w[14] + b[2] * w[10] + b[1] * w[6] + b[0] * w[2];
        out[3] = b[3] * w[15] + b[2] * w[11] + b[1] * w[7] + b[0] * w[3];
        for (int32_t row = 1; row < 4; row++) {
            const float* r = b + row * 4;
            for (int32_t col = 0; col < 4; col++) {
                out[row * 4 + col] = r[3] * w[12 + col] + r[2] * w[8 + col] + r[0] * w[col] + r[1] * w[4 + col];
            }
        }
        return;
    }
    // FUN_1411ba010
    out[0] = b[2] * w[8] + b[3] * w[12] + b[1] * w[4] + b[0] * w[0];
    out[1] = b[2] * w[9] + b[3] * w[13] + b[1] * w[5] + b[0] * w[1];
    out[2] = b[2] * w[10] + b[3] * w[14] + b[1] * w[6] + b[0] * w[2];
    out[3] = b[2] * w[11] + b[3] * w[15] + b[1] * w[7] + b[0] * w[3];
    for (int32_t row = 1; row < 4; row++) {
        const float* r = b + row * 4;
        for (int32_t col = 0; col < 4; col++) {
            out[row * 4 + col] = r[0] * w[col] + r[1] * w[4 + col] + r[2] * w[8 + col] + r[3] * w[12 + col];
        }
    }
}

}  // namespace

// ref: FUN_1411cecd0
void CM2Model::WaitForLoad(int32_t a2) {
    (void)a2;
    this->m_shared->WaitForLoad();
}

// ref: FUN_1411b9730
float* CM2Model::GetBounds(float* out) {
    if ((this->m_modelFlags & 0x800000) != 0) {
        std::memcpy(out, &this->m_accumBounds->m_box, sizeof(CAaBox));
        return out;
    }
    this->GetAnimatedBox(out, true);
    return out;
}

// ref: FUN_1411c61f0
bool CM2Model::AreTexturesReady() {
    for (uint32_t i = 0; i < this->m_shared->m_data->m_textures.Count(); i++) {
        if (!TextureIsLoaded(ModelTexture(this, i))) {
            return false;
        }
    }
    this->m_flags |= BASE_TEXTURES_READY;
    return true;
}

// 2 when the model and its textures are in, 0 while something is still loading.
// ref: FUN_1411b9820
int32_t CM2Model::GetLoadState() {
    if ((this->m_flags & 0x20) != 0) {
        return 2;
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        return 0;
    }
    int32_t state = 2;
    if (this->m_shared->m_data->m_textures.Count() != 0) {
        for (uint32_t i = 0; i < this->m_shared->m_data->m_textures.Count(); i++) {
            if (this->m_textures[i] != nullptr) {
                int32_t texture = TextureLoadState(ModelTexture(this, i));
                if (state < texture) {
                    texture = state;
                }
                state = texture;
                if (state == 0) {
                    return 0;
                }
            }
        }
        if (state != 2) {
            return state;
        }
    }
    this->m_flags |= 0x20;
    return state;
}

// ref: FUN_1411ced60
void CM2Model::WaitForAll() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if ((this->m_flags & BASE_TEXTURES_READY) == 0) {
        for (uint32_t i = 0; i < this->m_shared->m_data->m_textures.Count(); i++) {
            if (this->m_textures[i] != nullptr) {
                TextureGetGxTexBlocking(ModelTexture(this, i));
            }
        }
        this->m_flags |= BASE_TEXTURES_READY;
    }
    if ((this->m_flags & BASE_ALL_READY) == 0) {
        for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
            child->WaitForAll();
        }
        this->m_flags |= BASE_ALL_READY;
    }
}

// ref: FUN_1411cece0
void CM2Model::WaitForTextures() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
        if ((this->m_flags & BASE_LOADED) == 0) {
            return;
        }
    }
    if ((this->m_flags & BASE_TEXTURES_READY) == 0) {
        for (uint32_t i = 0; i < this->m_shared->m_data->m_textures.Count(); i++) {
            if (this->m_textures[i] != nullptr) {
                TextureGetGxTexBlocking(ModelTexture(this, i));
            }
        }
        this->m_flags |= BASE_TEXTURES_READY;
    }
}

// ref: FUN_1411c9b60
bool CM2Model::SetPriority(int32_t priority, bool textures) {
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        if (!child->SetPriority(priority, textures)) {
            return false;
        }
    }
    if (!this->m_shared->SetPriority(priority, false)) {
        return false;
    }
    if ((this->m_flags & BASE_LOADED) != 0 && textures) {
        for (uint32_t i = 0; i < this->m_shared->m_data->m_textures.Count(); i++) {
            if (this->m_textures[i] != nullptr && !TextureSetPriority(ModelTexture(this, i), priority)) {
                return false;
            }
        }
    }
    return true;
}

// The sphere around the root sequence's bounds through the placement matrix, merged with the
// effects' when the file has particles or ribbons.
// ref: FUN_1411bbf90
float* CM2Model::GetAnimatedSphere(float* out) {
    if ((this->m_flags & (BASE_LOADED | BASE_VISIBLE)) != (BASE_LOADED | BASE_VISIBLE)) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 0.0f;
        return out;
    }
    if ((this->m_modelFlags2 & 0x800) != 0) {
        float parent[4];
        this->m_parent->GetAnimatedSphere(parent);
        float pad = this->m_shared->m_data->m_boundsRadius;
        if (pad == 0.0f) {
            pad = 1.0f;  // DAT_1426c7afc
        }
        out[0] = parent[0];
        out[1] = parent[1];
        out[2] = parent[2];
        out[3] = parent[3] + pad;
        return out;
    }
    const float* bounds = RootSequenceBounds(this);
    float spheres[8] = {};
    const float* m = PlacementMatrix(this);
    float m0 = m[0];
    float m1 = m[1];
    float m2 = m[2];
    if (2.3841858e-07f <= std::fabs(bounds[6])) {  // DAT_14261a954
        float cy = (bounds[4] + bounds[1]) * 0.5f;
        float cx = (bounds[0] + bounds[3]) * 0.5f;
        float cz = (bounds[5] + bounds[2]) * 0.5f;
        spheres[2] = cy * m[6] + cx * m2 + cz * m[10] + m[14];
        spheres[1] = cy * m[5] + cx * m1 + cz * m[9] + m[13];
        spheres[0] = cy * m[4] + cx * m0 + cz * m[8] + m[12];
    }
    spheres[3] = std::sqrt(m0 * m0 + m1 * m1 + m2 * m2) * bounds[6];
    M2Data* data = this->m_shared->m_data;
    if (data->m_particles.Count() != 0 || data->m_ribbons.Count() != 0) {
        float effects[4];
        this->GetEffectSphere(effects);
        spheres[4] = effects[0];
        spheres[5] = effects[1];
        spheres[6] = effects[2];
        spheres[7] = effects[3];
        if (std::fabs(spheres[3]) < 2.3841858e-07f) {
            std::memcpy(out, effects, sizeof(effects));
            return out;
        }
        if (2.3841858e-07f <= std::fabs(effects[3])) {
            SphereFromSpheres(out, spheres, 2);
            return out;
        }
    }
    std::memcpy(out, spheres, sizeof(float) * 4);
    return out;
}

// The root sequence's box through the placement matrix, with the effects' box when asked.
// ref: FUN_1411bbb80
float* CM2Model::GetAnimatedBox(float* out, bool effects) {
    if ((this->m_flags & (BASE_LOADED | BASE_VISIBLE)) != (BASE_LOADED | BASE_VISIBLE)) {
        SetEmptyBox(out);
        return out;
    }
    if ((this->m_modelFlags2 & 0x800) != 0) {
        float parent[6];
        this->m_parent->GetAnimatedBox(parent, true);
        float pad = this->m_shared->m_data->m_boundsRadius;
        if (pad == 0.0f) {
            pad = 1.0f;  // DAT_1426c7afc
        }
        out[0] = parent[0] - pad;
        out[1] = parent[1] - pad;
        out[2] = parent[2] - pad;
        out[3] = parent[3] + pad;
        out[4] = parent[4] + pad;
        out[5] = parent[5] + pad;
        return out;
    }
    const float* bounds = RootSequenceBounds(this);
    float box[6];
    SetEmptyBox(box);
    if (effects) {
        float effectBox[6];
        this->GetEffectBox(effectBox);
        std::memcpy(box, effectBox, sizeof(box));
    }
    float placed[6];
    const float* sequenceBox;
    if (BoxHasExtent(bounds)) {
        sequenceBox = BoxTransform(placed, bounds, PlacementMatrix(this));
    } else {
        std::memcpy(placed, bounds, sizeof(placed));
        sequenceBox = placed;
    }
    float merged[6];
    BoxUnion(box, merged, sequenceBox);
    std::memcpy(out, box, sizeof(box));
    return out;
}

// The effects' box: the file's own bounds placed, if it has particles, and each ribbon's.
// ref: FUN_1411bc710
float* CM2Model::GetEffectBox(float* out) {
    SetEmptyBox(out);
    M2Data* data = this->m_shared->m_data;
    if (data->m_particles.Count() != 0) {
        const float* bounds = &data->m_bounds.m_min.x;
        float placed[6];
        const float* box;
        if (BoxHasExtent(bounds)) {
            box = BoxTransform(placed, bounds, PlacementMatrix(this));
        } else {
            std::memcpy(placed, bounds, sizeof(placed));
            box = placed;
        }
        float merged[6];
        BoxUnion(out, merged, box);
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_ribbons.Count(); i++) {
        const float* ribbon = &this->m_ribbons[i]->m_bounds.m_min.x;
        float maxX = ribbon[3] < out[3] ? out[3] : ribbon[3];
        float maxY = ribbon[4] < out[4] ? out[4] : ribbon[4];
        float maxZ = ribbon[5] < out[5] ? out[5] : ribbon[5];
        float minX = ribbon[0] < out[0] ? ribbon[0] : out[0];
        float minY = ribbon[1] < out[1] ? ribbon[1] : out[1];
        float minZ = ribbon[2] < out[2] ? ribbon[2] : out[2];
        out[0] = minX;
        out[1] = minY;
        out[2] = minZ;
        out[3] = maxX;
        out[4] = maxY;
        out[5] = maxZ;
    }
    return out;
}

// The effects' sphere: the file's bounds sphere placed, if it has particles, and the sphere
// around the ribbons' boxes.
// ref: FUN_1411bc940
float* CM2Model::GetEffectSphere(float* out) {
    float spheres[8] = {};
    uint32_t count = 0;
    M2Data* data = this->m_shared->m_data;
    if (data->m_particles.Count() != 0) {
        const float* m = PlacementMatrix(this);
        float cy = (data->m_bounds.m_max.y + data->m_bounds.m_min.y) * 0.5f;
        float cx = (data->m_bounds.m_min.x + data->m_bounds.m_max.x) * 0.5f;
        float cz = (data->m_bounds.m_max.z + data->m_bounds.m_min.z) * 0.5f;
        spheres[1] = cy * m[5] + cx * m[1] + cz * m[9] + m[13];
        spheres[0] = cy * m[4] + cx * m[0] + cz * m[8] + m[12];
        spheres[3] = std::sqrt(m[1] * m[1] + m[0] * m[0] + m[2] * m[2]) * data->m_boundsRadius;
        spheres[2] = cy * m[6] + cx * m[2] + cz * m[10] + m[14];
        count = 1;
    }
    float box[6];
    SetEmptyBox(box);
    for (uint32_t i = 0; i < data->m_ribbons.Count(); i++) {
        const float* ribbon = &this->m_ribbons[i]->m_bounds.m_min.x;
        if (box[3] <= ribbon[3]) {
            box[3] = ribbon[3];
        }
        if (box[4] <= ribbon[4]) {
            box[4] = ribbon[4];
        }
        if (box[5] <= ribbon[5]) {
            box[5] = ribbon[5];
        }
        if (ribbon[0] < box[0]) {
            box[0] = ribbon[0];
        }
        if (ribbon[1] < box[1]) {
            box[1] = ribbon[1];
        }
        if (ribbon[2] < box[2]) {
            box[2] = ribbon[2];
        }
    }
    if (box[0] < box[3] && box[1] < box[4] && box[2] < box[5]) {
        float* sphere = spheres + count * 4;
        sphere[0] = (box[3] + box[0]) * 0.5f;
        sphere[1] = (box[1] + box[4]) * 0.5f;
        sphere[2] = (box[2] + box[5]) * 0.5f;
        sphere[3] = std::sqrt((box[4] - box[1]) * (box[4] - box[1]) + (box[3] - box[0]) * (box[3] - box[0])
                        + (box[5] - box[2]) * (box[5] - box[2]))
            * 0.5f;
        count++;
    }
    SphereFromSpheres(out, spheres, count);
    return out;
}

// ref: FUN_1411bbdf0
float* CM2Model::GetBoundingBox(float* out) {
    if ((this->m_flags & (BASE_LOADED | BASE_VISIBLE)) != (BASE_LOADED | BASE_VISIBLE)) {
        SetEmptyBox(out);
        return out;
    }
    if ((this->m_modelFlags2 & 0x800) != 0) {
        float parent[6];
        this->m_parent->GetBoundingBox(parent);
        float pad = this->m_shared->m_data->m_boundsRadius;
        if (pad == 0.0f) {
            pad = 1.0f;  // DAT_1426c7afc
        }
        out[0] = parent[0] - pad;
        out[1] = parent[1] - pad;
        out[2] = parent[2] - pad;
        out[3] = parent[3] + pad;
        out[4] = parent[4] + pad;
        out[5] = parent[5] + pad;
        return out;
    }
    std::memcpy(out, RootSequenceBounds(this), sizeof(float) * 6);
    return out;
}

// The root sequence's bounds radius, or the largest of the children's.
// ref: FUN_1411c0320
float CM2Model::GetBoundsRadius() {
    float radius = 0.0f;
    if ((this->m_flags & (BASE_LOADED | BASE_FROM_DONOR)) == (BASE_LOADED | BASE_FROM_DONOR)) {
        M2SequenceTime* slot = this->m_animState->m_boneStates[0].m_sequence;
        radius = (*slot->m_source.m_sequences)[slot->m_nextSequence].m_bounds.m_radius;
        if ((this->m_modelFlags2 & 0x800) != 0) {
            return 0.0f;
        }
        auto parentBounds = static_cast<M2Array<uint8_t>*>(this->m_shared->m_parentSequenceBounds);
        if (parentBounds != nullptr && (slot->m_source.m_flags & 1) != 0) {
            radius = *reinterpret_cast<const float*>(&(*parentBounds)[0] + slot->m_nextSequence * 0x1c + 0x18);
        }
        if (radius <= 0.0f) {
            radius = 0.0f;
        }
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        if ((child->m_flags & BASE_LOADED) != 0
            && (child->m_type != 0 || (static_cast<CM2Model*>(child)->m_modelFlags2 & 0x800) == 0)
            && radius <= child->GetBoundsRadius()) {
            radius = child->GetBoundsRadius();
        }
    }
    return radius;
}

// The children attached at `id` detach; id 0x37 falls back to 0xb on a file without it.
// ref: FUN_1411b7040
void CM2Model::DetachChildrenAt(int32_t id) {
    if ((this->m_flags & BASE_LOADED) != 0 && id == 0x37) {
        M2Array<uint16_t>* lookup = this->m_animState->m_attachmentLookup;
        uint16_t index = lookup->Count() < 0x38 ? 0xffff : (*lookup)[0x37];
        if (this->m_animState->m_attachments->Count() <= index) {
            id = 0xb;
        }
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr;) {
        CM2BaseModel* next = child->m_siblingNext;
        if (child->m_attachId == id) {
            child->Detach(true);
        }
        child = next;
    }
}

// ref: FUN_1411c7bb0
void CM2Model::MarkRayTracingDirty() {
    if (this->m_scene == nullptr) {
        return;
    }
    void* rayTracing = M2SceneGetRayTracing(this->m_scene);
    if (rayTracing == nullptr || this->m_rtEntry[1] != 2) {
        return;
    }
    RayTracingMarkDirty(rayTracing, this->m_rtEntry);
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->MarkRayTracingDirty();
    }
}

// The effects follow an attachment matrix set from outside; the children attached to points
// take it too.
// ref: FUN_1411b60b0
void CM2Model::SetAttachMatrix(C44Matrix* matrix) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        return;
    }
    C44Matrix* previous = this->m_558;
    if (matrix == nullptr) {
        if (previous != nullptr) {
            this->SetEffectMatrix(nullptr);
        }
        this->m_558 = nullptr;
    } else {
        this->m_558 = matrix;
        if (previous == nullptr) {
            C44Matrix inverse;
            MatrixRigidInverse(matrix->m, inverse.m);
            this->SetEffectMatrix(&inverse);
        }
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        if (child->m_attachIndex != 0xffff) {
            child->SetAttachMatrix(matrix);
        }
    }
}

// ref: FUN_1411b5fe0
void CM2Model::SetEffectMatrix(const C44Matrix* matrix) {
    M2Data* data = this->m_shared->m_data;
    for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
        ParticleEmitterSetParentMatrix(this->m_particles[i], matrix);
    }
    for (uint32_t i = 0; i < data->m_ribbons.Count(); i++) {
        RibbonEmitterSetParentMatrix(this->m_ribbons[i], matrix);
    }
    if (this->m_effectMatrixCallback != nullptr) {
        this->m_effectMatrixCallback(matrix, &this->m_effectMatrixParam);
    }
}

// ref: FUN_1411b77d0
void CM2Model::DisableLights() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call != nullptr) {
            call->m_type = 0x14;
            call->m_next = nullptr;
            call->m_frame = this->m_scene->m_time;
            *this->m_callsTail = call;
            this->m_callsTail = &call->m_next;
        }
        return;
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_lights.Count(); i++) {
        this->m_lights[i].m_64 = 0;
    }
}

// ref: FUN_1411b8e80
void CM2Model::CollectEvents(CM2Scene* scene, void* callbacks) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        return;
    }
    if (this->m_pauseRequest == 0 && (this->m_modelFlags & 0x800) != 0 && !this->QueueEvents(scene, callbacks)) {
        return;
    }
    M2TrackState<uint8_t>* attachments = this->m_animState->m_attachmentStates;
    for (CM2BaseModel* child = this->m_children; child != nullptr;) {
        CM2BaseModel* next = child->m_siblingNext;
        bool shown;
        if (child->m_attachIndex == 0xffff) {
            shown = (child->m_flags >> 24 & 1) != 0;
        } else {
            shown = attachments[child->m_attachIndex].m_value != 0;
        }
        if (shown) {
            child->CollectEvents(scene, callbacks);
        }
        child = next;
    }
}

// ref: FUN_1411b8670
bool CM2Model::QueueEvents(CM2Scene* scene, void* callbacks) {
    (void)scene;
    (void)callbacks;
    FROST_UNIMPLEMENTED();  // the animation events (FUN_1411b8f30) into M2ModelCallbacks
    return true;
}

// ref: FUN_1411bd840
int32_t CM2Model::GetFileDataId() {
    return this->m_shared->m_fileDataId;
}

// ref: FUN_1411bbb60
bool CM2Model::GetContentKey(uint8_t* key) {
    CM2Shared* shared = this->m_shared;
    if (shared == nullptr) {
        return false;
    }
    if (shared->m_fileDataId != 0) {
        return cas::CASFileGetContentKeyById(shared->m_fileDataId, key);
    }
    return cas::CASFileGetContentKeyByName(shared->m_name, key, (shared->m_flags >> 13) & 0x20);
}

// ref: FUN_1411b6590
void CM2Model::CountFiles(int64_t* loaded, int64_t* total) {
    (void)loaded;
    (void)total;
    FROST_UNIMPLEMENTED();  // FUN_1402cce50 and FUN_14127e5b0: the files' resident sizes
}

// ref: FUN_1411ce0d0
bool CM2Model::AnimateAttachment(int32_t index, bool none) {
    if (!none) {
        M2Attachment& attachment = (*this->m_animState->m_attachments)[static_cast<uint32_t>(index)];
        return this->AnimateBoneFor(attachment.m_bone);
    }
    return this->AnimateBoneFor(0);
}

// One bone animated now, with its parents, for something attached to it; the model's
// transform is brought up first if this frame has not done it (FUN_1411ae3a0).
// ref: FUN_1411ae3a0
bool CM2Model::AnimateBoneFor(uint32_t bone) {
    CM2Scene* scene = this->m_scene;
    if ((this->m_flags & BASE_LOADED) == 0) {
        if (this->m_matrixFrame == scene->m_frame) {
            return false;
        }
    } else {
        if (this->m_matrixFrame != scene->m_frame) {
            std::memset(this->m_boneDone, 0, this->m_boneDoneBytes);
        }
        if ((this->m_boneDone[bone >> 3] & static_cast<uint8_t>(1 << (bone & 7))) != 0) {
            return false;
        }
    }
    bool animated = false;
    C44Matrix parent;
    if (this->m_parent == nullptr || (this->m_flags & BASE_DETACHED_MATRIX) != 0) {
        parent = scene->m_camera;
    } else {
        bool none = this->m_attachIndex == 0xffff;
        int32_t index = none ? 0 : static_cast<int32_t>(this->m_attachIndex);
        animated = this->m_parent->AnimateAttachment(index, none);
        this->m_parent->GetAttachmentWorld(index, none, &parent);
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->SetWorldMatrix(&parent);
        this->m_matrixFrame = this->m_scene->m_frame;
        return animated;
    }
    uint32_t frame = this->m_scene->m_frame;
    if (this->m_matrixFrame == frame) {
        this->SetWorldMatrix(&parent);
    } else if (this->m_parent == nullptr) {
        CM2ModelParams params;
        params.m_flags = 0;
        params.m_alpha = 1.0f;
        params.m_effect = nullptr;
        params.m_matrix = nullptr;
        params.m_colorAlpha = 1.0f;
        params.m_color = C3Vector{1.0f, 1.0f, 1.0f};  // DAT_142721230
        params.m_colorAdd.x = 0.0f;
        params.m_fade = 1.0f;
        params.m_colorAdd.y = 0.0f;
        params.m_colorAdd.z = 0.0f;
        // FUN_1411dd640
        params.m_effect = this->m_effect2;
        int32_t time = static_cast<int32_t>(this->m_scene->m_time);
        this->UpdateTransform(&this->m_scene->m_camera, &params, time);
        if (this->m_skeleton != nullptr) {
            M2SkeletonSetFrame(this->m_skeleton, frame, time);
        }
        this->m_matrixFrame = frame;
        animated = true;
    } else {
        this->UpdateFrameTransform(&parent, &this->m_parent->m_params, frame,
            static_cast<int32_t>(this->m_scene->m_time));
        animated = true;
    }
    if ((this->m_modelFlags & 0x40000000) == 0) {
        M2AnimateContext context;
        M2AnimateContextInit(&context, this->m_scene->m_time, this->m_scene->m_frame, 1, 0, 0, 0);
        this->AnimateBoneTree(bone, &context);
    } else {
        this->AnimateRootOnly(&this->m_animState->m_boneStates[bone]);
    }
    return animated;
}

// The transform for a new frame without clearing the bone-done bits (FUN_1411ce120).
// ref: FUN_1411ce120
void CM2Model::UpdateFrameTransform(const C44Matrix* parent, const CM2ModelParams* params, uint32_t frame,
    int32_t time) {
    this->UpdateTransform(parent, params, time);
    if (this->m_skeleton != nullptr) {
        M2SkeletonSetFrame(this->m_skeleton, frame, time);
    }
    this->m_matrixFrame = frame;
}

// ref: FUN_1411b98e0
void CM2Model::GetAttachmentWorld(int32_t index, bool none, C44Matrix* out) {
    if (!none) {
        this->GetAttachmentMatrix(static_cast<uint32_t>(index), out);
        return;
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        *out = this->m_world;
        return;
    }
    BoneTimesWorld(this->m_animState->m_boneMatrices[0].m, this->m_world.m, out->m, true);
}

// ref: FUN_1411ba010
void CM2Model::GetAttachmentMatrix(uint32_t index, C44Matrix* out) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        *out = this->m_world;
        return;
    }
    const M2Attachment& attachment = (*this->m_animState->m_attachments)[index];
    C3Vector position = attachment.m_position;
    BoneTimesWorld(this->m_animState->m_boneMatrices[attachment.m_bone].m, this->m_world.m, out->m, false);
    MatrixTranslate(out->m, &position.x);
}

// ref: FUN_1411c0480
CM2Shared* CM2Model::GetShared() {
    return this->m_shared;
}
