#include <intrin.h>

#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "base/ObjectAlloc.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "texture/Texture.hpp"

// Engine/Source/Model2/BaseModel.cpp.

namespace {

// DAT_14261f708, DAT_14261f70c: an empty box's corners.
constexpr float BOX_EMPTY_MIN = 3.4028235e+38f;
constexpr float BOX_EMPTY_MAX = -3.4028235e+38f;
// DAT_14261a954
constexpr float LENGTH_EPSILON = 2.3841858e-07f;
// DAT_14261a95c: below this a matrix is not inverted.
constexpr float DETERMINANT_EPSILON = 1.0e-06f;
// DAT_1426c6f7c, DAT_142618d64
constexpr float SCALE_EPSILON = 1.1920929e-07f;

void SetIdentity(C44Matrix& m) {
    MatrixIdentity(m.m);
}

M2ModelCall* AllocCall() {
    return static_cast<M2ModelCall*>(M2GetCallPool()->Alloc());
}

void QueueCall(CM2BaseModel* model, M2ModelCall* call) {
    *model->m_callsTail = call;
    model->m_callsTail = &call->m_next;
}

// A model whose last reference went: off its lists and onto the scene's release list.
void MoveToReleased(CM2BaseModel* model) {
    CM2Scene* scene = model->m_scene;
    model->Unlink();
    CM2BaseModel** head = &scene->m_released;
    model->m_sceneLink = head;
    model->m_sceneNext = *head;
    *head = model;
    if (model->m_sceneNext != nullptr) {
        model->m_sceneNext->m_sceneLink = &model->m_sceneNext;
    }
}

// The camera matrix, or the parent's attachment, a model's matrices start from.
void ParentMatrix(CM2BaseModel* model, C44Matrix* out) {
    CM2BaseModel* parent = model->m_parent;
    if (parent == nullptr || (model->m_flags & CM2BaseModel::BASE_DETACHED_MATRIX) != 0) {
        *out = model->m_scene->m_camera;
    } else {
        bool none = model->m_attachIndex == 0xffff;
        int32_t index = none ? 0 : static_cast<int32_t>(model->m_attachIndex);
        parent->AnimateAttachment(index, none);
        model->m_parent->GetAttachmentWorld(index, none, out);
    }
}

}  // namespace

// ref: FUN_1402c8fb0
int32_t M2RefIncrement(int32_t* count) {
    return _InterlockedIncrement(reinterpret_cast<volatile long*>(count));
}

// ref: FUN_1402c8fc0
int32_t M2RefDecrement(int32_t* count) {
    return _InterlockedDecrement(reinterpret_cast<volatile long*>(count));
}

// The call's resources go back (FUN_1411ad630): textures it held.
// ref: FUN_1411ad630
void M2ModelCallRelease(M2ModelCall* call) {
    HTEXTURE* textures = reinterpret_cast<HTEXTURE*>(&call->m_args[2]);
    if (call->m_type == 0) {
        if (textures[0] != nullptr) {
            TextureFree(&textures[0]);
        }
        for (int32_t i = 1; i < 6; i++) {
            if (textures[i] != nullptr) {
                TextureFree(&textures[i]);
            }
        }
    }
    if (call->m_type == 1 && textures[0] != nullptr) {
        TextureFree(&textures[0]);
    }
}

// ref: FUN_1411e6b40
void M2ModelCallFree(M2ModelCall** call) {
    M2ModelCall* node = *call;
    M2ModelCallRelease(node);
    M2GetCallPool()->Free(node);
    *call = nullptr;
}

// ref: FUN_1411e75d0
M2ModelCall* M2ModelCallAlloc() {
    M2ModelCall* call = AllocCall();
    if (call != nullptr) {
        call->m_type = -1;
    }
    return call;
}

// ref: FUN_1411e5ad0
CM2BaseModel::CM2BaseModel() {
    this->m_0d = 0;
    this->m_type = 4;
    this->m_parent = nullptr;
    this->m_scene = nullptr;
    this->m_flags = (this->m_flags & 0x80001082) | 0x1082;
    this->m_sceneLink = nullptr;
    this->m_sceneNext = nullptr;
    this->m_flags2 = (this->m_flags2 & 0xf8) | 8;
    this->m_bucketLink = nullptr;
    this->m_bucketNext = nullptr;
    this->m_children = nullptr;
    this->m_siblingLink = nullptr;
    this->m_siblingNext = nullptr;
    this->m_extraMatrix = nullptr;
    this->m_calls = nullptr;
    this->m_callsTail = nullptr;
    this->m_attachId = -1;
    this->m_attachIndex = 0xffff;
    this->m_refCount = 0;
    this->m_createFlags = 0;
    SetIdentity(this->m_world);
    SetIdentity(this->m_view);
    SetIdentity(this->m_local);
    SetIdentity(this->m_inverse);
    this->m_bounds.m_min = C3Vector{BOX_EMPTY_MIN, BOX_EMPTY_MIN, BOX_EMPTY_MIN};
    this->m_bounds.m_max = C3Vector{BOX_EMPTY_MAX, BOX_EMPTY_MAX, BOX_EMPTY_MAX};
    this->m_center = C3Vector{0.0f, 0.0f, 0.0f};
    this->m_radius = 0.0f;
    this->m_fileBounds.m_min = C3Vector{BOX_EMPTY_MIN, BOX_EMPTY_MIN, BOX_EMPTY_MIN};
    this->m_fileBounds.m_max = C3Vector{BOX_EMPTY_MAX, BOX_EMPTY_MAX, BOX_EMPTY_MAX};
    this->m_collisionCenter = C3Vector{0.0f, 0.0f, 0.0f};
    this->m_collisionSphereRadius = 0.0f;
    std::memset(&this->m_lighting, 0, 0xa0);
    this->m_lighting.m_maxLights = 8;
    this->m_lighting.m_fogIndex = 0;
    this->m_lighting.m_plane[0] = 0.0f;
    this->m_lighting.m_plane[1] = 0.0f;
    this->m_lighting.m_plane[2] = 1.0f;
    this->m_lighting.m_plane[3] = 0.0f;
    this->m_lightingUsed = nullptr;
    this->m_lightingCallback = nullptr;
    this->m_lightingParam = nullptr;
    this->m_alpha = 1.0f;
    this->m_32c = 1.0f;
    this->m_330 = 1.0f;
    this->m_334 = 1.0f;
    this->m_matrixFrame = 0xffffffff;
    this->m_lodMin = 0;
    this->m_lod = 0;
    this->m_lodTarget = 0;
    this->m_lodRequested = 0;
    // FUN_1411acff0
    this->m_params.m_flags &= ~1u;
    this->m_params.m_effect = nullptr;
    this->m_params.m_matrix = nullptr;
    this->m_params.m_alpha = 1.0f;
    this->m_params.m_colorAlpha = 1.0f;
    this->m_params.m_fade = 1.0f;
    this->m_params.m_color = C3Vector{1.0f, 1.0f, 1.0f};
    this->m_params.m_colorAdd = C3Vector{0.0f, 0.0f, 0.0f};
    this->m_createTime = 0;
    this->m_collisionBounds.m_min = C3Vector{BOX_EMPTY_MIN, BOX_EMPTY_MIN, BOX_EMPTY_MIN};
    this->m_collisionBounds.m_max = C3Vector{BOX_EMPTY_MAX, BOX_EMPTY_MAX, BOX_EMPTY_MAX};
    this->m_loadCallback = nullptr;
    this->m_loadParam = nullptr;
    this->m_impact = nullptr;
    this->m_33c = 0;
}

// ref: FUN_1411e5ec0
CM2BaseModel::~CM2BaseModel() {
    if (this->m_impact != nullptr) {
        M2GetImpactPool()->Free(this->m_impact);
        this->m_impact = nullptr;
    }
}

// ref: FUN_1411e5fa0
void CM2BaseModel::QueueAttachChild(CM2BaseModel* child, uint32_t id, bool flag) {
    M2ModelCall* call = M2ModelCallAlloc();
    if (call == nullptr) {
        return;
    }
    call->m_next = nullptr;
    call->m_type = 0x18;
    call->m_frame = this->m_scene->m_time;
    M2RefIncrement(&child->m_refCount);
    *reinterpret_cast<CM2BaseModel**>(&call->m_args[0]) = child;
    call->m_args[2] = id;
    *reinterpret_cast<uint8_t*>(&call->m_args[3]) = flag;
    QueueCall(this, call);
}

// ref: FUN_1411e6070
void CM2BaseModel::AddRef() {
    M2RefIncrement(&this->m_refCount);
}

// ref: FUN_1411e6080
void CM2BaseModel::QueueCallback(CM2BaseModel* target, void* callback) {
    M2ModelCall* call = M2ModelCallAlloc();
    if (call == nullptr) {
        return;
    }
    call->m_next = nullptr;
    call->m_type = 0x1b;
    call->m_frame = this->m_scene->m_time;
    M2RefIncrement(&target->m_refCount);
    *reinterpret_cast<CM2BaseModel**>(&call->m_args[0]) = target;
    *reinterpret_cast<void**>(&call->m_args[2]) = callback;
    QueueCall(this, call);
}

// ref: FUN_1411e6140
void CM2BaseModel::QueueAttachChildAt(CM2BaseModel* child, uint32_t a3, uint32_t a4, bool flag) {
    M2ModelCall* call = M2ModelCallAlloc();
    if (call == nullptr) {
        return;
    }
    call->m_next = nullptr;
    call->m_type = 0x1d;
    call->m_frame = this->m_scene->m_time;
    M2RefIncrement(&child->m_refCount);
    *reinterpret_cast<uint8_t*>(&call->m_args[4]) = flag;
    call->m_args[2] = a3;
    *reinterpret_cast<CM2BaseModel**>(&call->m_args[0]) = child;
    call->m_args[3] = a4;
    QueueCall(this, call);
}

// ref: FUN_1411e6220
bool CM2BaseModel::AttachTo(CM2BaseModel* parent, uint32_t id, const float* position, bool flag) {
    if ((parent->m_flags & BASE_LOADED) == 0) {
        parent->QueueAttachChild(this, id, flag);
    }
    this->m_attachIndex = 0xffff;
    M2RefIncrement(&this->m_refCount);
    if (this->m_parent != nullptr) {
        this->Detach(true);
    }
    this->m_parent = parent;
    this->m_attachId = static_cast<int32_t>(id);
    this->m_flags = static_cast<uint32_t>(flag) << 24 | (this->m_flags & 0xfeffffff) | BASE_ATTACH_SHOWN;
    CM2BaseModel** head = &parent->m_children;
    this->m_siblingLink = head;
    this->m_attachIndex = id;
    this->m_siblingNext = *head;
    if (this->m_siblingNext != nullptr) {
        this->m_siblingNext->m_siblingLink = &this->m_siblingNext;
    }
    *head = this;
    if ((static_cast<uint8_t>(this->m_flags) & 3) != 3) {
        for (CM2BaseModel* p = parent; p != nullptr; p = p->m_parent) {
            p->m_flags &= ~BASE_CHILDREN_LOADED;
        }
    }
    if ((static_cast<uint8_t>(this->m_flags) & 0x60) != 0x60) {
        for (CM2BaseModel* p = parent; p != nullptr; p = p->m_parent) {
            p->m_flags &= ~BASE_READY_CHECKED;
        }
    }
    if ((this->m_flags & BASE_TEXTURES_READY) == 0) {
        for (CM2BaseModel* p = parent; p != nullptr; p = p->m_parent) {
            p->m_flags &= ~BASE_ALL_READY;
        }
    }
    if (position != nullptr && (this->m_parent->m_flags & BASE_LOADED) != 0 && this->m_attachIndex != 0xffff) {
        C44Matrix attach;
        this->m_parent->GetAttachmentMatrixByIndex(id, &attach);
        float* m = attach.m;
        MatrixAffineInverse(m, std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]));
        float x = position[0];
        float y = position[1];
        float z = position[2];
        this->m_local.m[12] = y * m[4] + x * m[0] + z * m[8] + m[12];
        this->m_local.m[13] = y * m[5] + x * m[1] + z * m[9] + m[13];
        this->m_local.m[14] = y * m[6] + x * m[2] + z * m[10] + m[14];
    }
    this->SetVisibleFlags((this->m_parent->m_flags >> 2 & 1) != 0, false, false);
    return true;
}

// ref: FUN_1411e6430
void CM2BaseModel::AddToScene(CM2Scene* scene) {
    if (this->m_scene != nullptr) {
        this->RemoveFromScene();
    }
    CM2BaseModel** head = &scene->m_models;
    this->m_scene = scene;
    this->m_sceneLink = head;
    this->m_sceneNext = *head;
    *head = this;
    if (this->m_sceneNext != nullptr) {
        this->m_sceneNext->m_sceneLink = &this->m_sceneNext;
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        for (M2ModelCall* call = this->m_calls; call != nullptr; call = call->m_next) {
            call->m_frame += this->m_scene->m_time;
        }
    }
}

// Into another scene with every model attached to it (FUN_1411e64c0).
// ref: FUN_1411e64c0
void M2BaseModelMoveToScene(CM2BaseModel* model, CM2Scene* scene) {
    model->AddToScene(scene);
    for (CM2BaseModel* child = model->m_children; child != nullptr; child = child->m_siblingNext) {
        M2BaseModelMoveToScene(child, scene);
    }
}

// ref: FUN_1411e6510
void CM2BaseModel::FreeCalls() {
    M2ModelCall** head = &this->m_calls;
    M2ModelCall* call = *head;
    if (call == nullptr) {
        this->m_callsTail = head;
        return;
    }
    do {
        if (call->m_type == 0x18 || call->m_type == 0x1b || call->m_type == 0x1d) {
            CM2BaseModel* target = *reinterpret_cast<CM2BaseModel**>(&call->m_args[0]);
            if (M2RefDecrement(&target->m_refCount) == 0) {
                MoveToReleased(target);
            }
            call->m_args[0] = 0;
            call->m_args[1] = 0;
        }
        *head = call->m_next;
        M2ModelCallRelease(call);
        M2GetCallPool()->Free(call);
        call = *head;
    } while (call != nullptr);
    this->m_callsTail = head;
}

// ref: FUN_1411e6600
void CM2BaseModel::OnLodChanged() {
    this->m_lod = this->m_lodTarget;
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->OnLodChanged();
    }
}

// Whether either model has an impact (FUN_1411e6640).
// ref: FUN_1411e6640
bool M2BaseModelEitherHasImpact(CM2BaseModel* a, CM2BaseModel* b) {
    if (a->m_impact == nullptr) {
        return b->m_impact != nullptr;
    }
    return true;
}

// Takes the parent's LOD; true when it changed (FUN_1411e6660).
// ref: FUN_1411e6660
bool M2BaseModelInheritLod(CM2BaseModel* model) {
    uint8_t requested = model->m_parent->m_lodRequested;
    model->m_lodRequested = requested;
    uint8_t min = model->m_parent->m_lodMin;
    model->m_lodMin = min;
    if (requested <= min) {
        requested = min;
    }
    model->m_lodTarget = requested;
    uint8_t max = model->GetMaxLod();
    if (max <= requested) {
        requested = model->GetMaxLod();
    }
    model->m_lodTarget = requested;
    return requested != model->m_lod;
}

// ref: FUN_1411e66e0
void CM2BaseModel::UpdateCulling() {
    void* frustum = this->m_scene->m_frustum;
    uint32_t flags = this->m_flags;
    if (frustum != nullptr && (flags & 0x1001) == 0x1001 && (flags >> 13 & 1) == 0 && (flags & BASE_VISIBLE) != 0) {
        float box[6];
        this->GetBounds(box);
        if (box[0] <= box[3] && box[1] <= box[4] && box[2] <= box[5]) {
            int32_t inside = M2FrustumTestBox(static_cast<const float*>(frustum), box);
            this->m_flags = (inside == 0 ? BASE_CULLED : 0u) | (this->m_flags & ~BASE_CULLED);
            return;
        }
        this->m_flags |= BASE_CULLED;
        return;
    }
    this->m_flags &= ~BASE_CULLED;
}

// ref: FUN_1411e67b0
void CM2BaseModel::Unlink() {
    this->OnRemove();
    if (this->m_bucketLink != nullptr) {
        *this->m_bucketLink = this->m_bucketNext;
    }
    if (this->m_bucketNext != nullptr) {
        this->m_bucketNext->m_bucketLink = this->m_bucketLink;
    }
    CM2BaseModel* child = this->m_children;
    this->m_bucketLink = nullptr;
    this->m_bucketNext = nullptr;
    this->m_flags = (this->m_flags & ~BASE_SHOWN) | BASE_CULLED;
    this->m_loadCallback = nullptr;
    while (child != nullptr) {
        child->Detach(true);
        child = this->m_children;
    }
    this->RemoveFromScene();
    if (this->m_siblingLink != nullptr) {
        *this->m_siblingLink = this->m_siblingNext;
    }
    if (this->m_siblingNext != nullptr) {
        this->m_siblingNext->m_siblingLink = this->m_siblingLink;
    }
    this->m_siblingNext = nullptr;
    this->m_siblingLink = nullptr;
}

// ref: FUN_1411e68e0
void CM2BaseModel::Detach(bool notify) {
    if (this->m_siblingLink != nullptr) {
        *this->m_siblingLink = this->m_siblingNext;
    }
    if (this->m_siblingNext != nullptr) {
        this->m_siblingNext->m_siblingLink = this->m_siblingLink;
    }
    CM2BaseModel* parent = nullptr;
    if (notify) {
        if ((this->m_flags & BASE_LOADED) == 0) {
            parent = this->m_parent;
        } else if ((this->m_flags & BASE_CHILDREN_LOADED) == 0) {
            bool loaded = true;
            for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
                if (!child->AreChildrenLoaded()) {
                    loaded = false;
                    break;
                }
            }
            if (loaded) {
                this->m_flags |= BASE_CHILDREN_LOADED;
            } else {
                parent = this->m_parent;
            }
        }
    }
    this->m_flags &= ~BASE_VISIBLE;
    this->m_siblingLink = nullptr;
    this->m_siblingNext = nullptr;
    this->m_parent = nullptr;
    this->m_attachId = -1;
    if (parent != nullptr) {
        parent->RunLoadCallbacks();
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->OnChildrenChanged();
    }
    if (M2RefDecrement(&this->m_refCount) == 0) {
        MoveToReleased(this);
    }
}

// ref: FUN_1411e6a10
void CM2BaseModel::RemoveFromScene() {
    if (this->m_sceneLink != nullptr) {
        *this->m_sceneLink = this->m_sceneNext;
    }
    if (this->m_sceneNext != nullptr) {
        this->m_sceneNext->m_sceneLink = this->m_sceneLink;
    }
    this->m_sceneLink = nullptr;
    this->m_sceneNext = nullptr;
    if ((this->m_flags & BASE_LOADED) == 0 && this->m_scene != nullptr && this->m_calls != nullptr) {
        for (M2ModelCall* call = this->m_calls; call != nullptr; call = call->m_next) {
            call->m_frame -= this->m_scene->m_time;
        }
    }
    this->m_scene = nullptr;
}

// Every model attached below this one is told its parent changed (FUN_1411e6a90).
// ref: FUN_1411e6a90
void M2BaseModelNotifyTree(CM2BaseModel* model) {
    for (CM2BaseModel* child = model->m_children; child != nullptr; child = child->m_siblingNext) {
        child->DisableLights();
        M2BaseModelNotifyTree(child);
    }
}

// ref: FUN_1411e6ac0
void CM2BaseModel::InvalidateMatrices() {
    uint32_t flags = this->m_flags;
    bool visible;
    if (this->m_parent == nullptr || (flags >> 14 & 1) != 0) {
        flags = this->m_flags;
        if ((flags & BASE_VISIBLE) != 0
            && (this->m_matrixFrame != this->m_scene->m_frame || (flags >> 9 & 1) == 0)) {
            this->OnMatricesUpdated();
            return;
        }
        visible = true;
    } else {
        if (this->m_matrixFrame != this->m_scene->m_frame || (flags >> 9 & 1) == 0) {
            this->OnMatricesUpdated();
            return;
        }
        visible = (flags >> 2 & 1) != 0;
    }
    this->SetVisibleFlags(visible, false, false);
    this->OnMatricesUpdated();
}

// The load state of the model and everything attached: the lowest (FUN_1411e6b90).
// ref: FUN_1411e6b90
int32_t CM2BaseModel::GetTreeLoadState() {
    int32_t state = this->GetLoadState();
    if (state == 2) {
        if ((this->m_flags & BASE_READY_CHECKED) != 0) {
            return state;
        }
        for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
            int32_t childState = child->GetTreeLoadState();
            if (state < childState) {
                childState = state;
            }
            state = childState;
            if (state == 0) {
                return 0;
            }
        }
        if (state != 2) {
            return state;
        }
        this->m_flags |= BASE_READY_CHECKED;
    }
    return state;
}

// ref: FUN_1411e6c20
bool CM2BaseModel::GetAttachmentMatrixByIndex(uint32_t index, C44Matrix* out) {
    (void)index;
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if (this->m_parent != nullptr) {
        *out = this->m_view;
        return false;
    }
    *out = this->m_local;
    return false;
}

// The effect the model draws with (FUN_1411e6cc0).
// ref: FUN_1411e6cc0
void* M2BaseModelGetEffect(CM2BaseModel* model) {
    return model->m_params.m_effect;
}

// ref: FUN_1411e6cd0
uint32_t CM2BaseModel::MapAttachmentId(uint32_t id) {
    return id;
}

// ref: FUN_1411e6ce0
const char* CM2BaseModel::GetName() {
    return "BaseModel";
}

const char* CM2BaseModel::GetName2(char* buffer, size_t size) {
    (void)buffer;
    (void)size;
    return "BaseModel";
}

const char* CM2BaseModel::GetName3(char* buffer, size_t size) {
    (void)buffer;
    (void)size;
    return "BaseModel";
}

// ref: FUN_1411e6cf0
float CM2BaseModel::GetScaledRadius(C3Vector* center, float* radius) {
    float x;
    float scaleSq;
    if (this->m_parent == nullptr || (this->m_flags >> 14 & 1) != 0) {
        *center = C3Vector{this->m_local.m[12], this->m_local.m[13], this->m_local.m[14]};
        x = this->m_local.m[1] * this->m_local.m[1];
        scaleSq = this->m_local.m[0] * this->m_local.m[0] + x + this->m_local.m[2] * this->m_local.m[2];
    } else {
        *center = C3Vector{this->m_view.m[12], this->m_view.m[13], this->m_view.m[14]};
        x = this->m_view.m[2] * this->m_view.m[2];
        scaleSq = this->m_view.m[3] * this->m_view.m[3] + x + this->m_view.m[0] * this->m_view.m[0]
            + this->m_view.m[1] * this->m_view.m[1];
    }
    if (LENGTH_EPSILON < scaleSq) {
        *radius = std::sqrt(scaleSq);
        return scaleSq;
    }
    *radius = 0.0f;
    return x;
}

// ref: FUN_1411e6dd0
float* CM2BaseModel::GetBox(float* out) {
    out[0] = -1.0f;
    out[1] = -1.0f;
    out[2] = -1.0f;
    out[3] = 1.0f;
    out[4] = 1.0f;
    out[5] = 1.0f;
    return out;
}

float* CM2BaseModel::GetBox2(float* out) {
    return CM2BaseModel::GetBox(out);
}

// The box around the model and everything attached, in its own space (FUN_1411e6e00).
// ref: FUN_1411e6e00
float* CM2BaseModel::GetTreeBounds(float* out) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    out[0] = this->m_bounds.m_min.x;
    out[1] = this->m_bounds.m_min.y;
    out[2] = this->m_bounds.m_min.z;
    out[3] = this->m_bounds.m_max.x;
    out[4] = this->m_bounds.m_max.y;
    out[5] = this->m_bounds.m_max.z;
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        float box[6];
        child->GetTreeBounds(box);
        if (box[0] <= box[3] || box[1] <= box[4] || box[2] <= box[5]) {
            float scale = std::sqrt(child->m_local.m[0] * child->m_local.m[0]
                + child->m_local.m[1] * child->m_local.m[1] + child->m_local.m[2] * child->m_local.m[2]);
            float ez = ((box[5] - box[2]) + child->m_local.m[14]) * scale;
            float ey = ((box[4] - box[1]) + child->m_local.m[13]) * scale;
            float ex = ((box[3] - box[0]) + child->m_local.m[12]) * scale;
            float grown[6] = {
                this->m_bounds.m_min.x - ex,
                this->m_bounds.m_min.y - ey,
                this->m_bounds.m_min.z - ez,
                this->m_bounds.m_max.x + ex,
                this->m_bounds.m_max.y + ey,
                this->m_bounds.m_max.z + ez,
            };
            box[3] = grown[3] < out[3] ? out[3] : grown[3];
            box[4] = grown[4] < out[4] ? out[4] : grown[4];
            box[5] = grown[5] < out[5] ? out[5] : grown[5];
            box[0] = out[0] <= grown[0] ? out[0] : grown[0];
            box[1] = out[1] <= grown[1] ? out[1] : grown[1];
            box[2] = out[2] <= grown[2] ? out[2] : grown[2];
            for (int32_t i = 0; i < 6; i++) {
                out[i] = box[i];
            }
        }
    }
    return out;
}

// ref: FUN_1411e7080
float* CM2BaseModel::GetSphere(float* out) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    return out;
}

float* CM2BaseModel::GetSphere2(float* out) {
    return CM2BaseModel::GetSphere(out);
}

// The sphere around the model and everything attached (FUN_1411e70a0).
// ref: FUN_1411e70a0
float* CM2BaseModel::GetTreeSphere(float* out) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    out[0] = this->m_center.x;
    out[1] = this->m_center.y;
    out[2] = this->m_center.z;
    out[3] = this->m_radius;
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        float sphere[4];
        child->GetTreeSphere(sphere);
        float reach = (std::sqrt(child->m_local.m[12] * child->m_local.m[12]
                           + child->m_local.m[13] * child->m_local.m[13]
                           + child->m_local.m[14] * child->m_local.m[14])
                          + sphere[3])
                * std::sqrt(child->m_local.m[0] * child->m_local.m[0] + child->m_local.m[1] * child->m_local.m[1]
                    + child->m_local.m[2] * child->m_local.m[2])
            + out[3];
        if (out[3] <= reach && reach != out[3]) {
            out[3] = reach;
        }
    }
    return out;
}

// ref: FUN_1411e71a0
bool CM2BaseModel::IsShownInTree() {
    if (this->m_parent == nullptr) {
        return (this->m_flags >> 3 & 1) != 0;
    }
    return this->m_parent->IsShownInTree() && (this->m_flags & BASE_ATTACH_SHOWN) != 0;
}

// ref: FUN_1411e71e0
void CM2BaseModel::GetRenderMatrix(C44Matrix* out) {
    if (this->m_parent != nullptr && (this->m_flags >> 14 & 1) == 0) {
        *out = this->m_view;
        return;
    }
    *out = this->m_local;
}

// ref: FUN_1411e7250
C3Vector* CM2BaseModel::GetRenderPosition(C3Vector* out) {
    if (this->m_parent != nullptr && (this->m_flags >> 14 & 1) == 0) {
        *out = C3Vector{this->m_view.m[12], this->m_view.m[13], this->m_view.m[14]};
        return out;
    }
    *out = C3Vector{this->m_local.m[12], this->m_local.m[13], this->m_local.m[14]};
    return out;
}

// ref: FUN_1411e72a0
float CM2BaseModel::GetRenderScale() {
    float x;
    float y;
    float z;
    if (this->m_parent == nullptr || (this->m_flags >> 14 & 1) != 0) {
        y = this->m_local.m[1];
        x = this->m_local.m[0];
        z = this->m_local.m[2];
    } else {
        y = this->m_view.m[1];
        x = this->m_view.m[0];
        z = this->m_view.m[2];
    }
    return std::sqrt(x * x + y * y + z * z);
}

// Loaded: the model's own state, then the textures (FUN_1411e74c0).
// ref: FUN_1411e74c0
bool M2BaseModelIsLoadedAndReady(CM2BaseModel* model) {
    uint32_t flags = model->m_flags;
    if ((flags & CM2BaseModel::BASE_LOADED) == 0) {
        return false;
    }
    if ((flags >> 8 & 1) != 0) {
        return true;
    }
    return model->AreTexturesReady();
}

// ref: FUN_1411e74e0
bool CM2BaseModel::IsReady() {
    uint32_t flags = this->m_flags;
    if ((flags & BASE_LOADED) == 0) {
        return false;
    }
    if ((flags >> 8 & 1) == 0) {
        uint32_t ready = this->AreTexturesReady() ? 1 : 0;
        flags = ((ready & 0xff) << 8 ^ this->m_flags) & BASE_TEXTURES_READY ^ this->m_flags;
        this->m_flags = flags;
        if ((ready & 1) == 0) {
            return false;
        }
    }
    if (static_cast<int8_t>(flags) >= 0) {
        for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
            if (!child->IsReady()) {
                return false;
            }
        }
        this->m_flags |= BASE_ALL_READY;
    }
    return true;
}

// ref: FUN_1411e7570
bool CM2BaseModel::AreChildrenLoaded() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        return false;
    }
    if ((this->m_flags & BASE_CHILDREN_LOADED) == 0) {
        for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
            if (!child->AreChildrenLoaded()) {
                return false;
            }
        }
        this->m_flags |= BASE_CHILDREN_LOADED;
    }
    return true;
}

// ref: FUN_1411e7630
void CM2BaseModel::OnChildrenChanged() {
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->OnChildrenChanged();
    }
}

// ref: FUN_1411e7660
void CM2BaseModel::PrepareLighting(int32_t pass, uint32_t propagate) {
    uint32_t flags = this->m_flags;
    if ((flags & BASE_LOADED) != 0 && (flags >> 23 & 1) == 0 && ((flags & BASE_SHOWN) != 0 || (flags >> 21 & 1) == 0)) {
        CM2BaseModel* parent = this->m_parent;
        CM2ModelLighting* lighting;
        if (parent == nullptr) {
            lighting = &this->m_lighting;
        } else {
            flags = (flags ^ parent->m_flags) & BASE_VISIBLE ^ flags;
            this->m_flags = flags;
            if ((parent->m_createFlags & 1) != 0) {
                lighting = &this->m_lighting;
            } else {
                lighting = parent->m_lightingUsed;
            }
        }
        this->m_lightingUsed = lighting;
        if (lighting == nullptr) {
            this->m_lightingUsed = &this->m_lighting;
        }
    }
    if ((flags & BASE_SHOWN) != 0 && this->m_scene->m_collectDrawParams) {
        BaseModelDrawParams* params = M2SceneAddDrawParams(
            reinterpret_cast<uint8_t*>(this->m_scene) + (static_cast<int64_t>(pass) * 3 + 0x65) * 8);
        params->m_model = this;
        float box[6];
        this->GetBounds(box);
        for (int32_t i = 0; i < 6; i++) {
            params->m_box[i] = box[i];
        }
    }
    propagate = propagate | (this->m_flags2 >> 1 & 1);
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        bool hidden;
        if (child->m_attachIndex == 0xffff) {
            hidden = (child->m_flags & BASE_ATTACHED_HIDDEN) == 0;
        } else {
            hidden = !this->IsAttachmentVisible(child->m_attachIndex);
        }
        if (!hidden) {
            child->PrepareLighting(pass, propagate);
        }
    }
    if (propagate != 0) {
        this->UpdateCulling();
    }
}

// ref: FUN_1411e77d0
void CM2BaseModel::ClearLoaded() {
    this->m_flags &= ~BASE_LOADED;
}

// ref: FUN_1411e77e0
void CM2BaseModel::SetVisibleFlags(bool visible, bool a3, bool a4) {
    this->m_flags = (((static_cast<uint32_t>(visible) & 0xffffff7f) | static_cast<uint32_t>(a3) << 7) << 2
                        | (this->m_flags & 0xfffffdfb))
            & 0xfffffbff
        | static_cast<uint32_t>(a4) << 10;
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->SetVisibleFlags(visible, a3, a4);
    }
}

// ref: FUN_1411e7880
uint32_t CM2BaseModel::Release(uint32_t count) {
    (void)count;
    int32_t left = M2RefDecrement(&this->m_refCount);
    if (left == 0) {
        MoveToReleased(this);
    }
    return static_cast<uint32_t>(left);
}

// An attachment the cache's update made later (0x18 records, FUN_141197f90): attaches and drops
// the reference the record held.
// ref: FUN_1411e78f0
void M2BaseModelAttachPending(CM2BaseModel* model, const M2PendingAttach* attach) {
    model->AttachTo(attach->m_parent, attach->m_id, nullptr, false);
    if (M2RefDecrement(&model->m_refCount) == 0) {
        MoveToReleased(model);
    }
}

// ref: FUN_1411e7970
void CM2BaseModel::SetBucketed(int32_t bucketed) {
    if (bucketed == 0) {
        if (this->m_bucketLink != nullptr) {
            *this->m_bucketLink = this->m_bucketNext;
            if (this->m_bucketNext != nullptr) {
                this->m_bucketNext->m_bucketLink = this->m_bucketLink;
            }
            this->m_bucketLink = nullptr;
            this->m_bucketNext = nullptr;
        }
    } else if (this->m_bucketLink == nullptr) {
        CM2Scene* scene = this->m_scene;
        uint32_t bucket;
        if (!scene->m_bucketModels) {
            bucket = 0;
        } else {
            bucket = scene->m_bucketCounter & 0x3f;
            scene->m_bucketCounter = scene->m_bucketCounter + 1;
        }
        CM2BaseModel** head = &this->m_scene->m_buckets[bucket];
        this->m_bucketLink = head;
        this->m_bucketNext = *head;
        *head = this;
        if (this->m_bucketNext != nullptr) {
            this->m_bucketNext->m_bucketLink = &this->m_bucketNext;
        }
    }
}

// ref: FUN_1411e7a10
void CM2BaseModel::SetDetachedMatrix(int32_t detached) {
    this->m_flags = (detached != 0 ? BASE_DETACHED_MATRIX : 0u) | (this->m_flags & ~BASE_DETACHED_MATRIX);
}

// Requests a LOD, at least the minimum and at most the model's (FUN_1411e7a30).
// ref: FUN_1411e7a30
void M2BaseModelSetLodNow(CM2BaseModel* model, uint8_t lod) {
    uint8_t value = lod < 7 ? lod : 7;
    model->m_lodRequested = value;
    if (value <= model->m_lodMin) {
        value = model->m_lodMin;
    }
    model->m_lodTarget = value;
    uint8_t max = model->GetMaxLod();
    if (value < max) {
        model->m_lodTarget = value;
        model->m_lod = value;
        return;
    }
    uint8_t clamped = model->GetMaxLod();
    model->m_lodTarget = clamped;
    model->m_lod = clamped;
}

// The same without applying it; true when it differs from the current LOD (FUN_1411e7ac0).
// ref: FUN_1411e7ac0
bool M2BaseModelRequestLod(CM2BaseModel* model, uint8_t lod) {
    uint8_t value = lod < 7 ? lod : 7;
    model->m_lodRequested = value;
    if (value <= model->m_lodMin) {
        value = model->m_lodMin;
    }
    model->m_lodTarget = value;
    uint8_t max = model->GetMaxLod();
    if (max <= value) {
        value = model->GetMaxLod();
    }
    model->m_lodTarget = value;
    return value != model->m_lod;
}

// ref: FUN_1411e7b40
void CM2BaseModel::SetImpact(const C3Vector* position, const uint32_t* data, uint32_t a4) {
    uint8_t* impact = static_cast<uint8_t*>(this->m_impact);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    if (impact == nullptr) {
        if (bytes[3] == 0) {
            return;
        }
        impact = static_cast<uint8_t*>(M2GetImpactPool()->Alloc());
        if (impact != nullptr) {
            std::memset(impact, 0, 0x14);
        }
        this->m_impact = impact;
        if (impact == nullptr) {
            return;
        }
    }
    if (bytes[3] == 0) {
        M2GetImpactPool()->Free(impact);
        this->m_impact = nullptr;
    } else {
        std::memcpy(impact, position, sizeof(C3Vector));
        *reinterpret_cast<uint32_t*>(impact + 0xc) = *data;
        *reinterpret_cast<uint32_t*>(impact + 0x10) = a4;
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->SetImpact(position, data, a4);
    }
}

// ref: FUN_1411e7c70
void CM2BaseModel::SetLoadCallback(void (*callback)(CM2BaseModel* model, void* param), void* param) {
    this->m_loadCallback = callback;
    this->m_loadParam = param;
    this->RunLoadCallbacks();
}

// ref: FUN_1411e7c90
void CM2BaseModel::SetLod(uint32_t lod, bool notify) {
    if (this->SetLodTree(lod < 7 ? lod : 7) && notify) {
        this->OnLodChanged();
    }
}

// ref: FUN_1411e7ce0
bool CM2BaseModel::SetLodTree(uint32_t lod) {
    uint8_t value = static_cast<uint8_t>(lod) < 7 ? static_cast<uint8_t>(lod) : 7;
    this->m_lodRequested = value;
    if (value <= this->m_lodMin) {
        value = this->m_lodMin;
    }
    this->m_lodTarget = value;
    uint8_t max = this->GetMaxLod();
    if (max <= value) {
        value = this->GetMaxLod();
    }
    bool changed = value != this->m_lod;
    this->m_lodTarget = value;
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        changed = child->SetLodTree(lod) | changed;
    }
    return changed;
}

// ref: FUN_1411e7d90
void CM2BaseModel::SetLodMin(uint8_t lod) {
    this->m_lodMin = lod < 7 ? lod : 7;
}

// ref: FUN_1411e7db0
void CM2BaseModel::SetFlag400000(bool set) {
    this->m_flags = static_cast<uint32_t>(set) << 22 | (this->m_flags & 0xffbfffff);
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->SetFlag400000(set);
    }
}

// ref: FUN_1411e7e00
void CM2BaseModel::SetShadowed(bool set) {
    if ((this->m_flags >> 26 & 1) != static_cast<uint32_t>(set)) {
        this->m_flags = static_cast<uint32_t>(set) << 26 | (this->m_flags & ~BASE_SHADOWED);
        for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
            child->SetShadowed((this->m_flags >> 26 & 1) != 0);
        }
    }
}

// ref: FUN_1411e7e70
void CM2BaseModel::SetField330(float value) {
    this->m_330 = value;
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->SetField330(value);
    }
}

// ref: FUN_1411e7eb0
void CM2BaseModel::SetShown(bool shown) {
    if (this->m_parent != nullptr) {
        this->m_flags = static_cast<uint32_t>(shown) << 4 | (this->m_flags & ~BASE_ATTACH_SHOWN);
        return;
    }
    this->m_flags = static_cast<uint32_t>(shown) * 8 | (this->m_flags & ~BASE_SHOWN);
}

// ref: FUN_1411e7ee0
void CM2BaseModel::SetPositionScale(const C3Vector* position, float rotation, float scale) {
    if (scale <= SCALE_EPSILON) {
        scale = 1.0f;
    }
    SetIdentity(this->m_local);
    MatrixRotateZ(this->m_local.m, rotation);
    MatrixScaleUniform(this->m_local.m, scale);
    this->m_local.m[12] = position->x;
    this->m_local.m[13] = position->y;
    this->m_local.m[14] = position->z;
    this->InvalidateMatrices();
}

// ref: FUN_1411e7fa0
void CM2BaseModel::SetLocalMatrix(const float* matrix) {
    if (!std::isfinite(matrix[0]) || !std::isfinite(matrix[4]) || !std::isfinite(matrix[8]) || !std::isfinite(matrix[12])) {
        return;
    }
    if (!(SCALE_EPSILON < matrix[1] * matrix[1] + matrix[0] * matrix[0] + matrix[2] * matrix[2])) {
        return;
    }
    std::memcpy(this->m_local.m, matrix, sizeof(float) * 16);
    this->InvalidateMatrices();
}

// ref: FUN_1411e8090
void CM2BaseModel::UpdateLighting(void* param) {
    CM2BaseModel* parent = this->m_parent;
    if (parent == nullptr || (parent->m_createFlags & 1) != 0) {
        float position[4];
        if (parent == nullptr || (this->m_flags >> 14 & 1) != 0) {
            position[0] = this->m_local.m[12];
            position[1] = this->m_local.m[13];
            position[2] = this->m_local.m[14];
        } else {
            position[0] = this->m_view.m[12];
            position[1] = this->m_view.m[13];
            position[2] = this->m_view.m[14];
        }
        position[3] = 0.0f;
        M2ModelLightingBegin(&this->m_lighting, this->m_scene, position);
        if ((this->m_scene->m_flags & 0x40) == 0) {
            M2SceneGatherLights(this->m_scene, &this->m_lighting);
        }
        if (this->m_lightingCallback != nullptr) {
            this->m_lightingCallback(this, &this->m_lighting, this->m_lightingParam);
        }
        M2ModelLightingFinish(&this->m_lighting, param);
        M2ModelLightingEnd(&this->m_lighting);
    }
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        child->UpdateLighting(param);
    }
}

// ref: FUN_1411e8190
void CM2BaseModel::RunLoadCallbacks() {
    CM2BaseModel* model = this;
    do {
        if ((model->m_flags & BASE_LOADED) == 0) {
            return;
        }
        if ((model->m_flags & BASE_CHILDREN_LOADED) == 0) {
            for (CM2BaseModel* child = model->m_children; child != nullptr; child = child->m_siblingNext) {
                if (!child->AreChildrenLoaded()) {
                    return;
                }
            }
            model->m_flags |= BASE_CHILDREN_LOADED;
        }
        auto callback = model->m_loadCallback;
        if (callback != nullptr) {
            model->m_loadCallback = nullptr;
            callback(model, model->m_loadParam);
        }
        model = model->m_parent;
    } while (model != nullptr);
}

// ref: FUN_1411e8210
void CM2BaseModel::UpdateMatrices(const C44Matrix* parentMatrix, const CM2ModelParams* params, uint32_t a4, uint32_t a5,
    uint32_t a6) {
    (void)parentMatrix;
    CM2BaseModel* parent = this->m_parent;
    uint32_t flags;
    if (parent == nullptr) {
        this->m_flags &= 0xfffbffff;
        flags = this->m_flags & 0xffefffff;
    } else {
        bool shown;
        if (parent->m_parent == nullptr) {
            shown = (parent->m_flags >> 3 & 1) != 0;
        } else {
            shown = parent->IsShownInTree() && (parent->m_flags & BASE_ATTACH_SHOWN) != 0;
        }
        uint32_t current = this->m_flags;
        uint32_t shownBit = 0;
        if (shown && (current & BASE_ATTACH_SHOWN) != 0) {
            shownBit = BASE_SHOWN;
        }
        this->m_flags = (current & ~BASE_SHOWN) | shownBit;
        uint32_t hide = (parent->m_flags & 0x60000) != 0 ? 0x40000u : 0u;
        this->m_flags = hide | (current & 0xfffbfff7) | shownBit;
        flags = ((parent->m_flags & 0x180000) != 0 ? 0x100000u : 0u) | hide | (current & 0xffebfff7) | shownBit;
    }
    this->m_flags = flags;

    C44Matrix base;
    ParentMatrix(this, &base);
    this->SetWorldMatrix(&base);

    this->m_matrixFrame = this->m_scene->m_frame;
    for (CM2BaseModel* child = this->m_children; child != nullptr; child = child->m_siblingNext) {
        C44Matrix m;
        SetIdentity(m);
        float* out = m.m;
        if ((child->m_flags >> 14 & 1) == 0) {
            if (child->m_attachIndex == 0xffff) {
                m = this->m_world;
            } else {
                this->GetAttachmentMatrix(child->m_attachIndex, &m);
            }
            if ((child->m_flags >> 16 & 1) == 0) {
                if ((child->m_flags >> 15 & 1) != 0) {
                    const float* camera = this->m_scene->m_camera.m;
                    float scale = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
                    out[0] = scale * camera[0];
                    out[1] = scale * camera[1];
                    out[2] = scale * camera[2];
                    out[4] = scale * camera[4];
                    out[5] = scale * camera[5];
                    out[6] = scale * camera[6];
                    out[8] = scale * camera[8];
                    out[9] = scale * camera[9];
                    out[10] = scale * camera[10];
                }
            } else {
                const float* camera = this->m_scene->m_camera.m;
                out[0] = camera[0];
                out[1] = camera[1];
                out[2] = camera[2];
                out[4] = camera[4];
                out[5] = camera[5];
                out[6] = camera[6];
                out[8] = camera[8];
                out[9] = camera[9];
                out[10] = camera[10];
            }
        } else {
            m = this->m_scene->m_camera;
        }
        if (child->m_extraMatrix != nullptr) {
            if ((this->m_flags & BASE_INVERSE_CURRENT) == 0) {
                float determinant = MatrixDeterminant(this->m_world.m);
                if (std::fabs(determinant) < DETERMINANT_EPSILON) {
                    SetIdentity(this->m_inverse);
                } else {
                    MatrixInverse(this->m_world.m, this->m_inverse.m, determinant);
                }
                this->m_flags |= BASE_INVERSE_CURRENT;
            }
            // extra * (attachment * inverse(world)) * world, the middle product's last row
            // taken as (0, 0, 0, w) for the extra matrix's rotation rows and its first three
            // columns added after.
            C44Matrix relative;
            MatrixMultiply(relative.m, m.m, this->m_inverse.m);
            const float* r = relative.m;
            const float* e = child->m_extraMatrix->m;
            C44Matrix moved;
            for (int32_t row = 0; row < 4; row++) {
                for (int32_t column = 0; column < 4; column++) {
                    float w = column == 3 ? r[15] : 0.0f;
                    moved.m[row * 4 + column] = e[row * 4 + 2] * r[8 + column] + e[row * 4 + 3] * w
                        + e[row * 4 + 1] * r[4 + column] + e[row * 4] * r[column];
                }
            }
            moved.m[12] += r[12];
            moved.m[13] += r[13];
            moved.m[14] += r[14];
            MatrixMultiply(m.m, moved.m, this->m_world.m);
        }
        child->UpdateMatrices(&m, params, a4, a5, a6);
    }
}

// ref: FUN_1411e8bc0
void CM2BaseModel::RecomputeMatrices() {
    C44Matrix base;
    ParentMatrix(this, &base);
    this->SetWorldMatrix(&base);
}

// ref: FUN_1411e8c80
void CM2BaseModel::SetWorldMatrix(const C44Matrix* parent) {
    if ((this->m_flags & BASE_MATRIX_CURRENT) != 0 && this->m_matrixFrame == this->m_scene->m_frame) {
        return;
    }
    MatrixMultiply(this->m_world.m, this->m_local.m, parent->m);
    MatrixMultiply(this->m_view.m, this->m_world.m, this->m_scene->m_inverseView.m);
    this->m_flags |= BASE_MATRIX_CURRENT;
    this->m_flags &= ~BASE_INVERSE_CURRENT;
}

// ref: FUN_1411e8f60
void CM2BaseModel::EnsureLoaded() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
}

// The model and everything attached loaded (FUN_1411e8f70).
// ref: FUN_1411e8f70
void M2BaseModelEnsureTreeLoaded(CM2BaseModel* model) {
    if ((model->m_flags & CM2BaseModel::BASE_LOADED) == 0) {
        model->WaitForLoad(0);
    }
    if ((model->m_flags & CM2BaseModel::BASE_CHILDREN_LOADED) == 0) {
        for (CM2BaseModel* child = model->m_children; child != nullptr; child = child->m_siblingNext) {
            M2BaseModelEnsureTreeLoaded(child);
        }
    }
}

// ref: FUN_14026bea0
bool CM2BaseModel::IsAttachmentVisible(uint32_t index) {
    (void)index;
    return true;
}

// ref: FUN_1402ca290
uint8_t CM2BaseModel::GetMaxLod() {
    return 0;
}

const void* CM2BaseModel::GetLodData() {
    return nullptr;
}

bool CM2BaseModel::HasGpuBatches() {
    return false;
}

void CM2BaseModel::OnRemove() {
}

void CM2BaseModel::ReleaseGpuData() {
}

void CM2BaseModel::RefreshBatchEffects() {
}

void CM2BaseModel::OnRenderSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current) {
    (void)previous;
    (void)current;
}

void CM2BaseModel::OnMatricesUpdated() {
}

// FUN_14026bea0
bool CM2BaseModel::AreTexturesSettled() {
    return true;
}

void CM2BaseModel::SetTimeScale(float scale) {
    (void)scale;
}

void CM2BaseModel::GetFacets(const float* box, const float* matrix, void* facets) {
    (void)box;
    (void)matrix;
    (void)facets;
}

void CM2BaseModel::UpdateLodEffect() {
}
