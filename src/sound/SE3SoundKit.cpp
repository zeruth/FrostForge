// The engine's sound kit instances, cache, suppressors and emitters (SE3.cpp, SE3Cache.cpp,
// SE3Suppressors.cpp, SE3Emitter.cpp). Every list here holds sound kit instances; until playback
// is ported nothing is ever added to them, so the walks are stubs.

#include "sound/SE3.hpp"
#include "sound/SE3Internal.hpp"

#include <cstring>

#include "util/Unimplemented.hpp"

namespace {

const char s_suppressorsFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\SoundEngine\\SE3Suppressors.cpp";

int32_t s_cacheMaxSize;         // DAT_142cfcfc0
int32_t s_cacheMaxCacheable;    // DAT_142cfcfbc
blz::function_storage s_soundEventHandler = {nullptr, &blz::function_empty_manager, {}};    // DAT_142cfd070
blz::function_storage s_doneLoadingHandler = {nullptr, &blz::function_empty_manager, {}};   // DAT_142cfd090

// A handler slot's assignment (blz::function's move-assign).
void Assign(blz::function_storage* dest, blz::function_storage* src) {
    if (src != dest) {
        dest->m_manager(dest->m_storage, nullptr, 0);
        src->m_manager(dest->m_storage, src->m_storage, 1);
        dest->m_invoke = src->m_invoke;
        dest->m_manager = src->m_manager;
    }
    src->m_manager(src->m_storage, nullptr, 0);
}

}

// ref: FUN_1412db880
void SE3CacheSetMaxSize(int32_t bytes) {
    s_cacheMaxSize = bytes;
}

// ref: FUN_1412db890
void SE3CacheSetMaxCacheableSize(int32_t bytes) {
    s_cacheMaxCacheable = bytes;
}

// ref: FUN_1412db7a0
void SE3InstallSoundEndedHandler(blz::function_storage* handler) {
    Assign(&s_soundEventHandler, handler);
}

// ref: FUN_1412db810
void SE3InstallDoneLoadingHandler(blz::function_storage* handler) {
    Assign(&s_doneLoadingHandler, handler);
}

// ref: FUN_1412c7c60
void SE3OnCacheEvent(int32_t event, void* object) {
    (void)event;
    (void)object;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412d9b50
int32_t SE3OnDoneLoading(void* object, int32_t result) {
    (void)object;
    (void)result;
    FROST_UNIMPLEMENTED();
    return 0;
}

// ref: FUN_1412c8700
float F_CALLBACK SE3Rolloff(FMOD_CHANNEL* channel, float distance) {
    (void)channel;
    (void)distance;
    FROST_UNIMPLEMENTED();
    return 0.0f;
}

// ref: FUN_1412e0ce0
void SE3SuppressorsInit() {
    SE3Log(0x5f, s_suppressorsFile, 0, " - Init Suppressors");
    // the suppressor list (DAT_142cfd258) is emptied here; nothing is added to it yet
}

// ref: FUN_1412e13f0
void SE3SuppressorsUpdate(int32_t elapsed) {
    (void)elapsed;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412e12f0
void SE3SuppressorsShutdown() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412dd330
void SE3EmitterManagerInit(blz::function_storage* gridCell, int32_t param88, int32_t param8c) {
    (void)gridCell;
    (void)param88;
    (void)param8c;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412dcfb0
void SE3EmitterUpdate(int32_t elapsed) {
    (void)elapsed;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412deac0
void SE3EmitterManagerShutdown() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412daf50
void SE3CacheUpdate(int32_t elapsed, bool flush, bool restart) {
    (void)elapsed;
    (void)flush;
    (void)restart;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412dab90
void SE3CacheTrim() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412dac70
void SE3CacheReleaseIdle() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412db320
void SE3ProcessDoneLoading() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412cfb10
void SE3ReleaseFinishedInstances() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412cf720
void SE3UpdateCombinedKits() {
    FROST_UNIMPLEMENTED();
}

void SE3ReleaseUnusedKitGroups() {
    FROST_UNIMPLEMENTED();
}

void SE3UpdateRange() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412cfbd0
void SE3StartPendingInstances() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412d0300
void SE3StartLoadedInstances() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412d01b0
void SE3UpdateFades(int32_t elapsed) {
    (void)elapsed;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412d1bb0
void SE3ProcessSoundEnded() {
    FROST_UNIMPLEMENTED();
}

void SE3UpdateInstances(int32_t elapsed) {
    (void)elapsed;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412d4870
void SE3StopGroup(uint8_t group, bool immediate) {
    (void)group;
    (void)immediate;
    FROST_UNIMPLEMENTED();
}

void SE3FreeEngineLists() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1412c91a0
void SE3ReleaseKitDefs() {
    FROST_UNIMPLEMENTED();
}
