// SE3 (Engine/Source/SoundEngine/SE3.cpp): sound kit definitions, the handles, sound kit
// instances, and starting and stopping them. The frame's passes over the instances are in
// SE3Instance.cpp.

#include "sound/SE3Kit.hpp"

#include <cmath>
#include <cstring>
#include <new>

#include "base/ObjectAlloc.hpp"
#include "base/RndSeed.hpp"
#include "blz/RbTree.hpp"
#include "cas/CAS.hpp"
#include "cas/CASManifest.hpp"
#include "db/DB2Tables.hpp"
#include "db/ParameterCurve.hpp"
#include "db/WowClientDB2.hpp"
#include "db/WowDB2Lookup.hpp"
#include "fileservices/AsyncFile.hpp"
#include "fmod.hpp"
#include "os/Os.hpp"
#include "sound/SE3.hpp"
#include "sound/SE3Bus.hpp"
#include "sound/SE3Internal.hpp"
#include <cstddef>
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "ui/FrameTime.hpp"
#include "util/Unimplemented.hpp"

SE3HANDLETABLE g_se3Handles;
SE3COMBINEDTABLE g_se3Combined;
int32_t g_se3LastHandle;
// Each list threads its own link in the instance (the reference keeps the offset as the list's
// first word: 0x142cfcda8 holds 0x358, ...). Built at +0 they all threaded m_playing, so a fade
// re-linked a playing instance into the fading list and the two lists closed into a cycle
// (SE3UpdateFades spun forever, 2026-10-13).
SE3KITLIST g_se3Playing(offsetof(SE3SoundKitObject, m_playing));
SE3KITLIST g_se3Loading(offsetof(SE3SoundKitObject, m_loading));
SE3KITLIST g_se3Pending(offsetof(SE3SoundKitObject, m_pending));
SE3KITLIST g_se3Release(offsetof(SE3SoundKitObject, m_release));
SE3KITLIST g_se3Fading(offsetof(SE3SoundKitObject, m_fading));
SE3KITLIST g_se3Timed(offsetof(SE3SoundKitObject, m_timed));
SE3KITLIST g_se3Waiting(offsetof(SE3SoundKitObject, m_waiting));
static_assert(offsetof(SE3SoundKitObject, m_loading) == 0x358 && offsetof(SE3SoundKitObject, m_pending) == 0x368
                  && offsetof(SE3SoundKitObject, m_release) == 0x378 && offsetof(SE3SoundKitObject, m_fading) == 0x388
                  && offsetof(SE3SoundKitObject, m_waiting) == 0x3a8 && offsetof(SE3SoundKitObject, m_timed) == 0x3d8,
              "the instance lists' links are where the reference keeps them");
TSList<SE3SoundKitDef, TSGetLink<SE3SoundKitDef>, 8> g_se3DefTimers(offsetof(SE3SoundKitDef, m_timerLink));

namespace {

const char s_file[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\SoundEngine\\SE3.cpp";
const char s_handleTag[] = ".?AV?$TSHashTable@USE3SoundKitHandle@@VHASHKEY_DWORD@@$07@@";
const char s_combinedTag[] = ".?AV?$TSHashTable@USE3CombinedSoundKitHandle@@VHASHKEY_DWORD@@$07@@";
const char s_kitArrayTag[] = ".?AV?$TSGrowableArray@H$07@@";
const char s_defNodeTag[] = ".?AU?$rb_node@U?$pair@$$CBHPEAVSE3SoundKitDef@@@blz@@@blz@@";

// The engine's random stream (the table is base's g_rndSeedTable, DAT_142734470).
uint32_t s_randomCursors;               // DAT_1430a9534
uint32_t s_randomSum;                   // DAT_1430a9530

uint32_t RandomWord(uint32_t offset) {
    uint32_t word;
    std::memcpy(&word, g_rndSeedTable + offset, sizeof(word));
    return word;
}

uint32_t Rotl(uint32_t value, int32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

// The kit definitions by SoundKit id (DAT_142cfcf10): blz::map<int, SE3SoundKitDef*>.
struct DEFNODE {
    blz::rb_node_base m_base;           // 0x00
    int32_t m_key;                      // 0x20
    SE3SoundKitDef* m_def;              // 0x28
};
struct DEFMAP {
    blz::rb_node_base m_header;         // parent = root, left = leftmost, right = rightmost
    uint64_t m_size;
};
DEFMAP s_defs;

void DefMapInit() {
    if (s_defs.m_header.m_parent == nullptr && s_defs.m_header.m_left == nullptr) {
        s_defs.m_header.m_left = &s_defs.m_header;
        s_defs.m_header.m_right = &s_defs.m_header;
    }
}

DEFNODE* DefFind(int32_t key) {
    blz::rb_node_base* result = &s_defs.m_header;
    for (blz::rb_node_base* node = s_defs.m_header.m_parent; node != nullptr;) {
        if (reinterpret_cast<DEFNODE*>(node)->m_key < key) {
            node = node->m_right;
        } else {
            result = node;
            node = node->m_left;
        }
    }
    if (result == &s_defs.m_header || key < reinterpret_cast<DEFNODE*>(result)->m_key) {
        return nullptr;
    }
    return reinterpret_cast<DEFNODE*>(result);
}

// The slot of `key`, made when missing.
// ref: FUN_1412c7010
SE3SoundKitDef** DefInsert(int32_t key) {
    DefMapInit();
    blz::rb_node_base* parent = &s_defs.m_header;
    blz::rb_node_base** link = &s_defs.m_header.m_parent;
    bool left = true;
    for (blz::rb_node_base* node = s_defs.m_header.m_parent; node != nullptr;) {
        parent = node;
        int32_t nodeKey = reinterpret_cast<DEFNODE*>(node)->m_key;
        if (key < nodeKey) {
            link = &node->m_left;
            left = true;
            node = node->m_left;
        } else if (nodeKey < key) {
            link = &node->m_right;
            left = false;
            node = node->m_right;
        } else {
            return &reinterpret_cast<DEFNODE*>(node)->m_def;
        }
    }
    auto created = static_cast<DEFNODE*>(SMemAlloc(sizeof(DEFNODE), 0xb, s_defNodeTag, SStrHashFnv1a(s_defNodeTag), -6, 0));
    created->m_key = key;
    created->m_def = nullptr;
    created->m_base.m_parent = parent;
    created->m_base.m_left = nullptr;
    created->m_base.m_right = nullptr;
    created->m_base.m_color = 0;
    s_defs.m_size++;
    *link = &created->m_base;
    if (parent == &s_defs.m_header) {
        s_defs.m_header.m_parent = &created->m_base;
        s_defs.m_header.m_left = &created->m_base;
        s_defs.m_header.m_right = &created->m_base;
    } else {
        if (left && parent == s_defs.m_header.m_left) {
            s_defs.m_header.m_left = &created->m_base;
        }
        if (!left && parent == s_defs.m_header.m_right) {
            s_defs.m_header.m_right = &created->m_base;
        }
    }
    blz::rb_insert_fixup(&s_defs.m_header.m_parent, &created->m_base);
    return &created->m_def;
}

const uint8_t* SoundKitRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

const uint8_t* AdvancedRecord(int32_t id) {
    bool local;
    return static_cast<const uint8_t*>(g_soundKitAdvancedDB.GetRecord(static_cast<uint32_t>(id), false, &local));
}

}

// ---- random -------------------------------------------------------------------------------

uint32_t SE3Random() {
    uint32_t b3 = s_randomCursors >> 24;
    uint32_t b2 = (s_randomCursors >> 16) & 0xff;
    uint32_t b1 = (s_randomCursors >> 8) & 0xff;
    uint32_t b0 = s_randomCursors & 0xff;
    uint32_t i3 = 3 < b3 ? b3 - 4 : b3 + 0xb8;
    uint32_t i2 = 0xb < b2 ? b2 - 0xc : b2 + 200;
    uint32_t i1 = 0x17 < b1 ? b1 - 0x18 : b1 + 0xd4;
    uint32_t i0 = 0x1b < b0 ? b0 - 0x1c : b0 + 0xd8;
    s_randomCursors = ((i3 << 8 | i2) << 8 | i1) << 8 | i0;
    s_randomSum += Rotl(RandomWord(i1), 3) ^ Rotl(RandomWord(i2), 2) ^ Rotl(RandomWord(i3), 1) ^ RandomWord(i0);
    return s_randomSum;
}

// ref: FUN_1412d2160
int32_t SE3RandomRange(int32_t low, int32_t high) {
    if (high <= low) {
        return high;
    }
    uint32_t sum = SE3Random();
    return static_cast<int32_t>((static_cast<uint64_t>(static_cast<uint32_t>(high - low)) * sum) >> 32) + low;
}

// ---- play parameters ----------------------------------------------------------------------

namespace {

void ResetFunction(blz::function_storage* function) {
    function->m_manager(function->m_storage, nullptr, 0);
    function->m_invoke = nullptr;
    function->m_manager = &blz::function_empty_manager;
}

void CopyFunction(blz::function_storage* dest, const blz::function_storage* source) {
    if (dest == source) {
        return;
    }
    dest->m_manager(dest->m_storage, nullptr, 0);
    source->m_manager(dest->m_storage, const_cast<uint8_t*>(source->m_storage), 1);
    dest->m_invoke = source->m_invoke;
    dest->m_manager = source->m_manager;
}

}

// ref: FUN_1412d25a0
void SE3PlayParamsInit(SE3PLAYPARAMS* params) {
    params->m_a8[0] = 0;
    params->m_a8[1] = 0;
    ResetFunction(&params->m_88);
    params->m_bus = 0;
    params->m_04 = -1.0f;
    params->m_08 = 1.0f;
    params->m_entry = -1;
    params->m_10 = 2;
    params->m_3dMode = 0;
    params->m_18 = 4;
    params->m_1c = 1.0f;
    params->m_priority = 4;
    params->m_duplicates = 0;
    params->m_2c = 0;
    params->m_minDistance = -1.0f;
    params->m_maxDistance = -1.0f;
    params->m_38 = 2;
    params->m_3c = 0.5f;
    params->m_135 = 0;
    params->m_136 = 1;
    params->m_137 = 0;
    params->m_138 = 0;
    params->m_13a = 0;
    params->m_fadeInType = 1;
    params->m_fadeInMs = 0;
    params->m_fadeInCurve = 0xc57;
    params->m_fadeOutType = 1;
    params->m_fadeOutMs = 0;
    params->m_fadeOutCurve = 0xc58;
    params->m_fadeOutStart = 0.0f;
    params->m_parent = nullptr;
    params->m_fileDataId = 0;
    params->m_24 = 0x80;
    ResetFunction(&params->m_48);
    params->m_68 = 0;
    params->m_stopDuplicates = 1;
    params->m_checkRange = 1;
    params->m_13c = 0;
    params->m_13d = 0;
    params->m_prefetchOnly = 0;
    params->m_combinable = 0;
    params->m_142 = 0;
    params->m_143 = 0;
    params->m_144 = 0;
    params->m_b8 = 0;
    ResetFunction(&params->m_c0);
    params->m_e0 = 0;
    ResetFunction(&params->m_e8);
    params->m_108[0] = 0;
    params->m_108[1] = 0;
    params->m_40 = 0;
    params->m_13f = 0;
    params->m_140 = 0;
    params->m_141 = 0;
    params->m_7c[0] = 0.0f;
    params->m_7c[1] = 0.0f;
    params->m_7c[2] = 0.0f;
}

// ref: FUN_1412c5dc0
void SE3PlayParamsConstruct(SE3PLAYPARAMS* params) {
    std::memset(params, 0, sizeof(*params));
    params->m_entry = -1;
    params->m_48.m_manager = &blz::function_empty_manager;
    params->m_88.m_manager = &blz::function_empty_manager;
    params->m_c0.m_manager = &blz::function_empty_manager;
    params->m_e8.m_manager = &blz::function_empty_manager;
    SE3PlayParamsInit(params);
}

// ref: FUN_1412c6ce0
SE3PLAYPARAMS* SE3PlayParamsCopy(SE3PLAYPARAMS* dest, const SE3PLAYPARAMS* source) {
    if (dest == source) {
        return dest;
    }
    dest->m_bus = source->m_bus;
    dest->m_04 = source->m_04;
    dest->m_08 = source->m_08;
    dest->m_entry = source->m_entry;
    dest->m_10 = source->m_10;
    dest->m_3dMode = source->m_3dMode;
    dest->m_18 = source->m_18;
    dest->m_1c = source->m_1c;
    dest->m_priority = source->m_priority;
    dest->m_24 = source->m_24;
    dest->m_duplicates = source->m_duplicates;
    dest->m_2c = source->m_2c;
    dest->m_minDistance = source->m_minDistance;
    dest->m_maxDistance = source->m_maxDistance;
    dest->m_38 = source->m_38;
    dest->m_3c = source->m_3c;
    dest->m_40 = source->m_40;
    CopyFunction(&dest->m_48, &source->m_48);
    dest->m_68 = source->m_68;
    dest->m_parent = source->m_parent;
    dest->m_fileDataId = source->m_fileDataId;
    std::memcpy(dest->m_7c, source->m_7c, sizeof(dest->m_7c));
    CopyFunction(&dest->m_88, &source->m_88);
    std::memcpy(dest->m_a8, source->m_a8, sizeof(dest->m_a8));
    dest->m_b8 = source->m_b8;
    CopyFunction(&dest->m_c0, &source->m_c0);
    dest->m_e0 = source->m_e0;
    CopyFunction(&dest->m_e8, &source->m_e8);
    std::memcpy(dest->m_108, source->m_108, sizeof(dest->m_108));
    dest->m_fadeInType = source->m_fadeInType;
    dest->m_fadeInMs = source->m_fadeInMs;
    dest->m_fadeInCurve = source->m_fadeInCurve;
    dest->m_fadeOutType = source->m_fadeOutType;
    dest->m_fadeOutMs = source->m_fadeOutMs;
    dest->m_fadeOutCurve = source->m_fadeOutCurve;
    dest->m_fadeOutStart = source->m_fadeOutStart;
    std::memcpy(&dest->m_combinable, &source->m_combinable, 0x11);
    return dest;
}

void SE3PlayParamsDestroy(SE3PLAYPARAMS* params) {
    params->m_e8.m_manager(params->m_e8.m_storage, nullptr, 0);
    params->m_c0.m_manager(params->m_c0.m_storage, nullptr, 0);
    params->m_88.m_manager(params->m_88.m_storage, nullptr, 0);
    params->m_48.m_manager(params->m_48.m_storage, nullptr, 0);
}

// ---- the handle tables --------------------------------------------------------------------

void SE3HANDLETABLE::Delete(SE3HANDLENODE* node) {
    node->~SE3HANDLENODE();
    SMemFree(node, s_handleTag, -2, 0);
}

SE3HANDLENODE* SE3HANDLETABLE::NewNode(TSExplicitList<SE3HANDLENODE>* slot, uint32_t extra, uint32_t flags) {
    void* memory = SMemAlloc(extra + sizeof(SE3HANDLENODE), 8, s_handleTag, 0, -2, flags | 8);
    auto node = new (memory) SE3HANDLENODE();
    node->m_object = nullptr;
    slot->LinkToTail(node);
    return node;
}

void SE3COMBINEDTABLE::Delete(SE3COMBINEDNODE* node) {
    if (node->m_handles != nullptr) {
        SMemFree(node->m_handles, s_kitArrayTag, -2, 0);
    }
    node->~SE3COMBINEDNODE();
    SMemFree(node, s_combinedTag, -2, 0);
}

// The combined handle's node takes the next handle as its own (FUN_1412c52c0).
SE3COMBINEDNODE* SE3COMBINEDTABLE::NewNode(TSExplicitList<SE3COMBINEDNODE>* slot, uint32_t extra, uint32_t flags) {
    void* memory = SMemAlloc(extra + sizeof(SE3COMBINEDNODE), 8, s_combinedTag, 0, -2, flags | 8);
    auto node = new (memory) SE3COMBINEDNODE();
    node->m_30 = ++g_se3LastHandle;
    node->m_count = 0;
    node->m_handles = nullptr;
    node->m_capacity = 0;
    node->m_granularity = 0;
    slot->LinkToTail(node);
    return node;
}

namespace {

SE3HANDLENODE* FindHandle(uint32_t handle) {
    return g_se3Handles.Find(handle, [handle](SE3HANDLENODE* node) {
        return static_cast<uint32_t>(node->m_hashval) == handle;
    });
}

SE3COMBINEDNODE* FindCombined(uint32_t handle) {
    return g_se3Combined.Find(handle, [handle](SE3COMBINEDNODE* node) {
        return static_cast<uint32_t>(node->m_hashval) == handle;
    });
}

}

// TSGrowableArray<uint32_t>::Add (granularity up to 0x40).
// ref: FUN_1412c76a0
void SE3CombinedAdd(SE3COMBINEDNODE* node, uint32_t handle) {
    uint32_t count = node->m_count + 1;
    if (node->m_capacity < count) {
        uint32_t granularity = node->m_granularity;
        if (granularity == 0) {
            if (count < 0x40) {
                granularity = count;
                for (uint32_t low = (count - 1) & count; low != 0; low = (low - 1) & low) {
                    granularity = low;
                }
                if (granularity == 0) {
                    granularity = 1;
                }
            } else {
                node->m_granularity = 0x40;
                granularity = 0x40;
            }
        }
        if (count % granularity != 0) {
            count += granularity - count % granularity;
        }
        uint32_t* old = node->m_handles;
        node->m_capacity = count;
        node->m_handles = static_cast<uint32_t*>(SMemAlloc(static_cast<uint64_t>(count) * 4, 8, s_kitArrayTag, 0, -7, 0));
        if (old != nullptr) {
            std::memcpy(node->m_handles, old, static_cast<size_t>(node->m_count) * 4);
            SMemFree(old, s_kitArrayTag, -2, 0);
        }
    }
    node->m_handles[node->m_count++] = handle;
}

// ref: FUN_1412c9d80
SE3SoundKitObject* SE3GetObject(uint32_t handle) {
    while (handle != 0) {
        if (SE3HANDLENODE* node = FindHandle(handle)) {
            if (node->m_object != nullptr) {
                return node->m_object;
            }
        }
        SE3COMBINEDNODE* combined = FindCombined(handle);
        if (combined == nullptr || combined->m_count == 0) {
            return nullptr;
        }
        handle = combined->m_handles[0];
    }
    return nullptr;
}

namespace {

bool IsAlive(const SE3SoundKitObject* object) {
    return object->m_playing.m_next != nullptr || object->m_loading.m_next != nullptr
        || object->m_pending.m_next != nullptr || object->m_waiting.m_next != nullptr;
}

}

// ref: FUN_1412cd270
bool SE3IsPlaying(uint32_t handle) {
    SE3SoundKitObject* object = SE3GetObject(handle);
    return object != nullptr && IsAlive(object);
}

// An instance's first channel paused or resumed; a combined handle passes it to each member.
// ref: FUN_1412cde60
void SE3SetPaused(uint32_t handle, bool paused) {
    if (SE3HANDLENODE* node = FindHandle(handle)) {
        if (node->m_object != nullptr && node->m_object->m_slots[0].m_channel != nullptr) {
            node->m_object->m_slots[0].m_channel->setPaused(paused);
        }
        return;
    }
    if (SE3COMBINEDNODE* combined = FindCombined(handle)) {
        for (uint32_t i = 0; i < combined->m_count; i++) {
            SE3SetPaused(combined->m_handles[i], paused);
        }
    }
}

// Where an instance's first channel is, in milliseconds (4 for none, as the reference answers).
// ref: FUN_1412c96d0
uint32_t SE3GetPositionMs(uint32_t handle) {
    if (handle != 0) {
        if (SE3HANDLENODE* node = FindHandle(handle)) {
            if (node->m_object != nullptr && node->m_object->m_slots[0].m_channel != nullptr) {
                unsigned int position = 0;
                node->m_object->m_slots[0].m_channel->getPosition(&position, FMOD_TIMEUNIT_MS);
                return position;
            }
        }
    }
    return 4;
}

// ref: FUN_1412c9c50
int32_t SE3GetKitId(uint32_t handle) {
    SE3SoundKitObject* object = SE3GetObject(handle);
    if (object != nullptr && object->m_def != nullptr) {
        return object->m_def->m_kitId;
    }
    return 0;
}

// ---- kit definitions ----------------------------------------------------------------------

namespace {

// The definition's SoundKit fields: its entries, its bus, and whether it has a time window.
// ref: FUN_1412d3af0
void DefLoad(SE3SoundKitDef* def, int32_t kitId, int32_t advancedId) {
    def->m_kitId = kitId;
    def->m_advancedId = advancedId;
    WowDB2ParentLookup* entries = g_soundKitEntryDB.m_parentLookup;
    int32_t count = 0;
    if (entries != nullptr) {
        DB2PARENTITER it;
        DB2PARENTITER end;
        entries->Begin(&it, kitId);
        entries->End(&end);
        while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
               || it.m_source != end.m_source) {
            count++;
            entries->Next(&it);
        }
    }
    def->m_entryCount = count;
    if (def->m_entryWeights != nullptr) {
        SMemFree(def->m_entryWeights, s_file, 0x6f6, 0);
        def->m_entryWeights = nullptr;
    }
    if (1 < def->m_entryCount) {
        def->m_entryWeights = static_cast<uint8_t*>(
            SMemAlloc(static_cast<uint64_t>(def->m_entryCount), 0x16, s_file, SStrHashFnv1a(s_file), 0x6fd, 0));
        if (def->m_entryWeights != nullptr) {
            std::memset(def->m_entryWeights, 0, static_cast<size_t>(def->m_entryCount));
        }
    }
    if (const uint8_t* kit = SoundKitRecord(kitId)) {
        uint16_t bus = DB2FieldU16(kit, 0xe, 0, &g_soundKitDB);
        def->m_bus = bus == 0 ? SE3BUS_COUNT : SE3BusFromRecord(bus);
    }
    if (const uint8_t* advanced = AdvancedRecord(def->m_advancedId)) {
        for (uint32_t field = 4; field < 8; field++) {
            if (DB2FieldU32(advanced, field, 0, &g_soundKitAdvancedDB) != 0) {
                def->m_hasTimeWindow = 1;
                break;
            }
        }
    }
}

// Whether the kit's envelopes can be followed: each SoundEnvelope's curve must reach its
// sustain and release points in order; m_envelopeBusy stays set when one cannot.
// ref: FUN_1412d7160
void DefCheckEnvelopes(SE3SoundKitDef* def) {
    def->m_envelopeBusy = 1;
    WowDB2ParentLookup* envelopes = g_soundEnvelopeDB.m_parentLookup;
    if (envelopes != nullptr) {
        DB2PARENTITER it;
        DB2PARENTITER end;
        envelopes->Begin(&it, def->m_kitId);
        envelopes->End(&end);
        while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
               || it.m_source != end.m_source) {
            auto record = static_cast<const uint8_t*>(envelopes->Record(&it));
            uint32_t curve = static_cast<uint32_t>(DB2FieldI32(record, 3, 0, &g_soundEnvelopeDB));
            uint32_t points = ParameterCurveGetPointCount(curve);
            if (points < 2) {
                return;
            }
            uint16_t attack = DB2FieldU16(record, 4, 0, &g_soundEnvelopeDB);
            if (static_cast<int32_t>(points) <= attack) {
                return;
            }
            uint16_t sustain = DB2FieldU16(record, 5, 0, &g_soundEnvelopeDB);
            if (sustain < attack || static_cast<int32_t>(points) <= sustain) {
                return;
            }
            uint16_t release = DB2FieldU16(record, 6, 0, &g_soundEnvelopeDB);
            if (release < sustain) {
                return;
            }
            if (static_cast<int32_t>(points) <= release) {
                break;
            }
            C2VECTORF first;
            C2VECTORF last;
            C2VECTORF attackPoint;
            C2VECTORF sustainPoint;
            C2VECTORF releasePoint;
            if (!ParameterCurveGetFirstPoint(curve, &first) || !ParameterCurveGetLastPoint(curve, &last)
                || !ParameterCurveGetPoint(curve, attack, &attackPoint)
                || !ParameterCurveGetPoint(curve, sustain, &sustainPoint)
                || !ParameterCurveGetPoint(curve, release, &releasePoint)) {
                return;
            }
            (void)attackPoint;
            (void)releasePoint;
            bool oneShot = SE3KitIsLooping(def->m_kitId);
            if (oneShot && std::fabs(0.0f - sustainPoint.y) < 2.3841858e-07f) {
                return;
            }
            if ((DB2FieldU32(record, 2, 0, &g_soundEnvelopeDB) & 1) != 0) {
                if (oneShot || 2.3841858e-07f <= std::fabs(first.y)
                    || 2.3841858e-07f <= std::fabs(last.y - 1.0f)) {
                    return;
                }
            }
            envelopes->Next(&it);
        }
    }
    def->m_envelopeBusy = 0;
}

}

// The kit's definition when one was made, without making it (inlined into FUN_1412cf720).
SE3SoundKitDef* SE3FindKitDef(int32_t kitId) {
    DEFNODE* node = DefFind(kitId);
    return node != nullptr ? node->m_def : nullptr;
}

// The definition of a SoundKit, made on first use.
// ref: FUN_1412c93c0
SE3SoundKitDef* SE3GetKitDef(int32_t kitId) {
    DefMapInit();
    if (DEFNODE* node = DefFind(kitId)) {
        if (node->m_def != nullptr) {
            return node->m_def;
        }
    }
    const uint8_t* kit = SoundKitRecord(kitId);
    if (kit == nullptr) {
        return nullptr;
    }
    // FUN_1412cc8a0
    auto def = static_cast<SE3SoundKitDef*>(g_soundKitDefHeap->Alloc());
    def->m_link.m_prevlink = nullptr;
    def->m_link.m_next = nullptr;
    new (&def->m_instances) TSList<SE3SoundKitObject, TSGetLink<SE3SoundKitObject>, 8>();
    def->m_instances.m_linkoffset = 0x3c8;
    def->m_entryCount = 0;
    def->m_kitId = 0;
    def->m_advancedId = 0;
    def->m_34 = 0;
    def->m_envelopeBusy = 0;
    def->m_hasTimeWindow = 0;
    def->m_3a = 0;
    def->m_lastEntry = -1;
    def->m_loadState = 2;
    def->m_bus = SE3BUS_COUNT;
    def->m_prefetch = nullptr;
    def->m_prefetchTime = 0;
    def->m_soundGroup = nullptr;
    def->m_timerLink.m_prevlink = nullptr;
    def->m_timerLink.m_next = nullptr;
    def->m_70 = 0;
    def->m_78 = 0;
    def->m_childCount = 0;
    def->m_children = nullptr;
    def->m_childCapacity = 0;
    def->m_childGranularity = 0;
    def->m_entryWeights = nullptr;
    *DefInsert(kitId) = def;
    DefLoad(def, kitId, DB2FieldI32(kit, 7, 0, &g_soundKitDB));
    if (3 < OsGetCpuInfo()->m_logical) {
        WowDB2ParentLookup* children = g_soundKitChildDB.m_parentLookup;
        if (children != nullptr) {
            DB2PARENTITER it;
            DB2PARENTITER end;
            children->Begin(&it, kitId);
            children->End(&end);
            while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
                   || it.m_source != end.m_source) {
                auto record = static_cast<const uint8_t*>(children->Record(&it));
                int32_t child = static_cast<int32_t>(DB2FieldU32(record, 0, 0, &g_soundKitChildDB));
                if (def->m_childCapacity < def->m_childCount + 1) {
                    uint32_t capacity = def->m_childCount + 8;
                    auto grown = static_cast<int32_t*>(SMemAlloc(static_cast<uint64_t>(capacity) * 4, 8, s_kitArrayTag, 0, -7, 0));
                    if (def->m_children != nullptr) {
                        std::memcpy(grown, def->m_children, static_cast<size_t>(def->m_childCount) * 4);
                        SMemFree(def->m_children, s_kitArrayTag, -2, 0);
                    }
                    def->m_children = grown;
                    def->m_childCapacity = capacity;
                }
                def->m_children[def->m_childCount++] = child;
                children->Next(&it);
            }
        }
    }
    DefCheckEnvelopes(def);
    return def;
}

// The index-th SoundKitEntry of a kit.
// ref: FUN_1412c9b20
const uint8_t* SE3GetKitEntry(SE3SoundKitDef* def, int32_t index) {
    WowDB2ParentLookup* entries = g_soundKitEntryDB.m_parentLookup;
    if (entries == nullptr) {
        return nullptr;
    }
    DB2PARENTITER it;
    DB2PARENTITER end;
    entries->Begin(&it, def->m_kitId);
    entries->End(&end);
    int32_t i = 0;
    while (it.m_entry != end.m_entry || it.m_overflow != end.m_overflow || it.m_parent != end.m_parent
           || it.m_source != end.m_source) {
        auto record = static_cast<const uint8_t*>(entries->Record(&it));
        if (i == index) {
            return record;
        }
        entries->Next(&it);
        i++;
    }
    return nullptr;
}

// The kit's FMOD sound group: its instance limit and behaviour from SoundKit fields 3 and 0xf
// and SoundKitAdvanced field 0x22.
// ref: FUN_1412c99c0
FMOD::SoundGroup* SE3GetSoundGroup(SE3SoundKitDef* def) {
    if (def->m_soundGroup != nullptr) {
        return def->m_soundGroup;
    }
    // the name is FUN_1406b0410(kitId, 0): its second argument, so none
    g_fmodSystem->createSoundGroup(nullptr, &def->m_soundGroup);
    if (def->m_soundGroup == nullptr) {
        return nullptr;
    }
    def->m_soundGroup->setMuteFadeSpeed(0.5f);
    int32_t maxAudible = 5;
    FMOD_SOUNDGROUP_BEHAVIOR behavior = FMOD_SOUNDGROUP_BEHAVIOR_STEALLOWEST;
    if (const uint8_t* kit = SoundKitRecord(def->m_kitId)) {
        uint16_t flags = DB2FieldU16(kit, 3, 0, &g_soundKitDB);
        int8_t limit = static_cast<int8_t>(DB2FieldU8(kit, 0xf, 0, &g_soundKitDB));
        if (((flags >> 9) & 1) == 0) {
            if (limit != 0) {
                maxAudible = limit;
            }
        } else {
            maxAudible = limit == 0 ? 4 : limit;
            behavior = FMOD_SOUNDGROUP_BEHAVIOR_MUTE;
        }
    }
    def->m_soundGroup->setMaxAudible(maxAudible);
    if (const uint8_t* advanced = AdvancedRecord(def->m_advancedId)) {
        switch (DB2FieldU8(advanced, 0x22, 0, &g_soundKitAdvancedDB)) {
            case 1:
                behavior = FMOD_SOUNDGROUP_BEHAVIOR_FAIL;
                break;
            case 2:
                behavior = FMOD_SOUNDGROUP_BEHAVIOR_MUTE;
                break;
            case 3:
                behavior = FMOD_SOUNDGROUP_BEHAVIOR_STEALLOWEST;
                break;
            default:
                break;
        }
    }
    def->m_soundGroup->setMaxAudibleBehavior(behavior);
    return def->m_soundGroup;
}

// ref: FUN_1412cd2c0
bool SE3KitIsLooping(int32_t kitId) {
    SE3SoundKitDef* def = SE3GetKitDef(kitId);
    if (def == nullptr) {
        return false;
    }
    if (const uint8_t* advanced = AdvancedRecord(def->m_advancedId)) {
        return DB2FieldU8(advanced, 9, 0, &g_soundKitAdvancedDB) == 0;
    }
    if (const uint8_t* kit = SoundKitRecord(kitId)) {
        return ((DB2FieldU16(kit, 3, 0, &g_soundKitDB) >> 1) & 1) != 0;
    }
    return false;
}

// ref: FUN_1412cd200
bool SE3IsMp3(int32_t fileDataId) {
    cas::BuildManifestEntry* entry = cas::g_casManifest->FindById(static_cast<uint32_t>(fileDataId));
    if (entry == nullptr) {
        return false;
    }
    return ((entry->m_contentFlags >> 0x11) & 0xff) == 2;  // FUN_1402d7e80
}

int32_t SE3AdvancedStartOffset(const SE3SoundKitDef* def) {
    if (const uint8_t* advanced = AdvancedRecord(def->m_advancedId)) {
        int32_t offset = DB2FieldI32(advanced, 0x1f, 0, &g_soundKitAdvancedDB);
        if (0 < offset) {
            return offset;
        }
    }
    return 0;
}

// ref: FUN_1412d3ce0
bool SE3KitIgnoresSuppressors(const SE3SoundKitDef* def) {
    if (const uint8_t* kit = SoundKitRecord(def->m_kitId)) {
        return ((DB2FieldU16(kit, 3, 0, &g_soundKitDB) >> 6) & 1) != 0;
    }
    return false;
}

// ref: FUN_1412cd160
bool SE3EntryResident(SE3SoundKitDef* def, int32_t entry) {
    const uint8_t* record = SE3GetKitEntry(def, entry);
    int32_t fileDataId = record != nullptr ? DB2FieldI32(record, 1, 0, &g_soundKitEntryDB) : 0;
    return cas::CASFileGetResidency(fileDataId, 0, 0) != 1;
}

namespace {

// A definition's destructor.
// ref: FUN_1412c64d0
void DefDestroy(SE3SoundKitDef* def) {
    if (def->m_entryWeights != nullptr) {
        SMemFree(def->m_entryWeights, s_file, 0x607, 0);
        def->m_entryWeights = nullptr;
    }
    if (def->m_soundGroup != nullptr) {
        def->m_soundGroup->release();
        def->m_soundGroup = nullptr;
    }
    def->m_prefetch = nullptr;
    if (def->m_children != nullptr) {
        SMemFree(def->m_children, s_kitArrayTag, -2, 0);
    }
    def->m_timerLink.Unlink();
    def->m_instances.~TSList();
    def->m_link.Unlink();
}

// ref: FUN_1412d75e0
void DefFreeNodes(blz::rb_node_base* node) {
    while (node != nullptr) {
        DefFreeNodes(node->m_right);
        blz::rb_node_base* left = node->m_left;
        SMemFree(node, s_defNodeTag, -6, 0);
        node = left;
    }
}

}

// Every definition: prefetches canceled, groups released, freed; then the map and the heap.
// ref: FUN_1412c91a0
void SE3ReleaseKitDefs() {
    DefMapInit();
    for (blz::rb_node_base* node = s_defs.m_header.m_left; node != &s_defs.m_header; node = blz::rb_next(node)) {
        SE3SoundKitDef* def = reinterpret_cast<DEFNODE*>(node)->m_def;
        if (def == nullptr) {
            continue;
        }
        def->m_timerLink.Unlink();
        if (def->m_prefetch != nullptr) {
            AsyncFileCancel(static_cast<CAsyncObject*>(def->m_prefetch));
            if (g_se3ClosestPrefetch == def->m_prefetch) {
                g_se3ClosestPrefetch = nullptr;
            }
            def->m_prefetch = nullptr;
        }
        if (g_soundKitDefHeap != nullptr) {
            DefDestroy(def);
            g_soundKitDefHeap->Free(def);
        }
    }
    DefFreeNodes(s_defs.m_header.m_parent);
    s_defs.m_header.m_parent = nullptr;
    s_defs.m_header.m_left = &s_defs.m_header;
    s_defs.m_header.m_right = &s_defs.m_header;
    s_defs.m_size = 0;
    if (g_soundKitDefHeap != nullptr) {
        g_soundKitDefHeap->~TObjectAllocHeap();
        SMemFree(g_soundKitDefHeap, s_file, -2, 0);
    }
    g_soundKitDefHeap = nullptr;
}

// The shutdown's frees: every instance left, the instance heap, the combined handles, and the
// messages (FUN_1412d41d0's middle).
void SE3FreeEngineLists() {
    TSExplicitList<SE3HANDLENODE>& handles = g_se3Handles.m_fulllist;
    while (SE3HANDLENODE* node = handles.Head()) {
        SE3SoundKitObject* object = node->m_object;
        g_se3Handles.Unlink(node);
        if (object != nullptr) {
            object->m_def = nullptr;
            object->m_self = nullptr;
            SE3ObjectFree(object);
        }
        g_se3Handles.Delete(node);
    }
    if (g_soundKitHeap != nullptr) {
        g_soundKitHeap->~TObjectAllocHeap();
        SMemFree(g_soundKitHeap, s_file, -2, 0);
    }
    g_soundKitHeap = nullptr;
    TSExplicitList<SE3COMBINEDNODE>& combined = g_se3Combined.m_fulllist;
    while (SE3COMBINEDNODE* node = combined.Head()) {
        g_se3Combined.Unlink(node);
        g_se3Combined.Delete(node);
    }
    SE3SoundEndedShutdown();
    SE3CacheShutdown();
}

// ---- the cache's handlers -----------------------------------------------------------------

namespace {

blz::function_storage s_cacheEventHandler = {nullptr, &blz::function_empty_manager, {}};   // DAT_142cfd070
blz::function_storage s_doneLoadingHandler = {nullptr, &blz::function_empty_manager, {}};  // DAT_142cfd090

void Assign(blz::function_storage* dest, blz::function_storage* source) {
    if (source != dest) {
        dest->m_manager(dest->m_storage, nullptr, 0);
        source->m_manager(dest->m_storage, source->m_storage, 1);
        dest->m_invoke = source->m_invoke;
        dest->m_manager = source->m_manager;
    }
    source->m_manager(source->m_storage, nullptr, 0);
}

}

// ref: FUN_1412db7a0
void SE3InstallSoundEndedHandler(blz::function_storage* handler) {
    Assign(&s_cacheEventHandler, handler);
}

// ref: FUN_1412db810
void SE3InstallDoneLoadingHandler(blz::function_storage* handler) {
    Assign(&s_doneLoadingHandler, handler);
}

void SE3CallCacheEventHandler(int32_t event, SE3CacheNode* node) {
    if (s_cacheEventHandler.m_invoke != nullptr) {
        using INVOKE = void (*)(int32_t, void*, void*);
        reinterpret_cast<INVOKE>(s_cacheEventHandler.m_invoke)(event, node, s_cacheEventHandler.m_storage);
    }
}

// LAB_1412da380: FMOD's non-blocking load callback goes straight to the handler.
int32_t SE3CallDoneLoadingHandler(FMOD::Sound* sound, int32_t result) {
    using INVOKE = int32_t (*)(void*, int32_t, void*);
    return reinterpret_cast<INVOKE>(s_doneLoadingHandler.m_invoke)(sound, result, s_doneLoadingHandler.m_storage);
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1402d7e80=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142cfd070 = (void*)&s_cacheEventHandler;
extern "C" void* const gen_p_142cfd090 = (void*)&s_doneLoadingHandler;
// ---- end of generated globals ----
