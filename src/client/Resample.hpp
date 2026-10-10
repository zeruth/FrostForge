#ifndef CLIENT_RESAMPLE_HPP
#define CLIENT_RESAMPLE_HPP

// Copying a rendered scene into its destination at another size: a straight copy, a bilinear
// resample, or AMD FSR (EASU to upscale, RCAS to sharpen). The pipelines are made with the
// device.

#include <cstdint>

class GxRenderContext;
namespace gxDx12Backend {
struct Tex;
}

// How a scene is resampled (FUN_1412a29c0's options): the mode (0 copy, 2 bilinear, 3 FSR),
// the sharpness, and whether to sharpen.
struct RESAMPLEOPTIONS {
    uint8_t m_mode;                 // 0x00
    uint8_t m_pad01[3];
    float m_sharpness;              // 0x04
    uint8_t m_sharpen;              // 0x08
};

// The pipelines, made and released with the device (FUN_1412a2650, FUN_1412a2960), and
// dropped at shutdown (FUN_1412a28d0).
void ResampleDeviceCreated(void* param);
void ResampleDeviceDestroyed(void* param);
void ResampleShutdown();

// ref: FUN_14129d910
RESAMPLEOPTIONS* RenderSettingsResampleOptions(RESAMPLEOPTIONS* out);

// Draws `source` into `dest` through the environment's rectangle, over what is there when
// `blend` (FUN_1412a29c0).
void ResampleToTarget(GxRenderContext* context, const void* environment, gxDx12Backend::Tex* source,
    gxDx12Backend::Tex* dest, const RESAMPLEOPTIONS* options, bool blend);

#endif
