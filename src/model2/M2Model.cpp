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
#include "texture/Texture.hpp"
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

// ref: FUN_1411ad030
CM2Model::CM2Model() {
    this->m_3b0 = 0xffff;
    this->m_pauseRequest = 0;
    this->m_3bc = 0xffff;
    this->m_lodMaskBytes = 0;
    this->m_boneDoneBytes = 0;
    this->m_3c0 = 0;
    this->m_404 = 1.0f;
    this->m_modelFlags = (this->m_modelFlags & 0x14e0) | 4;
    this->m_modelFlags2 = (this->m_modelFlags2 & 0xfff00400) | 0x400;
    this->m_3b8 = 0;
    this->m_waitPrev = nullptr;
    this->m_waitNext = nullptr;
    this->m_shared = nullptr;
    this->m_shared2 = nullptr;
    this->m_donor = nullptr;
    this->m_globalTimes = nullptr;
    this->m_initTime = 0;
    this->m_408 = nullptr;
    this->m_410 = nullptr;
    this->m_418 = nullptr;
    this->m_callback420 = nullptr;
    this->m_callback420Closure[0] = nullptr;
    this->m_callback420Closure[1] = nullptr;
    this->m_screenSize = 0.0f;
    this->m_43c = -10000.0f;
    this->m_lastAnimateTime = 0;
    this->m_boneStates = nullptr;
    this->m_boneMatrices = nullptr;
    this->m_boneDone = nullptr;
    this->m_physicsBones = nullptr;
    this->m_4c0 = 0;
    this->m_windPhase = nullptr;
    this->m_colors = nullptr;
    this->m_textures = nullptr;
    this->m_4e0 = nullptr;
    this->m_textureWeights = nullptr;
    this->m_textureTransforms = nullptr;
    this->m_textureMatrices = nullptr;
    this->m_effectMatrixCallback = nullptr;
    this->m_510 = nullptr;
    this->m_effectMatrixParam = nullptr;
    std::memset(&this->m_attachWorld, 0, sizeof(C44Matrix));
    this->m_attachWorld.m[0] = 1.0f;
    this->m_attachWorld.m[5] = 1.0f;
    this->m_attachWorld.m[10] = 1.0f;
    this->m_attachWorld.m[15] = 1.0f;
    this->m_558 = nullptr;
    this->m_effect2 = nullptr;
    this->m_color = C3Vector{1.0f, 1.0f, 1.0f};
    this->m_colorAdd = C3Vector{0.0f, 0.0f, 0.0f};
    this->m_590 = nullptr;
    this->m_580 = 4.0f;
    this->m_debugColors[0] = 0;
    this->m_events = nullptr;
    this->m_5a0 = nullptr;
    this->m_5a8 = nullptr;
    this->m_lights = nullptr;
    this->m_cameras = nullptr;
    this->m_ribbonStates = nullptr;
    this->m_ribbons = nullptr;
    this->m_particleStates = nullptr;
    this->m_particles = nullptr;
    this->m_physicsBoneCount = 0;
    this->m_physicsBoneStates = nullptr;
    this->m_5f0 = 0;
    this->m_5f8 = nullptr;
    this->m_600 = nullptr;
    this->m_608 = 0;
    this->m_accumBounds = nullptr;
    this->m_618 = -1;
    this->m_rtEntry[0] = -1;
    this->m_rtEntry[1] = 0;
    this->m_skeleton = nullptr;
    this->m_638 = nullptr;
    this->m_effect = nullptr;
    this->m_648 = 0;
    this->m_parentModel = nullptr;
    this->m_attachedModels = nullptr;
    this->m_attachedLink = nullptr;
    this->m_attachedNext = nullptr;
    this->m_lock670 = nullptr;
    this->m_handle = -1;
    this->m_67c = 0xffffffff;
    this->m_680 = 0;
    this->m_684 = 0xff00;
    this->m_686 = 1;
    M2RefIncrement(&this->m_refCount);
    this->m_type = 0;
    this->m_block = nullptr;
    this->m_matrixBlock = nullptr;
    this->m_468 = nullptr;
    this->m_debugColors[0] = 0xffff0000;
    this->m_debugColors[1] = 0xff00ff00;
    this->m_debugColors[2] = 0xff0000ff;
    for (int32_t i = 0; i < 4; i++) {
        this->m_lodBoneMasks[i] = nullptr;
    }
}

// ref: FUN_1411ad380
CM2Model::~CM2Model() {
    if (this->m_donor != nullptr) {
        this->m_donor->Release(1);
    }
    if (this->m_sceneLink != nullptr) {
        *this->m_sceneLink = this->m_sceneNext;
    }
    if (this->m_sceneNext != nullptr) {
        this->m_sceneNext->m_sceneLink = this->m_sceneLink;
    }
    this->m_sceneLink = nullptr;
    this->m_sceneNext = nullptr;
    this->ClearEffect();
    auto effect = static_cast<M2ModelEffect*>(this->m_effect);
    if (effect != nullptr) {
        for (void*& texture : effect->m_textures) {
            if (texture == nullptr) {
                continue;
            }
            if (texture == TextureGetWhite()) {
                texture = nullptr;
            } else {
                TextureFree(reinterpret_cast<HTEXTURE*>(&texture));
            }
        }
        M2ModelEffectFree(static_cast<M2ModelEffect*>(this->m_effect));
        this->m_effect = nullptr;
    }
    this->FreeCalls();
    if (this->m_shared != nullptr) {
        this->FreeInstanceBlock();
        this->m_shared->ReleaseModelRef();
        this->m_shared = nullptr;
    }
    if (this->m_shared2 != nullptr) {
        this->m_shared2->ReleaseModelRef();
        this->m_shared2 = nullptr;
    }
    if (this->m_extraMatrix != nullptr) {
        s_matrixPool->Free(this->m_extraMatrix);
        this->m_extraMatrix = nullptr;
    }
    if (this->m_windPhase != nullptr) {
        s_windPhasePool->Free(this->m_windPhase);
        this->m_windPhase = nullptr;
    }
    this->ReleaseGpuBatches();
    if (this->m_lock670 != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_14228b740: the RW buffer
    }
    if (this->m_handle != -1) {
        FROST_UNIMPLEMENTED();  // FUN_142295360: the handle's release
        this->m_handle = -1;
        *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(this) + 0x685) = 0x1ff;
    }
    this->m_680 = 0;
    this->m_parent = nullptr;
    this->m_parentModel = nullptr;
    this->m_lightingUsed = nullptr;
    if (this->m_skeleton != nullptr) {
        M2SkeletonInstanceRelease(this->m_skeleton);
        this->m_skeleton = nullptr;
    }
    auto owned = static_cast<void***>(this->m_590);
    if (owned != nullptr) {
        this->m_590 = nullptr;
        // its deleting destructor (vtable slot 2)
        reinterpret_cast<void (*)(void*, int32_t)>((*owned)[2])(owned, 1);
    }
}

// ref: FUN_141201d60
void CM2Model::ReleasePhysicsBoneTextures() {
    for (uint32_t i = 0; i < this->m_physicsBoneCount; i++) {
        uint8_t* bone = static_cast<uint8_t*>(this->m_physicsBoneStates) + i * 0xe0;
        for (uint32_t slot = 0; slot < 3; slot++) {
            auto texture = reinterpret_cast<HTEXTURE*>(bone + 8 + slot * 8);
            if (*texture != nullptr) {
                TextureFree(texture);
                *texture = nullptr;
            }
        }
        bone[0xdc] = 0;
        bone[0xda] = 0;
    }
}

// The physics bones' textures released and the GPU batches freed (FUN_1411cdb90).
// ref: FUN_1411cdb90
void CM2Model::ReleaseGpuBatches() {
    this->ReleasePhysicsBoneTextures();
    for (uint32_t i = 0; i < this->m_5f0; i++) {
        this->m_5f8[i] = nullptr;
    }
    if (this->m_600 != nullptr) {
        FROST_UNIMPLEMENTED();  // the GPU batches' texture, GxBuf and RW buffer
        SMemFree(this->m_600, s_modelFile, 0x1580, 0);
        this->m_600 = nullptr;
    }
}

// The per-instance arrays freed: slots, states, lights, the accumulated bounds and the block
// they live in (FUN_1411b8320).
// ref: FUN_1411b8320
void CM2Model::FreeInstanceBlock() {
    if (this->m_block != nullptr) {
        M2Data* data = this->m_shared->m_data;
        if (this->m_boneStates != nullptr && (this->m_modelFlags2 & 0x1000) == 0) {
            for (int32_t i = static_cast<int32_t>(data->m_bones.Count()) - 1; i >= 0; i--) {
                this->DropSequence(&this->m_boneStates[i]);
                this->DropBlend(&this->m_boneStates[i]);
            }
            this->m_boneStates = nullptr;
        }
        this->m_globalTimes = nullptr;
        this->m_colors = nullptr;
        this->m_textureWeights = nullptr;
        this->m_textureTransforms = nullptr;
        auto replaceable = static_cast<HTEXTURE*>(this->m_4e0);
        if (replaceable != nullptr) {
            for (int32_t i = 0; i < 5; i++) {
                if (replaceable[i] != nullptr) {
                    TextureFree(&replaceable[i]);
                    replaceable[i] = nullptr;
                }
            }
        }
        this->m_attachmentStates = nullptr;
        if (this->m_lights != nullptr) {
            for (uint32_t i = 0; i < data->m_lights.Count(); i++) {
                this->m_lights[i].m_lighting.Unlink();
            }
            this->m_lights = nullptr;
        }
        this->m_cameras = nullptr;
        this->m_ribbonStates = nullptr;
        this->m_particleStates = nullptr;
        if ((this->m_modelFlags & 0x800000) != 0) {
            s_accumBoundsPool->Free(this->m_accumBounds);
            this->m_modelFlags &= ~0x800000u;
            this->m_accumBounds = nullptr;
        }
        // FUN_141200f60
        if (this->m_physicsBoneCount != 0) {
            for (uint32_t i = 0; i < this->m_physicsBoneCount; i++) {
                FROST_UNIMPLEMENTED();  // FUN_141201ce0: a physics bone's state
            }
            this->m_physicsBoneCount = 0;
        }
        for (uint32_t i = 0; i < this->m_5f0; i++) {
            this->m_5f8[i] = nullptr;
        }
        this->m_5f0 = 0;
        SMemFree(this->m_block, s_modelFile, 0x380, 0);
    }
    if (this->m_matrixBlock != nullptr) {
        if ((this->m_modelFlags2 & 0x800) == 0) {
            this->m_boneMatrices = nullptr;
        }
        this->m_textureMatrices = nullptr;
        M2FreeFileData(this->m_matrixBlock);
    }
    if (this->m_physicsBones != nullptr) {
        SMemFree(this->m_physicsBones, s_modelFile, 0x392, 0);
        this->m_physicsBones = nullptr;
    }
    if (this->m_468 != nullptr) {
        SMemFree(this->m_468, s_modelFile, 0x398, 0);
        this->m_468 = nullptr;
    }
}

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

bool M2ModelInitializeLoaded(CM2Model* model) {
    return model->InitializeLoaded();
}

namespace {

// The files whose models take the modelFlags2 bits 0x8000 and 0x10000 (DAT_142721100,
// DAT_142721140).
const int32_t s_flag8000Files[16] = {
    0x36632b, 0x368244, 0x368245, 0x368246, 0x368248, 0x368249, 0x36824a, 0x36824b,
    0x36824c, 0x36824d, 0x36824e, 0x36824f, 0x368250, 0x368251, 0x368252, 0x08c7c7,
};
const int32_t s_flag10000Files[9] = {
    0x30f6d0, 0x30f6d1, 0x309f42, 0x37023e, 0x30f6d3, 0x30f6d5, 0x30f6d6, 0x3ae6fd, 0x3d03b6,
};

// An effect copied from a donor's, its textures referenced again.
M2ModelEffect* CopyEffect(const M2ModelEffect* from) {
    M2ModelEffect* effect = M2ModelEffectAlloc();
    if (effect == nullptr) {
        return nullptr;
    }
    std::memcpy(effect, from, 0xa8);
    for (void* texture : effect->m_textures) {
        if (texture != nullptr) {
            TextureAddRef(static_cast<HTEXTURE>(texture));
        }
    }
    return effect;
}

}  // namespace

// ref: FUN_1411c1360
bool CM2Model::Initialize(CM2Scene* scene, CM2Shared* shared, CM2Model* donor, uint8_t lod, uint32_t createFlags) {
    this->AddToScene(scene);
    this->m_createTime = this->m_scene->m_time;
    this->m_shared = shared;
    shared->AddModelRef();
    this->m_lod = lod;
    this->m_lodTarget = lod;
    this->m_lodRequested = lod;
    this->m_donor = donor;
    CM2Shared* own = this->m_shared;
    this->m_fileBounds = own->m_bounds;
    this->m_createFlags = createFlags;
    // The file's 0x400 (it has bounds) as bit 29.
    this->m_flags ^= ((own->m_flags << 19) ^ this->m_flags) & 0x20000000;
    this->m_callsTail = &this->m_calls;
    this->m_initTime = this->m_scene->m_time;
    if (donor != nullptr) {
        donor->AddRef();
        this->m_lod = donor->m_lod;
        this->m_lodTarget = donor->m_lodTarget;
        this->m_lodRequested = donor->m_lodRequested;
        CM2Shared* file = this->m_shared;
        if (file->m_textures != nullptr && donor->m_textures != nullptr) {
            for (uint32_t i = 0; i < file->m_data->m_textures.Count(); i++) {
                void* texture = donor->m_textures[i];
                if (file->m_textures[i] != texture) {
                    int32_t type = static_cast<int32_t>(file->m_data->m_textures[i].m_type);
                    this->SetReplaceableTexture(type, static_cast<HTEXTURE>(texture),
                        type == 1 ? static_cast<HTEXTURE*>(donor->m_4e0) : nullptr);
                }
                file = this->m_shared;
            }
        }
        this->m_color = donor->m_color;
        this->m_colorAdd = donor->m_colorAdd;
        uint32_t flags = ((this->m_modelFlags ^ donor->m_modelFlags) & 0x20000000) ^ this->m_modelFlags;
        flags = ((flags ^ donor->m_modelFlags) & 0x4000000) ^ flags;
        this->m_flags ^= (this->m_flags ^ donor->m_flags) & 0x4000000;
        this->m_modelFlags = ((flags ^ donor->m_modelFlags) & 0x80000) ^ flags;
        if (donor->m_extraMatrix != nullptr) {
            if (this->m_extraMatrix == nullptr) {
                auto matrix = static_cast<C44Matrix*>(s_matrixPool->Alloc());
                std::memset(matrix, 0, sizeof(C44Matrix));
                matrix->m[0] = 1.0f;
                matrix->m[5] = 1.0f;
                matrix->m[10] = 1.0f;
                matrix->m[15] = 1.0f;
                this->m_extraMatrix = matrix;
            }
            *this->m_extraMatrix = *donor->m_extraMatrix;
        }
        this->m_flags ^= (this->m_flags ^ donor->m_flags) & BASE_VISIBLE;
        this->m_local = donor->m_local;
        this->m_648 = donor->m_648;
        if (donor->m_effect != nullptr) {
            this->m_effect = CopyEffect(static_cast<M2ModelEffect*>(donor->m_effect));
        }
        if (donor->m_effect2 != nullptr) {
            this->m_effect2 = CopyEffect(static_cast<M2ModelEffect*>(donor->m_effect2));
        }
        this->m_0d = donor->m_0d;
    }
    uint32_t flag8000 = 0;
    for (int32_t id : s_flag8000Files) {
        if (id == this->m_shared->m_fileDataId) {
            flag8000 = 0x8000;
            break;
        }
    }
    uint32_t flags2 = this->m_modelFlags2;
    this->m_modelFlags2 = (flags2 & 0xffff7fff) | flag8000;
    uint32_t flag10000 = 0;
    for (int32_t id : s_flag10000Files) {
        if (id == this->m_shared->m_fileDataId) {
            flag10000 = 0x10000;
            break;
        }
    }
    this->m_modelFlags2 = (flags2 & 0xfffe7fff) | flag8000 | flag10000;
    return M2SharedAttachModel(this->m_shared, this);
}

// A model made of the file: initialized now if the file and its skin profiles are in, else
// it waits on the file (FUN_1411a5230).
// ref: FUN_1411a5230
bool M2SharedAttachModel(CM2Shared* shared, CM2Model* model) {
    if ((shared->m_flags & CM2Shared::SHARED_LOADED) == 0 || shared->m_skinProfileLoads.Head() != nullptr) {
        M2ModelLinkWaiting(model);
        return true;
    }
    void* skeleton = model->m_skeleton;
    if (shared->m_skeleton != nullptr && (*reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(shared->m_skeleton) + 0x10) & 1) == 0) {
        FROST_UNIMPLEMENTED();  // FUN_1411f6860: the skeleton file's load
    }
    bool skeletonReady = skeleton == nullptr || (*(static_cast<uint8_t*>(skeleton) + 0x28) & 1) != 0;
    bool initialized = model->InitializeLoaded();
    return initialized && skeletonReady;
}

// The texture of every slot of replaceable type `type` replaced, the particles' too; type 1 also
// takes the five extra textures (FUN_1411c8e40).
// ref: FUN_1411c8e40
void CM2Model::SetReplaceableTexture(int32_t type, HTEXTURE texture, HTEXTURE* extra) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = M2ModelCallAlloc();
        if (call == nullptr) {
            return;
        }
        call->m_type = 0;
        call->m_next = nullptr;
        call->m_frame = this->m_scene->m_time;
        call->m_args[0] = static_cast<uint32_t>(type);
        if (texture != nullptr) {
            TextureAddRef(texture);
        }
        auto slots = reinterpret_cast<HTEXTURE*>(&call->m_args[2]);
        slots[0] = texture;
        for (int32_t i = 0; i < 5; i++) {
            slots[1 + i] = extra != nullptr ? extra[i] : nullptr;
            if (extra != nullptr && extra[i] != nullptr) {
                TextureAddRef(extra[i]);
            }
        }
        *this->m_callsTail = call;
        this->m_callsTail = &call->m_next;
        return;
    }
    M2Data* data = this->m_shared->m_data;
    for (uint32_t i = 0; i < data->m_textures.Count(); i++) {
        if (static_cast<int32_t>(data->m_textures[i].m_type) != type) {
            continue;
        }
        if (texture != nullptr) {
            TextureAddRef(texture);
        }
        auto slot = reinterpret_cast<HTEXTURE*>(&this->m_textures[i]);
        if (*slot != nullptr) {
            TextureFree(slot);
        }
        this->m_textures[i] = texture;
        if (texture != nullptr) {
            if (TextureGetGxTex(texture) == nullptr) {
                this->m_flags &= ~BASE_TEXTURES_READY;
            }
            if (TextureLoadState(texture) != 2) {
                this->m_flags &= ~0x20u;
                for (CM2BaseModel* above = this->m_parent; above != nullptr; above = above->m_parent) {
                    above->m_flags &= ~BASE_READY_CHECKED;
                }
            }
        }
    }
    if (type == 1 && extra != nullptr) {
        auto own = static_cast<HTEXTURE*>(this->m_4e0);
        for (int32_t i = 0; i < 5; i++) {
            if (own[i] != nullptr) {
                TextureFree(&own[i]);
            }
            if (extra[i] != nullptr) {
                own[i] = extra[i];
                TextureAddRef(own[i]);
            }
        }
    }
    for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
        M2Particle& particle = data->m_particles[i];
        uint16_t slots = particle.m_texture;
        if ((particle.m_flags & 0x10000000) == 0) {
            if (static_cast<int32_t>(data->m_textures[slots].m_type) == type) {
                ParticleEmitterSetTexture(this->m_particles[i], 0, texture);
            }
            continue;
        }
        if (static_cast<int32_t>(data->m_textures[slots & 0x1f].m_type) == type) {
            ParticleEmitterSetTexture(this->m_particles[i], 0, texture);
        }
        if (static_cast<int32_t>(data->m_textures[slots >> 5 & 0x1f].m_type) == type) {
            ParticleEmitterSetTexture(this->m_particles[i], 1, texture);
        }
        if (static_cast<int32_t>(data->m_textures[slots >> 10 & 0x1f].m_type) == type) {
            ParticleEmitterSetTexture(this->m_particles[i], 2, texture);
        }
    }
    this->m_modelFlags &= ~2u;
    this->ReleasePhysicsBoneTextures();
}

// ref: FUN_1411c5d90
void M2ModelInitializePools() {
    s_accumBoundsPool = MakePool("M2 Accum Bounds Pool", 0x44, 0x10, 0x1b6);
    s_effectPool = MakePool("M2 Model Effect Pool", 0xb0, 0x40, 0x1b7);
    s_matrixPool = MakePool("M2 Model Matrix Pool", 0x44, 8, 0x1b8);
    s_windPhasePool = MakePool("M2 Model Wind Phase Pool", 0x10, 0x40, 0x1b9);
    s_boneSeqPool = MakePool("M2 Bone Seq Pool", 0x68, 0x400, 0x3d66);
}

// ref: FUN_141200760
void M2PhysicsBoneInit(void* state) {
    auto raw = static_cast<uint8_t*>(state);
    std::memset(raw, 0, 0xdd);
    *reinterpret_cast<float*>(raw + 0x58) = 1.0f;
    *reinterpret_cast<float*>(raw + 0x6c) = 1.0f;
    *reinterpret_cast<float*>(raw + 0x80) = 1.0f;
    *reinterpret_cast<float*>(raw + 0x94) = 1.0f;
}

void* M2ModelWindPhaseAlloc() {
    return s_windPhasePool->Alloc();
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
    static CM2Scene* s_moduleScene;  // DAT_1430a0ed0
    if (s_moduleScene == nullptr) {
        s_moduleScene = M2CacheCreateScene(9, nullptr, nullptr, nullptr);
    }
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
