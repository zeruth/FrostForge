#ifndef SOUND_SE3KIT_HPP
#define SOUND_SE3KIT_HPP

// The engine's playback state (SE3.cpp, SE3Cache.cpp): play parameters, sound kit definitions,
// sound kit instances with their two sound slots, and the cached FMOD sounds. Layouts follow
// the reference's accesses; fields not yet named keep their offset.

#include <cstdint>

#include "blz/Function.hpp"
#include "fmod.hpp"
#include "storm/hash/TSHashTable.hpp"
#include "storm/list/TSList.hpp"

namespace cas {
struct CASFILE;
}
struct SE3SoundKitObject;

// What a play asks for (0x148, FUN_1412d25a0 init, FUN_1412c6ce0 copy).
struct SE3PLAYPARAMS {
    int8_t m_bus;                       // 0x000 SE3BUS_*
    uint8_t m_pad001[3];
    float m_04;                         // 0x004 -1.0
    float m_08;                         // 0x008 1.0
    int32_t m_entry;                    // 0x00C the kit entry to play, -1 to pick
    int32_t m_10;                       // 0x010 2
    int32_t m_3dMode;                   // 0x014 0 by kit flags, 1 looping, 2 one-shot
    int32_t m_18;                       // 0x018 4
    float m_1c;                         // 0x01C 1.0
    int8_t m_priority;                  // 0x020 4: SoundKit field 0xc
    uint8_t m_pad021[3];
    int32_t m_24;                       // 0x024 0x80
    int32_t m_duplicates;               // 0x028 0 by kit flags, 2 refuse duplicates
    int32_t m_2c;                       // 0x02C
    float m_minDistance;                // 0x030 -1.0
    float m_maxDistance;                // 0x034 -1.0
    int32_t m_38;                       // 0x038 2
    float m_3c;                         // 0x03C 0.5
    int32_t m_40;                       // 0x040
    uint8_t m_pad044[4];
    blz::function_storage m_48;         // 0x048
    int32_t m_68;                       // 0x068
    uint8_t m_pad06c[4];
    SE3SoundKitObject* m_parent;        // 0x070 the instance a re-trigger belongs to
    int32_t m_fileDataId;               // 0x078 a file played without a kit
    float m_7c[3];                      // 0x07C a direction
    blz::function_storage m_88;        // 0x088 bool(this+0xa8): may play
    uint64_t m_a8[2];                   // 0x0A8 the context m_88 is asked about
    int32_t m_b8;                       // 0x0B8
    uint8_t m_padbc[4];
    blz::function_storage m_c0;         // 0x0C0
    int32_t m_e0;                       // 0x0E0
    uint8_t m_pade4[4];
    blz::function_storage m_e8;         // 0x0E8 float(this+0x108): a volume scale
    uint64_t m_108[2];                  // 0x108
    int32_t m_fadeInType;               // 0x118 1; 0xb custom
    int32_t m_fadeInMs;                 // 0x11C
    int32_t m_fadeInCurve;              // 0x120 0xc57
    int32_t m_fadeOutType;              // 0x124 1
    int32_t m_fadeOutMs;                // 0x128
    int32_t m_fadeOutCurve;             // 0x12C 0xc58
    float m_fadeOutStart;               // 0x130 where a curve fade out starts
    uint8_t m_combinable;               // 0x134
    uint8_t m_135;                      // 0x135
    uint8_t m_136;                      // 0x136 1
    uint8_t m_137;                      // 0x137
    uint8_t m_138;                      // 0x138
    uint8_t m_stopDuplicates;           // 0x139 1
    uint8_t m_13a;                      // 0x13A install m_48 as the instance's callback
    uint8_t m_checkRange;               // 0x13B 1
    uint8_t m_13c;                      // 0x13C
    uint8_t m_13d;                      // 0x13D
    uint8_t m_prefetchOnly;             // 0x13E
    uint8_t m_13f;                      // 0x13F
    uint8_t m_140;                      // 0x140
    uint8_t m_141;                      // 0x141
    uint8_t m_142;                      // 0x142
    uint8_t m_143;                      // 0x143
    uint8_t m_144;                      // 0x144
    uint8_t m_pad145[3];
};
static_assert(sizeof(SE3PLAYPARAMS) == 0x148, "SE3PLAYPARAMS is 0x148 bytes");

// FUN_1412d25a0
void SE3PlayParamsInit(SE3PLAYPARAMS* params);
// FUN_1412c5dc0: construction (the function slots empty), then the init.
void SE3PlayParamsConstruct(SE3PLAYPARAMS* params);
// FUN_1412c6ce0
SE3PLAYPARAMS* SE3PlayParamsCopy(SE3PLAYPARAMS* dest, const SE3PLAYPARAMS* source);
// The function slots' destruction.
void SE3PlayParamsDestroy(SE3PLAYPARAMS* params);

// A sound kit definition (0xb0, SoundKitDefObjectHeap; FUN_1412cc8a0).
struct SE3SoundKitDef {
    TSLink<SE3SoundKitDef> m_link;      // 0x00
    TSList<SE3SoundKitObject, TSGetLink<SE3SoundKitObject>, 8> m_instances; // 0x10 (link +0x3c8)
    int32_t m_entryCount;               // 0x28 SoundKitEntry records
    int32_t m_kitId;                    // 0x2C SoundKit
    int32_t m_advancedId;               // 0x30 SoundKitAdvanced (SoundKit field 7)
    int32_t m_34;                       // 0x34 a timer (the def timer list)
    uint8_t m_envelopeBusy;             // 0x38 set while the envelopes are checked
    uint8_t m_hasTimeWindow;            // 0x39 SoundKitAdvanced fields 4..7
    uint8_t m_3a;                       // 0x3A
    uint8_t m_pad3b;
    int32_t m_lastEntry;                // 0x3C -1
    int32_t m_loadState;                // 0x40 2 ready, 1 waiting for a prefetch
    int8_t m_bus;                       // 0x44 from SoundKit field 0xe (0x62 none)
    uint8_t m_pad45[3];
    void* m_prefetch;                   // 0x48 the async prefetch while files download
    int32_t m_prefetchTime;             // 0x50
    uint8_t m_pad54[4];
    FMOD::SoundGroup* m_soundGroup;     // 0x58
    TSLink<SE3SoundKitDef> m_timerLink; // 0x60 (DAT_142cfce38 list)
    uint64_t m_70;                      // 0x70
    uint64_t m_78;                      // 0x78
    uint32_t m_childCount;              // 0x80 SoundKitChild kits (TSGrowableArray<int>)
    uint8_t m_pad84[4];
    int32_t* m_children;                // 0x88
    uint32_t m_childCapacity;           // 0x90
    uint32_t m_pad94;
    uint32_t m_childGranularity;        // 0x98
    uint32_t m_pad9c;
    uint8_t* m_entryWeights;            // 0xA0 one byte per entry: the variation picker's weights
    uint32_t m_objectIndex;             // 0xA8 the object heap's index (last 4 bytes)
    uint32_t m_padac;
};
static_assert(sizeof(SE3SoundKitDef) == 0xb0, "SE3SoundKitDef is 0xb0 bytes");

// One sound playing in an instance (0x58): the cached sound's key and its FMOD channel.
struct SE3SoundObject {
    TSLink<SE3SoundObject> m_link;      // 0x00
    int32_t m_serial;                   // 0x10 the cache node's serial
    uint8_t m_pad14[4];
    FMOD::Channel* m_channel;           // 0x18
    float m_frequency;                  // 0x20 the channel's frequency when started (0 unread)
    int32_t m_fade;                     // 0x24 0 none, 1 in, 2 out
    float m_fadeVolume;                 // 0x28 1.0
    uint8_t m_pad2c[4];
    FMOD::DSP* m_dsp[4];                // 0x30 lowpass, highpass, ...
    int32_t m_entry;                    // 0x50 the kit entry, -1
    uint8_t m_pad54[4];
};
static_assert(sizeof(SE3SoundObject) == 0x58, "SE3SoundObject is 0x58 bytes");

// A sound kit instance (0x3f0, SoundKitObjectHeap; FUN_1412c5980 / FUN_1412c6620).
struct SE3SoundKitObject {
    TSLink<SE3SoundKitObject> m_playing;    // 0x000 (DAT_142cfcd90)
    uint8_t m_10;                       // 0x010
    uint8_t m_streamed;                 // 0x011
    uint8_t m_12;                       // 0x012 1 (params 0x136)
    uint8_t m_13;
    uint8_t m_hasPosition;              // 0x014
    uint8_t m_retriggered;              // 0x015
    uint8_t m_envelopesDone;            // 0x016
    uint8_t m_started;                  // 0x017
    SE3SoundObject m_slots[2];          // 0x018, 0x070
    TSList<SE3SoundObject, TSGetLink<SE3SoundObject>, 8> m_oldSlots;    // 0x0C8
    SE3SoundKitDef* m_def;              // 0x0E0
    uint32_t m_context[4];              // 0x0E8 params 0xa8
    int32_t m_handle;                   // 0x0F8
    int32_t m_combinedHandle;           // 0x0FC
    int32_t m_fadeState;                // 0x100 0, 1 fading in, 2 fading out
    int32_t m_fadeElapsed;              // 0x104
    float m_fadeVolume;                 // 0x108 1.0
    float m_volume;                     // 0x10C 1.0
    float m_position[3];                // 0x110
    float m_offset[3];                  // 0x11C a random offset
    float m_minDistance;                // 0x128
    float m_maxDistance;                // 0x12C
    uint32_t m_mode;                    // 0x130 FMOD_MODE
    uint8_t m_pad134[4];
    blz::function_storage m_onEnded;    // 0x138 void(instance, storage)
    uint8_t m_pad158[8];
    int32_t m_channelPriority;          // 0x160 0x80
    int32_t m_priorityRange;            // 0x164 added with distance
    int32_t m_priorityBase;             // 0x168 0x80 (the bus's)
    float m_pitch;                      // 0x16C 1.0 the kit's pitch variation
    float m_pitchCurve;                 // 0x170 1.0 the envelope's pitch
    uint8_t m_3dMode;                   // 0x174 0 looping, 1 periodic, 2 one-shot
    uint8_t m_pad175[3];
    int32_t m_delay;                    // 0x178
    int32_t m_retrigger;                // 0x17C
    blz::function_storage m_onStart;    // 0x180 void(channel, int, storage)
    int32_t m_onStartArg;               // 0x1A0
    uint8_t m_pad1a4[4];
    SE3PLAYPARAMS m_params;             // 0x1A8
    SE3SoundKitObject* m_self;          // 0x2F0 (the handle node, see SE3HANDLENODE)
    int32_t m_entry;                    // 0x2F8
    float m_timeWindow;                 // 0x2FC 1.0
    int32_t m_300;                      // 0x300
    int32_t m_stopTimer;                // 0x304
    int32_t m_lifeTimer;                // 0x308
    int32_t m_elapsed;                  // 0x30C
    float m_310;                        // 0x310 1.0
    int32_t m_lengthMs;                 // 0x314
    int32_t m_kitId;                    // 0x318
    int8_t m_priority;                  // 0x31C
    uint8_t m_pad31d[3];
    float m_320;                        // 0x320 1.0
    float m_324;                        // 0x324 1.0
    float m_328[2];                     // 0x328 (smoothed 0x320)
    float m_330[2];                     // 0x330 (smoothed 0x324)
    float m_338;                        // 0x338 1.0
    float m_33c;                        // 0x33C 20000
    float m_340[2];                     // 0x340 (smoothed 0x338)
    float m_348[2];                     // 0x348 (smoothed 0x33c)
    float m_350[2];                     // 0x350
    TSLink<SE3SoundKitObject> m_loading;    // 0x358 (DAT_142cfcda8)
    TSLink<SE3SoundKitObject> m_pending;    // 0x368 (DAT_142cfcdc0)
    TSLink<SE3SoundKitObject> m_release;    // 0x378 (DAT_142cfcdd8)
    TSLink<SE3SoundKitObject> m_fading;     // 0x388 (DAT_142cfcdf0)
    TSLink<SE3SoundKitObject> m_398;        // 0x398
    TSLink<SE3SoundKitObject> m_waiting;    // 0x3A8 (DAT_142cfce20)
    TSLink<SE3SoundKitObject> m_bus;        // 0x3B8 (the bus's kits)
    TSLink<SE3SoundKitObject> m_defLink;    // 0x3C8 (the def's instances)
    TSLink<SE3SoundKitObject> m_timed;      // 0x3D8 (DAT_142cfce08)
    uint32_t m_pad3e8;
    uint32_t m_objectIndex;             // 0x3EC the object heap's index
};
static_assert(sizeof(SE3SoundKitObject) == 0x3f0, "SE3SoundKitObject is 0x3f0 bytes");

// The read-ahead of an encrypted or streamed file (0x550; SE3AsyncData).
struct SE3ASYNCDATA;

// A cached FMOD sound (0x68, SE3Cache.cpp).
struct SE3CacheNode {
    TSLink<SE3CacheNode> m_link;        // 0x00 the loading / ready lists
    cas::CASFILE* m_file;               // 0x10 until FMOD opens it
    FMOD::Sound* m_sound;               // 0x18
    SE3ASYNCDATA* m_async;              // 0x20
    int32_t m_fileDataId;               // 0x28
    int32_t m_key;                      // 0x2C
    int32_t m_state;                    // 0x30 0 loading, 2 loaded, 3 failed
    int32_t m_users;                    // 0x34
    int32_t m_size;                     // 0x38
    int32_t m_serial;                   // 0x3C
    int32_t m_releaseTimer;             // 0x40 500
    uint8_t m_flags;                    // 0x44 1 streamed, 2 the file is not yet FMOD's
    uint8_t m_pad45[3];
    TSLink<SE3CacheNode> m_lru;         // 0x48 (DAT_142cfd040)
    TSLink<SE3CacheNode> m_releaseLink; // 0x58 (DAT_142cfd058)
};
static_assert(sizeof(SE3CacheNode) == 0x68, "SE3CacheNode is 0x68 bytes");

// The handle tables' nodes. An instance's handle (TSHashTable at 0x142cfccf0):
struct SE3HANDLENODE : TSHashObject<SE3HANDLENODE> {
    SE3SoundKitObject* m_object;        // 0x30
};
static_assert(sizeof(SE3HANDLENODE) == 0x38, "SE3HANDLENODE is 0x38 bytes");

// A combined play's handle (0x142cfcd40): the member instances' handles.
struct SE3COMBINEDNODE : TSHashObject<SE3COMBINEDNODE> {
    int32_t m_30;                       // 0x30
    uint8_t m_pad34[4];
    uint32_t m_count;                   // 0x38 TSGrowableArray<uint32_t>
    uint8_t m_pad3c[4];
    uint32_t* m_handles;                // 0x40
    uint32_t m_capacity;                // 0x48
    uint32_t m_pad4c;
    uint32_t m_granularity;             // 0x50
    uint32_t m_pad54;
};
static_assert(sizeof(SE3COMBINEDNODE) == 0x58, "SE3COMBINEDNODE is 0x58 bytes");

// The engine's tables and lists.
class SE3HANDLETABLE : public TSHashTable<SE3HANDLENODE> {
    public:
    void Delete(SE3HANDLENODE* node) override;
    SE3HANDLENODE* NewNode(TSExplicitList<SE3HANDLENODE>* slot, uint32_t extra, uint32_t flags) override;
};
class SE3COMBINEDTABLE : public TSHashTable<SE3COMBINEDNODE> {
    public:
    void Delete(SE3COMBINEDNODE* node) override;
    SE3COMBINEDNODE* NewNode(TSExplicitList<SE3COMBINEDNODE>* slot, uint32_t extra, uint32_t flags) override;
};
extern SE3HANDLETABLE g_se3Handles;     // 0x142cfccf0
extern SE3COMBINEDTABLE g_se3Combined;  // 0x142cfcd40
extern int32_t g_se3LastHandle;         // DAT_142cfca0c

using SE3KITLIST = TSList<SE3SoundKitObject, TSGetLink<SE3SoundKitObject>, 8>;
extern SE3KITLIST g_se3Playing;         // 0x142cfcd90 (link +0x000)
extern SE3KITLIST g_se3Loading;         // 0x142cfcda8 (+0x358)
extern SE3KITLIST g_se3Pending;         // 0x142cfcdc0 (+0x368)
extern SE3KITLIST g_se3Release;         // 0x142cfcdd8 (+0x378)
extern SE3KITLIST g_se3Fading;          // 0x142cfcdf0 (+0x388)
extern SE3KITLIST g_se3Timed;           // 0x142cfce08 (+0x3d8)
extern SE3KITLIST g_se3Waiting;         // 0x142cfce20 (+0x3a8)
extern TSList<SE3SoundKitDef, TSGetLink<SE3SoundKitDef>, 8> g_se3DefTimers;   // 0x142cfce38 (+0x60)

// The engine's random stream (DAT_1430a9530 / DAT_1430a9534 over the table at 0x142734470).
uint32_t SE3Random();
// A random integer in [low, high) (high when low >= high).
// ref: FUN_1412d2160
int32_t SE3RandomRange(int32_t low, int32_t high);

// Instances (SE3SoundKit.cpp).
// ref: FUN_1412c9d80
SE3SoundKitObject* SE3GetObject(uint32_t handle);
// ref: FUN_1412cd270
bool SE3IsPlaying(uint32_t handle);
// ref: FUN_1412c9c50
int32_t SE3GetKitId(uint32_t handle);
// ref: FUN_1412c93c0
SE3SoundKitDef* SE3GetKitDef(int32_t kitId);
SE3SoundKitDef* SE3FindKitDef(int32_t kitId);
// A combinable kit is held for the frame: one queue entry per kit, each play adding its position
// (or none) to it (SE3PlayKit's combinable branch). The frame plays each entry once.
void SE3CombinableAdd(uint32_t kitId, const float* position, const SE3PLAYPARAMS* params);
// The shutdown's part: every queued kit and the positions' pool (FUN_1412d41d0).
void SE3CombinableShutdown();
// ref: FUN_1412c9b20
const uint8_t* SE3GetKitEntry(SE3SoundKitDef* def, int32_t index);
// ref: FUN_1412c99c0
FMOD::SoundGroup* SE3GetSoundGroup(SE3SoundKitDef* def);
// ref: FUN_1412c88b0
void SE3QueueRelease(SE3SoundKitObject* object);
// ref: FUN_1412c8620
SE3SoundKitObject* SE3CreateObject(int32_t* handle, SE3PLAYPARAMS* params, SE3SoundKitDef* def);
// ref: FUN_1412d3470
void SE3SetupObject(SE3SoundKitObject* object, const float* position, SE3PLAYPARAMS* params, uint32_t mode);
// ref: FUN_1412cdb40
void SE3SetSlotSound(SE3SoundKitObject* object, int32_t cacheKey, int32_t entry);
// ref: FUN_1412ce100
uint32_t SE3PlayKit(uint32_t kitId, const float* position, SE3PLAYPARAMS* params, int32_t* handle, const char* reason,
    bool prefetch, const void* key);
struct SE3VOICEOVERPRIORITY;
// ref: FUN_1412cf350
uint32_t SE3PlayVoiceOver(uint32_t kitId, const float* position, SE3PLAYPARAMS* params, int32_t* handle,
    const char* reason, bool prefetch, const void* key, const SE3VOICEOVERPRIORITY* priority);
// ref: FUN_1412d4d80
bool SE3Stop(uint32_t handle, int32_t fadeType, int32_t fadeMs, const char* reason);
// ref: FUN_1412d4d60
bool SE3StopNow(uint32_t handle, const char* reason);
// ref: FUN_1412d3920
bool SE3SetOnEnded(uint32_t handle, blz::function_storage* callback, uint64_t argument);

// The cache (SE3Cache.cpp).
// A loaded sample by key.
// ref: FUN_1412daa10
bool SE3CacheFindReady(int32_t key, SE3CacheNode** node);
// A sample still loading by key.
// ref: FUN_1412dab40
bool SE3CacheFindLoading(int32_t key, SE3CacheNode** node);
// ref: FUN_1412daa60
int32_t SE3CacheGetSound(SE3SoundKitObject* object, SE3CacheNode** node, uint32_t* slot);
// ref: FUN_1412da230
void SE3CacheReleaseNode(SE3CacheNode* node, bool loaded);
void SE3CacheShutdown();
// ref: FUN_1412db8a0
void SE3CacheAddUser(int32_t key, bool add);
// ref: FUN_1412d9bf0
uint32_t SE3CacheCreate(int32_t key, FMOD::SoundGroup* group, uint32_t mode, SE3SoundKitObject* object, int32_t size,
    int32_t fileDataId, cas::CASFILE** file, SE3CacheNode** node, SE3ASYNCDATA* async);
// The read-ahead for a file that will stream (FUN_1412ce100 allocates it).
SE3ASYNCDATA* SE3AsyncDataCreate(cas::CASFILE* file, const uint32_t* key);
// The handlers SE3SoundKit.cpp installs at post-init (DAT_142cfd070 / DAT_142cfd090).
void SE3CallCacheEventHandler(int32_t event, SE3CacheNode* node);
int32_t SE3CallDoneLoadingHandler(FMOD::Sound* sound, int32_t result);
// Whether a file is an MP3 (its manifest entry's content flags are 2).
// ref: FUN_1412cd200
bool SE3IsMp3(int32_t fileDataId);
// A streamed kit's start offset in ms (SoundKitAdvanced field 0x1f), 0 without one.
int32_t SE3AdvancedStartOffset(const SE3SoundKitDef* def);
extern int32_t g_se3CacheMaxCacheable;  // DAT_142cfcfbc

// The buses' parts the instances use (SE3Bus.cpp).
// ref: FUN_1412d8ba0
bool SE3BusIsUnder(int8_t bus, int8_t ancestor);
// ref: FUN_1412d7cf0
bool SE3BusAttach(int8_t bus, FMOD::Channel* channel);
// The bus's channel priority and its range with distance (SoundBus fields 2 and 3).
// ref: FUN_1412d7ea0
uint8_t SE3BusGetPriority(int8_t bus);
// ref: FUN_1412d7ed0
uint8_t SE3BusGetPriorityRange(int8_t bus);
// ref: FUN_1412d81d0
void SE3BusLinkObject(SE3SoundKitObject* object);
// ref: FUN_1412d7d80
void SE3BusUnlinkObject(SE3SoundKitObject* object);
// ref: FUN_1412d7f00
float SE3BusGetObjectVolume(int8_t bus, uint32_t handle);
// ref: FUN_1412d8be0
bool SE3BusIsEnabled(int8_t bus);
// ref: FUN_1412d8c20
bool SE3BusIsFull(int8_t bus);
// ref: FUN_1412d8d70
bool SE3BusAllowsReverb(int8_t bus);
// ref: FUN_1412d80b0
float SE3BusGetChainVolume(int8_t bus);

// A voice-over's priority (VoiceOverPriority id) and whether one was given.
struct SE3VOICEOVERPRIORITY {
    int32_t m_priority;
    uint8_t m_set;
};

// The engine's state the passes share.
extern int32_t g_se3VoiceOverHandle;        // DAT_1430a94d8
extern int32_t g_se3VoiceOverPriority;      // DAT_142cfca30
extern int32_t g_se3VoiceOverKit;           // DAT_1430a94c4
extern int32_t g_se3ClosestPrefetchPriority;    // DAT_1430a9470
extern void* g_se3ClosestPrefetch;          // DAT_1430a9468
extern uint8_t g_se3CheckFilters;           // DAT_1430a9474
extern float g_se3ListenerPosition[3];      // DAT_1430a9698
extern int32_t g_se3Period;                 // DAT_142cfca34
extern int32_t g_se3GameTime;               // DAT_1430a94dc
bool SE3IsInitialized();                    // DAT_142cfca08
float SE3GlobalVolume();                    // FUN_1412ca150 (DAT_142cfca04)
int32_t SE3VariationCap();                  // SoundPerf_VariationCap (32 without it)
bool SE3PositionalLowPassEnabled();         // Sound_EnablePositionalLowPassFilter
// Locks the samples' lists (DAT_142ddced0).
void SE3SamplesLock(bool enter);

// Kits (SE3SoundKit.cpp, SE3Play.cpp).
// ref: FUN_1412cd2c0
bool SE3KitIsLooping(int32_t kitId);
// SoundKit field 3 bit 6: the kit is not ducked.
// ref: FUN_1412d3ce0
bool SE3KitIgnoresSuppressors(const SE3SoundKitDef* def);
// Whether an entry's file is on disk (its residency is not 1).
// ref: FUN_1412cd160
bool SE3EntryResident(SE3SoundKitDef* def, int32_t entry);
// ref: FUN_1412d2740
void SE3PickEntry(SE3SoundKitDef* def, SE3PLAYPARAMS* params, int32_t* entry);
// ref: FUN_1412c76a0
void SE3CombinedAdd(SE3COMBINEDNODE* node, uint32_t handle);
// ref: FUN_1412c91a0
void SE3ReleaseKitDefs();
// ref: FUN_1412d47c0
void SE3SlotStop(SE3SoundObject* slot);
// ref: FUN_1412c6620
void SE3ObjectDestroy(SE3SoundKitObject* object);
void SE3ObjectFree(SE3SoundKitObject* object);
// ref: FUN_1412c9d00
float SE3GetMinDistance(const SE3PLAYPARAMS* params, const SE3SoundKitDef* def);
// ref: FUN_1412c9c80
float SE3GetMaxDistance(const SE3PLAYPARAMS* params, const SE3SoundKitDef* def);
// ref: FUN_1412cd430
bool SE3IsOutOfRange(const float* position, float range);
// ref: FUN_1412dbbf0
void SE3SetOnStart(SE3SoundKitObject* object, blz::function_storage* callback, int32_t argument);

// Playing and stopping (SE3PlayKit.cpp).
// ref: FUN_1412d3e10
uint32_t SE3CanPlay(SE3SoundKitDef* def, SE3PLAYPARAMS* params, const float* position);
// ref: FUN_1412c79c0
uint32_t SE3PrefetchKit(SE3SoundKitDef* def, int32_t fileDataId, int32_t entry, int32_t* handle,
    SE3PLAYPARAMS* params, const float* position);
// MuteSoundFile / UnmuteSoundFile.
void SE3MuteFile(int32_t fileDataId, bool mute);
// ref: FUN_1412d4ee0
bool SE3StopObject(SE3SoundKitObject* object, int32_t fadeType, int32_t fadeMs);
// ref: FUN_1412d50e0
void SE3StopSlots(SE3SoundKitObject* object);
// ref: FUN_1412d4bf0
void SE3StopCombinedMembers(SE3SoundKitObject* object);
// ref: FUN_1412c8ff0
void SE3FadeOut(SE3SoundKitObject* object);
// ref: FUN_1412c8f20
void SE3FadeIn(SE3SoundKitObject* object);
void SE3StopBus(int8_t bus, uint32_t fadeType, int32_t fadeMs);
// ref: FUN_1412c8170
void SE3OnSoundFailed(int32_t serial);
// ref: FUN_1412c7da0
void SE3OnSoundReleased(int32_t serial, bool keepPlaying);
// ref: FUN_1412cd8f0
void SE3QueueStart(SE3SoundKitObject* object);

// The frame (SE3Instance.cpp, SE3Start.cpp, SE3Update.cpp).
// The length of a fade type (DAT_142cfcf58; 0xb: `custom`).
int32_t SE3FadeMs(int32_t type, int32_t custom);
// ref: FUN_1412c9780
float SE3SlotVolume(SE3SoundKitObject* object, int32_t slot);
// ref: FUN_1412d6f00
void SE3ApplyVolume(SE3SoundKitObject* object);
// ref: FUN_1412d6d70
void SE3ApplyPriority(SE3SoundKitObject* object);
// ref: FUN_1412d6570
void SE3ApplyPitch(SE3SoundKitObject* object, float scale);
// ref: FUN_1412d6610
void SE3UpdateDistances(SE3SoundKitObject* object);
// ref: FUN_1412c82d0
float SE3RolloffCurve(float minimum, float maximum, float distance, int32_t type, float factor);
// ref: FUN_1412c7810
void SE3ApplyCurve(SE3SoundKitObject* object, int32_t kind, float t, float value);
// ref: FUN_1412d0030
bool SE3StepFade(SE3SoundKitObject* object, int32_t elapsed);
// ref: FUN_1412d5290
void SE3UpdateObject(SE3SoundKitObject* object, int32_t elapsed, bool retrigger);
// ref: FUN_1412cd760
bool SE3IsInaudible(SE3SoundKitObject* object);
// ref: FUN_1412cfbd0
void SE3UpdateWaitingInstances();
void SE3SoundEndedShutdown();
// FMOD's channel callback.
// ref: FUN_1412d46c0
FMOD_RESULT F_CALLBACK SE3ChannelCallback(FMOD_CHANNEL* channel, FMOD_CHANNEL_CALLBACKTYPE type, void* data1,
    void* data2);

// The suppressors (SE3Suppressors.cpp).
// ref: FUN_1412e0e00
void SE3SuppressorAdd(SE3SoundKitObject* object);
// ref: FUN_1412e13a0
void SE3SuppressorsForget(int32_t handle);
// ref: FUN_1412e0dd0
bool SE3SuppressorsAny();
// ref: FUN_1412e0a70
void SE3SuppressorGetTargets(uint32_t handle, float* targets);
// ref: FUN_1412e08f0
float SE3SuppressorFactor(int8_t category, uint32_t handle);

#endif
