#include "ui/FrameTime.hpp"
#include "blz/Vector.hpp"

uint32_t OsGetAsyncTimeMs();

namespace {

const char s_timerTag[] = ".?AUTimer@FrameTime@@";
const char s_timerIdTag[] = ".H";

// A timer (FrameTime::Timer, 0x18): its id, when it is due, and what it calls with what.
struct FRAMETIMER {
    int32_t m_id;                   // 0x00
    uint32_t m_due;                 // 0x04 FrameTime_GetTime() at which it fires
    FRAMETIMERCALLBACK m_callback;  // 0x08
    void* m_param;                  // 0x10
};
static_assert(sizeof(FRAMETIMER) == 0x18, "FrameTime::Timer is 0x18 bytes");

blz::vector<int32_t, s_timerIdTag> s_cancelled;     // DAT_142e9a1c0 the ids to drop at the next run
blz::vector<FRAMETIMER, s_timerTag> s_timers;       // DAT_142e9a1d8
int32_t s_lastTimerId;              // DAT_142e9a210 never 0 once issued
float s_unknown200;                 // DAT_142e9a200

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

// The clock since the last mark (FUN_14065f990), and the mark (FUN_14065fd30).
// ref: FUN_14065f990
uint32_t FrameTime_GetSinceMark() {
    return s_time - static_cast<uint32_t>(s_lastTime);
}

// ref: FUN_14065fd30
void FrameTime_Mark() {
    s_lastTime = (s_lastTime & ~0xffffffffull) | s_time;
}

// ref: FUN_14065f9d0
uint8_t FrameTime_GetFlag1b8() {
    return static_cast<uint8_t>(s_unknown1b8);
}

// ref: FUN_14065fb40
void FrameTime_SetFlag1b8() {
    s_unknown1b8 = (s_unknown1b8 & ~0xffu) | 1;
}

// ref: FUN_14065fa40
bool FrameTime_Unknown200Positive() {
    return 0.0f < s_unknown200;
}

// A timer `delay` nanoseconds from the frame's time: its id, never 0 (0 for no callback).
// ref: FUN_14065fb50
int32_t FrameTime_AddTimer(int64_t delay, FRAMETIMERCALLBACK callback, void* param) {
    if (callback == nullptr) {
        return 0;
    }
    int32_t id = s_lastTimerId + 1;
    if (id == 0) {
        id = s_lastTimerId + 2;
    }
    s_lastTimerId = id;
    FRAMETIMER timer;
    timer.m_id = id;
    timer.m_due = static_cast<uint32_t>(static_cast<int32_t>(delay / 1000000)) + s_time;
    timer.m_callback = callback;
    timer.m_param = param;
    s_timers.push_back(timer);
    return id;
}

// The timer is dropped at the next run.
// ref: FUN_14065fa60
void FrameTime_CancelTimer(int32_t id) {
    s_cancelled.push_back(id);
}

// Once a frame: the cancelled timers dropped, then each due timer called and dropped. The
// timers run from a list of their own, so the ones their callbacks add wait for the next frame;
// those that are not due yet go back after them.
// ref: FUN_14065fd40
void FrameTime_RunTimers() {
    for (uint64_t i = 0; i < s_cancelled.size(); i++) {
        for (uint64_t k = 0; k < s_timers.size(); k++) {
            if (s_timers[k].m_id == s_cancelled[i]) {
                s_timers.erase_unordered(k);
                break;
            }
        }
    }
    s_cancelled.m_size = 0;
    if (s_timers.size() == 0) {
        return;
    }
    static blz::vector<FRAMETIMER, s_timerTag> s_running;   // DAT_142e9a218
    uint32_t now = s_time;
    s_running.swap(s_timers);
    for (uint64_t i = 0; i < s_running.size();) {
        FRAMETIMER& timer = s_running[i];
        if (now < timer.m_due) {
            i++;
            continue;
        }
        if (timer.m_callback != nullptr) {
            timer.m_callback(timer.m_param);
        }
        s_running.erase_unordered(i);
    }
    if (s_timers.size() == 0) {
        s_running.swap(s_timers);
        return;
    }
    for (uint64_t i = 0; i < s_running.size(); i++) {
        s_timers.push_back(s_running[i]);
    }
    s_running.m_size = 0;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142e9a1c0 = (void*)&s_cancelled;
// ---- end of generated globals ----
