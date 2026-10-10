#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"

// Engine/Source/Model2/M2Model.cpp: attaching a model and placing it in a scene.

// The attachment point an id names on `parent`, as the index this model hangs from; false when
// it has none and `force` is not set (id 0x13 is always kept) (FUN_1411c98d0).
// ref: FUN_1411c98d0
bool CM2Model::ResolveAttachment(CM2BaseModel* parent, uint32_t id, bool force) {
    uint32_t index = 0xffff;
    if ((parent->m_flags & BASE_LOADED) != 0) {
        uint32_t mapped = parent->MapAttachmentId(id);
        index = parent->GetAttachmentIndex(mapped);
        if (static_cast<uint16_t>(index) == 0xffff && !force) {
            if (mapped != 0x13) {
                return false;
            }
            force = true;
        }
    }
    this->m_attachIndex = index & 0xffff;
    this->m_flags = static_cast<uint32_t>(force) << 24 | (this->m_flags & 0xfeffffff);
    return true;
}

// ref: FUN_1411b4b00
bool CM2Model::AttachTo(CM2BaseModel* parent, uint32_t id, const float* position, bool flag) {
    if ((parent->m_flags & BASE_LOADED) == 0) {
        parent->QueueAttachChild(this, id, flag);
        this->m_attachIndex = 0xffff;
        this->m_flags = static_cast<uint32_t>(flag) << 24 | (this->m_flags & 0xfeffffff);
    } else if (!this->ResolveAttachment(parent, id, flag)) {
        return false;
    }
    M2RefIncrement(&this->m_refCount);
    if (this->m_parent != nullptr) {
        this->Detach(true);
    }
    this->SetBucketed(0);
    this->m_flags |= BASE_ATTACH_SHOWN;
    CM2BaseModel** head = &parent->m_children;
    this->m_siblingLink = head;
    this->m_parent = parent;
    this->m_attachId = static_cast<int32_t>(id);
    this->m_siblingNext = *head;
    // The nearest M2 model up the chain.
    CM2BaseModel* above = parent;
    do {
        if (above->m_type == 0) {
            break;
        }
        above = above->m_parent;
    } while (above != nullptr);
    CM2Model* model = above != nullptr && above->m_type == 0 ? static_cast<CM2Model*>(above) : nullptr;
    if (model != nullptr) {
        this->m_parentModel = model;
        this->m_attachedLink = &model->m_attachedModels;
        CM2Model* first = model->m_attachedModels;
        this->m_attachedNext = first;
        if (first != nullptr) {
            first->m_attachedLink = &this->m_attachedNext;
        }
        this->m_parentModel->m_attachedModels = this;
    }
    this->m_0d = parent->m_0d;
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
    CM2BaseModel* attached = this->m_parent;
    if ((attached->m_flags & BASE_LOADED) != 0 && this->m_attachIndex != 0xffff) {
        if (position != nullptr) {
            C44Matrix m;
            attached->GetAttachmentMatrixByIndex(id, &m);
            MatrixAffineInverse(m.m, std::sqrt(m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2]));
            float x = position[0];
            float y = position[1];
            float z = position[2];
            this->m_local.m[12] = y * m.m[4] + x * m.m[0] + z * m.m[8] + m.m[12];
            this->m_local.m[13] = y * m.m[5] + x * m.m[1] + z * m.m[9] + m.m[13];
            this->m_local.m[14] = y * m.m[6] + x * m.m[2] + z * m.m[10] + m.m[14];
        }
        attached = this->m_parent;
        CM2ModelLighting* lighting;
        if (attached == nullptr || (attached->m_createFlags & 1) != 0) {
            lighting = &this->m_lighting;
        } else {
            lighting = attached->m_lightingUsed;
        }
        this->m_lightingUsed = lighting;
        if (lighting == nullptr) {
            this->m_lightingUsed = &this->m_lighting;
        }
    }
    this->SetVisibleFlags((attached->m_flags >> 2 & 1) != 0, false, false);
    std::memset(this->m_boneDone, 0, this->m_boneDoneBytes);
    if ((this->m_flags & BASE_LOADED) != 0) {
        M2Data* data = this->m_shared->m_data;
        if ((data->m_flags & 0x100) != 0) {
            for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
                if (data->m_particles[i].m_emitterType == 4) {
                    CParticleGenerator* generator = this->m_particles[i]->m_generator;
                    ParticleGeneratorSetModel(generator, this->m_parentModel);
                    ParticleGeneratorInitializeBones(generator);
                }
                data = this->m_shared->m_data;
            }
        }
    }
    if ((this->m_modelFlags & 0x40000) != 0 && M2SceneGetRayTracing(this->m_scene) != nullptr) {
        RayTracingRegisterModel(M2SceneGetRayTracing(this->m_scene), reinterpret_cast<uint8_t*>(this) + 0x61c, 0x40);
    }
    if (this->m_parentModel != nullptr && (this->m_flags & BASE_LOADED) != 0) {
        C44Matrix* matrix = this->m_parentModel->m_558;
        C44Matrix* copied = nullptr;
        if (matrix != nullptr) {
            copied = &this->m_attachWorld;
            this->m_attachWorld = *matrix;
        }
        this->SetAttachMatrix(copied);
    }
    return true;
}

// ref: FUN_1411b4f10
void CM2Model::AddToScene(CM2Scene* scene) {
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
    if (this->m_skeleton != nullptr) {
        M2SkeletonSetScene(this->m_skeleton, this->m_scene);
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        for (M2ModelCall* call = this->m_calls; call != nullptr; call = call->m_next) {
            call->m_frame += this->m_scene->m_time;
        }
        return;
    }
    M2Data* data = this->m_shared->m_data;
    for (uint32_t i = 0; i < data->m_lights.Count(); i++) {
        this->m_lights[i].m_lighting.SetScene(scene);
    }
    for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
        this->m_particles[i]->m_scene = scene;
    }
    if ((this->m_modelFlags2 & 0x800) == 0) {
        M2AnimState* anim = this->m_animState;
        M2SequenceResolve resolved;
        M2ResolveSequence(0, &resolved, anim, anim->m_sequences, anim->m_sequenceHash,
            static_cast<M2Array<uint16_t>*>(this->m_shared->m_blocklistAnimData));
        this->SetSequence(0xffffffff, resolved.m_sequence, 0xffffffff, 0, 1.0f, 1, 0, 5, 0, 0, 1.0f);
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if ((this->m_shared->m_data->m_flags & 0x20) != 0 && this->m_scene != nullptr) {
        this->RegisterRayTracing();
    }
}
