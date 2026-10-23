#include "client/GxWindow.hpp"
#include "client/ClientCVars.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxDevice.hpp"
#include "os/Os.hpp"
#include "os/Timer.hpp"
#include "os/Window.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "storm/Unicode.hpp"
#include "util/Unimplemented.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <malloc.h>

extern "C" int GenFault(unsigned long long address, EXCEPTION_POINTERS* info);

namespace {

// The window as last applied (DAT_142d08178) and what sizing works from.
GXWINDOWPARAMS s_params = { { 0, 0 }, { 0, 0 }, 0, 1, 1, 1, 1, 1, { 0, 0, 0 }, 0 };
float s_aspect = 1.0f;              // DAT_142d0815c: width / height
uint32_t s_aspectLocked;            // DAT_1430b0040: gxAspect
int32_t s_minClient[2] = { 320, 240 };  // DAT_142d08170
int32_t s_frameExtra[2];            // DAT_1430b0010: the frame's width and height around the client

HWND s_window;                      // DAT_1430b0030
ATOM s_class;                       // DAT_1430b0028
void* s_swapChain;                  // DAT_1430b0020: the device's swap chain for the window
HPOWERNOTIFY s_powerNotify;         // DAT_1430b0038
uint8_t s_clipCursor;               // DAT_1430b001c: ClipCursor
uint8_t s_updating;                 // DAT_1430b001d
uint8_t s_clipSuspended;            // DAT_1430b001e: the cursor left while clipping
uint8_t s_visible;                  // DAT_1430d009c: not minimized

// The once-only lookup of EnableNonClientDpiScaling (DAT_1430b0000: a lock, then "done").
SRWLOCK s_dpiOnceLock = SRWLOCK_INIT;  // DAT_1430b0000
uint8_t s_dpiOnceDone;
BOOL(WINAPI* s_enableNonClientDpiScaling)(HWND);  // DAT_1430b0048

// The hardware cursor the engine draws into (two 128x128 BGRA images and the hot spot).
uint16_t s_cursorHotspot[2];        // DAT_1430b0050
uint32_t s_cursorSizeIndex;         // DAT_1430d0060
uint32_t s_cursorPixels[0x4000];    // DAT_1430b0060
uint32_t s_cursorShown[0x4000];     // DAT_1430c0060
HCURSOR s_cursor;                   // DAT_1430d0268
uint8_t s_cursorWasDisabled = 1;    // DAT_142d08258 (1 in the image: the hardware cursor shows once the CVar is read)
uint8_t s_cursorEnable = 1;         // DAT_142d08259
uint8_t s_cursorPending = 1;        // DAT_142d08288

// Callbacks run when the window is made, resized and destroyed (blz::vector<pair<fn, param>>).
struct WINDOWCALLBACK {
    void (*m_function)(void* window, void* param);
    void* m_param;
};
struct WINDOWCALLBACKLIST {
    WINDOWCALLBACK* m_data;
    uint64_t m_size;
    uint64_t m_capacity;
};
WINDOWCALLBACKLIST s_createCallbacks;   // DAT_1430d01a0
WINDOWCALLBACKLIST s_resizeCallbacks;   // DAT_1430d01d0
WINDOWCALLBACKLIST s_destroyCallbacks;  // DAT_1430d01f8

const uint32_t WINDOWED_STYLE = 0x06cf0000;  // WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN
const uint32_t FULLSCREEN_STYLE = 0x90000000;  // WS_POPUP | WS_VISIBLE

// ref: FUN_1414fd030
void WindowSetVisible(uint8_t visible) {
    s_visible = visible;
}

// ref: FUN_1414fceb0
void WindowRunCreateCallbacks(HWND window) {
    for (uint64_t i = 0; i < s_createCallbacks.m_size; i++) {
        s_createCallbacks.m_data[i].m_function(window, s_createCallbacks.m_data[i].m_param);
    }
}

// ref: FUN_1414fcf10
void WindowRunDestroyCallbacks(HWND window) {
    for (uint64_t i = 0; i < s_destroyCallbacks.m_size; i++) {
        s_destroyCallbacks.m_data[i].m_function(window, s_destroyCallbacks.m_data[i].m_param);
    }
}

// ref: FUN_1414fcf70
void WindowRunResizeCallbacks() {
    for (uint64_t i = 0; i < s_resizeCallbacks.m_size; i++) {
        reinterpret_cast<void (*)(void*)>(s_resizeCallbacks.m_data[i].m_function)(s_resizeCallbacks.m_data[i].m_param);
    }
    g_pendingRenderSettings.m_resolution = s_params.m_resolution;
}

const char s_callbackTag[] = ".?AU?$pair@P6AXPEAX@ZPEAX@blz@@";

}  // namespace

// A resize callback added at the end of the list (grown by half again, at least one).
// ref: FUN_1414fca90
void WindowAddResizeCallback(void (*function)(void*), void* param) {
    uint64_t capacity = s_resizeCallbacks.m_capacity & 0x7fffffffffffffffull;
    if (s_resizeCallbacks.m_size == capacity && capacity < capacity + 1) {
        uint64_t half = (s_resizeCallbacks.m_capacity >> 1) & 0x3fffffffffffffffull;
        uint64_t grow = 1;
        if (capacity + 1 <= half + capacity) {
            grow = half;
        }
        s_resizeCallbacks.m_capacity =
            ((grow + capacity) & 0x7fffffffffffffffull) | (s_resizeCallbacks.m_capacity & 0x8000000000000000ull);
        auto data = static_cast<WINDOWCALLBACK*>(
            SMemAlloc((grow + capacity) * sizeof(WINDOWCALLBACK), 0xb, s_callbackTag, SStrHashFnv1a(s_callbackTag), -6, 0));
        WINDOWCALLBACK* out = data;
        for (WINDOWCALLBACK* in = s_resizeCallbacks.m_data; in != s_resizeCallbacks.m_data + s_resizeCallbacks.m_size; in++) {
            *out++ = *in;
        }
        if ((s_resizeCallbacks.m_capacity & 0x8000000000000000ull) == 0) {
            SMemFree(s_resizeCallbacks.m_data, s_callbackTag, -6, 0);
        }
        s_resizeCallbacks.m_capacity &= 0x7fffffffffffffffull;
        s_resizeCallbacks.m_data = data;
    }
    WINDOWCALLBACK* slot = s_resizeCallbacks.m_data + s_resizeCallbacks.m_size;
    if (slot != nullptr) {
        slot->m_function = reinterpret_cast<void (*)(void*, void*)>(function);
        slot->m_param = param;
    }
    s_resizeCallbacks.m_size++;
}

// The first resize callback with this function and param taken out, the rest moved down.
// ref: FUN_1414fcfc0
void WindowRemoveResizeCallback(void (*function)(void*), void* param) {
    WINDOWCALLBACK* end = s_resizeCallbacks.m_data + s_resizeCallbacks.m_size;
    WINDOWCALLBACK* entry = s_resizeCallbacks.m_data;
    if (entry == end) {
        return;
    }
    while (entry->m_function != reinterpret_cast<void (*)(void*, void*)>(function) || entry->m_param != param) {
        entry++;
        if (entry == end) {
            return;
        }
    }
    while (++entry != end) {
        entry[-1] = *entry;
    }
    s_resizeCallbacks.m_size--;
}

namespace {

// ref: FUN_1414f89a0
void WindowClipCursor(bool clip) {
    if (!clip) {
        ClipCursor(nullptr);
        return;
    }
    RECT client;
    GetClientRect(s_window, &client);
    POINT origin = { client.left, client.top };
    ClientToScreen(s_window, &origin);
    RECT screen = { origin.x, origin.y, origin.x + client.right, client.bottom + origin.y };
    ClipCursor(&screen);
}

// Fits a windowed client rect ({left, top, right, bottom}) onto the monitor's work area,
// keeping the aspect ratio when one is given, and centers it.
// ref: FUN_1414f8a20
bool WindowFitToMonitor(uint32_t monitorIndex, float aspect, int32_t* rect, uint32_t style) {
    MONITORINFO info;
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoA(static_cast<HMONITOR>(GxMonitorGet(monitorIndex)), &info)) {
        return false;
    }
    int32_t bottom = rect[3];
    int32_t top = rect[1];
    int32_t right = rect[2];
    int32_t left = rect[0];
    int32_t frame[4] = { info.rcWork.top, info.rcWork.left, info.rcWork.bottom, info.rcWork.right };
    int32_t height = bottom - top;
    OsAdjustWindowRect(frame, style, 0);
    const float tolerance = 0.01f;  // DAT_142636334
    int32_t frameLeft = info.rcWork.left - frame[1];
    int32_t frameTop = info.rcWork.top - frame[0];
    int32_t frameRight = frame[3] - info.rcWork.right;
    int32_t frameBottom = frame[2] - info.rcWork.bottom;
    int32_t frameWidth = frameRight + frameLeft;
    int32_t frameHeight = frameBottom + frameTop;
    int32_t newRight = frameWidth + info.rcWork.left + (right - left);
    rect[0] = info.rcWork.left;
    int32_t newBottom = (bottom - top) + info.rcWork.top + frameHeight;
    rect[1] = info.rcWork.top;
    rect[2] = newRight;
    rect[3] = newBottom;
    int32_t x = info.rcWork.left;
    int32_t y = info.rcWork.top;
    if (info.rcWork.right < newRight) {
        rect[2] = info.rcWork.right;
        newRight = info.rcWork.right;
        if (aspect != 0.0f) {
            int32_t width = info.rcWork.right - info.rcWork.left;
            float rows = 0.0f;
            bool found = false;
            for (int32_t w = width - frameWidth; (right - left) / 2 <= w; w--) {
                float h = static_cast<float>(w) / aspect;
                if (h - std::floor(h) < tolerance) {
                    width = w + frameWidth;
                    rows = std::floor(static_cast<float>(w) / aspect);
                    found = true;
                    break;
                }
            }
            if (!found) {
                rows = std::floor(static_cast<float>(width - frameWidth) / aspect + 0.5f);
            }
            newRight = width + rect[0];
            rect[2] = newRight;
            newBottom = rect[1] + static_cast<int32_t>(rows) + frameHeight;
            x = rect[0];
            y = rect[1];
        }
    }
    if (info.rcWork.bottom < newBottom) {
        rect[3] = info.rcWork.bottom;
        newBottom = info.rcWork.bottom;
        if (aspect != 0.0f) {
            int32_t heightSpan = info.rcWork.bottom - y;
            float columns = 0.0f;  // (set by the search or after it)
            bool found = false;
            for (int32_t h = heightSpan - frameHeight; height / 2 <= h; h--) {
                float w = static_cast<float>(h) * aspect;
                if (w - std::floor(w) < tolerance) {
                    heightSpan = h + frameHeight;
                    columns = static_cast<float>(h) * aspect;
                    found = true;
                    break;
                }
            }
            if (!found) {
                columns = static_cast<float>(heightSpan - frameHeight) * aspect + 0.5f;
            }
            float width = std::floor(columns);
            x = rect[0];
            y = rect[1];
            newBottom = heightSpan + y;
            newRight = x + static_cast<int32_t>(width) + frameWidth;
        }
    }
    int32_t shiftX = ((x - newRight) + (info.rcWork.right - info.rcWork.left)) / 2;
    rect[2] = (shiftX + newRight) - frameRight;
    int32_t shiftY = ((y - newBottom) + (info.rcWork.bottom - info.rcWork.top)) / 2;
    rect[0] = frameLeft + shiftX + x;
    rect[1] = frameTop + shiftY + y;
    rect[3] = (shiftY + newBottom) - frameBottom;
    return true;
}

// The window's position and size for the parameters: the monitor's whole area for a
// fullscreen window, else the requested client size fitted onto the work area, then grown by
// the frame.
// ref: FUN_1414f90e0
void WindowComputeRect(uint32_t style, GXWINDOWPARAMS* params, int32_t* position) {
    float aspect = 0.0f;
    if (params->m_aspect != 0 && params->m_size.m_width != 0 && params->m_size.m_height != 0) {
        aspect = static_cast<float>(params->m_size.m_width) / static_cast<float>(params->m_size.m_height);
    }
    auto monitor = static_cast<HMONITOR>(GxMonitorGet(params->m_monitor));
    if (monitor == nullptr) {
        if (params->m_maximize != 0) {
            position[0] = 0;
            position[1] = 0;
            params->m_size.m_width = static_cast<uint16_t>(GetSystemMetrics(SM_CXSCREEN));
            params->m_size.m_height = static_cast<uint16_t>(GetSystemMetrics(SM_CYSCREEN));
        }
    } else {
        uint16_t width = params->m_size.m_width;
        uint16_t height = params->m_size.m_height;
        int32_t left = 0;
        int32_t top = 0;
        MONITORINFO info;
        info.cbSize = sizeof(info);
        if (GetMonitorInfoA(monitor, &info)) {
            left = info.rcWork.left;
            top = info.rcWork.top;
            width = static_cast<uint16_t>(info.rcWork.right);
            height = static_cast<uint16_t>(info.rcWork.bottom);
            if (params->m_maximize != 0) {
                left = info.rcMonitor.left;
                top = info.rcMonitor.top;
                width = static_cast<uint16_t>(info.rcMonitor.right);
                height = static_cast<uint16_t>(info.rcMonitor.bottom);
            }
        }
        if (params->m_maximize == 0) {
            position[0] += left;
            position[1] += top;
        } else {
            position[1] = top;
            position[0] = left;
            params->m_size.m_width = static_cast<uint16_t>(width - left);
            params->m_size.m_height = static_cast<uint16_t>(height - top);
        }
    }
    if (params->m_maximize == 0) {
        int32_t rect[4] = { position[0], position[1], params->m_size.m_width + position[0],
            params->m_size.m_height + position[1] };
        if (WindowFitToMonitor(params->m_monitor, aspect, rect, style)) {
            position[0] = rect[0];
            position[1] = rect[1];
            params->m_size.m_width = static_cast<uint16_t>(rect[2] - rect[0]);
            params->m_size.m_height = static_cast<uint16_t>(rect[3] - rect[1]);
        }
    }
    uint16_t width = params->m_size.m_width;
    uint16_t height = params->m_size.m_height;
    s_aspectLocked = params->m_aspect;
    s_aspect = static_cast<float>(width) / static_cast<float>(height);
    if (params->m_maximize == 0 && width != 0 && height != 0) {
        int32_t rect[4] = { position[1], position[0], position[1] + height, position[0] + width };
        OsAdjustWindowRect(rect, style, 0);
        position[0] = rect[1];
        position[1] = rect[0];
        params->m_size.m_width = static_cast<uint16_t>(rect[3] - rect[1]);
        params->m_size.m_height = static_cast<uint16_t>(rect[2] - rect[0]);
    }
}

// The cursor image's scalers, from the size it was drawn at to the size the cursor is shown at
// (the 5x5 table PTR_FUN_142d08190, called by FUN_1414fc8b0): reached only through the table,
// so named here for the translator to bring in.
// translated: FUN_1414f9d60
// translated: FUN_1414f9d90
// translated: FUN_1414fa300
// translated: FUN_1414fa3a0
// translated: FUN_1414fa470
// translated: FUN_1414fa540
// translated: FUN_1414fa620
// translated: FUN_1414faf40
// translated: FUN_1414fafe0
// translated: FUN_1414fb080
// translated: FUN_1414fb150
// translated: FUN_1414fb180
// translated: FUN_1414fb6f0
// translated: FUN_1414fb720
// translated: FUN_1414fc050

}  // namespace

// The hardware cursor's size (an index into 32, 64, 64, 96, 128), for the next reset.
// ref: FUN_1414fc880
void WindowSetCursorSize(int32_t index) {
    if (static_cast<uint32_t>(index) != s_cursorSizeIndex) {
        s_cursorSizeIndex = static_cast<uint32_t>(index);
    }
}

namespace {

// The engine's hardware cursor: the current image as a cursor of the chosen size.
// ref: FUN_1414fd2f0
void WindowResetCursor() {
    std::memset(&s_cursorPixels[1], 0, 0xfffc);
    std::memset(&s_cursorShown[1], 0, 0xfffc);
    uint32_t hotspotX = s_cursorHotspot[0];
    uint32_t hotspotY = s_cursorHotspot[1];
    static const int32_t s_sizes[] = { 0x20, 0x40, 0x40, 0x60, 0x80 };
    int32_t size = s_sizes[s_cursorSizeIndex];
    s_cursorShown[0] = 0x1000000;
    s_cursorPixels[0] = 0x1000000;
    s_cursorPending = 1;
    if (s_cursor != nullptr) {
        DestroyCursor(s_cursor);
    }
    s_cursor = nullptr;
    HWND window = static_cast<HWND>(WindowGetHandle());
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 0x20;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HDC dc = GetDC(window);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(window, dc);
    if (color != nullptr && bits != nullptr) {
        std::memcpy(bits, s_cursorShown, static_cast<size_t>(static_cast<uint32_t>(size * size)) * 4);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO icon = {};
        icon.fIcon = FALSE;
        icon.xHotspot = hotspotX;
        icon.yHotspot = hotspotY;
        icon.hbmMask = mask;
        icon.hbmColor = color;
        s_cursor = CreateIconIndirect(&icon);
        DeleteObject(mask);
        DeleteObject(color);
        OsCursorSet(window, s_cursor, OsGetAsyncTimeMs());
    }
    if (s_cursorEnable != 0 && s_cursorPending != 0) {
        OsCursorEnable(WindowGetHandle(), OsGetAsyncTimeMs());
        s_cursorPending = 0;
    }
}

// ref: FUN_1414fd550
void WindowDestroyCursor() {
    if (s_cursor != nullptr) {
        DestroyCursor(s_cursor);
        s_cursor = nullptr;
    }
}

}  // namespace

// The shown image (scaled to the chosen size) as the window's cursor: a top-down 32-bit
// BGRA bitmap with its alpha, the hot spot where the image says. (The translation drew a
// quarter of it: by hand, as WindowResetCursor already builds one.)
// ref: FUN_1414fd100
bool WindowUpdateCursor() {
    static const int32_t s_sizes[] = { 0x20, 0x40, 0x40, 0x60, 0x80 };
    uint32_t hotspotX = s_cursorHotspot[0];
    uint32_t hotspotY = s_cursorHotspot[1];
    int32_t size = s_sizes[s_cursorSizeIndex];
    if (s_cursor != nullptr) {
        DestroyCursor(s_cursor);
    }
    s_cursor = nullptr;
    HWND window = static_cast<HWND>(WindowGetHandle());
    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 0x20;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HDC dc = GetDC(window);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(window, dc);
    if (color != nullptr && bits != nullptr) {
        std::memcpy(bits, s_cursorShown, static_cast<size_t>(static_cast<uint32_t>(size * size)) * 4);
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO icon = {};
        icon.fIcon = FALSE;
        icon.xHotspot = hotspotX;
        icon.yHotspot = hotspotY;
        icon.hbmMask = mask;
        icon.hbmColor = color;
        s_cursor = CreateIconIndirect(&icon);
        DeleteObject(mask);
        DeleteObject(color);
        OsCursorSet(window, s_cursor, OsGetAsyncTimeMs());
    }
    return s_cursor != nullptr;
}

namespace {

// The device's objects for the window (the software cursor texture and its material).
extern "C" unsigned long long FUN_1414fc320();
// translated: FUN_1414fc320
void WindowCreateDeviceObjects() {
    __try {
        FUN_1414fc320();
    } __except (GenFault(0x1414fc320ull, GetExceptionInformation())) {
    }
}

extern "C" unsigned long long FUN_1414fc3e0();
// translated: FUN_1414fc3e0
void WindowReleaseDeviceObjects() {
    __try {
        FUN_1414fc3e0();
    } __except (GenFault(0x1414fc3e0ull, GetExceptionInformation())) {
    }
}

// A device came up: give the window a swap chain.
// ref: FUN_1414f8db0
void WindowOnDeviceCreated(void* param) {
    (void)param;
    GXSWAPCHAINPARAMS params;
    params.m_window = s_window;
    params.m_width = s_params.m_resolution.m_width;
    params.m_height = s_params.m_resolution.m_height;
    params.m_vsync = s_params.m_vsync;
    params.m_format = 0;
    params.m_primary = 1;
    s_swapChain = GxSwapChainCreate(&params);
    WindowCreateDeviceObjects();
}

// ref: FUN_1414f8e00
void WindowOnDeviceDestroyed(void* param) {
    (void)param;
    WindowReleaseDeviceObjects();
    GxSwapChainDestroy(&s_swapChain);
}

// ref: FUN_1414f9d20
void LoadNonClientDpiScaling(void* storage) {
    (void)storage;
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32 != nullptr) {
        s_enableNonClientDpiScaling = reinterpret_cast<BOOL(WINAPI*)(HWND)>(GetProcAddress(user32, "EnableNonClientDpiScaling"));
    }
}

// ref: FUN_1414f9570
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    int16_t x = static_cast<int16_t>(lparam);
    int16_t y = static_cast<int16_t>(static_cast<uint64_t>(lparam) >> 16);
    switch (message) {
        case WM_MOVE:
            if (s_clipCursor != 0 && s_clipSuspended == 0) {
                WindowClipCursor(true);
            }
            break;
        case WM_SIZE:
            if (s_updating != 0) {
                break;
            }
            if (wparam == SIZE_MINIMIZED || wparam == SIZE_MAXHIDE) {
                WindowSetVisible(0);
                break;
            }
            {
                uint16_t width = static_cast<uint16_t>(x);
                uint16_t height = static_cast<uint16_t>(y);
                if (width == 0) {
                    width = 1;
                }
                if (height == 0) {
                    height = 1;
                }
                WindowSetVisible(1);
                if (s_params.m_size.m_width != width || s_params.m_size.m_height != height) {
                    if (s_params.m_sizeMatches != 0) {
                        s_params.m_resolution.m_width = width;
                        s_params.m_resolution.m_height = height;
                    }
                    GXWINDOWPARAMS params = s_params;
                    params.m_size.m_width = width;
                    params.m_size.m_height = height;
                    s_params.m_size.m_width = width;
                    s_params.m_size.m_height = height;
                    WindowApplyParams(&params, false);
                }
                SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOOWNERZORDER);
            }
            break;
        case WM_ACTIVATE:
            s_clipSuspended = 0;
            if (s_clipCursor != 0 && static_cast<int16_t>(wparam) != 0) {
                POINT cursor;
                GetCursorPos(&cursor);
                ScreenToClient(window, &cursor);
                RECT client;
                GetClientRect(window, &client);
                if (PtInRect(&client, cursor)) {
                    WindowClipCursor(true);
                    break;
                }
                s_clipSuspended = 1;
            }
            WindowClipCursor(false);
            break;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 0;
        case WM_DISPLAYCHANGE:
            if (s_updating == 0) {
                GxMonitorsReset();
                if (GxDevicePresent()) {
                    GxNotifyOnDisplayChange(s_params.m_monitor);
                }
            }
            break;
        case WM_NCCREATE:
            // FUN_1423631d0: the once-only lookup
            AcquireSRWLockExclusive(&s_dpiOnceLock);
            if (s_dpiOnceDone == 0) {
                LoadNonClientDpiScaling(nullptr);
                s_dpiOnceDone = 1;
            }
            ReleaseSRWLockExclusive(&s_dpiOnceLock);
            if (s_enableNonClientDpiScaling != nullptr) {
                s_enableNonClientDpiScaling(window);
            }
            break;
        case WM_SYSCOMMAND:
            if (wparam == SC_SCREENSAVE || wparam == SC_MONITORPOWER) {
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (s_clipCursor != 0 && s_clipSuspended != 0 && GetAsyncKeyState(VK_LBUTTON) >= 0
                && GetAsyncKeyState(VK_RBUTTON) >= 0) {
                POINT point = { x, y };
                RECT client;
                GetClientRect(window, &client);
                if (PtInRect(&client, point)) {
                    WindowClipCursor(true);
                    s_clipSuspended = 0;
                }
            }
            break;
        case WM_SIZING: {
            auto rect = reinterpret_cast<RECT*>(lparam);
            int32_t top = rect->top;
            int32_t left = rect->left;
            int32_t bottom = rect->bottom;
            int32_t right = rect->right;
            int32_t frame[4] = { top, left, bottom, right };
            OsAdjustWindowRect(frame, GetWindowLongA(window, GWL_STYLE), GetWindowLongA(window, GWL_EXSTYLE));
            s_frameExtra[0] = ((left - right) - frame[1]) + frame[3];
            s_frameExtra[1] = ((top - bottom) - frame[0]) + frame[2];
            if (right - left < 1 || bottom - top < 1) {
                return 0;
            }
            const float half = 0.5f;  // DAT_14261a988
            int32_t span;
            switch (wparam) {
                case WMSZ_LEFT:
                    span = right - s_frameExtra[0];
                    if (span - left < s_minClient[0]) {
                        left = span - s_minClient[0];
                    }
                    if (s_aspectLocked != 0) {
                        top = bottom - static_cast<int32_t>(static_cast<float>(span - left) / s_aspect + half
                            + static_cast<float>(s_frameExtra[1]));
                    }
                    break;
                case WMSZ_RIGHT:
                    if ((right - s_frameExtra[0]) - left < s_minClient[0]) {
                        right = left + s_frameExtra[0] + s_minClient[0];
                    }
                    if (s_aspectLocked != 0) {
                        span = (right - left) - s_frameExtra[0];
                        bottom = static_cast<int32_t>(static_cast<float>(span) / s_aspect + half
                            + static_cast<float>(s_frameExtra[1])) + top;
                    }
                    break;
                case WMSZ_TOP:
                    span = bottom - s_frameExtra[1];
                    if (span - top < s_minClient[1]) {
                        top = span - s_minClient[1];
                    }
                    if (s_aspectLocked != 0) {
                        left = right - static_cast<int32_t>(static_cast<float>(span - top) * s_aspect + half
                            + static_cast<float>(s_frameExtra[0]));
                    }
                    break;
                case WMSZ_TOPLEFT: {
                    span = bottom - s_frameExtra[1];
                    if (span - top < s_minClient[1]) {
                        top = span - s_minClient[1];
                    }
                    int32_t across = right - s_frameExtra[0];
                    if (across - left < s_minClient[0]) {
                        left = across - s_minClient[0];
                    }
                    if (s_aspectLocked != 0) {
                        top = bottom - static_cast<int32_t>(static_cast<float>(across - left) / s_aspect + half
                            + static_cast<float>(s_frameExtra[1]));
                        left = right - static_cast<int32_t>(static_cast<float>(span - top) * s_aspect + half
                            + static_cast<float>(s_frameExtra[0]));
                    }
                    break;
                }
                case WMSZ_TOPRIGHT:
                    span = bottom - s_frameExtra[1];
                    if (span - top < s_minClient[1]) {
                        top = span - s_minClient[1];
                    }
                    if ((right - left) - s_frameExtra[0] < s_minClient[0]) {
                        right = left + s_frameExtra[0] + s_minClient[0];
                    }
                    if (s_aspectLocked != 0) {
                        top = bottom - static_cast<int32_t>(static_cast<float>((right - left) - s_frameExtra[0]) / s_aspect
                            + half + static_cast<float>(s_frameExtra[1]));
                        right = static_cast<int32_t>(static_cast<float>(span - top) * s_aspect + half
                            + static_cast<float>(s_frameExtra[0])) + left;
                    }
                    break;
                case WMSZ_BOTTOM:
                    if ((bottom - s_frameExtra[1]) - top < s_minClient[1]) {
                        bottom = top + s_frameExtra[1] + s_minClient[1];
                    }
                    if (s_aspectLocked != 0) {
                        span = (bottom - top) - s_frameExtra[1];
                        right = static_cast<int32_t>(static_cast<float>(span) * s_aspect + half
                            + static_cast<float>(s_frameExtra[0])) + left;
                    }
                    break;
                case WMSZ_BOTTOMLEFT: {
                    if ((bottom - s_frameExtra[1]) - top < s_minClient[1]) {
                        bottom = top + s_frameExtra[1] + s_minClient[1];
                    }
                    int32_t across = right - s_frameExtra[0];
                    if (across - left < s_minClient[0]) {
                        left = across - s_minClient[0];
                    }
                    if (s_aspectLocked != 0) {
                        left = right - static_cast<int32_t>(static_cast<float>((bottom - top) - s_frameExtra[1]) * s_aspect
                            + half + static_cast<float>(s_frameExtra[0]));
                        span = across - left;
                        bottom = static_cast<int32_t>(static_cast<float>(span) / s_aspect + half
                            + static_cast<float>(s_frameExtra[1])) + top;
                    }
                    break;
                }
                case WMSZ_BOTTOMRIGHT:
                    if ((bottom - s_frameExtra[1]) - top < s_minClient[1]) {
                        bottom = top + s_frameExtra[1] + s_minClient[1];
                    }
                    if ((right - left) - s_frameExtra[0] < s_minClient[0]) {
                        right = left + s_frameExtra[0] + s_minClient[0];
                    }
                    if (s_aspectLocked != 0) {
                        bottom = static_cast<int32_t>(static_cast<float>((right - left) - s_frameExtra[0]) / s_aspect
                            + half + static_cast<float>(s_frameExtra[1])) + top;
                        span = (bottom - top) - s_frameExtra[1];
                        right = static_cast<int32_t>(static_cast<float>(span) * s_aspect + half
                            + static_cast<float>(s_frameExtra[0])) + left;
                    }
                    break;
            }
            rect->left = left;
            rect->top = top;
            rect->right = right;
            rect->bottom = bottom;
            return 1;
        }
    }
    return OsWindowProcNested(window, message, wparam, lparam);
}

}

// ref: FUN_1414f8d60
void* WindowGetHandle() {
    return s_window;
}

// The window's DPI scale, carried over to the back buffer: the monitor's DPI over 96, times the
// window's height over the back buffer's (the swap chain's requested height when it asked for
// one, else the parameters' resolution).
// ref: FUN_1414fcdc0
float WindowGetDpiScale() {
    float dpi = OsGetWindowDpiScale(WindowGetHandle());
    uint32_t height = s_params.m_size.m_height;
    uint32_t backBuffer;
    auto chain = static_cast<const GXSWAPCHAIN*>(WindowGetSwapChain());
    if (chain == nullptr) {
        backBuffer = s_params.m_resolution.m_height;
    } else if (chain->m_requestWidth == 0) {
        backBuffer = s_params.m_size.m_height;
    } else {
        backBuffer = chain->m_requestHeight;
    }
    return (static_cast<float>(height) / static_cast<float>(backBuffer)) * dpi;
}

// Once a frame: the foreground frame-rate cap applies while the window has the focus.
// ref: FUN_1414f8e20
void WindowUpdateForeground() {
    GxSetForeground(GetFocus() == s_window);
}

namespace {

float s_uiScaleTarget = 768.0f;     // DAT_142d08284: the height the UI is laid out for
float s_minUiAspect;                // DAT_1430d0198

float WindowAspect() {
    if (s_params.m_size.m_width == 0 || s_params.m_size.m_height == 0) {
        return 1.0f;
    }
    return static_cast<float>(s_params.m_size.m_width) / static_cast<float>(s_params.m_size.m_height);
}

}

// ref: FUN_1414fd040
float WindowClampUiScale(float scale) {
    float aspect = WindowAspect();
    if (s_minUiAspect > aspect) {
        float limit = aspect / s_minUiAspect;
        if (scale > limit) {
            scale = limit;
        }
    }
    if (0.64f > scale) {
        return 0.64f;
    }
    if (scale > 1.15f) {
        return 1.15f;
    }
    return scale;
}

// ref: FUN_1414fcd40
float WindowGetDefaultUiScale() {
    uint16_t width = s_params.m_size.m_width;
    uint16_t height = s_params.m_size.m_height;
    if (height == 0 || width == 0) {
        return WindowClampUiScale(1.0f);
    }
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (aspect < 1.0f) {
        return 1.0f;
    }
    if (static_cast<float>(height) > s_uiScaleTarget) {
        return WindowClampUiScale(s_uiScaleTarget / static_cast<float>(height));
    }
    return 1.0f;
}

// ref: FUN_1414fd580
void WindowSetCursorDisabled(bool disabled) {
    if (static_cast<uint8_t>(disabled) == s_cursorPending) {
        return;
    }
    void* window = WindowGetHandle();
    uint32_t time = OsGetAsyncTimeMs();
    if (disabled) {
        OsCursorDisable(window, time);
        s_cursorPending = 1;
        return;
    }
    OsCursorEnable(window, time);
    s_cursorPending = 0;
}

// The HardwareCursor CVar: a software cursor drops the hardware one; going back restores
// what the cursor was before (DAT_142d08258).
// ref: FUN_1414fca60
void ClientSetHardwareCursor(bool software) {
    if (s_cursorEnable == static_cast<uint8_t>(software)) {
        return;
    }
    s_cursorEnable = software;
    if (software) {
        WindowSetCursorDisabled(false);
        return;
    }
    WindowSetCursorDisabled(s_cursorWasDisabled != 0);
}

// ref: FUN_1414f8d70
void* WindowGetSwapChain() {
    return s_swapChain;
}

// The back buffer's size: the swap chain's, else what the parameters ask for.
// ref: FUN_1414fce40
GXSIZE* WindowGetResolution(GXSIZE* size) {
    *size = { 0, 0 };
    auto chain = static_cast<uint8_t*>(WindowGetSwapChain());
    if (chain == nullptr) {
        *size = s_params.m_resolution;
    } else {
        std::memcpy(size, chain + 0xc, 4);
        if (size->m_width == 0) {
            *size = s_params.m_size;
        }
    }
    return size;
}

// ref: FUN_1414f8d80
GXWINDOWPARAMS* WindowGetParams() {
    return &s_params;
}

// The aspect ratio of the window as last applied, else 1.
// ref: FUN_1414fcc60
float WindowGetAspect() {
    if (s_params.m_size.m_width != 0 && s_params.m_size.m_height != 0) {
        return static_cast<float>(s_params.m_size.m_width) / static_cast<float>(s_params.m_size.m_height);
    }
    return 1.0f;
}

// ref: FUN_1414fce90
GXSIZE* WindowGetSize(GXSIZE* size) {
    *size = s_params.m_size;
    return size;
}

// ref: FUN_1414f8980
void WindowSetClipCursor(bool clip) {
    if (s_clipCursor != static_cast<uint8_t>(clip)) {
        s_clipCursor = clip;
        WindowClipCursor(clip);
    }
}

// Applies the parameters: moves and restyles the window when asked, then settles the back
// buffer size (the client size when it is to match, else no larger than the client) and tells
// the swap chain and the resize callbacks.
// ref: FUN_1414f9330
void WindowApplyParams(const GXWINDOWPARAMS* params, bool reposition) {
    s_params = *params;
    s_updating = 1;
    if (OsIsRemoteSession()) {
        s_params.m_vsync = 0;  // no vsync over Remote Desktop
    }
    if (reposition) {
        uint32_t style = s_params.m_maximize != 0 ? FULLSCREEN_STYLE : WINDOWED_STYLE;
        int32_t position[2] = { 0, 0 };
        WindowComputeRect(style, &s_params, position);
        SetWindowLongPtrA(s_window, GWL_STYLE, style);
        SetWindowPos(s_window, nullptr, position[0], position[1], s_params.m_size.m_width, s_params.m_size.m_height,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    RECT client;
    uint16_t width;
    uint16_t height;
    if (!GetClientRect(s_window, &client)) {
        height = s_params.m_size.m_height;
        width = s_params.m_size.m_width;
    } else {
        width = static_cast<uint16_t>(client.right);
        height = static_cast<uint16_t>(client.bottom);
        if (width == 0) {
            width = 1;
        }
        if (height == 0) {
            height = 1;
        }
        s_params.m_size.m_width = width;
        s_params.m_size.m_height = height;
    }
    if (params->m_sizeMatches == 0) {
        uint16_t limitHeight = s_params.m_resolution.m_height;
        if (width < s_params.m_resolution.m_width) {
            s_params.m_resolution.m_width = width;
        }
        width = s_params.m_resolution.m_width;
        if (height < limitHeight) {
            limitHeight = height;
        }
        height = limitHeight;
    }
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
    s_params.m_resolution.m_width = width;
    s_params.m_resolution.m_height = height;
    if (s_swapChain != nullptr) {
        auto chain = static_cast<uint8_t*>(s_swapChain);
        std::memcpy(chain + 0xc, &s_params.m_resolution, 4);
        chain[0x11] = s_params.m_vsync;
    }
    WindowRunResizeCallbacks();
    if (s_clipCursor != 0 && s_clipSuspended == 0) {
        RECT area;
        GetClientRect(s_window, &area);
        POINT origin = { area.left, area.top };
        ClientToScreen(s_window, &origin);
        RECT screen = { origin.x, origin.y, origin.x + area.right, area.bottom + origin.y };
        ClipCursor(&screen);
    }
    s_updating = 0;
}

// ref: FUN_1414f9300
void WindowUpdate(const GXWINDOWPARAMS* params) {
    GXWINDOWPARAMS copy = *params;
    WindowApplyParams(&copy, true);
}

// Makes the game window ("World of Warcraft", class GxWindowClass) for the parameters, shows
// it, and hooks it to the device's lifetime.
// ref: FUN_1414f8ed0
void WindowCreate(const GXWINDOWPARAMS* params) {
    s_params = *params;
    s_updating = 1;
    GXWINDOWPARAMS request = *params;
    HINSTANCE instance = GetModuleHandleA(nullptr);
    uint32_t style = request.m_maximize != 0 ? FULLSCREEN_STYLE : WINDOWED_STYLE;
    int32_t position[2] = { 0, 0 };
    WindowComputeRect(style, &request, position);
    if (s_class == 0) {
        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_OWNDC;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hIcon = static_cast<HICON>(LoadImageA(instance, "BlizzardIcon.ico", IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        windowClass.lpszClassName = L"GxWindowClass";
        s_class = RegisterClassExW(&windowClass);
    }
    s_window = CreateWindowExW(WS_EX_APPWINDOW, L"GxWindowClass", L"World of Warcraft", style, position[0], position[1],
        request.m_size.m_width, request.m_size.m_height, nullptr, nullptr, instance, nullptr);
    WindowResetCursor();
    if (s_window != nullptr) {
        ShowWindow(s_window, SW_SHOWNORMAL);
    }
    GXWINDOWPARAMS current = s_params;
    WindowApplyParams(&current, false);
    WindowSetVisible(1);
    WindowRunCreateCallbacks(s_window);
    GxAddDeviceCreatedCallback(WindowOnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(WindowOnDeviceDestroyed, nullptr);
    s_powerNotify = RegisterPowerSettingNotification(s_window, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    s_updating = 0;
}

// ref: FUN_1414f8e40
void WindowDestroy() {
    if (s_window != nullptr) {
        UnregisterPowerSettingNotification(s_powerNotify);
        s_powerNotify = nullptr;
        GxRemoveDeviceCreatedCallback(WindowOnDeviceCreated, nullptr);
        GxRemoveDeviceDestroyedCallback(WindowOnDeviceDestroyed, nullptr);
        WindowReleaseDeviceObjects();
        GxSwapChainDestroy(&s_swapChain);
        WindowRunDestroyCallbacks(s_window);
        WindowDestroyCursor();
        DestroyWindow(s_window);
        GxMonitorsReset();
    }
    s_window = nullptr;
}

// Sets the window title (UTF-8).
// ref: FUN_141238150
void WindowSetTitle(void* window, const char* title) {
    uint32_t length = static_cast<uint32_t>(UnicodeCountUtf8ToUtf16(title, 0x7fffffff, nullptr));
    auto wide = static_cast<wchar_t*>(_alloca((static_cast<size_t>(length) * 2 + 0xf) & ~static_cast<size_t>(0xf)));
    UnicodeConvertUtf8ToUtf16(wide, length, title, 0x7fffffff, nullptr, nullptr);
    SetWindowTextW(static_cast<HWND>(window), wide);
}

namespace {

FRAMEBUFFER s_frameBuffer;          // DAT_1430d00a0

}

// ref: FUN_1414fce30
FRAMEBUFFER* WindowGetFrameBuffer() {
    return &s_frameBuffer;
}

extern "C" unsigned long long FUN_1414fc400();
// translated: FUN_1414fc400
void WindowDrawCursor() {
    __try {
        FUN_1414fc400();
    } __except (GenFault(0x1414fc400ull, GetExceptionInformation())) {
    }
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:GenFault=GenFaultMissing")
#pragma comment(linker, "/alternatename:FUN_1414fc320=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414fc3e0=GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_1414fc400=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142d08258 = (void*)&s_cursorWasDisabled;
extern "C" void* const gen_p_142d08259 = (void*)&s_cursorEnable;
extern "C" void* const gen_p_142d08284 = (void*)&s_uiScaleTarget;
extern "C" void* const gen_p_1430b0000 = (void*)&s_dpiOnceLock;
extern "C" void* const gen_p_1430b0020 = (void*)&s_swapChain;
extern "C" void* const gen_p_1430b0030 = (void*)&s_window;
extern "C" void* const gen_p_1430b0048 = (void*)&s_enableNonClientDpiScaling;
extern "C" void* const gen_p_1430b0050 = (void*)&s_cursorHotspot;
extern "C" void* const gen_p_1430b0060 = (void*)&s_cursorPixels;
extern "C" void* const gen_p_1430c0060 = (void*)&s_cursorShown;
extern "C" void* const gen_p_1430d0060 = (void*)&s_cursorSizeIndex;
extern "C" void* const gen_p_1430d009c = (void*)&s_visible;
extern "C" void* const gen_p_1430d00a0 = (void*)&s_frameBuffer;
extern "C" void* const gen_p_1430d0198 = (void*)&s_minUiAspect;
extern "C" void* const gen_p_1430d01a0 = (void*)&s_createCallbacks;
extern "C" void* const gen_p_1430d01d0 = (void*)&s_resizeCallbacks;
extern "C" void* const gen_p_1430d01f8 = (void*)&s_destroyCallbacks;
// ---- end of generated globals ----
