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

// The window procedure the engine's windows hand their messages to (input to events, the
// frame dragged by hand, power hooks), and its nesting entry.
intptr_t OsWindowProc(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam);
intptr_t OsWindowProcNested(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam);
void OsSetWindowResizeLock(uint32_t locked);
void OsSetMainWindowGetter(void* (*getter)());
void* OsGetMainWindow(uint32_t a1);

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
void OsCursorFrameBegin(uint32_t time);
void OsCursorFrameEnd(uint32_t time);

#endif
