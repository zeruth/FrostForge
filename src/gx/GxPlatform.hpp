#ifndef GX_GX_PLATFORM_HPP
#define GX_GX_PLATFORM_HPP

// What GxApi took from the platform for the device backends: the DXGI factories and the
// D3D12 entry points (GxApi.cpp), and the device-wide locks.

#include "storm/thread/SCritSect.hpp"
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_6.h>

typedef HRESULT(WINAPI* PFN_GXD3D12CREATEDEVICE)(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID iid, void** device);

IDXGIFactory1* GxPlatformFactory1();                // FUN_142295780
IDXGIFactory5* GxPlatformFactory5();                // FUN_142295790
IDXGIFactory6* GxPlatformFactory6();                // FUN_1422957a0
bool GxPlatformHasD3d12();                          // FUN_142295920
bool GxPlatformHasNvapi();                          // FUN_1422958a0
PFN_GXD3D12CREATEDEVICE GxPlatformD3d12CreateDevice();
PFN_D3D12_SERIALIZE_ROOT_SIGNATURE GxPlatformSerializeRootSignature();
PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE GxPlatformSerializeVersionedRootSignature();

// DAT_143490880: held across device object creation and release.
extern SCritSect g_gxDeviceLock;

// The lock FUN_14228f7a0 takes for the length of a device call (a function-local static).
struct GxDeviceCallLock {
    GxDeviceCallLock();
    ~GxDeviceCallLock();
};

// Counts device removals reported (FUN_14228aab0).
void GxCountDeviceRemoved();

// Runs the device-created callbacks: the engine's own, then the registered ones
// (FUN_14228e400); and the destroyed ones (FUN_14228e4d0).
void GxNotifyOnDeviceCreate();
void GxNotifyOnDeviceDestroy();

// Runs the low-memory callbacks (FUN_14228e650).
void GxNotifyOnLowMemory();

// Registers an engine callback run before the others on device creation, and at once when
// a device is up (FUN_14228e0c0); and a low-memory callback (FUN_14228e220).
void GxAddEngineDeviceCreatedCallback(void (*callback)(void*), void* param);
void GxAddLowMemoryCallback(void (*callback)(void*), void* param);

#endif
