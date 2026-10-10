// SE3 (Engine/Source/SoundEngine/SE3.cpp): making sound kit instances, playing a kit, and
// stopping one.

#include "sound/SE3Kit.hpp"

#include <cstring>
#include <new>

#include "base/ObjectAlloc.hpp"
#include "cas/CAS.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "fileservices/AsyncFile.hpp"
#include "fmod.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Bus.hpp"
#include "sound/SE3Internal.hpp"
#include "storm/Memory.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"

namespace {

const char s_soundObjectTag[] = ".?AV?$TSList@VSE3SoundObject@@V?$TSGetLink@VSE3SoundObject@@@@$07@@";

const uint8_t* SoundKitRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

const uint8_t* AdvancedRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitAdvancedDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

void LinkTail(SE3KITLIST* list, TSLink<SE3SoundKitObject>* link, SE3SoundKitObject* object) {
    link->Unlink();
    list->LinkToTail(object);
}

}

// ---- the sound objects --------------------------------------------------------------------

// Stops a slot: its DSPs released, its channel stopped while it plays.
// ref: FUN_1412d47c0
void SE3SlotStop(SE3SoundObject* slot) {
    for (FMOD::DSP*& dsp : slot->m_dsp) {
        if (dsp != nullptr) {
            dsp->remove();
            dsp->release();
            dsp = nullptr;
        }
    }
    if (slot->m_channel != nullptr) {
        bool playing = false;
        slot->m_channel->isPlaying(&playing);
        if (playing) {
            slot->m_channel->stop();
        }
    }
}

namespace {

// A retired slot leaves its sound's users and its list.
// ref: FUN_1412c6c70
void SoundObjectDestroy(SE3SoundObject* slot) {
    if (slot->m_serial != 0) {
        SE3CacheAddUser(slot->m_serial, false);
        slot->m_serial = 0;
    }
    slot->m_link.Unlink();
}

void ClearSlot(SE3SoundObject* slot) {
    slot->m_link.m_prevlink = nullptr;
    slot->m_link.m_next = nullptr;
    slot->m_serial = 0;
    slot->m_channel = nullptr;
    slot->m_frequency = 0.0f;
    slot->m_fade = 0;
    slot->m_fadeVolume = 1.0f;
    for (FMOD::DSP*& dsp : slot->m_dsp) {
        dsp = nullptr;
    }
    slot->m_entry = -1;
}

// The instance's constructor.
// ref: FUN_1412c5980
SE3SoundKitObject* ObjectConstruct(SE3SoundKitObject* object, SE3SoundKitDef* def, int8_t bus, const uint32_t* context) {
    std::memset(object, 0, offsetof(SE3SoundKitObject, m_objectIndex));
    object->m_12 = 1;
    object->m_slots[0].m_fadeVolume = 1.0f;
    object->m_slots[0].m_entry = -1;
    object->m_slots[1].m_fadeVolume = 1.0f;
    object->m_slots[1].m_entry = -1;
    new (&object->m_oldSlots) TSList<SE3SoundObject, TSGetLink<SE3SoundObject>, 8>();
    object->m_def = def;
    object->m_handle = ++g_se3LastHandle;
    object->m_fadeVolume = 1.0f;
    object->m_volume = 1.0f;
    object->m_onEnded.m_manager = &blz::function_empty_manager;
    object->m_channelPriority = 0x80;
    object->m_priorityBase = 0x80;
    object->m_pitch = 1.0f;
    object->m_pitchCurve = 1.0f;
    object->m_onStart.m_manager = &blz::function_empty_manager;
    SE3PlayParamsConstruct(&object->m_params);
    object->m_320 = 1.0f;
    object->m_324 = 1.0f;
    object->m_338 = 1.0f;
    object->m_33c = 20000.0f;
    object->m_timeWindow = 1.0f;
    object->m_310 = 1.0f;
    if (def != nullptr) {
        def->m_instances.LinkToTail(object);
    }
    object->m_kitId = def != nullptr ? def->m_kitId : 0;
    std::memcpy(object->m_context, context, sizeof(object->m_context));
    SE3PlayParamsInit(&object->m_params);
    object->m_params.m_bus = bus;
    SE3BusLinkObject(object);
    object->m_340[0] = object->m_338;
    object->m_348[0] = object->m_33c;
    object->m_340[1] = 0.0f;
    object->m_348[1] = 0.0f;
    bool quiet = false;
    if (g_se3Config.m_slot1f0.m_invoke != nullptr) {
        using INVOKE = bool (*)(SE3SoundKitDef*, void*);
        quiet = reinterpret_cast<INVOKE>(g_se3Config.m_slot1f0.m_invoke)(def, g_se3Config.m_slot1f0.m_storage);
    }
    object->m_350[0] = quiet ? 0.0f : 1.0f;
    object->m_350[1] = 0.0f;
    object->m_328[0] = object->m_320;
    object->m_330[0] = object->m_324;
    object->m_328[1] = 0.0f;
    object->m_330[1] = 0.0f;
    return object;
}

}

// The instance's destructor: its slots stopped and released, off every list.
// ref: FUN_1412c6620
void SE3ObjectDestroy(SE3SoundKitObject* object) {
    for (SE3SoundObject& slot : object->m_slots) {
        for (FMOD::DSP*& dsp : slot.m_dsp) {
            if (dsp != nullptr) {
                dsp->remove();
                dsp->release();
                dsp = nullptr;
            }
        }
    }
    SE3SuppressorsForget(object->m_handle);
    while (SE3SoundObject* old = object->m_oldSlots.Head()) {
        SoundObjectDestroy(old);
        SMemFree(old, s_soundObjectTag, -2, 0);
    }
    object->m_defLink.Unlink();
    SE3BusUnlinkObject(object);
    object->m_timed.Unlink();
    object->m_defLink.Unlink();
    object->m_bus.Unlink();
    object->m_waiting.Unlink();
    object->m_398.Unlink();
    object->m_fading.Unlink();
    object->m_release.Unlink();
    object->m_pending.Unlink();
    object->m_loading.Unlink();
    SE3PlayParamsDestroy(&object->m_params);
    object->m_onStart.m_manager(object->m_onStart.m_storage, nullptr, 0);
    object->m_onEnded.m_manager(object->m_onEnded.m_storage, nullptr, 0);
    object->m_oldSlots.~TSList();
    for (SE3SoundObject& slot : object->m_slots) {
        if (slot.m_serial != 0) {
            SE3CacheAddUser(slot.m_serial, false);
            slot.m_serial = 0;
        }
        slot.m_link.Unlink();
    }
    object->m_playing.Unlink();
}

// Frees an instance (the object heap).
void SE3ObjectFree(SE3SoundKitObject* object) {
    SE3ObjectDestroy(object);
    g_soundKitHeap->Free(object);
}

// An instance with a handle: the handle table's node, the instance on the object heap.
// ref: FUN_1412c8620
SE3SoundKitObject* SE3CreateObject(int32_t* handle, SE3PLAYPARAMS* params, SE3SoundKitDef* def) {
    int32_t next = g_se3LastHandle + 1;
    SE3HANDLENODE* node = g_se3Handles.New(static_cast<uint32_t>(next), 0, 0);
    node->m_hashval = static_cast<uint32_t>(next);
    auto object = static_cast<SE3SoundKitObject*>(g_soundKitHeap->Alloc());
    object = ObjectConstruct(object, def, params->m_bus, reinterpret_cast<const uint32_t*>(params->m_a8));
    node->m_object = object;
    SE3PlayParamsCopy(&object->m_params, params);
    if (handle != nullptr) {
        *handle = node->m_object->m_handle;
    }
    node->m_object->m_self = reinterpret_cast<SE3SoundKitObject*>(node);
    return node->m_object;
}

// The instance's distances from its kit, the play's overrides capped by the kit.
// ref: FUN_1412c9d00
float SE3GetMinDistance(const SE3PLAYPARAMS* params, const SE3SoundKitDef* def) {
    const uint8_t* kit = SoundKitRecord(def->m_kitId);
    if (kit == nullptr) {
        return 0.0f;
    }
    float kitValue = g_soundKitDB.FieldFloat(kit, 4, 0);
    if (0.0f < params->m_minDistance && params->m_minDistance < kitValue) {
        return params->m_minDistance;
    }
    return kitValue;
}

// ref: FUN_1412c9c80
float SE3GetMaxDistance(const SE3PLAYPARAMS* params, const SE3SoundKitDef* def) {
    const uint8_t* kit = SoundKitRecord(def->m_kitId);
    if (kit == nullptr) {
        return 0.0f;
    }
    float kitValue = g_soundKitDB.FieldFloat(kit, 5, 0);
    if (0.0f < params->m_maxDistance && params->m_maxDistance < kitValue) {
        return params->m_maxDistance;
    }
    return kitValue;
}

// Whether a position is out of `range` of the listener.
// ref: FUN_1412cd430
bool SE3IsOutOfRange(const float* position, float range) {
    FMOD_VECTOR listener = {};
    g_fmodSystem->get3DListenerAttributes(0, &listener, nullptr, nullptr, nullptr);
    float dx = listener.x - position[0];
    float dy = listener.y - position[1];
    float dz = listener.z - position[2];
    return range * range < dx * dx + dy * dy + dz * dz;
}

// The instance's playback settings from the play: fades, position, 3D mode, the start
// callback, the kit's delays, and whether its envelopes run.
// ref: FUN_1412d3470
void SE3SetupObject(SE3SoundKitObject* object, const float* position, SE3PLAYPARAMS* params, uint32_t mode) {
    object->m_params.m_fadeInType = params->m_fadeInType;
    object->m_params.m_fadeInMs = params->m_fadeInMs;
    object->m_params.m_fadeOutType = params->m_fadeOutType;
    object->m_params.m_fadeOutMs = params->m_fadeOutMs;
    if (1 < static_cast<uint32_t>(params->m_fadeInType) && params->m_142 == 0) {
        object->m_fadeVolume = 0.0f;
    }
    if (params->m_parent == nullptr) {
        if (position != nullptr) {
            object->m_hasPosition = 1;
            std::memcpy(object->m_position, position, sizeof(object->m_position));
        } else {
            object->m_hasPosition = 0;
            std::memset(object->m_position, 0, sizeof(object->m_position));
        }
    } else if (params->m_parent->m_hasPosition == 0) {
        object->m_hasPosition = 0;
        std::memset(object->m_position, 0, sizeof(object->m_position));
    } else {
        object->m_hasPosition = 1;
        std::memcpy(object->m_position, params->m_parent->m_position, sizeof(object->m_position));
    }
    std::memcpy(object->m_params.m_7c, params->m_7c, sizeof(params->m_7c));
    object->m_mode = mode;
    const uint8_t* kit = object->m_def != nullptr ? SoundKitRecord(object->m_def->m_kitId) : nullptr;
    const uint8_t* advanced = object->m_def != nullptr ? AdvancedRecord(object->m_def->m_advancedId) : nullptr;
    uint8_t mode3d;
    if (params->m_3dMode == 0 && object->m_def != nullptr) {
        if (advanced == nullptr) {
            mode3d = static_cast<uint8_t>((SE3KitIsLooping(object->m_def->m_kitId) ^ 1) * 2);
        } else {
            mode3d = DB2FieldU8(advanced, 9, 0, &g_soundKitAdvancedDB);
        }
    } else {
        mode3d = params->m_3dMode == 1 ? 0 : 2;
    }
    object->m_3dMode = mode3d;
    if (params->m_13a != 0) {
        blz::function_storage callback;
        callback.m_manager = &blz::function_empty_manager;
        callback.m_invoke = nullptr;
        params->m_48.m_manager(callback.m_storage, params->m_48.m_storage, 1);
        callback.m_invoke = params->m_48.m_invoke;
        callback.m_manager = params->m_48.m_manager;
        SE3SetOnStart(object, &callback, params->m_68);
    }
    object->m_12 = params->m_136;
    if (advanced == nullptr) {
        goto envelopes;
    }
    if (params->m_parent == nullptr || (object->m_3dMode == 2 && params->m_144 != 0)) {
        if (DB2FieldU32(advanced, 0xd, 0, &g_soundKitAdvancedDB) != 0) {
            object->m_delay = SE3RandomRange(static_cast<int32_t>(DB2FieldU32(advanced, 0xc, 0, &g_soundKitAdvancedDB)),
                static_cast<int32_t>(DB2FieldU32(advanced, 0xd, 0, &g_soundKitAdvancedDB)));
            SE3PLAYPARAMS* own = &object->m_params;
            if (kit != nullptr && (DB2FieldU16(kit, 3, 0, &g_soundKitDB) & 2) != 0
                && (own->m_108[0] != 0 || own->m_108[1] != 0) && own->m_e8.m_invoke != nullptr) {
                using INVOKE = float (*)(uint64_t*, void*);
                float scale = reinterpret_cast<INVOKE>(own->m_e8.m_invoke)(own->m_108, own->m_e8.m_storage);
                object->m_delay = static_cast<int32_t>(static_cast<float>(object->m_delay) * scale);
            }
            object->m_retrigger = object->m_delay;
            goto envelopes;
        }
    } else if (2 <= object->m_3dMode) {
        goto envelopes;
    }
    if (DB2FieldU32(advanced, 0xb, 0, &g_soundKitAdvancedDB) != 0) {
        object->m_retrigger = SE3RandomRange(static_cast<int32_t>(DB2FieldU32(advanced, 0xa, 0, &g_soundKitAdvancedDB)),
            static_cast<int32_t>(DB2FieldU32(advanced, 0xb, 0, &g_soundKitAdvancedDB)));
    }
envelopes:
    if (object->m_def != nullptr && object->m_envelopesDone == 0) {
        WowDB2ParentLookup* envelopes = g_soundEnvelopeDB.m_parentLookup;
        if (envelopes != nullptr) {
            DB2PARENTITER it;
            DB2PARENTITER end;
            envelopes->Begin(&it, object->m_def->m_kitId);
            envelopes->End(&end);
            while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
                   || it.m_source != end.m_source) {
                auto record = static_cast<const uint8_t*>(envelopes->Record(&it));
                if ((DB2FieldU32(record, 2, 0, &g_soundEnvelopeDB) & 1) != 0 && object->m_3dMode == 0) {
                    object->m_envelopesDone = 1;
                    break;
                }
                if (DB2FieldU8(record, 1, 0, &g_soundEnvelopeDB) == 0) {
                    object->m_310 = 0.0f;
                }
                envelopes->Next(&it);
            }
        }
    }
    object->m_priority = params->m_priority;
}

// The start callback: kept when it is set and asked for.
// ref: FUN_1412dbbf0
void SE3SetOnStart(SE3SoundKitObject* object, blz::function_storage* callback, int32_t argument) {
    if (object != nullptr && callback->m_invoke != nullptr && argument != 0) {
        if (&object->m_onStart != callback) {
            object->m_onStart.m_manager(object->m_onStart.m_storage, nullptr, 0);
            callback->m_manager(object->m_onStart.m_storage, callback->m_storage, 1);
            object->m_onStart.m_invoke = callback->m_invoke;
            object->m_onStart.m_manager = callback->m_manager;
        }
        object->m_onStartArg = argument;
    }
    callback->m_manager(callback->m_storage, nullptr, 0);
}

// Moves the playing sound to slot 1 (a crossfade) or retires slot 1, and puts the new sound
// in slot 0.
// ref: FUN_1412cdb40
void SE3SetSlotSound(SE3SoundKitObject* object, int32_t serial, int32_t entry) {
    if (object == nullptr) {
        return;
    }
    SE3SoundObject* current = &object->m_slots[0];
    SE3SoundObject* previous = &object->m_slots[1];
    bool fading = previous->m_channel != nullptr;
    if (fading) {
        auto old = static_cast<SE3SoundObject*>(SMemAlloc(sizeof(SE3SoundObject), 8, s_soundObjectTag, 0, -2, 8));
        ClearSlot(old);
        object->m_oldSlots.LinkToTail(old);
        old->m_serial = previous->m_serial;
        old->m_channel = previous->m_channel;
        old->m_frequency = previous->m_frequency;
        old->m_fade = previous->m_fade;
        old->m_fadeVolume = previous->m_fadeVolume;
        std::memcpy(old->m_dsp, previous->m_dsp, sizeof(old->m_dsp));
        old->m_entry = previous->m_entry;
        SE3SlotStop(previous);
        previous->m_channel = nullptr;
        previous->m_fade = 0;
        previous->m_fadeVolume = 1.0f;
        previous->m_serial = 0;
        previous->m_frequency = 0.0f;
        std::memset(previous->m_dsp, 0, sizeof(previous->m_dsp));
        previous->m_entry = -1;
    }
    if (current->m_channel != nullptr) {
        fading = true;
        for (FMOD::DSP*& dsp : previous->m_dsp) {
            if (dsp != nullptr) {
                dsp->remove();
                dsp->release();
                dsp = nullptr;
            }
        }
        if (previous->m_channel != nullptr) {
            bool playing = false;
            previous->m_channel->isPlaying(&playing);
            if (playing) {
                previous->m_channel->stop();
            }
        }
        previous->m_serial = current->m_serial;
        previous->m_channel = current->m_channel;
        previous->m_frequency = current->m_frequency;
        previous->m_fade = current->m_fade;
        previous->m_fadeVolume = current->m_fadeVolume;
        std::memcpy(previous->m_dsp, current->m_dsp, sizeof(previous->m_dsp));
        previous->m_entry = current->m_entry;
        current->m_fade = 0;
        current->m_fadeVolume = 1.0f;
        current->m_serial = 0;
        current->m_frequency = 0.0f;
        std::memset(current->m_dsp, 0, sizeof(current->m_dsp));
        current->m_entry = -1;
        if (object->m_3dMode == 0) {
            previous->m_fade = 2;
        }
    }
    if (object->m_3dMode == 0) {
        current->m_fade = 1;
        if (fading) {
            current->m_fadeVolume = 0.0f;
        }
    }
    current->m_channel = nullptr;
    if (current->m_serial != serial) {
        if (current->m_serial != 0) {
            SE3CacheAddUser(current->m_serial, false);
        }
        current->m_serial = serial;
        if (serial != 0) {
            SE3CacheAddUser(serial, true);
        }
    }
    current->m_entry = entry;
}

// Puts an instance on the release list (unless it is there).
// ref: FUN_1412c88b0
void SE3QueueRelease(SE3SoundKitObject* object) {
    if (object != nullptr) {
        SE3SlotStop(&object->m_slots[0]);
        SE3SlotStop(&object->m_slots[1]);
        if (object->m_release.m_next == nullptr) {
            g_se3Release.LinkToTail(object);
        }
    }
}

// The released instances: their handles dropped and their objects freed.
// ref: FUN_1412cfb10
void SE3ReleaseFinishedInstances() {
    for (SE3SoundKitObject* object = g_se3Release.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Release.Next(object);
        auto node = reinterpret_cast<SE3HANDLENODE*>(object->m_self);
        g_se3Handles.Unlink(node);
        g_se3Handles.Delete(node);
        object->m_self = nullptr;
        SE3ObjectFree(object);
        object = next;
    }
}

// ---- picking a variation ------------------------------------------------------------------

// The kit entry to play: the asked-for one, the next in order, or a weighted random one that
// does not repeat the last (bounded by SoundPerf_VariationCap).
// ref: FUN_1412d2740
void SE3PickEntry(SE3SoundKitDef* def, SE3PLAYPARAMS* params, int32_t* entry) {
    *entry = -1;
    uint32_t order = static_cast<uint32_t>(params->m_10);
    int32_t count = def->m_entryCount;
    if (2 < order || count < 1) {
        return;
    }
    if (params->m_entry != -1) {
        int32_t wanted = params->m_entry < count - 1 ? params->m_entry : count - 1;
        *entry = wanted;
        def->m_lastEntry = wanted;
        return;
    }
    if (count == 1) {
        *entry = 0;
        def->m_lastEntry = 0;
        return;
    }
    int32_t cap = SE3VariationCap();
    if (cap <= count) {
        count = cap;
    }
    int32_t limit = count < 0x40 ? count : 0x40;
    if (order == 0) {
        if (def->m_lastEntry < limit - 1) {
            def->m_lastEntry++;
        } else {
            def->m_lastEntry = 0;
        }
        *entry = def->m_lastEntry;
        def->m_entryWeights[def->m_lastEntry] = 0;
        return;
    }
    for (int32_t pass = 0;; pass++) {
        if (pass == 2) {
            def->m_lastEntry = -1;
        } else if (2 < pass) {
            if (def->m_entryWeights != nullptr) {
                std::memset(def->m_entryWeights, 0, static_cast<size_t>(def->m_entryCount));
            }
            *entry = 0;
            def->m_lastEntry = 0;
            return;
        }
        uint32_t total = 0;
        for (int32_t i = 0; i < limit; i++) {
            if (i != def->m_lastEntry) {
                total += def->m_entryWeights[i];
            }
        }
        if (total == 0) {
            // the weights are spent: refill them from the entries' Frequency and go again
            WowDB2ParentLookup* entries = g_soundKitEntryDB.m_parentLookup;
            if (entries != nullptr) {
                DB2PARENTITER it;
                DB2PARENTITER end;
                entries->Begin(&it, def->m_kitId);
                entries->End(&end);
                for (int32_t i = 0; i < limit; i++) {
                    if (it.m_entry == end.m_entry && it.m_overflow == end.m_overflow && it.m_parent == end.m_parent
                        && it.m_source == end.m_source) {
                        break;
                    }
                    auto record = static_cast<const uint8_t*>(entries->Record(&it));
                    def->m_entryWeights[i] = DB2FieldU8(record, 2, 0, &g_soundKitEntryDB);
                    entries->Next(&it);
                }
            }
            continue;
        }
        int32_t pick = static_cast<int32_t>((static_cast<uint64_t>(SE3Random()) * total) >> 32);
        for (int32_t i = 0; i < limit; i++) {
            if (i != def->m_lastEntry) {
                pick -= def->m_entryWeights[i];
            }
            if (pick < 0) {
                def->m_entryWeights[i] = 0;
                *entry = i;
                def->m_lastEntry = i;
                if (order == 1 && def->m_entryWeights != nullptr) {
                    std::memset(def->m_entryWeights, 0, static_cast<size_t>(def->m_entryCount));
                }
                return;
            }
        }
    }
}
