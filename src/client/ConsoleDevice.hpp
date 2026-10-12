#ifndef CLIENT_CONSOLE_DEVICE_HPP
#define CLIENT_CONSOLE_DEVICE_HPP

// The console's graphics device (WoW/Source/Console/Client): its CVars and commands, the
// window and device parameters they make, and bringing the window and device up.

#include "gx/Gx.hpp"
#include <cstdint>

struct CVar;

// The detected graphics defaults (0xc bytes): CPU, GPU and memory tiers, the window size, and
// the world render scale.
struct CONSOLEDETECTED {
    int8_t m_cpu;                   // 0x00
    int8_t m_gpu;                   // 0x01
    int8_t m_memory;                // 0x02
    uint8_t m_pad03;
    GXSIZE m_windowSize;            // 0x04
    float m_renderScale;            // 0x08
};

// Opens gx.log, registers the CVars and commands, starts Gx, makes the window (titled
// `title`) and the device. False when the machine cannot run the game.
bool ConsoleDeviceInitialize(const char* title);

// The detected defaults (FUN_140328750).
CONSOLEDETECTED* ConsoleGetDetected();

// The parameters the CVars ask for.
void ConsoleDeviceGetParams(GXDEVICEPARAMS* device, GXWINDOWPARAMS* window);

// Brings a device up, falling back from D3D12 to D3D11 to D3D11 legacy to windowed.
bool ConsoleDeviceCreate(GXDEVICEPARAMS* device, GXWINDOWPARAMS* window);

// What to run once the hardware detection defaults are written (DAT_142e303c0).
void ConsoleDeviceSetHardwareDetectHook(void (*hook)());

// The CVar callback that only says a GxRestart is pending (FUN_140328120).
bool ConsoleDeviceRequestRestart(CVar* cvar, const char* oldValue, const char* newValue, void* userData);

// The fullscreen sizes a monitor offers that suit the client (FUN_14032abf0).
void GetFullscreenSizes(blz::vector<GXSIZE, s_gxSizeTag>* sizes, uint32_t monitor, bool matchAspect);

// The default fullscreen size for the adapter on a monitor (FUN_1403281e0).
GXSIZE DefaultFullscreenSize(GxAdapterInfo* adapter, uint32_t monitor);

#endif
