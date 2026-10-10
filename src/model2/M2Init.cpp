#include "model2/M2Init.hpp"

#include <cstdarg>

#include "model2/M2Log.hpp"
#include "storm/Log.hpp"

// The reference instantiates one function per element type (M2Init<T>); the templates below
// stand for all of them, each instantiation tagged where it is used.

namespace {

typedef M2Array<uint8_t> RAWARRAY;

// The model's own arrays are fixed now (not keys for one sequence, not already done).
bool FixesMain(const M2INITCONTEXT* context) {
    return context->m_sequence == 0xffffffff && !context->m_skipMain;
}

uint64_t Address(void* base, uint32_t offset) {
    return reinterpret_cast<uint64_t>(base) + offset;
}

void SetPointer(RAWARRAY* array, uint64_t value) {
    array->m_dataLow = static_cast<uint32_t>(value);
    array->m_dataHigh = static_cast<uint16_t>(value >> 32);
}

// An array of `ElemSize` byte elements: checked against the data and pointed at it.
// FUN_141199790 (1), FUN_141199830 (2), FUN_1411998e0 (4), FUN_14119caa0 (8), FUN_14119a170 (12),
// FUN_1411a3d80 (16), FUN_14119a220 (36), FUN_1411a2ac0 (M2Sequence).
template <uint32_t ElemSize>
bool InitArray(void* base, uint32_t size, M2Data* data, void* field, M2INITCONTEXT* context) {
    (void)data;
    if (!FixesMain(context)) {
        return true;
    }
    auto array = static_cast<RAWARRAY*>(field);
    uint32_t offset = array->m_dataLow;
    if (size < offset) {
        M2InitLog("M2Init M2Array failure, offset():(%d) > size:(%d)", offset, size);
        return false;
    }
    uint32_t bytes = static_cast<uint32_t>(array->m_count) * ElemSize;
    if (size < bytes + offset) {
        M2InitLog("M2Init M2Array failure, offset()+bytes():(%d + %d) > size:(%d)", offset, bytes, size);
        return false;
    }
    SetPointer(array, array->m_count == 0 ? 0 : Address(base, offset));
    return true;
}

// An array of arrays of `ElemSize` byte elements (a global sequence's keys).
// FUN_141199990 (1), FUN_141199ab0 (2), FUN_141199bd0 (4), FUN_141199f30 (8), FUN_141199cf0 (12),
// FUN_14119a050 (16), FUN_141199e10 (36).
template <uint32_t ElemSize>
bool InitArrayOfArrays(void* base, uint32_t size, M2Data* data, void* field, M2INITCONTEXT* context) {
    auto array = static_cast<RAWARRAY*>(field);
    if (FixesMain(context)) {
        uint32_t offset = array->m_dataLow;
        if (size < offset) {
            M2InitLog("M2Init M2Array failure, offset():(%d) > size:(%d)", offset, size);
            return false;
        }
        uint32_t bytes = static_cast<uint32_t>(array->m_count) * 8;
        if (size < bytes + offset) {
            M2InitLog("M2Init M2Array failure, offset()+bytes():(%d + %d) > size:(%d)", offset, bytes, size);
            return false;
        }
        SetPointer(array, array->m_count == 0 ? 0 : Address(base, offset));
    }
    auto inner = reinterpret_cast<RAWARRAY*>(reinterpret_cast<M2Array<RAWARRAY>*>(array)->Data());
    for (uint32_t i = 0; i < array->m_count; i++) {
        if (!InitArray<ElemSize>(base, size, data, &inner[i], context)) {
            return false;
        }
    }
    return true;
}

// Whether the keys of sequence `index` are in this file.
bool SequenceInFile(const M2INITCONTEXT* context, uint32_t index) {
    if (context->m_allSequences) {
        return true;
    }
    const M2Array<M2Sequence>* sequences = context->m_sequences;
    return sequences != nullptr && (sequences->Data()[index].m_flags & 0x2020) == 0x20;
}

// One half of a track: the per-sequence key arrays (timestamps or values) of `ElemSize` bytes.
// `iterations` is the timestamps' sequence count, which the values are walked by too.
template <uint32_t ElemSize>
bool InitKeyPart(void* base, uint32_t size, M2Data* data, void* field, int16_t globalSequence,
    const uint16_t* iterations, M2INITCONTEXT* context) {
    auto part = static_cast<RAWARRAY*>(field);
    uint32_t sequence = context->m_sequence;
    if (sequence == 0xffffffff) {
        if (globalSequence != -1) {
            return InitArrayOfArrays<ElemSize>(base, size, data, part, context);
        }
        if (!context->m_skipMain && part->m_count != 0) {
            SetPointer(part, Address(base, part->m_dataLow));
        }
        auto keys = reinterpret_cast<RAWARRAY*>(reinterpret_cast<M2Array<RAWARRAY>*>(part)->Data());
        for (uint32_t i = 0; i < *iterations; i++) {
            if (!SequenceInFile(context, i)) {
                continue;
            }
            RAWARRAY* key = &keys[i];
            uint32_t offset = key->m_dataLow;
            if (size <= offset) {
                M2InitLog("M2InitKeyFrameData M2Array failure, offset():(%d) >= size:(%d)", offset, size);
                return false;
            }
            SetPointer(key, key->m_count == 0 ? 0 : Address(base, offset));
        }
        return true;
    }
    if (part->m_count <= sequence) {
        return true;
    }
    RAWARRAY* key = &reinterpret_cast<RAWARRAY*>(reinterpret_cast<M2Array<RAWARRAY>*>(part)->Data())[sequence];
    if (context->m_size != 0) {
        uint32_t offset = key->m_dataLow;
        if (context->m_size <= offset) {
            M2InitLog("M2InitKeyFrameData M2Array failure, offset():(%d) >= size:(%d)", offset);
            return false;
        }
        SetPointer(key, key->m_count == 0 ? 0 : context->m_base + offset);
        return true;
    }
    if (key->m_count != 0) {
        // The keys stay an offset, moved by the context's offset, with the pointer bits cleared.
        key->m_dataLow -= static_cast<uint32_t>(context->m_base);
        key->m_dataHigh = 0;
    }
    return true;
}

template <class T>
bool InitTrack(void* base, uint32_t size, M2Data* data, M2Track<T>* track, M2INITCONTEXT* context) {
    if (!InitKeyPart<4>(base, size, data, &track->m_timestamps, track->m_globalSequence, &track->m_timestamps.m_count, context)) {
        return false;
    }
    return InitKeyPart<sizeof(T)>(base, size, data, &track->m_values, track->m_globalSequence, &track->m_timestamps.m_count, context);
}

bool InitTrackBase(void* base, uint32_t size, M2Data* data, M2TrackBase* track, M2INITCONTEXT* context) {
    return InitKeyPart<4>(base, size, data, &track->m_timestamps, track->m_globalSequence, &track->m_timestamps.m_count, context);
}

template <class T, class F>
bool InitStructArray(void* base, uint32_t size, M2Data* data, M2Array<T>* array, M2INITCONTEXT* context, F each) {
    if (!InitArray<sizeof(T)>(base, size, data, array, context)) {
        return false;
    }
    for (uint32_t i = 0; i < array->m_count; i++) {
        if (!each(&array->Data()[i])) {
            return false;
        }
    }
    return true;
}

// ref: FUN_1411a3ce0
bool InitVertices(void* base, uint32_t size, M2Data* data, M2BigArray<M2Vertex>* array, M2INITCONTEXT* context) {
    (void)data;
    if (!FixesMain(context)) {
        return true;
    }
    uint32_t offset = static_cast<uint32_t>(array->m_offset);
    if (size < offset) {
        M2InitLog("M2Init M2BigArray failure, offset():(%d) > size:(%d)", offset, size);
        return false;
    }
    uint32_t bytes = array->m_count * sizeof(M2Vertex);
    if (size < bytes + offset) {
        M2InitLog("M2Init M2BigArray failure, offset()+bytes():(%d + %d) > size:(%d)", offset, bytes, size);
        return false;
    }
    if (array->m_count != 0) {
        array->m_offset = static_cast<int32_t>(offset - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(array))
            + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)));
    }
    return true;
}

// ref: FUN_1411a2b70
bool InitTextures(void* base, uint32_t size, M2Data* data, M2Array<M2Texture>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Texture* texture) {
        return InitArray<1>(base, size, data, &texture->m_filename, context);
    });
}

// ref: FUN_14119bf20
bool InitBones(void* base, uint32_t size, M2Data* data, M2Array<M2CompBone>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2CompBone* bone) {
        return InitTrack(base, size, data, &bone->m_translation, context)
            && InitTrack(base, size, data, &bone->m_rotation, context)
            && InitTrack(base, size, data, &bone->m_scale, context);
    });
}

// ref: FUN_14119b6a0
bool InitColors(void* base, uint32_t size, M2Data* data, M2Array<M2Color>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Color* color) {
        return InitTrack(base, size, data, &color->m_color, context) && InitTrack(base, size, data, &color->m_alpha, context);
    });
}

// ref: FUN_1411a37f0
bool InitTextureWeights(void* base, uint32_t size, M2Data* data, M2Array<M2TextureWeight>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2TextureWeight* weight) {
        return InitTrack(base, size, data, &weight->m_weight, context);
    });
}

// ref: FUN_1411a2c80
bool InitTextureTransforms(void* base, uint32_t size, M2Data* data, M2Array<M2TextureTransform>* array,
    M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2TextureTransform* transform) {
        return InitTrack(base, size, data, &transform->m_translation, context)
            && InitTrack(base, size, data, &transform->m_rotation, context)
            && InitTrack(base, size, data, &transform->m_scale, context);
    });
}

// ref: FUN_14119a2d0
bool InitAttachments(void* base, uint32_t size, M2Data* data, M2Array<M2Attachment>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Attachment* attachment) {
        return InitTrack(base, size, data, &attachment->m_animate, context);
    });
}

// ref: FUN_14119cb50
bool InitEvents(void* base, uint32_t size, M2Data* data, M2Array<M2Event>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Event* event) {
        return InitTrackBase(base, size, data, &event->m_enabled, context);
    });
}

// ref: FUN_14119cff0
bool InitLights(void* base, uint32_t size, M2Data* data, M2Array<M2Light>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Light* light) {
        return InitTrack(base, size, data, &light->m_ambientColor, context)
            && InitTrack(base, size, data, &light->m_ambientIntensity, context)
            && InitTrack(base, size, data, &light->m_diffuseColor, context)
            && InitTrack(base, size, data, &light->m_diffuseIntensity, context)
            && InitTrack(base, size, data, &light->m_attenuationStart, context)
            && InitTrack(base, size, data, &light->m_attenuationEnd, context)
            && InitTrack(base, size, data, &light->m_visibility, context);
    });
}

// ref: FUN_14119a7c0
bool InitCameras(void* base, uint32_t size, M2Data* data, M2Array<M2Camera>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Camera* camera) {
        return InitTrack(base, size, data, &camera->m_positions, context)
            && InitTrack(base, size, data, &camera->m_targetPositions, context)
            && InitTrack(base, size, data, &camera->m_roll, context)
            && InitTrack(base, size, data, &camera->m_fieldOfView, context);
    });
}

// ref: FUN_1411a1440
bool InitRibbons(void* base, uint32_t size, M2Data* data, M2Array<M2Ribbon>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Ribbon* ribbon) {
        return InitArray<2>(base, size, data, &ribbon->m_textureIndices, context)
            && InitArray<2>(base, size, data, &ribbon->m_materialIndices, context)
            && InitTrack(base, size, data, &ribbon->m_color, context)
            && InitTrack(base, size, data, &ribbon->m_alpha, context)
            && InitTrack(base, size, data, &ribbon->m_heightAbove, context)
            && InitTrack(base, size, data, &ribbon->m_heightBelow, context)
            && InitTrack(base, size, data, &ribbon->m_textureSlot, context)
            && InitTrack(base, size, data, &ribbon->m_visibility, context);
    });
}

template <class T>
bool InitPartTrack(void* base, uint32_t size, M2Data* data, M2PartTrack<T>* track, M2INITCONTEXT* context) {
    return InitArray<2>(base, size, data, &track->m_times, context)
        && InitArray<sizeof(T)>(base, size, data, &track->m_values, context);
}

// ref: FUN_14119e960
bool InitParticles(void* base, uint32_t size, M2Data* data, M2Array<M2Particle>* array, M2INITCONTEXT* context) {
    return InitStructArray(base, size, data, array, context, [&](M2Particle* particle) {
        return InitArray<1>(base, size, data, &particle->m_geometryModelFilename, context)
            && InitArray<1>(base, size, data, &particle->m_recursionModelFilename, context)
            && InitTrack(base, size, data, &particle->m_emissionSpeed, context)
            && InitTrack(base, size, data, &particle->m_speedVariation, context)
            && InitTrack(base, size, data, &particle->m_verticalRange, context)
            && InitTrack(base, size, data, &particle->m_horizontalRange, context)
            && InitTrack(base, size, data, &particle->m_gravity, context)
            && InitTrack(base, size, data, &particle->m_lifespan, context)
            && InitTrack(base, size, data, &particle->m_emissionRate, context)
            && InitTrack(base, size, data, &particle->m_emissionAreaLength, context)
            && InitTrack(base, size, data, &particle->m_emissionAreaWidth, context)
            && InitTrack(base, size, data, &particle->m_zSource, context)
            && InitPartTrack(base, size, data, &particle->m_colorTrack, context)
            && InitPartTrack(base, size, data, &particle->m_alphaTrack, context)
            && InitPartTrack(base, size, data, &particle->m_scaleTrack, context)
            && InitPartTrack(base, size, data, &particle->m_headCellTrack, context)
            && InitPartTrack(base, size, data, &particle->m_tailCellTrack, context)
            && InitArray<sizeof(C3Vector)>(base, size, data, &particle->m_splinePoints, context)
            && InitTrack(base, size, data, &particle->m_enabledIn, context);
    });
}

}

void M2InitLog(const char* format, ...) {
    if (g_m2Log == nullptr) {
        return;
    }
    va_list args;
    va_start(args, format);
    SLogWriteV(g_m2Log, format, args);
    va_end(args);
}

// ref: FUN_1411ab3a0
bool M2InitAll(void* base, uint32_t size, M2Data* data, M2INITCONTEXT* context) {
    if (reinterpret_cast<uintptr_t>(data) < reinterpret_cast<uintptr_t>(base)) {
        M2InitLog("M2InitAll failure: data(%p) < base(%p)", data, base);
        return false;
    }
    if (reinterpret_cast<uintptr_t>(base) + size < reinterpret_cast<uintptr_t>(data + 1)) {
        M2InitLog("M2InitAll failure: data(%p) + sizeof(M2Data2) > base(%p) + size(%d)", data + 1, base, size);
        return false;
    }
    if (data->m_magic != 0x3032444d || data->m_version < 0x110 || 0x112 < data->m_version) {
        return false;
    }
    if (!InitArray<1>(base, size, data, &data->m_name, context)) {
        return false;
    }
#define M2INIT_STEP(call, what) \
    if (!(call)) { \
        M2InitLog("M2Init failure: " what); \
        return false; \
    }
    M2INIT_STEP(InitArray<4>(base, size, data, &data->m_loops, context), "M2Loop");
    M2INIT_STEP(InitArray<sizeof(M2Sequence)>(base, size, data, &data->m_sequences, context), "M2Sequence");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_sequenceIdxHashById, context), "sequenceIdxHashById");
    M2INIT_STEP(InitBones(base, size, data, &data->m_bones, context), "M2CompBone");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_boneIndicesById, context), "boneIndicesById");
    M2INIT_STEP(InitVertices(base, size, data, &data->m_vertices, context), "M2Vertex");
    M2INIT_STEP(InitColors(base, size, data, &data->m_colors, context), "M2Color");
    M2INIT_STEP(InitTextures(base, size, data, &data->m_textures, context), "M2Texture");
    M2INIT_STEP(InitTextureWeights(base, size, data, &data->m_textureWeights, context), "M2TextureWeight");
    M2INIT_STEP(InitTextureTransforms(base, size, data, &data->m_textureTransforms, context), "M2TextureTransform");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_textureIndicesById, context), "textureIndicesById");
    M2INIT_STEP(InitArray<4>(base, size, data, &data->m_materials, context), "M2Material");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_boneCombos, context), "boneCombos");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_textureCombos, context), "textureCombos");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_textureTransformBoneMap, context), "textureTransformBoneMap");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_textureWeightCombos, context), "textureWeightCombos");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_textureTransformCombos, context), "textureTransformCombos");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_collisionIndices, context), "collisionIndices");
    M2INIT_STEP(InitArray<12>(base, size, data, &data->m_collisionPositions, context), "collisionPositions");
    M2INIT_STEP(InitArray<12>(base, size, data, &data->m_collisionFaceNormals, context), "collisionFaceNormals");
    M2INIT_STEP(InitAttachments(base, size, data, &data->m_attachments, context), "M2Attachment");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_attachmentIndicesById, context), "attachmentIndicesById");
    M2INIT_STEP(InitEvents(base, size, data, &data->m_events, context), "M2Event");
    M2INIT_STEP(InitLights(base, size, data, &data->m_lights, context), "M2Light");
    M2INIT_STEP(InitCameras(base, size, data, &data->m_cameras, context), "M2Camera");
    M2INIT_STEP(InitArray<2>(base, size, data, &data->m_cameraIndicesById, context), "cameraIndicesById");
    M2INIT_STEP(InitRibbons(base, size, data, &data->m_ribbons, context), "M2Ribbon");
    M2INIT_STEP(InitParticles(base, size, data, &data->m_particles, context), "M2Particle");
#undef M2INIT_STEP
    return true;
}

// The parent's keys (FUN_141199bd0 on the chunk's own array).
bool M2InitKeys32(void* base, uint32_t size, M2Data* data, M2Array<M2Array<uint32_t>>* keys, M2INITCONTEXT* context) {
    return InitArrayOfArrays<4>(base, size, data, keys, context);
}

// An array a chunk starts with, against the chunk (the PGD1, PABC and PSBC chunks).
bool M2InitChunkArray(void* base, uint32_t size, void* field, uint32_t elemSize, const char* what) {
    auto array = static_cast<RAWARRAY*>(field);
    uint32_t offset = array->m_dataLow;
    if (size < offset) {
        M2InitLog("M2Init M2Array failure, offset():(%d) > size:(%d)", offset, size);
        M2InitLog("%s", what);
        return false;
    }
    uint32_t bytes = static_cast<uint32_t>(array->m_count) * elemSize;
    if (size < bytes + offset) {
        M2InitLog("M2Init M2Array failure, offset()+bytes():(%d + %d) > size:(%d)", offset, bytes, size);
        M2InitLog("%s", what);
        return false;
    }
    SetPointer(array, array->m_count == 0 ? 0 : Address(base, offset));
    return true;
}

// The parent's texture weight tracks (PADC) and event tracks (PEDC): every sequence's keys are
// in the chunk.
bool M2InitParentAnimData(void* base, uint32_t size, M2Data* data, M2Array<M2Track<uint16_t>>* array) {
    M2INITCONTEXT context;
    context.m_allSequences = true;
    return InitStructArray(base, size, data, array, &context, [&](M2Track<uint16_t>* track) {
        return InitTrack(base, size, data, track, &context);
    });
}

bool M2InitParentEventData(void* base, uint32_t size, M2Data* data, M2Array<M2TrackBase>* array) {
    M2INITCONTEXT context;
    context.m_allSequences = true;
    return InitStructArray(base, size, data, array, &context, [&](M2TrackBase* track) {
        return InitTrackBase(base, size, data, track, &context);
    });
}

// ref: FUN_14119ce60
bool M2InitExtendedParticles(void* base, uint32_t size, M2Data* data, M2Array<M2ExtendedParticle>* array,
    M2INITCONTEXT* context) {
    if (!InitArray<sizeof(M2ExtendedParticle)>(base, size, data, array, context)) {
        return false;
    }
    for (uint32_t i = 0; i < array->m_count; i++) {
        M2ExtendedParticle* particle = &array->Data()[i];
        if (reinterpret_cast<uintptr_t>(base) + size < reinterpret_cast<uintptr_t>(particle)) {
            M2InitLog("M2Init M2ExtendedParticle failure: particle(%p) > base(%p) + size(%d)", particle, base, size);
            return false;
        }
        if (!InitArray<2>(base, size, data, &particle->m_alphaCutoffTimes, context)
            || !InitArray<2>(base, size, data, &particle->m_alphaCutoffValues, context)) {
            return false;
        }
    }
    return true;
}

// ref: FUN_1411aba40
bool M2InitSkinProfile(void* base, uint32_t size, M2Data* data, M2SkinProfile* profile) {
    (void)data;
    auto initBig = [&](void* field, uint32_t elemSize) {
        auto array = static_cast<M2BigArray<uint8_t>*>(field);
        uint32_t offset = static_cast<uint32_t>(array->m_offset);
        if (size < offset) {
            M2InitLog("M2Init M2BigArray failure, offset():(%d) > size:(%d)", offset, size);
            return false;
        }
        uint32_t bytes = array->m_count * elemSize;
        if (size < bytes + offset) {
            M2InitLog("M2Init M2BigArray failure, offset()+bytes():(%d + %d) > size:(%d)", offset, bytes, size);
            return false;
        }
        if (array->m_count != 0) {
            array->m_offset = static_cast<int32_t>(offset - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(array))
                + static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)));
        }
        return true;
    };
    auto initSmall = [&](void* field, uint32_t elemSize) {
        auto array = static_cast<RAWARRAY*>(field);
        uint32_t offset = array->m_dataLow;
        if (size < offset) {
            M2InitLog("M2Init M2Array failure, offset():(%d) > size:(%d)", offset, size);
            return false;
        }
        uint32_t bytes = static_cast<uint32_t>(array->m_count) * elemSize;
        if (size < offset + bytes) {
            M2InitLog("M2Init M2Array failure, offset()+bytes():(%d + %d) > size:(%d)", offset, bytes, size);
            return false;
        }
        SetPointer(array, array->m_count == 0 ? 0 : Address(base, offset));
        return true;
    };
    return initBig(&profile->m_vertices, 2) && initBig(&profile->m_indices, 2) && initBig(&profile->m_bones, 4)
        && initSmall(&profile->m_skinSections, sizeof(M2SkinSection)) && initSmall(&profile->m_batches, sizeof(M2Batch))
        && initSmall(&profile->m_shadowBatches, sizeof(M2ShadowBatch));
}
