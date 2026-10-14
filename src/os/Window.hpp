#ifndef OS_WINDOW_HPP
#define OS_WINDOW_HPP

// Engine/Source/Os/W32: the window, its input, and the event queue the main loop drains.

#include <cstdint>

// One queued OS event (0x14 bytes): an id, three parameters, the time it was queued.
struct OSEVENT {
    uint32_t m_id;
    uint32_t m_param[3];
    uint32_t m_time;
};
static_assert(sizeof(OSEVENT) == 0x14, "OSEVENT is 0x14 bytes");

typedef void (*OSEVENTSINK)(void* context, uint32_t id, uint32_t* params, void* param);
typedef void (*OSPOWERHANDLER)();

void OsQueueEvent(uint32_t id, uint32_t param0, uint32_t param1, uint32_t param2);

void OsAddSuspendHandler(OSPOWERHANDLER handler);
void OsAddResumeHandler(OSPOWERHANDLER handler);
// Run on WM_POWERBROADCAST PBT_POWERSETTINGCHANGE.
void OsAddPowerSettingHandler(OSPOWERHANDLER handler);
void OsRemovePowerSettingHandler(OSPOWERHANDLER handler);
void OsRemoveSuspendHandler(OSPOWERHANDLER handler);
void OsRemoveResumeHandler(OSPOWERHANDLER handler);
void OsRunSuspendHandlers();
void OsRunResumeHandlers();

void OsInputInitialize();
// The mouse speed, the DPI entry points, and the input setup.
void OsWindowSystemInitialize();

// The mouse settings the client's CVars drive.
float OsGetMouseSpeed();                            // the custom sensitivity, 0.1 .. 2.0
void OsSetMouseSpeed(float speed);
void OsSetUseSystemMouseSpeed(bool useSystem);
void OsSetRawMouse(bool enable);
void OsSetRawMouseAcceleration(bool enable);
void OsSetRawMouseRate(uint32_t rate);
void OsSetRawMouseResolution(uint32_t resolution);
void OsSetMouseNoRepositioning(bool disable);

// The window procedure the engine's windows hand their messages to (input to events, the
// frame dragged by hand, power hooks), and its nesting entry.
intptr_t OsWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam);
intptr_t OsWindowProcNested(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam);
void OsSetWindowResizeLock(uint32_t locked);
void OsSetMainWindowGetter(void* (*getter)());
void* OsGetMainWindow(uint32_t a1);
// The main window's client rect as {top, left, bottom, right} (FUN_14123ab20).
bool OsGetClientRect(int32_t* rect);
// The cursor in client coordinates (FUN_14123b140).
void OsGetCursorPosition(int32_t* x, int32_t* y);
// A window's DPI over 96 (FUN_141237e80).
float OsGetWindowDpiScale(void* window);
// An engine key code's Windows virtual key (FUN_141239f50), and whether it is down now
// (FUN_14123aba0).
bool OsKeyToVirtualKey(uint32_t key, uint32_t* virtualKey);
bool OsIsKeyDown(uint32_t key);

// Pumps messages and drains the event queue into `handler` (context, id, params, param).
bool OsEventPump(OSEVENTSINK handler, void* context, void* param);

// The window rect around a client rect, as {top, left, bottom, right}, for the monitor's DPI.
void OsAdjustWindowRect(int32_t* rect, uint32_t style, uint32_t exStyle);

// The cursor module (os/win/Cursor.cpp).
uint64_t OsCursorTrackMessage(void* window, uint32_t message, uint64_t wparam, uint64_t lparam, uint32_t time);
uint64_t OsCursorGetPosition();
uint64_t OsCursorGetClientPosition(void* window);
void OsCursorSet(void* window, void* cursor, uint32_t time);
void OsCursorEnable(void* window, uint32_t time);
void OsCursorDisable(void* window, uint32_t time);
void OsCursorFrameBegin(uint32_t time);
uint32_t OsCursorFrameEnd(uint32_t time);
void OsCursorSetPosition(void* window, const uint64_t* position, uint32_t time);
void OsCursorHold(void* window, uint32_t time, bool noRecenter);
void OsCursorRelease(uint32_t time);

// The mouse held for relative motion (1) or free (0) (FUN_14123b3c0).
void OsSetMouseMode(uint32_t mode);

// The IME (os/win/Window.cpp): on while an edit box has the focus (FUN_14123e5f0); the
// composition's target clause and cursor (FUN_14123e7e0); its result and composition strings
// (FUN_14123e9e0, FUN_14123e9f0); the native input language, 0 roman (FUN_14123ea00); native
// conversion on or off (FUN_14123ecd0).
void OsImeEnable(bool enable);
bool OsImeGetCompositionRange(int32_t* start, int32_t* end, int32_t* cursor);
bool OsImeGetResultString(char* buffer, int32_t size);
bool OsImeGetCompositionString(char* buffer, int32_t size);
int32_t OsImeGetLanguage();
void OsImeSetNative(bool native);

// The clipboard's text as UTF-8, freed with OsClipboardFree (os/win/Clipboard.cpp).
char* OsClipboardGetString();
bool OsClipboardPutString(const char* text);
void OsClipboardFree(char* text);

#endif
