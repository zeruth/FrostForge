#include "ui/FrameTime.hpp"

uint32_t OsGetAsyncTimeMs();

namespace {

uint64_t s_startTime;               // DAT_142e9a208
int32_t s_frames;                   // DAT_142e9a204 counts updates (and the initialization)
uint64_t s_lastTime;                // DAT_142e9a20c
uint32_t s_time;                    // DAT_142e9a1f0
float s_timeSeconds;                // DAT_142e9a1f4
uint32_t s_elapsed;                 // DAT_142e9a1f8 the last frame's ms
float s_elapsedSeconds;             // DAT_142e9a1fc the last frame's seconds
uint32_t s_unknown1b8;              // DAT_142e9a1b8

}

// ref: FUN_14065f9e0
void FrameTime_Initialize() {
    s_startTime = OsGetAsyncTimeMs();
    s_frames++;
    s_lastTime = s_startTime;
    s_time = static_cast<uint32_t>(s_startTime);
    s_elapsed = 0;
    s_elapsedSeconds = 0.0f;
    s_unknown1b8 = 0;
    s_timeSeconds = static_cast<float>(s_startTime) * 0.001f;
}

// ref: FUN_14065f960
uint32_t FrameTime_GetTime() {
    return s_time;
}

// ref: FUN_14065f9a0
uint32_t FrameTime_GetRunTime() {
    return s_time - static_cast<uint32_t>(s_startTime);
}

// ref: FUN_14065f9b0
uint32_t FrameTime_GetElapsed() {
    return s_elapsed;
}

// ref: FUN_14065f970
float FrameTime_GetTimeSeconds() {
    return s_timeSeconds;
}

// ref: FUN_14065f980
int32_t FrameTime_GetFrames() {
    return s_frames;
}

// ref: FUN_14065f9c0
float FrameTime_GetElapsedSeconds() {
    return s_elapsedSeconds;
}

// The clock, once a frame: the time now and the frame's length (FUN_14065fce0).
// ref: FUN_14065fce0
void FrameTime_Update(float seconds, uint32_t now) {
    s_frames++;
    s_time = now;
    s_elapsedSeconds = seconds;
    s_elapsed = static_cast<uint32_t>(static_cast<int32_t>(seconds * 1000.0f));
    s_timeSeconds = static_cast<float>(now) * 0.001f;
}
