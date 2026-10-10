#ifndef SOUND_SE3INTERNAL_HPP
#define SOUND_SE3INTERNAL_HPP

// The engine's own parts that SE3.cpp drives: the sound kit instances, the cache, the
// suppressors, the emitters.

#include <cstdint>

#include "blz/Function.hpp"
#include "fmod.hpp"

extern uint8_t s_1430a9538;    // DAT_1430a9538 the device list changed

// The distance falloff each instance's kit asks for.
// ref: FUN_1412c8700
float F_CALLBACK SE3Rolloff(FMOD_CHANNEL* channel, float distance);

// SE3Suppressor.cpp
// ref: FUN_1412e0ce0
void SE3SuppressorsInit();
// ref: FUN_1412e13f0
void SE3SuppressorsUpdate(int32_t elapsed);
// ref: FUN_1412e12f0
void SE3SuppressorsShutdown();

class TObjectAllocHeap;
extern TObjectAllocHeap* g_soundKitDefHeap;    // DAT_1430a9568 SoundKitDefObjectHeap
extern TObjectAllocHeap* g_soundKitHeap;       // DAT_1430a9570 SoundKitObjectHeap

// The cache's two handlers: an event on a cached sound (0 released, 1 and 2 loaded, 3 ended)
// and a finished asynchronous load. Both are installed by the post-init.
// ref: FUN_1412db7a0
void SE3InstallSoundEndedHandler(blz::function_storage* handler);
// ref: FUN_1412db810
void SE3InstallDoneLoadingHandler(blz::function_storage* handler);
// ref: FUN_1412c7c60
void SE3OnCacheEvent(int32_t event, void* object);
// ref: FUN_1412d9b50
int32_t SE3OnDoneLoading(void* object, int32_t result);

// SE3Emitter.cpp
// ref: FUN_1412dd330
void SE3EmitterManagerInit(blz::function_storage* gridCell, int32_t param88, int32_t param8c);
// ref: FUN_1412dcfb0
void SE3EmitterUpdate(int32_t elapsed);
// ref: FUN_1412deac0
void SE3EmitterManagerShutdown();

// SE3Cache.cpp
// ref: FUN_1412daf50
void SE3CacheUpdate(int32_t elapsed, bool flush, bool restart);
// ref: FUN_1412dab90
void SE3CacheTrim();
// ref: FUN_1412dac70
void SE3CacheReleaseIdle();
// ref: FUN_1412db320
void SE3ProcessDoneLoading();

// The sound kit instances (SE3SoundKit.cpp).
// ref: FUN_1412cfb10
void SE3ReleaseFinishedInstances();
// ref: FUN_1412cf720
void SE3UpdateCombinedKits();
// FUN_1412cab50's timed passes: every 10 s the kit handle groups whose instances are all
// gone, every 2 s the positional instances' range.
void SE3ReleaseUnusedKitGroups();
void SE3UpdateRange();
// ref: FUN_1412cfbd0
void SE3StartPendingInstances();
// ref: FUN_1412d0300
void SE3StartLoadedInstances();
// ref: FUN_1412d01b0
void SE3UpdateFades(int32_t elapsed);
// ref: FUN_1412d1bb0
void SE3ProcessSoundEnded();
// FUN_1412cab50's tail: each instance's update, the bus mutes, the stop timers.
void SE3UpdateInstances(int32_t elapsed);
// Stops every instance of a group (0xfe: all).
// ref: FUN_1412d4870
void SE3StopGroup(uint8_t group, bool immediate);
// FUN_1412d41d0's middle: the caches, the kit definitions, the message lists.
void SE3FreeEngineLists();
// The kit definitions.
// ref: FUN_1412c91a0
void SE3ReleaseKitDefs();

// SE3Bus.cpp
// ref: FUN_1412d9110
void SE3BusShutdown();

#endif
