#include "client/GxWindow.hpp"
#include "client/Startup.hpp"
#include "console/Command.hpp"
#include "gx/Gx.hpp"
#include "os/Os.hpp"
#include "os/Timer.hpp"
#include "os/Window.hpp"
#include "util/Unimplemented.hpp"
#include <cstring>

// The main loop: CommonMain runs Sub140225AB0 while Sub1402225D0 returns true. The condition
// pumps the OS events into the client and advances the game state; the iteration renders.

// The loop's stubs (StartupClient.cpp).
void Sub14065FCE0(uint32_t elapsed, uint32_t now);
void Sub14065FD40();
void AsyncReadPoll();
void Sub14135A290();
void Sub140222A60();
void Sub1402CF8D0();
void Sub14021F090();
void Sub1415642A0();
void Sub14230B040();
void Sub1412E1C00();
uint64_t Sub140216570();
void Sub1412CAB50(uint64_t a1);
void Sub1412E1FB0();
void Sub1402F4D00(uint64_t a1);
void Sub14021C040();
void Sub14228C670();
void Sub1414F8E20();
void Sub141350FD0(bool a1);
void* Sub14192FA70();
void Sub14192FCB0(void* a1);
void Sub1413528E0(bool a1);
void Sub1412EE600();
void Sub1412EA4A0();
void Sub1412E8790();
void Sub14167C520();
void Sub141687ED0();
void Sub141691B00();
void Sub1405F0BE0(void* a1, float elapsed);
void Sub141525060(void* a1, float elapsed);
void Sub140234DF0();
void Sub140234ED0();
void Sub140235040();
void Sub140234080();
void Sub140570630();
void Sub14228CB00();
void ClientOnMouseCapture(uint32_t x, uint32_t y);
void UiInputChar(void* input, uint32_t character);
void UiInputIme(void* input, const void* event);
void ClientOnFocus(uint32_t active);
void ClientKeyBinding(uint32_t key, uint32_t down, char* handled);
bool ConsoleIsToggleKey(uint32_t key);
void OsWindowFocus(void* window);
void UiInputKeyDown(void* input, const void* event);
void UiInputKeyDownBound(void* input, const void* event);
void ClientOnKeyUp(const uint32_t* params, uint32_t time);
uint64_t Sub14152CF90(uint32_t a1);
void UiInputGamepadDown(void* input, const void* event);
void UiInputGamepadUp(void* input, const void* event);
void UiInputGamepadStick(void* input, const void* event);
void OsGetClientRect(int32_t* rect);
void GxSetMouseCursorActive(uint32_t active);
void UiInputMouseDown(void* input, const void* event);
void UiInputMouseMove(void* input, const void* event, int32_t a3);
void UiInputMouseWheel(void* input, const void* event);
void UiInputMouseDelta(void* input, const void* event);
void ClientOnMouseUp(uint32_t button, uint32_t x, uint32_t y, uint32_t a4, uint32_t time, bool a6);
void UiInputDisplayChanged();
void Sub14065FB00();

namespace {

uint8_t s_quitRequested;            // DAT_142ddcf48
uint8_t s_quitHandled;              // DAT_142ddcfac
uint64_t s_quitArgument;            // DAT_142ddcf58
float s_frameSeconds;               // DAT_142ddfa00
uint32_t s_frameTime;               // DAT_142ddf9f4
uint8_t s_paused;                   // DAT_142ddfa04
int32_t s_gameState;                // DAT_142ddfa08: 0 glue, 7 in world
uint8_t s_pendingReset;             // DAT_142ddf9f1
uint8_t s_pendingFlush;             // DAT_142ddf9f0
void* s_uiInput;                    // DAT_142e979d8: the UI's input target
void* s_eventTarget;                // DAT_142de0e18
uint8_t s_eventsPaused;             // DAT_142de0e2c
uint8_t s_swallowChar;              // DAT_142de0e2d
uint32_t s_heldModifiers;           // DAT_142de0e24
uint32_t s_heldButtons;             // DAT_142de0e20
uint32_t s_pressedButtons;          // DAT_142de0e30
uint32_t s_mouseMode;               // DAT_142de0e34
int32_t* s_loadingScreen;           // DAT_142de0bf8

// What the loading screen is doing: true while it holds the game.
// ref: FUN_14022c010
bool LoadingScreenIsActive() {
    if (s_loadingScreen == nullptr) {
        return false;
    }
    if (*s_loadingScreen > 0) {
        return true;
    }
    FROST_UNIMPLEMENTED();  // the player's transport check (FUN_14170ff20 and on)
    return false;
}

// An OS event into the client: input goes to the UI's target, and close and quit stop the
// pump.
// ref: FUN_140236bc0
void ClientHandleEvent(void* context, uint32_t id, uint32_t* params, void* param) {
    (void)context;
    auto quit = static_cast<uint8_t*>(param);
    uint32_t time = params[3];
    switch (id) {
        case 0:
            ClientOnMouseCapture(params[1], params[2]);
            break;
        case 1:
            if (s_eventsPaused == 0) {
                if (s_swallowChar == 0) {
                    if (s_eventTarget != nullptr) {
                        UiInputChar(s_eventTarget, params[0] & 0x3fffffff);
                    }
                } else {
                    s_swallowChar = 0;
                }
            }
            break;
        case 2:
            if (s_eventsPaused == 0 && s_eventTarget != nullptr) {
                UiInputIme(s_eventTarget, params);
            }
            break;
        case 3:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x1b:
            break;
        case 4:
        case 0x14:
            *quit = 1;
            break;
        case 5:
            ClientOnFocus(params[0]);
            break;
        case 6: {
            uint32_t key = params[0] & 0x3fffffff;
            if (key < 6) {
                uint32_t bit = 1u << key;
                if ((s_heldModifiers & bit) != 0) {
                    break;
                }
                s_heldModifiers |= bit;
            }
            struct {
                uint32_t m_key;
                uint32_t m_time;
                uint8_t m_repeat;
                uint8_t m_system;
                uint16_t m_pad;
                uint32_t m_virtualKey;
                float m_scan;
            } event = { key, time, static_cast<uint8_t>((params[0] >> 30) & 1), static_cast<uint8_t>(params[0] >> 31), 0,
                params[1], static_cast<float>(params[2]) };
            char handled = 0;
            ClientKeyBinding(key, 1, &handled);
            if (handled == 0) {
                if (s_eventsPaused == 0) {
                    if (!ConsoleIsToggleKey(key)) {
                        if (s_eventTarget != nullptr) {
                            UiInputKeyDown(s_eventTarget, &event);
                        }
                    } else {
                        OsWindowFocus(OsGetMainWindow(0));
                        s_swallowChar = 1;
                    }
                }
            } else if (s_eventsPaused == 0 && s_eventTarget != nullptr) {
                UiInputKeyDownBound(s_eventTarget, &event);
            }
            break;
        }
        case 7:
            ClientOnKeyUp(params, time);
            break;
        case 8:
        case 9:
        case 10:
            if (s_eventsPaused == 0 && Sub14152CF90(0) != 0 && s_eventTarget != nullptr) {
                FROST_UNIMPLEMENTED();  // gamepad button and stick events (FUN_1405effd0 and its two siblings)
            }
            break;
        case 0xb:
        case 0x10:
        case 0xc:
        case 0x11:
        case 0xd:
        case 0x12:
            if (id == 0xb || id == 0x10) {
                s_heldButtons |= params[0];
                s_pressedButtons |= params[0];
            }
            if (s_eventsPaused == 0) {
                if ((id == 0xb || id == 0x10) && params[0] == 1) {
                    GxSetMouseCursorActive(1);
                }
                FROST_UNIMPLEMENTED();  // mouse events to the UI (normalized by the client rect, FUN_14123ab20)
            }
            break;
        case 0xe:
            if (s_eventsPaused == 0 && s_eventTarget != nullptr) {
                FROST_UNIMPLEMENTED();  // relative mouse motion to the UI (FUN_1405f1860)
            }
            break;
        case 0xf:
        case 0x13:
            ClientOnMouseUp(params[0], params[1], params[2], 0, time, id == 0x13);
            break;
        case 0x19:
            if (s_eventTarget != nullptr) {
                UiInputDisplayChanged();
            }
            break;
        case 0x1a:
            OsGetAsyncTimeMs();
            Sub14065FB00();
            break;
        case 0x1c:
            GxSetMouseCursorActive(0);
            break;
    }
}

// Runs the queued OS events into the client; false once something asked to quit.
// ref: FUN_1402379d0
bool ClientProcessEvents() {
    if (s_quitRequested != 0) {
        return false;
    }
    if (LoadingScreenIsActive()) {
        s_paused = 1;
    }
    s_eventTarget = s_uiInput;
    s_eventsPaused = s_paused;
    if (s_uiInput != nullptr) {
        FROST_UNIMPLEMENTED();  // FUN_1405ede10: the UI input's frame start
    }
    // The deferred event handlers (DAT_142de0e00) run here when the events are not paused;
    // nothing adds to them yet.
    uint8_t quit = 0;
    OsEventPump(ClientHandleEvent, nullptr, &quit);
    s_eventTarget = nullptr;
    return quit == 0;
}

}

// Asks the client to quit: sets the flag ClientProcessEvents stops on and, the first time,
// lets the window and the connection go and drains the pump once more.
// ref: FUN_140216680
void ClientRequestQuit() {
    s_quitRequested = 1;
    if (s_quitHandled == 0) {
        FROST_UNIMPLEMENTED();  // FUN_1414f8d90 (window), FUN_1412d4870(0xfe, 1) and FUN_1412cab50 (the connection)
        Sub1412CAB50(s_quitArgument);
        uint8_t ignored;
        OsEventPump([](void*, uint32_t, uint32_t*, void*) {}, nullptr, &ignored);
        s_quitHandled = 1;
    }
}

// The main loop's condition: pumps events and advances the game; false to stop.
// ref: FUN_1402225d0
bool Sub1402225D0() {
    GxFrameSetPhase(0);
    GxFrameLatencyWait();
    GxFrameWorkFlush(1);
    if (GxFrameLimited(s_gameState == 0 ? 0x3c : 0)) {
        GxFrameWorkFlush(2);
    }
    uint32_t now = OsGetAsyncTimeMs();
    if (now < g_startupValue && g_startupValue - now < 0x7fffffff) {
        now = g_startupValue;
    }
    uint32_t elapsed = now - g_startupValue;
    s_frameSeconds = static_cast<float>(elapsed) * 0.001f;  // DAT_142618f58
    g_startupValue = now;
    Sub14065FCE0(elapsed, now);
    s_frameTime = now;
    if (!ClientProcessEvents()) {
        return false;
    }
    Sub14065FD40();
    AsyncReadPoll();
    Sub14135A290();
    Sub140222A60();
    Sub1402CF8D0();
    Sub14021F090();
    Sub1415642A0();
    Sub14230B040();
    if (s_pendingReset != 0) {
        Sub1412E1C00();
        s_pendingReset = 0;
    }
    Sub1412CAB50(Sub140216570());
    Sub1412E1FB0();
    Sub1402F4D00(Sub140216570());
    Sub14021C040();
    Sub14228C670();
    Sub1414F8E20();
    // The scheduled callbacks (DAT_142cc8060, with additions at DAT_142cc8300 and removals at
    // DAT_142cc8460) run here; nothing schedules any yet.
    Sub141350FD0(s_gameState == 7);
    Sub14192FCB0(Sub14192FA70());
    Sub1413528E0(s_gameState != 7 || LoadingScreenIsActive());
    switch (s_gameState) {
        case 0:
            s_paused = 0;
            Sub1412EE600();
            Sub1412EA4A0();
            Sub1412E8790();
            Sub14167C520();
            Sub141687ED0();
            Sub141691B00();
            Sub1405F0BE0(s_uiInput, s_frameSeconds);
            Sub141525060(nullptr, s_frameSeconds);  // &DAT_142d082d0
            break;
        case 1:
            s_gameState = 2;
            break;
        case 2:
            Sub140234DF0();
            break;
        case 3:
            Sub140234ED0();
            break;
        case 4:
            Sub140235040();
            break;
        case 5:
            Sub140234080();
            break;
        case 7:
            FROST_UNIMPLEMENTED();  // the world's update (FUN_1418e6b00 and on)
            Sub14228CB00();
            break;
    }
    if (s_pendingFlush != 0) {
        Sub140570630();
        s_pendingFlush = 0;
    }
    return true;
}

// One frame: the world and the UI drawn into the window's swap chain.
// ref: FUN_140225ab0
void Sub140225AB0() {
    GxFrameSetPhase(1);
    FROST_UNIMPLEMENTED();  // the frame (needs the device and its swap chain)
}
