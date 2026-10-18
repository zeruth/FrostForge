#include "client/RenderSettings.hpp"
#include "blz/Vector.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPlatform.hpp"
#include "os/Os.hpp"
#include "prism/Prism.hpp"
#include <windows.h>
#include <cstring>

// The reference's static values (DAT_142cfbfa8 and DAT_142cfbfe0, the same 0x38 bytes): scale 1.0
// and 1.0, a 0x400 shadow map, filter mode 3 at level 1, one outline scene, the rest zero. One
// value per field of the layout as it stands: the list was written against an older one (bytes
// 0x0e..0x11 unnamed), so after 26ca1c0 named them it ran a field off, leaving the shadow map size
// 0, the device-reset count 4 and filtering off, and putting 3/1/1 in the water and reflection
// levels (C4838 on the float landing in a pad byte, 2026-10-17)
#define FROST_RENDER_SETTINGS_DEFAULTS                                                            \
    { 1.0f, 1.0f, { 0, 0 },                                                                       \
      0, 0, 0, 0,          /* msaa samples, quality, anti-aliasing, sunshafts */                  \
      0x400,               /* shadow texture size */                                              \
      0, 0, 0, {},         /* device resets, spell clutter, upscaler, pad */                     \
      0.0f,                /* sharpness */                                                        \
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x1c..0x26 */                                           \
      {},                  /* 0x27 */                                                             \
      3, 1, 1, 0, {},      /* filter mode, filter level, outlines, low latency, pad */           \
      0 }
RENDERSETTINGS g_renderSettings = FROST_RENDER_SETTINGS_DEFAULTS;
RENDERSETTINGS g_pendingRenderSettings = FROST_RENDER_SETTINGS_DEFAULTS;
#undef FROST_RENDER_SETTINGS_DEFAULTS

namespace {

const char s_validatorTag[] = ".?AU?$pair@P6A_NAEBURenderSettings@@AEAU1@PEAX@ZPEAX@blz@@";
const char s_validatorMapTag[] = ".PEAU?$pair@P6A_NAEBURenderSettings@@AEAU1@PEAX@ZPEAX@blz@@";
const char s_handlerTag[] = ".?AU?$pair@P6AXAEBURenderSettings@@0PEAX@ZPEAX@blz@@";
const char s_handlerMapTag[] = ".PEAU?$pair@P6AXAEBURenderSettings@@0PEAX@ZPEAX@blz@@";

template <class F>
struct HANDLERPAIR {
    F m_function;
    void* m_param;
};

blz::deque<HANDLERPAIR<RENDERSETTINGSHANDLER>, 8, s_handlerMapTag, s_handlerTag> s_handlers;        // DAT_1430a5860
blz::deque<HANDLERPAIR<RENDERSETTINGSVALIDATOR>, 8, s_validatorMapTag, s_validatorTag> s_validators;  // DAT_1430a5880
uint32_t s_rtShadows;               // DAT_1430a58a0

// The device's shader family (its capability block +8): dx_4_0 is 0.
int32_t ShaderFamily() {
    return *reinterpret_cast<int32_t*>(GxDeviceCaps() + 8);
}

template <class Deque, class F>
void RemovePair(Deque* deque, F function, void* param) {
    for (uint64_t i = 0; i < deque->size(); i++) {
        auto& pair = deque->at(i);
        if (pair.m_function == function && pair.m_param == param) {
            deque->erase(i);
            return;
        }
    }
}

// The settings' own validator: steps MSAA down until both target formats take it, clamps the
// render scale, and drops what the device cannot do.
// ref: FUN_14129d480
bool ValidateSettings(const RENDERSETTINGS* current, RENDERSETTINGS* pending, void* param) {
    (void)param;
    bool msaaOk = true;
    if (RenderSettingsCheckMsaa(pending->m_msaaSamples, pending->m_msaaQuality) != 0) {
        if (RenderSettingsCheckMsaa(current->m_msaaSamples, current->m_msaaQuality) == 0) {
            pending->m_msaaSamples = current->m_msaaSamples;
            pending->m_msaaQuality = current->m_msaaQuality;
        } else {
            pending->m_msaaQuality = 0;
            RENDERTARGETFORMATS formats = RenderTargetFormats(6);
            const uint8_t list[2] = { formats.m_color, formats.m_depth };
            while (pending->m_msaaSamples != 0) {
                bool supported = true;
                for (uint8_t format : list) {
                    if (!GxFormatSupportsMultisample(format, pending->m_msaaSamples, pending->m_msaaQuality)) {
                        supported = false;
                        break;
                    }
                }
                if (supported) {
                    break;
                }
                pending->m_msaaSamples--;
            }
        }
        msaaOk = false;
    }
    uint8_t* caps = GxDeviceCaps();
    bool flagOk = true;
    if ((pending->m_flags & 0x20) != 0 && caps[0xad] == 0) {
        pending->m_flags &= ~0x20u;
        flagOk = false;
    }
    float maxScale = RenderSettingsMaxScale();
    float scale = pending->m_renderScale;
    if (maxScale < scale) {
        pending->m_renderScale = maxScale;
    }
    bool rate25Ok = true;
    if (!GxShadingRateSupported(pending->m_msaaSamples, pending->m_msaaQuality, pending->m_25) ||
        (pending->m_25 != 0 && (pending->m_flags & 0x18) != 0)) {
        pending->m_25 = 0;
        rate25Ok = false;
    }
    bool rate26Ok = true;
    if (!GxShadingRateSupported(pending->m_msaaSamples, pending->m_msaaQuality, pending->m_26) ||
        (pending->m_26 != 0 && (pending->m_flags & 0x18) != 0)) {
        pending->m_26 = 0;
        rate26Ok = false;
    }
    bool rtOk = true;
    if ((pending->m_flags & 0x18) != 0 && (caps[0xaf] < 2 || pending->m_shadowMode == 0)) {
        pending->m_flags &= ~0x18u;
        rtOk = false;
    }
    bool filterOk = true;
    if (pending->m_filterMode < 6) {
        if (pending->m_filterMode == 5) {
            if (pending->m_filterLevel > 0x10) {
                pending->m_filterLevel = 0x10;
                filterOk = false;
            }
        } else if (pending->m_filterLevel > 1) {
            pending->m_filterLevel = 1;
            filterOk = false;
        }
    } else {
        pending->m_filterMode = current->m_filterMode;
        pending->m_filterLevel = current->m_filterLevel;
        filterOk = false;
    }
    bool upscalerOk = true;
    if (pending->m_upscaler >= 4 || (pending->m_upscaler == 3 && ShaderFamily() == 0)) {
        if (current->m_upscaler < 4 && (current->m_upscaler != 3 || ShaderFamily() != 0)) {
            pending->m_upscaler = current->m_upscaler;
        } else {
            pending->m_upscaler = 2;
        }
        upscalerOk = false;
    }
    if (RenderSettingsCheckLowLatencyMode(pending->m_lowLatencyMode) != 0) {
        if (RenderSettingsCheckLowLatencyMode(current->m_lowLatencyMode) != 0) {
            pending->m_lowLatencyMode = 0;
        } else {
            pending->m_lowLatencyMode = current->m_lowLatencyMode;
        }
        return false;
    }
    return flagOk && msaaOk && maxScale >= scale && rate26Ok && rate25Ok && rtOk && filterOk && upscalerOk;
}

// The device's low-latency mode follows the settings.
// ref: 0x14129d460
void OnSettingsChanged(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    (void)param;
    if (previous->m_lowLatencyMode != current->m_lowLatencyMode) {
        GxSetLowLatencyMode(current->m_lowLatencyMode);
    }
}

// ref: 0x14129dd60
void OnDeviceCreated(void* param) {
    (void)param;
    GxSetLowLatencyMode(g_renderSettings.m_lowLatencyMode);
}

// A lost device makes the pending settings differ, so they are applied again.
// ref: 0x14129dd70
void OnDeviceDestroyed(void* param) {
    (void)param;
    g_pendingRenderSettings.m_deviceResets++;
}

// ref: 0x14129de00 (jumps to FUN_14129da80)
void OnFrameStart(void* param) {
    (void)param;
    RenderSettingsApply();
}

// Runs the validators over the pending settings once the device is up.
// ref: FUN_14129dd80
void Revalidate(void* param) {
    (void)param;
    for (uint64_t i = 0; i < s_validators.size(); i++) {
        auto& pair = s_validators.at(i);
        pair.m_function(&g_renderSettings, &g_pendingRenderSettings, pair.m_param);
    }
}

}

// ref: FUN_1412af6f0
RENDERTARGETFORMATS RenderTargetFormats(uint32_t kind) {
    static const RENDERTARGETFORMATS s_formats[25] = {  // DAT_1430a5c60, filled on first use
        { 0x17, { 0x02, 0x00, 0x00 }, 0x1e },
        { 0x17, { 0x02, 0x00, 0x00 }, 0x00 },
        { 0x14, { 0x00, 0x00, 0x00 }, 0x1e },
        { 0x14, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x13, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x02, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x02, { 0x00, 0x00, 0x00 }, 0x1e },
        { 0x02, { 0x17, 0x00, 0x00 }, 0x1e },
        { 0x0c, { 0x00, 0x00, 0x00 }, 0x1e },
        { 0x0c, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x11, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x11, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x02, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x19, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x00, { 0x00, 0x00, 0x00 }, 0x1c },
        { 0x12, { 0x00, 0x00, 0x00 }, 0x1e },
        { 0x17, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x0f, { 0x00, 0x00, 0x00 }, 0x1b },
        { 0x00, { 0x00, 0x00, 0x00 }, 0x1b },
        { 0x02, { 0x00, 0x00, 0x00 }, 0x1e },
        { 0x02, { 0x00, 0x00, 0x00 }, 0x1e },
        { 0x11, { 0x13, 0x00, 0x00 }, 0x00 },
        { 0x0c, { 0x00, 0x00, 0x00 }, 0x1e },
        { 0x0c, { 0x00, 0x00, 0x00 }, 0x00 },
        { 0x0c, { 0x00, 0x00, 0x00 }, 0x00 },
    };
    return s_formats[kind];
}

// ref: FUN_14129d3e0
void RenderSettingsAddHandler(RENDERSETTINGSHANDLER handler, void* param) {
    auto slot = s_handlers.push_back_slot();
    slot->m_function = handler;
    slot->m_param = param;
}

// ref: FUN_14129de10
void RenderSettingsRemoveHandler(RENDERSETTINGSHANDLER handler, void* param) {
    RemovePair(&s_handlers, handler, param);
}

// ref: FUN_14129d420
void RenderSettingsAddValidator(RENDERSETTINGSVALIDATOR validator, void* param) {
    auto slot = s_validators.push_back_slot();
    slot->m_function = validator;
    slot->m_param = param;
}

// ref: FUN_14129dfd0
void RenderSettingsRemoveValidator(RENDERSETTINGSVALIDATOR validator, void* param) {
    RemovePair(&s_validators, validator, param);
}

// ref: FUN_14129d9f0
void RenderSettingsInitialize() {
    RenderSettingsAddValidator(ValidateSettings, nullptr);
    RenderSettingsAddHandler(OnSettingsChanged, nullptr);
    GxAddFrameStartCallback(OnFrameStart, nullptr);
    GxAddDeviceCreatedCallback(OnDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(OnDeviceDestroyed, nullptr);
    GxAddEngineDeviceCreatedCallback(Revalidate, nullptr);
}

// ref: FUN_14129d780
void RenderSettingsShutdown() {
    GxRemoveEngineDeviceCreatedCallback(Revalidate, nullptr);
    GxRemoveFrameStartCallback(OnFrameStart, nullptr);
    GxRemoveDeviceDestroyedCallback(OnDeviceDestroyed, nullptr);
    GxRemoveDeviceCreatedCallback(OnDeviceCreated, nullptr);
    RenderSettingsRemoveHandler(OnSettingsChanged, nullptr);
    RenderSettingsRemoveValidator(ValidateSettings, nullptr);
}

// ref: FUN_1412b2110
void RenderSettingsAddOutline() {
    g_pendingRenderSettings.m_outlines++;
}

// ref: FUN_1412b2250
void RenderSettingsRemoveOutline() {
    g_pendingRenderSettings.m_outlines--;
}

// ref: FUN_14129da80
void RenderSettingsApply() {
    for (int32_t pass = 2; pass > 0; pass--) {
        if (std::memcmp(&g_renderSettings, &g_pendingRenderSettings, sizeof(RENDERSETTINGS)) == 0) {
            return;
        }
        bool valid = true;
        if (s_validators.size() != 0) {
            for (uint64_t i = 0; i < s_validators.size(); i++) {
                auto& pair = s_validators.at(i);
                valid &= pair.m_function(&g_renderSettings, &g_pendingRenderSettings, pair.m_param);
            }
            if (!valid && std::memcmp(&g_renderSettings, &g_pendingRenderSettings, sizeof(RENDERSETTINGS)) == 0) {
                return;
            }
        }
        RENDERSETTINGS previous = g_renderSettings;
        g_renderSettings = g_pendingRenderSettings;
        float scale = g_renderSettings.m_renderScale;
        uint16_t width = g_renderSettings.m_resolution.m_width;
        uint16_t height = g_renderSettings.m_resolution.m_height;
        if (previous.m_resolution.m_width == width && previous.m_resolution.m_height == height && previous.m_renderScale == scale) {
            GxLog("Render Settings Changed");
        } else if (scale == 1.0f) {
            GxLog("Render Settings Changed. New Render Size: %hux%hu", width, height);
        } else {
            uint32_t scaledWidth = static_cast<int32_t>(static_cast<float>(width) * scale) != 0
                ? static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(width) * scale))
                : 1;
            uint32_t scaledHeight = static_cast<int32_t>(static_cast<float>(height) * scale) != 0
                ? static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(height) * scale))
                : 1;
            GxLog("Render Settings Changed. New Render Size: %ux%u (%hux%hu @ %.1f%%)", scaledWidth, scaledHeight, width, height,
                static_cast<double>(scale * 100.0f));
        }
        uint32_t rtShadows = (g_renderSettings.m_flags >> 3) & 3;
        if (s_rtShadows != rtShadows) {
            s_rtShadows = rtShadows;
            GxLog("RTShadows: %u", rtShadows);
        }
        for (uint64_t i = 0; i < s_handlers.size(); i++) {
            auto& pair = s_handlers.at(i);
            pair.m_function(&previous, &g_renderSettings, pair.m_param);
        }
    }
}

// The ray traced shadow kind from the flags: 0 none, else bit 0 set and bit 2 below the top
// setting.
// ref: FUN_14129d9a0
uint32_t RenderSettingsRayTracedShadows() {
    uint32_t mode = g_renderSettings.m_flags >> 3 & 3;
    if (mode == 0) {
        return 0;
    }
    uint32_t kind = mode < 2 ? 1u : 3u;
    if (mode < 3) {
        return kind | 4;
    }
    return kind;
}

// Whether a settings block's 0x60000000 bits select the first of their modes (0x20000000): the
// world clears to its own colour and the barrier passes add a depth fill.
// ref: FUN_14129d9d0
bool RenderSettingsDepthFillMode(const RENDERSETTINGS* settings) {
    return (settings->m_flags & 0x60000000) == 0x20000000;
}

// ref: FUN_14129d7e0
float RenderSettingsMaxScale() {
    uint32_t dedicated;
    uint32_t shared;
    GxDeviceVideoMemory(&dedicated, &shared);
    if (dedicated > 0xdff && ShaderFamily() != 0) {
        return 2.0f;
    }
    return 1.0f;
}

// ref: FUN_14129d830
int32_t* RenderSettingsScaledSize(int32_t* out, const int32_t* size) {
    int32_t width = static_cast<int32_t>(static_cast<int64_t>(static_cast<float>(size[0]) * g_renderSettings.m_renderScale));
    int32_t height = static_cast<int32_t>(static_cast<int64_t>(static_cast<float>(size[1]) * g_renderSettings.m_renderScale));
    out[0] = width != 0 ? width : 1;
    out[1] = height != 0 ? height : 1;
    return out;
}

// ref: FUN_14129d890
int32_t* RenderSettingsRenderSize(int32_t* out) {
    int32_t size[2] = { g_renderSettings.m_resolution.m_width, g_renderSettings.m_resolution.m_height };
    return RenderSettingsScaledSize(out, size);
}

// ref: FUN_14129e190
bool RenderSettingsSetMsaa(uint8_t samples, uint8_t quality) {
    if (RenderSettingsCheckMsaa(samples, quality) != 0) {
        return false;
    }
    g_pendingRenderSettings.m_msaaQuality = quality;
    g_pendingRenderSettings.m_msaaSamples = samples;
    return true;
}

// ref: FUN_14129e1e0
bool RenderSettingsSetRenderScale(float scale) {
    if (scale < 0.008333334f || RenderSettingsMaxScale() < scale) {
        return false;
    }
    g_pendingRenderSettings.m_renderScale = scale;
    return true;
}

// ref: FUN_14129e250
bool RenderSettingsSetUpscaler(uint8_t upscaler) {
    if (upscaler > 3 || (upscaler == 3 && ShaderFamily() == 0)) {
        return false;
    }
    g_pendingRenderSettings.m_upscaler = upscaler;
    return true;
}

// ref: FUN_14129e290
bool RenderSettingsSet04(float value) {
    if (value < 0.0f || value > 1.0f) {
        return false;
    }
    g_pendingRenderSettings.m_04 = value;
    return true;
}

// The reasons a low-latency mode is refused: 0x20 a remote session, 0x1d a legacy API, 2 an
// unknown mode or platform, 0x15 and 0x28 an old macOS, 0x1b Reflex on macOS, 0x11 Reflex
// without NVIDIA, 0x22 under Wine.
// ref: FUN_14129e2b0
uint32_t RenderSettingsCheckLowLatencyMode(uint8_t mode) {
    const OSVERSION* version = OsGetVersion();
    if (mode == 0) {
        return 0;
    }
    if (GetSystemMetrics(SM_REMOTESESSION) != 0) {  // FUN_140267270
        return 0x20;
    }
    if (GxApiIsLegacy()) {
        return 0x1d;
    }
    if (mode == 1) {
        if (version->m_platform != 10) {
            if (version->m_platform != 6) {
                return 2;
            }
            if (version->m_major < 0xb) {
                if (version->m_major != 10 || version->m_minor != 0xf) {
                    return 0x15;
                }
                if (version->m_servicePack < 4) {
                    return 0x28;
                }
            }
        }
        return 0;
    }
    if (static_cast<uint8_t>(mode - 2) > 1) {
        return 2;
    }
    if (version->m_platform != 10) {
        return version->m_platform == 6 ? 0x1b : 2;
    }
    GxAdapterInfo* adapter = GxSelectGpuToUse();
    if ((reinterpret_cast<const uint8_t*>(adapter->m_data)[0x128] & 1) == 0) {
        if (adapter->m_gpu->m_vendorId != 0x10de) {
            return 0x11;
        }
        return version->m_wine != 0 ? 0x22 : 0x28;
    }
    return 0;
}

// ref: FUN_14129e3e0
uint32_t RenderSettingsCheckMsaa(uint8_t samples, uint8_t quality) {
    if (samples == 0 && quality == 0) {
        return 0;
    }
    RENDERTARGETFORMATS formats = RenderTargetFormats(6);
    const uint8_t list[2] = { formats.m_color, formats.m_depth };
    for (uint8_t format : list) {
        if (!GxFormatSupportsMultisample(format, samples, quality)) {
            return 3;
        }
    }
    return 0;
}

// ref: FUN_14129e470
uint32_t RenderSettingsCheckUpscaler(uint8_t upscaler) {
    if (upscaler > 3) {
        return 1;
    }
    if (upscaler == 3) {
        return ShaderFamily() == 0 ? 9 : 0;
    }
    return 0;
}

// ---- GENERATED by tools/forward_stubs.py: link fallbacks where src/gen is not linked ----
#pragma comment(linker, "/include:GenForwardMissing")
#pragma comment(linker, "/alternatename:FUN_140267270=GenForwardMissing")
// ---- end of link fallbacks ----

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_1430a5860 = (void*)&s_handlers;
extern "C" void* const gen_p_1430a5880 = (void*)&s_validators;
// ---- end of generated globals ----
