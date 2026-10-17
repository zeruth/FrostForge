#include "os/Window.hpp"
#include "os/Sync.hpp"
#include "os/Timer.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <imm.h>
#include <malloc.h>
#include <cmath>
#include <cstring>

namespace {

const char s_eventTag[] = ".?AUOSEVENT@@";
const char s_eventBlockTag[] = ".PEAUOSEVENT@@";
const char s_handlerTag[] = ".P6AXXZ";

// blz::deque<OSEVENT> (DAT_1430a1570): a map of blocks of eight events.
struct EVENTQUEUE {
    OSEVENT** m_map;                // 0x00
    uint64_t m_mapSize;             // 0x08: a power of two
    uint64_t m_offset;              // 0x10: the first event's index
    uint64_t m_size;                // 0x18
};
EVENTQUEUE s_events;

// A direct event sink that bypasses the queue while set and enabled.
OSEVENTSINK s_eventSink;            // DAT_1430a15d0
void* s_eventSinkContext;           // DAT_1430a15d8
void* s_eventSinkParam;             // DAT_1430a15e0
int32_t s_eventSinkMode;            // DAT_1430a15e8: 1 delivers through the sink
uint8_t s_eventSinkUsed;            // DAT_1430a15c9

// The power hooks (blz::vector<void (*)()>), run by the window procedure on
// WM_POWERBROADCAST: suspend (PBT_APMSUSPEND), resume (PBT_APMRESUMEAUTOMATIC).
struct HANDLERLIST {
    OSPOWERHANDLER* m_data;
    uint64_t m_size;
    uint64_t m_capacity;            // bit 63: the storage is not owned
};
HANDLERLIST s_suspendHandlers;      // PTR_DAT_142cfba58
HANDLERLIST s_resumeHandlers;       // PTR_DAT_142cfba90
HANDLERLIST s_powerSettingHandlers;  // PTR_DAT_142cfbac8: PBT_POWERSETTINGCHANGE
SRWLOCK s_handlerSrw = SRWLOCK_INIT;  // DAT_1430a1568

// Input and DPI state set at startup.
SHORT s_numLockAtStartup;           // DAT_1430a164c
UINT s_mouseSpeed;                  // DAT_1430a1628
UINT s_mouseSpeedCopy;              // DAT_1430a162c
float s_mouseSpeedScale;            // DAT_1430a1630
void* s_adjustWindowRectExForDpi;   // DAT_1430a15f0
void* s_getDpiForMonitor;           // DAT_1430a15f8
uint8_t s_rawMouse;                 // DAT_1430a1590
uint8_t s_mouseCurves;              // DAT_1430a1593
float s_mouseCurve[5][2];           // DAT_1430a15a0: {x, y} per point

const uint64_t NOT_OWNED = 0x8000000000000000ull;

// ref: FUN_1412397a0
void PushHandler(HANDLERLIST* list, OSPOWERHANDLER* handler) {
    uint64_t capacity = list->m_capacity;
    uint64_t size = list->m_size;
    uint64_t owned = capacity & ~NOT_OWNED;
    if (size == owned && owned < owned + 1) {
        uint64_t half = (capacity >> 1) & 0x3FFFFFFFFFFFFFFFull;
        uint64_t grow = owned + 1 <= owned + half ? half : 1;
        grow += owned;
        list->m_capacity = ((grow ^ capacity) & NOT_OWNED) ^ grow;
        auto data = static_cast<OSPOWERHANDLER*>(SMemAlloc(grow * 8, 0xB, s_handlerTag, SStrHashFnv1a(s_handlerTag), -6, 0));
        for (uint64_t i = 0; i < list->m_size; i++) {
            data[i] = list->m_data[i];
        }
        if ((list->m_capacity & NOT_OWNED) == 0 && list->m_data != nullptr) {
            SMemFree(list->m_data, s_handlerTag, -6, 0);
        }
        list->m_capacity &= ~NOT_OWNED;
        list->m_data = data;
        size = list->m_size;
    }
    OSPOWERHANDLER* slot = list->m_data + size;
    if (slot != nullptr) {
        *slot = *handler;
    }
    list->m_size++;
}

void RunHandlers(HANDLERLIST* list) {
    AcquireSRWLockExclusive(&s_handlerSrw);
    for (uint64_t i = 0; i < list->m_size; i++) {
        list->m_data[i]();
    }
    ReleaseSRWLockExclusive(&s_handlerSrw);
}

// A slot at the back of the queue: grows the block map when the back block is full and the
// map is used up, and allocates the block on first use.
// The event at a queue index (its block already exists).
OSEVENT* EventQueueSlot(EVENTQUEUE* queue, uint64_t index) {
    return queue->m_map[(queue->m_mapSize - 1) & (index >> 3)] + (index & 7);
}

// ref: FUN_14123cbc0
OSEVENT* EventQueuePushSlot(EVENTQUEUE* queue) {
    uint64_t offset = queue->m_offset;
    if (((queue->m_size + offset) & 7) == 0 && queue->m_mapSize <= (queue->m_size + 8) >> 3) {
        uint64_t mapSize = queue->m_mapSize;
        if (mapSize == 0) {
            mapSize = 8;
        } else {
            uint64_t want = mapSize + 1;
            while (mapSize < want) {
                mapSize *= 2;
            }
        }
        auto map = static_cast<OSEVENT**>(SMemAlloc(mapSize * 8, 0xB, s_eventBlockTag, SStrHashFnv1a(s_eventBlockTag), -6, 0));
        uint64_t first = (queue->m_offset >> 3) & (queue->m_mapSize - 1);
        if (queue->m_mapSize != 0) {
            std::memmove(map, queue->m_map + first, (queue->m_mapSize - first) * 8);
            std::memmove(map + (queue->m_mapSize - first), queue->m_map, first * 8);
        }
        std::memset(map + queue->m_mapSize, 0, (mapSize - queue->m_mapSize) * 8);
        SMemFree(queue->m_map, s_eventBlockTag, -6, 0);
        queue->m_offset &= 7;
        offset = queue->m_offset;
        queue->m_map = map;
        queue->m_mapSize = mapSize;
    }
    uint64_t index = queue->m_size + offset;
    queue->m_size++;
    OSEVENT** block = &queue->m_map[(queue->m_mapSize - 1) & (index >> 3)];
    if (*block == nullptr) {
        *block = static_cast<OSEVENT*>(SMemAlloc(8 * sizeof(OSEVENT), 0xB, s_eventTag, SStrHashFnv1a(s_eventTag), -6, 0));
    }
    return *block + (static_cast<uint32_t>(index) & 7);
}

}

// ref: FUN_14123af90
void OsQueueEvent(uint32_t id, uint32_t param0, uint32_t param1, uint32_t param2) {
    if (s_eventSink == nullptr || s_eventSinkMode != 1) {
        OSEVENT* event = EventQueuePushSlot(&s_events);
        if (event != nullptr) {
            event->m_time = 0;
        }
        event->m_id = id;
        event->m_param[0] = param0;
        event->m_param[1] = param1;
        event->m_param[2] = param2;
        event->m_time = OsGetAsyncTimeMs();
    } else {
        uint32_t params[4] = { param0, param1, param2, OsGetAsyncTimeMs() };
        s_eventSinkUsed = 1;
        s_eventSink(s_eventSinkContext, id, params, s_eventSinkParam);
    }
}

// ref: FUN_14123aa10
void OsAddSuspendHandler(OSPOWERHANDLER handler) {
    AcquireSRWLockExclusive(&s_handlerSrw);
    PushHandler(&s_suspendHandlers, &handler);
    ReleaseSRWLockExclusive(&s_handlerSrw);
}

// ref: FUN_14123a990
void OsAddResumeHandler(OSPOWERHANDLER handler) {
    AcquireSRWLockExclusive(&s_handlerSrw);
    PushHandler(&s_resumeHandlers, &handler);
    ReleaseSRWLockExclusive(&s_handlerSrw);
}

// ref: FUN_14123a9d0
void OsAddPowerSettingHandler(OSPOWERHANDLER handler) {
    AcquireSRWLockExclusive(&s_handlerSrw);
    PushHandler(&s_powerSettingHandlers, &handler);
    ReleaseSRWLockExclusive(&s_handlerSrw);
}

// Unordered removal: the last handler takes the removed one's place.
namespace {

// Unordered removal: the last handler takes the removed one's place.
void RemoveHandler(HANDLERLIST* list, OSPOWERHANDLER handler) {
    AcquireSRWLockExclusive(&s_handlerSrw);
    for (uint64_t i = 0; i < list->m_size; i++) {
        if (list->m_data[i] == handler) {
            list->m_data[i] = list->m_data[list->m_size - 1];
            list->m_size--;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_handlerSrw);
}

}

// ref: FUN_14123b6e0
void OsRemoveSuspendHandler(OSPOWERHANDLER handler) {
    RemoveHandler(&s_suspendHandlers, handler);
}

// ref: FUN_14123b5e0
void OsRemoveResumeHandler(OSPOWERHANDLER handler) {
    RemoveHandler(&s_resumeHandlers, handler);
}

// ref: FUN_14123b660
void OsRemovePowerSettingHandler(OSPOWERHANDLER handler) {
    AcquireSRWLockExclusive(&s_handlerSrw);
    for (uint64_t i = 0; i < s_powerSettingHandlers.m_size; i++) {
        if (s_powerSettingHandlers.m_data[i] == handler) {
            s_powerSettingHandlers.m_data[i] = s_powerSettingHandlers.m_data[s_powerSettingHandlers.m_size - 1];
            s_powerSettingHandlers.m_size--;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_handlerSrw);
}

void OsRunSuspendHandlers() {
    RunHandlers(&s_suspendHandlers);
}

void OsRunResumeHandlers() {
    RunHandlers(&s_resumeHandlers);
}

// Reads the mouse setup: raw mice (outside a remote session) and the system's acceleration
// curves.
// ref: FUN_14123c770
void OsInputInitialize() {
    if (GetSystemMetrics(SM_REMOTESESSION) != 0) {  // FUN_140267270
        s_rawMouse = 0;
        s_mouseCurves = 0;
        return;
    }
    int32_t mice = 0;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == 0 && count != 0) {
        auto devices = static_cast<RAWINPUTDEVICELIST*>(
            SMemAlloc(uint64_t(count) * sizeof(RAWINPUTDEVICELIST), 0x13, __FILE__, SStrHashFnv1a(__FILE__), 0x153, 0));
        if (GetRawInputDeviceList(devices, &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
            count = 0;
        }
        for (UINT i = 0; i < count; i++) {
            if (devices[i].dwType == RIM_TYPEMOUSE) {
                char name[0x110];
                name[0] = '\0';
                UINT size = 0x100;
                mice++;
                // The name went to a log call compiled empty (FUN_140255c60, "Raw mouse %u: %s").
                GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, name, &size);
            }
        }
        s_rawMouse = mice != 0;
        SMemFree(devices, __FILE__, 0x16b, 0);
    }
    HKEY key;
    LSTATUS status = RegOpenKeyExA(HKEY_CURRENT_USER, "Control Panel\\Mouse", 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS) {
        uint32_t points[10];
        DWORD type;
        DWORD size = 0x28;
        status = RegQueryValueExA(key, "SmoothMouseXCurve", nullptr, &type, reinterpret_cast<BYTE*>(points), &size);
        const float scale = 1.0f / 65536.0f;  // DAT_1426c6544
        if (status == ERROR_SUCCESS) {
            size = 0x28;
            for (int32_t i = 0; i < 5; i++) {
                s_mouseCurve[i][0] = static_cast<float>(points[i * 2]) * scale;
            }
            status = RegQueryValueExA(key, "SmoothMouseYCurve", nullptr, &type, reinterpret_cast<BYTE*>(points), &size);
            if (status == ERROR_SUCCESS) {
                for (int32_t i = 0; i < 5; i++) {
                    s_mouseCurve[i][1] = static_cast<float>(points[i * 2]) * scale;
                }
            }
        }
        RegCloseKey(key);
    }
    s_mouseCurves = status == ERROR_SUCCESS;
}

namespace {

OSONCE s_dpiOnce;                   // DAT_1430a1508
void* s_getDpiForMonitorOnce;       // DAT_1430a1520: shcore's GetDpiForMonitor, looked up once

// ref: FUN_1412381f0
void LoadGetDpiForMonitor() {
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
        s_getDpiForMonitorOnce = reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor"));
    }
}

void LoadGetDpiForMonitorInvoke(const void*) {
    LoadGetDpiForMonitor();
}

}

// A window's DPI over 96: its monitor's effective DPI, else the screen's LOGPIXELSY.
// ref: FUN_141237e80
float OsGetWindowDpiScale(void* window) {
    void* function[4] = { reinterpret_cast<void*>(&LoadGetDpiForMonitorInvoke), nullptr, nullptr, nullptr };
    OsCallOnce(&s_dpiOnce, function);
    if (window != nullptr && s_getDpiForMonitorOnce != nullptr) {
        HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTOPRIMARY);
        UINT dpiX = 0;
        UINT dpiY = 0;
        typedef HRESULT(WINAPI * GETDPIFORMONITOR)(HMONITOR, int, UINT*, UINT*);
        if (reinterpret_cast<GETDPIFORMONITOR>(s_getDpiForMonitorOnce)(monitor, 0, &dpiX, &dpiY) >= 0) {
            return static_cast<float>(dpiX) * (1.0f / 96.0f);  // DAT_14272426c
        }
    }
    HDC screen = GetDC(nullptr);
    int32_t dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    return static_cast<float>(static_cast<uint32_t>(dpi)) * (1.0f / 96.0f);
}

// ref: FUN_14123b190
void OsWindowSystemInitialize() {
    s_numLockAtStartup = GetAsyncKeyState(VK_NUMLOCK);
    if (!SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &s_mouseSpeed, 0)) {
        s_mouseSpeed = 10;
    }
    s_mouseSpeedScale = static_cast<float>(s_mouseSpeed) * 0.1f;  // DAT_14261a97c
    s_mouseSpeedCopy = s_mouseSpeed;
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32 != nullptr) {
        s_adjustWindowRectExForDpi = reinterpret_cast<void*>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore != nullptr) {
        s_getDpiForMonitor = reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor"));
    }
    OsInputInitialize();
}

namespace {

// The window procedure's state.
uint64_t s_cursorMotion;            // DAT_1430a1638: what the cursor module reported for the message
uint8_t s_imeOwnsNotify;            // DAT_1430a1aa1
HWND s_mouseWindow;                 // DAT_1430a1640
uint32_t s_mouseMode;               // DAT_1430a1648: 0 absolute, 1 relative (raw deltas)
uint8_t s_mouseReentered;           // DAT_1430a15cb
int32_t s_appActive = 1;            // DAT_142cfba54
uint8_t s_rawRegistered;            // DAT_1430a1592
uint8_t s_haveRawPosition;          // DAT_1430a1591
int32_t s_rawSkip;                  // DAT_1430a1594: absolute samples to drop after activation
int32_t s_rawSkipReset = 2;         // DAT_142cfbb00
float s_rawPosition[2];             // DAT_1430a1598
uint8_t s_mouseAccelerate = 1;      // DAT_142cfba48
float s_mouseAccelScale = 125.0f;   // DAT_142cfba4c
float s_mouseAccelBase = 400.0f;    // DAT_142cfba50
uint32_t s_capturedButtons;         // DAT_1430a1620
uint32_t s_releasingCapture;        // DAT_1430a1624
uint32_t s_extraButtons;            // DAT_1430a1688
TIMERPROC s_modalTimerProc;         // DAT_1430a1a90: ticks the engine inside a modal move/size loop
uint32_t s_moved;                   // DAT_1430a1608
uint64_t s_lastSize;                // DAT_1430a1600
uint32_t s_displayChanged;          // DAT_1430a160c
uint8_t s_imeDisabled;              // DAT_1430a1aa0
int32_t s_imeComposing;             // DAT_1430a1aa4
uint32_t s_rightShiftScan = 0x100;  // DAT_142cfbb04: 0x100 until read
uint8_t s_altKeepsMenu;             // DAT_1430a15cc
uint32_t s_restoreMouseSpeed;       // DAT_1430a1634
uint32_t s_resizeLock;              // DAT_1430a1560: windowResizeLock
uint32_t s_resumeCount;             // DAT_1430a15ec

// The keyboard layout (DAT_142cfbb08): whether it is an IME, and its class.
struct KEYBOARDLAYOUT {
    uint8_t m_ime;
    uint32_t m_kind;
};
KEYBOARDLAYOUT s_keyboardLayout = { 0, 1 };

// Dragging the window by hand (the frame is drawn by the engine): the hit-test code the
// button went down on, the cursor there, and the client rect then.
uint32_t s_dragActive;              // DAT_1430a1658
uint64_t s_dragHit;                 // DAT_1430a1650
uint32_t s_dragCancelled;           // DAT_1430a165c
int32_t s_dragStart[2];             // DAT_1430a1660
RECT s_dragRect;                    // DAT_1430a1668

// The "PC_Latency_Stats_Ping" message (the static initializer FUN_1401e4c10).
UINT s_latencyPingMessage = RegisterClipboardFormatW(L"PC_Latency_Stats_Ping");  // DAT_1430a1564

// ref: FUN_14123cb00
void CALLBACK ModalTimerProc(HWND window, UINT message, UINT_PTR id, DWORD time) {
    s_modalTimerProc(window, message, id, time);
}

// The engine's key code for a virtual key: letters and digits as is, the rest from 0x100 up.
// ref: FUN_141239a90
bool KeyFromVirtualKey(uint32_t key, uint32_t* code) {
    if (key - 0x70 < 0xc) {
        *code = key + 0xb3;
        return true;
    }
    if (key - 0x30 < 10) {
        *code = key;
        return true;
    }
    switch (key) {
        case 0x08: *code = 0x111; return true;
        case 0x09: *code = 0x112; return true;
        case 0x0d: *code = 0x110; return true;
        case 0x13: *code = 0x120; return true;
        case 0x14: *code = 0x11d; return true;
        case 0x1b: *code = 0x10f; return true;
        case 0x20: *code = 0x20; return true;
        case 0x21: *code = 0x11b; return true;
        case 0x22: *code = 0x11c; return true;
        case 0x23: *code = 0x11a; return true;
        case 0x24: *code = 0x119; return true;
        case 0x25: *code = 0x113; return true;
        case 0x26: *code = 0x114; return true;
        case 0x27: *code = 0x115; return true;
        case 0x28: *code = 0x116; return true;
        case 0x2c: *code = 0x121; return true;
        case 0x2d: *code = 0x117; return true;
        case 0x2e: *code = 0x118; return true;
        case 0x5b: *code = 6; return true;
        case 0x5c: *code = 7; return true;
        case 0x60: *code = 0x100; return true;
        case 0x61: *code = 0x101; return true;
        case 0x62: *code = 0x102; return true;
        case 0x63: *code = 0x103; return true;
        case 0x64: *code = 0x104; return true;
        case 0x65: *code = 0x105; return true;
        case 0x66: *code = 0x106; return true;
        case 0x67: *code = 0x107; return true;
        case 0x68: *code = 0x108; return true;
        case 0x69: *code = 0x109; return true;
        case 0x6a: *code = 0x10c; return true;
        case 0x6b: *code = 0x10a; return true;
        case 0x6d: *code = 0x10b; return true;
        case 0x6e: *code = 0x10e; return true;
        case 0x6f: *code = 0x10d; return true;
        case 0x90: *code = 0x11e; return true;
        case 0x91: *code = 0x11f; return true;
        case 0xa0: *code = 0; return true;
        case 0xa1: *code = 1; return true;
        case 0xa2: *code = 2; return true;
        case 0xa3: *code = 3; return true;
        case 0xa4: *code = 4; return true;
        case 0xa5: *code = 5; return true;
        default: {
            uint16_t character = static_cast<uint16_t>(MapVirtualKeyA(key, MAPVK_VK_TO_CHAR));
            *code = character;
            return character != 0 && character < 0x100;
        }
    }
}

// Key down/up (WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_SYSKEYUP) as events 6 and 7, with left
// and right Shift, Ctrl and Alt told apart. True to let the message through.
// ref: FUN_14123a680
bool HandleKey(uint32_t key, uint64_t lparam, uint32_t message) {
    bool down = ((message - WM_KEYDOWN) & 0xfffffffb) == 0;
    uint32_t id = down ? 6 : 7;
    if (key == VK_SHIFT) {
        if (!down) {
            OsQueueEvent(7, 0, 0xa0, 0);
            OsQueueEvent(7, 1, 0xa1, 0);
            if (s_keyboardLayout.m_ime == 0 || s_imeComposing < 1) {
                return false;
            }
            OsQueueEvent(2, 0, 0, 0);
            return false;
        }
        if (s_rightShiftScan == 0x100) {
            s_rightShiftScan = MapVirtualKeyA(VK_RSHIFT, MAPVK_VK_TO_VSC);
        }
        key = (((static_cast<uint32_t>(lparam >> 16) & 0xff) == s_rightShiftScan) ? 1u : 0u) + 0xa0;
    } else {
        uint32_t extended = static_cast<uint32_t>(lparam >> 24) & 1;
        if (key == VK_CONTROL) {
            key = extended | 0xa2;
        } else if (key == VK_MENU) {
            key = extended | 0xa4;
        }
    }
    uint32_t code;
    if (!KeyFromVirtualKey(key, &code)) {
        return true;
    }
    uint32_t repeat = down && ((static_cast<uint16_t>(lparam >> 16) >> 14) & 1) != 0 ? 0x40000000u : 0u;
    uint32_t system = message - WM_SYSKEYDOWN < 2 ? 0x80000000u : 0u;
    OsQueueEvent(id, (code & 0x3fffffff) | repeat | system, key, static_cast<uint32_t>(lparam));
    if (code == 0x126 && (GetKeyState(VK_MENU) & 0xf000) != 0) {
        return s_altKeepsMenu == 0;
    }
    return false;
}

// The mouse button a message is about (1 left, 4 right, 2 middle, 8 and 0x10 the X buttons).
// ref: FUN_141239980
bool MouseButtonFromMessage(uint32_t message, uint64_t wparam, uint32_t* button) {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONUP:
        case WM_NCLBUTTONDBLCLK:
            *button = 1;
            return true;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONUP:
            *button = 4;
            return true;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_NCMBUTTONDOWN:
        case WM_NCMBUTTONUP:
            *button = 2;
            return true;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_NCXBUTTONDOWN:
        case WM_NCXBUTTONUP: {
            uint32_t which = static_cast<uint32_t>(wparam >> 16) & 0xffff;
            if (which == XBUTTON1) {
                *button = 8;
                return true;
            }
            if (which == XBUTTON2) {
                *button = 0x10;
                return true;
            }
            *button = 0;
            return false;
        }
        default:
            *button = 0;
            return false;
    }
}

// Activation: the system mouse speed is swapped for the session's while the client is active.
// ref: FUN_14123cb10
void SetAppActive(int32_t active) {
    if (s_appActive == active) {
        return;
    }
    s_capturedButtons = 0;
    s_rawSkip = s_rawSkipReset;
    s_haveRawPosition = 0;
    s_appActive = active;
    if (active != 0) {
        SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &s_mouseSpeed, 0);
    }
    if (s_restoreMouseSpeed == 0) {
        UINT speed = s_appActive == 0 ? s_mouseSpeed : s_mouseSpeedCopy;
        SystemParametersInfoA(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<uintptr_t>(speed)), 0);
    }
    OsQueueEvent(5, s_appActive != 0, 0, 0);
}

// The IME class of a keyboard layout, from its IME file name (FUN_14123d380), and what that
// means for the layout state (FUN_14123ee80).
// ref: FUN_14123d380
uint32_t KeyboardLayoutKind(HKL layout) {
    (void)layout;
    FROST_UNIMPLEMENTED();  // ImmGetIMEFileNameA + the SOGOUPY.IME table at 0x1427246c0
    return 0;
}

// ref: FUN_14123ee80
void KeyboardLayoutSet(KEYBOARDLAYOUT* state, uint32_t kind) {
    state->m_ime = kind == 1 || kind == 2 || kind == 3;
    state->m_kind = kind != 4;
}

// IME composition notifications (WM_IME_NOTIFY IMN_SETCONVERSIONMODE / IMN_SETOPENSTATUS).
// ref: FUN_14123d430
bool ImeHandleNotify(uint32_t lparam) {
    (void)lparam;
    FROST_UNIMPLEMENTED();
    return false;
}

// The window rect for a client rect, {top, left, bottom, right} as the engine keeps it, with
// the monitor's DPI where the system has it.
}

// ref: FUN_14123aa50
void OsAdjustWindowRect(int32_t* rect, uint32_t style, uint32_t exStyle) {
    RECT r;
    r.top = rect[0];
    r.left = rect[1];
    r.bottom = rect[2];
    r.right = rect[3];
    bool adjusted = false;
    if (s_adjustWindowRectExForDpi != nullptr) {
        HMONITOR monitor = MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY);
        UINT dpiX;
        UINT dpiY;
        typedef HRESULT(WINAPI * GETDPIFORMONITOR)(HMONITOR, int, UINT*, UINT*);
        if (SUCCEEDED(reinterpret_cast<GETDPIFORMONITOR>(s_getDpiForMonitor)(monitor, 0, &dpiX, &dpiY))) {
            typedef BOOL(WINAPI * ADJUSTFORDPI)(RECT*, DWORD, BOOL, DWORD, UINT);
            adjusted = reinterpret_cast<ADJUSTFORDPI>(s_adjustWindowRectExForDpi)(&r, style, FALSE, exStyle, dpiX) != 0;
        }
    }
    if (!adjusted) {
        AdjustWindowRectEx(&r, style, FALSE, exStyle);
    }
    rect[0] = r.top;
    rect[1] = r.left;
    rect[2] = r.bottom;
    rect[3] = r.right;
}

namespace {

// Moves the window by the cursor's travel since the button went down on the caption.
// ref: FUN_14123a5d0
void DragMove(HWND window, const int32_t* delta) {
    int32_t rect[4] = { s_dragRect.top + delta[1], s_dragRect.left + delta[0], s_dragRect.bottom + delta[1],
        s_dragRect.right + delta[0] };
    OsAdjustWindowRect(rect, GetWindowLongA(window, GWL_STYLE), GetWindowLongA(window, GWL_EXSTYLE));
    MoveWindow(window, rect[1], rect[0], rect[3] - rect[1], rect[2] - rect[0], TRUE);
}

// Moves or sizes the window while a caption or frame drag is under way.
// ref: FUN_14123a400
void DragUpdate(HWND window, uint64_t point) {
    if (s_dragActive == 0) {
        return;
    }
    int32_t delta[2] = { static_cast<int16_t>(point) - s_dragStart[0],
        static_cast<int16_t>(point >> 16) - s_dragStart[1] };
    if (s_dragHit == HTCAPTION) {
        DragMove(window, delta);
        return;
    }
    if (s_resizeLock != 0) {
        return;
    }
    int32_t rect[4] = { s_dragRect.top, s_dragRect.left, s_dragRect.bottom, s_dragRect.right };
    switch (s_dragHit) {
        case HTLEFT:
        case HTTOPLEFT:
        case HTBOTTOMLEFT:
            rect[1] = delta[0] + s_dragRect.left;
            break;
        case HTRIGHT:
        case HTTOPRIGHT:
        case HTBOTTOMRIGHT:
            rect[3] = delta[0] + s_dragRect.right;
            break;
    }
    switch (s_dragHit) {
        case HTTOP:
        case HTTOPLEFT:
        case HTTOPRIGHT:
            rect[0] = s_dragRect.top + delta[1];
            break;
        case HTBOTTOM:
        case HTBOTTOMLEFT:
        case HTBOTTOMRIGHT:
            rect[2] = s_dragRect.bottom + delta[1];
            break;
    }
    OsAdjustWindowRect(rect, GetWindowLongA(window, GWL_STYLE), GetWindowLongA(window, GWL_EXSTYLE));
    RECT sizing = { rect[1], rect[0], rect[3], rect[2] };
    if (SendMessageA(window, WM_SIZING, s_dragHit - 9, reinterpret_cast<LPARAM>(&sizing)) != 0) {
        MoveWindow(window, sizing.left, sizing.top, sizing.right - sizing.left, sizing.bottom - sizing.top, TRUE);
    }
}

// The button came up over the frame: finish what went down on the same spot.
// ref: FUN_14123a840
void NonClientButtonUp(HWND window, uint64_t hit, uint64_t point) {
    if (s_dragCancelled == 0) {
        if (s_dragHit == hit) {
            switch (hit) {
                case HTCAPTION:
                    if (s_dragActive == 0) {
                        s_dragCancelled = 0;
                        return;
                    } else {
                        int32_t delta[2] = { static_cast<int16_t>(point) - s_dragStart[0],
                            static_cast<int16_t>(point >> 16) - s_dragStart[1] };
                        DragMove(window, delta);
                    }
                    break;
                case HTMINBUTTON:
                    ShowWindow(window, SW_MINIMIZE);
                    break;
                case HTMAXBUTTON:
                    ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
                    break;
                case HTLEFT:
                case HTRIGHT:
                case HTTOP:
                case HTTOPLEFT:
                case HTTOPRIGHT:
                case HTBOTTOM:
                case HTBOTTOMLEFT:
                case HTBOTTOMRIGHT:
                    DragUpdate(window, point);
                    break;
                case HTCLOSE:
                    PostQuitMessage(0);
                    break;
            }
        }
        if (s_dragActive != 0) {
            ReleaseCapture();
            s_dragActive = 0;
            s_dragCancelled = 0;
            return;
        }
    }
    s_dragCancelled = 0;
}

// Raw mouse motion: relative deltas (or absolute positions turned into deltas), through the
// system's acceleration curve when the client asks for it. Event 0xe carries the two deltas
// as float bits.
void HandleRawInput(HRAWINPUT input) {
    RAWINPUT raw;
    UINT size = 0x30;
    if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
        return;
    }
    if ((raw.data.mouse.usFlags & MOUSE_ATTRIBUTES_CHANGED) != 0) {
        // FUN_140255c60("Mouse attributes changed\n"): a log call compiled empty
    }
    uint32_t moved = 0;
    // Relative motion goes out as the integer deltas; absolute positions and accelerated
    // motion as float bits.
    uint32_t bitsX = 0;
    uint32_t bitsY = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    bool absolute = (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
    if (!absolute) {
        bitsX = static_cast<uint32_t>(raw.data.mouse.lLastX);
        bitsY = static_cast<uint32_t>(raw.data.mouse.lLastY);
        if (bitsX == 0 && bitsY == 0) {
            return;
        }
    } else {
        bool virtualDesktop = (raw.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        int32_t width = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        int32_t height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
        const float scale = 1.0f / 65535.0f;  // DAT_14272454c
        float x = static_cast<float>(raw.data.mouse.lLastX) * scale * static_cast<float>(width);
        float y = static_cast<float>(raw.data.mouse.lLastY) * scale * static_cast<float>(height);
        if (s_haveRawPosition == 0) {
            if (s_rawSkip == 0) {
                s_haveRawPosition = 1;
                s_rawPosition[0] = x;
                s_rawPosition[1] = y;
                return;
            }
            moved = 0;
        } else {
            dx = x - s_rawPosition[0];
            dy = y - s_rawPosition[1];
            const float epsilon = 2.3841858e-07f;  // DAT_14261a954
            if (std::fabs(dx) < epsilon && std::fabs(dy) < epsilon) {
                return;
            }
            moved = 1;
            s_rawPosition[0] = x;
            s_rawPosition[1] = y;
        }
        std::memcpy(&bitsX, &dx, 4);
        std::memcpy(&bitsY, &dy, 4);
    }
    if (s_mouseMode != 1) {
        return;
    }
    if (!absolute && s_mouseAccelerate != 0 && s_mouseCurves != 0) {
        float rawX = static_cast<float>(raw.data.mouse.lLastX);
        float rawY = static_cast<float>(raw.data.mouse.lLastY);
        float speed = std::sqrt(rawY * rawY + rawX * rawX);
        float input = speed * s_mouseAccelScale / s_mouseAccelBase;
        int32_t segment = 1;
        while (segment < 4 && input > s_mouseCurve[segment][0]) {
            segment++;
        }
        float x0 = s_mouseCurve[segment - 1][0];
        float x1 = s_mouseCurve[segment][0];
        float clamped = x0;
        if (x0 <= input) {
            clamped = input <= x1 ? input : x1;
        }
        float y0 = s_mouseCurve[segment - 1][1];
        float y1 = s_mouseCurve[segment][1];
        float factor = (((clamped - x0) / (x1 - x0)) * (y1 - y0) + y0) * s_mouseSpeedScale * 96.0f  // DAT_142724550
            / s_mouseAccelScale / speed;
        moved = 1;
        dx = factor * rawX;
        dy = factor * rawY;
        std::memcpy(&bitsX, &dx, 4);
        std::memcpy(&bitsY, &dy, 4);
    }
    OsQueueEvent(0xe, moved, bitsX, bitsY);
}

}

// The game's main window, for the OS message boxes to parent to (DAT_1430a1518, read by
// 0x141237e40 and 0x141237f70).
void* (*s_mainWindowGetter)();

// ref: FUN_141238140
void OsSetMainWindowGetter(void* (*getter)()) {
    s_mainWindowGetter = getter;
}

// ref: FUN_14123b500
void OsSetWindowResizeLock(uint32_t locked) {
    s_resizeLock = locked;
}

// The engine's window procedure: input becomes events on the queue, the frame is dragged by
// hand, and the power hooks run.
// ref: FUN_14123b780
intptr_t OsWindowProc(void* hwnd, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    HWND window = static_cast<HWND>(hwnd);
    uint64_t param = static_cast<uint64_t>(lparam);
    s_cursorMotion = OsCursorTrackMessage(window, message, wparam, param, OsGetAsyncTimeMs());
    UINT_PTR timer = 0;
    int16_t x = static_cast<int16_t>(param);
    int16_t y = static_cast<int16_t>(param >> 16);
    if (s_imeOwnsNotify != 0 && message == WM_IME_NOTIFY) {
        if (wparam == 3 || wparam == 4 || wparam == 5) {
            return 0;
        }
    }
    switch (message) {
        case WM_DESTROY:
        case WM_NCRBUTTONDOWN:
            return 0;
        case WM_MOVE:
            s_moved = 1;
            break;
        case WM_SIZE:
            s_lastSize = param;
            break;
        case WM_ACTIVATE:
            SetAppActive(static_cast<int16_t>(wparam) != 0 && !IsIconic(window) ? 1 : 0);
            break;
        case WM_CLOSE:
            OsQueueEvent(4, 0, 0, 0);
            return 0;
        case WM_ACTIVATEAPP:
            SetAppActive(wparam != 0 && !IsIconic(window) ? 1 : 0);
            break;
        case WM_INPUTLANGCHANGE:
            if (s_imeDisabled != 0) {
                break;
            }
            KeyboardLayoutSet(&s_keyboardLayout, KeyboardLayoutKind(GetKeyboardLayout(0)));
            OsQueueEvent(2, 0, 0, 0);
            break;
        case WM_CONTEXTMENU:
        case WM_NCRBUTTONUP:
        case 0x313:  // the system menu from the taskbar
        modalLoop:
            if (s_modalTimerProc != nullptr) {
                timer = SetTimer(window, 0, 0x21, ModalTimerProc);
            }
            break;
        case WM_DISPLAYCHANGE:
            s_displayChanged = 1;
            s_lastSize = param;
            break;
        case WM_NCLBUTTONDOWN:
            s_dragCancelled = 0;
            switch (wparam) {
                case HTCAPTION:
                case HTLEFT:
                case HTRIGHT:
                case HTTOP:
                case HTTOPLEFT:
                case HTTOPRIGHT:
                case HTBOTTOM:
                case HTBOTTOMLEFT:
                case HTBOTTOMRIGHT: {
                    s_dragHit = wparam;
                    SetFocus(window);
                    WINDOWPLACEMENT placement = {};
                    placement.length = sizeof(placement);
                    if (GetWindowPlacement(window, &placement) && placement.showCmd != SW_MAXIMIZE) {
                        GetClientRect(window, &s_dragRect);
                        ClientToScreen(window, reinterpret_cast<POINT*>(&s_dragRect.left));
                        ClientToScreen(window, reinterpret_cast<POINT*>(&s_dragRect.right));
                        s_dragStart[0] = x;
                        s_dragStart[1] = y;
                        SetCapture(window);
                        s_dragActive = 1;
                        return 0;
                    }
                    s_dragActive = 0;
                    return 0;
                }
                default:
                    s_dragHit = wparam;
                    s_dragActive = 0;
                    s_dragCancelled = 0;
                    return 0;
            }
        case WM_NCLBUTTONUP:
            NonClientButtonUp(window, wparam, param);
            return 0;
        case WM_NCLBUTTONDBLCLK:
            s_dragCancelled = 1;
            if (s_dragActive != 0) {
                ReleaseCapture();
                s_dragActive = 0;
            }
            switch (wparam) {
                case HTCAPTION:
                case HTMAXBUTTON:
                    ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
                    return 0;
                case HTSYSMENU:
                case HTCLOSE:
                    PostQuitMessage(0);
                    return 0;
                case HTMINBUTTON:
                    ShowWindow(window, SW_MINIMIZE);
                    return 0;
                default:
                    return 0;
            }
        case WM_INPUT:
            if (s_rawMouse != 0 && s_rawRegistered != 0) {
                HandleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
            }
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            if (!HandleKey(static_cast<uint32_t>(wparam), param, message)) {
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam > 0x1f) {
                OsQueueEvent(1, ((static_cast<uint32_t>(param >> 16) & 0x4000) << 16) | (static_cast<uint32_t>(wparam) & 0x3fffffff),
                    static_cast<uint32_t>(wparam), static_cast<uint32_t>(param));
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (wparam > 0x1f) {
                uint32_t flags = (static_cast<uint32_t>(param >> 16) & 0xc000) | 0xffff8000u;
                OsQueueEvent(1, (flags << 16) | (static_cast<uint32_t>(wparam) & 0x3fffffff), static_cast<uint32_t>(wparam),
                    static_cast<uint32_t>(param));
                return 0;
            }
            break;
        case WM_IME_STARTCOMPOSITION:
            OsQueueEvent(2, 1, 0, 0);
            return 1;
        case WM_IME_ENDCOMPOSITION:
            OsQueueEvent(2, 6, 0, 0);
            return 1;
        case WM_IME_COMPOSITION: {
            uint32_t flags = static_cast<uint32_t>(param >> 7) & 1;
            if ((param & 0x8) != 0) {  // GCS_COMPSTR
                flags |= 2;
            }
            if ((param & 0x800) != 0) {  // GCS_RESULTSTR
                flags |= 4;
            }
            OsQueueEvent(2, 2, 0, flags);
            return 1;
        }
        case WM_SYSCOMMAND: {
            uint32_t command = static_cast<uint32_t>(wparam) & 0xfff0;
            if (command == SC_SIZE || command == SC_MOVE) {
                goto modalLoop;
            }
            if (command == SC_CLOSE) {
                OsQueueEvent(4, 0, 0, 0);
                return 0;
            }
            if (command == SC_KEYMENU) {
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            POINT point = { x, y };
            s_mouseWindow = window;
            ClientToScreen(window, &point);
            if (s_dragActive != 0) {
                DragUpdate(window, (static_cast<uint32_t>(static_cast<uint16_t>(point.y)) << 16)
                    | static_cast<uint16_t>(point.x));
                return 0;
            }
            if (s_mouseReentered != 0) {
                OsQueueEvent(5, s_appActive != 0, 0, 0);
                s_mouseReentered = 0;
            }
            if (s_mouseMode == 0) {
                uint64_t position = OsCursorGetClientPosition(window);
                OsQueueEvent(0xc, 0, static_cast<uint32_t>(position), static_cast<uint32_t>(position >> 32));
            } else {
                if (s_mouseMode != 1) {
                    __debugbreak();
                }
                if ((s_rawMouse == 0 || s_rawRegistered == 0) && s_cursorMotion != 0) {
                    OsQueueEvent(0xe, 0, static_cast<uint32_t>(s_cursorMotion), static_cast<uint32_t>(s_cursorMotion >> 32));
                }
            }
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            uint32_t button;
            if (!MouseButtonFromMessage(message, wparam, &button)) {
                break;
            }
            if (s_capturedButtons == 0) {
                SetCapture(window);
            }
            s_capturedButtons |= button;
            uint64_t position = OsCursorGetClientPosition(window);
            OsQueueEvent(0xb, button, static_cast<uint32_t>(position), static_cast<uint32_t>(position >> 32));
            return message - WM_XBUTTONDOWN < 2 || message - WM_NCXBUTTONDOWN < 2 ? 1 : 0;
        }
        case WM_LBUTTONUP:
            if (s_dragActive != 0) {
                POINT point = { x, y };
                ClientToScreen(window, &point);
                NonClientButtonUp(window, s_dragHit, (static_cast<uint32_t>(static_cast<uint16_t>(point.y)) << 16)
                    | static_cast<uint16_t>(point.x));
                return 0;
            }
            [[fallthrough]];
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            uint32_t button;
            if (!MouseButtonFromMessage(message, wparam, &button)) {
                break;
            }
            s_capturedButtons &= ~button;
            if (s_capturedButtons == 0) {
                s_releasingCapture = 1;
                ReleaseCapture();
                s_releasingCapture = 0;
            }
            uint64_t position = OsCursorGetClientPosition(window);
            OsQueueEvent(0xf, button, static_cast<uint32_t>(position), static_cast<uint32_t>(position >> 32));
            return message - WM_XBUTTONDOWN < 2 || message - WM_NCXBUTTONDOWN < 2 ? 1 : 0;
        }
        case WM_MOUSEWHEEL: {
            uint64_t position = OsCursorGetClientPosition(window);
            OsQueueEvent(0xd, static_cast<uint32_t>(static_cast<int16_t>(wparam >> 16)), static_cast<uint32_t>(position),
                static_cast<uint32_t>(position >> 32));
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (s_releasingCapture == 0) {
                s_capturedButtons = 0;
                s_mouseWindow = window;
                uint64_t position = OsCursorGetClientPosition(window);
                OsQueueEvent(0, 0, static_cast<uint32_t>(position), static_cast<uint32_t>(position >> 32));
                return 0;
            }
            break;
        case WM_POWERBROADCAST:
            if (wparam == PBT_APMRESUMEAUTOMATIC) {
                s_resumeCount++;
                AcquireSRWLockExclusive(&s_handlerSrw);
                for (uint64_t i = 0; i < s_resumeHandlers.m_size; i++) {
                    s_resumeHandlers.m_data[i]();
                }
            } else if (wparam == PBT_APMSUSPEND) {
                AcquireSRWLockExclusive(&s_handlerSrw);
                for (uint64_t i = 0; i < s_suspendHandlers.m_size; i++) {
                    s_suspendHandlers.m_data[i]();
                }
            } else if (wparam == PBT_POWERSETTINGCHANGE) {
                AcquireSRWLockExclusive(&s_handlerSrw);
                for (uint64_t i = 0; i < s_powerSettingHandlers.m_size; i++) {
                    s_powerSettingHandlers.m_data[i]();
                }
            } else {
                break;
            }
            ReleaseSRWLockExclusive(&s_handlerSrw);
            break;
        case WM_DROPFILES:
            OsQueueEvent(0x17, static_cast<uint32_t>(wparam), static_cast<uint32_t>(static_cast<int64_t>(wparam) >> 32), 0);
            return 0;
        case WM_IME_SETCONTEXT:
            OsQueueEvent(2, 0, 0, 0);
            lparam = 0;
            break;
        case WM_IME_NOTIFY:
            if (wparam == 3) {
                if (ImeHandleNotify(static_cast<uint32_t>(param))) {
                    OsQueueEvent(2, 4, 0, 0);
                    return 0;
                }
                break;
            }
            if (wparam == 4) {
                OsQueueEvent(2, 5, 0, 0);
                break;
            }
            if (wparam == 5) {
                if (ImeHandleNotify(static_cast<uint32_t>(param))) {
                    OsQueueEvent(2, 3, 0, 0);
                    return 0;
                }
                break;
            }
            if (wparam == 6) {
                OsQueueEvent(7, 0, 0xa0, 0);
                OsQueueEvent(7, 1, 0xa1, 0);
            } else if (wparam != 8) {
                break;
            }
            OsQueueEvent(2, 0, 0, 0);
            break;
        case WM_IME_CONTROL:
        case WM_IME_COMPOSITIONFULL:
        case WM_IME_SELECT:
        case WM_NCMOUSEMOVE:
            return 0;
        case WM_MOUSELEAVE: {
            s_mouseReentered = 1;
            POINT cursor;
            GetCursorPos(&cursor);
            RECT client;
            GetClientRect(window, &client);
            ClientToScreen(window, reinterpret_cast<POINT*>(&client.left));
            ClientToScreen(window, reinterpret_cast<POINT*>(&client.right));
            if (client.left < cursor.x && cursor.x < client.right && client.top < cursor.y && cursor.y < client.bottom) {
                s_capturedButtons = 0;
                s_rawSkip = s_rawSkipReset;
                s_haveRawPosition = 0;
                OsQueueEvent(5, s_appActive != 0, 0, 0);
            }
            break;
        }
        case WM_DPICHANGED:
            OsQueueEvent(0x19, 0, 0, 0);
            return 0;
        case 0x403: {
            // Extra mouse buttons from the input hook, as a mask in wparam
            uint32_t buttons = static_cast<uint32_t>(wparam) & 0xffffffe0;
            uint32_t changed = buttons ^ s_extraButtons;
            s_extraButtons = buttons;
            for (uint32_t bit = 5; bit < 0x20; bit++) {
                if (((changed >> bit) & 1) != 0) {
                    uint32_t button = 1u << bit;
                    uint64_t position = OsCursorGetClientPosition(window);
                    OsQueueEvent((buttons & button) != 0 ? 0xb : 0xf, button, static_cast<uint32_t>(position),
                        static_cast<uint32_t>(position >> 32));
                    if (s_capturedButtons == 0) {
                        SetCapture(window);
                    } else {
                        ReleaseCapture();
                    }
                    s_capturedButtons |= button;
                }
            }
            return 1;
        }
    }
    if (message == s_latencyPingMessage) {
        OsQueueEvent(0x1c, 0, 0, 0);
        return 1;
    }
    LRESULT result = DefWindowProcW(window, message, wparam, lparam);
    if (timer != 0) {
        KillTimer(window, 0);
    }
    return result;
}

// The client window procedure's way into OsWindowProc, counting the depth (the event sink
// delivers directly while it is above zero).
// ref: FUN_14123b760
intptr_t OsWindowProcNested(void* window, uint32_t message, uintptr_t wparam, intptr_t lparam) {
    s_eventSinkMode++;
    LRESULT result = OsWindowProc(window, message, wparam, lparam);
    s_eventSinkMode--;
    return result;
}

namespace {

uint8_t s_pumping;                  // DAT_1430a15ca
void (*s_beforePump)();             // DAT_1430a1680
RECT s_clientRect;                  // DAT_1430a1610: the main window's client rect, cached

}

// The main window, through the getter the client registered.
// The main window through the getter the client registered (0), the active window (1) or the
// foreground one (2).
// ref: FUN_141237f70
void* OsGetMainWindow(uint32_t a1) {
    if (a1 == 0) {
        return s_mainWindowGetter != nullptr ? s_mainWindowGetter() : nullptr;
    }
    if (a1 == 1) {
        return GetActiveWindow();
    }
    if (a1 == 2) {
        return GetForegroundWindow();
    }
    return nullptr;
}

// The main window's client rect as {top, left, bottom, right}, from the cache once it is known.
// ref: FUN_14123ab20
bool OsGetClientRect(int32_t* rect) {
    if (s_clientRect.right == 0 || s_clientRect.bottom == 0) {
        if (!GetClientRect(static_cast<HWND>(OsGetMainWindow(0)), &s_clientRect)) {
            return false;
        }
    }
    rect[0] = s_clientRect.top;
    rect[1] = s_clientRect.left;
    rect[2] = s_clientRect.bottom;
    rect[3] = s_clientRect.right;
    return true;
}

// The cursor in the main window's client coordinates.
// ref: FUN_14123b140
void OsGetCursorPosition(int32_t* x, int32_t* y) {
    uint64_t position = OsCursorGetClientPosition(OsGetMainWindow(0));
    if (x != nullptr) {
        *x = static_cast<int32_t>(position);
    }
    if (y != nullptr) {
        *y = static_cast<int32_t>(position >> 0x20);
    }
}

// The Windows virtual key of an engine key code: the modifiers (left and right shift, control,
// alt, then the two Windows keys), the digits and space as they are, the keypad, the named keys
// and F1..F24; anything else is a character, through the keyboard layout.
// ref: FUN_141239f50
bool OsKeyToVirtualKey(uint32_t key, uint32_t* virtualKey) {
    int32_t k = static_cast<int32_t>(key);
    if (k < 0x123) {
        if (k < 0x30) {
            if (k < 0x21) {
                if (key == 0x20) {
                    *virtualKey = VK_SPACE;
                    return true;
                }
                switch (key) {
                    case 0: *virtualKey = VK_LSHIFT; return true;
                    case 1: *virtualKey = VK_RSHIFT; return true;
                    case 2: *virtualKey = VK_LCONTROL; return true;
                    case 3: *virtualKey = VK_RCONTROL; return true;
                    case 4: *virtualKey = VK_LMENU; return true;
                    case 5: *virtualKey = VK_RMENU; return true;
                    case 6: *virtualKey = VK_LWIN; return true;
                    case 7: *virtualKey = VK_RWIN; return true;
                }
                goto scan;
            }
        } else if (k < 0x3a) {
            *virtualKey = key;  // the digits
            return true;
        }
    } else if (k < 0x12f) {
        *virtualKey = key - 0xb3;  // F1..F12
        return true;
    }
    switch (key) {
        case 0x100: *virtualKey = VK_NUMPAD0; return true;
        case 0x101: *virtualKey = VK_NUMPAD1; return true;
        case 0x102: *virtualKey = VK_NUMPAD2; return true;
        case 0x103: *virtualKey = VK_NUMPAD3; return true;
        case 0x104: *virtualKey = VK_NUMPAD4; return true;
        case 0x105: *virtualKey = VK_NUMPAD5; return true;
        case 0x106: *virtualKey = VK_NUMPAD6; return true;
        case 0x107: *virtualKey = VK_NUMPAD7; return true;
        case 0x108: *virtualKey = VK_NUMPAD8; return true;
        case 0x109: *virtualKey = VK_NUMPAD9; return true;
        case 0x10a: *virtualKey = VK_ADD; return true;
        case 0x10b: *virtualKey = VK_SUBTRACT; return true;
        case 0x10c: *virtualKey = VK_MULTIPLY; return true;
        case 0x10d: *virtualKey = VK_DIVIDE; return true;
        case 0x10e: *virtualKey = VK_DECIMAL; return true;
        case 0x10f: *virtualKey = VK_ESCAPE; return true;
        case 0x110: *virtualKey = VK_RETURN; return true;
        case 0x111: *virtualKey = VK_BACK; return true;
        case 0x112: *virtualKey = VK_TAB; return true;
        case 0x113: *virtualKey = VK_LEFT; return true;
        case 0x114: *virtualKey = VK_UP; return true;
        case 0x115: *virtualKey = VK_RIGHT; return true;
        case 0x116: *virtualKey = VK_DOWN; return true;
        case 0x117: *virtualKey = VK_INSERT; return true;
        case 0x118: *virtualKey = VK_DELETE; return true;
        case 0x119: *virtualKey = VK_HOME; return true;
        case 0x11a: *virtualKey = VK_END; return true;
        case 0x11b: *virtualKey = VK_PRIOR; return true;
        case 0x11c: *virtualKey = VK_NEXT; return true;
        case 0x11d: *virtualKey = VK_CAPITAL; return true;
        case 0x11e: *virtualKey = VK_NUMLOCK; return true;
        case 0x11f: *virtualKey = VK_SCROLL; return true;
        case 0x120: *virtualKey = VK_PAUSE; return true;
        case 0x121: *virtualKey = VK_SNAPSHOT; return true;
    }
scan:
    uint8_t scanned = static_cast<uint8_t>(VkKeyScanA(static_cast<char>(key)));
    *virtualKey = scanned;
    return scanned != 0;
}

// Whether an engine key is down on the keyboard now (not as the event queue has it).
// ref: FUN_14123aba0
bool OsIsKeyDown(uint32_t key) {
    uint32_t virtualKey[6];
    if (OsKeyToVirtualKey(key, virtualKey)) {
        if (GetAsyncKeyState(static_cast<int32_t>(virtualKey[0])) < 0) {
            return true;
        }
    }
    return false;
}

// Pumps the thread's messages, handing every queued event to `handler` (directly, while the
// pump runs) and turning size, move and display changes into events. WM_QUIT ends the pump with
// event 0x14. Returns whether the handler was called directly.
// ref: FUN_14123acf0
bool OsEventPump(OSEVENTSINK handler, void* context, void* param) {
    if (s_pumping != 0) {
        return false;
    }
    s_eventSinkUsed = 0;
    s_pumping = 1;
    s_eventSink = handler;
    s_eventSinkContext = context;
    s_eventSinkParam = param;
    OsCursorFrameBegin(OsGetAsyncTimeMs());
    if (s_beforePump != nullptr) {
        s_beforePump();
    }
    for (;;) {
        for (; s_events.m_size != 0; s_events.m_size--) {
            OSEVENT* event = EventQueueSlot(&s_events, s_events.m_offset);
            handler(context, event->m_id, event->m_param, param);
            s_events.m_offset++;
        }
        MSG message;
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            break;
        }
        if (message.message == WM_QUIT) {
            OsQueueEvent(0x14, 0, 0, 0);
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (s_lastSize != 0) {
            HWND window = static_cast<HWND>(OsGetMainWindow(0));
            if (!IsIconic(window)) {
                s_lastSize = 0;
                RECT client;
                GetClientRect(static_cast<HWND>(OsGetMainWindow(0)), &client);
                if (client.right - client.left != s_clientRect.right || client.bottom - client.top != s_clientRect.bottom) {
                    s_clientRect.left = 0;
                    s_clientRect.top = 0;
                    s_clientRect.right = client.right - client.left;
                    s_clientRect.bottom = client.bottom - client.top;
                    OsQueueEvent(3, 0, 0, 0);
                }
            }
        }
        if (s_moved != 0) {
            OsQueueEvent(0x1b, 0, 0, 0);
            s_moved = 0;
        }
        if (s_displayChanged != 0) {
            OsQueueEvent(0x19, 0, 0, 0);
            s_displayChanged = 0;
        }
    }
    OsCursorFrameEnd(OsGetAsyncTimeMs());
    if (s_rawSkip > 0) {
        s_rawSkip--;
    }
    s_eventSink = nullptr;
    s_eventSinkContext = nullptr;
    s_eventSinkParam = nullptr;
    s_pumping = 0;
    return s_eventSinkUsed != 0;
}

// ---- the IME ---------------------------------------------------------------------------------

namespace {

HIMC s_imeContext;                  // DAT_1430a1be8: the window's input context while the IME is off
void* s_tsfThreadManager;           // DAT_1430a1ab8: the TSF thread manager (made by FUN_14123f100)

// A composition string of the current IME context, as UTF-8 (FUN_14123d2c0).
bool ImeGetString(uint32_t index, char* buffer, int32_t size) {
    *buffer = '\0';
    HWND window = static_cast<HWND>(OsGetMainWindow(0));
    HIMC context = ImmGetContext(window);
    if (context != nullptr) {
        uint16_t wide[0x200];
        LONG bytes = ImmGetCompositionStringW(context, index, wide, 0x3fe);
        wide[(bytes & ~1) / 2] = 0;
        int32_t written = 0;
        UnicodeConvertUtf16ToUtf8(buffer, static_cast<uint32_t>(size - 1), wide, 0x200, &written, nullptr);
        buffer[written] = '\0';
        ImmReleaseContext(window, context);
    }
    return context != nullptr;
}

}

// The IME follows the edit focus: the first enable gives the window its input context back,
// the last disable cancels the composition and takes it away.
// ref: FUN_14123e5f0
void OsImeEnable(bool enable) {
    HWND window = static_cast<HWND>(OsGetMainWindow(0));
    if (!enable) {
        s_imeComposing--;
        if (s_imeComposing != 0) {
            return;
        }
        ImmNotifyIME(s_imeContext, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmAssociateContext(window, nullptr);
    } else {
        s_imeComposing++;
        if (s_imeComposing != 1) {
            return;
        }
        ImmAssociateContext(window, s_imeContext);
    }
    if (s_imeDisabled == 0 || s_tsfThreadManager == nullptr) {
        return;
    }
    // The TSF input processor profile sink, advised while the IME is on.
    FROST_UNIMPLEMENTED();
}

// The composition's target clause and cursor, in characters; false without a composition.
// ref: FUN_14123e7e0
bool OsImeGetCompositionRange(int32_t* start, int32_t* end, int32_t* cursor) {
    HWND window = static_cast<HWND>(OsGetMainWindow(0));
    HIMC context = ImmGetContext(window);
    if (context == nullptr) {
        return false;
    }
    uint32_t cursorPos = static_cast<uint16_t>(ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0));
    uint32_t clauseBytes = static_cast<uint32_t>(ImmGetCompositionStringW(context, GCS_COMPCLAUSE, nullptr, 0));
    if (clauseBytes < 5) {
        ImmReleaseContext(window, context);
        return false;
    }
    uint32_t* clauses = static_cast<uint32_t*>(_alloca((clauseBytes + 0xf) & ~0xfull));
    std::memset(clauses, 0, clauseBytes);
    uint32_t read = static_cast<uint32_t>(ImmGetCompositionStringW(context, GCS_COMPCLAUSE, clauses, clauseBytes));
    if (0xfffffffd < read) {
        ImmReleaseContext(window, context);
        return false;
    }
    uint32_t count = read >> 2;
    clauses[0] = 0;
    uint8_t attributes[0x200];
    uint32_t target = 0;
    if (count < 3) {
        ImmGetCompositionStringW(context, GCS_COMPATTR, attributes, 0x200);
        if (count == 1) {
            goto done;
        }
    } else {
        uint32_t current = 0;
        for (uint32_t i = 0; i < count - 1; i++) {
            if (clauses[i] <= cursorPos && cursorPos < clauses[i + 1]) {
                current = i;
            }
            target = current;
        }
        ImmGetCompositionStringW(context, GCS_COMPATTR, attributes, 0x200);
    }
    for (uint32_t i = 0; i < count - 1; i++) {
        if (attributes[clauses[i]] != 0) {
            target = i;
        }
    }
done:
    uint16_t composition[0x200] = {};
    ImmGetCompositionStringW(context, GCS_COMPSTR, composition, 0x3fe);
    ImmReleaseContext(window, context);
    *start = static_cast<int32_t>(clauses[target]);
    *end = static_cast<int32_t>(clauses[target + 1]);
    *cursor = static_cast<int32_t>(cursorPos);
    return true;
}

// ref: FUN_14123e9e0
bool OsImeGetResultString(char* buffer, int32_t size) {
    return ImeGetString(GCS_RESULTSTR, buffer, size);
}

// ref: FUN_14123e9f0
bool OsImeGetCompositionString(char* buffer, int32_t size) {
    return ImeGetString(GCS_COMPSTR, buffer, size);
}

// The input language while the IME converts natively: 1 Japanese, 2 Korean, 3 Chinese,
// otherwise 0 (roman).
// ref: FUN_14123ea00
int32_t OsImeGetLanguage() {
    bool native = false;
    if (s_imeDisabled == 0) {
        HWND window = static_cast<HWND>(OsGetMainWindow(0));
        HIMC context = ImmGetContext(window);
        if (context == nullptr) {
            return 0;
        }
        DWORD conversion = 0;
        DWORD sentence = 0;
        ImmGetConversionStatus(context, &conversion, &sentence);
        ImmReleaseContext(window, context);
        native = (conversion & IME_CMODE_NATIVE) != 0;
    } else if (s_tsfThreadManager != nullptr) {
        // The TSF compartments (keyboard open and input mode conversion).
        FROST_UNIMPLEMENTED();
    }
    if (!native) {
        return 0;
    }
    uint8_t language = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(GetKeyboardLayout(0)));
    if (language == LANG_CHINESE) {
        return 3;
    }
    if (language == LANG_JAPANESE) {
        return 1;
    }
    if (language == LANG_KOREAN) {
        return 2;
    }
    return 0;
}

// Native conversion on or off.
// ref: FUN_14123ecd0
void OsImeSetNative(bool native) {
    HWND window = static_cast<HWND>(OsGetMainWindow(0));
    HIMC context = ImmGetContext(window);
    if (context == nullptr) {
        return;
    }
    DWORD conversion = 0;
    DWORD sentence = 0;
    ImmGetConversionStatus(context, &conversion, &sentence);
    conversion = native ? (conversion | IME_CMODE_NATIVE) : (conversion & ~static_cast<DWORD>(IME_CMODE_NATIVE));
    ImmSetConversionStatus(context, conversion, sentence);
}

// ---- the mouse settings the client's CVars drive -------------------------------------------

namespace {

uint8_t s_mouseNoRepositioning;     // DAT_1430a15c8

}

// The mouse held in the main window for relative motion (any mode but 0), re-centered unless
// repositioning is off, or let go.
// ref: FUN_14123b3c0
void OsSetMouseMode(uint32_t mode) {
    uint8_t noRecenter = s_mouseNoRepositioning;
    if (mode == s_mouseMode) {
        return;
    }
    if (mode != 0) {
        uint32_t time = OsGetAsyncTimeMs();
        OsCursorHold(OsGetMainWindow(0), time, noRecenter != 0);
        s_mouseMode = mode;
        return;
    }
    OsCursorRelease(OsGetAsyncTimeMs());
    s_mouseMode = 0;
}

// ref: FUN_14123b180
float OsGetMouseSpeed() {
    return s_mouseSpeedScale;
}

// The custom sensitivity, clamped to 0.1 .. 2.0; it reaches the system speed while the game
// is active and not using the system's own.
// ref: FUN_14123b4a0
void OsSetMouseSpeed(float speed) {
    s_mouseSpeedScale = 0.1f;
    if (0.1f <= speed) {
        s_mouseSpeedScale = speed;
        if (2.0f <= speed) {
            s_mouseSpeedScale = 2.0f;
        }
    }
    s_mouseSpeedCopy = static_cast<UINT>(static_cast<int32_t>(s_mouseSpeedScale * 10.0f));
    if (s_appActive != 0 && s_restoreMouseSpeed == 0) {
        SystemParametersInfoA(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<int32_t>(s_mouseSpeedCopy))), 0);
    }
}

// ref: FUN_14123b520
void OsSetUseSystemMouseSpeed(bool useSystem) {
    uint32_t value = useSystem ? 1 : 0;
    if (value == s_restoreMouseSpeed) {
        return;
    }
    int32_t speed = useSystem ? static_cast<int32_t>(s_mouseSpeed) : static_cast<int32_t>(s_mouseSpeedCopy);
    s_restoreMouseSpeed = value;
    SystemParametersInfoA(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(speed)), 0);
}

// Raw input on or off; the mouse is registered again (RIDEV_NOLEGACY off) while it is up.
// ref: FUN_14123b300
void OsSetRawMouse(bool enable) {
    s_rawRegistered = enable;
    if (s_rawMouse != 0) {
        s_rawSkip = s_rawSkipReset;
        s_haveRawPosition = 0;
        RAWINPUTDEVICE device = {};
        device.usUsagePage = 1;
        device.usUsage = 2;
        device.dwFlags = static_cast<DWORD>(enable ? 0 : 1);
        device.hwndTarget = nullptr;
        RegisterRawInputDevices(&device, 1, sizeof(device));
    }
}

// ref: FUN_14123b360
void OsSetRawMouseAcceleration(bool enable) {
    s_mouseAccelerate = enable;
}

// ref: FUN_14123b390
void OsSetRawMouseRate(uint32_t rate) {
    s_mouseAccelScale = static_cast<float>(rate);
}

// ref: FUN_14123b370
void OsSetRawMouseResolution(uint32_t resolution) {
    s_mouseAccelBase = static_cast<float>(resolution);
}

// ref: FUN_14123b2f0
void OsSetMouseNoRepositioning(bool disable) {
    s_mouseNoRepositioning = disable;
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a1594 = (void*)&s_rawSkip;
extern "C" void* const gen_p_1430a1640 = (void*)&s_mouseWindow;
extern "C" void* const gen_p_1430a1648 = (void*)&s_mouseMode;
extern "C" void* const gen_p_1430a1680 = (void*)&s_beforePump;
// ---- end of generated globals ----
