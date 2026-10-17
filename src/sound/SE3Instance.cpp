// SE3 (Engine/Source/SoundEngine/SE3.cpp): the frame's passes over the sound kit instances --
// starting the loaded ones, fades, the per-instance update (volume, pitch, priority, cones,
// retriggers, envelopes), the sounds FMOD finished, and the timers.

#include "sound/SE3Kit.hpp"

#include <cmath>
#include <cstring>

#include "base/DataAllocator.hpp"
#include "db/DB2Tables.hpp"
#include "db/ParameterCurve.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "fmod.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Bus.hpp"
#include "sound/SE3Internal.hpp"
#include "storm/thread/SCritSect.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"

int32_t g_se3VoiceOverHandle;           // DAT_1430a94d8
int32_t g_se3VoiceOverPriority = 4;     // DAT_142cfca30
int32_t g_se3VoiceOverKit;              // DAT_1430a94c4
int32_t g_se3ClosestPrefetchPriority;   // DAT_1430a9470
void* g_se3ClosestPrefetch;             // DAT_1430a9468
uint8_t g_se3CheckFilters;              // DAT_1430a9474

namespace {

const char s_soundEndedTag[] = ".?AVSE3SoundEndedMessage@@";
// The fade lengths by type (DAT_142cfcf58): 0xb is the play's own length.
const int32_t s_fadeMs[12] = {-1, 0, 150, 300, 500, 700, 1000, 2000, 3000, 4000, 5000, -1};
// The channel reverb sends (FMOD_REVERB_CHANNELPROPERTIES): wet and dry.
const FMOD_REVERB_CHANNELPROPERTIES s_reverbWet = {0, 0, 0x10, nullptr};       // 0x14272a100
const FMOD_REVERB_CHANNELPROPERTIES s_reverbDry = {0, -10000, 0x10, nullptr};  // 0x14272a118
// The 2D speaker mix of software sounds (DAT_142cfcf88..0x142cfcf94, DAT_1430a96a4).
const float s_mixFront = 0.6f;
const float s_mixCenter = 0.001f;
const float s_mixLfe = 0.0f;
const float s_mixRear = 0.2f;
const float s_mixSide = 0.001f;

// The sound-ended messages FMOD's callback leaves (SE3SoundEndedMessage, 0x20).
struct SOUNDENDEDMSG {
    TSLink<SOUNDENDEDMSG> m_link;
    FMOD::Channel* m_channel;
    int32_t m_handle;
};
CDataAllocator s_soundEndedMessages(sizeof(SOUNDENDEDMSG), 0x20);  // DAT_1430a9508
TSList<SOUNDENDEDMSG, TSGetLink<SOUNDENDEDMSG>, 8> s_soundEnded;    // 0x142cfcef8
SCritSect s_soundEndedLock;             // DAT_142ddcf20

struct LOCKINIT {
    LOCKINIT() {
        s_soundEndedLock.Initialize();
    }
} s_lockInit;

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

// A value smoothed toward a target by a critically damped spring (value, velocity).
// ref: FUN_1413434b0
bool Smooth(float* state, const float* target, float stiffness, float dt, float epsilon) {
    float to = *target;
    float delta = state[0] - to;
    if (epsilon <= 9.9999997e-06f) {
        epsilon = 9.9999997e-06f;
    }
    if (epsilon < std::fabs(delta)) {
        float k = stiffness * dt;
        float damping = 1.0f / (k * k * 0.235f * k + 1.0f + k * k * 0.48f + k);
        float velocity = (delta * stiffness + state[1]) * dt;
        float value = (velocity + delta) * damping + to;
        state[1] = (state[1] - velocity * stiffness) * damping;
        state[0] = value;
        if (0.0f < (value - *target) * delta) {
            return false;
        }
        to = *target;
    }
    state[0] = to;
    state[1] = 0.0f;
    return true;
}

}

int32_t SE3FadeMs(int32_t type, int32_t custom) {
    if (static_cast<uint32_t>(type) < 0xc) {
        return type == 0xb ? custom : s_fadeMs[type];
    }
    return s_fadeMs[1];
}

// ---- volume and pitch ---------------------------------------------------------------------

namespace {

// SoundKit field 2 (volume) times its SoundMixGroup chain's volumes, and the entry's volume.
void KitVolumes(SE3SoundKitObject* object, float* kitVolume, float* mixVolume, float* entryVolume) {
    *kitVolume = 1.0f;
    *mixVolume = 1.0f;
    *entryVolume = 1.0f;
    if (object->m_def == nullptr) {
        return;
    }
    if (const uint8_t* kit = SoundKitRecord(object->m_def->m_kitId)) {
        *kitVolume = g_soundKitDB.FieldFloat(kit, 2, 0);
        bool local;
        auto mix = static_cast<const uint8_t*>(
            g_soundMixGroupDB.GetRecord(DB2FieldU32(kit, 0x10, 0, &g_soundKitDB), false, &local));
        for (int32_t depth = 0; mix != nullptr && depth < 0x14; depth++) {
            *mixVolume *= g_soundMixGroupDB.FieldFloat(mix + 8, 4, 8);
            uint32_t parent = static_cast<uint32_t>(DB2FieldI32(mix + 8, 3, 8, &g_soundMixGroupDB));
            mix = static_cast<const uint8_t*>(g_soundMixGroupDB.GetRecord(parent, false, &local));
        }
    }
    if (const uint8_t* entry = SE3GetKitEntry(object->m_def, object->m_entry)) {
        *entryVolume = g_soundKitEntryDB.FieldFloat(entry, 3, 0);
    }
}

float Clamp01(float value) {
    if (value < 0.0f) {
        return 0.0f;
    }
    return 1.0f <= value ? 1.0f : value;
}

}

// The volume of one slot.
// ref: FUN_1412c9780
float SE3SlotVolume(SE3SoundKitObject* object, int32_t slot) {
    float bus = SE3BusGetObjectVolume(object->m_params.m_bus, static_cast<uint32_t>(object->m_handle));
    float kitVolume;
    float mixVolume;
    float entryVolume;
    KitVolumes(object, &kitVolume, &mixVolume, &entryVolume);
    float volume = object->m_310 * object->m_fadeVolume * object->m_slots[slot].m_fadeVolume * mixVolume * entryVolume
        * kitVolume * object->m_params.m_08 * object->m_volume * object->m_350[0] * object->m_320 * object->m_338 * bus
        * object->m_timeWindow;
    return Clamp01(volume);
}

// Both slots' volumes.
// ref: FUN_1412d6f00
void SE3ApplyVolume(SE3SoundKitObject* object) {
    for (int32_t i = 0; i < 2; i++) {
        SE3SoundObject* slot = &object->m_slots[i];
        if (slot->m_channel == nullptr) {
            continue;
        }
        float bus = SE3BusGetObjectVolume(object->m_params.m_bus, static_cast<uint32_t>(object->m_handle));
        float kitVolume;
        float mixVolume;
        float entryVolume;
        KitVolumes(object, &kitVolume, &mixVolume, &entryVolume);
        float volume = mixVolume * entryVolume * kitVolume * object->m_params.m_08 * object->m_volume * object->m_310
            * object->m_fadeVolume * slot->m_fadeVolume * object->m_350[0] * object->m_320 * object->m_338 * bus
            * object->m_timeWindow;
        slot->m_channel->setVolume(Clamp01(volume));
    }
}

// The channels' priority: closer is more important when the kit asks for a range.
// ref: FUN_1412d6d70
void SE3ApplyPriority(SE3SoundKitObject* object) {
    if (object->m_priorityRange < 1) {
        if (object->m_10 == 0) {
            object->m_channelPriority = object->m_priorityBase;
        } else {
            object->m_channelPriority = 0xff;
        }
    } else {
        if (0.0f < object->m_maxDistance) {
            FMOD_VECTOR listener = {};
            g_fmodSystem->get3DListenerAttributes(0, &listener, nullptr, nullptr, nullptr);
            float dx = listener.x - object->m_position[0];
            float dy = listener.y - object->m_position[1];
            float dz = listener.z - object->m_position[2];
            float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - object->m_minDistance;
            if (distance <= 0.0f) {
                distance = 0.0f;
            }
            float span = object->m_maxDistance - object->m_minDistance;
            float fraction = 0.0f;
            if (1.1920929e-07f < span) {
                fraction = distance / span;
                if (1.0f <= fraction) {
                    fraction = 1.0f;
                }
            }
            float priority = static_cast<float>(object->m_priorityRange) * fraction;
            if (255.0f <= priority) {
                priority = 255.0f;
            }
            priority += static_cast<float>(object->m_priorityBase);
            if (255.0f <= priority) {
                priority = 255.0f;
            }
            object->m_channelPriority = static_cast<int32_t>(priority);
        }
        if (object->m_10 != 0) {
            object->m_channelPriority = 0xff;
        }
    }
    if (object->m_slots[0].m_channel != nullptr) {
        object->m_slots[0].m_channel->setPriority(object->m_channelPriority);
    }
    if (object->m_slots[1].m_channel != nullptr) {
        object->m_slots[1].m_channel->setPriority(object->m_channelPriority);
    }
}

// Both slots' frequency, scaled from their own.
// ref: FUN_1412d6570
void SE3ApplyPitch(SE3SoundKitObject* object, float scale) {
    for (SE3SoundObject& slot : object->m_slots) {
        if (slot.m_channel == nullptr) {
            continue;
        }
        if (slot.m_frequency == 0.0f) {
            slot.m_channel->getFrequency(&slot.m_frequency);
        }
        slot.m_channel->setFrequency(scale * slot.m_frequency);
    }
}

// The instance's distances from its kit and play.
// ref: FUN_1412d6610
void SE3UpdateDistances(SE3SoundKitObject* object) {
    float minimum = SE3GetMinDistance(&object->m_params, object->m_def);
    float maximum = SE3GetMaxDistance(&object->m_params, object->m_def);
    object->m_minDistance = minimum;
    object->m_maxDistance = maximum;
    if (minimum < 0.0f) {
        object->m_minDistance = 0.0f;
        minimum = 0.0f;
    }
    if (maximum < minimum) {
        object->m_maxDistance = minimum;
    }
}

// The rolloff the kit asks for: inverse-ish (0), linear (1), squared linear (2), inverse
// with the play's factor (3).
// ref: FUN_1412c82d0
float SE3RolloffCurve(float minimum, float maximum, float distance, int32_t type, float factor) {
    if (distance <= minimum) {
        return 1.0f;
    }
    if (distance < maximum) {
        if (type == 0) {
            float fade = maximum * 0.9f;
            float value = minimum / ((distance - minimum) * 4.0f + minimum);
            if (fade < distance) {
                value *= 1.0f - (distance - fade) / (maximum - fade);
            }
            return value;
        }
        if (type == 1 || type == 2) {
            float value = 1.0f;
            if (minimum < maximum) {
                value = (maximum - distance) / (maximum - minimum);
            }
            if (type != 1) {
                value *= value;
            }
            return value;
        }
    } else {
        if (type == 0 || type == 1 || type == 2) {
            return 0.0f;
        }
        distance = maximum;
    }
    if (type != 3) {
        return 0.0f;
    }
    return minimum / ((distance - minimum) * factor + minimum);
}

// FMOD's rolloff callback: the channel's instance's curve (type 4, the default, with the
// kit's own 90% fade).
// ref: FUN_1412c8700
float F_CALLBACK SE3Rolloff(FMOD_CHANNEL* channel, float distance) {
    void* data = nullptr;
    if (reinterpret_cast<FMOD::Channel*>(channel)->getUserData(&data) != FMOD_OK) {
        return 0.0f;
    }
    auto handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
    if (handle == 0) {
        return 0.0f;
    }
    SE3SoundKitObject* object = SE3GetObject(handle);
    if (object == nullptr || SE3GetKitDef(object->m_kitId) == nullptr) {
        return 0.0f;
    }
    int32_t type = object->m_params.m_18;
    float factor = object->m_params.m_1c;
    float maximum = object->m_maxDistance;
    float minimum = object->m_minDistance;
    if (type != 4) {
        return SE3RolloffCurve(minimum, maximum, distance, type, factor);
    }
    if (distance <= minimum) {
        return 1.0f;
    }
    if (maximum <= distance) {
        return 0.0f;
    }
    float fade = maximum * 0.9f;
    float value = minimum / ((distance - minimum) * 4.0f + minimum);
    if (fade < distance) {
        value *= 1.0f - (distance - fade) / (maximum - fade);
    }
    return value;
}

// ---- the sounds FMOD finished -------------------------------------------------------------

// FMOD's channel callback: a channel ended (its message is drained by the frame).
// ref: FUN_1412d46c0
FMOD_RESULT F_CALLBACK SE3ChannelCallback(FMOD_CHANNEL* channel, FMOD_CHANNEL_CALLBACKTYPE type, void* data1,
    void* data2) {
    (void)data1;
    (void)data2;
    if (type == FMOD_CHANNEL_CALLBACKTYPE_END && g_se3ShuttingDown == 0) {
        void* data = nullptr;
        reinterpret_cast<FMOD::Channel*>(channel)->getUserData(&data);
        s_soundEndedLock.Enter();
        auto message = static_cast<SOUNDENDEDMSG*>(s_soundEndedMessages.GetData(true, s_soundEndedTag, -2, 1));
        message->m_link.m_prevlink = nullptr;
        message->m_link.m_next = nullptr;
        message->m_channel = reinterpret_cast<FMOD::Channel*>(channel);
        message->m_handle = static_cast<int32_t>(reinterpret_cast<uintptr_t>(data));
        s_soundEnded.LinkToTail(message);
        s_soundEndedLock.Leave();
    }
    return FMOD_OK;
}

// The ended channels: their slots cleared, a voice-over's turn given up, a periodic kit's next
// trigger rolled, and an instance with nothing left released.
// ref: FUN_1412d1bb0
void SE3ProcessSoundEnded() {
    TSList<SOUNDENDEDMSG, TSGetLink<SOUNDENDEDMSG>, 8> messages;
    s_soundEndedLock.Enter();
    while (SOUNDENDEDMSG* message = s_soundEnded.Head()) {
        message->m_link.Unlink();
        messages.LinkToTail(message);
    }
    s_soundEndedLock.Leave();
    while (SOUNDENDEDMSG* message = messages.Head()) {
        FMOD::Channel* channel = message->m_channel;
        int32_t handle = message->m_handle;
        for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr; object = g_se3Playing.Next(object)) {
            if (object->m_handle != handle && object->m_handle != 1) {
                continue;
            }
            bool ended = false;
            for (int32_t i = 0; i < 2; i++) {
                SE3SoundObject* slot = &object->m_slots[i];
                if (slot->m_channel != channel) {
                    continue;
                }
                ended = true;
                SE3SlotStop(slot);
                slot->m_channel = nullptr;
                if (slot->m_serial != 0) {
                    SE3CacheAddUser(slot->m_serial, false);
                    slot->m_serial = 0;
                }
                slot->m_entry = -1;
                bool voiceOver = g_se3VoiceOverHandle != 0
                    && static_cast<uint32_t>(object->m_combinedHandle) == static_cast<uint32_t>(g_se3VoiceOverHandle);
                if (voiceOver) {
                    voiceOver = false;
                    uint32_t combinedHandle = static_cast<uint32_t>(object->m_combinedHandle);
                    SE3COMBINEDNODE* combined = g_se3Combined.Find(combinedHandle, [combinedHandle](SE3COMBINEDNODE* each) {
                        return static_cast<uint32_t>(each->m_hashval) == combinedHandle;
                    });
                    voiceOver = combined != nullptr && combined->m_count != 0
                        && static_cast<uint32_t>(object->m_handle) == combined->m_handles[0];
                }
                if (object->m_handle == g_se3VoiceOverHandle || voiceOver) {
                    g_se3VoiceOverHandle = 0;
                    g_se3VoiceOverPriority = 4;
                    g_se3VoiceOverKit = 0;
                }
            }
            uint8_t inUse = (object->m_slots[0].m_channel != nullptr ? 1 : 0) + (object->m_slots[1].m_channel != nullptr ? 1 : 0);
            if (!ended) {
                if (object->m_handle == handle) {
                    for (SE3SoundObject* old = object->m_oldSlots.Head(); old != nullptr; old = object->m_oldSlots.Next(old)) {
                        if (old->m_channel == channel) {
                            if (old->m_serial != 0) {
                                SE3CacheAddUser(old->m_serial, false);
                                old->m_serial = 0;
                            }
                            old->m_link.Unlink();
                            SMemFree(old, ".?AV?$TSList@VSE3SoundObject@@V?$TSGetLink@VSE3SoundObject@@@@$07@@", -2, 0);
                            break;
                        }
                    }
                }
                continue;
            }
            const uint8_t* advanced = object->m_3dMode == 1 && object->m_def != nullptr
                ? AdvancedRecord(object->m_def->m_advancedId)
                : nullptr;
            if (advanced != nullptr && DB2FieldU32(advanced, 0xb, 0, &g_soundKitAdvancedDB) != 0) {
                object->m_retrigger = SE3RandomRange(static_cast<int32_t>(DB2FieldU32(advanced, 0xa, 0, &g_soundKitAdvancedDB)),
                    static_cast<int32_t>(DB2FieldU32(advanced, 0xb, 0, &g_soundKitAdvancedDB)));
            } else if (inUse == 0 && object->m_pending.m_next == nullptr) {
                object->m_playing.Unlink();
                SE3StopCombinedMembers(object);
                SE3QueueRelease(object);
            }
            if (object->m_onEnded.m_invoke != nullptr) {
                using INVOKE = void (*)(SE3SoundKitObject*, void*);
                reinterpret_cast<INVOKE>(object->m_onEnded.m_invoke)(object, object->m_onEnded.m_storage);
            }
            break;
        }
        message->m_link.Unlink();
        s_soundEndedMessages.PutData(message);
    }
}

void SE3SoundEndedShutdown() {
    s_soundEndedLock.Enter();
    while (SOUNDENDEDMSG* message = s_soundEnded.Head()) {
        message->m_link.Unlink();
        s_soundEndedMessages.PutData(message);
    }
    s_soundEndedMessages.Clear(s_soundEndedTag, -2);
    s_soundEndedLock.Leave();
}

// ---- fades --------------------------------------------------------------------------------

// The volume at a point of a fade: on the kit's curve, or straight in decibels.
// ref: FUN_1412c7810
void SE3ApplyCurve(SE3SoundKitObject* object, int32_t kind, float t, float value) {
    if (kind == 0) {
        float before = object->m_fadeVolume * object->m_310;
        float after = object->m_fadeVolume;
        if (object->m_fadeState == 2) {
            float start = -80.0f;
            if (0.0f < object->m_310) {
                start = std::log10(object->m_310) * 20.0f;
            }
            float end = ParameterCurveEvaluate(static_cast<uint32_t>(object->m_params.m_fadeOutCurve), object->m_params.m_fadeOutStart);
            float scale = 1.0f;
            if (1.1754944e-35f < std::fabs(end - -80.0f)) {
                scale = (start - -80.0f) / (end - -80.0f);
            }
            after = std::pow(10.0f, scale * (value - end) * 0.05f);
            object->m_fadeVolume = after;
        } else {
            object->m_310 = std::pow(10.0f, value * 0.05f);
        }
        if (9.9999997e-05f < before && after * object->m_310 <= 9.9999997e-05f) {
            SE3Stop(static_cast<uint32_t>(object->m_handle), 1, 0, nullptr);
        }
    } else if (kind == 1) {
        object->m_pitchCurve = std::exp(value * 0.0578f);
        SE3ApplyPitch(object, object->m_324 * object->m_pitch * object->m_pitchCurve);
    }
    (void)t;
}

// One step of a fade: done when its time is up.
// ref: FUN_1412d0030
bool SE3StepFade(SE3SoundKitObject* object, int32_t elapsed) {
    int32_t length = object->m_fadeState == 1 ? SE3FadeMs(object->m_params.m_fadeInType, object->m_params.m_fadeInMs)
                                              : SE3FadeMs(object->m_params.m_fadeOutType, object->m_params.m_fadeOutMs);
    if (0 < length) {
        int32_t done = elapsed + object->m_fadeElapsed;
        object->m_fadeElapsed = done;
        int32_t curve = object->m_fadeState == 1 ? object->m_params.m_fadeInCurve : object->m_params.m_fadeOutCurve;
        float t = static_cast<float>(done);
        const uint8_t* advanced = object->m_def != nullptr ? AdvancedRecord(object->m_def->m_advancedId) : nullptr;
        uint8_t kind = advanced != nullptr ? DB2FieldU8(advanced, 0x23, 0, &g_soundKitAdvancedDB) : 0;
        if (kind == 2) {
            t *= 0.001f;
        } else {
            t /= static_cast<float>(length);
        }
        float start = object->m_fadeState == 2 ? object->m_params.m_fadeOutStart : 0.0f;
        float value = ParameterCurveEvaluate(static_cast<uint32_t>(curve), start + t);
        if (kind == 2) {
            SE3ApplyCurve(object, 0, start + t, value);
        } else {
            object->m_fadeVolume = std::pow(10.0f, value * 0.05f);
        }
    }
    if (object->m_fadeElapsed < length) {
        return false;
    }
    object->m_fadeVolume = object->m_fadeState == 1 ? 1.0f : 0.0f;
    object->m_fadeElapsed = 0;
    return true;
}

// The fading instances: a finished fade in stops fading, a finished fade out stops the sound.
// ref: FUN_1412d01b0
void SE3UpdateFades(int32_t elapsed) {
    for (SE3SoundKitObject* object = g_se3Fading.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Fading.Next(object);
        if (SE3StepFade(object, elapsed)) {
            if (object->m_fadeState == 1) {
                object->m_fading.Unlink();
            } else if (object->m_fadeState == 2) {
                object->m_fading.Unlink();
                SE3StopSlots(object);
            }
            object->m_fadeState = 0;
        }
        SE3ApplyVolume(object);
        object = next;
    }
}
