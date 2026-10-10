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

// The cursor's position in screen coordinates as the engine sees it.
// ref: FUN_14254c400
uint64_t OsCursorGetPosition() {
    FROST_UNIMPLEMENTED();
    return 0;
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
