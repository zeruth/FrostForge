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

// The clock since the last mark (FUN_14065f990), and the mark (FUN_14065fd30).
uint32_t FrameTime_GetSinceMark();
void FrameTime_Mark();
uint8_t FrameTime_GetFlag1b8();
void FrameTime_SetFlag1b8();
bool FrameTime_Unknown200Positive();

// The timers (FrameTime::Timer): a callback once the clock passes a delay in nanoseconds, by
// id (FUN_14065fb50); dropping one (FUN_14065fa60); and the once-a-frame run (FUN_14065fd40).
typedef void (*FRAMETIMERCALLBACK)(void* param);
int32_t FrameTime_AddTimer(int64_t delay, FRAMETIMERCALLBACK callback, void* param);
void FrameTime_CancelTimer(int32_t id);
void FrameTime_RunTimers();

#endif
