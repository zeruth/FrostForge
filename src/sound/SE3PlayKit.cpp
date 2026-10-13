// SE3 (Engine/Source/SoundEngine/SE3.cpp): playing a sound kit, and stopping one.

#include "sound/SE3Kit.hpp"

#include <cmath>
#include <cstring>

#include "blz/ChainedHash.hpp"
#include "cas/CAS.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "fileservices/AsyncFile.hpp"
#include "fmod.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Bus.hpp"
#include "sound/SE3Internal.hpp"
#include "storm/Memory.hpp"
#include "storm/thread/SCritSect.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"

namespace {

const uint8_t* SoundKitRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

const uint8_t* AdvancedRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitAdvancedDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

bool IsAlive(const SE3SoundKitObject* object) {
    return object->m_playing.m_next != nullptr || object->m_loading.m_next != nullptr
        || object->m_pending.m_next != nullptr || object->m_waiting.m_next != nullptr;
}

void LinkTail(SE3KITLIST* list, TSLink<SE3SoundKitObject>* link, SE3SoundKitObject* object) {
    link->Unlink();
    list->LinkToTail(object);
}

// The files MuteSoundFile silenced (0x142cfced8): blz::unordered_set<int>.
struct MUTEDNODE {
    MUTEDNODE* m_next;
    int32_t m_key;
};
blz::chained_hash_header s_mutedFiles;
const char s_mutedNodeTag[] = ".?AU?$chained_hash_node@H@blz@@";
const char s_mutedBucketTag[] = ".?AU?$chained_hash_node@H@blz@@*";

}

// ref: FUN_1412d51d0
void SE3MuteFile(int32_t fileDataId, bool mute) {
    if (mute) {
        bool inserted;
        blz::chained_insert<MUTEDNODE>(&s_mutedFiles, fileDataId, &inserted, s_mutedNodeTag, s_mutedBucketTag);
    } else if (MUTEDNODE* node = blz::chained_unlink<MUTEDNODE>(&s_mutedFiles, fileDataId)) {
        blz::chained_free_node(node, s_mutedNodeTag);
    }
}

// Whether a kit may play now: its chance, the game's filter, the play's own check, its range
// from the listener, duplicates, a prefetch in flight, and the bus.
// ref: FUN_1412d3e10
uint32_t SE3CanPlay(SE3SoundKitDef* def, SE3PLAYPARAMS* params, const float* position) {
    uint32_t flags = 0;
    if (def != nullptr) {
        if (const uint8_t* advanced = AdvancedRecord(def->m_advancedId)) {
            float chance = g_soundKitAdvancedDB.FieldFloat(advanced, 0x2a, 0);
            uint32_t bits = (SE3Random() & 0x7fffff) | 0x3f800000;
            float roll;
            std::memcpy(&roll, &bits, sizeof(roll));
            if (chance < roll - 1.0f) {
                return 0x18;
            }
        }
        if (g_se3Config.m_slot210.m_invoke != nullptr) {
            using INVOKE = bool (*)(SE3SoundKitDef*, SE3PLAYPARAMS*, void*);
            if (!reinterpret_cast<INVOKE>(g_se3Config.m_slot210.m_invoke)(def, params, g_se3Config.m_slot210.m_storage)) {
                return 0x19;
            }
        }
        if (const uint8_t* kit = SoundKitRecord(def->m_kitId)) {
            flags = DB2FieldU16(kit, 3, 0, &g_soundKitDB);
            if (((flags >> 13) & 1) != 0 && (params->m_a8[0] != 0 || params->m_a8[1] != 0)
                && params->m_88.m_invoke != nullptr) {
                using INVOKE = bool (*)(uint64_t*, void*);
                if (!reinterpret_cast<INVOKE>(params->m_88.m_invoke)(params->m_a8, params->m_88.m_storage)) {
                    return 0xe;
                }
            }
        }
    }
    bool hasPosition = position != nullptr
        && 2.3841858e-07f <= std::fabs(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]);
    if (def != nullptr) {
        if (hasPosition && params->m_checkRange != 0) {
            bool out;
            if (params->m_3dMode == 2 || (params->m_3dMode == 0 && ((flags >> 9) & 1) == 0)) {
                out = SE3IsOutOfRange(position, SE3GetMaxDistance(params, def));
            } else {
                out = SE3IsOutOfRange(position, SE3GetMaxDistance(params, def) + 25.0f);
            }
            if (out) {
                return 0xf;
            }
        }
        if (params->m_duplicates == 2 || (params->m_duplicates == 0 && (flags & 0x20) != 0)) {
            int32_t count = 0;
            for (SE3SoundKitObject* each = def->m_instances.Head(); each != nullptr; each = def->m_instances.Next(each)) {
                count++;
            }
            if (count != 0 && (params->m_parent == nullptr || params->m_parent->m_defLink.m_next == nullptr)) {
                return 0x10;
            }
        }
        if (def->m_timerLink.m_next != nullptr) {
            return 0x17;
        }
    }
    if (!SE3BusIsEnabled(params->m_bus)) {
        return 9;
    }
    return SE3BusIsFull(params->m_bus) ? 10 : 0;
}

namespace {

// LAB_1412c8cd0 / LAB_1412c8c60: a kit's prefetch finished or failed.
void PrefetchRead(void* userData, void* buffer, uint32_t size) {
    (void)buffer;
    (void)size;
    auto def = SE3GetKitDef(static_cast<int32_t>(reinterpret_cast<intptr_t>(userData)));
    if (def != nullptr) {
        def->m_prefetch = nullptr;
        def->m_loadState = 2;
    }
}

void PrefetchError(void* userData, void* buffer, uint32_t size, const char* error) {
    (void)error;
    PrefetchRead(userData, buffer, size);
}

}

// The rest of a play once its file is here: the muted check, the sound group, the mode, and
// the cached sound found or made (FUN_1412ce100's LAB_1412cebe2).
uint32_t PlayFile(SE3SoundKitDef* def, SE3PLAYPARAMS* p, int32_t* handle, const float* position, int32_t entry,
    int32_t fileDataId, bool stream, const void* key) {
    if (blz::chained_find<MUTEDNODE>(&s_mutedFiles, fileDataId) != nullptr) {
        return 9;
    }
    const uint8_t* advanced = nullptr;
    if (def != nullptr) {
        advanced = AdvancedRecord(def->m_advancedId);
        FMOD::SoundGroup* group = SE3GetSoundGroup(def);
        group->setMaxAudibleBehavior(static_cast<FMOD_SOUNDGROUP_BEHAVIOR>(p->m_38));
        if (advanced != nullptr) {
            switch (DB2FieldU8(advanced, 0x22, 0, &g_soundKitAdvancedDB)) {
                case 1:
                    group->setMaxAudibleBehavior(FMOD_SOUNDGROUP_BEHAVIOR_FAIL);
                    break;
                case 2:
                    group->setMaxAudibleBehavior(FMOD_SOUNDGROUP_BEHAVIOR_MUTE);
                    break;
                case 3:
                    group->setMaxAudibleBehavior(FMOD_SOUNDGROUP_BEHAVIOR_STEALLOWEST);
                    break;
                default:
                    break;
            }
        }
    }
    bool hasPosition = p->m_parent == nullptr ? position != nullptr : p->m_parent->m_hasPosition != 0;
    uint32_t placement = 0;
    if (p->m_138 == 0 && hasPosition) {
        if (position != nullptr) {
            placement = 0x20;
        }
    } else {
        placement = 0x40;
    }
    uint32_t dimension = hasPosition ? 0x10 : 8;
    bool looping;
    if (def == nullptr || p->m_3dMode != 0) {
        looping = p->m_3dMode == 1;
    } else {
        looping = SE3KitIsLooping(def->m_kitId);
    }
    uint32_t mode = dimension | placement | (looping ? 0x2010002 : 0x2010001);
    if (def != nullptr && def->m_advancedId != 0) {
        mode |= 0x40;
    }
    int32_t cacheKey = static_cast<int32_t>(((((mode & 2) * 2) ^ (mode & 0xfffffff8)) << 0x1c) ^ static_cast<uint32_t>(fileDataId));
    SE3CacheNode* node = nullptr;
    SE3SamplesLock(true);
    bool ready = SE3CacheFindReady(cacheKey, &node);
    if (ready || SE3CacheFindLoading(cacheKey, &node)) {
        SE3SoundKitObject* object;
        SE3PLAYPARAMS* own = p;
        if (p->m_parent == nullptr) {
            object = SE3CreateObject(handle, p, def);
        } else {
            object = p->m_parent;
            own = &object->m_params;
        }
        object->m_entry = entry;
        object->m_def = def;
        object->m_priorityBase = SE3BusGetPriority(own->m_bus);
        object->m_priorityRange = SE3BusGetPriorityRange(own->m_bus);
        object->m_channelPriority = object->m_priorityBase;
        SE3SetupObject(object, position, own, mode);
        SE3SetSlotSound(object, node->m_serial, entry);
        if (!ready) {
            LinkTail(&g_se3Loading, &object->m_loading, object);
        } else {
            SE3QueueStart(object);
        }
        SE3SamplesLock(false);
        return 0;
    }
    SE3SamplesLock(false);
    cas::CASFILE* file = nullptr;
    if (!cas::CASFileOpenById(fileDataId, &file, 1)) {
        return 8;
    }
    uint32_t size = static_cast<uint32_t>(cas::CASFileGetSize(file));
    uint8_t streamed;
    if (SE3IsMp3(fileDataId)) {
        streamed = 1;
    } else {
        streamed = static_cast<uint32_t>(g_se3CacheMaxCacheable) < size ? 1 : 0;
        size *= 0xc;
    }
    SE3SoundKitObject* object;
    SE3PLAYPARAMS* own = p;
    if (p->m_parent == nullptr) {
        object = SE3CreateObject(handle, p, def);
    } else {
        object = p->m_parent;
        own = &object->m_params;
    }
    object->m_entry = entry;
    object->m_def = def;
    if (advanced != nullptr) {
        int32_t range = DB2FieldI32(advanced, 8, 0, &g_soundKitAdvancedDB);
        object->m_300 = SE3RandomRange(-range, range);
    }
    object->m_streamed = streamed;
    object->m_priorityBase = SE3BusGetPriority(own->m_bus);
    object->m_priorityRange = SE3BusGetPriorityRange(own->m_bus);
    object->m_channelPriority = object->m_priorityBase;
    SE3SetupObject(object, position, own, mode);
    LinkTail(&g_se3Loading, &object->m_loading, object);
    FMOD::SoundGroup* group = def != nullptr ? SE3GetSoundGroup(def) : nullptr;
    SE3ASYNCDATA* async = stream ? SE3AsyncDataCreate(file, static_cast<const uint32_t*>(key)) : nullptr;
    uint32_t result = SE3CacheCreate(cacheKey, group, mode, object, static_cast<int32_t>(size), fileDataId, &file, &node, async);
    if (result == 0) {
        SE3SetSlotSound(object, node->m_serial, entry);
        return 0;
    }
    SE3QueueRelease(object);
    return 3;
}

// A kit whose file is not on disk yet: an instance waits on the download.
// ref: FUN_1412c79c0
uint32_t SE3PrefetchKit(SE3SoundKitDef* def, int32_t fileDataId, int32_t entry, int32_t* handle,
    SE3PLAYPARAMS* params, const float* position) {
    SE3SoundKitObject* object = params->m_parent;
    if (object == nullptr) {
        object = SE3CreateObject(handle, params, def);
    }
    object->m_def = def;
    object->m_entry = entry;
    bool hasPosition = params->m_parent == nullptr ? position != nullptr : params->m_parent->m_hasPosition != 0;
    uint32_t placement = 0;
    if (params->m_138 == 0 && hasPosition) {
        if (position != nullptr) {
            placement = 0x20;
        }
    } else {
        placement = 0x40;
    }
    uint32_t dimension = hasPosition ? 0x10 : 8;
    bool looping;
    if (def == nullptr || params->m_3dMode != 0) {
        looping = params->m_3dMode == 1;
    } else {
        looping = SE3KitIsLooping(def->m_kitId);
    }
    uint32_t mode = dimension | placement | (looping ? 0x2010002 : 0x2010001);
    if (def != nullptr && def->m_advancedId != 0) {
        mode |= 0x40;
    }
    SE3SetupObject(object, position, params, mode);
    if (SE3SoundKitDef* own = SE3GetKitDef(def->m_kitId)) {
        if (const uint8_t* advanced = AdvancedRecord(own->m_advancedId)) {
            if (DB2FieldU8(advanced, 9, 0, &g_soundKitAdvancedDB) == 1) {
                object->m_params.m_142 = 1;
            }
        }
    }
    params->m_144 = 0;
    def->m_prefetchTime = static_cast<int32_t>(FrameTime_GetTime());
    def->m_prefetch = AsyncFileReadById(fileDataId, 2, reinterpret_cast<void*>(static_cast<intptr_t>(object->m_def->m_kitId)),
        nullptr, nullptr, nullptr, &PrefetchRead, &PrefetchError, nullptr);
    if (def->m_prefetch == nullptr) {
        if (handle != nullptr) {
            *handle = 0;
        }
        SE3ObjectFree(object);
        return 8;
    }
    if (params->m_prefetchOnly == 0) {
        def->m_loadState = 2;
        LinkTail(&g_se3Waiting, &object->m_waiting, object);
    } else {
        AsyncFileWait(static_cast<CAsyncObject*>(def->m_prefetch));
        if (params->m_parent == nullptr) {
            SE3QueueRelease(object);
        }
        if (def->m_3a == 0) {
            return 0x13;
        }
    }
    return 0;
}

// Plays a sound kit (or, for kit -128, the play's file): picks the variation, finds or makes
// the cached sound, and puts an instance on the list that starts it. A kit with child kits
// plays each under one combined handle.
// ref: FUN_1412ce100
uint32_t SE3PlayKit(uint32_t kitId, const float* position, SE3PLAYPARAMS* params, int32_t* handle, const char* reason,
    bool prefetch, const void* key) {
    (void)reason;
    if (g_fmodSystem == nullptr || g_se3ShuttingDown != 0) {
        return 0x11;
    }
    if (g_fmodOutputType == FMOD_OUTPUTTYPE_NOSOUND) {
        return 0xb;
    }
    SE3PLAYPARAMS local;
    SE3PlayParamsConstruct(&local);
    SE3PLAYPARAMS* p = params != nullptr ? params : &local;
    uint32_t result = 0;
    SE3SoundKitDef* def = nullptr;
    if (kitId != 0xffffff80) {
        def = SE3GetKitDef(static_cast<int32_t>(kitId));
        if (def == nullptr) {
            SE3PlayParamsDestroy(&local);
            return 6;
        }
        if (def->m_entryCount == 0) {
            SE3PlayParamsDestroy(&local);
            return 7;
        }
    }
    if (p->m_parent == nullptr && def != nullptr && def->m_bus != SE3BUS_COUNT) {
        p->m_bus = def->m_bus;
    }
    const uint8_t* kit = def != nullptr ? SoundKitRecord(def->m_kitId) : nullptr;
    if (def != nullptr) {
        // SoundKit flags 0x2000: never combined; 0x8: m_10 cleared. A kit with child kits is
        // never combined either.
        if (kit != nullptr) {
            if (((DB2FieldU16(kit, 3, 0, &g_soundKitDB) >> 0xd) & 1) != 0) {
                p->m_combinable = 0;
            }
            if ((DB2FieldU16(kit, 3, 0, &g_soundKitDB) & 8) != 0) {
                p->m_10 = 0;
            }
        }
        if (def->m_childCount != 0) {
            p->m_combinable = 0;
        }
    }
    if (p->m_combinable != 0) {
        SE3CombinableAdd(kitId, position, p);
        SE3PlayParamsDestroy(&local);
        return 0;
    }
    result = SE3CanPlay(def, p, position);
    if (result != 0) {
        SE3PlayParamsDestroy(&local);
        return result;
    }
    int8_t priority = kit != nullptr ? static_cast<int8_t>(DB2FieldU8(kit, 0xc, 0, &g_soundKitDB)) : 0;
    int8_t wanted = p->m_priority;
    if (wanted == 4) {
        p->m_priority = priority;
        wanted = priority;
    }
    if (p->m_parent == nullptr && wanted != 0 && def->m_bus != 'a') {
        p->m_bus = SE3BUS_DIALOG;
    }
    if (handle != nullptr) {
        if (*handle != 0 && SE3IsPlaying(static_cast<uint32_t>(*handle)) && wanted != 0) {
            if (wanted != 1 && SE3GetObject(static_cast<uint32_t>(*handle))->m_priority <= wanted) {
                SE3PlayParamsDestroy(&local);
                return 0x16;
            }
            SE3Stop(static_cast<uint32_t>(*handle), 4, 0, nullptr);
        }
        int32_t existing = *handle;
        if (existing != 0 && p->m_stopDuplicates != 0) {
            SE3SoundKitObject* other = SE3GetObject(static_cast<uint32_t>(existing));
            if (other != nullptr && (other->m_3dMode == 0 || other->m_3dMode == 1)) {
                SE3Stop(static_cast<uint32_t>(existing), 4, 0, nullptr);
            }
        }
    }
    if (def == nullptr || def->m_childCount == 0 || p->m_parent != nullptr || p->m_143 != 0) {
        int32_t entry = -1;
        int32_t fileDataId = 0;
        bool stream = false;
        if (def == nullptr || kitId == 0xffffff80) {
            fileDataId = p->m_fileDataId;
            if (fileDataId == 0) {
                SE3PlayParamsDestroy(&local);
                return 8;
            }
            stream = SE3IsMp3(fileDataId) && prefetch;
            if (!stream && cas::CASFileGetResidency(fileDataId, 0, 0) != 2) {
                AsyncFilePrefetchById(fileDataId);
                SE3PlayParamsDestroy(&local);
                return 0x13;
            }
        } else {
            SE3PickEntry(def, p, &entry);
            const uint8_t* record = entry != -1 ? SE3GetKitEntry(def, entry) : nullptr;
            fileDataId = record != nullptr ? DB2FieldI32(record, 1, 0, &g_soundKitEntryDB) : 0;
            if (fileDataId == 0) {
                SE3PlayParamsDestroy(&local);
                return 8;
            }
            stream = prefetch;
            if (!(prefetch && SE3IsMp3(fileDataId))) {
                stream = false;
                if (!SE3EntryResident(def, entry)) {
                    if (def->m_prefetch != nullptr) {
                        SE3PlayParamsDestroy(&local);
                        return 0x13;
                    }
                    result = SE3PrefetchKit(def, fileDataId, entry, handle, p, position);
                    if (result != 0) {
                        SE3PlayParamsDestroy(&local);
                        return result;
                    }
                    if (def->m_prefetch != nullptr) {
                        // play one that is already here instead, if any; the file asked for
                        // stays the one first picked (as the reference does)
                        bool found = false;
                        if (p->m_entry != -1) {
                            for (int32_t i = 0; i < def->m_entryCount; i++) {
                                const uint8_t* other = SE3GetKitEntry(def, i);
                                int32_t otherFile = other != nullptr ? DB2FieldI32(other, 1, 0, &g_soundKitEntryDB) : 0;
                                if (cas::CASFileGetResidency(otherFile, 0, 0) != 1) {
                                    def->m_loadState = p->m_parent == nullptr ? 2 : 0;
                                    entry = i;
                                    found = true;
                                    break;
                                }
                            }
                        }
                        if (!found) {
                            def->m_loadState = 1;
                            SE3PlayParamsDestroy(&local);
                            return 0x13;
                        }
                    }
                }
            }
        }
        result = PlayFile(def, p, handle, position, entry, fileDataId, stream, key);
        SE3PlayParamsDestroy(&local);
        return result;
    }
    // a kit with child kits: each under one combined handle
    int32_t groupHandle = g_se3LastHandle + 1;
    SE3COMBINEDNODE* combined = g_se3Combined.New(static_cast<uint32_t>(groupHandle), 0, 0);
    combined->m_hashval = static_cast<uint32_t>(groupHandle);
    int32_t combinedHandle = combined->m_30;
    if (handle != nullptr) {
        *handle = combinedHandle;
    }
    SE3PLAYPARAMS member;
    SE3PlayParamsConstruct(&member);
    SE3PlayParamsCopy(&member, p);
    member.m_entry = -1;
    member.m_143 = 1;
    int32_t memberHandle = 0;
    result = SE3PlayKit(kitId, position, &member, &memberHandle, reason, false, nullptr);
    SE3SoundKitObject* first = SE3GetObject(static_cast<uint32_t>(memberHandle));
    if (result == 0 || (first != nullptr && first->m_waiting.m_next != nullptr)) {
        SE3CombinedAdd(combined, static_cast<uint32_t>(memberHandle));
        if (first != nullptr) {
            first->m_combinedHandle = combinedHandle;
        }
        for (uint32_t i = 0; i < def->m_childCount; i++) {
            int32_t child = def->m_children[i];
            bool resetMode = SE3KitIsLooping(child);
            if (!resetMode) {
                if (SE3SoundKitDef* childDef = SE3GetKitDef(child)) {
                    if (const uint8_t* advanced = AdvancedRecord(childDef->m_advancedId)) {
                        resetMode = DB2FieldU8(advanced, 9, 0, &g_soundKitAdvancedDB) == 1;
                    }
                }
            }
            if (resetMode) {
                member.m_3dMode = 0;
                member.m_checkRange = 0;
                member.m_135 = 1;
            }
            memberHandle = 0;
            uint32_t childResult = SE3PlayKit(static_cast<uint32_t>(child), position, &member, &memberHandle, nullptr,
                false, nullptr);
            SE3SoundKitObject* childObject = SE3GetObject(static_cast<uint32_t>(memberHandle));
            if (childResult == 0 || (childObject != nullptr && childObject->m_waiting.m_next != nullptr)) {
                SE3CombinedAdd(combined, static_cast<uint32_t>(memberHandle));
                if (childObject != nullptr) {
                    childObject->m_combinedHandle = combinedHandle;
                }
            }
        }
    } else {
        g_se3Combined.Unlink(combined);
        g_se3Combined.Delete(combined);
    }
    SE3PlayParamsDestroy(&member);
    SE3PlayParamsDestroy(&local);
    return result;
}

// A voice-over: one at a time, by VoiceOverPriority. A higher priority (or an equal one that
// may interrupt) stops the one playing, fading by the play's own fade out.
// ref: FUN_1412cf350
uint32_t SE3PlayVoiceOver(uint32_t kitId, const float* position, SE3PLAYPARAMS* params, int32_t* handle,
    const char* reason, bool prefetch, const void* key, const SE3VOICEOVERPRIORITY* priority) {
    if (priority->m_set == 0) {
        return SE3PlayKit(kitId, position, params, handle, reason, prefetch, key);
    }
    int32_t wanted = priority->m_priority;
    if (const uint8_t* kit = SoundKitRecord(static_cast<int32_t>(kitId))) {
        if (((DB2FieldU16(kit, 3, 0, &g_soundKitDB) >> 0xe) & 1) != 0) {
            return SE3PlayKit(kitId, position, params, nullptr, reason, prefetch, key);
        }
    }
    bool local;
    auto asked = static_cast<const uint8_t*>(g_voiceOverPriorityDB.GetRecord(static_cast<uint32_t>(wanted), false, &local));
    auto current = static_cast<const uint8_t*>(
        g_voiceOverPriorityDB.GetRecord(static_cast<uint32_t>(g_se3VoiceOverPriority), false, &local));
    if (asked == nullptr) {
        wanted = 4;
        asked = static_cast<const uint8_t*>(g_voiceOverPriorityDB.GetRecord(4, false, &local));
    }
    if (current == nullptr) {
        g_se3VoiceOverHandle = 0;
        g_se3VoiceOverPriority = 4;
        current = static_cast<const uint8_t*>(g_voiceOverPriorityDB.GetRecord(4, false, &local));
    }
    if (asked == nullptr || current == nullptr) {
        return 0x1a;
    }
    if (g_se3VoiceOverHandle == 0) {
        uint32_t result = SE3PlayKit(kitId, position, params, &g_se3VoiceOverHandle, reason, prefetch, key);
        if (result != 0) {
            return result;
        }
        if (handle != nullptr) {
            *handle = g_se3VoiceOverHandle;
        }
        g_se3VoiceOverPriority = wanted;
        g_se3VoiceOverKit = static_cast<int32_t>(kitId);
        return 0;
    }
    int32_t askedPriority = DB2FieldI32(asked + 4, 1, 4, &g_voiceOverPriorityDB);
    int32_t currentPriority = DB2FieldI32(current + 4, 1, 4, &g_voiceOverPriorityDB);
    bool preempt = askedPriority < currentPriority
        || (askedPriority == currentPriority && (DB2FieldI32(asked + 4, 2, 4, &g_voiceOverPriorityDB) & 1) == 0);
    if (!preempt) {
        return 0x16;
    }
    if (params->m_fadeOutType == 0xb) {
        SE3Stop(static_cast<uint32_t>(g_se3VoiceOverHandle), 0xb, params->m_fadeOutMs, reason);
    } else {
        SE3Stop(static_cast<uint32_t>(g_se3VoiceOverHandle), 2, 0, reason);
    }
    g_se3VoiceOverHandle = 0;
    uint32_t result = SE3PlayKit(kitId, position, params, &g_se3VoiceOverHandle, reason, prefetch, key);
    if (result == 0) {
        g_se3VoiceOverPriority = wanted;
        g_se3VoiceOverKit = static_cast<int32_t>(kitId);
        if (handle != nullptr) {
            *handle = g_se3VoiceOverHandle;
        }
    }
    return result;
}

// ---- stopping -----------------------------------------------------------------------------

// Stops one instance: an instance not yet started is released; a playing one fades out with
// the asked-for fade (or its own), or stops.
// ref: FUN_1412d4ee0
bool SE3StopObject(SE3SoundKitObject* object, int32_t fadeType, int32_t fadeMs) {
    if (object == nullptr) {
        return false;
    }
    if (object->m_playing.m_next == nullptr) {
        if (object->m_loading.m_next == nullptr && object->m_waiting.m_next == nullptr) {
            if (object->m_pending.m_next == nullptr) {
                return false;
            }
            object->m_pending.Unlink();
        } else {
            object->m_loading.Unlink();
            object->m_waiting.Unlink();
        }
        SE3QueueRelease(object);
        return true;
    }
    if (object->m_pending.m_next == nullptr) {
        if (fadeType == 0) {
            goto stop;
        }
    } else {
        object->m_pending.Unlink();
        fadeType = 1;
    }
    object->m_params.m_fadeOutType = fadeType;
    object->m_params.m_fadeOutMs = fadeMs;
stop:
    if (object->m_3dMode == 1) {
        object->m_3dMode = 2;
    }
    if (static_cast<uint32_t>(object->m_params.m_fadeOutType) < 2) {
        SE3StopSlots(object);
        return true;
    }
    SE3StopCombinedMembers(object);
    SE3FadeOut(object);
    return true;
}

// Stops a handle (or every member of a combined handle).
// ref: FUN_1412d4d80
bool SE3Stop(uint32_t handle, int32_t fadeType, int32_t fadeMs, const char* reason) {
    if (handle == 0) {
        return true;
    }
    if (SE3HANDLENODE* node = g_se3Handles.Find(handle, [handle](SE3HANDLENODE* each) {
            return static_cast<uint32_t>(each->m_hashval) == handle;
        })) {
        if (node->m_object != nullptr) {
            return SE3StopObject(node->m_object, fadeType, fadeMs);
        }
    }
    SE3COMBINEDNODE* combined = g_se3Combined.Find(handle, [handle](SE3COMBINEDNODE* each) {
        return static_cast<uint32_t>(each->m_hashval) == handle;
    });
    if (combined != nullptr) {
        for (uint32_t i = 0; i < combined->m_count; i++) {
            SE3Stop(combined->m_handles[i], fadeType, fadeMs, reason);
        }
    }
    return true;
}

// ref: FUN_1412d4d60
bool SE3StopNow(uint32_t handle, const char* reason) {
    return SE3Stop(handle, 0, 0, reason);
}

// Stops both slots; an instance with nothing left goes to be released.
// ref: FUN_1412d50e0
void SE3StopSlots(SE3SoundKitObject* object) {
    uint8_t playing = object->m_slots[0].m_channel != nullptr ? 1 : 0;
    if (playing != 0) {
        SE3SlotStop(&object->m_slots[0]);
    }
    if (object->m_slots[1].m_channel != nullptr) {
        playing++;
        SE3SlotStop(&object->m_slots[1]);
    }
    if (playing == 0) {
        object->m_playing.Unlink();
        SE3QueueRelease(object);
    }
}

// The other members of a combined play stop with its first.
// ref: FUN_1412d4bf0
void SE3StopCombinedMembers(SE3SoundKitObject* object) {
    uint32_t combinedHandle = static_cast<uint32_t>(object->m_combinedHandle);
    SE3COMBINEDNODE* combined = g_se3Combined.Find(combinedHandle, [combinedHandle](SE3COMBINEDNODE* each) {
        return static_cast<uint32_t>(each->m_hashval) == combinedHandle;
    });
    if (combined == nullptr || combined->m_count == 0
        || static_cast<uint32_t>(object->m_handle) != combined->m_handles[0]) {
        return;
    }
    for (uint32_t i = 1; i < combined->m_count; i++) {
        uint32_t member = combined->m_handles[i];
        SE3SoundKitObject* other = SE3GetObject(member);
        if (other == nullptr || 1 < other->m_3dMode || !IsAlive(other)) {
            continue;
        }
        if (other->m_fadeState != 2) {
            SE3Stop(member, 0, 0, nullptr);
        }
    }
}

// Starts the fade out from where a fade in got to.
// ref: FUN_1412c8ff0
void SE3FadeOut(SE3SoundKitObject* object) {
    if (object->m_fading.m_next != nullptr && object->m_fadeState == 2) {
        return;
    }
    const uint8_t* advanced = object->m_def != nullptr ? AdvancedRecord(object->m_def->m_advancedId) : nullptr;
    int32_t elapsed = 0;
    if (object->m_fadeState == 1
        && (advanced == nullptr || DB2FieldU8(advanced, 0x23, 0, &g_soundKitAdvancedDB) != 2)) {
        int32_t inMs = SE3FadeMs(object->m_params.m_fadeInType, object->m_params.m_fadeInMs);
        float done = static_cast<float>(object->m_fadeElapsed) / static_cast<float>(inMs);
        if (done < 0.0f) {
            done = 0.0f;
        } else if (1.0f <= done) {
            done = 1.0f;
        }
        int32_t outMs = SE3FadeMs(object->m_params.m_fadeOutType, object->m_params.m_fadeOutMs);
        elapsed = static_cast<int32_t>((1.0f - done) * static_cast<float>(outMs));
    }
    object->m_fadeElapsed = elapsed;
    object->m_fadeState = 2;
    LinkTail(&g_se3Fading, &object->m_fading, object);
}

// Starts the fade in.
// ref: FUN_1412c8f20
void SE3FadeIn(SE3SoundKitObject* object) {
    object->m_fadeState = 1;
    object->m_fadeElapsed = 0;
    object->m_fadeVolume = 0.0f;
    LinkTail(&g_se3Fading, &object->m_fading, object);
}

// Stops every instance on a bus (0xfe: all) with the fade asked for.
// ref: FUN_1412d4870
void SE3StopGroup(uint8_t bus, bool immediate) {
    SE3StopBus(static_cast<int8_t>(bus), immediate ? 1u : 0u, 0);
}

void SE3StopBus(int8_t bus, uint32_t fadeType, int32_t fadeMs) {
    for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Playing.Next(object);
        if (!SE3BusIsUnder(object->m_params.m_bus, bus)) {
            object = next;
            continue;
        }
        bool stopped = false;
        for (SE3SoundObject& slot : object->m_slots) {
            if (slot.m_channel == nullptr) {
                continue;
            }
            bool playing = false;
            slot.m_channel->isPlaying(&playing);
            if (!playing) {
                continue;
            }
            if (fadeType < 2) {
                if (object->m_slots[0].m_channel != nullptr) {
                    SE3SlotStop(&object->m_slots[0]);
                }
                if (object->m_slots[1].m_channel != nullptr) {
                    SE3SlotStop(&object->m_slots[1]);
                }
            } else {
                object->m_params.m_fadeOutType = static_cast<int32_t>(fadeType);
                if (fadeType == 0xb) {
                    object->m_params.m_fadeOutMs = fadeMs;
                }
                SE3FadeOut(object);
            }
            stopped = true;
            break;
        }
        if (!stopped) {
            if (object->m_3dMode == 1) {
                object->m_playing.Unlink();
                SE3QueueRelease(object);
            }
            if (object->m_3dMode == 2 && 0.0f < static_cast<float>(object->m_retrigger)) {
                object->m_playing.Unlink();
                SE3QueueRelease(object);
            }
        }
        object = next;
    }
    for (SE3SoundKitObject* object = g_se3Pending.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Pending.Next(object);
        if (SE3BusIsUnder(object->m_params.m_bus, bus)) {
            object->m_pending.Unlink();
            SE3QueueRelease(object);
        }
        object = next;
    }
    for (SE3SoundKitObject* object = g_se3Loading.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Loading.Next(object);
        if (SE3BusIsUnder(object->m_params.m_bus, bus)) {
            object->m_loading.Unlink();
            SE3QueueRelease(object);
        }
        object = next;
    }
}

// ---- the cache's events -------------------------------------------------------------------

namespace {

// A slot whose sound went away.
bool ForgetSound(SE3SoundObject* slot, int32_t serial, bool stop) {
    if (slot->m_serial != serial) {
        return false;
    }
    if (stop && slot->m_channel != nullptr) {
        bool playing = false;
        slot->m_channel->isPlaying(&playing);
        if (playing) {
            slot->m_channel->stop();
        }
    }
    slot->m_channel = nullptr;
    if (slot->m_serial != 0) {
        SE3CacheAddUser(slot->m_serial, false);
        slot->m_serial = 0;
    }
    slot->m_entry = -1;
    return true;
}

uint8_t SlotsInUse(const SE3SoundKitObject* object) {
    uint8_t used = object->m_slots[0].m_channel != nullptr ? 1 : 0;
    if (object->m_slots[1].m_channel != nullptr) {
        used++;
    }
    return used;
}

}

// A sound failed to load: the instances still loading it are released.
// ref: FUN_1412c8170
void SE3OnSoundFailed(int32_t serial) {
    for (SE3SoundKitObject* object = g_se3Loading.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Loading.Next(object);
        bool changed = ForgetSound(&object->m_slots[0], serial, false);
        changed = ForgetSound(&object->m_slots[1], serial, false) || changed;
        if (changed && SlotsInUse(object) == 0) {
            object->m_loading.Unlink();
            SE3QueueRelease(object);
        }
        object = next;
    }
}

// A sound was released: every instance using it lets go (stopping it unless `keepPlaying`).
// ref: FUN_1412c7da0
void SE3OnSoundReleased(int32_t serial, bool keepPlaying) {
    for (SE3SoundKitObject* object = g_se3Loading.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Loading.Next(object);
        ForgetSound(&object->m_slots[0], serial, false);
        ForgetSound(&object->m_slots[1], serial, false);
        if (SlotsInUse(object) == 0) {
            object->m_loading.Unlink();
            SE3QueueRelease(object);
        }
        object = next;
    }
    for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Playing.Next(object);
        ForgetSound(&object->m_slots[0], serial, !keepPlaying);
        ForgetSound(&object->m_slots[1], serial, !keepPlaying);
        if (SlotsInUse(object) == 0) {
            object->m_playing.Unlink();
            SE3QueueRelease(object);
        }
        object = next;
    }
    for (SE3SoundKitObject* object = g_se3Pending.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Pending.Next(object);
        ForgetSound(&object->m_slots[0], serial, false);
        ForgetSound(&object->m_slots[1], serial, false);
        if (SlotsInUse(object) == 0) {
            object->m_pending.Unlink();
            SE3QueueRelease(object);
        }
        object = next;
    }
}

// An instance whose sound is ready: it starts next frame (a kit that delays its first play
// waits on the playing list instead).
// ref: FUN_1412cd8f0
void SE3QueueStart(SE3SoundKitObject* object) {
    if (object == nullptr) {
        return;
    }
    if (object->m_def != nullptr) {
        if (const uint8_t* advanced = AdvancedRecord(object->m_def->m_advancedId)) {
            if (DB2FieldU32(advanced, 0xd, 0, &g_soundKitAdvancedDB) != 0 && object->m_params.m_142 == 0) {
                LinkTail(&g_se3Playing, &object->m_playing, object);
                return;
            }
        }
    }
    object->m_params.m_142 = 0;
    LinkTail(&g_se3Pending, &object->m_pending, object);
}

// The cache's event on one of its sounds: 0 failed, 1 released, 2 released while playing on,
// 3 loaded.
// ref: FUN_1412c7c60
void SE3OnCacheEvent(int32_t event, void* sound) {
    auto node = static_cast<SE3CacheNode*>(sound);
    int32_t serial = node->m_serial;
    if (event == 0) {
        SE3OnSoundFailed(serial);
        return;
    }
    if (event == 1) {
        SE3OnSoundReleased(serial, false);
        return;
    }
    if (event == 2) {
        SE3OnSoundReleased(serial, true);
        return;
    }
    if (event != 3 || g_se3ShuttingDown != 0) {
        return;
    }
    for (SE3SoundKitObject* object = g_se3Loading.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Loading.Next(object);
        for (SE3SoundObject& slot : object->m_slots) {
            if (slot.m_serial == serial) {
                object->m_loading.Unlink();
                SE3QueueStart(object);
                break;
            }
        }
        object = next;
    }
}

// The callback an instance (or each member of a combined handle, by its first) runs when its
// sound ends.
// ref: FUN_1412d3920
bool SE3SetOnEnded(uint32_t handle, blz::function_storage* callback, uint64_t argument) {
    if (SE3HANDLENODE* node = g_se3Handles.Find(handle, [handle](SE3HANDLENODE* each) {
            return static_cast<uint32_t>(each->m_hashval) == handle;
        })) {
        if (SE3SoundKitObject* object = node->m_object) {
            if (&object->m_onEnded != callback) {
                object->m_onEnded.m_manager(object->m_onEnded.m_storage, nullptr, 0);
                callback->m_manager(object->m_onEnded.m_storage, callback->m_storage, 1);
                object->m_onEnded.m_invoke = callback->m_invoke;
                object->m_onEnded.m_manager = callback->m_manager;
            }
            std::memcpy(object->m_pad158, &argument, sizeof(argument));
            callback->m_manager(callback->m_storage, nullptr, 0);
            return false;
        }
        callback->m_manager(callback->m_storage, nullptr, 0);
        return false;
    }
    SE3COMBINEDNODE* combined = g_se3Combined.Find(handle, [handle](SE3COMBINEDNODE* each) {
        return static_cast<uint32_t>(each->m_hashval) == handle;
    });
    if (combined != nullptr && combined->m_count != 0) {
        blz::function_storage copy;
        copy.m_manager = &blz::function_empty_manager;
        callback->m_manager(copy.m_storage, callback->m_storage, 1);
        copy.m_invoke = callback->m_invoke;
        copy.m_manager = callback->m_manager;
        bool result = SE3SetOnEnded(combined->m_handles[0], &copy, argument);
        callback->m_manager(callback->m_storage, nullptr, 0);
        return result;
    }
    callback->m_manager(callback->m_storage, nullptr, 0);
    return false;
}

