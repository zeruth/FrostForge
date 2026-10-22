#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "cas/CAS.hpp"
#include "gx/GxShader.hpp"
#include "model2/M2Camera.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Ribbon.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

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
        const float* ribbon = this->m_ribbons[i]->m_strip.m_drawBounds;
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
        const float* ribbon = this->m_ribbons[i]->m_strip.m_drawBounds;
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
void CM2Model::CollectEvents(M2SceneDrawList* list, void* callbacks) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        return;
    }
    if (this->m_pauseRequest == 0 && (this->m_modelFlags & 0x800) != 0 && !this->QueueEvents(list, callbacks)) {
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
            child->CollectEvents(list, callbacks);
        }
        child = next;
    }
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

// The files this model waits on, loaded against total: its own, its textures', its attached
// models'. The reference's own, translated (CM2Model keeps its layout, the static_asserts). As a
// stub the totals stayed 0, the character select screen's load progress (FUN_1416a8ad0,
// loaded / total) stayed 0.0, and the selected character was never shown (2026-10-22).
extern "C" unsigned long long FUN_1411b6590(unsigned long long, unsigned long long, unsigned long long);
// translated: FUN_1411b6590
void CM2Model::CountFiles(int64_t* loaded, int64_t* total) {
    __try {
        FUN_1411b6590(reinterpret_cast<unsigned long long>(this), reinterpret_cast<unsigned long long>(loaded),
            reinterpret_cast<unsigned long long>(total));
    } __except (GenFault(0x1411b6590ull, GetExceptionInformation())) {
    }
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

namespace {

// `a * b` with every row summed as r2, r3, r1, r0 (FUN_1411ce880's view matrix).
void MultiplyViewOrder(const float* a, const float* b, float* out) {
    for (int32_t row = 0; row < 4; row++) {
        const float* r = a + row * 4;
        for (int32_t col = 0; col < 4; col++) {
            out[row * 4 + col] = r[2] * b[8 + col] + r[3] * b[12 + col] + r[1] * b[4 + col] + r[0] * b[col];
        }
    }
}

}  // namespace

// ref: FUN_1411b70d0
void CM2Model::Detach(bool notify) {
    CM2BaseModel* notified = nullptr;
    if ((this->m_flags & BASE_LOADED) != 0) {
        M2Data* data = this->m_shared->m_data;
        if ((data->m_flags & 0x100) != 0) {
            for (uint32_t i = 0; i < this->m_shared->m_data->m_particles.Count(); i++) {
                if (this->m_shared->m_data->m_particles[i].m_emitterType == 4) {
                    ParticleGeneratorSetModel(this->m_particles[i]->m_generator, nullptr);
                }
            }
        }
    }
    if (this->m_siblingLink != nullptr) {
        *this->m_siblingLink = this->m_siblingNext;
    }
    if (this->m_siblingNext != nullptr) {
        this->m_siblingNext->m_siblingLink = this->m_siblingLink;
    }
    if (this->m_attachedLink != nullptr) {
        *this->m_attachedLink = this->m_attachedNext;
    }
    if (this->m_attachedNext != nullptr) {
        this->m_attachedNext->m_attachedLink = this->m_attachedLink;
    }
    this->m_lightingUsed = &this->m_lighting;
    C3Vector position;
    this->GetRenderPosition(&position);
    float origin[4] = {position.x, position.y, position.z, 0.0f};
    M2ModelLightingBegin(&this->m_lighting, this->m_scene, origin);
    if (notify && !this->AreChildrenLoaded()) {
        notified = this->m_parent;
    }
    this->m_siblingLink = nullptr;
    this->m_siblingNext = nullptr;
    if (this->m_params.m_effect == M2BaseModelGetEffect(this->m_parent)) {
        this->m_params.m_effect = this->m_effect2;
    }
    this->m_flags &= ~0x1000000u;
    this->m_parent = nullptr;
    this->m_parentModel = nullptr;
    this->m_attachId = -1;
    this->SetAttachMatrix(nullptr);
    this->m_flags &= ~BASE_VISIBLE;
    if (notified != nullptr) {
        notified->RunLoadCallbacks();
    }
    this->Release(1);
}

// The model takes the nearest CM2Model above it as the one it is attached under.
// ref: FUN_1411c77b0
void CM2Model::OnChildrenChanged() {
    if (this->m_parentModel != nullptr) {
        if (this->m_params.m_effect == M2BaseModelGetEffect(this->m_parent)) {
            this->m_params.m_effect = this->m_effect2;
        }
        CM2BaseModel* above = this->m_parent;
        while (above != nullptr && above->m_type != 0) {
            above = above->m_parent;
        }
        CM2Model* model = above != nullptr && above->m_type == 0 ? static_cast<CM2Model*>(above) : nullptr;
        if (model == nullptr) {
            if (this->m_parentModel != nullptr) {
                if (this->m_attachedLink != nullptr) {
                    *this->m_attachedLink = this->m_attachedNext;
                }
                if (this->m_attachedNext != nullptr) {
                    this->m_attachedNext->m_attachedLink = this->m_attachedLink;
                }
                this->m_parentModel = nullptr;
            }
        } else if (model != this->m_parentModel) {
            if (this->m_attachedLink != nullptr) {
                *this->m_attachedLink = this->m_attachedNext;
            }
            if (this->m_attachedNext != nullptr) {
                this->m_attachedNext->m_attachedLink = this->m_attachedLink;
            }
            this->m_parentModel = model;
            this->m_attachedLink = &model->m_attachedModels;
            this->m_attachedNext = model->m_attachedModels;
        }
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->OnChildrenChanged();
    }
}

// The frame's matrices: visibility from the parent, a sequence copied from the model this one
// mirrors, then the animation (or only the root and texture animation for a static model).
// ref: FUN_1411ce300
void CM2Model::UpdateMatrices(const C44Matrix* parent, const CM2ModelParams* params, uint32_t a4, uint32_t a5,
    uint32_t a6) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        return;
    }
    // FUN_14025cde0/FUN_14025ce40 push and pop this model on a thread-local list for crash reports.
    if (this->m_parent == nullptr) {
        this->m_flags &= ~0x140000u;
    } else {
        bool shown = this->IsShownInTree();
        uint32_t flags = this->m_flags;
        uint32_t visible = shown && (flags & BASE_ATTACH_SHOWN) != 0 ? BASE_SHOWN : 0;
        CM2BaseModel* above = this->m_parent;
        this->m_flags = (flags & ~BASE_SHOWN) | visible;
        uint32_t inherit40000 = (above->m_flags & 0x60000) != 0 ? 0x40000 : 0;
        this->m_flags = inherit40000 | (flags & 0xfffbfff7) | visible;
        this->m_flags = ((above->m_flags & 0x180000) != 0 ? 0x100000 : 0) | inherit40000 | (flags & 0xffebfff7) | visible;
        if ((above->m_flags & BASE_LOADED) != 0) {
            this->m_558 = params->m_matrix;
            if ((this->m_modelFlags2 >> 6 & 1) != 0 && this->m_parentModel != nullptr) {
                M2SequenceInfo info = {};
                info.m_override.m[0] = 1.0f;
                info.m_override.m[5] = 1.0f;
                info.m_override.m[10] = 1.0f;
                info.m_override.m[15] = 1.0f;
                info.m_blendMax = 1.0f;
                if (this->m_parentModel->GetSequenceInfo(0xffffffff, &info)) {
                    this->SetSequence(0xffffffff, info.m_id, info.m_variation, info.m_time, info.m_speed, 1, 0, 1, 0, 0,
                        1.0f);
                }
            }
        }
    }
    if ((this->m_modelFlags & 0x40000000) == 0) {
        this->Animate(parent, params, a4, static_cast<int32_t>(a5), static_cast<int32_t>(a6));
        return;
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        return;
    }
    uint32_t frame = this->m_scene->m_frame;
    if (this->m_matrixFrame != frame) {
        this->ResetBoneState(parent, params, frame, static_cast<int32_t>(this->m_scene->m_time));
    }
    M2Data* data = this->m_shared->m_data;
    if ((this->m_boneDone[0] & 1) == 0 && (this->m_modelFlags2 & 0x800) == 0) {
        this->AnimateRootOnly(this->m_animState->m_boneStates);
    }
    if (data->m_textureWeights.Count() != 0) {
        this->AnimateTextureWeights();
    }
    M2AnimateContext context;
    M2AnimateContextInit(&context, this->m_scene->m_time, this->m_scene->m_frame, 1,
        static_cast<uint32_t>(M2AnimFrameSkipLod()), 0, 0);
    if (data->m_textureTransforms.Count() != 0) {
        this->AnimateTextureTransforms(&context);
    }
    this->m_modelFlags &= ~8u;
    if (this->m_animState->m_attachmentStates != nullptr || this->m_children != nullptr) {
        this->UpdateChildren(&context);
    }
}

// The lighting this model uses (its own, or the parent's unless the parent was made to light
// its children separately) and the drawables for the pass.
// ref: FUN_1411c7a90
void CM2Model::PrepareLighting(int32_t pass, uint32_t propagate) {
    uint32_t flags = this->m_flags;
    if ((flags & BASE_LOADED) == 0 || (flags >> 23 & 1) != 0 || ((flags & BASE_SHOWN) == 0 && (flags >> 21 & 1) != 0)) {
        return;
    }
    CM2BaseModel* parent = this->m_parent;
    CM2ModelLighting* lighting;
    if (parent == nullptr) {
        lighting = &this->m_lighting;
    } else {
        this->m_flags = ((flags ^ parent->m_flags) & BASE_VISIBLE) ^ flags;
        lighting = (parent->m_createFlags & 1) != 0 ? &this->m_lighting : parent->m_lightingUsed;
    }
    this->m_lightingUsed = lighting;
    if (lighting == nullptr) {
        this->m_lightingUsed = &this->m_lighting;
    }
    int32_t now = static_cast<int32_t>(this->m_scene->m_time);
    int32_t last = static_cast<int32_t>(this->m_createTime);
    this->m_createTime = static_cast<uint32_t>(now);
    propagate |= this->m_flags2 >> 1 & 1;
    this->UpdateDrawables(pass, static_cast<float>(static_cast<uint32_t>(now - last)) * 0.001f, propagate);  // DAT_142618f58
    if (propagate != 0) {
        this->UpdateCulling();
    }
}

// Culled (0x800) when the bounds are outside the scene's frustum.
// ref: FUN_1411b6990
void CM2Model::UpdateCulling() {
    void* frustum = this->m_scene->m_frustum;
    uint32_t flags = this->m_flags;
    if (frustum == nullptr || (flags & 0x1001) != 0x1001 || (flags >> 13 & 1) != 0 || (flags & BASE_VISIBLE) == 0) {
        this->m_flags &= ~BASE_CULLED;
        return;
    }
    float storage[6];
    const float* box;
    if ((this->m_flags2 & 2) == 0) {
        box = this->GetBounds(storage);
    } else {
        box = this->GetKeyBoneBounds(storage, static_cast<uint16_t>(this->m_3b0));
    }
    float bounds[6];
    std::memcpy(bounds, box, sizeof(bounds));
    bool valid = !(bounds[3] < bounds[0] || bounds[4] < bounds[1] || bounds[5] < bounds[2]);
    flags = this->m_flags;
    if ((flags & BASE_LOADED) != 0 && this->m_shared->m_data->m_ribbons.Count() != 0) {
        if (!valid) {
            this->m_flags = flags & ~BASE_CULLED;
            return;
        }
    } else if (!valid) {
        this->m_flags = flags | BASE_CULLED;
        return;
    }
    uint32_t culled = M2FrustumTestBox(static_cast<const float*>(frustum), bounds) == 0 ? BASE_CULLED : 0;
    this->m_flags = culled | (this->m_flags & ~BASE_CULLED);
}

// ref: FUN_1411bdef0
float* CM2Model::GetBox(float* out) {
    if ((this->m_shared->m_flags & CM2Shared::SHARED_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if ((this->m_modelFlags & 0x800000) == 0) {
        std::memcpy(out, &this->m_shared->m_data->m_bounds, sizeof(CAaBox));
    } else {
        std::memcpy(out, &this->m_accumBounds->m_localBox, sizeof(CAaBox));
    }
    return out;
}

// ref: FUN_1411bdfe0
float* CM2Model::GetSphere(float* out) {
    if ((this->m_shared->m_flags & CM2Shared::SHARED_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if ((this->m_modelFlags & 0x800000) != 0) {
        std::memcpy(out, &this->m_accumBounds->m_center, sizeof(float) * 4);
        return out;
    }
    const CAaBox& bounds = this->m_shared->m_data->m_bounds;
    out[3] = this->m_shared->m_data->m_boundsRadius;
    out[0] = (bounds.m_min.x + bounds.m_max.x) * 0.5f;
    out[1] = (bounds.m_max.y + bounds.m_min.y) * 0.5f;
    out[2] = (bounds.m_max.z + bounds.m_min.z) * 0.5f;
    return out;
}

// ref: FUN_1411bdf70
float* CM2Model::GetBox2(float* out) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        std::memset(out, 0, sizeof(CAaBox));
        return out;
    }
    if ((this->m_modelFlags & 0x800000) != 0) {
        std::memcpy(out, &this->m_accumBounds->m_localBox, sizeof(CAaBox));
        return out;
    }
    std::memcpy(out, &this->m_shared->m_data->m_bounds, sizeof(CAaBox));
    return out;
}

// ref: FUN_1411be0a0
float* CM2Model::GetSphere2(float* out) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        std::memset(out, 0, sizeof(float) * 4);
        return out;
    }
    if ((this->m_modelFlags & 0x800000) != 0) {
        std::memcpy(out, &this->m_accumBounds->m_center, sizeof(float) * 4);
        return out;
    }
    const CAaBox& bounds = this->m_shared->m_data->m_bounds;
    out[3] = this->m_shared->m_data->m_boundsRadius;
    out[0] = (bounds.m_min.x + bounds.m_max.x) * 0.5f;
    out[1] = (bounds.m_max.y + bounds.m_min.y) * 0.5f;
    out[2] = (bounds.m_max.z + bounds.m_min.z) * 0.5f;
    return out;
}

// The matrix of the attachment point with id `index`, its bone animated now; the root's when
// the model has none (id 0x13 still counts as found).
// ref: FUN_1411b9f10
bool CM2Model::GetAttachmentMatrixByIndex(uint32_t index, C44Matrix* out) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    M2Array<M2Attachment>* attachments = this->m_animState->m_attachments;
    M2Array<uint16_t>* lookup = this->m_animState->m_attachmentLookup;
    uint16_t found = 0xffff;
    if ((this->m_flags & BASE_LOADED) != 0 && index == 0x37) {
        if (0x37 < lookup->Count()) {
            found = (*lookup)[0x37];
        }
        if (attachments->Count() <= found) {
            index = 0xb;
        }
    }
    if (index < lookup->Count() && (found = (*lookup)[index]) != 0xffff) {
        M2Attachment& attachment = (*attachments)[found];
        this->AnimateBoneFor(attachment.m_bone);
        this->GetAttachmentPointMatrix(&attachment.m_position, attachment.m_bone, out->m);
        return true;
    }
    this->GetRootMatrix(out->m);
    return index == 0x13;
}

// ref: FUN_1411c0860
void CM2Model::GetAttachmentPointMatrix(const C3Vector* position, uint32_t bone, float* out) {
    const float* b = this->m_animState->m_boneMatrices[bone].m;
    if (this->m_parent != nullptr && (this->m_flags >> 14 & 1) == 0) {
        const float* w = this->m_world.m;
        float r[16];
        for (int32_t col = 0; col < 4; col++) {
            r[col] = b[2] * w[8 + col] + b[3] * w[12 + col] + b[1] * w[4 + col] + b[0] * w[col];
        }
        for (int32_t row = 1; row < 3; row++) {
            const float* br = b + row * 4;
            for (int32_t col = 0; col < 4; col++) {
                r[row * 4 + col] = br[0] * w[col] + br[1] * w[4 + col] + br[2] * w[8 + col] + br[3] * w[12 + col];
            }
        }
        float r15 = b[12] * w[3] + b[13] * w[7] + b[14] * w[11] + b[15] * w[15];
        float t0 = b[12] * w[0] + b[13] * w[4] + b[14] * w[8] + b[15] * w[12] + position->y * r[4] + position->x * r[0]
            + position->z * r[8];
        float t1 = b[12] * w[1] + b[13] * w[5] + b[14] * w[9] + b[15] * w[13] + position->y * r[5] + position->x * r[1]
            + position->z * r[9];
        float t2 = b[12] * w[2] + b[13] * w[6] + b[14] * w[10] + b[15] * w[14] + position->y * r[6] + position->x * r[2]
            + position->z * r[10];
        const float* vp = this->m_scene->m_inverseView.m;
        for (int32_t col = 0; col < 4; col++) {
            out[col] = r[0] * vp[col] + r[1] * vp[4 + col] + r[3] * vp[12 + col] + r[2] * vp[8 + col];
        }
        for (int32_t row = 1; row < 3; row++) {
            const float* rr = r + row * 4;
            for (int32_t col = 0; col < 4; col++) {
                out[row * 4 + col] = rr[3] * vp[12 + col] + rr[2] * vp[8 + col] + rr[0] * vp[col] + rr[1] * vp[4 + col];
            }
        }
        for (int32_t col = 0; col < 4; col++) {
            out[12 + col] = t0 * vp[col] + t1 * vp[4 + col] + t2 * vp[8 + col] + r15 * vp[12 + col];
        }
        return;
    }
    const float* l = this->m_local.m;
    for (int32_t col = 0; col < 4; col++) {
        out[col] = b[2] * l[8 + col] + b[3] * l[12 + col] + b[1] * l[4 + col] + b[0] * l[col];
    }
    for (int32_t row = 1; row < 4; row++) {
        const float* br = b + row * 4;
        for (int32_t col = 0; col < 4; col++) {
            out[row * 4 + col] = br[0] * l[col] + br[1] * l[4 + col] + br[2] * l[8 + col] + br[3] * l[12 + col];
        }
    }
    if (std::fabs(out[0] * out[0] + out[1] * out[1] + out[3] * out[3] + out[2] * out[2]) < 2.3841858e-07f) {
        std::memcpy(out, l, sizeof(float) * 12);
    }
    float px = position->x;
    float py = position->y;
    float pz = position->z;
    out[12] = py * out[4] + px * out[0] + pz * out[8] + out[12];
    out[13] = py * out[5] + px * out[1] + pz * out[9] + out[13];
    out[14] = py * out[6] + px * out[2] + pz * out[10] + out[14];
}

// ref: FUN_1411c0d80
void CM2Model::GetRootMatrix(float* out) {
    this->AnimateBoneFor(0);
    if (this->m_parent != nullptr && (this->m_flags >> 14 & 1) == 0) {
        std::memcpy(out, this->m_view.m, sizeof(C44Matrix));
        return;
    }
    std::memcpy(out, this->m_local.m, sizeof(C44Matrix));
}

// The box of what a key bone plays, the placed bounds of its own sequence and the root's merged,
// with the effects'; the model's bounds when the bone keeps no sequence of its own.
// ref: FUN_1411c0490
float* CM2Model::GetKeyBoneBounds(float* out, uint16_t keyBone) {
    if (keyBone == 0xffff) {
        this->GetBounds(out);
        return out;
    }
    if ((this->m_flags & (BASE_LOADED | BASE_VISIBLE)) != (BASE_LOADED | BASE_VISIBLE)) {
        SetEmptyBox(out);
        return out;
    }
    M2BoneState* states = this->m_animState->m_boneStates;
    M2Array<M2Sequence>* rootSequences = states[0].m_sequence->m_source.m_sequences;
    M2Array<uint16_t>* lookup = this->m_animState->m_keyBoneLookup;
    if (1 < rootSequences->Count() && this->m_shared->m_data->m_particles.Count() == 0 && keyBone < lookup->Count()) {
        uint16_t bone = (*lookup)[keyBone];
        uint16_t rootIndex = states[0].m_sequence->m_nextSequence;
        M2SequenceTime* slot = bone != 0 && bone != 0xffff ? states[bone].m_sequence : nullptr;
        if (slot != nullptr && rootIndex != 0xffff) {
            auto parentBounds = static_cast<M2Array<uint8_t>*>(this->m_shared->m_parentSequenceBounds);
            float rootBox[6];
            std::memcpy(rootBox, &(*rootSequences)[rootIndex].m_bounds.m_extent, sizeof(rootBox));
            if (parentBounds != nullptr && (states[0].m_sequence->m_source.m_flags & 1) != 0) {
                std::memcpy(rootBox, &(*parentBounds)[0] + rootIndex * 0x1c, sizeof(rootBox));
            }
            float boneBox[6];
            std::memcpy(boneBox, &(*slot->m_source.m_sequences)[slot->m_nextSequence].m_bounds.m_extent, sizeof(boneBox));
            if (parentBounds != nullptr && (states[bone].m_sequence->m_source.m_flags & 1) != 0) {
                std::memcpy(boneBox, &(*parentBounds)[0] + slot->m_nextSequence * 0x1c, sizeof(boneBox));
            }
            float merged[6];
            merged[0] = boneBox[0] <= rootBox[0] ? boneBox[0] : rootBox[0];
            merged[5] = boneBox[5] <= rootBox[5] ? rootBox[5] : boneBox[5];
            merged[4] = boneBox[4] <= rootBox[4] ? rootBox[4] : boneBox[4];
            merged[1] = boneBox[1] <= rootBox[1] ? boneBox[1] : rootBox[1];
            merged[3] = boneBox[3] <= rootBox[3] ? rootBox[3] : boneBox[3];
            merged[2] = boneBox[2] <= rootBox[2] ? boneBox[2] : rootBox[2];
            float box[6];
            std::memcpy(box, merged, sizeof(box));
            if (merged[0] < merged[3] && merged[1] < merged[4] && merged[2] < merged[5]) {
                float placed[6];
                BoxTransform(placed, merged, PlacementMatrix(this));
                std::memcpy(box, placed, sizeof(box));
            }
            float effects[6];
            this->GetEffectBox(effects);
            float scratch[6];
            BoxUnion(box, scratch, effects);
            std::memcpy(out, box, sizeof(box));
            return out;
        }
    }
    this->GetBounds(out);
    return out;
}

// ref: FUN_1411bac00
bool CM2Model::GetSequenceInfo(uint32_t keyBone, M2SequenceInfo* out) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    M2AnimState* anim = this->m_animState;
    uint16_t bone = 0;
    if (keyBone != 0xffffffff) {
        bone = keyBone < anim->m_keyBoneLookup->Count() ? (*anim->m_keyBoneLookup)[keyBone] : 0xffff;
    }
    if (anim->m_bones->Count() <= bone) {
        return false;
    }
    M2BoneState* state = &anim->m_boneStates[bone];
    out->m_id = (state->m_flags & 0x2000) != 0 ? state->m_5c : 0xffffffff;
    out->m_flag80 = (state->m_flags >> 7 & 1) != 0;
    out->m_blendKind = state->m_blendKind;
    out->m_fixedBlend = state->m_fixedBlend;
    out->m_blendMax = state->m_blendMax;
    out->m_variation = state->m_variation;
    out->m_sequence = 0xffff;
    out->m_flags = 0;
    M2SequenceTime* slot = state->m_sequence;
    if (slot == nullptr) {
        return true;
    }
    int32_t length = static_cast<int32_t>(slot->m_timeOffset);
    int32_t time;
    if (slot->m_speed == 1.0f) {
        time = (length - slot->m_startTime) + static_cast<int32_t>(this->m_scene->m_time);
    } else {
        time = static_cast<int32_t>(static_cast<float>(static_cast<int32_t>(this->m_scene->m_time) - slot->m_startTime)
                   * slot->m_speed)
            + length;
    }
    if (length != 0) {
        while (time < 0) {
            time += length;
        }
    }
    int32_t clamped = time < 0 ? 0 : time;
    out->m_time = clamped;
    out->m_slotTime = slot->m_time;
    out->m_speed = slot->m_speed;
    uint32_t start = static_cast<uint32_t>(slot->m_startTime);
    out->m_startTime = slot->m_startTime;
    out->m_endTime = slot->m_endTime;
    out->m_pending = slot->m_pending != 0;
    uint16_t index = slot->m_nextSequence;
    out->m_ended = index != 0xffff && (*slot->m_source.m_sequences)[index].m_duration <= static_cast<uint32_t>(clamped);
    out->m_sequence = slot->m_nextSequence;
    if (slot->m_nextSequence != 0xffff) {
        out->m_flags = (*slot->m_source.m_sequences)[slot->m_nextSequence].m_flags;
    }
    out->m_blendLeft = 0;
    if (start < static_cast<uint32_t>(state->m_blendEnd)) {
        out->m_blendLeft = static_cast<uint16_t>(static_cast<uint32_t>(state->m_blendEnd) - start);
    }
    out->m_repeats = slot->m_repeats;
    if (state->m_override != nullptr) {
        out->m_override = *state->m_override;
    }
    return true;
}

// ref: FUN_1411b9cf0
bool CM2Model::IsAttachmentVisible(uint32_t index) {
    if ((this->m_flags & BASE_LOADED) != 0 && index < this->m_animState->m_attachments->Count()) {
        return this->m_animState->m_attachmentStates[index].m_value != 0;
    }
    return false;
}

// ref: FUN_1411c7b80
void CM2Model::ClearLoaded() {
    this->ReleaseInstanceData();
    if ((this->m_flags & BASE_LOADED) != 0) {
        this->m_flags &= ~BASE_LOADED;
    }
}

// ref: FUN_1411b80b0
void CM2Model::ReleaseInstanceData() {
    if ((this->m_flags & BASE_LOADED) == 0 || this->m_shared == nullptr) {
        return;
    }
    M2Data* data = this->m_shared->m_data;
    if (this->m_boneStates != nullptr && (this->m_modelFlags2 & 0x1000) == 0) {
        for (uint32_t i = 0; i < data->m_bones.Count(); i++) {
            M2FreeFileData(this->m_boneStates[i].m_override);
            this->m_boneStates[i].m_override = nullptr;
        }
    }
    if (this->m_textures != nullptr) {
        for (uint32_t i = 0; i < data->m_textures.Count(); i++) {
            auto texture = reinterpret_cast<HTEXTURE*>(&this->m_textures[i]);
            if (*texture != nullptr) {
                TextureFree(texture);
                this->m_textures[i] = nullptr;
            }
        }
    }
    if (this->m_lights != nullptr) {
        for (uint32_t i = 0; i < data->m_lights.Count(); i++) {
            this->m_lights[i].m_lighting.SetEnabled(0);
        }
    }
    if (this->m_cameras != nullptr) {
        for (uint32_t i = 0; i < data->m_cameras.Count(); i++) {
            if (this->m_cameras[i].m_camera != nullptr) {
                M2CameraRelease(&this->m_cameras[i].m_camera);
                this->m_cameras[i].m_camera = nullptr;
            }
        }
    }
    for (uint32_t i = 0; i < data->m_ribbons.Count(); i++) {
        if (this->m_ribbons[i] != nullptr) {
            RibbonEmitterDelete(this->m_ribbons[i]);
            this->m_ribbons[i] = nullptr;
        }
    }
    for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
        CParticleEmitter2* emitter = this->m_particles[i];
        if (emitter != nullptr) {
            if (emitter->m_generator != nullptr) {
                ParticleGeneratorDestroy(emitter->m_generator);
                this->m_particles[i]->m_generator = nullptr;
            }
            ParticleEmitterDelete(this->m_particles[i]);
            this->m_particles[i] = nullptr;
        }
    }
    if (this->m_skeleton != nullptr) {
        M2SkeletonReleaseModel(this->m_skeleton);
    }
}

// ref: FUN_1411b6b50
void CM2Model::OnRemove() {
    this->CancelAllPlaybacks();
    M2ModelUnlinkWaiting(this);
}

// ref: FUN_1411b5cb0
void CM2Model::CancelAllPlaybacks() {
    if (this->m_skeleton != nullptr) {
        M2SkeletonCancelModelPlaybacks(this->m_skeleton, this);
    }
    CM2Shared* shared = this->m_shared;
    bool reading = (shared->m_flags & CM2Shared::SHARED_SEQUENCES_PENDING) != 0;
    for (CM2SequenceLoad* load = shared->m_sequenceLoads.Head(); load != nullptr;) {
        CM2SequenceLoad* nextLoad = shared->m_sequenceLoads.Next(load);
        for (CM2SequencePlayBack* playback = load->m_playbacks.Head(); playback != nullptr;) {
            CM2SequencePlayBack* next = load->m_playbacks.Next(playback);
            if (playback->m_model == this) {
                if (reading) {
                    playback->m_flags |= 8;
                } else {
                    playback->m_link.Unlink();
                    SMemFree(playback, ".?AV?$TSList@UCM2SequencePlayBack@@V?$TSGetLink@UCM2SequencePlayBack@@@@$07@@",
                        -2, 0);
                }
            }
            playback = next;
        }
        load = nextLoad;
    }
}

// The world matrix as the local one through `world`, and the view matrix from it.
// ref: FUN_1411ce880
void CM2Model::SetWorldMatrix(const C44Matrix* world) {
    if ((this->m_flags & BASE_MATRIX_CURRENT) != 0 && this->m_matrixFrame == this->m_scene->m_frame) {
        return;
    }
    BoneTimesWorld(this->m_local.m, world->m, this->m_world.m, false);
    MultiplyViewOrder(this->m_world.m, this->m_scene->m_inverseView.m, this->m_view.m);
    this->m_flags |= BASE_MATRIX_CURRENT;
    this->m_flags &= ~BASE_INVERSE_CURRENT;
}

// The GPU data a model keeps for compute skinning and ray tracing (+0x600, +0x670, +0x678); none
// of it is made yet, so each release is reached only once its creation is ported.
// ref: FUN_1411b6f80
void CM2Model::ReleaseGpuData() {
    if (this->m_lock670 != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_14228b740: the RW buffer
    }
    if (this->m_600 != nullptr) {
        FROST_UNIMPLEMENTED();  // the compute batches' RW buffer, texture and GxBuf
    }
    if (this->m_handle != -1) {
        FROST_UNIMPLEMENTED();  // FUN_142295360: the handle's release
        this->m_handle = -1;
        *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(this) + 0x685) = 0x1ff;
    }
    this->m_680 = 0;
}

// ref: FUN_1411b6910
void CM2Model::RefreshBatchEffects() {
    auto batches = static_cast<uint8_t*>(this->m_600);
    if (batches == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < *reinterpret_cast<uint16_t*>(batches); i++) {
        auto batch = *reinterpret_cast<M2Batch**>(batches + 0x10);
        CShaderEffect* effect = this->m_shared->GetBatchEffect(reinterpret_cast<M2Batch*>(
            reinterpret_cast<uint8_t*>(batch) + i * 0x18));
        (*reinterpret_cast<CShaderEffect***>(static_cast<uint8_t*>(this->m_600) + 0x40))[i] = effect;
        batches = static_cast<uint8_t*>(this->m_600);
    }
}

namespace {

// Copies at most size - 1 characters and terminates (the reference's inlined copy).
const char* CopyLimited(char* buffer, const char* source, size_t size) {
    char* out = buffer;
    if (size == 0) {
        return buffer;
    }
    for (;;) {
        size--;
        if (size == 0) {
            *out = '\0';
            return buffer;
        }
        char c = *source++;
        *out++ = c;
        if (c == '\0') {
            return buffer;
        }
    }
}

// A texture slot of an effect released, the shared white texture only forgotten.
void ReleaseEffectTexture(void** slot) {
    if (*slot == nullptr) {
        return;
    }
    if (*slot == TextureGetWhite()) {
        *slot = nullptr;
    } else {
        TextureFree(reinterpret_cast<HTEXTURE*>(slot));
    }
}

}  // namespace

// A mirrored model's local matrix flipped, the ray tracing instance moved, and the bone-done
// bits of this model and its attached CM2Models cleared for the new matrices.
// ref: FUN_1411c6a80
void CM2Model::OnMatricesUpdated() {
    auto flip = [](float& value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        bits ^= 0x80000000;  // DAT_14261aa30
        std::memcpy(&value, &bits, sizeof(bits));
    };
    if ((this->m_modelFlags >> 26 & 1) != 0) {
        flip(this->m_local.m[4]);
        flip(this->m_local.m[5]);
        flip(this->m_local.m[6]);
    }
    if ((this->m_modelFlags >> 19 & 1) != 0) {
        flip(this->m_local.m[4]);
        flip(this->m_local.m[5]);
        flip(this->m_local.m[6]);
        flip(this->m_local.m[8]);
        flip(this->m_local.m[9]);
        flip(this->m_local.m[10]);
    }
    if (this->m_rtEntry[0] != -1 && this->m_rtEntry[1] == 8 && M2SceneGetRayTracing(this->m_scene) != nullptr) {
        RayTracingUpdateInstance(M2SceneGetRayTracing(this->m_scene), this->m_rtEntry, this->m_local.m);
    }
    if ((this->m_flags & BASE_LOADED) != 0 && (this->m_boneDone[0] & 1) != 0) {
        std::memset(this->m_boneDone, 0, this->m_boneDoneBytes);
        for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
            if ((child->m_flags & BASE_LOADED) != 0 && child->m_type == 0) {
                auto model = static_cast<CM2Model*>(child);
                std::memset(model->m_boneDone, 0, model->m_boneDoneBytes);
            }
        }
    }
}

// False while any texture is still being read.
// ref: FUN_1411cd550
bool CM2Model::AreTexturesSettled() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_textures.Count(); i++) {
        if (this->m_textures[i] != nullptr && TextureIsLoading(ModelTexture(this, i))) {
            return false;
        }
    }
    return true;
}

// The root sequence's speed and the model's own clock rescaled, keeping where they are.
// ref: FUN_1411c99e0
void CM2Model::SetTimeScale(float scale) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call != nullptr) {
            call->m_type = 0x12;
            call->m_next = nullptr;
            call->m_frame = this->m_scene->m_time;
            std::memcpy(&call->m_args[0], &scale, sizeof(float));
            *this->m_callsTail = call;
            this->m_callsTail = &call->m_next;
        }
        return;
    }
    this->SetSequenceSpeed(0xffffffff, scale);
    if (this->m_skeleton != nullptr) {
        FROST_UNIMPLEMENTED();  // the skeleton's clock (+0x70, +0x74)
        return;
    }
    int32_t now = static_cast<int32_t>(this->m_scene->m_time);
    float old = this->m_404;
    this->m_404 = scale;
    float inverse = std::fabs(scale) <= 1.0e-05f ? 0.0f : 1.0f / scale;  // DAT_1426c75c4
    this->m_initTime = static_cast<uint32_t>(
        now
        - static_cast<int32_t>(std::fabs(inverse)
            * static_cast<float>(static_cast<int32_t>(
                static_cast<float>(static_cast<uint32_t>(now - static_cast<int32_t>(this->m_initTime))) * old))));
}

// ref: FUN_1411cab20
void CM2Model::SetSequenceSpeed(uint32_t keyBone, float speed) {
    float clamped = 1.0f;
    if (speed <= 10000.0f) {  // DAT_1426c6510
        clamped = speed;
        if (speed < -10000.0f) {  // DAT_1426cc58c
            clamped = -1.0f;  // DAT_14261a9c8
        }
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call == nullptr) {
            return;
        }
        call->m_type = 9;
        call->m_next = nullptr;
        call->m_frame = this->m_scene->m_time;
        std::memcpy(&call->m_args[1], &clamped, sizeof(float));
        call->m_args[0] = keyBone;
        *this->m_callsTail = call;
        this->m_callsTail = &call->m_next;
        return;
    }
    M2AnimState* anim = this->m_animState;
    uint16_t bone = 0;
    if (keyBone != 0xffffffff) {
        bone = keyBone < anim->m_keyBoneLookup->Count() ? (*anim->m_keyBoneLookup)[keyBone] : 0xffff;
    }
    if (anim->m_bones->Count() <= bone) {
        return;
    }
    M2SequenceTime* slot = anim->m_boneStates[bone].m_sequence;
    if (slot == nullptr || slot->m_nextSequence == 0xffff) {
        return;
    }
    int32_t length = static_cast<int32_t>(slot->m_timeOffset);
    int32_t now = static_cast<int32_t>(this->m_scene->m_time);
    int32_t time;
    if (slot->m_speed == 1.0f) {
        time = (length - slot->m_startTime) + now;
    } else {
        time = static_cast<int32_t>(static_cast<float>(now - slot->m_startTime) * slot->m_speed) + length;
    }
    if (length != 0) {
        while (time < 0) {
            time += length;
        }
    }
    if (time < 0) {
        time = 0;
    }
    uint32_t duration = (*slot->m_source.m_sequences)[slot->m_nextSequence].m_duration;
    float inverse = std::fabs(clamped) <= 1.0e-05f ? 0.0f : 1.0f / clamped;
    float rate = std::fabs(inverse);
    slot->m_speed = clamped;
    slot->m_inverseSpeed = inverse;
    int32_t start = now - static_cast<int32_t>(static_cast<float>(time) * rate);
    slot->m_startTime = start;
    slot->m_endTime = static_cast<int32_t>(static_cast<int64_t>(static_cast<float>(slot->m_repeats * duration) * rate))
        + start;
}

// The collision facets inside a box (world collision only, not reached by the glue screens).
// ref: FUN_1411bb3d0
void CM2Model::GetFacets(const float* box, const float* matrix, void* facets) {
    (void)box;
    (void)matrix;
    (void)facets;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1411be860
const char* CM2Model::GetName() {
    return this->m_shared->GetDisplayName();
}

// ref: FUN_1411bd850
const char* CM2Model::GetName2(char* buffer, size_t size) {
    CM2Shared* shared = this->m_shared;
    if (shared == nullptr) {
        return CopyLimited(buffer, "NA", size);
    }
    // FUN_1411a8310
    if (shared->m_name[0] == '\0') {
        return GxShaderName(shared->m_fileDataId, buffer, static_cast<uint32_t>(size));
    }
    return CopyLimited(buffer, shared->m_name, size);
}

// ref: FUN_1411bd8a0
const char* CM2Model::GetName3(char* buffer, size_t size) {
    CM2Shared* shared = this->m_shared;
    if (shared == nullptr) {
        return CopyLimited(buffer, "NA", size);
    }
    if (shared->m_name[0] == '\0') {
        return GxShaderName(shared->m_fileDataId, buffer, static_cast<uint32_t>(size));
    }
    return CopyLimited(buffer, shared->m_name, size);
}

// Out of the scene: the ray tracing entry dropped, the queued calls' times made relative, and
// the lights unlinked from the scene's.
// ref: FUN_1411b72b0
void CM2Model::RemoveFromScene() {
    if (this->m_scene != nullptr) {
        void* rayTracing = M2SceneGetRayTracing(this->m_scene);
        if (rayTracing != nullptr) {
            if (this->m_rtEntry[1] == 8) {
                RayTracingRemoveInstance(rayTracing, this->m_rtEntry);
                this->m_rtEntry[0] = -1;
                this->m_rtEntry[1] = 0;
            }
            if (this->m_rtEntry[1] == 2) {
                RayTracingRemoveModel(rayTracing, this->m_rtEntry);
                this->m_rtEntry[0] = -1;
                this->m_rtEntry[1] = 0;
            }
        }
    }
    if (this->m_sceneLink != nullptr) {
        *this->m_sceneLink = this->m_sceneNext;
    }
    if (this->m_sceneNext != nullptr) {
        this->m_sceneNext->m_sceneLink = this->m_sceneLink;
    }
    this->m_sceneLink = nullptr;
    this->m_sceneNext = nullptr;
    if ((this->m_flags & BASE_LOADED) == 0) {
        if (this->m_scene != nullptr) {
            for (M2ModelCall* call = this->m_calls; call != nullptr; call = call->m_next) {
                call->m_frame -= this->m_scene->m_time;
            }
        }
        this->m_scene = nullptr;
        return;
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_lights.Count(); i++) {
        M2LightState* light = &this->m_lights[i];
        light->m_lighting.Unlink();
        light->m_lighting.m_scene = nullptr;
    }
    this->m_scene = nullptr;
}

// ref: FUN_1411be680
uint8_t CM2Model::GetMaxLod() {
    if ((this->m_flags & BASE_LOADED) != 0) {
        return this->m_shared->m_lodMax;
    }
    return 0;
}

// ref: FUN_1411be670
const void* CM2Model::GetLodData() {
    return &this->m_shared->m_lodFlags;
}

// ref: FUN_1411c0d60
bool CM2Model::HasGpuBatches() {
    if ((this->m_modelFlags2 & 0x20000) == 0) {
        return this->m_600 != nullptr;
    }
    return true;
}

// ref: FUN_1411b6520
void CM2Model::OnLodChanged() {
    if (this->m_lodTarget != this->m_lod) {
        if (this->m_600 == nullptr) {
            this->m_lod = this->m_lodTarget;
        } else {
            FROST_UNIMPLEMENTED();  // FUN_1411cdb90, FUN_1411c6d30: the GPU batches for the new LOD
            this->m_lod = this->m_lodTarget;
        }
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->OnLodChanged();
    }
}

// A file flagged 0x10000000 fades past its last LOD: the LOD effect (type 8) is made when the
// model is at it, and dropped when it is not.
// ref: FUN_1411ce170
void CM2Model::UpdateLodEffect() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if (this->m_shared->m_data == nullptr) {
        return;
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if ((this->m_shared->m_data->m_flags & 0x10000000) == 0) {
        return;
    }
    uint8_t maxLod = this->GetMaxLod();
    auto effect = static_cast<M2ModelEffect*>(this->m_params.m_effect);
    if (this->m_lod < maxLod) {
        if (effect != nullptr && effect->m_type == 8) {
            this->ClearEffect();
        }
        return;
    }
    if (effect != nullptr && effect->m_type == 8) {
        return;
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call != nullptr) {
            call->m_type = 0x1a;
            call->m_next = nullptr;
            call->m_frame = this->m_scene->m_time;
            call->m_args[0] = 8;
            *this->m_callsTail = call;
            this->m_callsTail = &call->m_next;
        }
        return;
    }
    auto own = static_cast<M2ModelEffect*>(this->m_effect2);
    if (own == nullptr) {
        own = M2ModelEffectAlloc();
        this->m_effect2 = own;
        if (own == nullptr) {
            return;
        }
    }
    own->m_type = 8;
    static_cast<M2ModelEffect*>(this->m_effect2)->m_strength = 1.0f;
    own = static_cast<M2ModelEffect*>(this->m_effect2);
    if (own != nullptr) {
        for (void*& texture : own->m_textures) {
            ReleaseEffectTexture(&texture);
        }
    }
}

// ref: FUN_1411b6240
void CM2Model::ClearEffect() {
    auto effect = static_cast<M2ModelEffect*>(this->m_effect2);
    if (effect == nullptr) {
        return;
    }
    for (void*& texture : effect->m_textures) {
        ReleaseEffectTexture(&texture);
    }
    uint32_t flags = this->m_flags;
    this->m_flags = flags & ~0x10000000u;
    if ((flags & BASE_LOADED) != 0 && this->m_shared != nullptr) {
        for (uint32_t i = 0;; i++) {
            if ((this->m_flags & BASE_LOADED) == 0) {
                this->WaitForLoad(0);
            }
            if (this->m_shared->m_data->m_particles.Count() <= i) {
                break;
            }
            this->m_particles[i]->m_flags |= 2;
        }
    }
    for (CM2Model* model = this->m_attachedModels; model != nullptr && (model->m_flags & BASE_LOADED) != 0;
         model = model->m_attachedNext) {
        model->ClearEffectFrom(this->m_effect2);
    }
    if (this->m_params.m_effect == this->m_effect2) {
        this->m_params.m_effect = nullptr;
    }
    M2ModelEffectFree(static_cast<M2ModelEffect*>(this->m_effect2));
    this->m_effect2 = nullptr;
}

// ref: FUN_1411b6380
void CM2Model::ClearEffectFrom(void* effect) {
    if ((this->m_flags & BASE_LOADED) == 0 || this->m_shared == nullptr) {
        return;
    }
    this->m_flags &= ~0x10000000u;
    auto current = static_cast<M2ModelEffect*>(this->m_params.m_effect);
    if (current != nullptr && current->m_type == 5 && (this->m_modelFlags & 0x400000) == 0
        && (this->m_shared->m_data->m_flags & 4) == 0) {
        for (uint32_t i = 0;; i++) {
            if ((this->m_flags & BASE_LOADED) == 0) {
                this->WaitForLoad(0);
            }
            if (this->m_shared->m_data->m_particles.Count() <= i) {
                break;
            }
            this->m_particles[i]->m_flags |= 2;
        }
    }
    if (this->m_params.m_effect == effect) {
        this->m_params.m_effect = this->m_effect2;
    }
    for (CM2Model* model = this->m_attachedModels; model != nullptr && (model->m_flags & BASE_LOADED) != 0;
         model = model->m_attachedNext) {
        model->ClearEffectFrom(effect);
    }
}

// The centre of the bounds (the file's, or the root sequence's for a model flagged 0x80000)
// placed, and the placement's scale; returns the squared scale.
// ref: FUN_1411bdad0
float CM2Model::GetScaledRadius(C3Vector* center, float* radius) {
    float cx;
    float cy;
    float cz;
    if ((this->m_modelFlags2 & 0x80000) == 0) {
        cx = this->m_shared->m_center.x;
        cy = this->m_shared->m_center.y;
        cz = this->m_shared->m_center.z;
    } else if ((this->m_flags & (BASE_LOADED | BASE_VISIBLE)) == (BASE_LOADED | BASE_VISIBLE)) {
        const float* bounds = RootSequenceBounds(this);
        cz = (bounds[5] + bounds[2]) * 0.5f;
        cy = (bounds[4] + bounds[1]) * 0.5f;
        cx = (bounds[0] + bounds[3]) * 0.5f;
    } else {
        cx = 0.0f;
        cy = 0.0f;
        cz = 0.0f;
    }
    float partial;
    if (this->m_parent == nullptr || (this->m_flags >> 14 & 1) != 0) {
        const float* l = this->m_local.m;
        center->x = cy * l[4] + cx * l[0] + cz * l[8] + l[12];
        center->y = cy * l[5] + cx * l[1] + cz * l[9] + l[13];
        center->z = cy * l[6] + cx * l[2] + cz * l[10] + l[14];
        partial = l[1] * l[1];
        float scale = l[0] * l[0] + partial + l[2] * l[2];
        if (2.3841858e-07f < scale) {
            *radius = std::sqrt(scale);
            return scale;
        }
    } else {
        const float* w = this->m_world.m;
        const float* v = this->m_scene->m_inverseView.m;
        float r0 = w[2] * v[8] + w[3] * v[12] + w[1] * v[4] + w[0] * v[0];
        float r1 = w[2] * v[9] + w[3] * v[13] + w[1] * v[5] + w[0] * v[1];
        float r2 = w[2] * v[10] + w[3] * v[14] + w[1] * v[6] + w[0] * v[2];
        float r3 = w[2] * v[11] + w[3] * v[15] + w[1] * v[7] + w[0] * v[3];
        center->y = (w[8] * v[1] + w[9] * v[5] + w[10] * v[9] + w[11] * v[13]) * cz + w[12] * v[1] + w[13] * v[5]
            + w[14] * v[9] + w[15] * v[13] + (w[4] * v[1] + w[5] * v[5] + w[6] * v[9] + w[7] * v[13]) * cy + cx * r1;
        center->x = (w[8] * v[0] + w[9] * v[4] + w[10] * v[8] + w[11] * v[12]) * cz + w[12] * v[0] + w[13] * v[4]
            + w[14] * v[8] + w[15] * v[12] + (w[4] * v[0] + w[5] * v[4] + w[6] * v[8] + w[7] * v[12]) * cy + cx * r0;
        center->z = (w[8] * v[2] + w[9] * v[6] + w[10] * v[10] + w[11] * v[14]) * cz + w[12] * v[2] + w[13] * v[6]
            + w[14] * v[10] + w[15] * v[14] + (w[4] * v[2] + w[5] * v[6] + w[6] * v[10] + w[7] * v[14]) * cy + cx * r2;
        partial = r0 * r0 + r1 * r1;
        float scale = r3 * r3 + r2 * r2 + partial;
        if (2.3841858e-07f < scale) {
            *radius = std::sqrt(scale);
            return scale;
        }
    }
    *radius = 0.0f;
    return partial;
}

// The name with its extension replaced (FUN_140214020): the last '.' after the last separator.
// ref: FUN_140214020
void ReplaceExtension(char* buffer, size_t size, const char* name, const char* extension) {
    const char* dot = nullptr;
    const char* cursor = name;
    for (char c = *cursor; c != '\0'; c = *++cursor) {
        if (c == '.') {
            dot = cursor;
        } else if (c == '/' || c == '\\') {
            dot = nullptr;
        }
    }
    const char* end = dot != nullptr ? dot : cursor;
    size_t length = static_cast<size_t>(end - name);
    if (size <= length) {
        *buffer = '\0';
        return;
    }
    if (name != buffer) {
        std::memcpy(buffer, name, length);
    }
    char* out = buffer + length;
    size_t left = size - length;
    for (;;) {
        left--;
        if (left == 0) {
            *out = '\0';
            return;
        }
        char c = *extension++;
        *out++ = c;
        if (c == '\0') {
            return;
        }
    }
}

namespace {

// The shared file's content key, by FileDataID or by name.
bool SharedContentKey(const CM2Shared* shared, uint8_t* key) {
    if (shared->m_fileDataId == 0) {
        return cas::CASFileGetContentKeyByName(shared->m_name, key, (shared->m_flags >> 13) & 0x20);
    }
    return cas::CASFileGetContentKeyById(shared->m_fileDataId, key);
}

}  // namespace

// Whether the other model is a CM2Model of the same file (FUN_1411aa470 compares the keys).
// ref: FUN_1411c6270
bool CM2Model::IsSameFile(CM2BaseModel* other) {
    if (this->m_shared == nullptr || other->m_type != 0) {
        return false;
    }
    CM2Shared* theirs = static_cast<CM2Model*>(other)->m_shared;
    // FUN_1411aa470
    uint8_t a[16];
    uint8_t b[16];
    if (!SharedContentKey(theirs, a) || !SharedContentKey(this->m_shared, b)) {
        return false;
    }
    return std::memcmp(a, b, sizeof(a)) == 0;
}

// ref: FUN_1411c62a0
bool CM2Model::IsFileDataId(int32_t fileDataId) {
    if (this->m_shared == nullptr) {
        return false;
    }
    uint8_t a[16];
    uint8_t b[16];
    if (!cas::CASFileGetContentKeyById(fileDataId, a) || !SharedContentKey(this->m_shared, b)) {
        return false;
    }
    return std::memcmp(a, b, sizeof(a)) == 0;
}

// ref: FUN_1411c62c0
bool CM2Model::IsFileName(const char* name) {
    if (this->m_shared == nullptr) {
        return false;
    }
    char path[0x110];
    ReplaceExtension(path, 0x104, name, ".m2");  // DAT_14271fc30
    uint8_t a[16];
    uint8_t b[16];
    if (!cas::CASFileGetContentKeyByName(path, a, 0) || !SharedContentKey(this->m_shared, b)) {
        return false;
    }
    return std::memcmp(a, b, sizeof(a)) == 0;
}

// ref: FUN_1411b9a80
uint32_t CM2Model::GetAttachmentIndex(uint32_t id) {
    M2Array<uint16_t>* lookup = this->m_animState->m_attachmentLookup;
    if (lookup->Count() <= id) {
        return 0xffff;
    }
    return (*lookup)[id];
}

// Id 0x37 stands in for 0xb on a file without it.
// ref: FUN_1411bd740
uint32_t CM2Model::MapAttachmentId(uint32_t id) {
    if ((this->m_flags & BASE_LOADED) != 0 && id == 0x37) {
        M2Array<uint16_t>* lookup = this->m_animState->m_attachmentLookup;
        if (lookup->Count() < 0x38) {
            return 0xb;
        }
        id = 0xb;
        if ((*lookup)[0x37] < this->m_animState->m_attachments->Count()) {
            id = 0x37;
        }
    }
    return id;
}

// ref: FUN_1411c5ad0
void CM2Model::RegisterRayTracing() {
    void* rayTracing = M2SceneGetRayTracing(this->m_scene);
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if (this->m_shared->m_physics == nullptr || rayTracing == nullptr) {
        return;
    }
    FROST_UNIMPLEMENTED();  // the physics shape's kind (FUN_1412606e0) and its registration
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1402cce50=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1411c6d30=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1411cdb90=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412606e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14127e5b0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_14228b740=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_142295360=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1411cda20 = (void*)&BoxUnion;
// ---- end of generated globals ----
