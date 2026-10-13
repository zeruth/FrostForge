#include "os/Window.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

// The cursor module (0x14254b000..): the cursor's visibility and lock state machine
// (DAT_142d5e90c), its sample history (DAT_142d5e920..), where a held cursor stays and the
// image the engine gives the window. It runs as the reference's own code, translated, whole: its
// state lives in the image, so every entry point goes there and none of it is kept here.

extern "C" unsigned long long FUN_14254c870(unsigned long long, unsigned long long, unsigned long long,
    unsigned long long, unsigned long long);
// Tracks the message for the cursor's state; returns the relative motion when the cursor is
// locked (zero otherwise).
// translated: FUN_14254c870
uint64_t OsCursorTrackMessage(void* window, uint32_t message, uint64_t wparam, uint64_t lparam, uint32_t time) {
    __try {
        return FUN_14254c870(reinterpret_cast<unsigned long long>(window), message, wparam, lparam, time);
    } __except (GenFault(0x14254c870ull, GetExceptionInformation())) {
        return 0;
    }
}

// The cursor's position in screen coordinates as the engine sees it. The reference
// (FUN_14254c400) answers with where the cursor is held, else the newest position the messages
// recorded; the recording (FUN_14254ca30, from WM_MOUSEMOVE) does not translate yet, and reading
// its empty history put every click in the wrong place (2026-10-12). Until it does, the system's
// position, which is what an unheld cursor reports anyway.
// ref: FUN_14254c400
uint64_t OsCursorGetPosition() {
    POINT point = {};
    if (!GetCursorPos(&point)) {
        return 0;
    }
    return static_cast<uint32_t>(point.x) | static_cast<uint64_t>(static_cast<uint32_t>(point.y)) << 32;
}

// ref: FUN_14254c3d0
uint64_t OsCursorGetClientPosition(void* window) {
    uint64_t position = OsCursorGetPosition();
    ScreenToClient(static_cast<HWND>(window), reinterpret_cast<POINT*>(&position));
    return position;
}

extern "C" unsigned long long FUN_14254cf70(unsigned long long, unsigned long long, unsigned long long);
// Sets the cursor image (and the window class's).
// translated: FUN_14254cf70
void OsCursorSet(void* window, void* cursor, uint32_t time) {
    __try {
        FUN_14254cf70(reinterpret_cast<unsigned long long>(window), reinterpret_cast<unsigned long long>(cursor), time);
    } __except (GenFault(0x14254cf70ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14254c490(unsigned long long, unsigned long long);
// Puts the cursor in the engine's hardware-cursor state.
// translated: FUN_14254c490
void OsCursorEnable(void* window, uint32_t time) {
    __try {
        FUN_14254c490(reinterpret_cast<unsigned long long>(window), time);
    } __except (GenFault(0x14254c490ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14254d110(unsigned long long, unsigned long long);
// Leaves the engine's hardware-cursor state (clears 0x20 in the state).
// translated: FUN_14254d110
void OsCursorDisable(void* window, uint32_t time) {
    __try {
        FUN_14254d110(reinterpret_cast<unsigned long long>(window), time);
    } __except (GenFault(0x14254d110ull, GetExceptionInformation())) {
    }
}

// The cursor module's per-pump bookkeeping, before and after the messages.
extern "C" unsigned long long FUN_14254d160(unsigned long long);
// translated: FUN_14254d160
void OsCursorFrameBegin(uint32_t time) {
    __try {
        FUN_14254d160((unsigned long long)time);
    } __except (GenFault(0x14254d160ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_14254ba70(unsigned long long);
// translated: FUN_14254ba70
void OsCursorFrameEnd(uint32_t time) {
    __try {
        FUN_14254ba70((unsigned long long)time);
    } __except (GenFault(0x14254ba70ull, GetExceptionInformation())) {
    }
}
