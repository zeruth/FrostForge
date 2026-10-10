// SE3 (Engine/Source/SoundEngine/SE3.cpp): starting the instances whose sounds are loaded, and
// the instances waiting on a download.

#include "sound/SE3Kit.hpp"

#include <cmath>
#include <cstring>

#include "db/DB2Tables.hpp"
#include "db/ParameterCurve.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "fileservices/AsyncFile.hpp"
#include "fmod.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Bus.hpp"
#include "sound/SE3Internal.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"

namespace {

// The channel reverb sends (FMOD_REVERB_CHANNELPROPERTIES): wet and dry.
const FMOD_REVERB_CHANNELPROPERTIES s_reverbWet = {0, 0, 0x10, nullptr};       // 0x14272a100
const FMOD_REVERB_CHANNELPROPERTIES s_reverbDry = {0, -10000, 0x10, nullptr};  // 0x14272a118

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

float RandomUnit() {
    return (static_cast<float>((static_cast<uint64_t>(SE3Random()) * 0x3e9) >> 32) - 500.0f) * 0.002f;
}

// A random value in [-minus, plus] from a kit's variation fields (in thousandths).
float Variation(float plus, float minus) {
    float span = std::fabs(minus) + std::fabs(plus);
    uint32_t random = SE3Random();
    return static_cast<float>((static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int64_t>(span * 1000.0f))) * random) >> 32)
        * 0.001f - std::fabs(minus);
}

// The first SoundEnvelope of a kit with the given type.
// ref: FUN_1405ce310
const uint8_t* KitEnvelope(int32_t kitId, uint32_t type) {
    WowDB2ParentLookup* envelopes = g_soundEnvelopeDB.m_parentLookup;
    if (envelopes == nullptr) {
        return nullptr;
    }
    DB2PARENTITER it;
    DB2PARENTITER end;
    envelopes->Begin(&it, kitId);
    envelopes->End(&end);
    while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
           || it.m_source != end.m_source) {
        auto record = static_cast<const uint8_t*>(envelopes->Record(&it));
        if (DB2FieldU8(record, 1, 0, &g_soundEnvelopeDB) == type) {
            return record;
        }
        envelopes->Next(&it);
    }
    return nullptr;
}

// A prefetch's priority: its distance (to 10000), behind every urgent one unless it is urgent.
// ref: FUN_1405dbce0
int32_t AsyncPriorityForDistance(uint8_t urgent, float distance) {
    if (10000.0f <= distance) {
        distance = 10000.0f;
    }
    if (distance <= 0.0f) {
        distance = 0.0f;
    }
    return static_cast<int32_t>(std::lround(distance)) + (urgent == 0 ? 10000 : 0);
}

// A loaded instance fails to start: off the pending list, released.
void Fail(SE3SoundKitObject* object) {
    object->m_pending.Unlink();
    SE3QueueRelease(object);
}

// Its volume, pitch, fades, priority, reverb and mix as it starts (FUN_1412d0300's
// LAB_1412d0dda), then unpaused.
bool StartChannel(SE3SoundKitObject* object, uint32_t slotIndex, FMOD::Channel* channel, const uint8_t* kit,
    const uint8_t* advanced, SE3CacheNode* node, bool randomStart) {
    SE3SoundObject* slot = &object->m_slots[slotIndex];
    float pitch = 1.0f;
    if (kit == nullptr) {
        object->m_volume = 1.0f;
        channel->setVolume(SE3SlotVolume(object, static_cast<int32_t>(slotIndex)));
    } else {
        float volumePlus = g_soundKitDB.FieldFloat(kit, 8, 0);
        float volumeMinus = g_soundKitDB.FieldFloat(kit, 9, 0);
        if (std::fabs(volumeMinus) + std::fabs(volumePlus) <= 0.0f) {
            object->m_volume = 1.0f;
        } else {
            object->m_volume = Variation(volumePlus, volumeMinus) + 1.0f;
        }
        channel->setVolume(SE3SlotVolume(object, static_cast<int32_t>(slotIndex)));
        float pitchPlus = g_soundKitDB.FieldFloat(kit, 10, 0);
        float pitchMinus = g_soundKitDB.FieldFloat(kit, 0xb, 0);
        float adjust = g_soundKitDB.FieldFloat(kit, 0xd, 0);
        pitch = adjust + 1.0f;
        if (0.0f < std::fabs(pitchMinus) + std::fabs(pitchPlus)) {
            pitch = Variation(pitchPlus, pitchMinus) + adjust + 1.0f;
        }
    }
    if (2.3841858e-07f <= std::fabs(pitch - 1.0f)) {
        object->m_pitch = pitch;
        SE3ApplyPitch(object, pitch);
    }
    if (advanced != nullptr) {
        uint8_t fadeKind = DB2FieldU8(advanced, 0x23, 0, &g_soundKitAdvancedDB);
        if (fadeKind == 1) {
            int32_t inMin = DB2FieldI32(advanced, 0x24, 0, &g_soundKitAdvancedDB);
            int32_t inMax = DB2FieldI32(advanced, 0x25, 0, &g_soundKitAdvancedDB);
            if (-1 < inMin && -1 < inMax) {
                object->m_params.m_fadeInType = 0xb;
                object->m_params.m_fadeInMs = SE3RandomRange(inMin, inMax);
            }
            uint32_t inCurve = DB2FieldU32(advanced, 0x26, 0, &g_soundKitAdvancedDB);
            object->m_params.m_fadeInCurve = inCurve == 0 ? 0xc57 : static_cast<int32_t>(inCurve);
            int32_t outMin = DB2FieldI32(advanced, 0x27, 0, &g_soundKitAdvancedDB);
            int32_t outMax = DB2FieldI32(advanced, 0x28, 0, &g_soundKitAdvancedDB);
            if (-1 < outMin && -1 < outMax) {
                object->m_params.m_fadeOutType = 0xb;
                object->m_params.m_fadeOutMs = SE3RandomRange(outMin, outMax);
            }
            uint32_t outCurve = DB2FieldU32(advanced, 0x29, 0, &g_soundKitAdvancedDB);
            object->m_params.m_fadeOutCurve = outCurve == 0 ? 0xc58 : static_cast<int32_t>(outCurve);
        } else if (fadeKind == 2 && kit != nullptr && ((DB2FieldU16(kit, 3, 0, &g_soundKitDB) >> 9) & 1) != 0) {
            if (const uint8_t* envelope = KitEnvelope(object->m_def->m_kitId, 0)) {
                uint32_t curve = static_cast<uint32_t>(DB2FieldI32(envelope, 3, 0, &g_soundEnvelopeDB));
                object->m_params.m_fadeOutCurve = static_cast<int32_t>(curve);
                C2VECTORF first = {};
                C2VECTORF last = {};
                ParameterCurveGetPoint(curve, DB2FieldU16(envelope, 6, 0, &g_soundEnvelopeDB), &first);
                ParameterCurveGetLastPoint(curve, &last);
                int32_t ms = static_cast<int32_t>(last.x * 1000.0f) - static_cast<int32_t>(first.x * 1000.0f);
                if (ms < 1) {
                    object->m_params.m_fadeOutType = 1;
                    object->m_params.m_fadeOutMs = 0;
                    object->m_params.m_fadeOutStart = 0.0f;
                } else {
                    object->m_params.m_fadeOutStart = first.x;
                    object->m_params.m_fadeOutType = 0xb;
                    object->m_params.m_fadeOutMs = ms;
                }
            }
        }
    }
    if (object->m_params.m_13f != 0) {
        object->m_params.m_e0 = 0x20;
    }
    int32_t priority = object->m_channelPriority;
    if ((priority != 0x80 && -1 < priority && priority < 0x101) || object->m_priorityBase != 0x80
        || object->m_priorityRange != 0) {
        SE3ApplyPriority(object);
    }
    if (object->m_onStart.m_invoke != nullptr) {
        using INVOKE = void (*)(FMOD::Channel*, int32_t, void*);
        reinterpret_cast<INVOKE>(object->m_onStart.m_invoke)(channel, object->m_onStartArg, object->m_onStart.m_storage);
    }
    FMOD_MODE mode = 0;
    if (channel->getMode(&mode) == FMOD_OK) {
        bool wet = SE3BusAllowsReverb(object->m_params.m_bus) && !((mode & FMOD_2D) != 0 && object->m_12 == 0);
        channel->setReverbProperties(wet ? &s_reverbWet : &s_reverbDry);
        if ((mode & FMOD_2D) == 0) {
            if ((mode & FMOD_SOFTWARE) == 0) {
                channel->setSpeakerMix(0, 0, 0, 0, 0, 0, 0, 0);
            } else {
                channel->setSpeakerMix(0.6f, 0.6f, 0.001f, 0.0f, 0.2f, 0.2f, 0.001f, 0.001f);
            }
        } else if ((mode & FMOD_SOFTWARE) != 0) {
            channel->setSpeakerMix(0.6f, 0.6f, 0.001f, 0.0f, 0.2f, 0.2f, 0.001f, 0.001f);
        }
    }
    FMOD_MODE current = 0;
    channel->getMode(&current);
    channel->setMode(current | 0x400000);
    bool clampFade = randomStart;
    if (object->m_params.m_b8 == 0) {
        if (advanced != nullptr && 0 < DB2FieldI32(advanced, 0x1f, 0, &g_soundKitAdvancedDB)) {
            channel->setPosition(static_cast<unsigned int>(DB2FieldI32(advanced, 0x1f, 0, &g_soundKitAdvancedDB)),
                FMOD_TIMEUNIT_MS);
            clampFade = true;
        }
    } else {
        channel->setPosition(static_cast<unsigned int>(object->m_params.m_b8), FMOD_TIMEUNIT_MS);
        clampFade = true;
    }
    if (clampFade && !SE3BusIsUnder(object->m_params.m_bus, SE3BUS_MUSIC)
        && !SE3BusIsUnder(object->m_params.m_bus, 0x42)) {
        if (SE3FadeMs(object->m_params.m_fadeInType, object->m_params.m_fadeInMs) < 10) {
            object->m_params.m_fadeInType = 0xb;
            object->m_params.m_fadeInMs = 10;
        }
    }
    uint8_t fadeKind = advanced != nullptr ? DB2FieldU8(advanced, 0x23, 0, &g_soundKitAdvancedDB) : 3;
    if (SE3FadeMs(object->m_params.m_fadeInType, object->m_params.m_fadeInMs) == 0 || fadeKind == 2) {
        object->m_fadeVolume = 1.0f;
    } else if (object->m_started == 0 && object->m_fadeState != 2) {
        SE3FadeIn(object);
    }
    if (0 < object->m_params.m_2c) {
        object->m_lifeTimer = object->m_params.m_2c;
        LinkTail(&g_se3Timed, &object->m_timed, object);
    }
    SE3UpdateObject(object, 0, false);
    if ((object->m_mode & FMOD_3D) != 0 && slot->m_dsp[0] == nullptr && SE3PositionalLowPassEnabled()
        && g_fmodSystem->createDSPByType(FMOD_DSP_TYPE_LOWPASS_SIMPLE, &slot->m_dsp[0]) == FMOD_OK) {
        slot->m_channel->addDSP(slot->m_dsp[0], nullptr);
    }
    if ((object->m_mode & FMOD_2D) != 0 && object->m_params.m_140 != 0 && slot->m_dsp[1] == nullptr
        && g_fmodSystem->createDSPByType(FMOD_DSP_TYPE_LOWPASS_SIMPLE, &slot->m_dsp[1]) == FMOD_OK) {
        slot->m_channel->addDSP(slot->m_dsp[1], nullptr);
    }
    if (channel->setPaused(false) != FMOD_OK) {
        for (FMOD::DSP*& dsp : slot->m_dsp) {
            if (dsp != nullptr) {
                dsp->remove();
                dsp->release();
                dsp = nullptr;
            }
        }
        Fail(object);
        return false;
    }
    if (advanced != nullptr && DB2FieldU32(advanced, 0x21, 0, &g_soundKitAdvancedDB) != 0) {
        SE3SoundKitDef* def = object->m_def;
        def->m_34 = SE3RandomRange(static_cast<int32_t>(DB2FieldU32(advanced, 0x20, 0, &g_soundKitAdvancedDB)),
            static_cast<int32_t>(DB2FieldU32(advanced, 0x21, 0, &g_soundKitAdvancedDB)));
        def->m_timerLink.Unlink();
        g_se3DefTimers.LinkToTail(def);
    }
    object->m_pending.Unlink();
    LinkTail(&g_se3Playing, &object->m_playing, object);
    object->m_started = 1;
    if (object->m_3dMode == 1) {
        object->m_retrigger = 0;
    }
    (void)node;
    return true;
}

}

// The pending instances whose sounds are ready: a channel each, placed, set up and unpaused.
// ref: FUN_1412d0300
void SE3StartLoadedInstances() {
    for (SE3SoundKitObject* object = g_se3Pending.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Pending.Next(object);
        SE3SoundKitDef* def = object->m_def;
        const uint8_t* kit = def != nullptr ? SoundKitRecord(def->m_kitId) : nullptr;
        const uint8_t* advanced = def != nullptr ? AdvancedRecord(def->m_advancedId) : nullptr;
        SE3CacheNode* node = nullptr;
        uint32_t slotIndex = 0;
        if (SE3CacheGetSound(object, &node, &slotIndex) != 0) {
            object = next;
            continue;
        }
        unsigned int length = 0;
        node->m_sound->getLength(&length, FMOD_TIMEUNIT_MS);
        object->m_lengthMs = static_cast<int32_t>(length);
        if (def != nullptr && def->m_timerLink.m_next != nullptr) {
            Fail(object);
            object = next;
            continue;
        }
        SE3SoundObject* slot = &object->m_slots[slotIndex];
        if (g_fmodSystem->playSound(FMOD_CHANNEL_FREE, node->m_sound, true, &slot->m_channel) != FMOD_OK) {
            Fail(object);
            object = next;
            continue;
        }
        slot->m_channel->setCallback(&SE3ChannelCallback);
        if (!SE3BusAttach(object->m_params.m_bus, slot->m_channel)) {
            Fail(object);
            object = next;
            continue;
        }
        FMOD::Channel* channel = slot->m_channel;
        channel->setUserData(reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(object->m_handle))));
        if ((object->m_mode & FMOD_3D) != 0) {
            SE3UpdateDistances(object);
            channel->set3DMinMaxDistance(object->m_minDistance, object->m_maxDistance);
            if (object->m_3dMode == 1) {
                object->m_offset[0] = RandomUnit();
                object->m_offset[1] = RandomUnit();
                object->m_offset[2] = RandomUnit();
                float x = object->m_offset[0];
                float y = object->m_offset[1];
                float z = object->m_offset[2];
                float length2 = x * x + y * y + z * z;
                if (2.3841858e-07f < length2) {
                    float scale = 1.0f / std::sqrt(length2);
                    object->m_offset[0] = scale * x;
                    object->m_offset[1] = scale * y;
                    object->m_offset[2] = scale * z;
                }
                uint32_t minimum = 0;
                float extra = 0.0f;
                if (advanced != nullptr) {
                    minimum = DB2FieldU8(advanced, 0x1d, 0, &g_soundKitAdvancedDB);
                    uint16_t maximum = DB2FieldU16(advanced, 0x1e, 0, &g_soundKitAdvancedDB);
                    uint32_t span = (static_cast<uint32_t>(maximum) - minimum) * 1000;
                    if (0 < static_cast<int32_t>(span)) {
                        extra = static_cast<float>((static_cast<uint64_t>(span) * SE3Random()) >> 32) * 0.001f;
                    }
                }
                float ox = object->m_offset[0];
                float oy = object->m_offset[1];
                float oz = object->m_offset[2];
                float scale = (static_cast<float>(minimum) + extra) / std::sqrt(ox * ox + oy * oy + oz * oz);
                object->m_offset[0] = scale * ox;
                object->m_offset[1] = scale * oy;
                object->m_offset[2] = scale * oz;
            }
            FMOD_VECTOR position = {object->m_offset[0] + object->m_position[0], object->m_offset[1] + object->m_position[1],
                object->m_offset[2] + object->m_position[2]};
            channel->set3DAttributes(&position, nullptr);
        }
        bool randomStart = false;
        if (SE3BusIsUnder(object->m_params.m_bus, SE3BUS_MUSIC) || SE3BusIsUnder(object->m_params.m_bus, 0x42)
            || object->m_params.m_137 != 0 || node == nullptr || object->m_3dMode != 0) {
            StartChannel(object, slotIndex, channel, kit, advanced, node, randomStart);
            object = next;
            continue;
        }
        // a looping sound on any other bus starts at a random point
        unsigned int bytes = 0;
        if (node->m_sound->getLength(&bytes, FMOD_TIMEUNIT_PCMBYTES) == FMOD_OK) {
            randomStart = true;
            unsigned int position =
                static_cast<unsigned int>((static_cast<uint64_t>(SE3Random()) * (bytes >> 1)) >> 32) & 0xfffffff8;
            if (channel->setPosition(position, FMOD_TIMEUNIT_PCMBYTES) == FMOD_OK) {
                StartChannel(object, slotIndex, channel, kit, advanced, node, randomStart);
                object = next;
                continue;
            }
        }
        Fail(object);
        object = next;
    }
}

// The instances waiting on a download: the closest prefetch is the most urgent, and a kit
// whose files arrived plays again into its instance.
// ref: FUN_1412cfbd0
void SE3UpdateWaitingInstances() {
    void* closest = nullptr;
    int32_t closestPriority = 0x7fffffff;
    uint32_t now = FrameTime_GetTime();
    for (SE3SoundKitObject* object = g_se3Waiting.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Waiting.Next(object);
        SE3SoundKitDef* def = object->m_def;
        const uint8_t* advanced = AdvancedRecord(def->m_advancedId);
        if (def->m_prefetch != nullptr) {
            float distance = 0.0f;
            if (g_se3ListenerPosition[0] == object->m_position[0] && g_se3ListenerPosition[1] == object->m_position[1]
                && g_se3ListenerPosition[2] == object->m_position[2]) {
                if (SE3BusIsUnder(object->m_params.m_bus, SE3BUS_MUSIC)
                    || SE3BusIsUnder(object->m_params.m_bus, SE3BUS_AMBIENCE)) {
                    distance = 10000.0f;
                }
            } else {
                FMOD_VECTOR listener = {};
                g_fmodSystem->get3DListenerAttributes(0, &listener, nullptr, nullptr, nullptr);
                float dx = object->m_position[0] - listener.x;
                float dy = object->m_position[1] - listener.y;
                float dz = object->m_position[2] - listener.z;
                distance = dy * dy + dx * dx + dz * dz;
            }
            int32_t priority = AsyncPriorityForDistance(object->m_params.m_13d, distance);
            if (priority < closestPriority) {
                closest = def->m_prefetch;
                closestPriority = priority;
            }
            object = next;
            continue;
        }
        object->m_waiting.Unlink();
        if (def->m_3a == 0 || def->m_loadState != 1) {
            if (def->m_loadState == 2) {
                SE3StopSlots(object);
            }
            object = next;
            continue;
        }
        uint32_t waited = now - static_cast<uint32_t>(def->m_prefetchTime);
        if (object->m_params.m_13d != 0 || waited < 500) {
            bool keepEntry = false;
            if (object->m_params.m_entry == -1 && def->m_lastEntry != -1) {
                object->m_params.m_entry = def->m_lastEntry;
                keepEntry = true;
            }
            object->m_params.m_parent = object;  // the instance plays into itself
            object->m_params.m_144 = 1;
            const float* position = object->m_hasPosition != 0 ? object->m_position : nullptr;
            uint32_t result = SE3PlayKit(static_cast<uint32_t>(def->m_kitId), position, &object->m_params, nullptr,
                nullptr, false, nullptr);
            if (result == 0) {
                if (object->m_params.m_bus == 8 && waited < 500 && g_se3Config.m_slot170.m_invoke != nullptr) {
                    using INVOKE = void (*)(int32_t, void*);
                    reinterpret_cast<INVOKE>(g_se3Config.m_slot170.m_invoke)(object->m_handle, g_se3Config.m_slot170.m_storage);
                }
                if (keepEntry) {
                    object->m_params.m_entry = -1;
                }
            } else {
                SE3QueueRelease(object);
            }
            object = next;
            continue;
        }
        if (object->m_3dMode < 2 && advanced != nullptr && DB2FieldU32(advanced, 0xb, 0, &g_soundKitAdvancedDB) != 0) {
            object->m_retrigger = SE3RandomRange(static_cast<int32_t>(DB2FieldU32(advanced, 0xa, 0, &g_soundKitAdvancedDB)),
                static_cast<int32_t>(DB2FieldU32(advanced, 0xb, 0, &g_soundKitAdvancedDB)));
            LinkTail(&g_se3Playing, &object->m_playing, object);
        } else {
            SE3StopSlots(object);
        }
        object = next;
    }
    g_se3ClosestPrefetchPriority = closestPriority;
    g_se3ClosestPrefetch = closest;
}
