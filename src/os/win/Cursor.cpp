#include "os/Window.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

// The cursor module (0x14254b000..): the cursor's visibility and lock state machine
// (DAT_142d5e90c), its sample history, and the position the engine reads. Its entry points
// are stubs until the module is ported; the window procedure calls them on every message.

// Tracks the message for the cursor's state; returns the relative motion when the cursor is
// locked (zero otherwise).
// ref: FUN_14254c870
uint64_t OsCursorTrackMessage(void* window, uint32_t message, uint64_t wparam, uint64_t lparam, uint32_t time) {
    (void)window;
    (void)message;
    (void)wparam;
    (void)lparam;
    (void)time;
    FROST_UNIMPLEMENTED();
    return 0;
}

namespace {

// The cursor's state (DAT_142d5e90c): 1 a remote session, 2 held in place, 0x40 the window
// inactive, 0x80 minimized.
uint32_t s_cursorState;
// Where a held cursor stays (DAT_1447ceae0).
uint64_t s_heldPosition;
// The positions the window's messages reported (DAT_142d5e920..): TrackMessage fills them, and
// until it is ported there are none.
uint32_t s_sampleCount;             // DAT_142d5e940
// The window whose middle stands in when the system cannot say (DAT_1447ceac8).
HWND s_cursorWindow;

// The system's cursor position, or the middle of the window (the desktop when there is none)
// when it cannot be had.
// ref: FUN_14254c350
uint64_t CursorSystemPosition() {
    POINT point;
    if (!GetCursorPos(&point)) {
        HWND window = s_cursorWindow != nullptr ? s_cursorWindow : GetDesktopWindow();
        RECT rect;
        GetWindowRect(window, &rect);
        point.x = (rect.right + rect.left) / 2;
        point.y = (rect.bottom + rect.top) / 2;
    }
    return static_cast<uint32_t>(point.x) | static_cast<uint64_t>(static_cast<uint32_t>(point.y)) << 32;
}

// The newest reported position (FUN_14254c760).
// ref: FUN_14254c760
uint64_t CursorLatestSample() {
    FROST_UNIMPLEMENTED();
    return CursorSystemPosition();
}

}  // namespace

// The cursor's position in screen coordinates as the engine sees it: where it is held, the
// last position the messages reported, or the system's.
// ref: FUN_14254c400
uint64_t OsCursorGetPosition() {
    if ((s_cursorState & 2) != 0 && (s_cursorState & 0xc0) == 0) {
        return s_heldPosition;
    }
    if (s_sampleCount != 0) {
        return CursorLatestSample();
    }
    return CursorSystemPosition();
}

// ref: FUN_14254c3d0
uint64_t OsCursorGetClientPosition(void* window) {
    uint64_t position = OsCursorGetPosition();
    ScreenToClient(static_cast<HWND>(window), reinterpret_cast<POINT*>(&position));
    return position;
}

// Sets the cursor image (and the window class's).
// ref: FUN_14254cf70
void OsCursorSet(void* window, void* cursor, uint32_t time) {
    (void)window;
    (void)cursor;
    (void)time;
    FROST_UNIMPLEMENTED();
}

// Puts the cursor in the engine's hardware-cursor state.
// ref: FUN_14254c490
void OsCursorEnable(void* window, uint32_t time) {
    (void)window;
    (void)time;
    FROST_UNIMPLEMENTED();
}

// Leaves the engine's hardware-cursor state (clears 0x20 in the state).
// ref: FUN_14254d110
void OsCursorDisable(void* window, uint32_t time) {
    (void)window;
    (void)time;
    FROST_UNIMPLEMENTED();
}

// The cursor module's per-pump bookkeeping, before and after the messages.
// ref: FUN_14254d160
void OsCursorFrameBegin(uint32_t time) {
    (void)time;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_14254ba70
void OsCursorFrameEnd(uint32_t time) {
    (void)time;
    FROST_UNIMPLEMENTED();
}
