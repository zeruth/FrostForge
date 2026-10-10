#ifndef CLIENT_GX_WINDOW_HPP
#define CLIENT_GX_WINDOW_HPP

// The game window (class "GxWindowClass"): made from the console's GXWINDOWPARAMS, sized and
// restyled as they change, given a swap chain when a device comes up (client/GxWindow.cpp).

#include "gx/Gx.hpp"

void WindowCreate(const GXWINDOWPARAMS* params);
void WindowDestroy();
void WindowUpdate(const GXWINDOWPARAMS* params);
void WindowApplyParams(const GXWINDOWPARAMS* params, bool reposition);
void* WindowGetHandle();

// The UI scale the window suggests (FUN_1414fcd40), and a scale held to 0.64 .. 1.15 and to
// the minimum aspect (FUN_1414fd040).
float WindowGetDefaultUiScale();
float WindowClampUiScale(float scale);
GXWINDOWPARAMS* WindowGetParams();
float WindowGetAspect();
GXSIZE* WindowGetSize(GXSIZE* size);
GXSIZE* WindowGetResolution(GXSIZE* size);
void* WindowGetSwapChain();

// The window's frame buffer (DAT_1430d00a0, FUN_1414fce30).
struct FRAMEBUFFER;
FRAMEBUFFER* WindowGetFrameBuffer();

// Draws the software cursor (FUN_1414fc400).
void WindowDrawCursor();
void WindowSetClipCursor(bool clip);
void WindowSetTitle(void* window, const char* title);

#endif
