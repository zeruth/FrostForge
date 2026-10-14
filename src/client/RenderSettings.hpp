#ifndef CLIENT_RENDER_SETTINGS_HPP
#define CLIENT_RENDER_SETTINGS_HPP

// The render settings: what the graphics options ask the renderer for. The options write the
// pending copy; once a frame the pending copy is validated against the device and, when it
// differs, becomes the current one and the change handlers run.

#include "gx/Gx.hpp"
#include <cstdint>

struct RENDERSETTINGS {
    float m_renderScale;            // 0x00: the 3D render size against the back buffer
    float m_04;                     // 0x04: 0..1
    GXSIZE m_resolution;            // 0x08: the back buffer
    uint8_t m_msaaSamples;          // 0x0c: log2 of the sample count
    uint8_t m_msaaQuality;          // 0x0d
    uint8_t m_antiAliasing;         // 0x0e: ffxAntiAliasingMode, 0..4
    uint8_t m_sunshafts;            // 0x0f: 0..2
    uint16_t m_shadowTextureSize;   // 0x10: 0x200, 0x400 or 0x800
    uint8_t m_deviceResets;         // 0x12: counts device losses so a lost device re-applies
    uint8_t m_spellClutter;         // 0x13: 0..100
    uint8_t m_upscaler;             // 0x14: 0..3 (3 needs dx_5_0 or newer)
    uint8_t m_pad15[3];
    float m_sharpness;              // 0x18
    uint8_t m_modelScreenRects;     // 0x1c: the model scenes track their models' screen rectangle
    uint8_t m_1d;
    uint8_t m_shadowMode;           // 0x1e: 0..4 (0 blob shadows); ray traced shadows need one
    uint8_t m_ssao;                 // 0x1f: 0..4
    uint8_t m_ssaoType;             // 0x20: 0..2
    uint8_t m_waterDetail;          // 0x21: 0..3
    uint8_t m_rippleDetail;         // 0x22: 0..2
    uint8_t m_reflectionMode;       // 0x23: 0..3
    uint8_t m_reflectionDownscale;  // 0x24: 0..4
    uint8_t m_25;                   // 0x25: a shading rate (vrsWorldGeo), checked against the MSAA mode
    uint8_t m_26;                   // 0x26: another (vrsParticles)
    uint8_t m_27[6];
    uint8_t m_filterMode;           // 0x2d: below 6; 5 allows up to 16 of m_filterLevel
    uint8_t m_filterLevel;          // 0x2e
    uint8_t m_outlines;             // 0x2f: how many scenes draw outlines
    uint8_t m_lowLatencyMode;       // 0x30: GxLowLatencyMode
    uint8_t m_pad31[3];
    uint32_t m_flags;               // 0x34: 0x18 ray traced shadows, 0x20 needs the device's +0x12d,
                                    //       0x200 frame buffer heaps, 0x400 sharpen
};
static_assert(sizeof(RENDERSETTINGS) == 0x38, "RENDERSETTINGS is 0x38 bytes");

// A render target's colour and depth formats by kind (FUN_1412af6f0).
struct RENDERTARGETFORMATS {
    uint8_t m_color;                // 0x00
    uint8_t m_01[3];
    uint8_t m_depth;                // 0x04
};
RENDERTARGETFORMATS RenderTargetFormats(uint32_t kind);

// The settings in force (DAT_142cfbfa8) and the ones asked for (DAT_142cfbfe0).
extern RENDERSETTINGS g_renderSettings;
extern RENDERSETTINGS g_pendingRenderSettings;

typedef bool (*RENDERSETTINGSVALIDATOR)(const RENDERSETTINGS* current, RENDERSETTINGS* pending, void* param);
typedef void (*RENDERSETTINGSHANDLER)(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param);

// Change handlers (FUN_14129d3e0, FUN_14129de10) and validators (FUN_14129d420, FUN_14129dfd0).
void RenderSettingsAddHandler(RENDERSETTINGSHANDLER handler, void* param);
void RenderSettingsRemoveHandler(RENDERSETTINGSHANDLER handler, void* param);
void RenderSettingsAddValidator(RENDERSETTINGSVALIDATOR validator, void* param);
void RenderSettingsRemoveValidator(RENDERSETTINGSVALIDATOR validator, void* param);

// Registers the settings' own validator, handler and Gx callbacks (FUN_14129d9f0) and takes
// them down again (FUN_14129d780).
void RenderSettingsInitialize();
void RenderSettingsShutdown();

// A scene starting (FUN_1412b2110) and stopping (FUN_1412b2250) its outline passes.
void RenderSettingsAddOutline();
void RenderSettingsRemoveOutline();

// Makes the pending settings current when they differ (FUN_14129da80).
void RenderSettingsApply();

// The ray traced shadow kind from the current flags (FUN_14129d9a0): 0 none, bit 1 lights cast.
uint32_t RenderSettingsRayTracedShadows();

// Whether a settings block's 0x60000000 bits are 0x20000000 (FUN_14129d9d0).
bool RenderSettingsDepthFillMode(const RENDERSETTINGS* settings);

// The largest render scale the GPU allows: 2 with 3.5 GB of video memory, else 1 (FUN_14129d7e0).
float RenderSettingsMaxScale();

// A size, and the back buffer, scaled by the render scale (FUN_14129d830, FUN_14129d890).
int32_t* RenderSettingsScaledSize(int32_t* out, const int32_t* size);
int32_t* RenderSettingsRenderSize(int32_t* out);

// The pending settings' setters, each false when the device cannot take the value
// (FUN_14129e190, FUN_14129e1e0, FUN_14129e250, FUN_14129e290).
bool RenderSettingsSetMsaa(uint8_t samples, uint8_t quality);
bool RenderSettingsSetRenderScale(float scale);
bool RenderSettingsSetUpscaler(uint8_t upscaler);
bool RenderSettingsSet04(float value);

// Why a setting cannot be had, 0 when it can (FUN_14129e2b0, FUN_14129e3e0, FUN_14129e470).
uint32_t RenderSettingsCheckLowLatencyMode(uint8_t mode);
uint32_t RenderSettingsCheckMsaa(uint8_t samples, uint8_t quality);
uint32_t RenderSettingsCheckUpscaler(uint8_t upscaler);

#endif
