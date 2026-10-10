#include <cmath>
#include <cstring>

#include "base/Matrix.hpp"
#include "fileservices/AsyncFile.hpp"
#include "gx/GxRenderTargets.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Ribbon.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "os/Sync.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Model.cpp: the model's public setters. Each one made before the file is
// in queues an M2ModelCall that InitializeLoaded replays in order.

namespace {

M2ModelCall* QueueCall(CM2Model* model, int32_t type) {
    M2ModelCall* call = M2ModelCallAlloc();
    if (call == nullptr) {
        return nullptr;
    }
    call->m_type = type;
    call->m_next = nullptr;
    call->m_frame = model->m_scene->m_time;
    return call;
}

void LinkCall(CM2Model* model, M2ModelCall* call) {
    *model->m_callsTail = call;
    model->m_callsTail = &call->m_next;
}

uint16_t KeyBoneIndex(const M2AnimState* anim, uint32_t keyBone) {
    if (keyBone == 0xffffffff) {
        return 0;
    }
    return keyBone < anim->m_keyBoneLookup->Count() ? (*anim->m_keyBoneLookup)[keyBone] : 0xffff;
}

// The skin-LOD handlers (DAT_1430a0f68, a vector of 0x28-byte entries under DAT_1430a0f80).
struct LodHandler {
    void* m_00;
    void (*m_callback)(CM2Model* model, void* data);
    uint8_t m_data[0x18];
};
LodHandler* s_lodHandlers;
uint64_t s_lodHandlerCount;
OSMUTEX s_lodHandlerLock;
bool s_lodHandlerLockReady = OsMutexInit(&s_lodHandlerLock);

}  // namespace

// The texture of slot `index` replaced, with the ribbons' and particles' that use it
// (FUN_1411c92b0).
// ref: FUN_1411c92b0
void CM2Model::SetTextureAt(uint32_t index, CTexture* texture) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 1);
        if (call != nullptr) {
            call->m_args[0] = index;
            if (texture != nullptr) {
                TextureAddRef(texture);
            }
            std::memcpy(&call->m_args[2], &texture, sizeof(texture));
            LinkCall(this, call);
        }
        return;
    }
    M2Data* data = this->m_shared->m_data;
    if (texture != nullptr) {
        TextureAddRef(texture);
    }
    auto slot = reinterpret_cast<HTEXTURE*>(&this->m_textures[index]);
    if (*slot != nullptr) {
        TextureFree(slot);
    }
    this->m_textures[index] = texture;
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
    for (uint32_t i = 0; i < data->m_ribbons.Count(); i++) {
        const M2Ribbon& ribbon = data->m_ribbons[i];
        for (uint32_t t = 0; t < ribbon.m_textureIndices.Count(); t++) {
            if (ribbon.m_textureIndices[t] == index) {
                RibbonEmitterSetTexture(this->m_ribbons[i], t, texture);
            }
        }
    }
    for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
        const M2Particle& particle = data->m_particles[i];
        uint16_t slots = particle.m_texture;
        if ((particle.m_flags & 0x10000000) == 0) {
            if (index == slots) {
                ParticleEmitterSetTexture(this->m_particles[i], 0, texture);
            }
            continue;
        }
        if ((slots & 0x1f) == index) {
            ParticleEmitterSetTexture(this->m_particles[i], 0, texture);
        }
        if ((slots >> 5 & 0x1f) == index) {
            ParticleEmitterSetTexture(this->m_particles[i], 1, texture);
        }
        if ((slots >> 10 & 0x1f) == index) {
            ParticleEmitterSetTexture(this->m_particles[i], 2, texture);
        }
    }
    this->m_modelFlags &= ~2u;
    this->ReleasePhysicsBoneTextures();
}

// The skin sections of geosets lo..hi shown or hidden on every LOD (FUN_1411cb1b0): whether
// the current LOD changed.
// ref: FUN_1411cb1b0
bool CM2Model::SetGeosets(uint32_t lo, uint32_t hi, uint32_t show) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 2);
        if (call == nullptr) {
            return false;
        }
        call->m_args[0] = lo;
        call->m_args[1] = hi;
        call->m_args[2] = show;
        LinkCall(this, call);
        return true;
    }
    bool changed = false;
    uint8_t lods = this->m_shared->m_lodMax;
    for (uint32_t lod = 0; lod <= lods; lod++) {
        const M2SkinProfile* profile = this->m_shared->m_skins[lod].m_profile;
        uint8_t* mask = this->m_lodBoneMasks[lod];
        for (uint32_t s = 0; s < profile->m_skinSections.Count(); s++) {
            uint16_t geoset = *reinterpret_cast<const uint16_t*>(&profile->m_skinSections[s]);
            if (lo <= geoset && geoset <= hi) {
                uint8_t* byte = &mask[s >> 3];
                uint8_t bit = static_cast<uint8_t>(1 << (s & 7));
                if (((bit & *byte) != 0) != (show != 0)) {
                    if (show == 0) {
                        *byte = static_cast<uint8_t>(~bit & *byte);
                    } else {
                        *byte = static_cast<uint8_t>(*byte | bit);
                    }
                    this->ReleasePhysicsBoneTextures();
                    if (this->m_lod == lod) {
                        changed = true;
                    }
                }
            }
        }
    }
    auto geosetParticles = static_cast<M2Array<uint16_t>*>(this->m_shared->m_particleGeosets);
    if (geosetParticles != nullptr) {
        for (uint32_t i = 0; i < this->m_shared->m_data->m_particles.Count(); i++) {
            uint16_t geoset = (*geosetParticles)[i];
            if (geoset != 0 && lo <= geoset && geoset <= hi) {
                CParticleEmitter2* emitter = this->m_particles[i];
                emitter->m_flags = show == 0 ? emitter->m_flags & ~2u : emitter->m_flags | 2;
            }
        }
    }
    if (changed) {
        this->ReleaseGpuBatches();
        this->m_686 = 1;
    }
    return changed;
}

// The current LOD's skin sections lo..hi shown or hidden, on every LOD's mask (FUN_1411cc4b0).
// ref: FUN_1411cc4b0
void CM2Model::SetSkinSections(uint32_t lo, uint32_t hi, uint32_t show) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 3);
        if (call != nullptr) {
            call->m_args[0] = lo;
            call->m_args[1] = hi;
            call->m_args[2] = show;
            LinkCall(this, call);
        }
        return;
    }
    bool changed = false;
    uint8_t lods = this->m_shared->m_lodMax;
    for (uint32_t lod = 0; lod <= lods; lod++) {
        const M2SkinProfile* profile = this->m_shared->m_skins[this->m_lod].m_profile;
        uint8_t* mask = this->m_lodBoneMasks[lod];
        for (uint32_t s = 0; s < profile->m_skinSections.Count(); s++) {
            if (lo <= s && s <= hi) {
                uint8_t* byte = &mask[s >> 3];
                uint8_t bit = static_cast<uint8_t>(1 << (s & 7));
                if (((bit & *byte) != 0) != (show != 0)) {
                    if (show == 0) {
                        *byte = static_cast<uint8_t>(~bit & *byte);
                    } else {
                        *byte = static_cast<uint8_t>(*byte | bit);
                    }
                    this->ReleasePhysicsBoneTextures();
                    if (lod == this->m_lod) {
                        changed = true;
                    }
                }
            }
        }
    }
    if (changed) {
        this->ReleaseGpuBatches();
    }
}

// A key bone's flags: `value` under `mask` (FUN_1411c9e80).
// ref: FUN_1411c9e80
void CM2Model::SetBoneFlags(uint32_t keyBone, uint32_t value, uint32_t mask) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 5);
        if (call != nullptr) {
            call->m_args[0] = keyBone;
            call->m_args[1] = value;
            call->m_args[2] = mask;
            LinkCall(this, call);
        }
        return;
    }
    M2AnimState* anim = this->m_animState;
    uint16_t bone = KeyBoneIndex(anim, keyBone);
    if (bone < anim->m_bones->Count()) {
        M2BoneState* state = &anim->m_boneStates[bone];
        state->m_flags = (~mask & state->m_flags) | (value & mask);
    }
}

// A key bone's sequence moved to `time` (FUN_1411cad10).
// ref: FUN_1411cad10
void CM2Model::SetSequenceTime(uint32_t keyBone, int32_t time) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 8);
        if (call != nullptr) {
            call->m_args[0] = keyBone;
            call->m_args[1] = static_cast<uint32_t>(time);
            LinkCall(this, call);
        }
        return;
    }
    M2AnimState* anim = this->m_animState;
    uint16_t bone = keyBone == 0xffffffff ? 0 : KeyBoneIndex(anim, keyBone);
    if (anim->m_bones->Count() <= bone) {
        return;
    }
    M2SequenceTime* slot = anim->m_boneStates[bone].m_sequence;
    if (slot == nullptr || slot->m_nextSequence == 0xffff) {
        return;
    }
    float rate = std::fabs(slot->m_inverseSpeed);
    uint32_t duration = (*slot->m_source.m_sequences)[slot->m_nextSequence].m_duration;
    int32_t start = static_cast<int32_t>(this->m_scene->m_time) - static_cast<int32_t>(static_cast<float>(time) * rate);
    slot->m_startTime = start;
    slot->m_endTime = static_cast<int32_t>(static_cast<int64_t>(static_cast<float>(duration * slot->m_repeats) * rate)) + start;
}

// ref: FUN_1411cb690
void CM2Model::SetLightEnabled(uint32_t index, uint32_t enabled) {
    if ((this->m_flags & BASE_LOADED) != 0) {
        this->m_lights[index].m_64 = enabled;
        return;
    }
    M2ModelCall* call = QueueCall(this, 0xb);
    if (call != nullptr) {
        call->m_args[0] = index;
        call->m_args[1] = enabled;
        LinkCall(this, call);
    }
}

// The particle colours of a colour index, and the ribbons with a colour slot past 10 take one
// of the three (FUN_1411c8c70).
// ref: FUN_1411c8c70
void CM2Model::SetParticleColors(uint32_t colorIndex, uint32_t start, uint32_t middle, uint32_t end) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0xc);
        if (call != nullptr) {
            call->m_args[0] = colorIndex;
            call->m_args[1] = start;
            call->m_args[2] = middle;
            call->m_args[3] = end;
            LinkCall(this, call);
        }
        return;
    }
    M2Data* data = this->m_shared->m_data;
    for (uint32_t i = 0; i < data->m_particles.Count(); i++) {
        const M2Particle& particle = data->m_particles[i];
        if (particle.m_particleColorIndex == colorIndex && particle.m_colorTrack.m_values.m_count == 3) {
            ParticleEmitterSetColors(this->m_particles[i], reinterpret_cast<const uint8_t*>(&start),
                reinterpret_cast<const uint8_t*>(&middle), reinterpret_cast<const uint8_t*>(&end));
        }
    }
    uint32_t colors[3] = {start, middle, end};
    for (uint32_t i = 0; i < data->m_ribbons.Count(); i++) {
        uint8_t slot = reinterpret_cast<const uint8_t*>(&data->m_ribbons[i])[0xae];
        if (10 < slot) {
            // The three colours sit at slots 11..13 of the frame the reference indexes.
            float rgba[4];
            GxColorToFloat4(rgba, colors[slot - 11]);
            RibbonEmitterSetColorOverride(this->m_ribbons[i], rgba[0], rgba[1], rgba[2]);
        }
    }
}

// ref: FUN_1411cae60
void CM2Model::SetParticlesEnabled(int32_t enabled) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0xd);
        if (call != nullptr) {
            call->m_args[0] = static_cast<uint32_t>(enabled);
            LinkCall(this, call);
        }
        return;
    }
    uint16_t count = this->m_shared->m_data->m_particles.m_count;
    for (uint32_t i = 0; i < count; i++) {
        CParticleEmitter2* emitter = this->m_particles[i];
        emitter->m_flags = enabled == 0 ? emitter->m_flags & ~2u : emitter->m_flags | 2;
    }
}

// ref: FUN_1411cc7a0
void CM2Model::SetRibbonsEnabled(uint32_t enabled) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0xe);
        if (call != nullptr) {
            call->m_args[0] = enabled;
            LinkCall(this, call);
        }
        return;
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_ribbons.Count(); i++) {
        RibbonEmitterSetEnabled(this->m_ribbons[i], enabled != 0);
    }
}

// The emitters stop emitting (FUN_1411b7fd0).
// ref: FUN_1411b7fd0
void CM2Model::StopParticles() {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0x10);
        if (call != nullptr) {
            LinkCall(this, call);
        }
        return;
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_particles.Count(); i++) {
        this->m_particles[i]->m_flags |= 0x4000000;
    }
}

// The file's textures read ahead (FUN_1411c6460 into FUN_1411abc60).
// ref: FUN_1411c6460
void CM2Model::PrefetchTextures() {
    if ((this->m_flags & BASE_LOADED) != 0) {
        // FUN_1411abc60
        CM2Shared* shared = this->m_shared;
        for (uint32_t i = 0; i < shared->m_data->m_textures.Count(); i++) {
            const M2Texture& texture = shared->m_data->m_textures[i];
            if (texture.m_type != 0) {
                continue;
            }
            if (shared->m_textureFileIds == nullptr) {
                AsyncFilePrefetchByName(&texture.m_filename[0]);
            } else {
                AsyncFilePrefetchById(shared->m_textureFileIds[i]);
            }
        }
        return;
    }
    M2ModelCall* call = QueueCall(this, 0x11);
    if (call != nullptr) {
        LinkCall(this, call);
    }
}

// ref: FUN_1411c9d00
void CM2Model::SetKeyBoneWeight(uint32_t keyBone, float weight) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0x13);
        if (call != nullptr) {
            std::memcpy(&call->m_args[1], &weight, sizeof(float));
            call->m_args[0] = keyBone;
            *reinterpret_cast<uint8_t*>(&call->m_args[2]) = 0;
            LinkCall(this, call);
        }
        return;
    }
    this->SetBoneWeight(KeyBoneIndex(this->m_animState, keyBone), weight);
}

// ref: FUN_1411c9dd0
void CM2Model::SetBoneWeight(uint32_t bone, float weight) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0x13);
        if (call != nullptr) {
            std::memcpy(&call->m_args[1], &weight, sizeof(float));
            call->m_args[0] = bone;
            *reinterpret_cast<uint8_t*>(&call->m_args[2]) = 1;
            LinkCall(this, call);
        }
        return;
    }
    if (bone < this->m_animState->m_bones->Count()) {
        this->m_animState->m_boneStates[bone].m_weight = weight;
    }
}

// The bone file (BFID) the model's bones are offset by, 0 for none (FUN_1411caff0).
// ref: FUN_1411caff0
void CM2Model::SetBoneFile(int32_t fileDataId) {
    uint32_t loaded = this->m_flags & BASE_LOADED;
    if (loaded != 0 && this->m_skeleton != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_141204980: the skeleton's bone file
        return;
    }
    if (fileDataId == 0) {
        this->m_618 = -1;
        return;
    }
    if (loaded == 0) {
        M2ModelCall* call = QueueCall(this, 0x15);
        call->m_args[0] = static_cast<uint32_t>(fileDataId);
        LinkCall(this, call);
        return;
    }
    // FUN_1411a8090
    CM2Shared* shared = this->m_shared;
    int32_t found = -1;
    for (uint32_t i = 0; i < shared->m_boneFileCount; i++) {
        if (shared->m_boneFileIds[i] == fileDataId) {
            found = static_cast<int32_t>(i);
            break;
        }
    }
    this->m_618 = found;
}

// The attached models driven by this one's bones take them (FUN_1411b6140).
// ref: FUN_1411b6140
void CM2Model::ShareBonesWithAttached() {
    for (CM2Model* model = this->m_attachedModels; model != nullptr; model = model->m_attachedNext) {
        if ((model->m_modelFlags2 >> 11 & 1) != 0 && (model->m_modelFlags2 >> 12 & 1) == 0) {
            model->ShareParentBones();
        }
    }
}

// ref: FUN_1411cc6e0
void CM2Model::SetRibbonQuality(uint32_t quality) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0x17);
        if (call != nullptr) {
            call->m_args[0] = quality;
            LinkCall(this, call);
        }
        return;
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_ribbons.Count(); i++) {
        RibbonEmitterSetQuality(this->m_ribbons[i], quality);
    }
}

// A child queued before this model was in finds its attachment point and visibility
// (FUN_1411c0e80).
// ref: FUN_1411c0e80
void CM2Model::OnChildLoaded(CM2BaseModel* child, uint32_t id) {
    if (child->m_parent != this) {
        return;
    }
    if (child->m_type == 0) {
        static_cast<CM2Model*>(child)->ResolveAttachment(this, id, false);
    }
    child->SetVisibleFlags((this->m_flags >> 2 & 1) != 0, false, false);
}

// The model effect: none, one of the presets (with `params` null) or the given parameters
// (FUN_1411cb800). Type 5 projects its textures and needs vertices.
// ref: FUN_1411cb800
void CM2Model::SetEffect(int32_t type, const void* params) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0x1a);
        if (call != nullptr) {
            call->m_args[0] = static_cast<uint32_t>(type);
            if (params != nullptr) {
                std::memcpy(&call->m_args[1], params, 0x78);
            }
            LinkCall(this, call);
        }
        return;
    }
    if (type == 5 && this->m_shared->m_data->m_vertices.m_count == 0) {
        return;
    }
    auto effect = static_cast<M2ModelEffect*>(this->m_effect2);
    if (type == 0) {
        if (effect != nullptr) {
            this->ClearEffect();
            this->m_effect2 = nullptr;
        }
        return;
    }
    if (effect == nullptr) {
        effect = M2ModelEffectAlloc();
        this->m_effect2 = effect;
        if (effect == nullptr) {
            return;
        }
    }
    effect->m_type = type;
    static_cast<M2ModelEffect*>(this->m_effect2)->m_strength = 1.0f;
    effect = static_cast<M2ModelEffect*>(this->m_effect2);
    auto values = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(effect) + 8);
    if (params == nullptr) {
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
        }
        effect = static_cast<M2ModelEffect*>(this->m_effect2);
        values = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(effect) + 8);
        auto bits = [](uint32_t value) {
            float f;
            std::memcpy(&f, &value, sizeof(f));
            return f;
        };
        switch (type) {
        case 1:
            values[0] = 1.0f;
            values[1] = 1.0f;
            values[2] = 1.0f;
            break;
        case 2:
            values[0] = bits(0x3e30b0b1);
            values[1] = bits(0x3eaaaaab);
            values[2] = bits(0x3e2cacad);
            values[3] = bits(0x3f078788);
            values[4] = bits(0x3f42c2c3);
            values[5] = bits(0x3f0b8b8c);
            values[6] = 1.0f;
            values[7] = 1.0f;
            break;
        case 3:
            values[0] = bits(0x3f39b9ba);
            values[1] = bits(0x3ef4f4f5);
            values[2] = bits(0x3eaeaeaf);
            values[3] = 1.0f;
            values[4] = bits(0x3f27a7a8);
            values[5] = bits(0x3ea2a2a3);
            break;
        case 5:
            effect->m_textures[0] = nullptr;
            effect->m_textures[1] = nullptr;
            effect->m_textures[2] = nullptr;
            break;
        case 6:
            values[0] = bits(0x3e30b0b1);
            values[1] = bits(0x3eaaaaab);
            values[2] = bits(0x3e2cacad);
            values[5] = 1.0f;
            values[3] = 1.0f;
            values[4] = 1.0f;
            *reinterpret_cast<uint8_t*>(&values[6]) = 0;
            break;
        case 7:
            values[0] = bits(0x3dd8d8d9);
            values[1] = bits(0x3e28a8a9);
            values[2] = bits(0x3e8a8a8b);
            values[3] = bits(0x3f088889);
            values[4] = bits(0x3f33b3b4);
            values[5] = bits(0x3f3bbbbc);
            values[6] = 1.0f;
            values[7] = 1.0f;
            values[8] = 1.0f;
            values[9] = 0.5f;
            values[10] = 1.0f;
            break;
        default:
            break;
        }
        return;
    }
    std::memcpy(values, params, 0x78);
    if (type == 5 && !this->ComputeEffectProjection(effect)) {
        this->ClearEffect();
        this->m_effect2 = nullptr;
    }
}

// The effect's projection from the visible vertices' extent (FUN_1411c18e0).
// ref: FUN_1411c18e0
bool CM2Model::ComputeEffectProjection(M2ModelEffect* effect) {
    (void)effect;
    FROST_UNIMPLEMENTED();  // the projection and its three textures
    return false;
}

// The skin-LOD handlers run when the shown section count changes (FUN_1411ceb60).
// ref: FUN_1411ceb60
void CM2Model::SetShownSectionCount(int32_t count) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        M2ModelCall* call = QueueCall(this, 0x1c);
        if (call != nullptr) {
            call->m_args[0] = static_cast<uint32_t>(count);
            LinkCall(this, call);
        }
        return;
    }
    if (count <= 0 || this->CountShownSections() == count) {
        return;
    }
    OsMutexLock(&s_lodHandlerLock);
    for (uint64_t i = 0; i < s_lodHandlerCount; i++) {
        s_lodHandlers[i].m_callback(this, s_lodHandlers[i].m_data);
    }
    OsMutexUnlock(&s_lodHandlerLock);
}

// The current LOD's shown sections (FUN_1411b66f0).
// ref: FUN_1411b66f0
int32_t CM2Model::CountShownSections() {
    uint16_t sections = this->m_shared->m_skins[0].m_profile->m_skinSections.m_count;
    int32_t shown = 0;
    for (uint32_t i = 0; i < sections; i++) {
        if ((this->m_lodBoneMasks[0][i >> 3] & static_cast<uint8_t>(1 << (i & 7))) != 0) {
            shown++;
        }
    }
    return shown;
}

// An animation event's position by id, zero when the file has none (FUN_1411bd0c0).
// ref: FUN_1411bd0c0
C3Vector* CM2Model::GetEventPosition(C3Vector* out, int32_t id) {
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    M2Data* data = this->m_shared->m_data;
    for (uint32_t i = 0; i < data->m_events.Count(); i++) {
        auto event = reinterpret_cast<const uint8_t*>(&data->m_events[i]);
        if (*reinterpret_cast<const int32_t*>(event) == id) {
            std::memcpy(out, event + 0xc, sizeof(C3Vector));
            return out;
        }
    }
    *out = C3Vector{0.0f, 0.0f, 0.0f};
    return out;
}

// A child placed at an event's position under attachment 0x13 (type 0x1d's replay).
void CM2Model::AttachChildAtEvent(CM2BaseModel* child, float scale, int32_t eventId, bool clamp) {
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    MatrixScaleUniform(m, scale);
    C3Vector position;
    this->GetEventPosition(&position, eventId);
    m[12] = position.x;
    m[13] = position.y;
    m[14] = position.z;
    if (clamp) {
        if (position.x <= 0.05f) {  // DAT_14261a974
            m[12] = 0.3f;
        }
        if (0.05f < std::fabs(position.z)) {
            m[14] = 0.0f;
        }
    }
    static_cast<CM2Model*>(child)->StopParticles();
    child->AttachTo(this, 0x13, nullptr, false);
    child->SetLocalMatrix(m);
}

// The GPU batches (FUN_1411c6d30): not made by this port yet.
// ref: FUN_1411c6d30
void CM2Model::CreateGpuBatches() {
    FROST_UNIMPLEMENTED();
}

// A model driven by its parent's bones takes the parent's bone states and matrices
// (FUN_1411c8550).
// ref: FUN_1411c8550
void CM2Model::ShareParentBones() {
    FROST_UNIMPLEMENTED();
}
