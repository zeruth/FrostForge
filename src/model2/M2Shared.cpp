#include "model2/M2Shared.hpp"

#include <intrin.h>

#include <cstring>
#include <new>

#include "base/Status.hpp"
#include "cas/CAS.hpp"
#include "fileservices/AsyncFile.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxPlatform.hpp"
#include "gx/GxShader.hpp"
#include "gx/GxVertexFormat.hpp"
#include "model2/M2Init.hpp"
#include "model2/M2Log.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Shaders.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"

namespace {

const char s_sharedFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Model2\\M2Shared.cpp";
const char s_skinProfileLoadTag[] = ".?AV?$TSList@UCM2SkinProfileLoad@@V?$TSGetLink@UCM2SkinProfileLoad@@@@$07@@";
const char s_boneOffsetLoadTag[] = ".?AV?$TSList@UCM2BoneOffsetLoad@@V?$TSGetLink@UCM2BoneOffsetLoad@@@@$07@@";
const char s_sequenceLoadTag[] = ".?AV?$TSList@UCM2SequenceLoad@@V?$TSGetLink@UCM2SequenceLoad@@@@$07@@";
const char s_playbackTag[] = ".?AV?$TSList@UCM2SequencePlayBack@@V?$TSGetLink@UCM2SequencePlayBack@@@@$07@@";
const char s_keyframeDataTag[] = ".?AV?$TSList@UCM2KeyframeData@@V?$TSGetLink@UCM2KeyframeData@@@@$07@@";

// The bounds a sequence-less model takes (DAT_14261f708, DAT_14261f70c).
constexpr float BOUNDS_EMPTY_MIN = 3.4028235e38f;
constexpr float BOUNDS_EMPTY_MAX = -3.4028235e38f;

// The skin profile counts the models carry and the bone limit of each (DAT_1427208f0): a model
// with N profiles uses entries 4 - N .. 3.
const uint32_t s_skinProfileBones[] = { 0x100, 0x40, 0x35, 0x15 };

// A name into a 0x104 buffer, cut at 0x103 characters.
void CopyName(char* dest, const char* source) {
    for (int64_t left = 0x103;;) {
        char c = *source++;
        *dest++ = c;
        if (c == '\0') {
            return;
        }
        if (--left == 0) {
            *dest = '\0';
            return;
        }
    }
}

// The memory of the files read (FUN_1412be640, FUN_1412be750): 16-byte aligned, the alignment
// shift kept in the byte before.
void* AlignedAlloc(uint32_t size, const char* file, int32_t line, int32_t flags, uint32_t extra) {
    if (flags == 0) {
        flags = 1;
    }
    auto memory = static_cast<uint8_t*>(SMemAlloc(size + 0x10, flags, file, 0, line, extra));
    if (memory == nullptr) {
        return nullptr;
    }
    uint64_t shift = 0x10 - (reinterpret_cast<uintptr_t>(memory) & 0xf);
    memory[shift - 1] = static_cast<uint8_t>(shift);
    return memory + shift;
}

}

// ref: FUN_1412be750
void M2FreeFileData(void* data) {
    if (data != nullptr) {
        auto memory = static_cast<uint8_t*>(data);
        SMemFree(memory - memory[-1], nullptr, 0, 0);
    }
}

namespace {

// The async reads' buffers (FUN_1411a4f00 for the model, FUN_1411a4f30 for bone files).
// ref: FUN_1411a4f00
void* AllocModelBuffer(uint32_t size) {
    return AlignedAlloc(size, s_sharedFile, 0x489, 0x12, 0);
}

// ref: FUN_1411a4f30
void* AllocBoneBuffer(uint32_t size) {
    return AlignedAlloc(size, s_sharedFile, 0x822, 0x12, 0);
}

// The read's error: the reason it failed, taken from the CAS for the next model error
// (DAT_1430a0f24).
uint32_t s_lastReadError;

void RecordReadError(CAsyncObject* object) {
    if (object == nullptr) {
        return;
    }
    // FUN_1402cdb50: the error's category
    int32_t category;
    int32_t kind = object->m_errorKind;
    uint32_t code = object->m_errorCode;
    if (kind == 0) {
        category = code == 0 ? 0 : (code == 2 ? 6 : (code == 0xd ? 7 : 1));
    } else if (kind == 1 || kind == 3) {
        category = static_cast<int32_t>(code);
    } else if (kind == 2) {
        category = code == 0 ? 0 : 9;
    } else {
        category = 1;
    }
    if (category != 2 && category != 7) {
        s_lastReadError = static_cast<uint32_t>(object->m_fileDataId);
    }
}

// The open procs: the file's keys must be known before it is opened.
// ref: FUN_1411ac4f0
bool OpenModelById(CAsyncObject* object, cas::CASFILE** file) {
    blz::function_storage check = {};
    check.m_manager = blz::function_empty_manager;
    bool opened = static_cast<uint8_t>(cas::CASFileCheckKeysById(object->m_fileDataId, &check, 0, 0)) != 0
        && cas::CASFileOpenById(object->m_fileDataId, file, 0);
    if (opened) {
        return true;
    }
    object->m_errorCode = cas::CASGetErrorCode(&object->m_errorKind);
    return false;
}

// ref: FUN_1411ac5a0
bool OpenModelByName(CAsyncObject* object, cas::CASFILE** file) {
    blz::function_storage check = {};
    check.m_manager = blz::function_empty_manager;
    uint32_t flags = (object->m_flags >> 2) & 0x20;
    bool opened = static_cast<uint8_t>(cas::CASFileCheckKeysByName(object->m_name, &check, 0, 0, flags)) != 0
        && cas::CASFileOpenByName(object->m_name, file, flags);
    if (!opened) {
        object->m_errorCode = cas::CASGetErrorCode(&object->m_errorKind);
    }
    return opened;
}

// ref: FUN_1411ac900
bool OpenSkinProfile(CAsyncObject* object, cas::CASFILE** file) {
    return cas::CASFileOpenById(object->m_fileDataId, file, 0);
}

// ref: FUN_1411a5010
bool OpenBoneFile(CAsyncObject* object, cas::CASFILE** file) {
    if (object->m_name[0] != '\0') {
        return cas::CASFileOpenByName(object->m_name, file, 0);
    }
    return cas::CASFileOpenById(object->m_fileDataId, file, 0);
}

// The model's file is in (FUN_1411ac660).
// ref: FUN_1411ac660
void OnModelRead(void* userData, void* buffer, uint32_t size) {
    auto shared = static_cast<CM2Shared*>(userData);
    shared->m_asyncObject = nullptr;
    if (shared->ParseChunks(static_cast<uint32_t*>(buffer), size)) {
        shared->LoadBoneFiles();
        return;
    }
    M2SetErrorLocation(s_sharedFile, 0x4a0);
    M2SetError("Corrupted model: %s", shared->GetDisplayName());
    shared->m_flags |= CM2Shared::SHARED_ERROR;
}

// ref: FUN_1411ac410
void OnModelError(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)size;
    (void)error;
    auto shared = static_cast<CM2Shared*>(userData);
    M2FreeFileData(buffer);
    char name[0x110];
    if (shared->m_name[0] == '\0') {
        GxShaderName(shared->m_fileDataId, name, 0x104);
    } else {
        CopyName(name, shared->m_name);
    }
    RecordReadError(shared->m_asyncObject);
    shared->m_flags |= CM2Shared::SHARED_FILE_FREED | CM2Shared::SHARED_ERROR;
    shared->m_asyncObject = nullptr;
    shared->ClearChunkData();
    // FUN_1411fa590: the procedural (sequence-less) models made of it
    M2SharedProceduralRelease(shared);
}

// The read was cancelled (LAB_1411a4f60): the buffer goes.
void OnReadCancelled(void* userData, void* buffer, uint32_t size) {
    (void)userData;
    (void)size;
    M2FreeFileData(buffer);
}

void UnlinkSkinLoad(CM2SkinProfileLoad* load) {
    load->m_link.Unlink();
    SMemFree(load, s_skinProfileLoadTag, -2, 0);
}

void UnlinkBoneLoad(CM2BoneOffsetLoad* load) {
    load->m_link.Unlink();
    SMemFree(load, s_boneOffsetLoadTag, -2, 0);
}

// A skin profile is in (FUN_1411ac920).
// ref: FUN_1411ac920
void OnSkinRead(void* userData, void* buffer, uint32_t size) {
    auto load = static_cast<CM2SkinProfileLoad*>(userData);
    CM2Shared* shared = load->m_shared;
    uint32_t slot = load->m_index;
    load->m_asyncObject = nullptr;
    UnlinkSkinLoad(load);
    auto profile = static_cast<M2SkinProfile*>(buffer);
    shared->m_skins[slot].m_profile = profile;
    if (!M2InitSkinProfile(buffer, size, shared->m_data, profile)) {
        M2SetErrorLocation(s_sharedFile, 0x7c9);
        M2SetError("Corrupt skin profile data: %s", shared->GetDisplayName());
        shared->m_flags |= CM2Shared::SHARED_ERROR;
        return;
    }
    if (!shared->InitializeSkinProfile(slot)) {
        M2SetErrorLocation(s_sharedFile, 2000);
        M2SetError("Failed to initialize model skin profile: %s", shared->GetDisplayName());
        shared->m_flags |= CM2Shared::SHARED_ERROR;
        return;
    }
    shared->m_skinsLoaded |= static_cast<uint8_t>(1u << (slot & 0x1f));
    if (shared->m_skinProfileLoads.Head() == nullptr && (shared->m_flags & CM2Shared::SHARED_LOADED)) {
        shared->OnFullyLoaded();
    }
}

// A skin profile could not be read (FUN_1411ac6d0).
// ref: FUN_1411ac6d0
void OnSkinError(void* userData, void* buffer, uint32_t size, const char* reason) {
    (void)size;
    auto load = static_cast<CM2SkinProfileLoad*>(userData);
    CM2Shared* shared = load->m_shared;
    char name[0x110];
    if (shared->m_name[0] == '\0') {
        GxShaderName(shared->m_fileDataId, name, 0x104);
    } else {
        CopyName(name, shared->m_name);
    }
    if (!(shared->m_flags & CM2Shared::SHARED_FILE_FREED)) {
        RecordReadError(shared->m_asyncObject);
    }
    char message[0x400];
    if (load->m_asyncObject == nullptr) {
        SStrPrintf(message, sizeof(message), "SkipProfileFailCallback() SkinProfileIndex : %d, File : %s, Reason : %s",
            load->m_index, name, reason);
    } else {
        SStrPrintf(message, sizeof(message),
            "SkipProfileFailCallback() SkinProfileIndex : %d, FailCodeType : %d, FailCode : %d, File : %s, Reason : %s",
            load->m_index, load->m_asyncObject->m_errorKind, load->m_asyncObject->m_errorCode, name, reason);
    }
    // FUN_141197f60
    M2InitLog("%s", message);
    load->m_asyncObject = nullptr;
    shared->m_flags |= CM2Shared::SHARED_ERROR;
    UnlinkSkinLoad(load);
    if (shared->m_skeleton != nullptr) {
        M2SkeletonRelease(shared->m_skeleton);
        shared->m_skeleton = nullptr;
    }
    if (!(shared->m_flags & CM2Shared::SHARED_FILE_FREED)) {
        shared->CancelSequenceLoads();
        shared->CancelLoads();
        shared->ReleaseTextures();
        shared->ReleaseBoneData();
        shared->m_flags |= CM2Shared::SHARED_FILE_FREED | CM2Shared::SHARED_ERROR;
        shared->m_lodMax = 0;
        if (shared->m_fileData != nullptr) {
            M2FreeFileData(shared->m_fileData);
            shared->m_fileData = nullptr;
            shared->m_data = nullptr;
            shared->m_dataSize = 0;
        }
        shared->ClearChunkData();
        M2SharedProceduralRelease(shared);
    }
    shared->m_flags |= CM2Shared::SHARED_LOADED;
    M2FreeFileData(buffer);
}

// A bone file is in (FUN_1411a5050): the BIDA chunk maps the model's bones, BOMT holds the
// offsets.
// ref: FUN_1411a5050
void OnBoneRead(void* userData, void* buffer, uint32_t size) {
    auto load = static_cast<CM2BoneOffsetLoad*>(userData);
    CM2Shared* shared = load->m_shared;
    if (buffer != nullptr && size != 0) {
        CM2BoneOffset* offset = &shared->m_boneOffsets[load->m_index];
        if (offset->m_data != nullptr) {
            M2FreeFileData(offset->m_data);
        }
        offset->m_data = buffer;
        auto end = static_cast<uint8_t*>(buffer) + size;
        for (auto chunk = static_cast<uint8_t*>(buffer) + 4; chunk < end;) {
            uint32_t id = *reinterpret_cast<uint32_t*>(chunk);
            uint32_t chunkSize = *reinterpret_cast<uint32_t*>(chunk + 4);
            auto data = reinterpret_cast<uint16_t*>(chunk + 8);
            if (id == 0x41444942) {
                std::memset(offset->m_boneIndices, 0xff, static_cast<size_t>(shared->m_data->m_bones.m_count) * 2);
                for (uint32_t i = 0; i < chunkSize >> 1; i++) {
                    offset->m_boneIndices[data[i]] = static_cast<uint16_t>(i);
                }
            } else if (id == 0x544d4f42) {
                offset->m_offsets = data;
            }
            chunk += chunkSize + 8;
        }
    }
    shared->m_boneOffsets[load->m_index].m_flags &= ~1u;
    shared->m_boneOffsets[load->m_index].m_flags |= 2;
    load->m_asyncObject = nullptr;
    UnlinkBoneLoad(load);
    if (shared->m_skinProfileLoads.Head() == nullptr && (shared->m_flags & CM2Shared::SHARED_LOADED)) {
        shared->OnFullyLoaded();
    }
}

// ref: FUN_1411a4f70
void OnBoneError(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)size;
    (void)error;
    auto load = static_cast<CM2BoneOffsetLoad*>(userData);
    CM2Shared* shared = load->m_shared;
    M2FreeFileData(buffer);
    shared->m_boneOffsets[load->m_index].m_flags &= ~1u;
    load->m_asyncObject = nullptr;
    UnlinkBoneLoad(load);
}

// The upload of a skin profile's indices: each section's run of the profile's index list
// (FUN_141199530).
struct BUFFERUPLOAD {
    CM2Shared* m_shared;
    CM2SkinProfileData* m_skin;
};

// ref: FUN_141199530
void WriteIndices(void* param, void* data) {
    auto upload = static_cast<BUFFERUPLOAD*>(param);
    M2SkinProfile* profile = upload->m_skin->m_profile;
    M2SkinSection* sections = upload->m_skin->m_sections;
    auto out = static_cast<uint16_t*>(data);
    for (uint32_t i = 0; i < profile->m_skinSections.m_count; i++) {
        const M2SkinSection* section = &sections[i];
        uint32_t start = section->IndexStart();
        uint32_t count = section->IndexCount();
        for (uint32_t k = 0; k < count; k++) {
            const uint16_t* indices = profile->m_indices.m_count != 0 ? profile->m_indices.Data() : nullptr;
            out[k] = indices[start + k];
        }
        out += count;
    }
}

// The upload of a skin profile's vertices: the model's vertices each section uses, in order,
// with the profile's bone indices (FUN_141199630).
// ref: FUN_141199630
void WriteVertices(void* param, void* data) {
    auto upload = static_cast<BUFFERUPLOAD*>(param);
    M2SkinProfile* profile = upload->m_skin->m_profile;
    M2Data* model = upload->m_shared->m_data;
    uint32_t base = (model->m_flags & 0x2000) != 0 ? profile->m_globalVertexOffset : 0;
    const M2Vertex* vertices = model->m_vertices.m_count != 0 ? model->m_vertices.Data() + base : nullptr;
    auto out = static_cast<M2Vertex*>(data);
    for (uint32_t i = 0; i < profile->m_skinSections.m_count; i++) {
        const M2SkinSection* section = &profile->m_skinSections[i];
        for (uint32_t v = section->m_vertexStart; v < static_cast<uint32_t>(section->m_vertexStart) + section->m_vertexCount; v++) {
            const uint16_t* remap = profile->m_vertices.m_count != 0 ? profile->m_vertices.Data() : nullptr;
            out[v] = vertices[remap[v]];
            const uint32_t* bones = profile->m_bones.m_count != 0 ? profile->m_bones.Data() : nullptr;
            std::memcpy(out[v].m_boneIndices, &bones[v], 4);
        }
    }
}

}

// ---- construction ----------------------------------------------------------------------------

// ref: FUN_1411a3e50
CM2Shared::CM2Shared(M2CACHE* cache) {
    this->m_flags &= 0xffe00000;
    this->m_cache = cache;
    this->m_refCount = 0;
    this->m_useCount = 0;
    this->m_priority = 0;
    this->m_skinsLoaded = 0;
    this->m_center = {};
    this->m_pad3c = 0;
    this->m_lodFlags = 0;
    this->m_lodCount = 0;
    this->m_lodMax = 0;
    this->m_lodDistance = 0.0f;
    std::memset(this->m_lodLevels, 0, sizeof(this->m_lodLevels));
    this->m_lodScale = 1.0f;
    this->m_boundsRadius = 0.0f;
    this->m_cacheLink.m_prevlink = nullptr;
    this->m_cacheLink.m_next = nullptr;
    this->m_releaseFrame = 0;
    this->m_fileDataId = 0;
    this->m_asyncObject = nullptr;
    this->m_waitingModels = nullptr;
    this->m_waitingTail = &this->m_waitingModels;
    this->m_178 = nullptr;
    this->m_180 = nullptr;
    this->m_fileData = nullptr;
    this->m_fileSize = 0;
    this->m_data = nullptr;
    this->m_dataSize = 0;
    this->m_bounds.m_min = { BOUNDS_EMPTY_MIN, BOUNDS_EMPTY_MIN, BOUNDS_EMPTY_MIN };
    this->m_bounds.m_max = { BOUNDS_EMPTY_MAX, BOUNDS_EMPTY_MAX, BOUNDS_EMPTY_MAX };
    this->ClearChunkData();
    std::memset(this->m_skins, 0, sizeof(this->m_skins));
    for (CM2SkinProfileData& skin : this->m_skins) {
        skin.m_rtGeometry = -1;
    }
    this->m_block = nullptr;
    this->m_textures = nullptr;
    this->m_boneOrder = nullptr;
    this->m_sequenceState = nullptr;
    this->m_550 = 0;
    this->m_558 = nullptr;
    this->m_boneFileCount = 0;
    this->m_boneOffsets = nullptr;
    this->m_keyframeLink.m_prevlink = nullptr;
    this->m_keyframeLink.m_next = nullptr;
    this->m_particleData = nullptr;
    std::memset(this->m_tint, 0, sizeof(this->m_tint));
    this->m_name[0] = '\0';
}

// ref: FUN_1411a4460
CM2Shared::~CM2Shared() {
    this->CancelSequenceLoads();
    this->CancelLoads();
    int32_t fileDataId = this->m_fileDataId;
    if (this->m_cache != nullptr
        && (0 < fileDataId || (fileDataId = cas::CASFileGetIdByName(this->m_name, (this->m_flags >> 13) & 0x20)) != 0)) {
        M2CacheRemoveShared(this->m_cache, fileDataId);
    }
    if (this->m_particleData != nullptr) {
        SMemFree(this->m_particleData, s_sharedFile, 0x1b9, 0);
        this->m_particleData = nullptr;
    }
    if (this->m_558 != nullptr) {
        SMemFree(this->m_558, nullptr, 0, 0);
        this->m_558 = nullptr;
        this->m_550 = 0;
    }
    this->ReleaseTextures();
    this->ReleaseBoneData();
    while (CM2KeyframeData* keyframes = this->m_keyframeData.Head()) {
        M2FreeFileData(keyframes->m_data);
        keyframes->m_link.Unlink();
        SMemFree(keyframes, s_keyframeDataTag, -2, 0);
    }
    for (CM2SkinProfileData& skin : this->m_skins) {
        M2SkinProfileRelease(&skin);
        if (skin.m_effects != nullptr) {
            SMemFree(skin.m_effects, s_sharedFile, 0x9d, 0);
            skin.m_effects = nullptr;
        }
        if (skin.m_profile != nullptr) {
            M2FreeFileData(skin.m_profile);
            skin.m_profile = nullptr;
            skin.m_sections = nullptr;
        }
    }
    if (this->m_physics != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_14125f490: the physics definition
        this->m_physics = nullptr;
    }
    if (this->m_fileData != nullptr) {
        M2FreeFileData(this->m_fileData);
        this->m_fileData = nullptr;
    }
    this->m_data = nullptr;
    if (this->m_skeleton != nullptr) {
        M2SkeletonRelease(this->m_skeleton);
        this->m_skeleton = nullptr;
    }
    this->m_keyframeLink.Unlink();
    this->m_cacheLink.Unlink();
}

// ref: FUN_1411a4e70
int32_t CM2Shared::AddRef() {
    M2CacheLock();
    int32_t count = this->m_refCount;
    if (count == 0 && this->m_cache != nullptr) {
        // Back in use: off the cache's idle list.
        this->m_cacheLink.Unlink();
        this->m_cacheLink.m_prevlink = nullptr;
        this->m_cacheLink.m_next = nullptr;
        this->m_releaseFrame = 0;
        count = this->m_refCount;
    }
    this->m_refCount = count + 1;
    M2CacheUnlock();
    return this->m_refCount;
}

// ref: FUN_1411a4ef0
void CM2Shared::AddUse() {
    this->m_useCount++;
}

// ref: FUN_1411a4df0
int32_t CM2Shared::AddModelRef() {
    this->m_useCount++;
    M2CacheLock();
    int32_t count = this->m_refCount;
    if (count == 0 && this->m_cache != nullptr) {
        if (this->m_cacheLink.IsLinked()) {
            this->m_cacheLink.Unlink();
            this->m_cacheLink.m_prevlink = nullptr;
            this->m_cacheLink.m_next = nullptr;
            count = this->m_refCount;
        }
        this->m_releaseFrame = 0;
    }
    this->m_refCount = count + 1;
    M2CacheUnlock();
    return this->m_refCount;
}

// The last model's reference puts the file on the cache's idle list, stamped with the frame,
// or frees it when it has no cache.
// ref: FUN_1411abed0
int32_t CM2Shared::ReleaseModelRef() {
    this->m_useCount--;
    M2CacheLock();
    this->m_refCount--;
    if (this->m_refCount == 0) {
        M2CACHE* cache = this->m_cache;
        if (cache == nullptr) {
            M2SharedFree(this);
            M2CacheUnlock();
            return 0;
        }
        this->m_releaseFrame = cache->m_frame;
        if (this->m_cacheLink.IsLinked()) {
            this->m_cacheLink.Unlink();
            this->m_cacheLink.m_prevlink = nullptr;
            this->m_cacheLink.m_next = nullptr;
        }
        cache->m_idleShared.LinkToTail(this);
    }
    M2CacheUnlock();
    return this->m_refCount;
}

// ---- reading --------------------------------------------------------------------------------

namespace {

uint32_t LoadFlags(uint32_t flags, uint32_t current) {
    uint32_t value = ((current ^ flags) & 2) ^ current;
    value = ((flags >> 5 ^ value) & 4) ^ value;
    value = ((flags >> 1 ^ value) & 0x80) ^ value;
    value = ((flags * 2 ^ value) & 0x800) ^ value;
    value = (((flags & 4) << 0xf) ^ ((((flags * 4) ^ value) & 0x4000) ^ value) & 0xfffdffff) & 0xffffffdf;
    return value;
}

}

// ref: FUN_1411aa6f0
void CM2Shared::LoadById(int32_t fileDataId, uint32_t flags, uint8_t priority) {
    this->m_flags = LoadFlags(flags, this->m_flags);
    this->m_priority = priority;
    this->m_asyncObject = AsyncFileReadById(fileDataId, priority, this, reinterpret_cast<void*>(&OpenModelById),
        &AllocModelBuffer, nullptr, &OnModelRead, &OnModelError, &OnReadCancelled);
}

// ref: FUN_1411aa7c0
void CM2Shared::LoadByName(const char* name, uint32_t flags, uint8_t priority) {
    this->m_flags = LoadFlags(flags, this->m_flags);
    this->m_priority = priority;
    this->m_asyncObject = AsyncFileReadByName(name, priority, this, reinterpret_cast<void*>(&OpenModelByName),
        &AllocModelBuffer, nullptr, &OnModelRead, &OnModelError, &OnReadCancelled);
}

// ref: FUN_1411a6de0
bool CM2Shared::ParseChunks(uint32_t* data, uint32_t size) {
    uint32_t* end = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(data) + size);
    this->m_fileData = data;
    this->m_fileSize = size;
    uint32_t* chunk = data;
    while (chunk < end) {
        uint32_t id = chunk[0];
        uint32_t chunkSize = chunk[1];
        uint32_t* body = chunk + 2;
        auto next = [&]() {
            return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(body) + chunkSize);
        };
        M2INITCONTEXT context;
        switch (id) {
            case 0x3032444d:  // MD20: the whole file is the model
                if (!this->InitializeData(reinterpret_cast<M2Data*>(chunk), size)) {
                    return false;
                }
                chunk = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(chunk) + size);
                continue;
            case 0x3132444d:  // MD21
                if (!this->InitializeData(reinterpret_cast<M2Data*>(body), chunkSize)) {
                    return false;
                }
                break;
            case 0x3156444c: {  // LDV1
                auto bytes = reinterpret_cast<uint8_t*>(body);
                this->m_lodFlags = static_cast<int16_t>(*reinterpret_cast<uint16_t*>(body));
                uint8_t count = bytes[2];
                this->m_lodCount = count;
                uint8_t max;
                if (count < 9) {
                    max = count == 0 ? 0 : static_cast<uint8_t>(count - 1);
                } else {
                    this->m_lodCount = 8;
                    max = 7;
                }
                this->m_lodMax = max;
                this->m_lodDistance = *reinterpret_cast<float*>(body + 1);
                this->m_lodLevels[0] = bytes[8];
                this->m_lodLevels[1] = bytes[9];
                this->m_lodLevels[2] = bytes[10];
                this->m_lodLevels[3] = bytes[11];
                this->m_lodLevels[4] = bytes[11];
                this->m_lodLevels[5] = bytes[11];
                this->m_lodLevels[6] = bytes[11];
                this->m_lodLevels[7] = bytes[11];
                uint16_t flags = static_cast<uint16_t>(this->m_lodFlags);
                float scale = 1.0f;
                if (*body & 8) {
                    // DAT_142720d5c
                    scale = static_cast<float>(*reinterpret_cast<uint16_t*>(bytes + 0xc)) * (1.0f / 2048.0f);
                }
                this->m_boundsRadius = 0.0f;
                this->m_lodScale = scale;
                // DAT_142720d54
                if (1.17549435e-38f <= this->m_lodDistance) {
                    flags |= 4;
                }
                this->m_lodFlags = static_cast<int16_t>(flags | 0x8000);
                break;
            }
            case 0x31444750:  // PGD1
                this->m_particleGeosets = body;
                if (!M2InitChunkArray(body, chunkSize, body, 2, "M2InitParticleGeosetData failure: m_emitterSelectionGroup")) {
                    return false;
                }
                break;
            case 0x32505845:  // EXP2
                this->m_extendedParticles = reinterpret_cast<M2Array<M2ExtendedParticle>*>(body);
                if (!M2InitExtendedParticles(body, chunkSize, this->m_data, this->m_extendedParticles, &context)) {
                    return false;
                }
                if (this->m_data->m_particles.m_count != 0) {
                    this->m_flags |= SHARED_EXTENDED_PARTICLES;
                }
                break;
            case 0x33564657:  // WFV3
                this->m_waterfallData = body;
                break;
            case 0x41524641:  // AFRA
                this->m_afra = body;
                break;
            case 0x43415854: {  // TXAC
                uint16_t materials = static_cast<uint16_t>(this->m_data->m_materials.m_count);
                uint16_t particles = static_cast<uint16_t>(this->m_data->m_particles.m_count);
                if (chunkSize != static_cast<uint32_t>(particles + materials) * 2) {
                    return false;
                }
                this->m_textureCombiners = reinterpret_cast<uint16_t*>(body);
                if (particles != 0) {
                    this->m_particleCombiners = reinterpret_cast<uint16_t*>(body) + materials;
                }
                break;
            }
            case 0x43424150:  // PABC: the sequences blocked from the parent
                this->m_blocklistAnimData = body;
                if (!M2InitChunkArray(body, chunkSize, body, 2, "M2InitBlocklistAnimData failure: sequenceIds")) {
                    return false;
                }
                break;
            case 0x43425350:  // PSBC: the parent's sequence bounds
                this->m_parentSequenceBounds = body;
                if (!M2InitChunkArray(body, chunkSize, body, sizeof(M2Bounds), "M2InitParentSequenceBoundsData failure: M2Bounds")) {
                    return false;
                }
                break;
            case 0x43444150:  // PADC: the parent's texture weights
                this->m_parentAnimData = body;
                if (!M2InitParentAnimData(body, chunkSize, this->m_data, reinterpret_cast<M2Array<M2Track<uint16_t>>*>(body))) {
                    M2InitLog("M2InitParentAnimData failure: parentTextureWeights");
                    return false;
                }
                break;
            case 0x43444550:  // PEDC: the parent's events
                this->m_parentEventData = body;
                if (!M2InitParentEventData(body, chunkSize, this->m_data, reinterpret_cast<M2Array<M2TrackBase>*>(body))) {
                    M2InitLog("M2InitParentEventData failure: eventTracks");
                    return false;
                }
                break;
            case 0x43444650:  // PFDC
                // FUN_14125de40: the physics definition, which nothing here simulates yet.
                FROST_UNIMPLEMENTED();
                break;
            case 0x44495047:  // GPID
                if (chunkSize & 3) {
                    return false;
                }
                this->m_geometryParticleIds = body;
                break;
            case 0x44494650:  // PFID
                if (chunkSize != 4) {
                    return false;
                }
                this->m_physicsFileId = static_cast<int32_t>(*body);
                break;
            case 0x434f4244:  // DBOC
                this->m_dbocs = body;
                this->m_dbocCount = chunkSize >> 4;
                break;
            case 0x44494641:  // AFID
                if (chunkSize & 7) {
                    return false;
                }
                this->m_animFileIds = reinterpret_cast<uint64_t*>(body);
                this->m_animFileCount = chunkSize >> 3;
                break;
            case 0x44494642:  // BFID
                if (chunkSize & 3) {
                    return false;
                }
                this->m_boneFileIds = reinterpret_cast<int32_t*>(body);
                this->m_boneFileCount = chunkSize >> 2;
                break;
            case 0x44494653:  // SFID
                if (chunkSize & 3) {
                    return false;
                }
                this->m_skinFileIds = reinterpret_cast<int32_t*>(body);
                this->m_skinFileCount = chunkSize >> 2;
                break;
            case 0x44494b53:  // SKID
                if (chunkSize != 4) {
                    return false;
                }
                this->m_skeletonFileId = static_cast<int32_t>(*body);
                break;
            case 0x44495052:  // RPID
                if (chunkSize & 3) {
                    return false;
                }
                this->m_recursiveParticleIds = body;
                break;
            case 0x44495854:  // TXID
                if (chunkSize & 3) {
                    return false;
                }
                this->m_textureFileIds = reinterpret_cast<int32_t*>(body);
                break;
            case 0x46474445:  // EDGF
                this->m_edgeFades = body;
                this->m_edgeFadeCount = chunkSize / 0x18;
                break;
            case 0x4652454e:  // NERF
                this->m_nerf = body;
                break;
            case 0x4c544544:  // DETL
                this->m_detailLayers = body;
                break;
            default:
                break;
        }
        chunk = next();
    }
    if ((this->m_data->m_flags & 0x100000) == 0) {
        return this->InitializeLoaded();
    }
    FROST_UNIMPLEMENTED();  // FUN_1411f9c90: the skeleton file (SKID)
    return false;
}

// ref: FUN_1411abcf0
bool CM2Shared::InitializeData(M2Data* data, uint32_t size) {
    this->m_dataSize = size;
    this->m_data = data;
    M2INITCONTEXT context;
    if (((data->m_flags >> 0x14) & 1) == 0) {
        context.m_sequences = &data->m_sequences;
    }
    if ((data->m_flags >> 0x17) & 1) {
        this->m_flags |= SHARED_FROM_NAME;
    }
    if (!M2InitAll(data, size, data, &context)) {
        M2SetErrorLocation(s_sharedFile, 0x9b1);
        M2SetError("Failed to initialize model: %s", this->GetDisplayName());
        this->m_flags |= SHARED_ERROR;
        return false;
    }
    // A model with something to draw but empty bounds: the sequences' bounds go too.
    if ((data->m_ribbons.m_count != 0 || data->m_particles.m_count != 0 || data->m_vertices.m_count != 0)
        && data->m_bounds.m_max.x < data->m_bounds.m_min.x && data->m_bounds.m_max.y < data->m_bounds.m_min.y
        && data->m_bounds.m_max.z < data->m_bounds.m_min.z) {
        data->m_bounds = {};
        for (uint32_t i = 0; i < data->m_sequences.m_count; i++) {
            data->m_sequences[i].m_bounds.m_extent = {};
        }
    }
    for (uint32_t i = 0; i < data->m_materials.m_count; i++) {
        M2Material* material = &data->m_materials[i];
        if (4 < material->m_blendMode) {
            material->m_flags |= 1;
        }
    }
    if (this->m_waterfallData != nullptr) {
        data->m_textures.m_count = 4;
    }
    return true;
}

// ref: FUN_1411a6d50
bool CM2Shared::InitializeLoaded() {
    if (!this->InitializeSkinsAndBones()) {
        M2SetErrorLocation(s_sharedFile, 0xd04);
        M2SetError("Failed to initialize model: %s", this->GetDisplayName());
        this->m_flags |= SHARED_ERROR;
        return false;
    }
    if ((this->m_data->m_flags & 0x1000000) == 0 && this->m_physicsFileId != 0) {
        FROST_UNIMPLEMENTED();  // FUN_14125dde0: the physics file
    }
    this->m_flags |= SHARED_LOADED;
    return true;
}

// ref: FUN_1411a9640
bool CM2Shared::InitializeSkinsAndBones() {
    if ((this->m_flags & SHARED_FILE_FREED) == 0) {
        uint32_t profiles = this->m_data->m_numSkinProfiles;
        uint32_t chosen = 0xffffffff;
        for (uint32_t i = 0; i < profiles; i++) {
            if (s_skinProfileBones[i + (4 - profiles)] < 0x101) {
                chosen = i;
                break;
            }
        }
        if (chosen == 0xffffffff) {
            M2SetErrorLocation(s_sharedFile, 0xbc7);
            M2SetError("Failed to choose skin profile: %s", this->GetDisplayName());
            return false;
        }
        this->m_flags &= ~SHARED_TINTED;
        this->LoadSkinProfile(0, chosen, false);
        if ((this->m_cache->m_flags & 0x200) == 0) {
            this->m_lodMax = 0;
        } else {
            if (0 <= this->m_lodFlags) {
                // FUN_14057b640: the ModelFileData LOD count
                FROST_UNIMPLEMENTED();
                this->m_lodMax = 0;
                this->m_lodCount = 1;
            }
            if (this->m_skinFileCount - this->m_data->m_numSkinProfiles == this->m_lodMax) {
                for (uint32_t lod = 1; lod <= this->m_lodMax; lod++) {
                    this->LoadSkinProfile(lod, lod, true);
                    if (this->m_flags & SHARED_FILE_FREED) {
                        return true;
                    }
                }
            } else {
                this->m_lodCount = 1;
                this->m_lodMax = 0;
            }
        }
    }

    M2Array<M2CompBone>* bones = this->GetBones();
    uint32_t specialBones = 0;
    for (uint32_t i = 0; i < bones->m_count; i++) {
        if ((*bones)[i].m_flags & 0x70000) {
            specialBones = static_cast<uint32_t>(bones->m_count) << 2;
            break;
        }
    }
    uint32_t textureBytes = this->m_data->m_textures.m_count != 0 ? static_cast<uint32_t>(this->m_data->m_textures.m_count) << 3 : 0;
    uint32_t boneFileBytes = 0;
    uint32_t boneIndexBytes = 0;
    if (this->m_boneFileCount != 0) {
        boneFileBytes = this->m_boneFileCount << 5;
        boneIndexBytes = static_cast<uint32_t>(bones->m_count) * this->m_boneFileCount * 2;
    }
    uint32_t particleBytes = 0;
    uint32_t* exp2 = nullptr;
    if (this->m_extendedParticles == nullptr) {
        auto chunk = static_cast<uint32_t*>(this->m_fileData);
        auto end = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(this->m_fileData) + this->m_fileSize);
        while (chunk < end) {
            uint32_t id = chunk[0];
            uint32_t step = this->m_fileSize;
            if (id != 0x3032444d) {
                step = chunk[1];
                chunk += 2;
                if (id == 0x54505845) {  // EXPT: the old per-emitter extras
                    particleBytes = static_cast<uint32_t>(this->m_data->m_particles.m_count) * 0x1c + 8;
                    exp2 = chunk;
                    break;
                }
            }
            chunk = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(chunk) + step);
        }
    }
    uint32_t sequences = this->m_data->m_sequences.m_count;
    uint32_t total = particleBytes + sequences * 4 + boneIndexBytes + boneFileBytes + textureBytes + specialBones;
    if (total != 0) {
        auto block = static_cast<uint8_t*>(SMemAlloc(total, 0x12, s_sharedFile, SStrHashFnv1a(s_sharedFile), 0xc59, 8));
        this->m_block = block;
        if (textureBytes != 0) {
            this->m_textures = reinterpret_cast<void**>(block);
            block += textureBytes;
        }
        if (specialBones != 0) {
            this->m_boneOrder = reinterpret_cast<uint32_t*>(block);
            block += specialBones;
        }
        if (sequences != 0) {
            this->m_sequenceState = reinterpret_cast<uint32_t*>(block);
            for (uint32_t i = 0; i < this->m_data->m_sequences.m_count; i++) {
                this->m_sequenceState[i] = 0;
            }
            block += sequences * 4;
        }
        if (boneFileBytes != 0) {
            this->m_boneOffsets = reinterpret_cast<CM2BoneOffset*>(block);
            uint8_t* indices = block + boneFileBytes;
            block = indices + boneIndexBytes;
            for (uint32_t i = 0; i < this->m_boneFileCount; i++) {
                CM2BoneOffset* offset = &this->m_boneOffsets[i];
                offset->m_boneIndices = reinterpret_cast<uint16_t*>(indices);
                offset->m_flags = 0;
                offset->m_offsets = nullptr;
                offset->m_data = nullptr;
                indices += static_cast<uint32_t>(bones->m_count) * 2;
            }
        }
        if (particleBytes != 0) {
            auto array = reinterpret_cast<M2Array<M2ExtendedParticle>*>(block);
            this->m_extendedParticles = array;
            array->m_count = static_cast<uint16_t>(this->m_data->m_particles.m_count);
            array->SetData(block + 8);
            for (uint32_t i = 0; i < this->m_data->m_particles.m_count; i++) {
                M2ExtendedParticle* particle = &array->Data()[i];
                std::memcpy(particle, exp2 + i * 3, 12);
            }
        }
    }
    this->CreateTextures();
    if (this->m_data->m_flags & 0x40000) {
        // Tinted by ModelFileData's color.
        FROST_UNIMPLEMENTED();  // the tint from the ModelFileData and its DB lookups
        this->m_flags |= SHARED_TINTED;
        this->m_tint[0] = 1.0f;
        this->m_tint[1] = 1.0f;
        this->m_tint[2] = 1.0f;
        this->m_tint[3] = 1.0f;
    }

    // The bones in draw order: plain ones first, then each billboard kind.
    uint32_t billboards[0x400];
    uint32_t billboardCount = 0;
    uint32_t plainCount = 0;
    for (uint32_t i = 0; i < bones->m_count; i++) {
        uint32_t flags = (*bones)[i].m_flags;
        if (flags & 0x40006f8) {
            this->m_flags |= SHARED_HAS_SPECIAL_BONES;
        }
        if (flags & 0x80000) {
            this->m_flags |= SHARED_HAS_PHYSICS_BONES;
        }
        if (this->m_boneOrder != nullptr) {
            if ((flags & 0x70000) == 0) {
                this->m_boneOrder[plainCount++] = i;
            } else if (billboardCount < 0x400) {
                billboards[billboardCount++] = i;
            }
        }
    }
    if (this->m_boneOrder != nullptr && billboardCount != 0) {
        uint32_t next = plainCount;
        for (uint32_t i = 0; i < billboardCount; i++) {
            if (((*bones)[billboards[i]].m_flags & 0x70000) == 0x40000) {
                this->m_boneOrder[next++] = billboards[i];
            }
        }
        for (uint32_t i = 0; i < billboardCount; i++) {
            if (((*bones)[billboards[i]].m_flags & 0x30000) == 0x20000) {
                this->m_boneOrder[next++] = billboards[i];
            }
        }
        for (uint32_t i = 0; i < billboardCount; i++) {
            if ((*bones)[billboards[i]].m_flags & 0x10000) {
                this->m_boneOrder[next++] = billboards[i];
            }
        }
    }
    if (this->m_data->m_particles.m_count != 0) {
        this->InitializeParticleData();
    }
    return true;
}

// ref: FUN_1411aabd0
void CM2Shared::LoadSkinProfile(uint32_t slot, uint32_t index, bool lod) {
    auto load = static_cast<CM2SkinProfileLoad*>(SMemAlloc(sizeof(CM2SkinProfileLoad), 8, s_skinProfileLoadTag, 0, -2, 8));
    std::memset(load, 0, sizeof(*load));
    this->m_skinProfileLoads.LinkToTail(load);
    load->m_shared = this;
    load->m_index = slot;
    if (this->m_skinFileIds == nullptr) {
        UnlinkSkinLoad(load);
        return;
    }
    uint32_t file = (this->m_data->m_numSkinProfiles - 1) * (lod ? 1u : 0u) + index;
    if (file < this->m_skinFileCount) {
        load->m_asyncObject = AsyncFileReadById(this->m_skinFileIds[file], this->m_priority, load,
            reinterpret_cast<void*>(&OpenSkinProfile), &AllocModelBuffer, nullptr, &OnSkinRead, &OnSkinError,
            &OnReadCancelled);
    } else {
        OnSkinError(load, nullptr, 0, "Invalid file ID Index");
    }
}

// ref: FUN_1411aa100
bool CM2Shared::InitializeSkinProfile(uint32_t slot) {
    CM2SkinProfileData* skin = &this->m_skins[slot];
    M2SkinProfile* profile = skin->m_profile;
    uint16_t maxBones = 1;
    skin->m_maxBones = 1;
    for (uint32_t i = 0; i < profile->m_skinSections.m_count; i++) {
        uint16_t bones = profile->m_skinSections[i].m_boneCount;
        if (maxBones <= bones) {
            maxBones = bones;
        }
        skin->m_maxBones = maxBones;
    }
    skin->m_bonesPerSlot = maxBones + 1 < 0x101 ? static_cast<uint16_t>(0x100 / (maxBones + 1)) : 1;
    skin->m_boneCount = 0;
    skin->m_lastBone = 0;
    skin->m_sections = profile->m_skinSections.Data();
    if (profile->m_indices.m_count != 0 && profile->m_batches.m_count != 0) {
        skin->m_effects = static_cast<CShaderEffect**>(SMemAlloc(static_cast<uint64_t>(profile->m_batches.m_count) << 3, 0x12,
            s_sharedFile, SStrHashFnv1a(s_sharedFile), 0xe0f, 8));
    }
    if (skin->m_effects != nullptr && (this->m_cache->m_flags & 0x800) == 0) {
        for (uint32_t i = 0; i < profile->m_batches.m_count; i++) {
            M2Batch* batch = &profile->m_batches[i];
            if (this->m_data->m_version < 0x112) {
                batch->m_geosetIndex = 0;
            }
            for (uint32_t t = 0; t < batch->m_textureCount; t++) {
                uint16_t texture = this->m_data->m_textureCombos[batch->m_textureComboIndex + t];
                if (this->m_data->m_textures[texture].m_type == 1) {
                    batch->m_geosetIndex |= 1;
                    break;
                }
            }
            skin->m_effects[i] = this->GetBatchEffect(batch);
        }
    }
    M2Array<M2CompBone>* bones = this->GetBones();
    for (uint32_t i = 0; i < profile->m_skinSections.m_count; i++) {
        const M2SkinSection* section = &profile->m_skinSections[i];
        for (uint32_t b = 0; b < section->m_boneCount; b++) {
            uint16_t bone = this->m_data->m_boneCombos[section->m_boneComboIndex + b];
            while (bone != 0xffff) {
                M2CompBone* compBone = &(*bones)[bone];
                compBone->m_flags |= 0x100000;
                bone = static_cast<uint16_t>(compBone->m_parentBone);
            }
        }
    }
    if ((this->m_data->m_flags & 0x80) && profile->m_shadowBatches.m_count != 0) {
        this->m_flags |= SHARED_HAS_SHADOW_BATCHES;
    }
    return true;
}

// ref: FUN_1411aa8a0
bool CM2Shared::LoadBoneFiles() {
    if (this->m_boneFileCount == 0) {
        return true;
    }
    for (uint32_t i = 0; i < this->m_boneFileCount; i++) {
        CM2BoneOffset* offset = &this->m_boneOffsets[i];
        if ((offset->m_flags & 3) != 0) {
            continue;
        }
        offset->m_flags |= 1;
        auto load = static_cast<CM2BoneOffsetLoad*>(SMemAlloc(sizeof(CM2BoneOffsetLoad), 8, s_boneOffsetLoadTag, 0, -2, 8));
        std::memset(load, 0, sizeof(*load));
        this->m_boneOffsetLoads.LinkToTail(load);
        load->m_index = i;
        load->m_shared = this;
        load->m_asyncObject = AsyncFileReadById(this->m_boneFileIds[i], 3, load, reinterpret_cast<void*>(&OpenBoneFile),
            &AllocBoneBuffer, nullptr, &OnBoneRead, &OnBoneError, &OnReadCancelled);
    }
    return true;
}

// ref: FUN_1411a7bf0
void CM2Shared::OnFullyLoaded() {
    this->m_flags = (this->m_flags & ~SHARED_MULTIPLE_SKINS) | (1 < this->m_skins[0].m_boneCount ? SHARED_MULTIPLE_SKINS : 0);
    CM2Model* model = this->m_waitingModels;
    while (model != nullptr) {
        M2ModelUnlinkWaiting(model);
        if (!M2ModelInitializeLoaded(model)) {
            this->m_flags |= SHARED_ERROR;
        }
        model = this->m_waitingModels;
    }
    // The first sequence's bounds give the center and radius.
    const M2Array<M2Sequence>* sequences;
    uint16_t index;
    if (this->m_skeleton == nullptr) {
        // FUN_1411fed70: the stand sequence (id 0)
        index = M2FindSequenceById(0, &this->m_data->m_sequenceIdxHashById, &this->m_data->m_sequences);
        sequences = &this->m_data->m_sequences;
    } else {
        FROST_UNIMPLEMENTED();  // the skeleton's sequences
        return;
    }
    const M2Sequence* sequence = &(*sequences)[index];
    this->m_boundsRadius = sequence->m_bounds.m_radius;
    const CAaBox& extent = sequence->m_bounds.m_extent;
    this->m_center.x = (extent.m_min.x + extent.m_max.x) * 0.5f;
    this->m_center.y = (extent.m_min.y + extent.m_max.y) * 0.5f;
    this->m_center.z = (extent.m_max.z + extent.m_min.z) * 0.5f;
}

// ref: FUN_1411a5fd0
void CM2Shared::CreateTextures() {
    CStatus status;
    M2Data* data = this->m_data;
    uint8_t quality = (this->m_flags & SHARED_FILE_NAME) == 0 ? 0xc : 0;
    uint32_t flags = this->m_flags & SHARED_FROM_NAME_FLAG;
    // DAT_142720b54, and the pair DAT_142720b4c / DAT_142720b50
    static const uint32_t s_replaceable = 0xffffffff;
    static const uint32_t s_freed[2] = { 0xffffffff, 0xff0000ee };
    for (uint32_t i = 0; i < data->m_textures.m_count; i++) {
        M2Texture* texture = &data->m_textures[i];
        HTEXTURE created;
        if ((this->m_flags & SHARED_FILE_FREED) == 0) {
            // A file name of a character or less is no name: the texture comes by id, if any.
            bool unnamed = texture->m_filename.m_count < 2;
            bool hasId = this->m_textureFileIds != nullptr && this->m_textureFileIds[i] != 0;
            if (unnamed ? !hasId : (this->m_textureFileIds != nullptr && !hasId)) {
                created = TextureCreateSolid(&s_replaceable, nullptr, "replaceable", 0);
            } else if (unnamed) {
                created = TextureCreate(this->m_textureFileIds[i], 0x82094, &status, flags, this->m_priority, quality);
            } else {
                if (data->m_flags & 0x800000) {
                    flags |= 0x40;
                }
                created = TextureCreateByName(texture->m_filename.Data(), 0x82094, &status, flags, this->m_priority, quality);
            }
        } else {
            created = TextureCreateSolid(&s_freed[0], &s_freed[1], "", 2);
        }
        this->m_textures[i] = created;
    }
}

// ref: FUN_1411a61c0
void CM2Shared::InitializeParticleData() {
    FROST_UNIMPLEMENTED();  // the particle emitters' key data (needed with particles)
}

// ---- releasing -------------------------------------------------------------------------------

// ref: FUN_1411a64c0
void CM2Shared::InitializeParticleSequence(uint32_t particle, uint32_t sequence) {
    (void)particle;
    (void)sequence;
    FROST_UNIMPLEMENTED();  // with InitializeParticleData: the emitters' per-sequence directions
}

// ---- .anim files ----------------------------------------------------------------------------

namespace {

// ref: FUN_1411ab330
bool OpenSequenceFile(CAsyncObject* object, cas::CASFILE** file) {
    if (object->m_fileDataId != 0) {
        return cas::CASFileOpenById(object->m_fileDataId, file, 0);
    }
    return cas::CASFileOpenByName(object->m_name, file, (object->m_flags >> 2) & 0x20);
}

// The read is in (LAB_1411ab380): the load forgets its object and the file takes the data.
void OnSequenceFileRead(void* userData, void* buffer, uint32_t size) {
    auto load = static_cast<CM2SequenceLoad*>(userData);
    load->m_asyncObject = nullptr;
    load->m_shared->OnSequenceRead(load, buffer, size);
}

// A sequence's .anim file could not be read (FUN_1411aadc0): it and its aliases are marked
// failed, and the load goes.
// ref: FUN_1411aadc0
void OnSequenceFileError(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)size;
    (void)error;
    auto load = static_cast<CM2SequenceLoad*>(userData);
    CM2Shared* shared = load->m_shared;
    load->m_asyncObject = nullptr;
    M2Sequence* sequence = &shared->m_data->m_sequences[load->m_sequence];
    sequence->m_flags = (sequence->m_flags & ~0x10u) | 0x100;
    for (uint16_t alias = sequence->m_aliasNext; alias != load->m_sequence;) {
        M2Sequence* other = &shared->m_data->m_sequences[alias];
        other->m_flags = (other->m_flags & ~0x10u) | 0x100;
        alias = other->m_aliasNext;
    }
    M2SequenceLoadDestroy(load);
    SMemFree(load, s_sequenceLoadTag, -2, 0);
    if (buffer != nullptr) {
        M2FreeFileData(buffer);
    }
}

}  // namespace

// ref: FUN_1411aa9f0
CM2SequenceLoad* M2SharedRequestSequence(CM2Shared* shared, uint16_t sequence) {
    M2Sequence* requested = &shared->m_data->m_sequences[sequence];
    auto load = static_cast<CM2SequenceLoad*>(SMemAlloc(sizeof(CM2SequenceLoad), 8, s_sequenceLoadTag, 0, -2, 8));
    new (load) CM2SequenceLoad();
    load->m_sequence = 0;
    shared->m_sequenceLoads.LinkToTail(load);
    load->m_sequence = sequence;
    load->m_shared = shared;
    // An alias reads the file of the sequence it shares data with.
    M2Sequence* source = requested;
    while ((source->m_flags & 0x40) != 0) {
        load->m_sequence = source->m_aliasNext;
        source = &shared->m_data->m_sequences[source->m_aliasNext];
    }
    int16_t id = static_cast<int16_t>(source->m_id);
    int16_t variation = static_cast<int16_t>(source->m_variationIndex);
    requested->m_flags |= 0x10;
    for (uint16_t alias = requested->m_aliasNext; alias != sequence;) {
        M2Sequence* other = &shared->m_data->m_sequences[alias];
        other->m_flags |= 0x10;
        alias = other->m_aliasNext;
    }
    for (uint32_t i = 0; i < shared->m_animFileCount; i++) {
        auto entry = reinterpret_cast<const int16_t*>(&shared->m_animFileIds[i]);
        if (entry[0] == id && entry[1] == variation) {
            int32_t fileDataId = *reinterpret_cast<const int32_t*>(entry + 2);
            if (fileDataId != 0) {
                load->m_asyncObject = AsyncFileReadById(fileDataId, 2, load, reinterpret_cast<void*>(&OpenSequenceFile),
                    &AllocModelBuffer, nullptr, &OnSequenceFileRead, &OnSequenceFileError, &OnReadCancelled);
            }
            return load;
        }
    }
    return load;
}

// ref: FUN_1411a4ce0
void M2SequenceLoadDestroy(CM2SequenceLoad* load) {
    while (CM2SequencePlayBack* playback = load->m_playbacks.Head()) {
        playback->m_link.Unlink();
        SMemFree(playback, s_playbackTag, -2, 0);
    }
    load->m_playbacks.~TSList();
    load->m_link.Unlink();
}

// ref: FUN_1411a5510
void M2SequenceLoadsFree(TSList<CM2SequenceLoad, TSGetLink<CM2SequenceLoad>, 8>* loads) {
    while (CM2SequenceLoad* load = loads->Head()) {
        M2SequenceLoadDestroy(load);
        SMemFree(load, s_sequenceLoadTag, -2, 0);
    }
}

// The file's keys for one sequence patched in from its .anim data (FUN_1411a88d0): M2InitAll
// with the sequence in the context.
// ref: FUN_1411a88d0
bool CM2Shared::PatchSequence(uint32_t sequence, M2INITCONTEXT* context) {
    context->m_sequence = sequence;
    context->m_sequences = &this->m_data->m_sequences;
    bool ok = M2InitAll(this->m_data, this->m_dataSize, this->m_data, context);
    M2Sequence* patched = &this->m_data->m_sequences[sequence];
    if (!ok) {
        M2SetErrorLocation(s_sharedFile, 0x6cc);
        M2SetError("Corrupt model sequence data: %s (%d)", this->GetDisplayName(), patched->m_id);
        return false;
    }
    patched->m_flags = (patched->m_flags & ~0x10u) | 0x20;
    for (uint32_t i = 0; i < this->m_data->m_particles.Count(); i++) {
        this->InitializeParticleSequence(i, sequence);
    }
    return true;
}

// ref: FUN_1411aae90
void CM2Shared::OnSequenceRead(CM2SequenceLoad* load, void* buffer, uint32_t size) {
    auto keyframes = static_cast<CM2KeyframeData*>(SMemAlloc(sizeof(CM2KeyframeData), 8, s_keyframeDataTag, 0, -2, 8));
    keyframes->m_link.m_prevlink = nullptr;
    keyframes->m_link.m_next = nullptr;
    keyframes->m_10 = nullptr;
    keyframes->m_data = nullptr;
    keyframes->m_size = 0;
    keyframes->m_sequence = 0xffff;
    this->m_keyframeData.LinkToTail(keyframes);
    keyframes->m_data = buffer;
    keyframes->m_size = size;
    keyframes->m_sequence = load->m_sequence;
    // The file goes to the tail of the cache's list of files with .anim data.
    this->m_keyframeLink.Unlink();
    this->m_cache->m_keyframeShared.LinkToTail(this);
    M2INITCONTEXT context;
    context.m_sequence = 0xffffffff;
    context.m_size = size;
    context.m_base = reinterpret_cast<uint64_t>(buffer);
    if ((this->m_data->m_flags & 0x200000) != 0) {
        // Chunked: the keys are in the AF2M chunk.
        auto chunk = static_cast<uint32_t*>(buffer);
        auto end = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(buffer) + size);
        for (; chunk < end; chunk = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(chunk + 2) + chunk[1])) {
            if (chunk[0] == 0x324d4641) {
                context.m_size = chunk[1];
                context.m_base = reinterpret_cast<uint64_t>(chunk + 2);
            }
        }
    }
    this->m_flags |= SHARED_SEQUENCES_PENDING;
    uint16_t sequence = keyframes->m_sequence;
    if (this->PatchSequence(sequence, &context)) {
        for (uint16_t alias = this->m_data->m_sequences[sequence].m_aliasNext;
             alias != sequence && this->PatchSequence(alias, &context);
             alias = this->m_data->m_sequences[alias].m_aliasNext) {
        }
    }
    for (CM2SequencePlayBack* playback = load->m_playbacks.Head(); playback != nullptr;) {
        CM2SequencePlayBack* next = load->m_playbacks.Next(playback);
        if ((playback->m_flags & 8) != 0) {
            playback->m_link.Unlink();
            SMemFree(playback, s_playbackTag, -2, 0);
            playback = next;
            continue;
        }
        CM2Model* model = playback->m_model;
        uint32_t keyBone = static_cast<uint32_t>((*this->GetBones())[playback->m_bone].m_keyBoneId);
        if (model->AllowSequenceChange(keyBone, playback->m_bone)) {
            M2BoneState* state = &model->m_animState->m_boneStates[playback->m_bone];
            float speed = playback->m_speed;
            M2SequenceTime* slot;
            if ((playback->m_flags & 2) == 0) {
                model->StartBlendSequence(load->m_sequence, playback->m_bone, &playback->m_resolve, playback->m_offset,
                    speed, playback->m_repeats);
                slot = state->m_blendSequence;
            } else {
                M2Array<M2Sequence>* sequences = this->m_skeleton == nullptr
                    ? &this->m_data->m_sequences
                    : M2SkeletonSharedSequences(this->m_skeleton);
                const M2Sequence& played = (*sequences)[load->m_sequence];
                state->m_5c = played.m_id;
                state->m_variation = played.m_variationIndex;
                uint8_t kind = (playback->m_flags & 1) != 0 ? playback->m_kind : 5;
                float weight;
                std::memcpy(&weight, &playback->m_weight, sizeof(float));
                model->StartSequence(load->m_sequence, playback->m_bone, &playback->m_resolve, playback->m_offset, speed,
                    playback->m_repeats, kind, playback->m_fixedBlend, weight);
                slot = state->m_sequence;
            }
            slot->m_endless = playback->m_flags & 4;
            playback->m_model = nullptr;
        }
        playback = next;
    }
    if ((this->m_flags & SHARED_SEQUENCES_FREED) == 0) {
        M2SequenceLoadDestroy(load);
        SMemFree(load, s_sequenceLoadTag, -2, 0);
        this->m_flags &= ~SHARED_SEQUENCES_PENDING;
        return;
    }
    M2SequenceLoadsFree(&this->m_sequenceLoads);
    this->m_flags &= ~(SHARED_SEQUENCES_PENDING | SHARED_SEQUENCES_FREED);
}

// A sequence's keys made offsets into its .anim data again, with its aliases' (FUN_1411a7dd0).
// ref: FUN_1411a7dd0
void CM2Shared::UnpatchSequence(uint32_t sequence, void* data, uint32_t size) {
    M2INITCONTEXT context;
    context.m_base = reinterpret_cast<uint64_t>(data);
    M2Sequence* current = &this->m_data->m_sequences[sequence];
    current->m_flags &= ~0x20u;
    if ((this->m_data->m_flags & 0x200000) != 0) {
        auto end = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data) + size);
        for (auto chunk = static_cast<uint32_t*>(data); chunk < end;
             chunk = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(chunk + 2) + chunk[1])) {
            if (chunk[0] == 0x324d4641) {
                context.m_base = reinterpret_cast<uint64_t>(chunk + 2);
            }
        }
    }
    context.m_size = 0;
    context.m_sequence = sequence;
    M2InitAll(this->m_data, this->m_dataSize, this->m_data, &context);
    for (uint16_t alias = current->m_aliasNext; alias != sequence; alias = current->m_aliasNext) {
        current = &this->m_data->m_sequences[alias];
        current->m_flags &= ~0x20u;
        context.m_sequence = alias;
        if ((this->m_data->m_flags & 0x200000) != 0) {
            auto end = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data) + size);
            for (auto chunk = static_cast<uint32_t*>(data); chunk < end;
                 chunk = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(chunk + 2) + chunk[1])) {
                if (chunk[0] == 0x324d4641) {
                    context.m_size = 0;
                    context.m_base = reinterpret_cast<uint64_t>(chunk + 2);
                }
            }
        }
        M2InitAll(this->m_data, this->m_dataSize, this->m_data, &context);
    }
}

// ref: FUN_1411acb40
bool CM2Shared::ReleaseUnusedKeyframes() {
    bool released = false;
    for (CM2KeyframeData* keyframes = this->m_keyframeData.Head(); keyframes != nullptr;) {
        CM2KeyframeData* next = this->m_keyframeData.Next(keyframes);
        // Kept while any variation of the sequence or of an alias of it is referenced.
        bool used = false;
        uint16_t alias = keyframes->m_sequence;
        do {
            M2Sequence* sequence = &this->m_data->m_sequences[alias];
            for (uint16_t variation = M2FindSequenceById(sequence->m_id, &this->m_data->m_sequenceIdxHashById,
                     &this->m_data->m_sequences);
                 ; variation = static_cast<uint16_t>(this->m_data->m_sequences[variation].m_variationNext)) {
                if (this->m_sequenceState[variation] != 0) {
                    used = true;
                    break;
                }
                if (static_cast<uint16_t>(this->m_data->m_sequences[variation].m_variationNext) == 0xffff) {
                    break;
                }
            }
            if (used) {
                break;
            }
            alias = sequence->m_aliasNext;
        } while (alias != keyframes->m_sequence);
        if (!used) {
            this->UnpatchSequence(keyframes->m_sequence, keyframes->m_data, keyframes->m_size);
            M2FreeFileData(keyframes->m_data);
            keyframes->m_link.Unlink();
            SMemFree(keyframes, s_keyframeDataTag, -2, 0);
            released = true;
        }
        keyframes = next;
    }
    if (this->m_keyframeData.Head() == nullptr) {
        this->m_keyframeLink.Unlink();
    }
    return released;
}

// ref: FUN_1411a5470
void CM2Shared::CancelSequenceLoads() {
    for (CM2SequenceLoad* load = this->m_sequenceLoads.Head(); load != nullptr;
         load = this->m_sequenceLoads.Next(load)) {
        if (load->m_asyncObject != nullptr) {
            AsyncFileCancel(load->m_asyncObject);
            load->m_asyncObject = nullptr;
        }
    }
    if ((this->m_flags & SHARED_SEQUENCES_PENDING) == 0) {
        M2SequenceLoadsFree(&this->m_sequenceLoads);
        return;
    }
    this->m_flags |= SHARED_SEQUENCES_FREED;
}

// ref: FUN_1411a52e0
void CM2Shared::CancelLoads() {
    if (this->m_asyncObject != nullptr) {
        AsyncFileCancel(this->m_asyncObject);
        this->m_asyncObject = nullptr;
    }
    for (CM2SkinProfileLoad* load = this->m_skinProfileLoads.Head(); load != nullptr;) {
        CM2SkinProfileLoad* next = this->m_skinProfileLoads.Next(load);
        if (load->m_asyncObject != nullptr) {
            AsyncFileCancel(load->m_asyncObject);
            load->m_asyncObject = nullptr;
        }
        UnlinkSkinLoad(load);
        load = next;
    }
    for (CM2BoneOffsetLoad* load = this->m_boneOffsetLoads.Head(); load != nullptr;) {
        CM2BoneOffsetLoad* next = this->m_boneOffsetLoads.Next(load);
        if (load->m_asyncObject != nullptr) {
            AsyncFileCancel(load->m_asyncObject);
            load->m_asyncObject = nullptr;
        }
        UnlinkBoneLoad(load);
        load = next;
    }
}

// ref: FUN_1411a6cd0
void CM2Shared::ReleaseTextures() {
    if (this->m_textures == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < this->m_data->m_textures.m_count; i++) {
        if (this->m_textures[i] != nullptr) {
            TextureFree(reinterpret_cast<HTEXTURE*>(&this->m_textures[i]));
        }
    }
    this->m_textures = nullptr;
}

// ref: FUN_1411a7d10
void CM2Shared::ReleaseBoneData() {
    if (this->m_boneOffsets != nullptr) {
        for (uint32_t i = 0; i < this->m_boneFileCount; i++) {
            CM2BoneOffset* offset = &this->m_boneOffsets[i];
            if (offset->m_data != nullptr) {
                M2FreeFileData(offset->m_data);
                offset->m_data = nullptr;
            }
            offset->m_boneIndices = nullptr;
            offset->m_offsets = nullptr;
        }
        this->m_boneOffsets = nullptr;
        this->m_boneFileCount = 0;
    }
    this->m_boneOrder = nullptr;
    this->m_extendedParticles = nullptr;
    if (this->m_block != nullptr) {
        SMemFree(this->m_block, s_sharedFile, 399, 0);
        this->m_block = nullptr;
    }
}

// ref: FUN_1411a5640
void CM2Shared::ClearChunkData() {
    this->m_animFileIds = nullptr;
    this->m_animFileCount = 0;
    this->m_skinFileIds = nullptr;
    this->m_skinFileCount = 0;
    this->m_textureFileIds = nullptr;
    this->m_geometryParticleIds = nullptr;
    this->m_recursiveParticleIds = nullptr;
    this->m_physicsFileId = 0;
    this->m_skeletonFileId = 0;
    this->m_boneFileIds = nullptr;
    this->m_textureCombiners = nullptr;
    this->m_particleCombiners = nullptr;
    this->m_extendedParticles = nullptr;
    this->m_physics = nullptr;
    this->m_skeleton = nullptr;
    this->m_parentEventData = nullptr;
    this->m_parentAnimData = nullptr;
    this->m_parentSequenceBounds = nullptr;
    this->m_blocklistAnimData = nullptr;
    this->m_waterfallData = nullptr;
    this->m_edgeFades = nullptr;
    this->m_edgeFadeCount = 0;
    this->m_nerf = nullptr;
    this->m_detailLayers = nullptr;
    this->m_particleGeosets = nullptr;
    this->m_dbocs = nullptr;
    this->m_dbocCount = 0;
    this->m_afra = nullptr;
}

// ---- queries -----------------------------------------------------------------------------------

// ref: FUN_1411a85d0
const char* CM2Shared::GetDisplayName() {
    if (this->m_flags & SHARED_LOADED) {
        // FUN_140213b70: past the last separator
        const char* name = this->m_name;
        for (const char* p = this->m_name; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\') {
                name = p + 1;
            }
        }
        return name;
    }
    return "Model not loaded";
}

// ref: FUN_1411a86a0
M2Array<M2CompBone>* CM2Shared::GetBones() {
    if (this->m_skeleton != nullptr) {
        FROST_UNIMPLEMENTED();  // the skeleton's bones
    }
    return &this->m_data->m_bones;
}

// ref: FUN_1411a83e0
GxBuf* CM2Shared::GetIndexBuf(uint32_t slot) {
    CM2SkinProfileData* skin = &this->m_skins[slot];
    GxBuf* buf = skin->m_indexBuf;
    if (buf != nullptr && !(GxDevExists() && (buf->m_flags & 0x3000000) != 0x3000000)) {
        return skin->m_indexBuf;
    }
    GxDeviceCallLock lock;
    if (skin->m_indexBuf == nullptr) {
        uint32_t count = skin->m_profile->m_indices.m_count;
        GXBUFINFO info = {};
        info.m_size = static_cast<uint64_t>(count) * 2;
        info.m_bind = 1;
        info.m_format = 1;
        GxBuf* created = GxBufCreateFromInfo(&info);
        GxBuf* old = skin->m_indexBuf;
        skin->m_indexBuf = created;
        if (old != nullptr) {
            GxBufDestroy(&old);
        }
        if (GxDeviceCaps()[0x2f] != 0) {
            FROST_UNIMPLEMENTED();  // the ray-tracing view of the indices
        }
    }
    GxBuf* indexBuf = skin->m_indexBuf;
    if (GxDevExists() && (indexBuf->m_flags & 0x3000000) != 0x3000000) {
        BUFFERUPLOAD upload = { this, skin };
        g_gxUploadInterface->BufUpdate(skin->m_indexBuf, 0, &WriteIndices, &upload);
    }
    return skin->m_indexBuf;
}

// ref: FUN_1411a8700
GxBuf* CM2Shared::GetVertexBuf(uint32_t slot) {
    CM2SkinProfileData* skin = &this->m_skins[slot];
    GxBuf* buf = skin->m_vertexBuf;
    if (buf != nullptr && !(GxDevExists() && (buf->m_flags & 0x3000000) != 0x3000000)) {
        return skin->m_vertexBuf;
    }
    GxDeviceCallLock lock;
    if (skin->m_vertexBuf == nullptr) {
        uint32_t count = skin->m_profile->m_vertices.m_count;
        GXBUFINFO info = {};
        info.m_size = static_cast<uint64_t>(count) * sizeof(M2Vertex);
        info.m_bind = 0;
        info.m_format = 0x14;
        GxBuf* created = GxBufCreateFromInfo(&info);
        GxBuf* old = skin->m_vertexBuf;
        skin->m_vertexBuf = created;
        if (old != nullptr) {
            GxBufDestroy(&old);
        }
        if (GxDeviceCaps()[0x2f] != 0) {
            FROST_UNIMPLEMENTED();  // the ray-tracing view of the vertices
        }
    }
    GxBuf* vertexBuf = skin->m_vertexBuf;
    if (GxDevExists() && (vertexBuf->m_flags & 0x3000000) != 0x3000000) {
        BUFFERUPLOAD upload = { this, skin };
        g_gxUploadInterface->BufUpdate(skin->m_vertexBuf, 0, &WriteVertices, &upload);
    }
    return skin->m_vertexBuf;
}

// The batch's shader effect: the projected ones by texture count, else by its vertex and pixel
// shaders, else the fallback (FUN_1411a5a40).
// ref: FUN_1411a8190
CShaderEffect* CM2Shared::GetBatchEffect(const M2Batch* batch) {
    CShaderEffect* effect;
    if ((batch->m_flags & 4) == 0 && ((batch->m_geosetIndex >> 8) & 2) == 0) {
        uint32_t vertexShader = M2GetVertexShaderId(batch->m_textureCount, batch->m_shaderId);
        uint32_t pixelShader = M2GetPixelShaderId(batch->m_textureCount, batch->m_shaderId);
        char name[0x100];
        if (static_cast<int16_t>(batch->m_shaderId) < 0) {
            SStrPrintf(name, sizeof(name), "M2Effect %d", batch->m_shaderId & 0x7fff);
        } else {
            SStrPrintf(name, sizeof(name), "Vertex %d, Pixel %d", vertexShader, pixelShader);
        }
        effect = ShaderEffectFind(name);
        if (effect == nullptr) {
            effect = ShaderEffectCreate(name, vertexShader, pixelShader);
            if (effect == nullptr) {
                effect = M2FallbackEffect();
            }
        }
        return effect;
    }
    const M2Material* material = &this->m_data->m_materials[batch->m_materialIndex];
    uint32_t lit = ((material->m_flags & 1) != 0 || g_m2ProjectedBlendLit[material->m_blendMode] == 0) ? 0 : 1;
    uint32_t kind;
    if (batch->m_textureCount == 1) {
        kind = material->m_blendMode == 5 ? 8 : 0;
    } else {
        kind = ((batch->m_textureCount - 1) & 3) * 2;
    }
    effect = ShaderEffectFind(g_m2ProjectedEffects[kind | lit]);
    if (effect == nullptr) {
        effect = M2FallbackEffect();
    }
    return effect;
}

// ref: FUN_1411a6bf0
void M2SkinProfileRelease(CM2SkinProfileData* skin) {
    if (skin->m_indexView != nullptr || skin->m_vertexView != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_142288bd0: the ray-tracing views
        skin->m_indexView = nullptr;
        skin->m_vertexView = nullptr;
    }
    GxBuf* buf = skin->m_indexBuf;
    skin->m_indexBuf = nullptr;
    if (buf != nullptr) {
        GxBufDestroy(&buf);
    }
    buf = skin->m_vertexBuf;
    skin->m_vertexBuf = nullptr;
    if (buf != nullptr) {
        GxBufDestroy(&buf);
    }
    if (skin->m_rtGeometry != -1 || skin->m_rtTexcoords != nullptr || skin->m_rtObject != nullptr) {
        FROST_UNIMPLEMENTED();  // the ray-tracing geometry
        skin->m_rtGeometry = -1;
        skin->m_rtTexcoords = nullptr;
        skin->m_rtObject = nullptr;
    }
}

// ref: FUN_1411a8070
CM2BoneOffset* M2SharedGetBoneOffset(CM2Shared* shared, uint32_t index) {
    if (index < shared->m_boneFileCount) {
        return &shared->m_boneOffsets[index];
    }
    return nullptr;
}

// ref: FUN_1411a6190
bool M2BoneOffsetIsLoaded(const CM2BoneOffset* offset) {
    return offset->m_boneIndices != nullptr && offset->m_offsets != nullptr;
}

// ref: FUN_1411a86d0
uint32_t M2SharedLodBoneMask(CM2Shared* shared, uint32_t lod) {
    if (lod == 0) {
        return 0;
    }
    if (shared->m_lodFlags < 0) {
        return 0x10000u << (shared->m_lodLevels[lod] & 0x1f);
    }
    return 0x10000u << ((lod - 1) & 0x1f);
}

// ref: FUN_1411acd30
bool CM2Shared::WaitForLoad() {
    bool waited = this->m_asyncObject != nullptr;
    if (waited) {
        AsyncFileWait(this->m_asyncObject);
    }
    if (this->m_skeleton != nullptr) {
        waited = M2SkeletonWaitForLoad(this->m_skeleton);
    }
    for (CM2SkinProfileLoad* load = this->m_skinProfileLoads.Head(); load != nullptr;) {
        CM2SkinProfileLoad* next = this->m_skinProfileLoads.Next(load);
        AsyncFileWait(load->m_asyncObject);
        waited = true;
        load = next;
        if ((this->m_flags & SHARED_ERROR) != 0) {
            break;
        }
    }
    for (CM2BoneOffsetLoad* load = this->m_boneOffsetLoads.Head(); load != nullptr;) {
        CM2BoneOffsetLoad* next = this->m_boneOffsetLoads.Next(load);
        AsyncFileWait(load->m_asyncObject);
        waited = true;
        if ((this->m_flags & SHARED_ERROR) != 0) {
            return true;
        }
        load = next;
    }
    return waited;
}

// ref: FUN_1411ac0e0
bool CM2Shared::SetPriority(int32_t priority, bool textures) {
    if (this->m_asyncObject != nullptr && !AsyncFileSetSortKey(this->m_asyncObject, priority)) {
        return false;
    }
    if (textures && (this->m_flags & SHARED_LOADED) != 0) {
        for (uint32_t i = 0; i < this->m_data->m_textures.Count(); i++) {
            auto texture = static_cast<HTEXTURE>(this->m_textures[i]);
            if (texture != nullptr && !TextureSetPriority(texture, priority)) {
                return false;
            }
        }
    }
    for (CM2SkinProfileLoad* load = this->m_skinProfileLoads.Head(); load != nullptr;
         load = this->m_skinProfileLoads.Next(load)) {
        if (load->m_asyncObject != nullptr && !AsyncFileSetSortKey(load->m_asyncObject, priority)) {
            return false;
        }
    }
    for (CM2BoneOffsetLoad* load = this->m_boneOffsetLoads.Head(); load != nullptr;
         load = this->m_boneOffsetLoads.Next(load)) {
        if (load->m_asyncObject != nullptr && !AsyncFileSetSortKey(load->m_asyncObject, priority)) {
            return false;
        }
    }
    return true;
}

// ref: FUN_1411ac0d0
void M2SharedAddSequenceRef(CM2Shared* shared, uint32_t sequence) {
    _InterlockedIncrement(
        reinterpret_cast<volatile long*>(&shared->m_sequenceState[static_cast<uint16_t>(sequence)]));
}

// ref: FUN_1411ac0b0
void M2SharedReleaseSequence(CM2Shared* shared, uint32_t sequence) {
    _InterlockedDecrement(
        reinterpret_cast<volatile long*>(&shared->m_sequenceState[static_cast<uint16_t>(sequence)]));
}
