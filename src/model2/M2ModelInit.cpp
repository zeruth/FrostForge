#include <cmath>
#include <cstring>

#include "db/DB2Tables.hpp"
#include "db/WowDB2Lookup.hpp"
#include "model2/M2Camera.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Ribbon.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Model.cpp: a model set up once its file is in (FUN_1411c24f0). One
// allocation holds every per-instance array, carved in a fixed order; the bone and texture
// matrices live in a second, 16-byte aligned one.

namespace {

const char s_modelFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Model.cpp";

// A half float's bits as a float (FUN_142364480).
// ref: FUN_142364480
float HalfToFloat(uint16_t half) {
    uint32_t mantissa = half & 0x3ff;
    uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    uint32_t exponent = half >> 10 & 0x1f;
    uint32_t bits;
    if (exponent == 0x1f) {
        bits = (mantissa | 0x3fc00) << 13 | sign;
    } else if (exponent != 0) {
        bits = ((exponent + 0x70) * 0x400 | mantissa) << 13 | sign;
    } else {
        exponent = 0x71;
        if (mantissa != 0) {
            uint32_t shifted;
            uint32_t previous;
            do {
                shifted = mantissa * 2;
                exponent--;
                previous = mantissa & 0x7fffffff;
                mantissa = shifted;
            } while ((previous >> 9 & 1) == 0);
            mantissa = shifted & 0x3ff;
        }
        bits = (exponent << 10 | mantissa) << 13 | sign;
    }
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A 7.9 fixed point with its sign in bit 15 (the particle texture scroll and scale).
float Fixed7_9(uint16_t value) {
    float sign = 1.0f;  // DAT_142618d64
    if (static_cast<int16_t>(value) < 0) {
        sign = -1.0f;  // DAT_14261a9c8
    }
    uint16_t magnitude = static_cast<int16_t>(value) < 0 ? static_cast<uint16_t>(value & 0x7fff) : value;
    return (static_cast<float>(magnitude & 0x1ff) * 0.00390625f + static_cast<float>(magnitude >> 9)) * sign;  // DAT_1426c6fdc
}

// The most skin sections any of the file's LOD skin profiles has (FUN_1411a8590).
// ref: FUN_1411a8590
uint16_t MaxSkinSections(const CM2Shared* shared) {
    uint16_t most = 0;
    for (uint32_t i = 0; i <= shared->m_lodMax; i++) {
        uint16_t count = shared->m_skins[i].m_profile->m_skinSections.m_count;
        most = most < count ? count : most;
    }
    return most;
}

// A light's DETL entry (FUN_1411a8570): null without the chunk.
// ref: FUN_1411a8570
const uint16_t* LightDetail(const CM2Shared* shared, uint32_t light) {
    if (shared->m_detailLayers == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(shared->m_detailLayers) + light * 0xc);
}

// A particle emitter's share of the file's particle data (FUN_1411a83b0): a zero record when
// there is none (DAT_1430a0f28).
// ref: FUN_1411a83b0
const M2Track<C3Vector>* ParticleData(const CM2Shared* shared, uint32_t particle) {
    static const uint8_t s_empty[0x14] = {};
    if (shared->m_particleData == nullptr) {
        return reinterpret_cast<const M2Track<C3Vector>*>(s_empty);
    }
    return reinterpret_cast<const M2Track<C3Vector>*>(static_cast<const uint8_t*>(shared->m_particleData) + particle * 0x14);
}

// The ribbon quality a file asks for (ModelRibbonQuality by FileDataID, its first nonzero
// RibbonQualityID): 2 by default.
uint8_t RibbonQualityFor(int32_t fileDataId) {
    WowDB2ParentLookup* lookup = g_modelRibbonQualityDB.m_parentLookup;
    if (lookup == nullptr) {
        return 2;
    }
    DB2PARENTITER it;
    DB2PARENTITER end;
    lookup->Begin(&it, fileDataId);
    lookup->End(&end);
    while (std::memcmp(&it, &end, sizeof(it)) != 0) {
        void* record = lookup->Record(&it);
        uint8_t quality = static_cast<uint8_t>(g_modelRibbonQualityDB.FieldValue(static_cast<uint8_t*>(record), 0, 0));
        if (quality != 0) {
            return quality;
        }
        lookup->Next(&it);
    }
    return 2;
}

// A bone state's starting fields, as the instance block lays them (the reference's order:
// the 0x1c..0x2b clear undoes the 1 stored at 0x28; the tracks' starting values come after).
void InitBoneState(M2BoneState* state) {
    auto raw = reinterpret_cast<uint8_t*>(state);
    std::memset(raw, 0, 0x18);
    *reinterpret_cast<float*>(raw + 0x28) = 1.0f;
    *reinterpret_cast<uint32_t*>(raw + 0x2c) = 0;
    *reinterpret_cast<uint32_t*>(raw + 0x18) = 0;
    std::memset(raw + 0x1c, 0, 0x10);
    std::memset(raw + 0x30, 0, 0x10);
    state->m_override = nullptr;
    state->m_flags = 0;
    state->m_5c = 0xffffffff;
    state->m_variation = 0;
    state->m_ownerNext = 0xffff;
    state->m_blendEnd = 0;
    state->m_blendRate = 0.0f;
    state->m_blendMax = 1.0f;
    state->m_blendWeight = 0.0f;
    state->m_weight = 1.0f;
    state->m_inheritedWeight = 1.0f;
    state->m_sequence = nullptr;
    state->m_blendSequence = nullptr;
    state->m_ownerLink = nullptr;
    state->m_blendKind = 5;
    state->m_fixedBlend = 0;
}

// A texture transform's state: the bone state's first 0x40 bytes laid out the same way.
void InitTransformState(M2TextureTransformState* state) {
    auto raw = reinterpret_cast<uint8_t*>(state);
    std::memset(raw, 0, 0x18);
    *reinterpret_cast<float*>(raw + 0x28) = 1.0f;
    *reinterpret_cast<uint32_t*>(raw + 0x2c) = 0;
    *reinterpret_cast<uint32_t*>(raw + 0x18) = 0;
    std::memset(raw + 0x1c, 0, 0x10);
    std::memset(raw + 0x30, 0, 0x10);
}

const C4Quaternion IDENTITY_QUAT = {0.0f, 0.0f, 0.0f, 1.0f};  // DAT_14261aa10

}  // namespace

// ref: FUN_1411c24f0
bool CM2Model::InitializeLoaded() {
    CM2Shared* shared = this->m_shared;
    this->m_flags ^= (((shared->m_flags >> 13) << 27) ^ this->m_flags) & 0x8000000;
    M2Data* data = shared->m_data;
    this->m_bounds = data->m_bounds;
    data = shared->m_data;
    float cz = (data->m_bounds.m_min.z + data->m_bounds.m_max.z) * 0.5f;
    float cx = (data->m_bounds.m_min.x + data->m_bounds.m_max.x) * 0.5f;
    float cy = (data->m_bounds.m_min.y + data->m_bounds.m_max.y) * 0.5f;
    this->m_center = C3Vector{cx, cy, cz};
    this->m_radius = data->m_boundsRadius;
    data = shared->m_data;
    this->m_collisionBounds = data->m_collisionBounds;
    this->m_collisionRadius = data->m_collisionRadius;
    if (data->m_collisionIndices.Count() == 0) {
        this->m_flags2 &= 0xfb;
        this->m_collisionCenter = C3Vector{0.0f, 0.0f, 0.0f};
        this->m_collisionSphereRadius = 0.0f;
    } else {
        this->m_flags2 |= 4;
        float z = (this->m_collisionBounds.m_max.z + this->m_collisionBounds.m_min.z) * 0.5f;
        float x = (this->m_collisionBounds.m_max.x + this->m_collisionBounds.m_min.x) * 0.5f;
        float y = (this->m_collisionBounds.m_max.y + this->m_collisionBounds.m_min.y) * 0.5f;
        this->m_collisionCenter = C3Vector{x, y, z};
        this->m_collisionSphereRadius = this->m_collisionRadius;
    }
    if (shared->m_skeleton != nullptr) {
        this->m_skeleton = M2SkeletonCreateInstance(shared->m_skeleton, this->m_scene, this->m_createFlags);
        if (this->m_skeleton == nullptr) {
            return false;
        }
    }
    data = this->m_shared->m_data;
    uint32_t bones = data->m_bones.Count();
    uint32_t loops = data->m_loops.Count();
    uint32_t attachments = data->m_attachments.Count();
    if (this->m_skeleton != nullptr) {
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(this->m_skeleton) + 0x70) = this->m_initTime;
        bones = 0;
        loops = 0;
        attachments = 0;
    }
    // The emitters (and their generators) live in the block too.
    uint32_t particles = data->m_particles.Count();
    uint32_t emitterBytes = particles * 0x2f8;
    if ((this->m_createFlags & 0x800) == 0) {
        for (uint32_t i = 0; i < particles; i++) {
            switch (data->m_particles[i].m_emitterType) {
            case 1:
                emitterBytes += 0x60;
                break;
            case 2:
                emitterBytes += 0x68;
                break;
            case 3:
                emitterBytes += 0x228;
                break;
            case 4:
                emitterBytes += 0xa8;
                break;
            }
        }
    } else if (particles != 0) {
        emitterBytes = particles * 0x390;
    }
    this->m_physicsBoneCount = 0;
    this->m_5f0 = 0;
    const M2SkinProfile* skin = this->m_shared->m_skins[0].m_profile;
    uint16_t batchCount = skin->m_batches.m_count;
    for (uint32_t i = 0; i < batchCount; i++) {
        const M2Batch& batch = skin->m_batches[i];
        uint16_t geoset = batch.m_geosetIndex;
        if ((geoset & 2) != 0) {
            this->m_physicsBoneCount++;
            geoset = batch.m_geosetIndex;
        }
        if ((batch.m_flags & 4) != 0 || (geoset & 2) != 0) {
            this->m_5f0 = batchCount;
        }
    }
    uint32_t blockBytes = emitterBytes
        + (loops
              + (data->m_textureWeights.Count() + attachments
                    + (data->m_cameras.Count() + (data->m_particles.Count() + bones) * 2) * 6)
                  * 3
              + (data->m_ribbons.Count() * 0x46
                    + (data->m_colors.Count()
                          + (data->m_textureTransforms.Count() + data->m_lights.Count() * 4 + 3) * 2
                          + this->m_physicsBoneCount * 7)
                        * 4
                    + data->m_textures.Count() + this->m_5f0 + data->m_particles.Count())
                  * 2)
            * 4;
    uint32_t boneDoneBytes = (bones + 7) >> 3;
    if ((boneDoneBytes & 3) != 0) {
        boneDoneBytes = (boneDoneBytes + 4) & ~3u;
    }
    if (this->m_skeleton != nullptr) {
        boneDoneBytes = 0;
    }
    uint32_t lodMaskBytes = (static_cast<uint32_t>(MaxSkinSections(this->m_shared)) + 7) >> 3;
    if ((lodMaskBytes & 3) != 0) {
        lodMaskBytes = (lodMaskBytes + 4) & ~3u;
    }
    uint32_t matrixBytes = (data->m_textureTransforms.Count() + bones) * 0x40;
    auto block = static_cast<uint8_t*>(SMemAlloc(boneDoneBytes + lodMaskBytes * 8 + blockBytes, 0x12, s_modelFile, 0, 0x64d, 0));
    if (block == nullptr) {
        return false;
    }
    auto matrices = static_cast<C44Matrix*>(M2AlignedAlloc(matrixBytes, s_modelFile, 0x654, 0x12, 0));
    if (matrices == nullptr) {
        return false;
    }
    this->m_block = block;
    this->m_matrixBlock = matrices;
    uint8_t* cursor = block;
    C44Matrix* matrixCursor = matrices;
    if (bones != 0) {
        if ((this->m_modelFlags2 & 0x800) == 0) {
            this->m_boneStates = reinterpret_cast<M2BoneState*>(cursor);
            cursor += bones * sizeof(M2BoneState);
            for (uint32_t i = 0; i < bones; i++) {
                InitBoneState(&this->m_boneStates[i]);
            }
            for (uint32_t i = 0; i < bones; i++) {
                const M2CompBone& bone = data->m_bones[i];
                M2BoneState* state = &this->m_boneStates[i];
                M2TrackInit(&bone.m_rotation, &state->m_rotation, &IDENTITY_QUAT);
                C3Vector one = {1.0f, 1.0f, 1.0f};
                M2TrackInit(&bone.m_scale, &state->m_scale, &one);
                C3Vector zero = {0.0f, 0.0f, 0.0f};
                M2TrackInit(&bone.m_translation, &state->m_translation, &zero);
                state->m_flags = static_cast<uint16_t>(bone.m_flags);
            }
            if ((this->m_shared->m_flags & CM2Shared::SHARED_HAS_PHYSICS_BONES) != 0) {
                this->m_physicsBones = SMemAlloc(bones * 0xc, 0x12, s_modelFile, 0, 0x672, 0);
            }
            this->m_boneMatrices = matrices;
            for (uint32_t i = 0; i < bones; i++) {
                std::memset(&this->m_boneMatrices[i], 0, sizeof(C44Matrix));
                this->m_boneMatrices[i].m[0] = 1.0f;
                this->m_boneMatrices[i].m[5] = 1.0f;
                this->m_boneMatrices[i].m[10] = 1.0f;
                this->m_boneMatrices[i].m[15] = 1.0f;
            }
        }
        matrixCursor = matrices + data->m_bones.Count();
    }
    if (loops != 0) {
        this->m_globalTimes = reinterpret_cast<uint32_t*>(cursor);
        cursor += data->m_loops.Count() * 4;
        for (uint32_t i = 0; i < data->m_loops.Count(); i++) {
            this->m_globalTimes[i] = 0;
        }
    }
    if (data->m_colors.Count() != 0) {
        this->m_colors = reinterpret_cast<M2ColorState*>(cursor);
        cursor += data->m_colors.Count() * sizeof(M2ColorState);
        for (uint32_t i = 0; i < data->m_colors.Count(); i++) {
            M2ColorState* state = &this->m_colors[i];
            std::memset(state, 0, sizeof(M2ColorState));
            C3Vector zero = {0.0f, 0.0f, 0.0f};
            M2TrackInit(&data->m_colors[i].m_color, &state->m_color, &zero);
            float one = 1.0f;
            M2TrackInitFixed16(&data->m_colors[i].m_alpha, &state->m_alpha, &one);
        }
    }
    if (data->m_textures.Count() != 0) {
        this->m_textures = reinterpret_cast<void**>(cursor);
        cursor += data->m_textures.Count() * sizeof(void*);
        void** source = this->m_donor == nullptr ? this->m_shared->m_textures : this->m_donor->m_textures;
        for (uint32_t i = 0; i < data->m_textures.Count(); i++) {
            void* texture = source[i];
            if (texture != nullptr) {
                TextureAddRef(static_cast<HTEXTURE>(texture));
            }
            this->m_textures[i] = texture;
        }
    }
    this->m_4e0 = cursor;
    std::memset(cursor, 0, 5 * sizeof(void*));
    cursor += 5 * sizeof(void*);
    if (data->m_textureWeights.Count() != 0) {
        this->m_textureWeights = reinterpret_cast<M2TrackState<float>*>(cursor);
        cursor += data->m_textureWeights.Count() * 0xc;
        for (uint32_t i = 0; i < data->m_textureWeights.Count(); i++) {
            M2TrackState<float>* state = &this->m_textureWeights[i];
            std::memset(state, 0, 0xc);
            float one = 1.0f;
            M2TrackInitFixed16(&data->m_textureWeights[i].m_weight, state, &one);
        }
    }
    if (data->m_textureTransforms.Count() != 0) {
        this->m_textureTransforms = reinterpret_cast<M2TextureTransformState*>(cursor);
        cursor += data->m_textureTransforms.Count() * sizeof(M2TextureTransformState);
        for (uint32_t i = 0; i < data->m_textureTransforms.Count(); i++) {
            M2TextureTransformState* state = &this->m_textureTransforms[i];
            const M2TextureTransform& def = data->m_textureTransforms[i];
            InitTransformState(state);
            M2TrackInit(&def.m_rotation, &state->m_rotation, &IDENTITY_QUAT);
            C3Vector one = {1.0f, 1.0f, 1.0f};
            M2TrackInit(&def.m_scale, &state->m_scale, &one);
            C3Vector zero = {0.0f, 0.0f, 0.0f};
            M2TrackInit(&def.m_translation, &state->m_translation, &zero);
        }
        this->m_textureMatrices = matrixCursor;
    }
    if (attachments != 0) {
        this->m_attachmentStates = reinterpret_cast<M2TrackState<uint8_t>*>(cursor);
        cursor += data->m_attachments.Count() * 0xc;
        for (uint32_t i = 0; i < data->m_attachments.Count(); i++) {
            M2TrackState<uint8_t>* state = &this->m_attachmentStates[i];
            state->m_key = 0;
            state->m_blendKey = 0;
            state->m_value = 0;
            uint8_t one = 1;
            M2TrackInit(&data->m_attachments[i].m_animate, state, &one);
        }
    }
    if (data->m_lights.Count() != 0) {
        this->m_lights = reinterpret_cast<M2LightState*>(cursor);
        cursor += data->m_lights.Count() * sizeof(M2LightState);
        for (uint32_t i = 0; i < data->m_lights.Count(); i++) {
            const M2Light& def = data->m_lights[i];
            const uint16_t* detail = LightDetail(this->m_shared, i);
            M2LightState* state = &this->m_lights[i];
            std::memset(state, 0, 0x61);
            new (&state->m_lighting) CM2Lighting();
            state->m_64 = 1;
            state->m_lighting.SetScene(this->m_scene);
            state->m_lighting.SetLightType(def.m_type);
            if (detail != nullptr) {
                state->m_lighting.m_78 = HalfToFloat(detail[1]);
                float second = HalfToFloat(detail[2]);
                if (std::fabs(second) < 2.3841858e-07f) {  // DAT_14261a954
                    second = 1.0f;
                }
                state->m_lighting.m_7c = second;
                uint16_t flags = static_cast<uint16_t>((detail[0] & 1) | (state->m_lighting.m_flags & 0xfffe));
                flags = static_cast<uint16_t>(((flags ^ detail[0]) & 2) ^ flags);
                state->m_lighting.m_flags = static_cast<uint16_t>(((detail[0] ^ flags) & 4) ^ flags);
            }
            uint8_t visible = 1;
            M2TrackInit(&def.m_visibility, &state->m_visibility, &visible);
            float one = 1.0f;
            M2TrackInit(&def.m_ambientIntensity, &state->m_ambientIntensity, &one);
            C3Vector zero = {0.0f, 0.0f, 0.0f};
            M2TrackInit(&def.m_ambientColor, &state->m_ambientColor, &zero);
            float diffuse = 1.0f;
            M2TrackInit(&def.m_diffuseIntensity, &state->m_diffuseIntensity, &diffuse);
            C3Vector black = {0.0f, 0.0f, 0.0f};
            M2TrackInit(&def.m_diffuseColor, &state->m_diffuseColor, &black);
            float start = 0.0f;
            M2TrackInit(&def.m_attenuationStart, &state->m_attenuationStart, &start);
            float end = 1.0f;
            M2TrackInit(&def.m_attenuationEnd, &state->m_attenuationEnd, &end);
        }
    }
    if (data->m_cameras.Count() != 0) {
        this->m_cameras = reinterpret_cast<M2CameraState*>(cursor);
        cursor += data->m_cameras.Count() * sizeof(M2CameraState);
        for (uint32_t i = 0; i < data->m_cameras.Count(); i++) {
            std::memset(&this->m_cameras[i], 0, sizeof(M2CameraState));
        }
        for (uint32_t i = 0; i < data->m_cameras.Count(); i++) {
            const M2Camera& def = data->m_cameras[i];
            CM2Camera* camera = M2CameraCreateDefault();
            float fieldOfView = 1.5707964f;  // DAT_142632530
            for (uint32_t k = 0; k < def.m_fieldOfView.m_values.Count(); k++) {
                if (def.m_fieldOfView.m_values[k].Count() != 0) {
                    fieldOfView = def.m_fieldOfView.m_values[k][0].m_value;
                    break;
                }
            }
            M2CameraSetFloat(camera, CM2Camera::FLOAT_FIELD_OF_VIEW, fieldOfView);
            M2CameraSetFloat(camera, CM2Camera::FLOAT_NEAR_CLIP, def.m_nearClip);
            M2CameraSetFloat(camera, CM2Camera::FLOAT_FAR_CLIP, def.m_farClip);
            M2CameraState* state = &this->m_cameras[i];
            state->m_camera = camera;
            state->m_position.m_value = C3Vector{0.0f, 0.0f, 0.0f};
            if (def.m_positions.m_values.Count() != 0 && def.m_positions.m_values[0].Count() != 0) {
                state->m_position.m_value = def.m_positions.m_values[0][0].m_value;
            }
            state->m_target.m_value = C3Vector{0.0f, 0.0f, 0.0f};
            if (def.m_targetPositions.m_values.Count() != 0 && def.m_targetPositions.m_values[0].Count() != 0) {
                state->m_target.m_value = def.m_targetPositions.m_values[0][0].m_value;
            }
            state->m_roll.m_value = 0.0f;
            if (def.m_roll.m_values.Count() != 0 && def.m_roll.m_values[0].Count() != 0) {
                state->m_roll.m_value = def.m_roll.m_values[0][0].m_value;
            }
            state->m_fieldOfView.m_value = fieldOfView;
            if (def.m_fieldOfView.m_values.Count() != 0 && def.m_fieldOfView.m_values[0].Count() != 0) {
                state->m_fieldOfView.m_value = def.m_fieldOfView.m_values[0][0].m_value;
            }
        }
    }
    if (data->m_ribbons.Count() != 0) {
        uint32_t count = data->m_ribbons.Count();
        this->m_ribbonStates = reinterpret_cast<M2RibbonState*>(cursor);
        cursor += count * sizeof(M2RibbonState);
        for (uint32_t i = 0; i < count; i++) {
            std::memset(&this->m_ribbonStates[i], 0, 0x4d);
        }
        this->m_ribbons = reinterpret_cast<CRibbonEmitter**>(cursor);
        std::memset(cursor, 0, count * sizeof(CRibbonEmitter*));
        cursor += count * sizeof(CRibbonEmitter*);
        uint8_t* emitters = cursor;
        cursor += count * sizeof(CRibbonEmitter);
        for (uint32_t i = 0; i < count; i++) {
            const M2Ribbon& def = data->m_ribbons[i];
            CRibbonEmitter* ribbon = RibbonEmitterConstruct(emitters);
            emitters += sizeof(CRibbonEmitter);
            this->m_ribbons[i] = ribbon;
            CTexture* textures[3] = {nullptr, nullptr, nullptr};
            uint32_t types[3] = {0, 0, 0};
            for (uint32_t t = 0; t < def.m_textureIndices.Count(); t++) {
                uint16_t index = def.m_textureIndices[t];
                textures[t] = static_cast<CTexture*>(this->m_shared->m_textures[index]);
                types[t] = this->m_shared->m_data->m_textures[index].m_type;
            }
            uint16_t material = def.m_materialIndices[0];
            uint32_t blend = this->m_shared->m_data->m_materials[material].m_blendMode;
            uint32_t color = 0xffffffff;
            RibbonEmitterInitialize(ribbon, def.m_edgeLifetime, &color, textures, &blend, types);
            RibbonEmitterSetGravity(ribbon, def.m_gravity);
            RibbonEmitterSetVisible(ribbon, false);
            uint8_t quality = RibbonQualityFor(this->GetFileDataId());
            RibbonEmitterSetQuality(ribbon, quality);
            M2RibbonState* state = &this->m_ribbonStates[i];
            uint8_t visible = 1;
            M2TrackInit(&def.m_visibility, &state->m_visibility, &visible);
            C3Vector zero = {0.0f, 0.0f, 0.0f};
            M2TrackInit(&def.m_color, &state->m_color, &zero);
            float one = 1.0f;
            M2TrackInitFixed16(&def.m_alpha, &state->m_alpha, &one);
            float above = 0.0f;
            M2TrackInit(&def.m_heightAbove, &state->m_heightAbove, &above);
            float below = 0.0f;
            M2TrackInit(&def.m_heightBelow, &state->m_heightBelow, &below);
            uint16_t slot = 0;
            M2TrackInit(&def.m_textureSlot, &state->m_textureSlot, &slot);
        }
    }
    if (data->m_particles.Count() != 0) {
        uint32_t count = data->m_particles.Count();
        this->m_particleStates = reinterpret_cast<M2ParticleState*>(cursor);
        cursor += count * sizeof(M2ParticleState);
        for (uint32_t i = 0; i < count; i++) {
            M2ParticleState* state = &this->m_particleStates[i];
            std::memset(state, 0, 0x8c);
            state->m_8c = 1;
            state->m_8d = 1;
            state->m_8e = 0;
        }
        this->m_particles = reinterpret_cast<CParticleEmitter2**>(cursor);
        std::memset(cursor, 0, count * sizeof(CParticleEmitter2*));
        cursor += count * sizeof(CParticleEmitter2*);
        uint8_t* memory = cursor;
        cursor += emitterBytes;
        for (uint32_t i = 0; i < count; i++) {
            const M2Particle& def = data->m_particles[i];
            M2ParticleState* state = &this->m_particleStates[i];
            CParticleEmitter2* emitter = ParticleEmitterConstruct(memory, this->m_scene);
            memory += sizeof(CParticleEmitter2);
            this->m_particles[i] = emitter;
            float params[2] = {def.m_emissionRateVary, def.m_lifespanVary};
            CParticleGenerator* generator = nullptr;
            void* random = &emitter->m_random;
            if ((this->m_createFlags & 0x800) == 0) {
                switch (def.m_emitterType) {
                case 1:
                    generator = ParticleGeneratorCreatePlane(memory, random, params);
                    memory += 0x60;
                    break;
                case 2:
                    generator = ParticleGeneratorCreateSphere(memory, random, params, (def.m_flags >> 8 & 1) != 0);
                    memory += 0x68;
                    break;
                case 3:
                    generator = ParticleGeneratorCreateSpline(memory, random, params, def.m_splinePoints.m_count,
                        def.m_splinePoints.Count() != 0 ? &def.m_splinePoints[0] : nullptr);
                    memory += 0x228;
                    break;
                case 4: {
                    if ((def.m_flags & 0x1000000) == 0) {
                        generator = ParticleGeneratorCreateBone(memory, random, params);
                    } else {
                        generator = ParticleGeneratorCreateBoneMirrored(memory, random, params);
                    }
                    memory += 0xa8;
                    CM2BaseModel* parent = this->m_parent;
                    if (parent != nullptr && parent->m_type != 0) {
                        parent = nullptr;
                    }
                    ParticleGeneratorSetModel(generator, static_cast<CM2Model*>(parent));
                    break;
                }
                default:
                    break;
                }
            } else {
                generator = ParticleGeneratorCreateBox(memory, random, params);
                memory += 0x98;
            }
            emitter->m_flags &= ~1u;
            emitter->m_generator = generator;
            uint8_t enabled = 1;
            M2TrackInit(&def.m_enabledIn, &state->m_enabled, &enabled);
            M2TrackInit(&def.m_emissionSpeed, &state->m_emissionSpeed, &generator->m_speed);
            M2TrackInit(&def.m_speedVariation, &state->m_speedVariation, &generator->m_speedVariation);
            M2TrackInit(&def.m_lifespan, &state->m_lifespan, &generator->m_lifespan);
            M2TrackInit(&def.m_emissionRate, &state->m_emissionRate, &generator->m_emissionRate);
            M2TrackInit(ParticleData(this->m_shared, i), &state->m_18, &generator->m_gravity);
            float zSource = 0.0f;
            M2TrackInit(&def.m_zSource, &state->m_zSource, &zSource);
            M2TrackInit(&def.m_emissionAreaLength, &state->m_emissionAreaLength, &generator->m_areaLength);
            M2TrackInit(&def.m_emissionAreaWidth, &state->m_emissionAreaWidth, &generator->m_areaWidth);
            M2TrackInit(&def.m_verticalRange, &state->m_verticalRange, &generator->m_verticalRange);
            M2TrackInit(&def.m_horizontalRange, &state->m_horizontalRange, &generator->m_horizontalRange);
            generator->m_speed = state->m_emissionSpeed.m_value;
            generator->m_speedVariation = state->m_speedVariation.m_value;
            generator->m_lifespan = state->m_lifespan.m_value;
            generator->m_emissionRate = state->m_emissionRate.m_value;
            generator->m_gravity = state->m_18.m_value;
            M2Array<M2ExtendedParticle>* extended = this->m_shared->m_extendedParticles;
            generator->m_zSource = extended == nullptr ? state->m_zSource.m_value : (*extended)[i].m_zSource;
            int32_t kind = 0;  // (set on every path that sets textured)
            bool textured = false;
            if ((def.m_flags & 0x10100000) == 0) {
                if ((this->m_createFlags & 0x800) != 0) {
                    kind = 4;
                    textured = true;
                } else {
                    uint32_t* geometryIds = this->m_shared->m_geometryParticleIds;
                    if (geometryIds == nullptr || geometryIds[i] == 0) {
                        const char* name = def.m_geometryModelFilename.Count() != 0 ? &def.m_geometryModelFilename[0] : nullptr;
                        if (name == nullptr || *name == '\0') {
                            ParticleEmitterSetKind(emitter, 0);
                        } else {
                            ParticleEmitterSetKind(emitter, 1);
                            ParticleEmitterSetModelByName(emitter, this->m_scene, name);
                        }
                    } else {
                        ParticleEmitterSetKind(emitter, 1);
                        ParticleEmitterSetModelById(emitter, this->m_scene, static_cast<int32_t>(geometryIds[i]));
                    }
                }
            } else {
                if ((this->m_createFlags & 0x800) != 0) {
                    kind = 4;
                } else {
                    kind = static_cast<int32_t>((~(def.m_flags >> 28) & 1) | 2);
                }
                textured = true;
            }
            if (textured) {
                ParticleEmitterSetKind(emitter, kind);
                auto params16 = reinterpret_cast<const uint16_t*>(def.m_multiTextureParams);
                auto bytes = reinterpret_cast<const uint8_t*>(&def.m_particleType);
                float scroll0[2] = {Fixed7_9(params16[0]), Fixed7_9(params16[1])};
                float scale0[2] = {Fixed7_9(params16[4]), Fixed7_9(params16[5])};
                float rotation0 = static_cast<float>(bytes[0] & 0x1f) * 0.03125f + static_cast<float>(bytes[0] >> 5);  // DAT_1427211dc
                ParticleEmitterSetTextureTransform(emitter, 0, rotation0, scroll0, scale0);
                float scroll1[2] = {Fixed7_9(params16[2]), Fixed7_9(params16[3])};
                float scale1[2] = {Fixed7_9(params16[6]), Fixed7_9(params16[7])};
                float rotation1 = static_cast<float>(bytes[1] & 0x1f) * 0.03125f + static_cast<float>(bytes[1] >> 5);
                ParticleEmitterSetTextureTransform(emitter, 1, rotation1, scroll1, scale1);
            }
            uint32_t flags = (def.m_flags & 0x10) == 0 ? emitter->m_flags & ~0x200u : emitter->m_flags | 0x200;
            emitter->m_flags = flags;
            if ((def.m_flags & 0x40) != 0) {
                flags |= 0x800;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x20) != 0) {
                flags |= 0x400;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x800) != 0) {
                flags |= 0x2000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x1000) != 0) {
                flags |= 0x4000;
                emitter->m_flags = flags;
            }
            if (def.m_emitterType == 2 && (def.m_flags & 0x80) != 0) {
                flags |= 0x1000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x200) != 0) {
                flags |= 0x10000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x2000) != 0) {
                flags |= 0x40000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x4000) != 0) {
                flags |= 0x80000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x8000) != 0) {
                flags &= ~1u;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x80000) != 0) {
                flags |= 0x800000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x2000000) != 0) {
                flags |= 0x10000000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x4000000) != 0) {
                flags |= 0x400000;
                emitter->m_flags = flags;
            }
            if ((def.m_flags & 0x8000000) != 0) {
                emitter->m_flags = flags | 0x20000000;
            }
            ParticleEmitterSetFlags(emitter, def.m_flags & 0x20000, def.m_flags & 0x40000, def.m_tailLength,
                def.m_flags & 0x400);
            // The blend: the material's mode, its alpha test and the depth flags.
            uint8_t mode = 0;
            uint8_t mask = 7;
            switch (def.m_blendingType) {
            case 0:
                mode = 0;
                break;
            case 1:
                mode = 1;
                break;
            case 2:
                mode = 2;
                mask = 3;
                break;
            case 3:
                mode = 10;
                mask = 3;
                break;
            case 4:
                mode = 3;
                mask = 3;
                break;
            case 5:
                mode = 4;
                mask = 3;
                break;
            case 6:
                mode = 5;
                mask = 3;
                break;
            case 7:
                mode = 0xd;
                mask = 3;
                break;
            default:
                break;
            }
            uint8_t base = static_cast<uint8_t>((~((def.m_flags >> 3) * 2) & 2)
                | ((this->m_shared->m_data->m_flags >> 12 & 1) << 3));
            uint8_t blend[2];
            blend[0] = mode;
            uint8_t bits = static_cast<uint8_t>(base | (mask & 0xf5));
            if ((def.m_flags >> 28 & 1) == 0) {
                if ((def.m_flags >> 20 & 1) == 0) {
                    bits = static_cast<uint8_t>(((~def.m_flags ^ bits) & 1) ^ bits);
                } else {
                    bits = static_cast<uint8_t>(base | (mask & 0xf4));
                }
                blend[1] = bits;
                ParticleEmitterSetTexture(emitter, 0, static_cast<CTexture*>(this->m_shared->m_textures[def.m_texture]));
            } else {
                blend[1] = static_cast<uint8_t>(((def.m_flags >> 30 & 1) << 5)
                    ^ (((def.m_flags >> 29 & 1) << 4) | base | (mask & 0xe4)));
                uint16_t packed = def.m_texture;
                uint16_t slots[3] = {static_cast<uint16_t>(packed & 0x1f), static_cast<uint16_t>(packed >> 5 & 0x1f),
                    static_cast<uint16_t>(packed >> 10 & 0x1f)};
                for (uint32_t t = 0; t < 3; t++) {
                    ParticleEmitterSetTexture(emitter, t, static_cast<CTexture*>(this->m_shared->m_textures[slots[t]]));
                }
            }
            if ((def.m_flags & 2) != 0) {
                emitter->m_flags |= 0x20;
            }
            if ((def.m_flags & 4) != 0) {
                emitter->m_flags |= 0x200000;
            }
            ParticleEmitterSetTileGrid(emitter, def.m_textureDimensionsRows, def.m_textureDimensionsColumns);
            if ((def.m_flags & 0x10000) != 0) {
                ParticleEmitterSetRandomTiles(emitter, 1);
            }
            if ((def.m_flags & 0x200000) != 0) {
                ParticleEmitterSetRandomStartCell(emitter, 1);
            }
            if (this->m_shared->m_particleCombiners != nullptr) {
                emitter->m_combiner = &this->m_shared->m_particleCombiners[i];
            }
            if (extended != nullptr) {
                emitter->m_extended = &(*extended)[i];
            }
            ParticleEmitterSetBlend(emitter, blend);
            if (static_cast<int32_t>(def.m_flags) < 0) {
                emitter->m_flags |= 0x40000000;
            } else {
                emitter->m_flags |= 0x80000000;
                emitter->m_wind = def.m_windVector;
                emitter->m_windTime = def.m_windTime;
            }
            emitter->m_colorTrack = &def.m_colorTrack;
            emitter->m_alphaTrack = &def.m_alphaTrack;
            emitter->m_scaleTrack = &def.m_scaleTrack;
            emitter->m_scaleVary = def.m_scaleVary;
            emitter->m_headCellTrack = &def.m_headCellTrack;
            emitter->m_tailCellTrack = &def.m_tailCellTrack;
            emitter->m_twinkleSpeed = def.m_twinkleSpeed;
            emitter->m_twinklePercent = def.m_twinklePercent;
            ParticleEmitterSetTwinkleScale(emitter, &def.m_twinkleScaleMin);
            emitter->m_burstMultiplier = def.m_burstMultiplier;
            emitter->m_tumble[0] = def.m_tumble[0];
            emitter->m_tumble[1] = def.m_tumble[3] - def.m_tumble[0];
            emitter->m_tumble[2] = def.m_tumble[1];
            emitter->m_tumble[3] = def.m_tumble[4] - def.m_tumble[1];
            emitter->m_tumble[4] = def.m_tumble[2];
            emitter->m_tumble[5] = def.m_tumble[5] - def.m_tumble[2];
            emitter->m_drag = def.m_drag;
            emitter->m_baseSpin = def.m_baseSpin;
            emitter->m_baseSpinVary = def.m_baseSpinVary;
            emitter->m_spinVary = def.m_spinVary;
            emitter->m_spin = def.m_spin;
            ParticleEmitterSetFollow(emitter, def.m_followSpeed1, def.m_followScale1, def.m_followSpeed2,
                def.m_followScale2);
            emitter->m_tileRotation = static_cast<int16_t>(def.m_textureTileRotation);
            const char* recursion = def.m_recursionModelFilename.Count() != 0 ? &def.m_recursionModelFilename[0] : nullptr;
            if (recursion == nullptr || *recursion == '\0') {
                uint32_t* recursiveIds = this->m_shared->m_recursiveParticleIds;
                if (recursiveIds != nullptr && recursiveIds[i] != 0) {
                    ParticleEmitterSetRecursionModel(emitter, this->m_scene, static_cast<int32_t>(recursiveIds[i]), nullptr);
                }
            } else {
                ParticleEmitterSetRecursionModel(emitter, this->m_scene, 0, recursion);
            }
            ParticleEmitterPrepareFastPath(emitter);
            if (this->m_donor != nullptr) {
                CParticleEmitter2* theirs = this->m_donor->m_particles[i];
                if ((theirs->m_flags & 0x10) != 0) {
                    uint8_t colors[3][4] = {};
                    ParticleEmitterGetColors(theirs, colors[0], colors[1], colors[2]);
                    if (def.m_colorTrack.m_values.m_count == 3) {
                        ParticleEmitterSetColors(emitter, colors[0], colors[1], colors[2]);
                    }
                }
            }
            state->m_enabled.m_value = 1;
        }
    }
    if (this->m_physicsBoneCount != 0) {
        this->m_physicsBoneStates = cursor;
        cursor += this->m_physicsBoneCount * 0xe0;
        for (uint32_t i = 0; i < this->m_physicsBoneCount; i++) {
            M2PhysicsBoneInit(static_cast<uint8_t*>(this->m_physicsBoneStates) + i * 0xe0);
        }
    }
    if (this->m_5f0 != 0) {
        this->m_5f8 = reinterpret_cast<void**>(cursor);
        cursor += this->m_5f0 * sizeof(void*);
        for (uint32_t i = 0; i < this->m_5f0; i++) {
            this->m_5f8[i] = nullptr;
        }
    }
    // FUN_141200610
    this->m_animState = reinterpret_cast<M2AnimState*>(cursor);
    std::memset(this->m_animState, 0, sizeof(M2AnimState));
    cursor += sizeof(M2AnimState);
    this->SetupAnimState();
    if (this->m_skeleton == nullptr) {
        this->m_boneDone = block + blockBytes;
        cursor = this->m_boneDone + boneDoneBytes;
        this->m_boneDoneBytes = static_cast<uint8_t>(boneDoneBytes);
        std::memset(this->m_boneDone, 0, boneDoneBytes);
    } else {
        auto skeleton = static_cast<uint8_t*>(this->m_skeleton);
        this->m_boneDone = *reinterpret_cast<uint8_t**>(skeleton + 0x98);
        this->m_boneDoneBytes = skeleton[0x2e];
    }
    for (uint8_t*& mask : this->m_lodBoneMasks) {
        mask = cursor;
        cursor += lodMaskBytes;
    }
    this->m_lodMaskBytes = static_cast<uint8_t>(lodMaskBytes);
    uint32_t donorSequence = 0;
    if (this->m_donor == nullptr) {
        this->m_modelFlags = ((this->m_shared->m_flags >> 7 ^ this->m_modelFlags) & 2) ^ this->m_modelFlags;
        for (uint32_t lod = 0; lod <= this->m_shared->m_lodMax; lod++) {
            std::memset(this->m_lodBoneMasks[lod], 0xff, this->m_lodMaskBytes);
        }
    } else {
        this->CopyFromDonor(&donorSequence);
    }
    // The children attached before the file came in find their attachment points.
    M2Array<uint16_t>* lookup = this->m_animState->m_attachmentLookup;
    for (CM2BaseModel** link = &this->m_children; *link != nullptr;) {
        CM2BaseModel* child = *link;
        uint32_t id = static_cast<uint32_t>(child->m_attachId);
        uint16_t index = id < lookup->Count() ? (*lookup)[id] : 0xffff;
        if (index == 0xffff && (child->m_flags >> 24 & 1) == 0) {
            child->Detach(true);
        } else {
            child->m_attachIndex = index;
            link = &child->m_siblingNext;
        }
    }
    // A single still bone with nothing else animated only ever animates its root.
    M2AnimState* anim = this->m_animState;
    data = this->m_shared->m_data;
    if ((anim->m_flags & 1) == 0 && anim->m_bones->Count() == 1 && anim->m_sequences->Count() == 1
        && (anim->m_boneStates[0].m_flags & 0x40002f8) == 0 && data->m_lights.Count() == 0
        && data->m_cameras.Count() == 0 && data->m_ribbons.Count() == 0 && data->m_particles.Count() == 0
        && data->m_colors.Count() == 0) {
        this->m_modelFlags |= 0x40000000;
    }
    this->m_modelFlags |= 0x10;
    this->m_flags = (this->m_flags & ~BASE_TEXTURES_READY) | BASE_LOADED;
    M2SequenceResolve stand;
    M2ResolveSequence(0, &stand, anim, anim->m_sequences, anim->m_sequenceHash, nullptr);
    uint32_t first = 0;
    if (!M2HasSequence(static_cast<int16_t>(stand.m_sequence), anim->m_sequences, anim->m_sequenceHash, nullptr)) {
        first = (*anim->m_sequences)[0].m_id;
    }
    this->SetSequence(0xffffffff, first, 0xffffffff, 0, 1.0f, 1, 0, 5, 0, 0, 1.0f);
    if ((static_cast<int32_t>(this->m_modelFlags) >= 0 && (this->m_408 != nullptr || 1 < anim->m_sequences->Count()))
        || (this->m_events != nullptr && data->m_events.Count() != 0)) {
        this->m_modelFlags |= 0x800;
    }
    this->UpdateNoParticlesFlag();
    this->SetSequence(0xffffffff, donorSequence, 0xffffffff, 0, 1.0f, 1, 0, 0, 0, 0, 1.0f);
    if (this->m_parentModel != nullptr && (this->m_flags & BASE_LOADED) != 0) {
        C44Matrix* matrix = this->m_parentModel->m_558;
        C44Matrix* attach = nullptr;
        if (matrix != nullptr) {
            this->m_attachWorld = *matrix;
            attach = &this->m_attachWorld;
        }
        this->SetAttachMatrix(attach);
    }
    this->RunQueuedCalls();
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if ((this->m_shared->m_data->m_flags & 0x20) != 0 && this->m_scene != nullptr) {
        this->RegisterRayTracing();
    }
    if ((this->m_modelFlags2 & 0x800) != 0) {
        this->ShareParentBones();
    }
    if ((this->m_flags & BASE_LOADED) == 0) {
        this->WaitForLoad(0);
    }
    if (this->m_shared->m_data != nullptr) {
        for (CM2Model* above = this->m_parentModel; above != nullptr; above = above->m_parentModel) {
            if (above->m_600 != nullptr) {
                for (uint32_t i = 0; i < this->m_shared->m_data->m_particles.Count(); i++) {
                    this->m_particles[i]->m_flags |= 0x8000;
                }
                break;
            }
        }
        if (this->m_600 != nullptr) {
            FROST_UNIMPLEMENTED();  // FUN_1411b78c0: the GPU batches handed down
        }
    }
    this->RunLoadCallbacks();
    this->m_modelFlags &= ~0x10u;
    if ((this->m_shared->m_flags & CM2Shared::SHARED_TINTED) != 0) {
        auto phase = static_cast<uint8_t*>(M2ModelWindPhaseAlloc());
        std::memset(phase, 0, 0xc);
        this->m_windPhase = phase;
    }
    if ((this->m_shared->m_lodFlags & 3) != 0) {
        this->m_modelFlags2 |= 0x20000;
    }
    return true;
}

// Whether the model may skip its emitters: none, or bounds that say nothing draws (the
// reference's tail of FUN_1411c24f0 between the two sequence starts).
void CM2Model::UpdateNoParticlesFlag() {
    M2Data* data = this->m_shared->m_data;
    if (data->m_ribbons.Count() == 0 && data->m_particles.Count() == 0 && data->m_vertices.m_count == 0) {
        this->m_flags |= BASE_NO_PARTICLES;
        return;
    }
    if (data->m_bounds.m_max.x <= data->m_bounds.m_min.x || data->m_bounds.m_max.y <= data->m_bounds.m_min.y
        || data->m_bounds.m_max.z <= data->m_bounds.m_min.z) {
        if (this->m_skeleton != nullptr) {
            FROST_UNIMPLEMENTED();  // FUN_141204a30: the skeleton's bounds
        }
        return;
    }
    const float* box = nullptr;
    void* skeleton = this->m_skeleton;
    bool useSkeleton = skeleton != nullptr && (*(static_cast<uint8_t*>(skeleton) + 0x28) & 1) != 0;
    if (useSkeleton) {
        FROST_UNIMPLEMENTED();  // the skeleton's sequence bounds
        return;
    }
    if (data->m_bones.Count() == 0 || this->m_boneStates == nullptr) {
        return;
    }
    M2SequenceTime* slot = this->m_boneStates[0].m_sequence;
    box = &data->m_sequences[slot->m_nextSequence].m_bounds.m_extent.m_min.x;
    if (box[0] <= box[3] || box[1] <= box[4] || box[2] <= box[5]) {
        return;
    }
    uint32_t none = data->m_ribbons.Count() != 0 || data->m_particles.Count() != 0 ? 0 : BASE_NO_PARTICLES;
    this->m_flags = (this->m_flags & ~BASE_NO_PARTICLES) | none;
}

// The anim state's arrays: the file's, or the skeleton's (FUN_1411b7d80).
// ref: FUN_1411b7d80
void CM2Model::SetupAnimState() {
    M2Data* data = this->m_shared->m_data;
    M2AnimState* anim = this->m_animState;
    anim->m_bones = &data->m_bones;
    anim->m_sequences = &data->m_sequences;
    anim->m_keyBoneLookup = &data->m_boneIndicesById;
    anim->m_sequenceHash = &data->m_sequenceIdxHashById;
    anim->m_attachments = &data->m_attachments;
    anim->m_attachmentLookup = &data->m_attachmentIndicesById;
    anim->m_boneStates = this->m_boneStates;
    anim->m_globalTimes = this->m_globalTimes;
    anim->m_attachmentStates = this->m_attachmentStates;
    anim->m_boneMatrices = this->m_boneMatrices;
    anim->m_ownerHead = &this->m_3bc;
    if (this->m_skeleton != nullptr) {
        FROST_UNIMPLEMENTED();  // the skeleton's arrays (FUN_141203b20)
    }
}

// The queued calls run in order, each at the scene time it was made (FUN_1411c24f0's tail).
void CM2Model::RunQueuedCalls() {
    CM2Scene* scene = this->m_scene;
    uint32_t savedTime = scene->m_time;
    for (M2ModelCall* call = this->m_calls; call != nullptr; call = this->m_calls) {
        this->m_scene->m_time = call->m_frame;
        const uint32_t* args = call->m_args;
        auto argFloat = [args](int32_t index) {
            float value;
            std::memcpy(&value, &args[index], sizeof(value));
            return value;
        };
        auto argPointer = [args](int32_t index) {
            void* value;
            std::memcpy(&value, &args[index], sizeof(value));
            return value;
        };
        bool releaseModel = false;
        switch (call->m_type) {
        case 0:
            this->SetReplaceableTexture(static_cast<int32_t>(args[0]), static_cast<CTexture*>(argPointer(2)),
                reinterpret_cast<CTexture**>(&call->m_args[4]));
            break;
        case 1:
            this->SetTextureAt(args[0], static_cast<CTexture*>(argPointer(2)));
            break;
        case 2:
            this->SetGeosets(args[0], args[1], args[2]);
            break;
        case 3:
            this->SetSkinSections(args[0], args[1], args[2]);
            break;
        case 4:
            this->CreateGpuBatches();
            break;
        case 5:
            this->SetBoneFlags(args[0], args[1], args[2]);
            break;
        case 6:
            this->SetSequence(args[0], args[1], args[2], static_cast<int32_t>(args[3]), argFloat(4),
                static_cast<int32_t>(args[5]), static_cast<int32_t>(args[6]), *reinterpret_cast<const uint8_t*>(&args[7]),
                *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(&args[7]) + 2),
                static_cast<int32_t>(args[8]), argFloat(9));
            break;
        case 7:
            this->SetBoneSequenceNone(args[0], *reinterpret_cast<const uint8_t*>(&args[1]), static_cast<int32_t>(args[2]),
                static_cast<int32_t>(args[3]), args[4], *reinterpret_cast<const uint16_t*>(&args[5]));
            break;
        case 8:
            this->SetSequenceTime(args[0], static_cast<int32_t>(args[1]));
            break;
        case 9:
            this->SetSequenceSpeed(args[0], argFloat(1));
            break;
        case 10:
            this->SetKeyBoneMatrix(args[0], reinterpret_cast<const C44Matrix*>(&args[1]));
            break;
        case 0xb:
            this->SetLightEnabled(args[0], args[1]);
            break;
        case 0xc:
            this->SetParticleColors(args[0], args[1], args[2], args[3]);
            break;
        case 0xd:
            this->SetParticlesEnabled(static_cast<int32_t>(args[0]));
            break;
        case 0xe:
            this->SetRibbonsEnabled(args[0]);
            break;
        case 0xf:
            this->PreloadSequence(args[0]);
            break;
        case 0x10:
            this->StopParticles();
            break;
        case 0x11:
            this->PrefetchTextures();
            break;
        case 0x12:
            this->SetTimeScale(argFloat(0));
            break;
        case 0x13:
            if (*reinterpret_cast<const uint8_t*>(&args[2]) == 0) {
                this->SetKeyBoneWeight(args[0], argFloat(1));
            } else {
                this->SetBoneWeight(args[0], argFloat(1));
            }
            break;
        case 0x14:
            this->DisableLights();
            break;
        case 0x15:
            this->SetBoneFile(static_cast<int32_t>(args[0]));
            break;
        case 0x16:
            this->ShareBonesWithAttached();
            break;
        case 0x17:
            this->SetRibbonQuality(args[0]);
            break;
        case 0x18:
            this->OnChildLoaded(static_cast<CM2BaseModel*>(argPointer(0)), args[2]);
            releaseModel = true;
            break;
        case 0x1a:
            this->SetEffect(static_cast<int32_t>(args[0]), &args[1]);
            break;
        case 0x1b:
            reinterpret_cast<void (*)(void*, CM2Model*)>(argPointer(2))(argPointer(0), this);
            releaseModel = true;
            break;
        case 0x1c:
            this->SetShownSectionCount(static_cast<int32_t>(args[0]));
            break;
        case 0x1d:
            this->AttachChildAtEvent(static_cast<CM2BaseModel*>(argPointer(0)), argFloat(2),
                static_cast<int32_t>(args[3]), *reinterpret_cast<const uint8_t*>(&args[4]) != 0);
            releaseModel = true;
            break;
        default:
            break;
        }
        if (releaseModel) {
            static_cast<CM2BaseModel*>(argPointer(0))->Release(1);
            std::memset(&call->m_args[0], 0, sizeof(void*));
        }
        this->m_calls = call->m_next;
        M2ModelCallFree(&call);
    }
    this->m_callsTail = &this->m_calls;
    this->m_scene->m_time = savedTime;
}

// A copy of another model takes its children, LOD masks, flags and root sequence, then lets
// it go (FUN_1411c24f0's donor branch). `sequence` gets the donor's root sequence id.
void CM2Model::CopyFromDonor(uint32_t* sequence) {
    CM2Model* donor = this->m_donor;
    for (CM2BaseModel* child = donor->m_children; child != nullptr; child = child->m_siblingNext) {
        CM2BaseModel* copy = M2SceneCloneModel(this->m_scene, child, false);
        if (copy == nullptr) {
            continue;
        }
        bool drivesBones = false;
        if (copy->m_type == 0) {
            CM2Model* source = child->m_type == 0 ? static_cast<CM2Model*>(child) : nullptr;
            drivesBones = (source->m_modelFlags2 >> 11 & 1) != 0;
        }
        copy->AttachTo(this, static_cast<uint32_t>(child->m_attachId), nullptr, drivesBones);
        if (copy->m_type == 0) {
            CM2Model* source = child->m_type == 0 ? static_cast<CM2Model*>(child) : nullptr;
            if ((source->m_modelFlags2 & 0x800) != 0) {
                auto target = static_cast<CM2Model*>(copy);
                target->m_modelFlags2 |= 0x800;
                if (target->m_skeleton != nullptr) {
                    FROST_UNIMPLEMENTED();  // FUN_1412049e0: the skeleton driven from outside
                }
                CM2Model* above = target->m_parentModel;
                if ((above->m_flags & BASE_LOADED) == 0) {
                    M2ModelCall* call = M2ModelCallAlloc();
                    if (call != nullptr) {
                        call->m_next = nullptr;
                        call->m_type = 0x16;
                        call->m_frame = above->m_scene->m_time;
                        *above->m_callsTail = call;
                        above->m_callsTail = &call->m_next;
                    }
                } else {
                    target->ShareParentBones();
                }
                CM2Model* parent = copy->m_parent != nullptr && copy->m_parent->m_type == 0
                    ? static_cast<CM2Model*>(copy->m_parent)
                    : nullptr;
                if (parent != nullptr && (parent->m_flags & BASE_LOADED) != 0 && (target->m_flags & BASE_LOADED) != 0) {
                    for (uint32_t type = 1; type < 0x1c; type++) {
                        if ((parent->m_flags & BASE_LOADED) == 0) {
                            parent->WaitForLoad(0);
                        }
                        M2Array<uint16_t>& lookup = parent->m_shared->m_data->m_textureIndicesById;
                        if (type < lookup.Count() && lookup[type] != 0xffff) {
                            target->SetReplaceableTexture(static_cast<int32_t>(type),
                                static_cast<CTexture*>(parent->m_textures[lookup[type]]),
                                static_cast<CTexture**>(parent->m_4e0));
                        }
                    }
                }
            }
        }
        copy->Release(1);
    }
    this->m_618 = donor->m_618;
    this->m_modelFlags = ((this->m_modelFlags ^ donor->m_modelFlags) & 2) ^ this->m_modelFlags;
    this->CopyLodMasks(donor);
    if (this->m_skeleton != nullptr && donor->m_skeleton != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_1412045d0: the skeleton's time from the donor's
    }
    if (donor->m_600 != nullptr) {
        this->CreateGpuBatches();
    }
    for (uint32_t i = 0; i < this->m_shared->m_data->m_ribbons.Count(); i++) {
        RibbonEmitterSetQuality(this->m_ribbons[i], RibbonEmitterGetQuality(donor->m_ribbons[i]));
    }
    if ((donor->m_flags & BASE_LOADED) == 0) {
        donor->WaitForLoad(0);
    }
    if (donor->m_animState->m_bones->Count() != 0) {
        M2BoneState* root = donor->m_animState->m_boneStates;
        *sequence = (root->m_flags & 0x2000) != 0 ? root->m_5c : 0xffffffff;
    }
    donor->Release(1);
    this->m_donor = nullptr;
}

// The LOD masks copied from another model of the same file, or matched by skin section id
// for another file (FUN_1411b7970).
// ref: FUN_1411b7970
void CM2Model::CopyLodMasks(CM2Model* other) {
    if ((other->m_600 == nullptr || this->m_lod == other->m_lod) && this->m_shared == other->m_shared) {
        for (uint32_t lod = 0; lod <= this->m_shared->m_lodMax; lod++) {
            for (uint32_t i = 0; i < this->m_lodMaskBytes; i++) {
                this->m_lodBoneMasks[lod][i] = other->m_lodBoneMasks[lod][i];
            }
        }
        return;
    }
    int16_t ids[64];
    uint32_t count = 0;
    const M2SkinProfile* theirs = other->m_shared->m_skins[0].m_profile;
    for (uint32_t s = 0; s < theirs->m_skinSections.Count(); s++) {
        if ((other->m_lodBoneMasks[0][s >> 3] & static_cast<uint8_t>(1 << (s & 7))) != 0) {
            if (0x3f < count) {
                break;
            }
            ids[count++] = *reinterpret_cast<const int16_t*>(&theirs->m_skinSections[s]);
        }
    }
    for (uint32_t lod = 0; lod <= this->m_shared->m_lodMax; lod++) {
        uint8_t* mask = this->m_lodBoneMasks[lod];
        for (uint32_t i = 0; i < this->m_lodMaskBytes; i++) {
            mask[i] = 0;
        }
        const M2SkinProfile* ours = this->m_shared->m_skins[lod].m_profile;
        for (uint32_t s = 0; s < ours->m_skinSections.Count(); s++) {
            int16_t id = *reinterpret_cast<const int16_t*>(&ours->m_skinSections[s]);
            for (uint32_t j = 0; j < count; j++) {
                if (id == ids[j]) {
                    mask[s >> 3] |= static_cast<uint8_t>(1 << (s & 7));
                    this->ReleasePhysicsBoneTextures();
                    break;
                }
            }
        }
    }
    auto geosets = static_cast<M2Array<int16_t>*>(this->m_shared->m_particleGeosets);
    if (geosets != nullptr) {
        for (uint32_t i = 0; i < this->m_shared->m_data->m_particles.Count(); i++) {
            int16_t geoset = (*geosets)[i];
            if (geoset == 0 || count == 0) {
                continue;
            }
            for (uint32_t j = 0; j < count; j++) {
                if (geoset == ids[j]) {
                    this->m_particles[i]->m_flags |= 2;
                }
            }
        }
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1411b78c0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141203b20=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412045d0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1412049e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_141204a30=GenForwardMissing")
// ---- end of link fallbacks ----
