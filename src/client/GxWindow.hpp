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
GXWINDOWPARAMS* WindowGetParams();
GXSIZE* WindowGetSize(GXSIZE* size);
GXSIZE* WindowGetResolution(GXSIZE* size);
void* WindowGetSwapChain();
void WindowSetClipCursor(bool clip);
void WindowSetTitle(void* window, const char* title);

#endif
