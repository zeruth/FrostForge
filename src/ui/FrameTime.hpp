#ifndef UI_FRAME_TIME_HPP
#define UI_FRAME_TIME_HPP

// FrameTime: the UI's clock and its timers (Engine FrameTime::Timer).

#include <cstdint>

// Takes the starting time (FUN_14065f9e0).
void FrameTime_Initialize();
// The clock in milliseconds (FUN_14065f960).
uint32_t FrameTime_GetTime();
// The clock since the starting time (FUN_14065f9a0).
uint32_t FrameTime_GetRunTime();
// The last frame's length in milliseconds (FUN_14065f9b0).
uint32_t FrameTime_GetElapsed();
float FrameTime_GetTimeSeconds();
int32_t FrameTime_GetFrames();
float FrameTime_GetElapsedSeconds();
void FrameTime_Update(float seconds, uint32_t now);

#endif
