// SE3 (Engine/Source/SoundEngine/SE3.cpp): each playing instance's frame (FUN_1412d5290), its
// envelopes, and the rest of the engine's frame over the instances.

#include "sound/SE3Kit.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <new>

#include "db/DB2Tables.hpp"
#include "db/ParameterCurve.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "fmod.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Bus.hpp"
#include "sound/SE3Internal.hpp"
#include "base/DataAllocator.hpp"
#include "storm/Memory.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

float g_se3ListenerPosition[3];         // DAT_1430a9698

namespace {

const uint8_t* SoundKitRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

const uint8_t* AdvancedRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitAdvancedDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

uint32_t AdvancedU32(const uint8_t* advanced, uint32_t field) {
    return DB2FieldU32(advanced, field, 0, &g_soundKitAdvancedDB);
}

float AdvancedFloat(const uint8_t* advanced, uint32_t field) {
    return g_soundKitAdvancedDB.FieldFloat(advanced, field, 0);
}

// A spring toward a target (FUN_1413434b0, kept with the instance code).
bool Smooth(float* state, const float* target, float stiffness, float dt) {
    float to = *target;
    float delta = state[0] - to;
    if (9.9999997e-06f < std::fabs(delta)) {
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

float CallFloatSlot(blz::function_storage* slot, SE3SoundKitDef* def) {
    using INVOKE = float (*)(SE3SoundKitDef*, void*);
    return reinterpret_cast<INVOKE>(slot->m_invoke)(def, slot->m_storage);
}

// The game's time window of a kit (SoundKitAdvanced fields 4..7, in ms of the period).
// ref: FUN_1412c9f40
int32_t TimeB(const SE3SoundKitDef* def) {
    const uint8_t* advanced = AdvancedRecord(def->m_advancedId);
    if (advanced == nullptr) {
        return 0;
    }
    uint32_t b = AdvancedU32(advanced, 5);
    if (b < AdvancedU32(advanced, 4)) {
        return static_cast<int32_t>(b) + g_se3Period;
    }
    return static_cast<int32_t>(b);
}

// ref: FUN_1412c9ff0
int32_t TimeC(const SE3SoundKitDef* def) {
    const uint8_t* advanced = AdvancedRecord(def->m_advancedId);
    if (advanced == nullptr) {
        return 0;
    }
    uint32_t c = AdvancedU32(advanced, 6);
    if (c < static_cast<uint32_t>(TimeB(def))) {
        return static_cast<int32_t>(c) + g_se3Period;
    }
    return static_cast<int32_t>(c);
}

// ref: FUN_1412ca0a0
int32_t TimeD(const SE3SoundKitDef* def) {
    const uint8_t* advanced = AdvancedRecord(def->m_advancedId);
    if (advanced == nullptr) {
        return 0;
    }
    uint32_t d = AdvancedU32(advanced, 7);
    if (d < static_cast<uint32_t>(TimeC(def))) {
        return static_cast<int32_t>(d) + g_se3Period;
    }
    return static_cast<int32_t>(d);
}

// The kit's fade kind (SoundKitAdvanced field 0x23).
// ref: FUN_1412ca160
uint8_t FadeKind(const SE3SoundKitObject* object) {
    if (object->m_def != nullptr) {
        if (const uint8_t* advanced = AdvancedRecord(object->m_def->m_advancedId)) {
            return DB2FieldU8(advanced, 0x23, 0, &g_soundKitAdvancedDB);
        }
    }
    return 0;
}

// One SoundEnvelope of a playing instance: its curve followed from the instance's start (and
// looped between its sustain points, or played out from its release when fading).
// ref: FUN_1412d60d0
void FollowEnvelope(SE3SoundKitObject* object, const uint8_t* envelope) {
    if (object->m_def == nullptr || object->m_def->m_envelopeBusy != 0 || object->m_envelopesDone != 0) {
        return;
    }
    uint32_t curve = static_cast<uint32_t>(DB2FieldI32(envelope, 3, 0, &g_soundEnvelopeDB));
    if (!g_curveDB.HasRecord(static_cast<int32_t>(curve))) {
        return;
    }
    if (object->m_elapsed < object->m_delay) {
        return;
    }
    uint32_t elapsed = static_cast<uint32_t>(object->m_elapsed - object->m_delay);
    uint8_t kind = DB2FieldU8(envelope, 1, 0, &g_soundEnvelopeDB);
    int32_t points = static_cast<int32_t>(ParameterCurveGetPointCount(curve));
    float t;
    if (!SE3KitIsLooping(object->m_def->m_kitId)) {
        t = 0.0f;
        if ((DB2FieldU32(envelope, 2, 0, &g_soundEnvelopeDB) & 1) == 0) {
            t = static_cast<float>(elapsed) * 0.001f;
        } else if (0 < object->m_lengthMs) {
            t = static_cast<float>(elapsed) / static_cast<float>(object->m_lengthMs);
        }
    } else {
        bool fromSustain = object->m_fadeState != 2;
        if (!fromSustain) {
            if (kind == 1) {
                fromSustain = DB2FieldU16(envelope, 6, 0, &g_soundEnvelopeDB) == static_cast<uint16_t>(points - 1);
            } else if (kind == 0) {
                return;
            }
        }
        if (!fromSustain) {
            C2VECTORF release = {};
            if (!ParameterCurveGetPoint(curve, DB2FieldU16(envelope, 6, 0, &g_soundEnvelopeDB), &release)) {
                return;
            }
            t = static_cast<float>(object->m_fadeElapsed) * 0.001f + release.x;
        } else {
            C2VECTORF sustain = {};
            ParameterCurveGetPoint(curve, DB2FieldU16(envelope, 5, 0, &g_soundEnvelopeDB), &sustain);
            uint32_t sustainMs = static_cast<uint32_t>(static_cast<int64_t>(sustain.x * 1000.0f));
            if (elapsed < sustainMs) {
                t = static_cast<float>(elapsed) * 0.001f;
            } else {
                C2VECTORF loopEnd = {};
                ParameterCurveGetPoint(curve, DB2FieldU16(envelope, 6, 0, &g_soundEnvelopeDB), &loopEnd);
                uint32_t loop = static_cast<uint32_t>(static_cast<int64_t>(loopEnd.x * 1000.0f)) - sustainMs;
                if (loop == 0) {
                    object->m_envelopesDone = 1;
                    return;
                }
                t = static_cast<float>((elapsed - sustainMs) % loop) * 0.001f + sustain.x;
            }
        }
    }
    float value = ParameterCurveEvaluate(curve, t);
    SE3ApplyCurve(object, kind, t, value);
}

}

// The game time's period (DAT_142cfca34) and its value now (DAT_1430a94dc).
int32_t g_se3Period;
int32_t g_se3GameTime;

// One instance's frame.
// ref: FUN_1412d5290
void SE3UpdateObject(SE3SoundKitObject* object, int32_t elapsed, bool retrigger) {
    SE3SoundKitDef* def = object->m_def;
    if (def == nullptr) {
        return;
    }
    if (g_se3CheckFilters != 0 && g_se3Config.m_slot210.m_invoke != nullptr) {
        using INVOKE = bool (*)(SE3SoundKitDef*, SE3PLAYPARAMS*, void*);
        if (!reinterpret_cast<INVOKE>(g_se3Config.m_slot210.m_invoke)(def, &object->m_params, g_se3Config.m_slot210.m_storage)) {
            SE3Stop(static_cast<uint32_t>(object->m_handle), 0, 0, nullptr);
            return;
        }
    }
    object->m_elapsed += elapsed;
    SE3UpdateDistances(object);
    const uint8_t* advanced = AdvancedRecord(def->m_advancedId);
    float radius = 0.0f;
    if (advanced != nullptr) {
        radius = AdvancedFloat(advanced, 3);
        if (0.0f < object->m_maxDistance && object->m_maxDistance < radius) {
            radius = object->m_maxDistance;
        }
    }
    FMOD_VECTOR listener = {};
    g_fmodSystem->get3DListenerAttributes(0, &listener, nullptr, nullptr, nullptr);
    float dy = listener.y - object->m_position[1];
    float dx = listener.x - object->m_position[0];
    float dz = listener.z - object->m_position[2];
    float seconds = static_cast<float>(elapsed) * 0.001f;
    if (g_se3Config.m_slot1f0.m_invoke != nullptr) {
        using INVOKE = bool (*)(SE3SoundKitDef*, void*);
        bool quiet = reinterpret_cast<INVOKE>(g_se3Config.m_slot1f0.m_invoke)(def, g_se3Config.m_slot1f0.m_storage);
        float target = quiet ? 0.0f : 1.0f;
        Smooth(object->m_350, &target, 10.0f, seconds);
    }
    if (advanced != nullptr) {
        bool duck = AdvancedFloat(advanced, 0xf) < 1.0f || AdvancedFloat(advanced, 0x10) < 1.0f
            || AdvancedFloat(advanced, 0x11) < 1.0f || AdvancedFloat(advanced, 0x12) < 1.0f;
        if (duck) {
            float reach = AdvancedFloat(advanced, 0x16);
            if (0.0f < object->m_minDistance && object->m_minDistance < reach) {
                reach = object->m_minDistance;
            }
            bool placed = object->m_position[0] != 0.0f || object->m_position[1] != 0.0f || object->m_position[2] != 0.0f;
            if (!placed || dy * dy + dx * dx + dz * dz <= reach * reach) {
                SE3SuppressorAdd(object);
            }
        }
    }
    if (0.0f < radius && (object->m_slots[0].m_channel != nullptr || object->m_slots[1].m_channel != nullptr)) {
        FROST_UNIMPLEMENTED();  // FUN_14158fbf0: a Channel call of the reference's FMOD build with the kit's radius
    }
    if (advanced != nullptr) {
        float* direction = object->m_params.m_7c;
        float length2 = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
        if (2.3841858e-07f <= std::fabs(length2)) {
            float inside = AdvancedFloat(advanced, 0x1a);
            float outside = AdvancedFloat(advanced, 0x1b);
            float outsideVolume = AdvancedFloat(advanced, 0x1c);
            if (inside != 180.0f || outside != 270.0f || outsideVolume != 0.0f) {
                float length = std::sqrt(length2);
                if (9.9999997e-05f <= length) {
                    float inverse = 1.0f / length;
                    direction[0] *= inverse;
                    direction[1] *= inverse;
                    direction[2] *= inverse;
                } else {
                    direction[0] = 0.0f;
                    direction[1] = 0.0f;
                    direction[2] = 1.0f;
                }
                FMOD_VECTOR orientation = {direction[0], direction[1], -direction[2]};
                for (SE3SoundObject& slot : object->m_slots) {
                    if (slot.m_channel != nullptr) {
                        slot.m_channel->set3DConeSettings(inside, outside, outsideVolume);
                        slot.m_channel->set3DConeOrientation(&orientation);
                    }
                }
            }
        }
        if (retrigger && object->m_fadeState == 0 && def->m_prefetch == nullptr
            && (AdvancedU32(advanced, 0xd) != 0 || AdvancedU32(advanced, 0xb) != 0)) {
            uint8_t mode = object->m_3dMode;
            bool allowed = true;
            if (mode == 2) {
                allowed = object->m_retriggered == 0;
            } else if (mode == 0 && def->m_entryCount < 2) {
                allowed = object->m_params.m_parent == nullptr;
            }
            if (allowed && 0 < object->m_retrigger) {
                object->m_retrigger -= elapsed;
                if (object->m_retrigger < 1) {
                    object->m_retrigger = 0;
                    object->m_params.m_parent = object;
                    object->m_params.m_142 = 1;
                    const char* reason = mode == 1 ? "Re-Trigger Periodic Sound"
                        : (mode == 0 ? "Re-Trigger Looping Sound" : "Actually play delayed one-shot sound");
                    const float* position = object->m_hasPosition != 0 ? object->m_position : nullptr;
                    uint32_t result = SE3PlayKit(static_cast<uint32_t>(def->m_kitId), position, &object->m_params,
                        nullptr, reason, false, nullptr);
                    if (result == 0) {
                        if (object->m_3dMode == 2) {
                            object->m_retriggered = 1;
                        }
                    } else if (object->m_3dMode == 2) {
                        object->m_playing.Unlink();
                        SE3QueueRelease(object);
                    } else if (AdvancedU32(advanced, 0xb) != 0 && result != 0x13) {
                        object->m_retrigger = SE3RandomRange(static_cast<int32_t>(AdvancedU32(advanced, 0xa)),
                            static_cast<int32_t>(AdvancedU32(advanced, 0xb)));
                    }
                }
            }
        }
    }
    for (int32_t i = 0; i < 2; i++) {
        SE3SoundObject* slot = &object->m_slots[i];
        if (slot->m_channel != nullptr && slot->m_fade == 1) {
            slot->m_fadeVolume += seconds;
            if (1.0f < slot->m_fadeVolume) {
                slot->m_fadeVolume = 1.0f;
                slot->m_fade = 0;
            }
        }
        if (slot->m_fade == 2) {
            slot->m_fadeVolume -= seconds;
            if (slot->m_fadeVolume < 0.0f) {
                slot->m_fade = 0;
                slot->m_fadeVolume = 0.0f;
                SE3SlotStop(slot);
            }
        }
    }
    if (advanced != nullptr) {
        const uint8_t* kit = SoundKitRecord(def->m_kitId);
        uint16_t flags = kit != nullptr ? DB2FieldU16(kit, 3, 0, &g_soundKitDB) : 0;
        if (g_se3Config.m_slot110.m_invoke != nullptr && kit != nullptr && (flags & 0x10) != 0) {
            Smooth(object->m_340, &object->m_338, 1.5f, seconds);
            object->m_338 = object->m_340[0];
        }
        if (g_se3Config.m_slot130.m_invoke != nullptr && kit != nullptr && ((flags >> 0xb) & 1) != 0) {
            float target = CallFloatSlot(&g_se3Config.m_slot130, def);
            Smooth(object->m_328, &target, 1.5f, seconds);
            object->m_320 = object->m_328[0];
        }
        if (g_se3Config.m_slot150.m_invoke != nullptr && kit != nullptr && ((flags >> 0xb) & 1) != 0) {
            float target = CallFloatSlot(&g_se3Config.m_slot150, def);
            Smooth(object->m_330, &target, 1.5f, seconds);
            SE3ApplyPitch(object, object->m_pitch * object->m_330[0]);
        }
    }
    if (g_se3Config.m_getGameTime.m_invoke != nullptr && def->m_hasTimeWindow != 0) {
        int32_t offset = object->m_300;
        const uint8_t* own = AdvancedRecord(def->m_advancedId);
        int32_t a = (own != nullptr ? static_cast<int32_t>(AdvancedU32(own, 4)) : 0) + offset;
        int32_t d = TimeD(def) + offset;
        int32_t now = g_se3GameTime;
        bool inside = false;
        if (d < g_se3Period) {
            inside = a <= now && now <= d;
        } else {
            if (now < a) {
                now += g_se3Period;
            }
            inside = a <= now && now <= d;
        }
        if (inside) {
            int32_t start = a;
            int32_t b = TimeB(def) + offset;
            int32_t c = TimeC(def) + offset;
            int32_t t = g_se3GameTime;
            if (g_se3Period <= d && t < start) {
                t += g_se3Period;
            }
            if (t < b) {
                object->m_timeWindow = static_cast<float>(t - start) / static_cast<float>(b - start);
            } else {
                float value = 1.0f;
                if (c <= t) {
                    value = 1.0f - static_cast<float>(t - c) / static_cast<float>(d - c);
                }
                object->m_timeWindow = value;
            }
        } else {
            object->m_timeWindow = 0.0f;
        }
    }
    WowDB2ParentLookup* envelopes = g_soundEnvelopeDB.m_parentLookup;
    if (envelopes != nullptr) {
        DB2PARENTITER it;
        DB2PARENTITER end;
        envelopes->Begin(&it, def->m_kitId);
        envelopes->End(&end);
        while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
               || it.m_source != end.m_source) {
            FollowEnvelope(object, static_cast<const uint8_t*>(envelopes->Record(&it)));
            envelopes->Next(&it);
        }
    }
    float pitch = object->m_pitchCurve * object->m_pitch;
    for (SE3SoundObject& slot : object->m_slots) {
        if (slot.m_channel == nullptr) {
            continue;
        }
        if (slot.m_frequency == 0.0f) {
            slot.m_channel->getFrequency(&slot.m_frequency);
        }
        slot.m_channel->setFrequency(pitch * slot.m_frequency);
    }
}

// ---- the rest of the engine's frame -------------------------------------------------------

// The positional lowpass of one slot (Sound_EnablePositionalLowPassFilter).
// ref: FUN_1412d6690
void SE3UpdateLowpass(SE3SoundObject* slot, bool bypass, int32_t filter) {
    (void)filter;
    if (!SE3PositionalLowPassEnabled() || slot->m_dsp[0] == nullptr) {
        return;
    }
    if (bypass) {
        slot->m_dsp[0]->setBypass(true);
        return;
    }
    FROST_UNIMPLEMENTED();  // the SoundFilterElem cutoffs by distance and listener speed
}

// The frame's pass over the playing instances: each one's update, its volume and priority,
// and its lowpass; then the kit timers, the inaudible one-shots, and the timed instances.
void SE3UpdateInstances(int32_t elapsed) {
    for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Playing.Next(object);
        SE3UpdateObject(object, elapsed, true);
        object = next;
    }
    g_se3CheckFilters = 0;
    for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Playing.Next(object);
        SE3ApplyVolume(object);
        SE3ApplyPriority(object);
        bool kitBypass = false;
        if (object->m_def != nullptr) {
            if (const uint8_t* kit = SoundKitRecord(object->m_def->m_kitId)) {
                kitBypass = (DB2FieldU16(kit, 3, 0, &g_soundKitDB) & 1) != 0;
            }
        }
        int32_t filter = object->m_params.m_e0;
        bool bypass = kitBypass || SE3BusGetFlag(object->m_params.m_bus);
        SE3UpdateLowpass(&object->m_slots[0], bypass, filter);
        SE3UpdateLowpass(&object->m_slots[1], SE3BusGetFlag(object->m_params.m_bus) || kitBypass, filter);
        object = next;
    }
    for (SE3SoundKitDef* def = g_se3DefTimers.Head(); def != nullptr;) {
        SE3SoundKitDef* next = g_se3DefTimers.Next(def);
        if (0 < def->m_34) {
            def->m_34 -= elapsed;
            if (def->m_34 < 1) {
                def->m_34 = 0;
                def->m_timerLink.Unlink();
            }
        }
        def = next;
    }
    for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Playing.Next(object);
        if (object->m_params.m_135 == 0 && static_cast<uint8_t>(object->m_3dMode - 1) <= 1) {
            for (SE3SoundObject& slot : object->m_slots) {
                if (slot.m_channel == nullptr) {
                    continue;
                }
                bool playing = false;
                slot.m_channel->isPlaying(&playing);
                if (!playing) {
                    continue;
                }
                if (SE3IsInaudible(object)) {
                    object->m_stopTimer += elapsed;
                    if (400u < static_cast<uint32_t>(object->m_stopTimer)) {
                        SE3Stop(static_cast<uint32_t>(object->m_handle), 1, 0, nullptr);
                    }
                }
                break;
            }
        }
        object = next;
    }
    for (SE3SoundKitObject* object = g_se3Timed.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Timed.Next(object);
        object->m_lifeTimer -= elapsed;
        if (object->m_lifeTimer < 1) {
            object->m_timed.Unlink();
            SE3Stop(static_cast<uint32_t>(object->m_handle), 4, 0, nullptr);
        }
        object = next;
    }
}

// Whether an instance's first playing channel is virtual or inaudible.
// ref: FUN_1412cd760
bool SE3IsInaudible(SE3SoundKitObject* object) {
    for (SE3SoundObject& slot : object->m_slots) {
        if (slot.m_channel != nullptr) {
            bool isVirtual = false;
            slot.m_channel->isVirtual(&isVirtual);
            float audibility = 1.0f;
            slot.m_channel->getAudibility(&audibility);
            return audibility == 0.0f ? true : isVirtual;
        }
    }
    return true;
}

// Every 10 s: the combined handles whose members are all gone (FUN_1412cab50's first pass).
void SE3ReleaseUnusedKitGroups() {
    TSExplicitList<SE3COMBINEDNODE>& full = g_se3Combined.m_fulllist;
    for (SE3COMBINEDNODE* node = full.Head(); node != nullptr;) {
        SE3COMBINEDNODE* next = full.Link(node)->Next();
        bool alive = false;
        for (uint32_t i = 0; i < node->m_count; i++) {
            SE3SoundKitObject* object = SE3GetObject(node->m_handles[i]);
            if (object != nullptr && (object->m_playing.m_next != nullptr || object->m_loading.m_next != nullptr
                                      || object->m_pending.m_next != nullptr || object->m_waiting.m_next != nullptr)) {
                alive = true;
                break;
            }
        }
        if (!alive) {
            g_se3Combined.Unlink(node);
            g_se3Combined.Delete(node);
        }
        node = next;
    }
}

// Every 2 s: placed instances out of range stop (one-shots that may) and the rest note it.
void SE3UpdateRange() {
    for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr;) {
        SE3SoundKitObject* next = g_se3Playing.Next(object);
        float length2 = object->m_position[0] * object->m_position[0] + object->m_position[1] * object->m_position[1]
            + object->m_position[2] * object->m_position[2];
        if (2.3841858e-07f <= std::fabs(length2)) {
            if (object->m_params.m_checkRange != 0 && SE3IsOutOfRange(object->m_position, object->m_maxDistance + 50.0f)) {
                SE3StopObject(object, 4, 0);
            }
            object->m_10 = SE3IsOutOfRange(object->m_position, object->m_maxDistance) ? 1 : 0;
        }
        object = next;
    }
}

// ---- the combinable kits ------------------------------------------------------------------

// A kit marked combinable is not played when asked: each request in a frame adds its position
// (or none) to the kit's entry, and the frame plays the entry once, at the one position when
// there was one request, otherwise unplaced with the volume of the nearest.

namespace {

// One request's position (0x20, from the pool below).
struct SE3CombinableSoundKitPosition {
    TSLink<SE3CombinableSoundKitPosition> m_link;   // 0x00
    uint8_t m_hasPosition;                          // 0x10
    float m_position[3];                            // 0x14
};
static_assert(sizeof(SE3CombinableSoundKitPosition) == 0x20, "SE3CombinableSoundKitPosition is 0x20 bytes");

// One kit's requests this frame (0x198).
struct SE3CombinableSoundList : TSHashObject<SE3CombinableSoundList> {
    int32_t m_kitId;                                // 0x30 (also the hash)
    uint32_t m_pad34;
    SE3PLAYPARAMS m_params;                         // 0x38 the last request's
    // 0x180: the unplaced requests at the head, the placed ones at the tail
    TSList<SE3CombinableSoundKitPosition, TSGetLink<SE3CombinableSoundKitPosition>, 8> m_positions;
};
static_assert(offsetof(SE3CombinableSoundList, m_params) == 0x38, "m_params is at 0x38");
static_assert(offsetof(SE3CombinableSoundList, m_positions) == 0x180, "m_positions is at 0x180");
static_assert(sizeof(SE3CombinableSoundList) == 0x198, "SE3CombinableSoundList is 0x198 bytes");

}  // namespace

template <>
struct TSHashTableTags<SE3CombinableSoundList> {
    static constexpr const char* s_fixedArray =
        ".?AV?$TSFixedArray@V?$TSExplicitList@USE3CombinableSoundList@@$0?CCCCCCCD@$07@@$07@@";
    static constexpr const char* s_growableArray =
        ".?AV?$TSGrowableArray@V?$TSExplicitList@USE3CombinableSoundList@@$0?CCCCCCCD@$07@@$07@@";
};

namespace {

const char s_combinableTableTag[] = ".?AV?$TSHashTable@USE3CombinableSoundList@@VHASHKEY_NONE@@$07@@";
const char s_combinableListTag[] =
    ".?AV?$TSList@USE3CombinableSoundList@@V?$TSGetExplicitLink@USE3CombinableSoundList@@@@$07@@";
const char s_positionTag[] = ".?AVSE3CombinableSoundKitPosition@@";
const char s_positionListTag[] =
    ".?AV?$TSList@VSE3CombinableSoundKitPosition@@V?$TSGetLink@VSE3CombinableSoundKitPosition@@@@$07@@";

CDataAllocator s_positions(0x20, 0x20);     // DAT_1430a94e0 (FUN_1401e5d00)

// The entry's destructor: its positions back to the pool, its parameters, its links.
// ref: FUN_1412c6330
void CombinableListDestroy(SE3CombinableSoundList* list) {
    for (SE3CombinableSoundKitPosition* position = list->m_positions.Head(); position != nullptr;) {
        SE3CombinableSoundKitPosition* next = list->m_positions.Next(position);
        position->m_link.Unlink();
        s_positions.PutData(position);
        position = next;
    }
    while (SE3CombinableSoundKitPosition* position = list->m_positions.Head()) {
        position->m_link.Unlink();
        SMemFree(position, s_positionListTag, -2, 0);
    }
    list->m_positions.~TSList();
    SE3PlayParamsDestroy(&list->m_params);
    list->Destroy();
}

// TSHashTableReuse<SE3CombinableSoundList, HASHKEY_NONE, 8> (vtable 0x142728d50); the
// requests' path only reaches its first two slots, which allocate and free.
class SE3COMBINABLETABLE : public TSHashTable<SE3CombinableSoundList> {
    public:
    // ref: FUN_1412c5610
    void Delete(SE3CombinableSoundList* node) override {
        CombinableListDestroy(node);
        SMemFree(node, s_combinableTableTag, -2, 0);
    }

    // FUN_1412c5640 -> TSList<SE3CombinableSoundList>::NewNode (FUN_1412cda80)
    // ref: FUN_1412c5640
    SE3CombinableSoundList* NewNode(TSExplicitList<SE3CombinableSoundList>* slot, uint32_t extra, uint32_t flags) override {
        void* memory = SMemAlloc(extra + sizeof(SE3CombinableSoundList), 8, s_combinableListTag, 0, -2, flags | 8);
        std::memset(memory, 0, offsetof(SE3CombinableSoundList, m_hashval) + sizeof(uint32_t));
        auto node = static_cast<SE3CombinableSoundList*>(memory);
        SE3PlayParamsConstruct(&node->m_params);
        new (&node->m_positions) TSList<SE3CombinableSoundKitPosition, TSGetLink<SE3CombinableSoundKitPosition>, 8>();
        slot->LinkToTail(node);
        return node;
    }
};

SE3COMBINABLETABLE s_combinable;            // 0x142cfce70 (FUN_1401e59c0)

}  // namespace

// SE3PlayKit's combinable branch (FUN_1412ce100): the kit's entry, found by its id or made, takes
// the request's parameters and its position.
void SE3CombinableAdd(uint32_t kitId, const float* position, const SE3PLAYPARAMS* params) {
    SE3CombinableSoundList* list = s_combinable.Find(kitId, [](SE3CombinableSoundList*) {
        return true;
    });
    if (list == nullptr) {
        list = s_combinable.New(kitId, 0, 0);
        list->m_hashval = kitId;
    }
    list->m_kitId = static_cast<int32_t>(kitId);
    SE3PlayParamsCopy(&list->m_params, params);
    auto entry = static_cast<SE3CombinableSoundKitPosition*>(s_positions.GetData(false, s_positionTag, -2, 1));
    entry->m_link.m_prevlink = nullptr;
    entry->m_link.m_next = nullptr;
    entry->m_hasPosition = 0;
    entry->m_position[0] = 0.0f;
    entry->m_position[1] = 0.0f;
    entry->m_position[2] = 0.0f;
    if (position != nullptr) {
        entry->m_hasPosition = 1;
        entry->m_position[0] = position[0];
        entry->m_position[1] = position[1];
        entry->m_position[2] = position[2];
        list->m_positions.LinkToTail(entry);
    } else {
        list->m_positions.LinkToHead(entry);
    }
}

// The frame's pass over the entries: each plays once and goes. An entry with more than one
// request plays unplaced, as loud as its nearest request would have been (silent past the
// kit's maximum distance, fading over its last tenth), or at full volume when any request
// had no position.
// ref: FUN_1412cf720
void SE3UpdateCombinedKits() {
    SE3CombinableSoundList* list = s_combinable.m_fulllist.Head();
    if (list == nullptr) {
        return;
    }
    FMOD_VECTOR listener = {0.0f, 0.0f, 0.0f};
    g_fmodSystem->get3DListenerAttributes(0, &listener, nullptr, nullptr, nullptr);
    float last[3] = {0.0f, 0.0f, 0.0f};
    do {
        SE3SoundKitDef* def = SE3FindKitDef(list->m_kitId);
        if (def == nullptr) {
            s_combinable.Unlink(list);
            s_combinable.Delete(list);
            list = s_combinable.m_fulllist.Head();
            continue;
        }
        bool placed = true;
        uint32_t count = 0;
        float nearest = 3.4028235e+38f;
        for (SE3CombinableSoundKitPosition* entry = list->m_positions.Head(); entry != nullptr;
             entry = list->m_positions.Next(entry)) {
            last[0] = entry->m_position[0];
            last[1] = entry->m_position[1];
            last[2] = entry->m_position[2];
            count++;
            if (entry->m_hasPosition == 0) {
                placed = false;
                break;
            }
            float dx = listener.x - entry->m_position[0];
            float dz = listener.z - entry->m_position[2];
            float distance2 = (listener.y - entry->m_position[1]) * (listener.y - entry->m_position[1]) + dx * dx + dz * dz;
            if (distance2 < nearest) {
                nearest = distance2;
            }
        }
        list->m_params.m_combinable = 0;
        if (count == 1) {
            SE3PlayKit(static_cast<uint32_t>(list->m_kitId), placed ? last : nullptr, &list->m_params, nullptr, nullptr,
                false, nullptr);
        } else {
            float volume = 1.0f;
            if (placed) {
                float distance = std::sqrt(nearest);
                bool local;
                g_soundKitAdvancedDB.GetRecord(static_cast<uint32_t>(def->m_advancedId), false, &local);
                float minDistance = SE3GetMinDistance(&list->m_params, def);
                if (minDistance <= 0.0f) {
                    minDistance = 0.0f;
                }
                float maxDistance = SE3GetMaxDistance(&list->m_params, def);
                if (maxDistance <= minDistance) {
                    maxDistance = minDistance;
                }
                if (minDistance < distance) {
                    if (distance < maxDistance) {
                        float fadeStart = maxDistance * 0.9f;
                        volume = minDistance / ((distance - minDistance) * 4.0f + minDistance);
                        if (fadeStart < distance) {
                            volume *= 1.0f - (distance - fadeStart) / (maxDistance - fadeStart);
                        }
                    } else {
                        volume = 0.0f;
                    }
                }
            }
            list->m_params.m_08 = volume;
            SE3PlayKit(static_cast<uint32_t>(list->m_kitId), nullptr, &list->m_params, nullptr, nullptr, false, nullptr);
        }
        s_combinable.Unlink(list);
        s_combinable.Delete(list);
        list = s_combinable.m_fulllist.Head();
    } while (list != nullptr);
}

// The shutdown's part (FUN_1412d41d0): every entry deleted, then the pool's chunks freed.
void SE3CombinableShutdown() {
    s_combinable.Clear(false);
    s_positions.Clear(s_positionTag, -2);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cfce70 = (void*)&s_combinable;
extern "C" void* const gen_p_1430a94e0 = (void*)&s_positions;
// ---- end of generated globals ----
