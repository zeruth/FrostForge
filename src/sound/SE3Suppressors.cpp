// SE3Suppressors (Engine/Source/SoundEngine/SE3Suppressors.cpp): a playing kit can duck the
// other buses near it (SoundKitAdvanced fields 0xf..0x15, one factor per bus category); each
// suppressor eases its factors in while its sound plays in range and back out after.

#include "sound/SE3Kit.hpp"

#include <cmath>

#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "fmod.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Internal.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\SoundEngine\\SE3Suppressors.cpp";
const char s_listTag[] = ".?AV?$TSList@VSE3SuppressorSound@@V?$TSGetLink@VSE3SuppressorSound@@@@$07@@";

// One suppressor (0x68).
struct SUPPRESSOR {
    TSLink<SUPPRESSOR> m_link;          // 0x00
    int32_t m_handle;                   // 0x10 0 once its sound is gone
    float m_factor[7];                  // 0x14 now, by category
    float m_distance;                   // 0x30
    float m_target[7];                  // 0x34
    float m_radiusIn;                   // 0x50
    float m_radiusOut;                  // 0x54
    uint8_t m_active;                   // 0x58
    uint8_t m_pad59[3];
    int32_t m_fadeIn;                   // 0x5C ms
    int32_t m_fadeOut;                  // 0x60 ms
    uint8_t m_pad64[4];
};
static_assert(sizeof(SUPPRESSOR) == 0x68, "SUPPRESSOR is 0x68 bytes");

TSList<SUPPRESSOR, TSGetLink<SUPPRESSOR>, 8> s_suppressors;    // 0x142cfd248

const uint8_t* AdvancedRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitAdvancedDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

}

// ref: FUN_1412e0ce0
void SE3SuppressorsInit() {
    SE3Log(0x5f, s_file, 0, " - Init Suppressors");
    while (SUPPRESSOR* suppressor = s_suppressors.Head()) {
        suppressor->m_link.Unlink();
        SMemFree(suppressor, s_listTag, -2, 0);
    }
}

// ref: FUN_1412e12f0
void SE3SuppressorsShutdown() {
    while (SUPPRESSOR* suppressor = s_suppressors.Head()) {
        suppressor->m_link.Unlink();
        SMemFree(suppressor, s_file, -2, 0);
    }
}

// An instance that ducks starts a suppressor (one per instance).
// ref: FUN_1412e0e00
void SE3SuppressorAdd(SE3SoundKitObject* object) {
    if (object == nullptr || object->m_def == nullptr) {
        return;
    }
    const uint8_t* advanced = AdvancedRecord(object->m_def->m_advancedId);
    if (advanced == nullptr) {
        return;
    }
    bool ducks = false;
    for (uint32_t field : {0xfu, 0x10u, 0x11u, 0x12u, 0x14u, 0x15u}) {
        float value = g_soundKitAdvancedDB.FieldFloat(advanced, field, 0);
        if (0.0f <= value && value < 1.0f) {
            ducks = true;
            break;
        }
    }
    if (!ducks) {
        return;
    }
    for (SUPPRESSOR* each = s_suppressors.Head(); each != nullptr; each = s_suppressors.Next(each)) {
        if (each->m_handle == object->m_handle) {
            return;
        }
    }
    auto suppressor = static_cast<SUPPRESSOR*>(SMemAlloc(sizeof(SUPPRESSOR), 0x16, s_file, SStrHashFnv1a(s_file), 0xf7, 0));
    suppressor->m_link.m_prevlink = nullptr;
    suppressor->m_link.m_next = nullptr;
    for (float& factor : suppressor->m_factor) {
        factor = 1.0f;
    }
    suppressor->m_distance = 0.0f;
    suppressor->m_active = 0;
    suppressor->m_handle = object->m_handle;
    for (uint32_t i = 0; i < 7; i++) {
        suppressor->m_target[i] = g_soundKitAdvancedDB.FieldFloat(advanced, 0xf + i, 0);
    }
    suppressor->m_fadeOut = static_cast<int32_t>(DB2FieldU32(advanced, 0x19, 0, &g_soundKitAdvancedDB));
    suppressor->m_fadeIn = static_cast<int32_t>(DB2FieldU32(advanced, 0x18, 0, &g_soundKitAdvancedDB));
    float radiusIn = g_soundKitAdvancedDB.FieldFloat(advanced, 0x16, 0);
    if (0.0f < object->m_params.m_minDistance && object->m_params.m_minDistance < radiusIn) {
        radiusIn = object->m_params.m_minDistance;
    }
    suppressor->m_radiusIn = radiusIn;
    float radiusOut = g_soundKitAdvancedDB.FieldFloat(advanced, 0x17, 0);
    if (0.0f < object->m_params.m_maxDistance && object->m_params.m_maxDistance < radiusOut) {
        radiusOut = object->m_params.m_maxDistance;
    }
    suppressor->m_radiusOut = radiusOut;
    s_suppressors.LinkToTail(suppressor);
}

// An instance going away leaves its suppressors to fade back.
// ref: FUN_1412e13a0
void SE3SuppressorsForget(int32_t handle) {
    for (SUPPRESSOR* each = s_suppressors.Head(); each != nullptr; each = s_suppressors.Next(each)) {
        if (each->m_handle == handle) {
            each->m_handle = 0;
        }
    }
}

// ref: FUN_1412e0dd0
bool SE3SuppressorsAny() {
    return s_suppressors.Head() != nullptr;
}

// An instance's own ducking factors (all 1 without a kit that ducks).
// ref: FUN_1412e0a70
void SE3SuppressorGetTargets(uint32_t handle, float* targets) {
    for (int32_t i = 0; i < 7; i++) {
        targets[i] = 1.0f;
    }
    SE3SoundKitObject* object = SE3GetObject(handle);
    if (object == nullptr || object->m_def == nullptr) {
        return;
    }
    const uint8_t* advanced = AdvancedRecord(object->m_def->m_advancedId);
    if (advanced == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < 7; i++) {
        targets[i] = g_soundKitAdvancedDB.FieldFloat(advanced, 0xf + i, 0);
    }
}

// The ducking of a category from every other instance's suppressor.
// ref: FUN_1412e08f0
float SE3SuppressorFactor(int8_t category, uint32_t handle) {
    float factor = 1.0f;
    float own[7];
    SE3SuppressorGetTargets(handle, own);
    bool ownDucks = false;
    for (float value : own) {
        if (value < 1.0f) {
            ownDucks = true;
            break;
        }
    }
    size_t offset;
    switch (category) {
        case 0:
            offset = 0x14;
            break;
        case 1:
            offset = 0x18;
            break;
        case 2:
            offset = 0x1c;
            break;
        case 3:
            offset = 0x20;
            break;
        case 4:
            offset = 0x24;
            break;
        case 0x41:
            offset = 0x28;
            break;
        case 0x42:
            offset = 0x2c;
            break;
        default:
            offset = 0x30;
            break;
    }
    for (SUPPRESSOR* each = s_suppressors.Head(); each != nullptr; each = s_suppressors.Next(each)) {
        if (handle != 0 && static_cast<uint32_t>(each->m_handle) == handle) {
            continue;
        }
        float value = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(each) + offset);
        if (factor <= value) {
            value = factor;
        }
        factor = value;
        if (ownDucks && each->m_factor[4] < value) {
            factor = each->m_factor[4];
        }
    }
    return factor;
}

// The frame's suppressors: each eases toward its targets while its sound plays in range and
// back to 1 otherwise; one fully back and orphaned is freed.
// ref: FUN_1412e13f0
void SE3SuppressorsUpdate(int32_t elapsed) {
    FMOD_VECTOR listener = {};
    g_fmodSystem->get3DListenerAttributes(0, &listener, nullptr, nullptr, nullptr);
    bool any = s_suppressors.Head() != nullptr;
    for (SUPPRESSOR* suppressor = s_suppressors.Head(); suppressor != nullptr;) {
        SUPPRESSOR* next = s_suppressors.Next(suppressor);
        float position[3] = {};
        if (SE3SoundKitObject* owner = SE3GetObject(static_cast<uint32_t>(suppressor->m_handle))) {
            position[0] = owner->m_position[0];
            position[1] = owner->m_position[1];
            position[2] = owner->m_position[2];
        }
        for (int32_t i = 0; i < 7; i++) {
            float& factor = suppressor->m_factor[i];
            float target = suppressor->m_target[i];
            if (suppressor->m_handle == 0) {
                if (factor != 1.0f) {
                    if (suppressor->m_fadeOut < 1) {
                        factor = 1.0f;
                    } else {
                        float t = static_cast<float>(elapsed) / static_cast<float>(suppressor->m_fadeOut)
                            + (factor - target) / (1.0f - target);
                        if (1.0f <= t) {
                            t = 1.0f;
                        }
                        factor = t * (1.0f - target) + target;
                    }
                }
                continue;
            }
            bool inRange = std::fabs(position[1] * position[1] + position[0] * position[0] + position[2] * position[2])
                < 2.3841858e-07f;
            if (!inRange) {
                float dx = listener.x - position[0];
                float dy = listener.y - position[1];
                float dz = listener.z - position[2];
                suppressor->m_distance = std::sqrt(dy * dy + dx * dx + dz * dz);
                inRange = suppressor->m_distance < suppressor->m_radiusIn
                    || (suppressor->m_distance < suppressor->m_radiusOut && suppressor->m_active != 0);
            }
            if (inRange && SE3IsPlaying(static_cast<uint32_t>(suppressor->m_handle))) {
                if (factor != target) {
                    if (0 < suppressor->m_fadeIn) {
                        float t = (factor - target) / (1.0f - target)
                            - static_cast<float>(elapsed) / static_cast<float>(suppressor->m_fadeIn);
                        if (t <= 0.0f) {
                            t = 0.0f;
                        }
                        factor = t * (1.0f - target) + target;
                    } else {
                        factor = target;
                    }
                }
                suppressor->m_active = 1;
                continue;
            }
            int32_t handle = suppressor->m_handle;
            suppressor->m_active = 0;
            for (SUPPRESSOR* each = s_suppressors.Head(); each != nullptr; each = s_suppressors.Next(each)) {
                if (each->m_handle == handle) {
                    each->m_handle = 0;
                }
            }
        }
        bool back = suppressor->m_handle == 0;
        for (float factor : suppressor->m_factor) {
            back = back && factor == 1.0f;
        }
        if (back) {
            suppressor->m_link.Unlink();
            SMemFree(suppressor, s_file, -2, 0);
        }
        suppressor = next;
    }
    if (any) {
        for (SE3SoundKitObject* object = g_se3Playing.Head(); object != nullptr; object = g_se3Playing.Next(object)) {
            SE3ApplyVolume(object);
        }
    }
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cfd248 = (void*)&s_suppressors;
// ---- end of generated globals ----
