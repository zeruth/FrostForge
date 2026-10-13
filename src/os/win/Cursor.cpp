#include "os/Window.hpp"
#include <windows.h>
#include <cstdlib>
#include <cstring>

// The cursor module (0x14254b000..): the system cursor's visibility and hold state machine, the
// history of positions the mouse messages reported, where a held cursor is kept, and the image
// the system shows for each state.
//
// The state's bits: 0x1 a remote session (the samples come late and in bursts), 0x2 held (the
// cursor is kept in place while the engine reads relative motion), 0x4 held without
// re-centering, 0x8 held but given up on (a remote session stopped answering the warps), 0x10
// letting go (waiting for the warp back to land), 0x20 the engine draws the cursor, 0x40 the
// window inactive, 0x80 minimized. The system shows one of four things for a state: the
// application's cursor (0), the blank cursor (1, the engine's own in a remote session), the
// move cursor (2, held without re-centering), or nothing (3, the engine's own locally).
//
// A sample is a position the system reported, with when and in which frame. The mouse moves
// report positions the warps caused as well as the user's, so each report is matched to the
// nearest sample (predicted by the recent velocity) and only motion from a sample the history
// has not seen yet counts as the user's.

namespace {

// A position report: its sequence number, time, position, the frame it came in, and the count
// of mouse moves so far (0 for a warp).
struct CURSORRECORD {
    uint32_t m_sequence;    // 0x00
    uint32_t m_time;        // 0x04
    POINT m_position;       // 0x08
    uint32_t m_frame;       // 0x10
    uint32_t m_moves;       // 0x14
};

// A sample: an id, its state, when it was made, and the latest report matched to it. Flags:
// 0x1 an older sample was matched after it, 0x2 matched while newest, 0x4 both, 0x8 moved,
// 0x10 settled (the cursor is known to be there), 0x20 expired.
struct CURSORSAMPLE {
    uint32_t m_id;          // 0x00
    uint32_t m_flags;       // 0x04
    uint32_t m_created;     // 0x08
    CURSORRECORD m_record;  // 0x0C
};
static_assert(sizeof(CURSORSAMPLE) == 0x24, "a cursor sample is 0x24 bytes");

// The sample history: a vector with room for eight in place, constructed on first use
// (DAT_142d5e920 is the guard), through the CRT heap allocator at DAT_142d5e918.
struct CURSORSAMPLES {
    CURSORSAMPLE* m_data;           // DAT_142d5e938
    uint32_t m_size;                // DAT_142d5e940
    uint32_t m_capacity;            // DAT_142d5e944 the high bit: the storage is not owned
    CURSORSAMPLE m_inline[8];       // DAT_142d5e948
};

uint32_t s_sampleTimeout = 20;          // DAT_142d5e8e0 ms a sample stays current
uint32_t s_sampleTimeoutRemote = 200;   // DAT_142d5e8e4 in a remote session
uint32_t s_velocityWindow = 6;          // DAT_142d5e8e8 ms of motion before a velocity is taken
uint32_t s_predictMin = 3;              // DAT_142d5e8ec ms a prediction looks ahead at least
uint32_t s_predictMax = 30;             // DAT_142d5e8f0 and at most
uint32_t s_predictMaxRemote = 100;      // DAT_142d5e8f4 in a remote session
int32_t s_predictDivisor = 1;           // DAT_142d5e8f8
int32_t s_predictDivisorRemote = 2;     // DAT_142d5e8fc
uint32_t s_holdGrace = 500;             // DAT_142d5e900 ms after a hold the cursor is re-centered early
int32_t s_holdSlop = 10;                // DAT_142d5e904 px a held cursor may wander before it counts as moved
uint32_t s_staleLimit = 3;              // DAT_142d5e908 unanswered warps before a remote hold gives up
uint32_t s_cursorState = 0xc0;          // DAT_142d5e90c
uint8_t s_trackLeave = 1;               // DAT_142d5e910 the next mouse move asks for WM_MOUSELEAVE
uint32_t s_cursorFrame = 1;             // DAT_142d5e914 counts the frames that saw a message
int32_t s_samplesConstructed;           // DAT_142d5e920
CURSORSAMPLES s_samples;                // DAT_142d5e938

uint8_t s_initialized;                  // DAT_1447ceac0
uint8_t s_remoteSession;                // DAT_1447ceac1
uint8_t s_holdMoved;                    // DAT_1447ceac2 the held cursor left its slop or was warped
uint8_t s_messageSeen;                  // DAT_1447ceac3 this frame
uint32_t s_remoteCheckTime;             // DAT_1447ceac4 when the session was last checked
HWND s_holdWindow;                      // DAT_1447ceac8
uint32_t s_holdTime;                    // DAT_1447cead0
uint8_t s_haveMessagePosition;          // DAT_1447cead4
uint8_t s_velocityStarted;              // DAT_1447cead5
POINT s_holdCenter;                     // DAT_1447cead8 where the cursor was when the hold began
POINT s_heldPosition;                   // DAT_1447ceae0 where the engine has it while held
uint32_t s_releaseSample;               // DAT_1447ceae8 the warp that lets go
uint32_t s_messageTime;                 // DAT_1447ceaec
HCURSOR s_appCursor;                    // DAT_1447ceaf0
HCURSOR s_blankCursor;                  // DAT_1447ceaf8
int32_t s_frameDepth;                   // DAT_1447ceb00
uint32_t s_staleWarps;                  // DAT_1447ceb04
uint32_t s_lastSampleId;                // DAT_1447ceb08
uint32_t s_sequence;                    // DAT_1447ceb0c
uint32_t s_moves;                       // DAT_1447ceb10
uint32_t s_currentSample;               // DAT_1447ceb14
POINT s_messagePosition;                // DAT_1447ceb18
uint32_t s_movedSample;                 // DAT_1447ceb20
uint32_t s_movedTime;                   // DAT_1447ceb24
POINT s_motion;                         // DAT_1447ceb28 accumulated since s_motionStart
uint32_t s_motionStart;                 // DAT_1447ceb30
uint32_t s_velocitySpan;                // DAT_1447ceb34 ms s_velocity was taken over
POINT s_velocity;                       // DAT_1447ceb38
uint32_t s_velocityTime;                // DAT_1447ceb40
uint32_t s_velocityMoves;               // DAT_1447ceb44
void (*s_visibilityCallback)(bool shown, void* param);  // DAT_1447ceb48
void* s_visibilityParam;                // DAT_1447ceb50

}

// The CRT heap allocator's two halves (the allocator object at DAT_142d5e918 reaches them through
// FUN_14254b3e0 and FUN_14254b4e0): the block is over-allocated and the original pointer kept
// just below the aligned one.
// ref: FUN_1423638d0
void* CrtAlignedAlloc(uint64_t size, uint64_t alignment) {
    uint64_t align = 8;
    if (alignment > 8) {
        align = alignment;
    }
    uint8_t* block = static_cast<uint8_t*>(malloc(size + 8 + align));
    if (block == nullptr) {
        return nullptr;
    }
    uint8_t* aligned = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(block + align + 8) & ~(align - 1));
    reinterpret_cast<void**>(aligned)[-1] = block;
    return aligned;
}

// ref: FUN_142363920
void CrtAlignedFree(void* pointer) {
    if (pointer != nullptr) {
        free(reinterpret_cast<void**>(pointer)[-1]);
    }
}

namespace {

// The history constructed on first use: empty, in its own storage.
CURSORSAMPLES& Samples() {
    if (s_samplesConstructed != 1) {
        s_samples.m_data = s_samples.m_inline;
        s_samples.m_capacity = 0x80000008;
        s_samples.m_size = 0;
        s_samplesConstructed = 1;
    }
    return s_samples;
}

// The ms a sample stays current, for the session.
uint32_t SampleTimeout(uint32_t state) {
    return (state & 1) != 0 ? s_sampleTimeoutRemote : s_sampleTimeout;
}

// Expires every sample before the one with `id`.
void ExpireBefore(uint32_t id) {
    CURSORSAMPLES& samples = Samples();
    CURSORSAMPLE* end = samples.m_data + samples.m_size;
    for (CURSORSAMPLE* sample = samples.m_data; sample != end && sample->m_id != id; sample++) {
        if ((sample->m_flags & 0x20) == 0) {
            sample->m_flags |= 0x20;
        }
    }
}

}

// The session and window state, the first time anything asks: remote or not, active,
// minimized.
// ref: FUN_14254c6a0
void OsCursorInitialize(void* window, uint32_t time) {
    if (s_initialized) {
        return;
    }
    s_initialized = 1;
    int remote = GetSystemMetrics(SM_REMOTESESSION);
    s_remoteSession = remote != 0;
    s_cursorState = remote != 0 ? s_cursorState | 1 : s_cursorState & ~1u;
    s_remoteCheckTime = time != 0 ? time : 1;
    HWND active = GetActiveWindow();
    BOOL iconic = IsIconic(static_cast<HWND>(window));
    uint32_t state = static_cast<HWND>(window) == active ? s_cursorState & ~0x40u : s_cursorState | 0x40;
    s_cursorState = iconic ? state | 0x80 : state & ~0x80u;
}

// Where the system has the cursor, or (when it cannot say) the last message's position or the
// middle of the held window.
// ref: FUN_14254c350
uint64_t OsCursorGetSystemPosition() {
    POINT point;
    if (!GetCursorPos(&point)) {
        if (s_haveMessagePosition) {
            return static_cast<uint32_t>(s_messagePosition.x) |
                static_cast<uint64_t>(static_cast<uint32_t>(s_messagePosition.y)) << 32;
        }
        HWND window = s_holdWindow != nullptr ? s_holdWindow : GetDesktopWindow();
        RECT rect;
        GetWindowRect(window, &rect);
        point.x = (rect.right + rect.left) / 2;
        point.y = (rect.bottom + rect.top) / 2;
    }
    return static_cast<uint32_t>(point.x) | static_cast<uint64_t>(static_cast<uint32_t>(point.y)) << 32;
}

// The point on `rect`'s edge a cursor moving along `direction` should be put back to: the
// side it came from, so the motion has the whole rect to run across (the middle when still).
// ref: FUN_14254c230
uint64_t CursorRecenterPoint(const POINT* direction, const RECT* rect) {
    int32_t dx = direction->x;
    int32_t dy = direction->y;
    int32_t centerX = (rect->left + rect->right) / 2;
    int32_t centerY = (rect->top + rect->bottom) / 2;
    int32_t offsetX;
    int32_t offsetY;
    if (dx == 0) {
        if (dy == 0) {
            return static_cast<uint32_t>(centerX) | static_cast<uint64_t>(static_cast<uint32_t>(centerY)) << 32;
        }
        offsetY = (rect->bottom - rect->top) / 2;
        offsetX = 0;
    } else {
        offsetX = (rect->right - rect->left) / 2;
        int32_t halfHeight = (rect->bottom - rect->top) / 2;
        if (dy == 0) {
            offsetY = 0;
        } else {
            int32_t absX = dx < 0 ? -dx : dx;
            int32_t absY = dy < 0 ? -dy : dy;
            offsetY = absY * offsetX / absX;
            offsetX = absX * offsetX / absX;
            if (halfHeight < offsetY) {
                offsetX = absX * halfHeight / absY;
                offsetY = absY * halfHeight / absY;
            }
        }
        if (dx > 0) {
            offsetX = -offsetX;
        }
    }
    if (dy >= 1) {
        offsetY = -offsetY;
    }
    return static_cast<uint32_t>(offsetX + centerX) | static_cast<uint64_t>(static_cast<uint32_t>(offsetY + centerY)) << 32;
}

// The report matched to `sample`: the motion since its last report (zero for a fresh sample),
// fed to the velocity, which is taken once enough of it has built up over at least
// s_velocityWindow ms; the report then becomes the sample's latest.
// ref: FUN_14254b500
uint64_t CursorSampleUpdate(CURSORSAMPLE* sample, const CURSORRECORD* record) {
    uint64_t motion = 0;
    if (sample->m_record.m_sequence != 0) {
        int32_t dy = record->m_position.y - sample->m_record.m_position.y;
        uint32_t time = record->m_time;
        int32_t dx = record->m_position.x - sample->m_record.m_position.x;
        motion = static_cast<uint32_t>(dx) | static_cast<uint64_t>(static_cast<uint32_t>(dy)) << 32;
        int32_t accumulatedY = 0;
        if (!s_velocityStarted || 250 < time - s_motionStart) {
            s_velocityStarted = 1;
            s_motion.x = 0;
            s_motionStart = time;
        } else {
            accumulatedY = s_motion.y;
        }
        s_motion.x += dx;
        s_motion.y = accumulatedY + dy;
        bool still = dx == 0 && dy == 0;
        uint32_t span = time - s_motionStart;
        if (s_velocityWindow <= span && record->m_moves != s_velocityMoves && !still) {
            s_velocity = s_motion;
            s_motion = {};
            s_motionStart = time;
            s_velocitySpan = span;
            s_velocityTime = time;
            s_velocityMoves = record->m_moves;
        }
    }
    sample->m_record = *record;
    return motion;
}

// A new sample for the report, at the end of the history.
// ref: FUN_14254d240
CURSORSAMPLE* CursorSampleAdd(const CURSORRECORD* record) {
    CURSORSAMPLES& samples = Samples();
    uint32_t size = samples.m_size;
    uint32_t capacity = samples.m_capacity & 0x7fffffff;
    if (size == capacity && capacity < size + 1) {
        uint32_t grown = capacity + ((samples.m_capacity >> 1) & 0x3fffffff);
        if (grown < size + 1) {
            grown = size + 1;
        }
        if (grown < 8) {
            grown = 8;
        }
        CURSORSAMPLE* data = static_cast<CURSORSAMPLE*>(CrtAlignedAlloc(static_cast<uint64_t>(grown) * sizeof(CURSORSAMPLE), 4));
        memcpy(data, samples.m_data, static_cast<uint64_t>(samples.m_size) * sizeof(CURSORSAMPLE));
        if (static_cast<int32_t>(samples.m_capacity) >= 0) {
            CrtAlignedFree(samples.m_data);
        }
        samples.m_capacity = grown & 0x7fffffff;
        samples.m_data = data;
        size = samples.m_size;
    }
    CURSORSAMPLE* sample = samples.m_data + size;
    *sample = {};
    samples.m_size = size + 1;
    sample->m_id = ++s_lastSampleId;
    sample->m_created = record->m_time;
    CursorSampleUpdate(sample, record);
    return sample;
}

// The newest sample not expired (the newest of all when every one has).
// ref: FUN_14254c760
CURSORSAMPLE* CursorSampleNewest() {
    CURSORSAMPLES& samples = Samples();
    for (CURSORSAMPLE* sample = samples.m_data + samples.m_size; samples.m_data < sample;) {
        sample--;
        if ((sample->m_flags & 0x20) == 0) {
            return sample;
        }
    }
    return samples.m_data + (samples.m_size - 1);
}

// The live sample the report most likely came from: the nearest, each one moved along the
// recent velocity for the time since its report (a tie goes to the later report).
// ref: FUN_14254bf70
CURSORSAMPLE* CursorSampleNearest(const CURSORRECORD* record) {
    CURSORSAMPLES& samples = Samples();
    if (samples.m_size == 1 && (samples.m_data[0].m_flags & 0x20) == 0) {
        return samples.m_data;
    }
    uint32_t predictMax = s_predictMax;
    int32_t divisor = s_predictDivisor;
    if ((s_cursorState & 1) != 0) {
        predictMax = s_predictMaxRemote;
        divisor = s_predictDivisorRemote;
    }
    CURSORSAMPLE* best = nullptr;
    uint32_t bestDistance = 0xffffffff;
    CURSORSAMPLE* end = samples.m_data + samples.m_size;
    for (CURSORSAMPLE* sample = samples.m_data; sample != end; sample++) {
        if ((sample->m_flags & 0x20) != 0) {
            continue;
        }
        int32_t x = sample->m_record.m_position.x;
        int32_t y = sample->m_record.m_position.y;
        uint32_t age = record->m_time - sample->m_record.m_time;
        if (s_velocitySpan - 1 < 0xf9 && record->m_time - s_velocityTime < 0xfa && age < 0xfa) {
            uint32_t ahead = predictMax;
            if (age <= predictMax) {
                ahead = age;
                if (age < s_predictMin) {
                    ahead = s_predictMin;
                }
            }
            int32_t aheadX = static_cast<int32_t>(static_cast<int64_t>(static_cast<int32_t>(s_velocity.x * ahead)) /
                static_cast<int64_t>(s_velocitySpan));
            int32_t aheadY = static_cast<int32_t>(static_cast<int64_t>(static_cast<int32_t>(s_velocity.y * ahead)) /
                static_cast<int64_t>(s_velocitySpan));
            x += aheadX / divisor;
            y += aheadY / divisor;
        }
        int32_t dy = y - record->m_position.y;
        int32_t dx = x - record->m_position.x;
        uint32_t distance = static_cast<uint32_t>(dy * dy + dx * dx);
        if (best == nullptr || distance < bestDistance ||
            (distance == bestDistance && best->m_record.m_sequence < sample->m_record.m_sequence)) {
            best = sample;
            bestDistance = distance;
        }
    }
    return best;
}

// Expires the samples before `newest` that have gone `timeout` ms without a report (all of
// them when the timeout is 0), except the current one when there is a timeout.
// ref: FUN_14254ce40
void CursorSampleExpire(uint32_t time, const CURSORSAMPLE* newest, uint32_t timeout) {
    uint32_t current = s_currentSample;
    CURSORSAMPLES& samples = Samples();
    CURSORSAMPLE* end = samples.m_data + samples.m_size;
    for (CURSORSAMPLE* sample = samples.m_data; sample != end; sample++) {
        if (sample->m_id == newest->m_id) {
            return;
        }
        if ((sample->m_flags & 0x20) == 0 && (timeout == 0 || sample->m_id != current) &&
            timeout <= time - sample->m_record.m_time) {
            sample->m_flags |= 0x20;
        }
    }
}

// Whether the warp that lets go has landed: true unless it is the newest sample, still live and
// unsettled, and younger than twice the timeout. The reason is never filled in.
// ref: FUN_14254d770
bool CursorReleaseLanded(uint32_t time, const char** reason) {
    *reason = "";
    CURSORSAMPLES& samples = Samples();
    if (samples.m_size != 0) {
        CURSORSAMPLE* last = samples.m_data + (samples.m_size - 1);
        if (last->m_id == s_releaseSample && (last->m_flags & 0x20) == 0 && (last->m_flags & 0x10) == 0) {
            return SampleTimeout(s_cursorState) * 2 < time - last->m_created;
        }
    }
    return true;
}

// Moves the system cursor to `position` (screen coordinates) and records the warp as a sample
// of its own, so the mouse move it causes is not taken for the user's; the history is first
// given the cursor's present position if it has nothing. The warp's sample id.
// ref: FUN_14254cfd0
uint32_t CursorWarp(uint32_t time, const POINT* position) {
    uint32_t frame = 0;
    if (s_frameDepth != 0) {
        frame = s_cursorFrame;
    }
    CURSORRECORD record;
    if (Samples().m_size == 0) {
        uint64_t here = OsCursorGetSystemPosition();
        s_sequence++;
        s_moves++;
        record.m_sequence = s_sequence;
        record.m_time = time;
        record.m_position.x = static_cast<int32_t>(here);
        record.m_position.y = static_cast<int32_t>(here >> 32);
        record.m_frame = frame;
        record.m_moves = s_moves;
        CURSORSAMPLE* sample = CursorSampleAdd(&record);
        sample->m_flags |= 0x10;
        s_currentSample = sample->m_id;
    }
    SetCursorPos(position->x, position->y);
    s_sequence++;
    record.m_sequence = s_sequence;
    record.m_time = time;
    record.m_position = *position;
    record.m_frame = frame;
    record.m_moves = 0;
    return CursorSampleAdd(&record)->m_id;
}

// Puts a held cursor back where the motion has room: when it has left the window's middle half
// (or during the grace after the hold began, its middle less 100 px) heading outwards, or
// always in a remote session, it goes to the edge of the middle third it came from.
// ref: FUN_14254b620
void CursorRecenter(uint32_t time) {
    uint64_t position = static_cast<uint32_t>(s_messagePosition.x) |
        static_cast<uint64_t>(static_cast<uint32_t>(s_messagePosition.y)) << 32;
    if (!s_haveMessagePosition) {
        position = OsCursorGetSystemPosition();
    }
    RECT client;
    GetClientRect(s_holdWindow, &client);
    int32_t width = client.right - client.left;
    int32_t height = client.bottom - client.top;
    RECT screen = client;
    ClientToScreen(s_holdWindow, reinterpret_cast<POINT*>(&screen.left));
    ClientToScreen(s_holdWindow, reinterpret_cast<POINT*>(&screen.right));
    int32_t insetX = width / 4;
    int32_t insetY = height / 4;
    bool grace;
    if (!s_holdMoved && time - s_holdTime < s_holdGrace) {
        grace = true;
        if (400 < width && 400 < height) {
            insetX = 100;
            insetY = 100;
        }
    } else {
        grace = false;
    }
    RECT inner = screen;
    InflateRect(&inner, -insetX, -insetY);
    POINT velocity = {};
    if (time - s_velocityTime < 0xfa) {
        velocity = s_velocity;
    }
    int32_t x = static_cast<int32_t>(position);
    int32_t y = static_cast<int32_t>(position >> 32);
    uint64_t target = position;
    if ((x < inner.left && (grace || velocity.x < 0)) || (inner.right < x && (grace || 0 < velocity.x)) ||
        (y < inner.top && (grace || velocity.y < 0)) || (inner.bottom < y && (grace || 0 < velocity.y)) ||
        s_remoteSession) {
        RECT outer = screen;
        // a third in from each side: (w/3 - w)/2, the third taken as the compiler does it
        int32_t thirdX = static_cast<int32_t>((static_cast<int64_t>(width) * 0x55555555) >> 32) - width;
        int32_t thirdY = static_cast<int32_t>((static_cast<int64_t>(height) * 0x55555555) >> 32) - height;
        InflateRect(&outer, thirdX / 2, thirdY / 2);
        target = CursorRecenterPoint(&velocity, &outer);
        if (static_cast<int32_t>(target) != x) {
            POINT point = {static_cast<int32_t>(target), static_cast<int32_t>(target >> 32)};
            CursorWarp(time, &point);
            s_holdMoved = 1;
            return;
        }
    }
    if (static_cast<int32_t>(target >> 32) == y) {
        return;
    }
    POINT point = {static_cast<int32_t>(target), static_cast<int32_t>(target >> 32)};
    CursorWarp(time, &point);
    s_holdMoved = 1;
}

// What the system shows for a state: the application's cursor (0), the blank one (1), the move
// cursor (2) or nothing (3).
static uint8_t CursorStateShows(uint8_t state) {
    if ((state & 0xc0) != 0 || (state & 0x32) == 0) {
        return 0;
    }
    if ((state & 2) == 0 || (state & 0xc) == 0) {
        return static_cast<uint8_t>((~state & 1) * 2 | 1);
    }
    return 2;
}

// The state goes from `from` to `to`: the system shows what the new state shows (hiding or
// showing the cursor when the engine takes it over or gives it back); a hold that begins takes
// the cursor's place and re-centers it, one that ends warps the cursor back to where the
// engine had it; an inactive window forgets the history.
// ref: FUN_14254d4b0
void CursorStateChange(uint32_t time, void* window, uint8_t from, uint8_t to) {
    uint8_t shown = CursorStateShows(from);
    uint8_t shows = CursorStateShows(to);
    if (shown != shows) {
        HCURSOR cursor = nullptr;
        LPCSTR fallback = nullptr;
        if (shows == 0) {
            cursor = s_appCursor;
            fallback = MAKEINTRESOURCEA(0x7f00);
        } else if (shows == 1) {
            cursor = s_blankCursor;
            fallback = MAKEINTRESOURCEA(0x7f03);
        } else if (shows == 2) {
            fallback = MAKEINTRESOURCEA(0x7f86);
        }
        if (shows != 3) {
            if (cursor == nullptr) {
                cursor = LoadCursorA(nullptr, fallback);
            }
            if (cursor != nullptr) {
                SetCursor(cursor);
                if (window != nullptr) {
                    SetClassLongPtrW(static_cast<HWND>(window), GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(cursor));
                }
            }
        }
        if ((shown == 3) != (shows == 3)) {
            ShowCursor(shows != 3);
            if (s_visibilityCallback != nullptr) {
                s_visibilityCallback(shows != 3, s_visibilityParam);
            }
        }
    }
    bool wasHeld = (from & 2) != 0 && (from & 0xc0) == 0;
    bool held = (to & 2) != 0 && (to & 0xc0) == 0;
    if (!wasHeld) {
        if (held) {
            if ((from & 0xc0) != 0) {
                uint64_t here = OsCursorGetSystemPosition();
                s_holdCenter = {static_cast<int32_t>(here), static_cast<int32_t>(here >> 32)};
                s_heldPosition = s_holdCenter;
                s_holdTime = time;
                s_holdMoved = 0;
            }
            if ((to & 0xc) == 0) {
                CursorRecenter(time);
            }
        }
    } else if (!held && (to & 0xc) == 0) {
        if (s_holdMoved || s_heldPosition.x != s_holdCenter.x || s_heldPosition.y != s_holdCenter.y) {
            uint32_t sample = CursorWarp(time, &s_heldPosition);
            if ((to & 0x10) != 0) {
                s_releaseSample = sample;
            }
        }
    }
    if ((from & 0xc0) == 0 && (to & 0xc0) != 0) {
        s_trackLeave = 1;
        s_currentSample = 0;
        s_velocityStarted = from & 0xc0;
        s_motion = {};
        s_motionStart = 0;
        s_velocity = {};
        s_velocitySpan = 0;
        s_velocityTime = 0;
        s_velocityMoves = 0;
        Samples().m_size = 0;
    }
}

// The blank cursor (fully transparent but for one barely visible pixel, so the system keeps
// drawing it), made once; the cross when it cannot be.
// ref: FUN_14254c4f0
void CursorCreateBlank(void* window) {
    if (s_blankCursor != nullptr) {
        return;
    }
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = 32;
    header.bV5Height = -32;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    uint32_t* bits = nullptr;
    HDC dc = GetDC(static_cast<HWND>(window));
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
        reinterpret_cast<void**>(&bits), nullptr, 0);
    ReleaseDC(static_cast<HWND>(window), dc);
    if (color != nullptr && bits != nullptr) {
        memset(bits, 0, 0x1000);
        bits[0] = 0x01777777;
        HBITMAP mask = CreateBitmap(32, 32, 1, 1, nullptr);
        ICONINFO info = {};
        info.hbmMask = mask;
        info.hbmColor = color;
        HICON icon = CreateIconIndirect(&info);
        DeleteObject(mask);
        DeleteObject(color);
        if (icon != nullptr) {
            s_blankCursor = icon;
            return;
        }
    }
    s_blankCursor = nullptr;
    s_blankCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(0x7f03));
}

// A mouse move: its position recorded (screen coordinates) and matched to the history. The
// motion it carries is the user's when it came from a sample the history already had; a move
// from a warp is not. The motion, while the cursor is held.
// ref: FUN_14254ca30
uint64_t CursorOnMouseMove(uint32_t time, void* window, uint64_t lparam) {
    if ((s_cursorState & 0xc0) != 0) {
        return 0;
    }
    POINT point = {static_cast<int16_t>(lparam), static_cast<int16_t>(lparam >> 16)};
    ClientToScreen(static_cast<HWND>(window), &point);
    uint32_t state = s_cursorState;
    s_messagePosition = point;
    s_haveMessagePosition = 1;
    if ((state & 2) != 0 &&
        (point.x < s_holdCenter.x - s_holdSlop || s_holdSlop + s_holdCenter.x < point.x ||
         point.y < s_holdCenter.y - s_holdSlop || s_holdCenter.y + s_holdSlop < point.y)) {
        s_holdMoved = 1;
    }
    s_sequence++;
    s_moves++;
    CURSORRECORD record;
    record.m_sequence = s_sequence;
    record.m_time = time;
    record.m_position = point;
    record.m_frame = s_cursorFrame;
    record.m_moves = s_moves;
    bool fresh = s_samplesConstructed != 1;
    CURSORSAMPLES& samples = Samples();
    if (fresh || samples.m_size == 0 || (s_trackLeave && (CursorSampleNewest()->m_flags & 0x10) != 0)) {
        CURSORSAMPLE* sample = CursorSampleAdd(&record);
        sample->m_flags |= 0x10;
        s_currentSample = sample->m_id;
        ExpireBefore(sample->m_id);
        return 0;
    }
    CURSORSAMPLE* nearest = CursorSampleNearest(&record);
    uint64_t motion = CursorSampleUpdate(nearest, &record);
    CURSORSAMPLE* newest = CursorSampleNewest();
    bool moved = false;
    if (static_cast<int32_t>(motion) != 0 || static_cast<int32_t>(motion >> 32) != 0) {
        moved = (state & 1) == 0 && (nearest->m_flags & 8) == 0;
        s_movedSample = nearest->m_id;
        nearest->m_flags |= 8;
        s_movedTime = time;
    }
    bool settle = moved;
    if (nearest->m_id < newest->m_id) {
        uint32_t flags = newest->m_flags;
        newest->m_flags = flags | 1;
        if ((flags & 2) != 0) {
            newest->m_flags = flags | 5;
        }
    } else if (nearest->m_id == newest->m_id) {
        uint32_t flags = newest->m_flags;
        newest->m_flags = flags | 2;
        if ((state & 1) != 0 &&
            ((flags & 4) != 0 || ((flags & 8) != 0 && s_sampleTimeoutRemote < record.m_time - nearest->m_created))) {
            settle = true;
        }
    }
    if (settle && (nearest->m_flags & 0x10) == 0) {
        ExpireBefore(nearest->m_id);
        nearest->m_flags |= 0x10;
        if ((state & 2) != 0 && (state & 0xc0) == 0 &&
            nearest->m_id == Samples().m_data[Samples().m_size - 1].m_id) {
            s_staleWarps = 0;
            s_remoteSession = 0;
        }
    }
    if ((nearest->m_flags & 0x10) != 0) {
        s_currentSample = nearest->m_id;
    }
    return motion;
}

// A remote hold that keeps losing its warps gives up re-centering.
// ref: FUN_14254b840
void CursorHoldGiveUp(void* window, uint32_t time) {
    uint32_t state = s_cursorState;
    uint32_t stale = state | 8;
    CursorStateChange(time, window, static_cast<uint8_t>(state), static_cast<uint8_t>(stale));
    s_cursorState = stale;
    if ((state & 0xc) == 0 && (state & 0x10) != 0) {
        uint32_t settled = (state & ~0x10u) | 8;
        CursorStateChange(time, window, static_cast<uint8_t>(stale), static_cast<uint8_t>(settled));
        s_cursorState = settled;
    }
}

// The message, for the cursor's state: a session check every ten seconds, the window's
// minimize and activation, and the mouse moves (the motion while held is the result).
// ref: FUN_14254c870
uint64_t OsCursorTrackMessage(void* window, uint32_t message, uint64_t wparam, uint64_t lparam, uint32_t time) {
    OsCursorInitialize(window, time);
    if (10000 < time - s_remoteCheckTime) {
        int remote = GetSystemMetrics(SM_REMOTESESSION);
        uint32_t state = remote != 0 ? s_cursorState | 1 : s_cursorState & ~1u;
        CursorStateChange(time, window, static_cast<uint8_t>(s_cursorState), static_cast<uint8_t>(state));
        s_remoteCheckTime = 1;
        s_cursorState = state;
        if (time != 0) {
            s_remoteCheckTime = time;
        }
    }
    s_messageTime = time;
    s_messageSeen = 1;
    uint32_t state;
    if (message == WM_SIZE) {
        state = IsIconic(static_cast<HWND>(window)) ? s_cursorState | 0x80 : s_cursorState & ~0x80u;
    } else if (message == WM_ACTIVATE) {
        state = static_cast<int16_t>(wparam) == 0 ? s_cursorState | 0x40 : s_cursorState & ~0x40u;
    } else if (message == WM_ACTIVATEAPP) {
        state = wparam == 0 ? s_cursorState | 0x40 : s_cursorState & ~0x40u;
    } else if (message == WM_MOUSEMOVE) {
        uint64_t motion = CursorOnMouseMove(time, window, lparam);
        if (s_trackLeave) {
            s_trackLeave = 0;
            TRACKMOUSEEVENT track = {};
            track.cbSize = sizeof(TRACKMOUSEEVENT);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = static_cast<HWND>(window);
            track.dwHoverTime = 0;
            TrackMouseEvent(&track);
        }
        return motion;
    } else if (message == WM_MOUSELEAVE) {
        s_trackLeave = 1;
        s_messageSeen = 1;
        return 0;
    } else {
        s_messageSeen = 1;
        return 0;
    }
    CursorStateChange(s_messageTime, window, static_cast<uint8_t>(s_cursorState), static_cast<uint8_t>(state));
    s_cursorState = state;
    return 0;
}

// The cursor's position in screen coordinates as the engine sees it: where it is held, else the
// newest live sample, else the system's.
// ref: FUN_14254c400
uint64_t OsCursorGetPosition() {
    if ((s_cursorState & 2) != 0 && (s_cursorState & 0xc0) == 0) {
        return static_cast<uint32_t>(s_heldPosition.x) | static_cast<uint64_t>(static_cast<uint32_t>(s_heldPosition.y)) << 32;
    }
    if (Samples().m_size != 0) {
        CURSORSAMPLE* sample = CursorSampleNewest();
        return static_cast<uint32_t>(sample->m_record.m_position.x) |
            static_cast<uint64_t>(static_cast<uint32_t>(sample->m_record.m_position.y)) << 32;
    }
    return OsCursorGetSystemPosition();
}


// ref: FUN_14254c3d0
uint64_t OsCursorGetClientPosition(void* window) {
    uint64_t position = OsCursorGetPosition();
    ScreenToClient(static_cast<HWND>(window), reinterpret_cast<POINT*>(&position));
    return position;
}

// Puts the cursor at a client position: the held position while held, else a warp.
// ref: FUN_14254cef0
void OsCursorSetPosition(void* window, const uint64_t* position, uint32_t time) {
    OsCursorInitialize(window, time);
    uint64_t point = *position;
    ClientToScreen(static_cast<HWND>(window), reinterpret_cast<POINT*>(&point));
    if ((s_cursorState & 2) == 0) {
        if ((s_cursorState & 0xc0) == 0) {
            CursorWarp(time, reinterpret_cast<POINT*>(&point));
        }
    } else if ((s_cursorState & 0xc0) == 0) {
        s_heldPosition = *reinterpret_cast<POINT*>(&point);
    }
}

// Sets the application's cursor image (and the window class's), shown now when the state shows
// it.
// ref: FUN_14254cf70
void OsCursorSet(void* window, void* cursor, uint32_t time) {
    OsCursorInitialize(window, time);
    s_appCursor = static_cast<HCURSOR>(cursor);
    if ((s_cursorState & 0xc0) != 0 || (s_cursorState & 0x32) == 0) {
        SetCursor(static_cast<HCURSOR>(cursor));
        if (window != nullptr) {
            SetClassLongPtrW(static_cast<HWND>(window), GCLP_HCURSOR, reinterpret_cast<LONG_PTR>(cursor));
        }
    }
}

// The engine draws the cursor (state 0x20): the system shows the blank cursor or none.
// ref: FUN_14254c490
void OsCursorEnable(void* window, uint32_t time) {
    OsCursorInitialize(window, time);
    CursorCreateBlank(window);
    uint32_t state = s_cursorState | 0x20;
    CursorStateChange(time, window, static_cast<uint8_t>(s_cursorState), static_cast<uint8_t>(state));
    s_cursorState = state;
}

// ref: FUN_14254d110
void OsCursorDisable(void* window, uint32_t time) {
    OsCursorInitialize(window, time);
    uint32_t state = s_cursorState & ~0x20u;
    CursorStateChange(time, window, static_cast<uint8_t>(s_cursorState), static_cast<uint8_t>(state));
    s_cursorState = state;
}

// Holds the cursor in `window` (relative motion), re-centering it unless `noRecenter`.
// ref: FUN_14254b980
void OsCursorHold(void* window, uint32_t time, bool noRecenter) {
    OsCursorInitialize(window, time);
    if ((s_cursorState & 2) != 0) {
        return;
    }
    CursorCreateBlank(window);
    s_holdWindow = static_cast<HWND>(window);
    if ((s_cursorState & 0xc0) == 0) {
        uint64_t here = OsCursorGetPosition();
        s_holdCenter = {static_cast<int32_t>(here), static_cast<int32_t>(here >> 32)};
        s_holdMoved = 0;
        s_holdTime = time;
        s_heldPosition = s_holdCenter;
    }
    uint32_t state = noRecenter ? s_cursorState | 4 : s_cursorState & ~4u;
    CursorStateChange(time, window, static_cast<uint8_t>(s_cursorState), static_cast<uint8_t>(state));
    uint32_t held = state | 2;
    s_cursorState = state;
    CursorStateChange(time, window, static_cast<uint8_t>(state), static_cast<uint8_t>(held));
    s_cursorState = held;
    if ((state & 0x10) != 0) {
        uint32_t settled = (state & ~0x10u) | 2;
        CursorStateChange(time, window, static_cast<uint8_t>(held), static_cast<uint8_t>(settled));
        s_cursorState = settled;
    }
}

// Lets a held cursor go: it is warped back to where the engine had it, and the state stays
// letting-go (0x10) until that warp lands.
// ref: FUN_14254b8c0
void OsCursorRelease(uint32_t time) {
    uint32_t state = s_cursorState;
    if ((state & 2) == 0) {
        return;
    }
    uint32_t releasing = state | 0x10;
    s_releaseSample = 0;
    CursorStateChange(time, s_holdWindow, static_cast<uint8_t>(state), static_cast<uint8_t>(releasing));
    uint32_t released = (state & ~2u) | 0x10;
    s_cursorState = releasing;
    CursorStateChange(time, s_holdWindow, static_cast<uint8_t>(releasing), static_cast<uint8_t>(released));
    s_cursorState = released;
    const char* reason;
    if (s_releaseSample == 0 || CursorReleaseLanded(time, &reason)) {
        uint32_t done = state & ~0x12u;
        CursorStateChange(time, s_holdWindow, static_cast<uint8_t>(released), static_cast<uint8_t>(done));
        s_holdWindow = nullptr;
        s_cursorState = done;
    }
}

// Before the frame's messages: stale samples expired, and a pending let-go finished once its
// warp has landed.
// ref: FUN_14254d160
void OsCursorFrameBegin(uint32_t time) {
    uint32_t state = s_cursorState;
    if (Samples().m_size != 0) {
        uint32_t timeout = SampleTimeout(s_cursorState);
        CursorSampleExpire(time, CursorSampleNewest(), timeout);
    }
    if ((state & 0x10) != 0) {
        const char* reason;
        if (CursorReleaseLanded(time, &reason)) {
            CursorStateChange(time, s_holdWindow, static_cast<uint8_t>(state), static_cast<uint8_t>(state & ~0x10u));
            s_holdWindow = nullptr;
            s_cursorState = state & ~0x10u;
        }
    }
    s_frameDepth++;
}

// After the frame's messages: the newest sample checked against where the system says the
// cursor is (locally) or the samples a remote session never answered expired (giving up the hold
// after too many), a held cursor re-centered once its sample settles, and the expired samples
// dropped. The frame number before this frame.
// ref: FUN_14254ba70
uint32_t OsCursorFrameEnd(uint32_t time) {
    uint32_t frame = s_cursorFrame;
    s_frameDepth--;
    if (s_messageSeen) {
        s_cursorFrame++;
    }
    if ((s_cursorState & 0xc0) != 0) {
        return frame;
    }
    if (s_samplesConstructed != 1) {
        Samples();
        return frame;
    }
    if (s_samples.m_size == 0) {
        return frame;
    }
    uint32_t state = s_cursorState;
    if ((state & 1) == 0) {
        CURSORSAMPLE* last = s_samples.m_data + (s_samples.m_size - 1);
        if ((last->m_flags & 0x30) == 0 && s_sampleTimeout <= time - last->m_created) {
            POINT point;
            if (!GetCursorPos(&point) || last->m_record.m_position.x != point.x ||
                last->m_record.m_position.y != point.y) {
                last->m_flags |= 0x20;
                if ((s_cursorState & 0xc) == 0 && ((s_cursorState & 2) == 0 || (s_cursorState & 0xc0) != 0)) {
                    CursorWarp(time, &last->m_record.m_position);
                }
            } else {
                last->m_flags |= 0x10;
                s_currentSample = last->m_id;
                ExpireBefore(last->m_id);
            }
            state = s_cursorState;
        }
    } else {
        CURSORSAMPLE* last = s_samples.m_data + (s_samples.m_size - 1);
        CURSORSAMPLE* end = s_samples.m_data + s_samples.m_size;
        for (CURSORSAMPLE* sample = s_samples.m_data; sample != end; sample++) {
            if ((sample->m_flags & 0x30) == 0 && s_movedSample != 0 && sample->m_id != s_movedSample &&
                s_sampleTimeoutRemote <= s_movedTime - sample->m_created &&
                (sample->m_id != last->m_id || sample->m_created < s_movedTime) &&
                s_sampleTimeoutRemote <= time - sample->m_created) {
                sample->m_flags |= 0x20;
                if ((state & 0xc) == 0 && (state & 2) != 0 && (state & 0xc0) == 0 && sample->m_id == last->m_id &&
                    ++s_staleWarps == s_staleLimit) {
                    CursorHoldGiveUp(s_holdWindow, time);
                    state = s_cursorState;
                }
            }
        }
    }
    CURSORSAMPLES& samples = Samples();
    if ((state & 2) != 0 && (state & 0xc0) == 0 && (state & 0xc) == 0 &&
        SampleTimeout(state) <= time - samples.m_data[samples.m_size - 1].m_created) {
        CursorRecenter(time);
    }
    for (uint32_t i = 0; i < Samples().m_size;) {
        CURSORSAMPLE* sample = s_samples.m_data + i;
        if ((sample->m_flags & 0x20) == 0 || time == sample->m_record.m_time) {
            i++;
            continue;
        }
        uint32_t after = s_samples.m_size - i - 1;
        if (after != 0) {
            memmove(sample, sample + 1, static_cast<uint64_t>(after) * sizeof(CURSORSAMPLE));
        }
        s_samples.m_size--;
    }
    return frame;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142d5e8e0 = (void*)&s_sampleTimeout;
extern "C" void* const gen_p_142d5e8e4 = (void*)&s_sampleTimeoutRemote;
extern "C" void* const gen_p_142d5e900 = (void*)&s_holdGrace;
extern "C" void* const gen_p_142d5e90c = (void*)&s_cursorState;
extern "C" void* const gen_p_142d5e910 = (void*)&s_trackLeave;
extern "C" void* const gen_p_142d5e914 = (void*)&s_cursorFrame;
extern "C" void* const gen_p_142d5e920 = (void*)&s_samplesConstructed;
extern "C" void* const gen_p_142d5e938 = (void*)&s_samples;
extern "C" void* const gen_p_1447ceac0 = (void*)&s_initialized;
extern "C" void* const gen_p_1447ceac1 = (void*)&s_remoteSession;
extern "C" void* const gen_p_1447ceac2 = (void*)&s_holdMoved;
extern "C" void* const gen_p_1447ceac4 = (void*)&s_remoteCheckTime;
extern "C" void* const gen_p_1447ceac8 = (void*)&s_holdWindow;
extern "C" void* const gen_p_1447cead0 = (void*)&s_holdTime;
extern "C" void* const gen_p_1447cead4 = (void*)&s_haveMessagePosition;
extern "C" void* const gen_p_1447cead5 = (void*)&s_velocityStarted;
extern "C" void* const gen_p_1447cead8 = (void*)&s_holdCenter;
extern "C" void* const gen_p_1447ceae0 = (void*)&s_heldPosition;
extern "C" void* const gen_p_1447ceae8 = (void*)&s_releaseSample;
extern "C" void* const gen_p_1447ceaf0 = (void*)&s_appCursor;
extern "C" void* const gen_p_1447ceaf8 = (void*)&s_blankCursor;
extern "C" void* const gen_p_1447ceb00 = (void*)&s_frameDepth;
extern "C" void* const gen_p_1447ceb0c = (void*)&s_sequence;
extern "C" void* const gen_p_1447ceb10 = (void*)&s_moves;
extern "C" void* const gen_p_1447ceb14 = (void*)&s_currentSample;
extern "C" void* const gen_p_1447ceb18 = (void*)&s_messagePosition;
extern "C" void* const gen_p_1447ceb28 = (void*)&s_motion;
extern "C" void* const gen_p_1447ceb30 = (void*)&s_motionStart;
extern "C" void* const gen_p_1447ceb34 = (void*)&s_velocitySpan;
extern "C" void* const gen_p_1447ceb38 = (void*)&s_velocity;
extern "C" void* const gen_p_1447ceb40 = (void*)&s_velocityTime;
extern "C" void* const gen_p_1447ceb44 = (void*)&s_velocityMoves;
extern "C" void* const gen_p_1447ceb50 = (void*)&s_visibilityParam;
// ---- end of generated globals ----
