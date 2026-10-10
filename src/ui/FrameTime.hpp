#ifndef UI_FRAME_TIME_HPP
#define UI_FRAME_TIME_HPP

// FrameTime: the UI's clock and its timers (Engine FrameTime::Timer).

#include <cstdint>

// Takes the starting time (FUN_14065f9e0).
void FrameTime_Initialize();
// The clock in milliseconds (FUN_14065f960).
uint32_t FrameTime_GetTime();
// The last frame's length in milliseconds (FUN_14065f9b0).
uint32_t FrameTime_GetElapsed();

#endif
