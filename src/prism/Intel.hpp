#ifndef PRISM_INTEL_HPP
#define PRISM_INTEL_HPP

// Intel's part of the device data, shared by the D3D11 and D3D12 device creators: the GPU
// generation from the device id, and the driver's "Intel Device Information" counter.

#include "prism/Prism.hpp"
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace pm {

// The generation (pmIntelFamily) a device id belongs to, from its high byte (FUN_142358720).
// `ids` points at the vendor id, as the device data stores it.
uint8_t IntelGeneration(const uint16_t* ids);

// Reads the driver's device information counter into `info` (the device data's Intel block at
// +0x78): the frequency range from version 1, and three more fields from version 2. False when
// the driver has no such counter (FUN_1423583b0).
bool IntelQueryDeviceInfo(Allocator* allocator, uint8_t* info, ID3D11Device* device, ID3D11DeviceContext* context);

}

#endif
