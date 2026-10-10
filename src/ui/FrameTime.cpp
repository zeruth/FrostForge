#include "ui/FrameTime.hpp"

uint32_t OsGetAsyncTimeMs();

namespace {

uint64_t s_startTime;               // DAT_142e9a208
int32_t s_initCount;                // DAT_142e9a204
uint64_t s_lastTime;                // DAT_142e9a20c
uint32_t s_time;                    // DAT_142e9a1f0
float s_timeSeconds;                // DAT_142e9a1f4
uint32_t s_elapsed;                 // DAT_142e9a1f8 the last frame's ms
uint32_t s_unknown1fc;              // DAT_142e9a1fc
uint32_t s_unknown1b8;              // DAT_142e9a1b8

}

// ref: FUN_14065f9e0
void FrameTime_Initialize() {
    s_startTime = OsGetAsyncTimeMs();
    s_initCount++;
    s_lastTime = s_startTime;
    s_time = static_cast<uint32_t>(s_startTime);
    s_elapsed = 0;
    s_unknown1fc = 0;
    s_unknown1b8 = 0;
    s_timeSeconds = static_cast<float>(s_startTime) * 0.001f;
}

// ref: FUN_14065f960
uint32_t FrameTime_GetTime() {
    return s_time;
}

// ref: FUN_14065f9b0
uint32_t FrameTime_GetElapsed() {
    return s_elapsed;
}
