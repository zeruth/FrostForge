// SE3Bus (Engine/Source/SoundEngine/SE3Bus.cpp): the sound buses. Each SoundBus record is one
// FMOD channel group, linked under its parent record's group, with the master bus under FMOD's
// own master group; the bus volumes the game sets live in a map beside them.

#include "sound/SE3.hpp"

#include <cstring>

#include "blz/ChainedHash.hpp"
#include "console/Var.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "fmod.hpp"
#include "sound/SE3Bus.hpp"
#include "sound/SE3Internal.hpp"
#include "sound/SE3Kit.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"

SE3BUSARRAY g_se3Buses;

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\SoundEngine\\SE3Bus.cpp";
const char s_arrayTag[] = ".?AV?$TSGrowableArray@VSE3SoundBus@@$07@@";
const char s_fixedTag[] = ".?AV?$TSFixedArray@VSE3SoundBus@@$07@@";
const char s_nodeTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBW4SE3BUS@@M@blz@@@blz@@";
const char s_bucketTag[] = ".?AU?$chained_hash_node@U?$pair@$$CBW4SE3BUS@@M@blz@@@blz@@*";

// The bus volumes (DAT_142cfcf98): blz::unordered_map<SE3BUS, float>.
struct VOLUMENODE {
    VOLUMENODE* m_next;
    int8_t m_key;
    float m_value;
};
blz::chained_hash_header s_volumes;

int32_t s_masterRecord;         // DAT_1430a96a8 the master bus record's id
FMOD::DSP* s_ambienceHighpass;  // DAT_1430a96d0 bus 2
FMOD::DSP* s_allyHighpass;      // DAT_1430a96d8 bus 'C'
FMOD::DSP* s_enemyHighpass;     // DAT_1430a96e0 bus 'N'
FMOD::DSP* s_npcHighpass;       // DAT_1430a96e8 bus 0x1f
CVar* s_ambienceCutoff;         // DAT_1430a96f0
CVar* s_allyCutoff;             // DAT_1430a96f8
CVar* s_enemyCutoff;            // DAT_1430a9700
CVar* s_npcCutoff;              // DAT_1430a9708

// The volume slot of a bus, made at 0 when missing.
// ref: FUN_1412d7700
float* VolumeSlot(int8_t bus) {
    bool inserted;
    return &blz::chained_insert<VOLUMENODE>(&s_volumes, bus, &inserted, s_nodeTag, s_bucketTag)->m_value;
}

// A highpass cutoff CVar: the DSP's cutoff, 10 Hz to 22 kHz.
// ref: FUN_1412d78c0
bool HighpassCutoffChanged(CVar* cvar, const char* oldValue, const char* newValue, void* userData) {
    (void)cvar;
    (void)oldValue;
    if (userData == nullptr) {
        return false;
    }
    float cutoff = SStrToFloat(newValue);
    if (cutoff < 10.0f || 22000.0f < cutoff) {
        return false;
    }
    static_cast<FMOD::DSP*>(userData)->setParameter(0, cutoff);
    return true;
}

// ref: FUN_1412dbb80
int32_t CreateDSP(FMOD_DSP_TYPE type, FMOD::DSP** dsp) {
    return g_fmodSystem->createDSPByType(type, dsp);
}

// ref: FUN_1412dbba0
void ReleaseDSP(FMOD::DSP* dsp) {
    if (dsp != nullptr) {
        dsp->remove();
        dsp->release();
    }
}

// The bus list's link destroy (FUN_1402f9a70 over +0x30).
void DestroyBusList(SE3SoundBus* bus) {
    bus->m_kits.~TSList();
}

// Grows the array's storage to `count` buses.
// ref: FUN_1412d7910
void Reserve(SE3BUSARRAY* array, uint32_t count) {
    auto data = static_cast<SE3SoundBus*>(
        SMemReAlloc(array->m_data, static_cast<uint64_t>(count) * sizeof(SE3SoundBus), 0xb, s_arrayTag,
            SStrHashFnv1a(s_arrayTag), -2, 0));
    array->m_data = data;
    array->m_capacity = count;
}

// Sets the count, constructing each new bus.
// ref: FUN_1412d7ab0
void SetCount(SE3BUSARRAY* array, uint32_t count) {
    if (array->m_count < count) {
        if (array->m_capacity < count) {
            uint32_t granularity = array->m_granularity;
            if (granularity == 0) {
                if (count < 8) {
                    // the highest power of two in the count
                    granularity = count;
                    for (uint32_t rest = (count - 1) & count; rest != 0; rest = (rest - 1) & rest) {
                        granularity = rest;
                    }
                    if (granularity == 0) {
                        granularity = 1;
                    }
                } else {
                    array->m_granularity = 8;
                    granularity = 8;
                }
            }
            uint32_t capacity = count;
            if (count % granularity != 0) {
                capacity = granularity - count % granularity + count;
            }
            Reserve(array, capacity);
        }
        for (uint32_t i = array->m_count; i < count; i++) {
            SE3SoundBus* bus = &array->m_data[i];
            bus->m_record = 0;
            bus->m_bus = -1;
            bus->m_parent = -1;
            bus->m_muted = 0;
            bus->m_enabled = 1;
            bus->m_flag = 0;
            bus->m_09 = 1;
            bus->m_group = nullptr;
            bus->m_18 = 0;
            bus->m_1c = 0xff;
            bus->m_1d = 0xff;
            bus->m_volume = 1.0f;
            bus->m_targetVolume = 1.0f;
            bus->m_28 = 0x80;
            bus->m_29 = 0x80;
            bus->m_2a = 0;
            bus->m_2b = 0;
            new (&bus->m_kits) TSList<SE3SoundKitObject, TSGetLink<SE3SoundKitObject>, 8>();
            bus->m_kits.m_linkoffset = 0x3b8;
        }
        array->m_count = count;
        return;
    }
    for (uint32_t i = count; i < array->m_count; i++) {
        DestroyBusList(&array->m_data[i]);
    }
    array->m_count = count;
}

void ClearBuses() {
    for (uint32_t i = 0; i < g_se3Buses.m_count; i++) {
        DestroyBusList(&g_se3Buses.m_data[i]);
    }
    if (g_se3Buses.m_data != nullptr) {
        SMemFree(g_se3Buses.m_data, s_fixedTag, -2, 0);
    }
    g_se3Buses.m_capacity = 0;
    g_se3Buses.m_count = 0;
    g_se3Buses.m_data = nullptr;
}

// A highpass on one bus: made, set to the CVar's cutoff (100 Hz without it), and added to the
// bus's group.
bool AddHighpass(FMOD::DSP** dsp, CVar* cutoff, int8_t bus) {
    int32_t result = CreateDSP(FMOD_DSP_TYPE_HIGHPASS_SIMPLE, dsp);
    if (result == FMOD_OK) {
        (*dsp)->setParameter(0, cutoff != nullptr ? cutoff->m_floatValue : 100.0f);
        result = g_se3Buses.m_data[bus].m_group->addDSP(*dsp, nullptr);
        if (result == FMOD_OK) {
            return true;
        }
    }
    SE3Log(0x12a, s_file, result, "- Failed to create Simple Highpass DSP for bus %d", bus);
    ReleaseDSP(*dsp);
    *dsp = nullptr;
    return false;
}

// One bus from its record: its fields, its channel group under the parent's, the highpass of
// the buses that have one, and then every record whose parent it is.
// ref: FUN_1412d8260
void CreateBus(const uint8_t* record, int8_t parent, bool flag) {
    int8_t id = static_cast<int8_t>(DB2FieldU8(record, 6, 0, &g_soundBusDB));
    if ('a' < id) {
        return;
    }
    SE3SoundBus* bus = &g_se3Buses.m_data[id];
    bus->m_record = DB2FieldI32(record, 0, 0, &g_soundBusDB);
    bus->m_bus = static_cast<int8_t>(DB2FieldU8(record, 6, 0, &g_soundBusDB));
    bus->m_parent = parent;
    bus->m_muted = 0;
    bus->m_enabled = 1;
    bus->m_09 = flag;
    bus->m_flag = DB2FieldU8(record, 1, 0, &g_soundBusDB) & 1;
    uint8_t value = DB2FieldU8(record, 5, 0, &g_soundBusDB);
    bus->m_1c = value;
    bus->m_1d = value;
    float volume = g_soundBusDB.FieldFloat(record, 4, 0);
    bus->m_volume = volume;
    bus->m_targetVolume = volume;
    value = DB2FieldU8(record, 2, 0, &g_soundBusDB);
    bus->m_28 = value;
    bus->m_29 = value;
    value = DB2FieldU8(record, 3, 0, &g_soundBusDB);
    bus->m_2a = value;
    bus->m_2b = value;
    bus->m_group = SE3CreateChannelGroup(nullptr);
    if (static_cast<uint32_t>(static_cast<int32_t>(parent)) < g_se3Buses.m_count &&
        g_se3Buses.m_data[parent].m_group != nullptr) {
        g_se3Buses.m_data[parent].m_group->addGroup(g_se3Buses.m_data[id].m_group);
        if (id == SE3BUS_AMBIENCE) {
            if (s_ambienceHighpass == nullptr) {
                AddHighpass(&s_ambienceHighpass, s_ambienceCutoff, SE3BUS_AMBIENCE);
            }
        } else if (id == 'C') {
            if (s_allyHighpass == nullptr) {
                AddHighpass(&s_allyHighpass, s_allyCutoff, 'C');
            }
        } else if (id == 'N') {
            if (s_enemyHighpass == nullptr) {
                AddHighpass(&s_enemyHighpass, s_enemyCutoff, 'N');
            }
        } else if (id == 0x1f && s_npcHighpass == nullptr) {
            AddHighpass(&s_npcHighpass, s_npcCutoff, 0x1f);
        }
    } else if (id == SE3BUS_MASTER) {
        FMOD::ChannelGroup* master = nullptr;
        if (g_fmodSystem->getMasterChannelGroup(&master) == FMOD_OK) {
            master->addGroup(g_se3Buses.m_data[SE3BUS_MASTER].m_group);
        }
    }
    WowDB2ParentLookup* children = g_soundBusDB.m_parentLookup;
    if (children == nullptr) {
        return;
    }
    DB2PARENTITER it;
    DB2PARENTITER end;
    children->Begin(&it, DB2FieldI32(record, 0, 0, &g_soundBusDB));
    children->End(&end);
    while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent ||
           it.m_source != end.m_source) {
        auto child = static_cast<const uint8_t*>(children->Record(&it));
        if (static_cast<int8_t>(DB2FieldU8(child, 6, 0, &g_soundBusDB)) != -1) {
            CreateBus(child, static_cast<int8_t>(DB2FieldU8(record, 6, 0, &g_soundBusDB)), true);
        }
        children->Next(&it);
    }
}

// Releases a bus's group, its children's first, and its highpass.
// ref: FUN_1412d8f20
void DestroyBus(int8_t id) {
    if (g_se3Buses.m_count <= static_cast<uint32_t>(static_cast<int32_t>(id))) {
        return;
    }
    WowDB2ParentLookup* children = g_soundBusDB.m_parentLookup;
    if (children != nullptr) {
        DB2PARENTITER it;
        DB2PARENTITER end;
        children->Begin(&it, g_se3Buses.m_data[id].m_record);
        children->End(&end);
        while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent ||
               it.m_source != end.m_source) {
            auto child = static_cast<const uint8_t*>(children->Record(&it));
            DestroyBus(static_cast<int8_t>(DB2FieldU8(child, 6, 0, &g_soundBusDB)));
            children->Next(&it);
        }
    }
    SE3SoundBus* bus = &g_se3Buses.m_data[id];
    switch (bus->m_bus) {
        case SE3BUS_AMBIENCE:
            ReleaseDSP(s_ambienceHighpass);
            s_ambienceHighpass = nullptr;
            break;
        case 0x1f:
            ReleaseDSP(s_npcHighpass);
            s_npcHighpass = nullptr;
            break;
        case 'C':
            ReleaseDSP(s_allyHighpass);
            s_allyHighpass = nullptr;
            break;
        case 'N':
            ReleaseDSP(s_enemyHighpass);
            s_enemyHighpass = nullptr;
            break;
        default:
            break;
    }
    if (bus->m_group != nullptr) {
        bus->m_group->release();
        bus->m_group = nullptr;
    }
}

// The SoundBus record whose bus is `id` (FUN_1405ce010, a scan of the table).
const uint8_t* BusRecord(uint8_t id) {
    WowDB2Lookup* lookup = g_soundBusDB.m_lookup;
    if (lookup == nullptr) {
        return nullptr;
    }
    DB2LOOKUPITER it;
    WowDB2LookupBegin(lookup, &it);
    while (it.m_index != -2) {
        auto record = static_cast<const uint8_t*>(it.m_record);
        if (DB2FieldU8(record, 6, 0, &g_soundBusDB) == id) {
            return record;
        }
        lookup->Next(&it);
    }
    return nullptr;
}

}

// ref: FUN_1412d8840
void SE3BusInit(SE3CONFIG* config) {
    if (s_ambienceCutoff == nullptr) {
        s_ambienceCutoff = CVarRegister("Sound_AmbienceHighpassDSPCutoff",
            "The cutoff value to use for the Highpass filter on the Ambience bus (default 100 Hz)", 0, "100",
            &HighpassCutoffChanged, 7, false, s_ambienceHighpass, false);
    }
    if (s_allyCutoff == nullptr) {
        s_allyCutoff = CVarRegister("Sound_AllyPlayerHighpassDSPCutoff",
            "The cutoff value to use for the Highpass filter on the Ally Player bus (default 80 Hz)", 0, "80",
            &HighpassCutoffChanged, 7, false, s_allyHighpass, false);
    }
    if (s_enemyCutoff == nullptr) {
        s_enemyCutoff = CVarRegister("Sound_EnemyPlayerHighpassDSPCutoff",
            "The cutoff value to use for the Highpass filter on the Enemy Player bus (default 80 Hz)", 0, "80",
            &HighpassCutoffChanged, 7, false, s_enemyHighpass, false);
    }
    if (s_npcCutoff == nullptr) {
        s_npcCutoff = CVarRegister("Sound_NPCHighpassDSPCutoff",
            "The cutoff value to use for the Highpass filter on the NPC bus (default 80 Hz)", 0, "80",
            &HighpassCutoffChanged, 7, false, s_npcHighpass, false);
    }
    ClearBuses();
    Reserve(&g_se3Buses, SE3BUS_COUNT);
    SetCount(&g_se3Buses, SE3BUS_COUNT);
    const uint8_t* master = BusRecord(SE3BUS_MASTER);
    if (master != nullptr) {
        s_masterRecord = DB2FieldI32(master, 0, 0, &g_soundBusDB);
        CreateBus(master, -1, true);
    }
    *VolumeSlot(SE3BUS_MASTER) = config->m_masterVolume;
    *VolumeSlot(SE3BUS_SFX) = config->m_sfxVolume;
    *VolumeSlot(SE3BUS_MUSIC) = config->m_musicVolume;
    *VolumeSlot(SE3BUS_AMBIENCE) = config->m_ambienceVolume;
    *VolumeSlot(SE3BUS_DIALOG) = config->m_dialogVolume;
    *VolumeSlot(SE3BUS_17) = config->m_volume20;
    g_se3Buses.m_data[SE3BUS_MASTER].m_enabled = config->m_enableAllSound != 0;
    g_se3Buses.m_data[SE3BUS_SFX].m_enabled = config->m_enableSFX != 0;
    g_se3Buses.m_data[SE3BUS_MUSIC].m_enabled = config->m_enableMusic != 0;
    g_se3Buses.m_data[SE3BUS_AMBIENCE].m_enabled = config->m_enableAmbience != 0;
    g_se3Buses.m_data[SE3BUS_DIALOG].m_enabled = config->m_enableDialog != 0;
}

// ref: FUN_1412d9110
void SE3BusShutdown() {
    DestroyBus(SE3BUS_MASTER);
    ClearBuses();
}

// ref: FUN_1412d7df0
void SE3BusSetEnabled(int8_t bus, bool enabled) {
    if (static_cast<uint32_t>(static_cast<int32_t>(bus)) < g_se3Buses.m_count) {
        g_se3Buses.m_data[bus].m_enabled = enabled;
    }
}

// ref: FUN_1412d8d50
void SE3BusSetMuted(int8_t bus, bool muted) {
    if (static_cast<uint32_t>(static_cast<int32_t>(bus)) < g_se3Buses.m_count) {
        g_se3Buses.m_data[bus].m_muted = muted;
    }
}

// Whether a bus is muted, or with `inherited` any bus above it.
// ref: FUN_1412d8d00
bool SE3BusIsMuted(int8_t bus, bool inherited) {
    if (static_cast<uint32_t>(static_cast<int32_t>(bus)) < g_se3Buses.m_count) {
        do {
            const SE3SoundBus& entry = g_se3Buses.m_data[bus];
            if (entry.m_muted != 0) {
                return true;
            }
            bus = entry.m_parent;
        } while (inherited && bus != -1);
    }
    return false;
}

// ref: FUN_1412d8e60
void SE3BusSetVolume(int8_t bus, float volume) {
    if (blz::chained_find<VOLUMENODE>(&s_volumes, bus) == nullptr) {
        return;
    }
    float clamped = 0.0f;
    if (0.0f <= volume) {
        clamped = volume < 1.0f ? volume : 1.0f;
    }
    *VolumeSlot(bus) = clamped;
}

// ref: FUN_1412d7e70
bool SE3BusGetFlag(int8_t bus) {
    if (static_cast<uint32_t>(static_cast<int32_t>(bus)) < g_se3Buses.m_count) {
        return g_se3Buses.m_data[bus].m_flag != 0;
    }
    return false;
}

float SE3BusGetVolume(int8_t bus) {
    VOLUMENODE* node = blz::chained_find<VOLUMENODE>(&s_volumes, bus);
    return node != nullptr ? node->m_value : 1.0f;
}

// ref: FUN_1412d7860
int8_t SE3BusFromRecord(int32_t id) {
    if (id == 0) {
        return SE3BUS_COUNT;
    }
    bool local;
    auto record = static_cast<const uint8_t*>(g_soundBusDB.GetRecord(static_cast<uint32_t>(id), false, &local));
    if (record != nullptr) {
        return static_cast<int8_t>(DB2FieldU8(record, 6, 0, &g_soundBusDB));
    }
    return SE3BUS_COUNT;
}

namespace {

bool InRange(int8_t bus) {
    return static_cast<uint32_t>(static_cast<int32_t>(bus)) < g_se3Buses.m_count;
}

}

// Whether a bus is `ancestor` or under it (-2: any).
// ref: FUN_1412d8ba0
bool SE3BusIsUnder(int8_t bus, int8_t ancestor) {
    if (ancestor == -2) {
        return true;
    }
    if (!InRange(bus)) {
        return false;
    }
    while (bus != ancestor) {
        bus = g_se3Buses.m_data[bus].m_parent;
        if (bus == -1) {
            return false;
        }
    }
    return true;
}

// Puts a channel in its bus's channel group.
// ref: FUN_1412d7cf0
bool SE3BusAttach(int8_t bus, FMOD::Channel* channel) {
    if (!InRange(bus)) {
        return false;
    }
    return channel->setChannelGroup(g_se3Buses.m_data[bus].m_group) == FMOD_OK;
}

// ref: FUN_1412d7ea0
uint8_t SE3BusGetPriority(int8_t bus) {
    return InRange(bus) ? g_se3Buses.m_data[bus].m_29 : 0x80;
}

// ref: FUN_1412d7ed0
uint8_t SE3BusGetPriorityRange(int8_t bus) {
    return InRange(bus) ? g_se3Buses.m_data[bus].m_2b : 0;
}

// An instance joins its bus's list.
// ref: FUN_1412d81d0
void SE3BusLinkObject(SE3SoundKitObject* object) {
    int8_t bus = object->m_params.m_bus;
    if (!InRange(bus)) {
        return;
    }
    SE3SoundBus* entry = &g_se3Buses.m_data[bus];
    object->m_bus.Unlink();
    entry->m_kits.LinkToTail(object);
}

// ref: FUN_1412d7d80
void SE3BusUnlinkObject(SE3SoundKitObject* object) {
    if (InRange(object->m_params.m_bus)) {
        object->m_bus.Unlink();
    }
}

// Whether a bus and every bus above it are enabled.
// ref: FUN_1412d8be0
bool SE3BusIsEnabled(int8_t bus) {
    if (InRange(bus)) {
        do {
            if (g_se3Buses.m_data[bus].m_enabled == 0) {
                return false;
            }
            bus = g_se3Buses.m_data[bus].m_parent;
        } while (bus != -1);
    }
    return true;
}

// Whether a bus holds its most instances (SoundBus field 5); a bus that counts only audible
// ones skips the inaudible.
// ref: FUN_1412d8c20
bool SE3BusIsFull(int8_t bus) {
    if (!InRange(bus)) {
        return false;
    }
    SE3SoundBus* entry = &g_se3Buses.m_data[bus];
    int32_t count = 0;
    for (SE3SoundKitObject* object = entry->m_kits.Head(); object != nullptr; object = entry->m_kits.Next(object)) {
        if (entry->m_09 == 0 || !SE3IsInaudible(object)) {
            count++;
        }
    }
    return static_cast<int32_t>(entry->m_1d) <= count;
}

// Every bus but 'a' sends to the reverb.
// ref: FUN_1412d8d70
bool SE3BusAllowsReverb(int8_t bus) {
    return bus != 'a';
}

// A bus's volume: its own and every parent's (0 when muted or disabled), times the volumes the
// game set.
// ref: FUN_1412d80b0
float SE3BusGetChainVolume(int8_t bus) {
    if (!SE3IsInitialized()) {
        return 0.0f;
    }
    float volume = 1.0f;
    if (InRange(bus)) {
        int8_t each = bus;
        do {
            SE3SoundBus* entry = &g_se3Buses.m_data[each];
            volume *= entry->m_muted == 0 && entry->m_enabled != 0 ? entry->m_targetVolume : 0.0f;
            if (VOLUMENODE* node = blz::chained_find<VOLUMENODE>(&s_volumes, each)) {
                volume *= node->m_value;
            }
            each = entry->m_parent;
        } while (static_cast<uint8_t>(each) != 0xff);
    }
    return volume;
}

// An instance's bus volume, ducked by the suppressors of the category its bus is under.
// ref: FUN_1412d7f00
float SE3BusGetObjectVolume(int8_t bus, uint32_t handle) {
    float duck = 1.0f;
    bool ownKitDucks = false;
    SE3SoundKitObject* object = SE3GetObject(handle);
    if (object != nullptr && object->m_def != nullptr) {
        ownKitDucks = SE3KitIgnoresSuppressors(object->m_def);
    }
    if (SE3SuppressorsAny() && !ownKitDucks) {
        float targets[7];
        SE3SuppressorGetTargets(handle, targets);
        if (targets[4] < 1.0f) {
            return SE3BusGetChainVolume(bus);
        }
        for (int8_t category : {static_cast<int8_t>(3), static_cast<int8_t>(0), static_cast<int8_t>(1),
                 static_cast<int8_t>(2), static_cast<int8_t>('A'), static_cast<int8_t>('B')}) {
            if (SE3BusIsUnder(bus, category)) {
                duck = SE3SuppressorFactor(category, handle);
                break;
            }
        }
    }
    return SE3GlobalVolume() * SE3BusGetChainVolume(bus) * duck;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a96d0 = (void*)&s_ambienceHighpass;
extern "C" void* const gen_p_1430a96d8 = (void*)&s_allyHighpass;
extern "C" void* const gen_p_1430a96e0 = (void*)&s_enemyHighpass;
extern "C" void* const gen_p_1430a96e8 = (void*)&s_npcHighpass;
extern "C" void* const gen_p_1430a96f0 = (void*)&s_ambienceCutoff;
extern "C" void* const gen_p_1430a96f8 = (void*)&s_allyCutoff;
extern "C" void* const gen_p_1430a9700 = (void*)&s_enemyCutoff;
extern "C" void* const gen_p_1430a9708 = (void*)&s_npcCutoff;
// ---- end of generated globals ----
